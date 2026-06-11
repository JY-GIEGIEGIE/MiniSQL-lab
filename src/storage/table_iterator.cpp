#include "storage/table_iterator.h"

#include "common/macros.h"
#include "storage/table_heap.h"

/**
 * 构造函数：创建一个指向特定记录的迭代器。
 *
 * 如果 rid 是有效的（page_id != INVALID_PAGE_ID），则从 TableHeap
 * 中读出该记录的数据存入 row_。
 * 如果 rid 是 INVALID_ROWID（End 迭代器），row_ 置空。
 */
TableIterator::TableIterator(TableHeap *table_heap, RowId rid, Txn *txn)
    : table_heap_(table_heap), txn_(txn) {
  if (rid.GetPageId() != INVALID_PAGE_ID) {
    // 有效记录：构造 Row 并从堆表加载数据
    row_ = new Row(rid);
    table_heap_->GetTuple(row_, txn_);
  } else {
    // End 迭代器哨兵
    row_ = nullptr;
  }
}

/**
 * 拷贝构造：深拷贝 row_（如果非空），避免两个迭代器共享同一 Row 对象。
 */
TableIterator::TableIterator(const TableIterator &other)
    : table_heap_(other.table_heap_), txn_(other.txn_) {
  if (other.row_ != nullptr) {
    row_ = new Row(*other.row_);  // Row 的拷贝构造是深拷贝
  } else {
    row_ = nullptr;
  }
}

/**
 * 析构：释放 row_ 对象。
 */
TableIterator::~TableIterator() {
  delete row_;
}

/**
 * operator==：两个迭代器相等当且仅当它们指向同一个 RowId。
 * 同时 row_ 都为空（End 哨兵）也认为相等。
 */
bool TableIterator::operator==(const TableIterator &itr) const {
  if (row_ == nullptr && itr.row_ == nullptr) {
    return true;  // 两个都是 End 迭代器
  }
  if (row_ == nullptr || itr.row_ == nullptr) {
    return false;  // 一个是 End 一个不是
  }
  return row_->GetRowId() == itr.row_->GetRowId();
}

bool TableIterator::operator!=(const TableIterator &itr) const {
  return !(*this == itr);
}

/**
 * operator*：返回当前记录的 const 引用。
 */
const Row &TableIterator::operator*() {
  return *row_;
}

/**
 * operator->：返回当前记录的指针。
 */
Row *TableIterator::operator->() {
  return row_;
}

/**
 * operator=：赋值运算符，深拷贝 row_。
 */
TableIterator &TableIterator::operator=(const TableIterator &itr) noexcept {
  if (this == &itr) {
    return *this;  // 自赋值保护
  }
  table_heap_ = itr.table_heap_;
  txn_ = itr.txn_;
  delete row_;
  if (itr.row_ != nullptr) {
    row_ = new Row(*itr.row_);
  } else {
    row_ = nullptr;
  }
  return *this;
}

/**
 * operator++（前缀 ++iter）：移动到下一条有效记录。
 *
 * 步骤：
 * 1. 在当前页内调 GetNextTupleRid 找下一个未被删除的记录
 * 2. 如果当前页没有更多记录，沿 NextPageId 去下一页，
 *    在下一页调 GetFirstTupleRid
 * 3. 如果链表走到头，设为 End 迭代器（row_ = nullptr）
 */
TableIterator &TableIterator::operator++() {
  RowId cur_rid = row_->GetRowId();
  page_id_t page_id = cur_rid.GetPageId();

  // Step 1: 在当前页内查找下一条记录
  auto *page = reinterpret_cast<TablePage *>(
      table_heap_->buffer_pool_manager_->FetchPage(page_id));
  if (page != nullptr) {
    RowId next_rid;
    bool found = page->GetNextTupleRid(cur_rid, &next_rid);
    page_id_t next_page_id = page->GetNextPageId();
    table_heap_->buffer_pool_manager_->UnpinPage(page_id, false);

    if (found) {
      // 当前页内找到了，更新 row_
      row_->SetRowId(next_rid);
      table_heap_->GetTuple(row_, txn_);
      return *this;
    }

    // Step 2: 当前页没有更多记录，沿链表去下一页
    page_id = next_page_id;
    while (page_id != INVALID_PAGE_ID) {
      auto *next_page = reinterpret_cast<TablePage *>(
          table_heap_->buffer_pool_manager_->FetchPage(page_id));
      if (next_page == nullptr) {
        break;
      }

      RowId first_rid;
      bool first_found = next_page->GetFirstTupleRid(&first_rid);
      page_id_t following_page_id = next_page->GetNextPageId();
      table_heap_->buffer_pool_manager_->UnpinPage(page_id, false);

      if (first_found) {
        // 在新页找到第一条有效记录
        row_->SetRowId(first_rid);
        table_heap_->GetTuple(row_, txn_);
        return *this;
      }

      page_id = following_page_id;
    }
  }

  // Step 3: 链表走到头——设为 End 迭代器
  delete row_;
  row_ = nullptr;
  return *this;
}

/**
 * operator++(int)（后缀 iter++）：返回当前位置的拷贝，自身前进到下一位。
 */
TableIterator TableIterator::operator++(int) {
  TableIterator copy(*this);  // 拷贝当前状态
  ++(*this);                  // 自身前进（调用前缀 ++）
  return copy;                // 返回"前进前"的拷贝
}
