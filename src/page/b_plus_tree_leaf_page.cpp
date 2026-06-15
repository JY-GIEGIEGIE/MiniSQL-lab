#include "page/b_plus_tree_leaf_page.h"

#include <algorithm>

#include "index/generic_key.h"

#define pairs_off (data_)
#define pair_size (GetKeySize() + sizeof(RowId))
#define key_off 0
#define val_off GetKeySize()
/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/

/**
 * TODO: Student Implement
 */
/**
 * Init method after creating a new leaf page
 * Including set page type, set current size to zero, set page id/parent id, set
 * next page id and set max size
 * 未初始化next_page_id
 */
void LeafPage::Init(page_id_t page_id, page_id_t parent_id, int key_size, int max_size) {
  SetPageType(IndexPageType::LEAF_PAGE);
  SetSize(0);
  SetPageId(page_id);
  SetParentPageId(parent_id);
  SetKeySize(key_size);
  SetMaxSize(max_size);
  SetNextPageId(INVALID_PAGE_ID);  // 初始无后继叶子页
}

/**
 * Helper methods to set/get next page id
 */
page_id_t LeafPage::GetNextPageId() const {
  return next_page_id_;
}

void LeafPage::SetNextPageId(page_id_t next_page_id) {
  next_page_id_ = next_page_id;
  if (next_page_id == 0) {
    LOG(INFO) << "Fatal error";
  }
}

/**
 * TODO: Student Implement
 */
/**
 * Helper method to find the first index i so that pairs_[i].first >= key
 * NOTE: This method is only used when generating index iterator
 * 二分查找
 */
int LeafPage::KeyIndex(const GenericKey *key, const KeyManager &KM) {
  int lft = 0, rht = GetSize() - 1;
  while (lft <= rht) {
    int mid = (lft + rht) / 2;
    if (KM.CompareKeys(key, KeyAt(mid)) <= 0) { // key <= KeyAt(mid)，目标在左半区
      rht = mid - 1;
    } else { // key > KeyAt(mid)，目标在右半区
      lft = mid + 1;
    }
  }
  return lft; // 循环结束后 lft 是第一个 KeyAt(lft) >= key 的位置
}

/*
 * Helper method to find and return the key associated with input "index"(a.k.a
 * array offset)
 */
GenericKey *LeafPage::KeyAt(int index) {
  return reinterpret_cast<GenericKey *>(pairs_off + index * pair_size + key_off);
}

void LeafPage::SetKeyAt(int index, GenericKey *key) {
  memcpy(pairs_off + index * pair_size + key_off, key, GetKeySize());
}

RowId LeafPage::ValueAt(int index) const {
  return *reinterpret_cast<const RowId *>(pairs_off + index * pair_size + val_off);
}

void LeafPage::SetValueAt(int index, RowId value) {
  *reinterpret_cast<RowId *>(pairs_off + index * pair_size + val_off) = value;
}

void *LeafPage::PairPtrAt(int index) {
  return KeyAt(index);
}

void LeafPage::PairCopy(void *dest, void *src, int pair_num) {
  memcpy(dest, src, pair_num * (GetKeySize() + sizeof(RowId)));
}
/*
 * Helper method to find and return the key & value pair associated with input
 * "index"(a.k.a. array offset)
 */
std::pair<GenericKey *, RowId> LeafPage::GetItem(int index) { return {KeyAt(index), ValueAt(index)}; }

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert key & value pair into leaf page ordered by key
 * @return page size after insertion
 */
int LeafPage::Insert(GenericKey *key, const RowId &value, const KeyManager &KM) {
  int index = KeyIndex(key, KM);
  if (index < GetSize() && KM.CompareKeys(key, KeyAt(index)) == 0) {
    // Key already exists, update value
    SetValueAt(index, value);
    return GetSize();
  }
  // Key does not exist, insert new key-value pair
  for (int i = GetSize(); i > index; i--) {
    SetKeyAt(i, KeyAt(i - 1));
    SetValueAt(i, ValueAt(i - 1));
  }
  SetKeyAt(index, key);
  SetValueAt(index, value);
  IncreaseSize(1);
  return GetSize();
}

/*****************************************************************************
 * SPLIT
 *****************************************************************************/
/*
 * Remove half of key & value pairs from this page to "recipient" page
 */
void LeafPage::MoveHalfTo(LeafPage *recipient) {
  int start = GetSize() / 2;
  int move_count = GetSize() - start;
  // 把 [start, size) 的键值对拷贝到 recipient 的 data_ 数组头部（recipient 是刚创建的空页）
  recipient->CopyNFrom(PairPtrAt(start), move_count);
  // 维护叶子链表：recipient 插在本页和本页原来的下一页之间
  recipient->SetNextPageId(GetNextPageId());
  SetNextPageId(recipient->GetPageId());
  SetSize(start);  // 本页保留前一半
}

/*
 * Copy starting from items, and copy {size} number of elements into me.
 */
void LeafPage::CopyNFrom(void *src, int size) {
  // 把 src 处的 size 对键值对追加到本页 data_ 尾部
  PairCopy(PairPtrAt(GetSize()), src, size);
  IncreaseSize(size);
}

/*****************************************************************************
 * LOOKUP
 *****************************************************************************/
/*
 * For the given key, check to see whether it exists in the leaf page. If it
 * does, then store its corresponding value in input "value" and return true.
 * If the key does not exist, then return false
 */
bool LeafPage::Lookup(const GenericKey *key, RowId &value, const KeyManager &KM) {
  int index = KeyIndex(key, KM); // 找到第一个 KeyAt(index) >= key 的位置
  if (index < GetSize() && KM.CompareKeys(key, KeyAt(index)) == 0) {
    value = ValueAt(index);
    return true;
  }
  return false;
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * First look through leaf page to see whether delete key exist or not. If
 * existed, perform deletion, otherwise return immediately.
 * NOTE: store key&value pair continuously after deletion
 * @return  page size after deletion
 */
int LeafPage::RemoveAndDeleteRecord(const GenericKey *key, const KeyManager &KM) {
  int index = KeyIndex(key, KM);
  // key 不存在，直接返回
  if (index >= GetSize() || KM.CompareKeys(key, KeyAt(index)) != 0) {
    return GetSize();
  }
  // 把 [index+1, size) 的键值对往前搬一格，覆盖被删位置
  for (int i = index; i < GetSize() - 1; i++) {
    SetKeyAt(i, KeyAt(i + 1));
    SetValueAt(i, ValueAt(i + 1));
  }
  IncreaseSize(-1);
  return GetSize();
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
/*
 * Remove all key & value pairs from this page to "recipient" page. Don't forget
 * to update the next_page id in the sibling page
 */
void LeafPage::MoveAllTo(LeafPage *recipient) {
  recipient->CopyNFrom(PairPtrAt(0), GetSize());
  SetSize(0);
  // 若本页在 recipient 左边（本页 next → recipient），不复写 NextPageId 避免自环
  if (GetNextPageId() != recipient->GetPageId())
    recipient->SetNextPageId(GetNextPageId());
}

/*****************************************************************************
 * REDISTRIBUTE
 *****************************************************************************/
/*
 * Remove the first key & value pair from this page to "recipient" page.
 *
 */
void LeafPage::MoveFirstToEndOf(LeafPage *recipient) {
  // 把本页第一个键值对搬到 recipient 末尾
  recipient->CopyLastFrom(KeyAt(0), ValueAt(0));
  // 本页数据前移一格，覆盖被搬走的位置
  for (int i = 0; i < GetSize() - 1; i++) {
    SetKeyAt(i, KeyAt(i + 1));
    SetValueAt(i, ValueAt(i + 1));
  }
  IncreaseSize(-1);
}

/*
 * Copy the item into the end of my item list. (Append item to my array)
 */
void LeafPage::CopyLastFrom(GenericKey *key, const RowId value) {
  // 在本页末尾追加一对键值对
  SetKeyAt(GetSize(), key);
  SetValueAt(GetSize(), value);
  IncreaseSize(1);
}

/*
 * Remove the last key & value pair from this page to "recipient" page.
 */
void LeafPage::MoveLastToFrontOf(LeafPage *recipient) {
  // 把本页最后一个键值对搬到 recipient 最前面
  recipient->CopyFirstFrom(KeyAt(GetSize() - 1), ValueAt(GetSize() - 1));
  IncreaseSize(-1);
}

/*
 * Insert item at the front of my items. Move items accordingly.
 *
 */
void LeafPage::CopyFirstFrom(GenericKey *key, const RowId value) {
  // 现有键值对全部往后挪一格（从后往前搬，避免覆盖）
  for (int i = GetSize() - 1; i >= 0; i--) {
    SetKeyAt(i + 1, KeyAt(i));
    SetValueAt(i + 1, ValueAt(i));
  }
  // 在最前面写入新键值对
  SetKeyAt(0, key);
  SetValueAt(0, value);
  IncreaseSize(1);
}