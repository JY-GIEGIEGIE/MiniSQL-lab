#include "page/b_plus_tree_internal_page.h"

#include "index/generic_key.h"

#define pairs_off (data_)
#define pair_size (GetKeySize() + sizeof(page_id_t))
#define key_off 0
#define val_off GetKeySize()

/**
 * TODO: Student Implement
 */
/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/
/*
 * Init method after creating a new internal page
 * Including set page type, set current size, set page id, set parent id and set
 * max page size
 */
void InternalPage::Init(page_id_t page_id, page_id_t parent_id, int key_size, int max_size) {
  SetPageType(IndexPageType::INTERNAL_PAGE);
  SetSize(0);
  SetPageId(page_id);
  SetParentPageId(parent_id);
  SetKeySize(key_size);
  SetMaxSize(max_size);
}
/*
 * Helper method to get/set the key associated with input "index"(a.k.a
 * array offset)
 */
GenericKey *InternalPage::KeyAt(int index) {
  return reinterpret_cast<GenericKey *>(pairs_off + index * pair_size + key_off);
}

void InternalPage::SetKeyAt(int index, GenericKey *key) {
  memcpy(pairs_off + index * pair_size + key_off, key, GetKeySize());
}

page_id_t InternalPage::ValueAt(int index) const {
  return *reinterpret_cast<const page_id_t *>(pairs_off + index * pair_size + val_off);
}

void InternalPage::SetValueAt(int index, page_id_t value) {
  *reinterpret_cast<page_id_t *>(pairs_off + index * pair_size + val_off) = value;
}

int InternalPage::ValueIndex(const page_id_t &value) const {
  for (int i = 0; i < GetSize(); ++i) {
    if (ValueAt(i) == value)
      return i;
  }
  return -1;
}

void *InternalPage::PairPtrAt(int index) {
  return KeyAt(index);
}

void InternalPage::PairCopy(void *dest, void *src, int pair_num) {
  memcpy(dest, src, pair_num * (GetKeySize() + sizeof(page_id_t)));
}
/*****************************************************************************
 * LOOKUP
 *****************************************************************************/
/*
 * Find and return the child pointer(page_id) which points to the child page
 * that contains input "key"
 * Start the search from the second key(the first key should always be invalid)
 * 用了二分查找
 */
page_id_t InternalPage::Lookup(const GenericKey *key, const KeyManager &KM) {
  // 二分查找：找到第一个 KeyAt(mid) > key 的位置，返回 ValueAt(mid-1)
  // key[0] 是 INVALID，从 index 1 开始搜索
  int lft = 1, rht = GetSize() - 1;
  while (lft <= rht) {
    int mid = (lft + rht) / 2;
    if (KM.CompareKeys(key, KeyAt(mid)) < 0) {
      rht = mid - 1;
    } else {
      lft = mid + 1;
    }
  }
  // 循环结束后 lft 是第一个 KeyAt(lft) > key 的位置，走 ValueAt(lft-1)
  return ValueAt(lft - 1);
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Populate new root page with old_value + new_key & new_value
 * When the insertion cause overflow from leaf page all the way upto the root
 * page, you should create a new root page and populate its elements.
 * NOTE: This method is only called within InsertIntoParent()(b_plus_tree.cpp)
 */
void InternalPage::PopulateNewRoot(const page_id_t &old_value, GenericKey *new_key, const page_id_t &new_value) {
  // 当根结点分裂时，创建一个新的根结点，只有两个指针 + 一个分隔键
  SetValueAt(0, old_value);
  SetKeyAt(1, new_key);
  SetValueAt(1, new_value);
  SetSize(2);  // 新根只有两对：[INVALID, old_value] 和 [new_key, new_value]
}

/*
 * Insert new_key & new_value pair right after the pair with its value ==
 * old_value
 * @return:  new size after insertion
 */
int InternalPage::InsertNodeAfter(const page_id_t &old_value, GenericKey *new_key, const page_id_t &new_value) {
  // 找到 old_value 在数组中的下标
  int old_index = ValueIndex(old_value);
  // 把 [old_index+1, size) 的所有键值对往后挪一格 —— 必须从后往前搬，否则会覆盖
  for (int i = GetSize() - 1; i > old_index; i--) {
    SetKeyAt(i + 1, KeyAt(i));
    SetValueAt(i + 1, ValueAt(i));
  }
  // 在腾出的位置写入新键值对
  SetKeyAt(old_index + 1, new_key);
  SetValueAt(old_index + 1, new_value);
  IncreaseSize(1);
  return GetSize();
}

/*****************************************************************************
 * SPLIT
 *****************************************************************************/
/*
 * Remove half of key & value pairs from this page to "recipient" page
 * buffer_pool_manager 是干嘛的？传给CopyNFrom()用于Fetch数据页
 */
void InternalPage::MoveHalfTo(InternalPage *recipient, BufferPoolManager *buffer_pool_manager) {
  // 从中间位置开始，把后半部分键值对搬给 recipient（新分裂出的兄弟页）
  int start = GetSize() / 2;
  int move_count = GetSize() - start;
  // PairPtrAt(start) 返回第 start 对键值对的起始地址，作为拷贝源
  recipient->CopyNFrom(PairPtrAt(start), move_count, buffer_pool_manager);
  SetSize(start);  // 本页保留前一半
}

/* Copy entries into me, starting from {items} and copy {size} entries.
 * Since it is an internal page, for all entries (pages) moved, their parents page now changes to me.
 * So I need to 'adopt' them by changing their parent page id, which needs to be persisted with BufferPoolManger
 *
 */
void InternalPage::CopyNFrom(void *src, int size, BufferPoolManager *buffer_pool_manager) {
  // 把 src 处的 size 对键值对拷贝到本页尾部
  int start = GetSize();
  PairCopy(PairPtrAt(start), src, size);
  // 逐个处理被搬来的子结点：更新它们的 parent_page_id 为本页 page_id
  for (int i = 0; i < size; i++) {
    page_id_t child_id = ValueAt(start + i);
    auto *child_page = reinterpret_cast<BPlusTreePage *>(
        buffer_pool_manager->FetchPage(child_id)->GetData());
    child_page->SetParentPageId(GetPageId());
    buffer_pool_manager->UnpinPage(child_id, true);  
    // 标记脏：parent 变了，如果不做，为什么导致丢失？
    // 因为后续可能会有其他操作把 child_page 从 buffer pool 中淘汰掉，如果没有标记脏，buffer pool 就不会把更新后的 parent_page_id 写回磁盘，导致丢失
  }
  IncreaseSize(size);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Remove the key & value pair in internal page according to input index(a.k.a
 * array offset)
 * NOTE: store key&value pair continuously after deletion
 */
void InternalPage::Remove(int index) {
  // 把 [index+1, size) 的键值对往前搬一格，覆盖被删位置
  for (int i = index; i < GetSize() - 1; i++) {
    SetKeyAt(i, KeyAt(i + 1));
    SetValueAt(i, ValueAt(i + 1));
  }
  IncreaseSize(-1);
}

/*
 * Remove the only key & value pair in internal page and return the value
 * NOTE: only call this method within AdjustRoot()(in b_plus_tree.cpp)
 * 当根内结点只剩一个子结点（size==1，只有 index=0 的 dummy 键 + 唯一 value）时调用。
 */
page_id_t InternalPage::RemoveAndReturnOnlyChild() {
  page_id_t child = ValueAt(0);
  SetSize(0);
  return child;
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
/*
 * Remove all of key & value pairs from this page to "recipient" page.
 * The middle_key is the separation key you should get from the parent. You need
 * to make sure the middle key is added to the recipient to maintain the invariant.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those
 * pages that are moved to the recipient
 */
void InternalPage::MoveAllTo(InternalPage *recipient, GenericKey *middle_key, BufferPoolManager *buffer_pool_manager) {
  // 把 middle_key + 本页第一个子结点 放在 recipient 末尾，作为新旧数据之间的分隔
  recipient->SetKeyAt(recipient->GetSize(), middle_key);
  recipient->SetValueAt(recipient->GetSize(), ValueAt(0));
  // 收养这个被搬走的子结点
  auto *first_child = reinterpret_cast<BPlusTreePage *>(
      buffer_pool_manager->FetchPage(ValueAt(0))->GetData());
  first_child->SetParentPageId(recipient->GetPageId());
  buffer_pool_manager->UnpinPage(ValueAt(0), true);
  recipient->IncreaseSize(1);
  // 把本页剩余的键值对（index 1 ~ size-1）逐个追加到 recipient 尾部
  for (int i = 1; i < GetSize(); i++) {
    recipient->CopyLastFrom(KeyAt(i), ValueAt(i), buffer_pool_manager);
  }
  SetSize(0);  // 本页已被搬空
}

/*****************************************************************************
 * REDISTRIBUTE
 *****************************************************************************/
/*
 * Remove the first key & value pair from this page to tail of "recipient" page.
 *
 * The middle_key is the separation key you should get from the parent. You need
 * to make sure the middle key is added to the recipient to maintain the invariant.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those
 * pages that are moved to the recipient
 */
void InternalPage::MoveFirstToEndOf(InternalPage *recipient, GenericKey *middle_key,
                                    BufferPoolManager *buffer_pool_manager) {
  // 把本页的第一个子结点（ValueAt(0)）搬到 recipient 末尾，
  // middle_key（父结点中的分隔键）作为新的分隔键放在 recipient 末尾
  recipient->CopyLastFrom(middle_key, ValueAt(0), buffer_pool_manager);
  // 本页数据前移一格：原先 index=1 的键值对现在变成 index=0
  for (int i = 0; i < GetSize() - 1; i++) {
    SetKeyAt(i, KeyAt(i + 1));
    SetValueAt(i, ValueAt(i + 1));
  }
  IncreaseSize(-1);
}

/* Append an entry at the end.
 * Since it is an internal page, the moved entry(page)’s parent needs to be updated.
 * So I need to ‘adopt’ it by changing its parent page id, which needs to be persisted with BufferPoolManger
 */
void InternalPage::CopyLastFrom(GenericKey *key, const page_id_t value, BufferPoolManager *buffer_pool_manager) {
  // 在本页末尾追加一对键值对，并处理被搬来的子结点
  SetKeyAt(GetSize(), key);
  SetValueAt(GetSize(), value);
  auto *child = reinterpret_cast<BPlusTreePage *>(
      buffer_pool_manager->FetchPage(value)->GetData());
  child->SetParentPageId(GetPageId());
  buffer_pool_manager->UnpinPage(value, true);
  IncreaseSize(1);
}

/*
 * Remove the last key & value pair from this page to head of "recipient" page.
 * You need to handle the original dummy key properly, e.g. updating recipient’s array to position the middle_key at the
 * right place.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those pages that are
 * moved to the recipient
 */
void InternalPage::MoveLastToFrontOf(InternalPage *recipient, GenericKey *middle_key,
                                     BufferPoolManager *buffer_pool_manager) {
  // 把本页最后一个子结点 pointer 搬到 recipient 最前面
  recipient->CopyFirstFrom(ValueAt(GetSize() - 1), buffer_pool_manager);
  // CopyFirstFrom 把所有数据右移了一格，原来的 KeyAt(0)=INVALID 被推到 KeyAt(1)
  // 需要用 middle_key 覆盖 KeyAt(1)，使之成为合法的分隔键
  recipient->SetKeyAt(1, middle_key);
  IncreaseSize(-1);  // 本页少了一个
}

/* Append an entry at the beginning.
 * Since it is an internal page, the moved entry(page)’s parent needs to be updated.
 * So I need to ‘adopt’ it by changing its parent page id, which needs to be persisted with BufferPoolManger
 */
void InternalPage::CopyFirstFrom(const page_id_t value, BufferPoolManager *buffer_pool_manager) {
  // 现有键值对全部往后挪一格（从后往前搬）
  for (int i = GetSize() - 1; i >= 0; i--) {
    SetKeyAt(i + 1, KeyAt(i));
    SetValueAt(i + 1, ValueAt(i));
  }
  // 在最前面放入新 value
  SetValueAt(0, value);
  // 处理这个子结点
  auto *child = reinterpret_cast<BPlusTreePage *>(
      buffer_pool_manager->FetchPage(value)->GetData());
  child->SetParentPageId(GetPageId());
  buffer_pool_manager->UnpinPage(value, true);
  IncreaseSize(1);
}