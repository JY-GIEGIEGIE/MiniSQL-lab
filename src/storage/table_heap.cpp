#include "storage/table_heap.h"

/**
 * InsertTuple: 向堆表中插入一行记录。
 *
 * 采用 First Fit 策略：沿 TablePage 双向链表从 first_page_id_ 开始
 * 逐页查找，找到第一个能容纳该记录的页就插入。如果所有已有页都装不下，
 * 则新建一个 TablePage 挂在链表末尾，在新页中插入。
 */
bool TableHeap::InsertTuple(Row &row, Txn *txn) {
  // 如果记录序列化后的大小超过了单页能容纳的最大值，直接拒绝。
  // SIZE_MAX_ROW = PAGE_SIZE - 表头 - 一条slot条目。
  if (row.GetSerializedSize(schema_) > TablePage::SIZE_MAX_ROW) {
    return false;
  }

  // 从链表头开始遍历每一页
  page_id_t page_id = first_page_id_;
  page_id_t prev_page_id = INVALID_PAGE_ID;  // 记录前一页，用于新建页时更新链接
  while (page_id != INVALID_PAGE_ID) {
    auto *page = reinterpret_cast<TablePage *>(
        buffer_pool_manager_->FetchPage(page_id));
    if (page == nullptr) {
      return false;
    }

    // 尝试在当前页插入
    if (page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_)) {
      // 插入成功：row.rid_ 已被 TablePage::InsertTuple 设置好
      buffer_pool_manager_->UnpinPage(page_id, true);  // 标记脏
      return true;
    }

    // 当前页装不下，去下一页
    prev_page_id = page_id;
    page_id = page->GetNextPageId();
    buffer_pool_manager_->UnpinPage(prev_page_id, false);  // 未修改，不标记脏
  }

  // 所有已有页都装不下——新建一页
  page_id_t new_page_id;
  auto *new_page = buffer_pool_manager_->NewPage(new_page_id);
  if (new_page == nullptr) {
    return false;  // 缓冲池满了，无法创建新页
  }

  // 初始化新 TablePage
  auto *new_table_page = reinterpret_cast<TablePage *>(new_page);
  new_table_page->Init(new_page_id, prev_page_id, log_manager_, txn);

  // 如果这不是表的第一页，更新前一页的 NextPageId 指向新页
  if (prev_page_id != INVALID_PAGE_ID) {
    auto *prev_page = reinterpret_cast<TablePage *>(
        buffer_pool_manager_->FetchPage(prev_page_id));
    if (prev_page != nullptr) {
      prev_page->SetNextPageId(new_page_id);
      buffer_pool_manager_->UnpinPage(prev_page_id, true);  // 脏：修改了 NextPageId
    }
  }

  // 在新页中插入记录
  bool inserted = new_table_page->InsertTuple(row, schema_, txn,
                                               lock_manager_, log_manager_);
  buffer_pool_manager_->UnpinPage(new_page_id, true);  // 脏：写入了新数据
  return inserted;
}

/**
 * MarkDelete: 逻辑删除——将记录的 DELETE_MASK 标记位置 1，不回收物理空间。
 */
bool TableHeap::MarkDelete(const RowId &rid, Txn *txn) {
  auto page = reinterpret_cast<TablePage *>(
      buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if (page == nullptr) {
    return false;
  }
  page->WLatch();
  page->MarkDelete(rid, txn, lock_manager_, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
  return true;
}

/**
 * UpdateTuple: 将 RowId 为 rid 的旧记录替换为新记录。
 *
 * 策略：先尝试原地更新（TablePage::UpdateTuple）。
 * 如果旧页空间不足以容纳新记录（UpdateTuple 返回 false），
 * 则采用"删旧插新"：MarkDelete 标记删除 → ApplyDelete 物理回收
 * → InsertTuple 把新记录插入到有足够空间的位置。
 */
bool TableHeap::UpdateTuple(Row &row, const RowId &rid, Txn *txn) {
  // 新纪录是否大到任何单页都装不下
  if (row.GetSerializedSize(schema_) > TablePage::SIZE_MAX_ROW) {
    return false;
  }

  // 尝试在原页原地更新
  auto *page = reinterpret_cast<TablePage *>(
      buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if (page == nullptr) {
    return false;
  }

  // 先读出旧记录到临时 Row（UpdateTuple 需要 old_row 参数）
  Row old_row(rid);
  bool old_exists = page->GetTuple(&old_row, schema_, txn, lock_manager_);
  if (!old_exists) {
    buffer_pool_manager_->UnpinPage(rid.GetPageId(), false);
    return false;
  }

  // 尝试原地更新
  bool updated = page->UpdateTuple(row, &old_row, schema_, txn,
                                    lock_manager_, log_manager_);
  if (updated) {
    // 原地更新成功，new_row.rid_ 已在 TablePage 内部被设为和 old_row 相同
    buffer_pool_manager_->UnpinPage(rid.GetPageId(), true);
    return true;
  }

  // 原地更新失败（空间不够），释放旧页
  buffer_pool_manager_->UnpinPage(rid.GetPageId(), false);

  // 删旧 + 插新
  if (!MarkDelete(rid, txn)) {
    return false;
  }
  ApplyDelete(rid, txn);
  return InsertTuple(row, txn);
}

/**
 * ApplyDelete: 物理删除——真正回收记录占用的空间并移动数据消除空洞。
 * 通常跟在 MarkDelete 之后调用（事务提交时）。
 */
void TableHeap::ApplyDelete(const RowId &rid, Txn *txn) {
  // 找到记录所在的页
  auto *page = reinterpret_cast<TablePage *>(
      buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if (page == nullptr) {
    return;
  }

  // 委托 TablePage 执行物理删除（回收空间、移动后续记录、更新 offset）
  page->ApplyDelete(rid, txn, log_manager_);
  buffer_pool_manager_->UnpinPage(rid.GetPageId(), true);
}

/**
 * RollbackDelete: 回滚逻辑删除——清除 DELETE_MASK 标记使记录重新可见。
 */
void TableHeap::RollbackDelete(const RowId &rid, Txn *txn) {
  auto page = reinterpret_cast<TablePage *>(
      buffer_pool_manager_->FetchPage(rid.GetPageId()));
  assert(page != nullptr);
  page->WLatch();
  page->RollbackDelete(rid, txn, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
}

/**
 * GetTuple: 根据 RowId 读取一条记录。
 * 入参 row 已设置好 RowId（row->GetRowId()），函数从对应页的对应
 * slot 反序列化出数据填充进 row 的 fields_ 向量。
 */
bool TableHeap::GetTuple(Row *row, Txn *txn) {
  // 找到 RowId 中 page_id 对应的页
  auto *page = reinterpret_cast<TablePage *>(
      buffer_pool_manager_->FetchPage(row->GetRowId().GetPageId()));
  if (page == nullptr) {
    return false;
  }

  // 委托 TablePage 根据 slot_num 定位并反序列化
  bool found = page->GetTuple(row, schema_, txn, lock_manager_);
  buffer_pool_manager_->UnpinPage(row->GetRowId().GetPageId(), false);
  return found;
}

/**
 * DeleteTable: 递归删除整张表的所有页并释放磁盘空间。
 */
void TableHeap::DeleteTable(page_id_t page_id) {
  if (page_id != INVALID_PAGE_ID) {
    auto temp_table_page = reinterpret_cast<TablePage *>(
        buffer_pool_manager_->FetchPage(page_id));
    if (temp_table_page->GetNextPageId() != INVALID_PAGE_ID)
      DeleteTable(temp_table_page->GetNextPageId());
    buffer_pool_manager_->UnpinPage(page_id, false);
    buffer_pool_manager_->DeletePage(page_id);
  } else {
    DeleteTable(first_page_id_);
  }
}

/**
 * Begin: 返回指向表内第一条有效记录的迭代器。
 *
 * 从 first_page_id_ 开始沿链表逐页查找，对每一页调 GetFirstTupleRid
 * 找第一个未被删除的记录。如果整张表为空，返回 End 迭代器
 * （RowId 为 INVALID_ROWID）。
 */
TableIterator TableHeap::Begin(Txn *txn) {
  page_id_t page_id = first_page_id_;
  while (page_id != INVALID_PAGE_ID) {
    auto *page = reinterpret_cast<TablePage *>(
        buffer_pool_manager_->FetchPage(page_id));
    if (page == nullptr) {
      break;
    }

    RowId first_rid;
    bool found = page->GetFirstTupleRid(&first_rid);
    buffer_pool_manager_->UnpinPage(page_id, false);

    if (found) {
      // 找到第一条有效记录，构造指向它的迭代器
      return TableIterator(this, first_rid, txn);
    }

    // 当前页没有有效记录，去下一页
    page_id = page->GetNextPageId();
  }

  // 表为空，返回尾后迭代器
  return End();
}

/**
 * End: 返回尾后迭代器，表示"已遍历完所有记录"的哨兵。
 * End 迭代器的 RowId 设为 INVALID_ROWID（page_id = INVALID_PAGE_ID）。
 */
TableIterator TableHeap::End() {
  return TableIterator(this, RowId(INVALID_PAGE_ID, 0), nullptr);
}
