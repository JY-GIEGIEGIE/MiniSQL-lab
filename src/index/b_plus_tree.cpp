#include "index/b_plus_tree.h"
#include <string>
#include "glog/logging.h"
#include "index/basic_comparator.h"
#include "index/generic_key.h"
#include "page/index_roots_page.h"

BPlusTree::BPlusTree(index_id_t index_id, BufferPoolManager *buffer_pool_manager, const KeyManager &KM,
                     int leaf_max_size, int internal_max_size)
    : index_id_(index_id), buffer_pool_manager_(buffer_pool_manager), processor_(KM),
      leaf_max_size_(leaf_max_size), internal_max_size_(internal_max_size) {
  if (leaf_max_size == UNDEFINED_SIZE)
    leaf_max_size_ = (PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / (processor_.GetKeySize() + sizeof(RowId)) - 1;
  if (internal_max_size == UNDEFINED_SIZE)
    internal_max_size_ = (PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (processor_.GetKeySize() + sizeof(page_id_t)) - 1;
}

void BPlusTree::Destroy(page_id_t current_page_id) {
  if (current_page_id == INVALID_PAGE_ID) {
    if (root_page_id_ == INVALID_PAGE_ID) return;
    current_page_id = root_page_id_;
    root_page_id_ = INVALID_PAGE_ID;
  }
  auto *page = buffer_pool_manager_->FetchPage(current_page_id);
  auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());
  if (node->IsLeafPage()) {
    buffer_pool_manager_->UnpinPage(current_page_id, false);
    buffer_pool_manager_->DeletePage(current_page_id);
  } else {
    auto *in = reinterpret_cast<InternalPage *>(node);
    for (int i = 0; i < in->GetSize(); i++) Destroy(in->ValueAt(i));
    buffer_pool_manager_->UnpinPage(current_page_id, false);
    buffer_pool_manager_->DeletePage(current_page_id);
  }
}

bool BPlusTree::IsEmpty() const {
  if (root_page_id_ == INVALID_PAGE_ID) return true;
  auto *page = buffer_pool_manager_->FetchPage(root_page_id_);
  auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());
  bool empty = (node->GetSize() == 0);
  buffer_pool_manager_->UnpinPage(root_page_id_, false);
  return empty;
}

bool BPlusTree::GetValue(const GenericKey *key, std::vector<RowId> &result, Txn *transaction) {
  if (root_page_id_ == INVALID_PAGE_ID) return false;
  auto *page = FindLeafPage(key);
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  RowId value;
  bool found = leaf->Lookup(key, value, processor_);
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
  if (found) result.push_back(value);
  return found;
}

bool BPlusTree::Insert(GenericKey *key, const RowId &value, Txn *transaction) {
  if (root_page_id_ == INVALID_PAGE_ID) { StartNewTree(key, value); return true; }
  return InsertIntoLeaf(key, value, transaction);
}

void BPlusTree::StartNewTree(GenericKey *key, const RowId &value) {
  page_id_t pid;
  auto *page = buffer_pool_manager_->NewPage(pid);
  if (!page) throw std::runtime_error("Out of memory");
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  leaf->Init(pid, INVALID_PAGE_ID, processor_.GetKeySize(), leaf_max_size_);
  leaf->Insert(key, value, processor_);
  root_page_id_ = pid;
  buffer_pool_manager_->UnpinPage(pid, true);
  UpdateRootPageId(1);
}

bool BPlusTree::InsertIntoLeaf(GenericKey *key, const RowId &value, Txn *transaction) {
  auto *page = FindLeafPage(key);
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  RowId dummy;
  if (leaf->Lookup(key, dummy, processor_)) {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return false;
  }
  int ns = leaf->Insert(key, value, processor_);
  if (ns > leaf->GetMaxSize()) {
    auto *nl = Split(leaf, transaction);
    InsertIntoParent(leaf, nl->KeyAt(0), nl, transaction);
    buffer_pool_manager_->UnpinPage(nl->GetPageId(), true);
  }
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
  return true;
}

void BPlusTree::InsertIntoParent(BPlusTreePage *old_node, GenericKey *key, BPlusTreePage *new_node, Txn *txn) {
  if (old_node->IsRootPage()) {
    page_id_t nrid;
    auto *page = buffer_pool_manager_->NewPage(nrid);
    if (!page) throw std::runtime_error("Out of memory");
    auto *nr = reinterpret_cast<InternalPage *>(page->GetData());
    nr->Init(nrid, INVALID_PAGE_ID, processor_.GetKeySize(), internal_max_size_);
    nr->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());
    old_node->SetParentPageId(nrid);
    new_node->SetParentPageId(nrid);
    root_page_id_ = nrid;
    UpdateRootPageId();
    buffer_pool_manager_->UnpinPage(nrid, true);
    return;
  }
  page_id_t pid = old_node->GetParentPageId();
  auto *parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(pid)->GetData());
  new_node->SetParentPageId(pid);
  int ns = parent->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
  if (ns > parent->GetMaxSize()) {
    auto *np = Split(parent, txn);
    InsertIntoParent(parent, np->KeyAt(1), np, txn);
    buffer_pool_manager_->UnpinPage(np->GetPageId(), true);
  }
  buffer_pool_manager_->UnpinPage(pid, true);
}

LeafPage *BPlusTree::Split(LeafPage *node, Txn *) {
  page_id_t pid;
  auto *page = buffer_pool_manager_->NewPage(pid);
  if (!page) throw std::runtime_error("Out of memory");
  auto *nn = reinterpret_cast<LeafPage *>(page->GetData());
  nn->Init(pid, node->GetParentPageId(), processor_.GetKeySize(), leaf_max_size_);
  node->MoveHalfTo(nn);
  return nn;
}

InternalPage *BPlusTree::Split(InternalPage *node, Txn *) {
  page_id_t pid;
  auto *page = buffer_pool_manager_->NewPage(pid);
  if (!page) throw std::runtime_error("Out of memory");
  auto *nn = reinterpret_cast<InternalPage *>(page->GetData());
  nn->Init(pid, node->GetParentPageId(), processor_.GetKeySize(), internal_max_size_);
  node->MoveHalfTo(nn, buffer_pool_manager_);
  return nn;
}

// ===== REMOVE — 逐级向上的 while 循环，不递归跨层 =====

void BPlusTree::Remove(const GenericKey *key, Txn *txn) {
  if (IsEmpty()) return;
  auto *page = FindLeafPage(key);
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  int old_sz = leaf->GetSize();
  leaf->RemoveAndDeleteRecord(key, processor_);
  if (old_sz == leaf->GetSize()) {  // key 不在这个 leaf 里
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return;
  }

  // cur: 当前需要检查 underflow 的结点。从叶子开始，逐级向上。
  BPlusTreePage *cur = leaf;
  bool is_leaf = true;

  while (true) {
    // 根或满足 min_size → 结束
    if (cur->IsRootPage()) {
      AdjustRoot(cur);
      buffer_pool_manager_->UnpinPage(cur->GetPageId(), true);
      return;
    }
    if (cur->GetSize() >= cur->GetMinSize()) {
      buffer_pool_manager_->UnpinPage(cur->GetPageId(), true);
      return;
    }

    // 获取 parent 和 sibling
    page_id_t p_id = cur->GetParentPageId();
    auto *p_page = buffer_pool_manager_->FetchPage(p_id);
    auto *parent = reinterpret_cast<InternalPage *>(p_page->GetData());
    int idx = parent->ValueIndex(cur->GetPageId());
    int sib_idx = (idx > 0) ? idx - 1 : idx + 1;
    page_id_t sib_id = parent->ValueAt(sib_idx);
    auto *sib_page = buffer_pool_manager_->FetchPage(sib_id);

    if (is_leaf) {
      auto *node = reinterpret_cast<LeafPage *>(cur);
      auto *sib  = reinterpret_cast<LeafPage *>(sib_page->GetData());
      if (node->GetSize() + sib->GetSize() >= node->GetMaxSize()) {
        // Redistribute leaf
        if (idx == 0) { sib->MoveFirstToEndOf(node); parent->SetKeyAt(1, sib->KeyAt(0)); }
        else          { sib->MoveLastToFrontOf(node); parent->SetKeyAt(idx, node->KeyAt(0)); }
        buffer_pool_manager_->UnpinPage(sib_id, true);
        buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
        buffer_pool_manager_->UnpinPage(p_id, true);
        return;
      }
      // Merge: 始终把右兄弟合并到左兄弟（CMU 参考做法），删除右兄弟
      if (idx > 0) {
        // node 是右孩子，sib(idx-1) 是左兄弟：node → sib, 删 node(idx)
        node->MoveAllTo(sib);
        parent->Remove(idx);
        buffer_pool_manager_->UnpinPage(sib_id, true);
        buffer_pool_manager_->UnpinPage(node->GetPageId(), false);
        buffer_pool_manager_->DeletePage(node->GetPageId());
      } else {
        // node 是左孩子(idx=0)，sib(idx+1) 是右兄弟：sib → node, 删 sib(1)
        sib->MoveAllTo(node);
        parent->Remove(1);
        buffer_pool_manager_->UnpinPage(sib_id, false);
        buffer_pool_manager_->DeletePage(sib_id);
        buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
      }
      cur = parent;
      is_leaf = false;
    } else {
      auto *node = reinterpret_cast<InternalPage *>(cur);
      auto *sib  = reinterpret_cast<InternalPage *>(sib_page->GetData());
      if (node->GetSize() + sib->GetSize() >= node->GetMaxSize()) {
        // Redistribute internal
        int sep_idx = (idx == 0) ? 1 : idx;
        char kbuf[processor_.GetKeySize()];
        memcpy(kbuf, parent->KeyAt(sep_idx), processor_.GetKeySize());
        if (idx == 0) {
          sib->MoveFirstToEndOf(node, reinterpret_cast<GenericKey *>(kbuf), buffer_pool_manager_);
          auto *lm = reinterpret_cast<LeafPage *>(FindLeafPage(nullptr, sib->ValueAt(0), true)->GetData());
          parent->SetKeyAt(1, lm->KeyAt(0));
          buffer_pool_manager_->UnpinPage(lm->GetPageId(), false);
        } else {
          sib->MoveLastToFrontOf(node, reinterpret_cast<GenericKey *>(kbuf), buffer_pool_manager_);
          auto *lm = reinterpret_cast<LeafPage *>(FindLeafPage(nullptr, node->ValueAt(0), true)->GetData());
          parent->SetKeyAt(sep_idx, lm->KeyAt(0));
          buffer_pool_manager_->UnpinPage(lm->GetPageId(), false);
        }
        buffer_pool_manager_->UnpinPage(sib_id, true);
        buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
        buffer_pool_manager_->UnpinPage(p_id, true);
        return;
      }
      // Merge internal: 右合并到左
      if (idx > 0) {
        // node 是右孩子，sib 是左兄弟：node → sib, parent->Remove(idx)
        node->MoveAllTo(sib, parent->KeyAt(idx), buffer_pool_manager_);
        parent->Remove(idx);
        buffer_pool_manager_->UnpinPage(sib_id, true);
        buffer_pool_manager_->UnpinPage(node->GetPageId(), false);
        buffer_pool_manager_->DeletePage(node->GetPageId());
      } else {
        // node 是左孩子(idx=0)，sib 是右兄弟：sib → node, parent->Remove(1)
        sib->MoveAllTo(node, parent->KeyAt(1), buffer_pool_manager_);
        parent->Remove(1);
        buffer_pool_manager_->UnpinPage(sib_id, false);
        buffer_pool_manager_->DeletePage(sib_id);
        buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
      }
      cur = parent;
    }
  }
}

bool BPlusTree::HandleLeafUnderflow(LeafPage *, Txn *) { return false; }
bool BPlusTree::HandleInternalUnderflow(InternalPage *, Txn *) { return false; }

bool BPlusTree::AdjustRoot(BPlusTreePage *old_root_node) {
  if (old_root_node->IsLeafPage()) {
    if (old_root_node->GetSize() == 0) {
      buffer_pool_manager_->UnpinPage(old_root_node->GetPageId(), false);
      buffer_pool_manager_->DeletePage(old_root_node->GetPageId());
      root_page_id_ = INVALID_PAGE_ID;
      UpdateRootPageId();
      return true;
    }
    return false;
  }
  if (old_root_node->GetSize() <= 1) {
    auto *ri = reinterpret_cast<InternalPage *>(old_root_node);
    page_id_t cid = ri->RemoveAndReturnOnlyChild();
    auto *cp = reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(cid)->GetData());
    cp->SetParentPageId(INVALID_PAGE_ID);
    buffer_pool_manager_->UnpinPage(cid, true);
    buffer_pool_manager_->UnpinPage(old_root_node->GetPageId(), false);
    buffer_pool_manager_->DeletePage(old_root_node->GetPageId());
    root_page_id_ = cid;
    UpdateRootPageId();
    return true;
  }
  return false;
}

IndexIterator BPlusTree::Begin() {
  if (IsEmpty()) return End();
  auto *pg = FindLeafPage(nullptr, root_page_id_, true);
  auto *lf = reinterpret_cast<LeafPage *>(pg->GetData());
  page_id_t lid = lf->GetPageId();
  buffer_pool_manager_->UnpinPage(lid, false);
  return IndexIterator(lid, buffer_pool_manager_, 0);
}

IndexIterator BPlusTree::Begin(const GenericKey *key) {
  if (IsEmpty()) return End();
  auto *pg = FindLeafPage(key);
  auto *lf = reinterpret_cast<LeafPage *>(pg->GetData());
  int ix = lf->KeyIndex(key, processor_);
  page_id_t lid = lf->GetPageId();
  buffer_pool_manager_->UnpinPage(lid, false);
  return IndexIterator(lid, buffer_pool_manager_, ix);
}

IndexIterator BPlusTree::End() { return IndexIterator(); }

Page *BPlusTree::FindLeafPage(const GenericKey *key, page_id_t page_id, bool leftMost) {
  if (page_id == INVALID_PAGE_ID) page_id = root_page_id_;
  for (;;) {
    auto *page = buffer_pool_manager_->FetchPage(page_id);
    auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());
    if (node->IsLeafPage()) return page;
    auto *in = reinterpret_cast<InternalPage *>(node);
    page_id_t next = leftMost ? in->ValueAt(0) : in->Lookup(key, processor_);
    buffer_pool_manager_->UnpinPage(node->GetPageId(), false);
    page_id = next;
  }
}

void BPlusTree::UpdateRootPageId(int insert_record) {
  auto *page = buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID);
  auto *roots = reinterpret_cast<IndexRootsPage *>(page->GetData());
  if (insert_record == 1) roots->Insert(index_id_, root_page_id_);
  else                    roots->Update(index_id_, root_page_id_);
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
}

void BPlusTree::ToGraph(BPlusTreePage *page, BufferPoolManager *bpm, std::ofstream &out, Schema *schema) const {
  std::string lpf("LEAF_"), ipf("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    out << lpf << leaf->GetPageId() << "[shape=plain color=green label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << leaf->GetPageId() << ",Parent=" << leaf->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">max=" << leaf->GetMaxSize() << ",min=" << leaf->GetMinSize() << ",size=" << leaf->GetSize() << "</TD></TR>\n<tr>";
    for (int i = 0; i < leaf->GetSize(); i++) { Row ans; processor_.DeserializeToKey(leaf->KeyAt(i), ans, schema); out << "<TD>" << ans.GetField(0)->toString() << "</TD>\n"; }
    out << "</TR></TABLE>>];\n";
    if (leaf->GetNextPageId() != INVALID_PAGE_ID)
      out << lpf << leaf->GetPageId() << " -> " << lpf << leaf->GetNextPageId() << ";\n{rank=same " << lpf << leaf->GetPageId() << " " << lpf << leaf->GetNextPageId() << "};\n";
    if (leaf->GetParentPageId() != INVALID_PAGE_ID)
      out << ipf << leaf->GetParentPageId() << ":p" << leaf->GetPageId() << " -> " << lpf << leaf->GetPageId() << ";\n";
  } else {
    auto *inner = reinterpret_cast<InternalPage *>(page);
    out << ipf << inner->GetPageId() << "[shape=plain color=pink label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << inner->GetPageId() << ",Parent=" << inner->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">max=" << inner->GetMaxSize() << ",min=" << inner->GetMinSize() << ",size=" << inner->GetSize() << "</TD></TR>\n<tr>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) { Row ans; processor_.DeserializeToKey(inner->KeyAt(i), ans, schema); out << ans.GetField(0)->toString(); }
      else out << " ";
      out << "</TD>\n";
    }
    out << "</TR></TABLE>>];\n";
    if (inner->GetParentPageId() != INVALID_PAGE_ID)
      out << ipf << inner->GetParentPageId() << ":p" << inner->GetPageId() << " -> " << ipf << inner->GetPageId() << ";\n";
    for (int i = 0; i < inner->GetSize(); i++) {
      auto *cp = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(cp, bpm, out, schema);
      if (i > 0) {
        auto *sp = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i - 1))->GetData());
        if (!sp->IsLeafPage() && !cp->IsLeafPage())
          out << "{rank=same " << ipf << sp->GetPageId() << " " << ipf << cp->GetPageId() << "};\n";
        bpm->UnpinPage(sp->GetPageId(), false);
      }
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

void BPlusTree::ToString(BPlusTreePage *page, BufferPoolManager *bpm) const {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId() << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) std::cout << leaf->KeyAt(i) << ",";
    std::cout << std::endl << std::endl;
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    std::cout << std::endl << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(internal->ValueAt(i))->GetData()), bpm);
      bpm->UnpinPage(internal->ValueAt(i), false);
    }
  }
}

bool BPlusTree::Check() {
  bool all_unpinned = buffer_pool_manager_->CheckAllUnpinned();
  if (!all_unpinned) LOG(ERROR) << "problem in page unpin" << endl;
  return all_unpinned;
}
