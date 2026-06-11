#include "storage/table_heap.h"

#include <unordered_map>
#include <vector>

#include "common/instance.h"
#include "gtest/gtest.h"
#include "record/field.h"
#include "record/schema.h"
#include "utils/utils.h"

static string db_file_name = "table_heap_student_test.db";
using Fields = std::vector<Field>;

// =============================================================================
// MarkDelete + ApplyDelete：逻辑删除后物理删除，记录不可再读取
// =============================================================================

TEST(TableHeapStudentTest, DeleteThenGetTupleReturnsFalse) {
  remove(db_file_name.c_str());
  auto disk_mgr = new DiskManager(db_file_name);
  auto bpm = new BufferPoolManager(10, disk_mgr);

  std::vector<Column *> columns = {
      new Column("id", TypeId::kTypeInt, 0, false, false),
      new Column("name", TypeId::kTypeChar, 64, 1, true, false),
  };
  auto schema = std::make_shared<Schema>(columns);

  auto *table = TableHeap::Create(bpm, schema.get(), nullptr, nullptr, nullptr);

  // 插入一条记录
  char name[] = "test_name";
  Fields fields = {Field(TypeId::kTypeInt, 100),
                   Field(TypeId::kTypeChar, name, strlen(name), true)};
  Row row(fields);
  ASSERT_TRUE(table->InsertTuple(row, nullptr));
  RowId rid = row.GetRowId();

  // 逻辑删除后 GetTuple 返回 false（DELETE_MASK 被置位）
  ASSERT_TRUE(table->MarkDelete(rid, nullptr));
  Row deleted_row(rid);
  ASSERT_FALSE(table->GetTuple(&deleted_row, nullptr));

  // RollbackDelete 恢复后可以读取
  table->RollbackDelete(rid, nullptr);
  Row recovered(rid);
  ASSERT_TRUE(table->GetTuple(&recovered, nullptr));

  // 物理删除（MarkDelete + ApplyDelete），之后 Rollback 无法恢复
  ASSERT_TRUE(table->MarkDelete(rid, nullptr));
  table->ApplyDelete(rid, nullptr);
  Row gone(rid);
  ASSERT_FALSE(table->GetTuple(&gone, nullptr));

  disk_mgr->Close();
  remove(db_file_name.c_str());
  delete table;
  delete bpm;
  delete disk_mgr;
}

// =============================================================================
// UpdateTuple：原地更新 vs 删旧插新
// =============================================================================

TEST(TableHeapStudentTest, UpdateTupleInPlace) {
  remove(db_file_name.c_str());
  auto disk_mgr = new DiskManager(db_file_name);
  auto bpm = new BufferPoolManager(10, disk_mgr);

  std::vector<Column *> columns = {
      new Column("id", TypeId::kTypeInt, 0, false, false),
      new Column("value", TypeId::kTypeInt, 1, false, false),
  };
  auto schema = std::make_shared<Schema>(columns);
  auto *table = TableHeap::Create(bpm, schema.get(), nullptr, nullptr, nullptr);

  // 插入初始行
  Fields fields1 = {Field(TypeId::kTypeInt, 1), Field(TypeId::kTypeInt, 10)};
  Row row1(fields1);
  ASSERT_TRUE(table->InsertTuple(row1, nullptr));
  RowId rid = row1.GetRowId();

  // 原地更新：新行大小一样（都是两个 int），应成功
  Fields fields2 = {Field(TypeId::kTypeInt, 1), Field(TypeId::kTypeInt, 999)};
  Row row2(fields2);
  ASSERT_TRUE(table->UpdateTuple(row2, rid, nullptr));

  // 验证更新后的数据
  Row result(rid);
  ASSERT_TRUE(table->GetTuple(&result, nullptr));
  ASSERT_EQ(CmpBool::kTrue, result.GetField(1)->CompareEquals(fields2[1]));

  disk_mgr->Close();
  remove(db_file_name.c_str());
  delete table;
  delete bpm;
  delete disk_mgr;
}

TEST(TableHeapStudentTest, UpdateTupleDeleteAndInsert) {
  remove(db_file_name.c_str());
  auto disk_mgr = new DiskManager(db_file_name);
  auto bpm = new BufferPoolManager(50, disk_mgr);  // 大一些的缓冲池

  // 两列：一个小 int 和一个大 char —— 更新 char 变大时原页装不下
  std::vector<Column *> columns = {
      new Column("id", TypeId::kTypeInt, 0, false, false),
      new Column("bio", TypeId::kTypeChar, 3000, 1, true, false),
  };
  auto schema = std::make_shared<Schema>(columns);
  auto *table = TableHeap::Create(bpm, schema.get(), nullptr, nullptr, nullptr);

  // 插入多条小记录填满第一页，使得原地更新的空间不够
  char filler[2000];
  memset(filler, 'X', 1999);
  filler[1999] = '\0';

  // 填满第一页：每条约 2004 字节，一页只能装约 2 条，插 3 条保证触发第二页
  for (int i = 0; i < 5; i++) {
    Fields f = {Field(TypeId::kTypeInt, i),
                Field(TypeId::kTypeChar, filler, 2000, true)};
    Row r(f);
    ASSERT_TRUE(table->InsertTuple(r, nullptr));
  }

  // 插入一条小记录（它很可能在第二页或更后）
  char tiny[10] = "hello";
  Fields tiny_fields = {Field(TypeId::kTypeInt, 999),
                        Field(TypeId::kTypeChar, tiny, 5, true)};
  Row tiny_row(tiny_fields);
  ASSERT_TRUE(table->InsertTuple(tiny_row, nullptr));
  RowId rid = tiny_row.GetRowId();

  // 原地装不下新数据（2000 > 5），走 删旧+插新 路径
  Fields big_fields = {Field(TypeId::kTypeInt, 999),
                       Field(TypeId::kTypeChar, filler, 2000, true)};
  Row big_row(big_fields);
  ASSERT_TRUE(table->UpdateTuple(big_row, rid, nullptr));

  // 验证更新后的值：删旧插新后 rid 已变，用 big_row 的新 rid
  RowId new_rid = big_row.GetRowId();
  ASSERT_FALSE(rid == new_rid);  // rid 发生了变化（原空间不够，移到新位置）
  Row result(new_rid);
  ASSERT_TRUE(table->GetTuple(&result, nullptr));
  ASSERT_EQ(CmpBool::kTrue, result.GetField(1)->CompareEquals(big_fields[1]));

  disk_mgr->Close();
  remove(db_file_name.c_str());
  delete table;
  delete bpm;
  delete disk_mgr;
}

// =============================================================================
// 迭代器测试：跨页遍历、空表
// =============================================================================

TEST(TableHeapStudentTest, IteratorTraversal) {
  remove(db_file_name.c_str());
  auto disk_mgr = new DiskManager(db_file_name);
  auto bpm = new BufferPoolManager(20, disk_mgr);

  std::vector<Column *> columns = {
      new Column("seq", TypeId::kTypeInt, 0, false, false),
  };
  auto schema = std::make_shared<Schema>(columns);
  auto *table = TableHeap::Create(bpm, schema.get(), nullptr, nullptr, nullptr);

  const int num_rows = 100;
  for (int i = 0; i < num_rows; i++) {
    Fields f = {Field(TypeId::kTypeInt, i)};
    Row r(f);
    ASSERT_TRUE(table->InsertTuple(r, nullptr));
  }

  // 用迭代器遍历，验证数量
  int count = 0;
  for (auto iter = table->Begin(nullptr); iter != table->End(); iter++) {
    const Row &row = *iter;
    // 堆表不保证顺序，只统计数量
    ASSERT_FALSE(row.GetField(0)->IsNull());
    count++;
  }
  ASSERT_EQ(num_rows, count);

  disk_mgr->Close();
  remove(db_file_name.c_str());
  delete table;
  delete bpm;
  delete disk_mgr;
}

TEST(TableHeapStudentTest, EmptyTableBeginEqualsEnd) {
  remove(db_file_name.c_str());
  auto disk_mgr = new DiskManager(db_file_name);
  auto bpm = new BufferPoolManager(10, disk_mgr);

  std::vector<Column *> columns = {
      new Column("id", TypeId::kTypeInt, 0, false, false),
  };
  auto schema = std::make_shared<Schema>(columns);
  auto *table = TableHeap::Create(bpm, schema.get(), nullptr, nullptr, nullptr);

  // 空表的 Begin 应等于 End
  auto begin = table->Begin(nullptr);
  auto end = table->End();
  ASSERT_TRUE(begin == end);

  disk_mgr->Close();
  remove(db_file_name.c_str());
  delete table;
  delete bpm;
  delete disk_mgr;
}

TEST(TableHeapStudentTest, IteratorSkipsDeletedRows) {
  remove(db_file_name.c_str());
  auto disk_mgr = new DiskManager(db_file_name);
  auto bpm = new BufferPoolManager(20, disk_mgr);

  std::vector<Column *> columns = {
      new Column("val", TypeId::kTypeInt, 0, false, false),
  };
  auto schema = std::make_shared<Schema>(columns);
  auto *table = TableHeap::Create(bpm, schema.get(), nullptr, nullptr, nullptr);

  // 插入 5 条，记录 RowId
  std::vector<RowId> rids;
  for (int i = 0; i < 5; i++) {
    Fields f = {Field(TypeId::kTypeInt, i)};
    Row r(f);
    ASSERT_TRUE(table->InsertTuple(r, nullptr));
    rids.push_back(r.GetRowId());
  }

  // 删除第 2 和第 4 条（下标 1 和 3）
  ASSERT_TRUE(table->MarkDelete(rids[1], nullptr));
  table->ApplyDelete(rids[1], nullptr);
  ASSERT_TRUE(table->MarkDelete(rids[3], nullptr));
  table->ApplyDelete(rids[3], nullptr);

  // 迭代器应只看到剩余的 3 条
  int count = 0;
  for (auto iter = table->Begin(nullptr); iter != table->End(); iter++) {
    count++;
  }
  ASSERT_EQ(3, count);

  // 已删除的记录读不到
  ASSERT_FALSE(table->GetTuple(new Row(rids[1]), nullptr));
  ASSERT_FALSE(table->GetTuple(new Row(rids[3]), nullptr));

  disk_mgr->Close();
  remove(db_file_name.c_str());
  delete table;
  delete bpm;
  delete disk_mgr;
}

// =============================================================================
// 错误路径与边界条件
// =============================================================================

TEST(TableHeapStudentTest, InsertTooLargeRowReturnsFalse) {
  remove(db_file_name.c_str());
  auto disk_mgr = new DiskManager(db_file_name);
  auto bpm = new BufferPoolManager(10, disk_mgr);

  // 两个 char(2047) 列，每条 4+2047=2051 字节 ×2，加 header ~5 字节 ≈ 4107 > SIZE_MAX_ROW(4064)
  std::vector<Column *> columns = {
      new Column("a", TypeId::kTypeChar, 2047, 0, false, false),
      new Column("b", TypeId::kTypeChar, 2047, 1, false, false),
  };
  auto schema = std::make_shared<Schema>(columns);
  auto *table = TableHeap::Create(bpm, schema.get(), nullptr, nullptr, nullptr);

  char big[2047];
  memset(big, 'X', 2046);
  big[2046] = '\0';
  Fields f = {
      Field(TypeId::kTypeChar, big, 2047, true),
      Field(TypeId::kTypeChar, big, 2047, true),
  };
  Row r(f);
  ASSERT_FALSE(table->InsertTuple(r, nullptr));  // 超过 SIZE_MAX_ROW，拒绝

  disk_mgr->Close();
  remove(db_file_name.c_str());
  delete table;
  delete bpm;
  delete disk_mgr;
}

TEST(TableHeapStudentTest, GetTupleNonExistentPageReturnsFalse) {
  remove(db_file_name.c_str());
  auto disk_mgr = new DiskManager(db_file_name);
  auto bpm = new BufferPoolManager(10, disk_mgr);

  std::vector<Column *> columns = {
      new Column("id", TypeId::kTypeInt, 0, false, false),
  };
  auto schema = std::make_shared<Schema>(columns);
  auto *table = TableHeap::Create(bpm, schema.get(), nullptr, nullptr, nullptr);

  // 使用一个不存在的 page_id（远大于实际分配的页号）
  Row row(RowId(99999, 0));
  ASSERT_FALSE(table->GetTuple(&row, nullptr));

  disk_mgr->Close();
  remove(db_file_name.c_str());
  delete table;
  delete bpm;
  delete disk_mgr;
}

TEST(TableHeapStudentTest, UpdateTupleNonExistentRowReturnsFalse) {
  remove(db_file_name.c_str());
  auto disk_mgr = new DiskManager(db_file_name);
  auto bpm = new BufferPoolManager(10, disk_mgr);

  std::vector<Column *> columns = {
      new Column("val", TypeId::kTypeInt, 0, false, false),
  };
  auto schema = std::make_shared<Schema>(columns);
  auto *table = TableHeap::Create(bpm, schema.get(), nullptr, nullptr, nullptr);

  Fields f = {Field(TypeId::kTypeInt, 42)};
  Row row(f);
  // 使用不存在的 RowId
  ASSERT_FALSE(table->UpdateTuple(row, RowId(99999, 0), nullptr));

  disk_mgr->Close();
  remove(db_file_name.c_str());
  delete table;
  delete bpm;
  delete disk_mgr;
}

TEST(TableHeapStudentTest, IteratorPostfixIncrement) {
  remove(db_file_name.c_str());
  auto disk_mgr = new DiskManager(db_file_name);
  auto bpm = new BufferPoolManager(10, disk_mgr);

  std::vector<Column *> columns = {
      new Column("v", TypeId::kTypeInt, 0, false, false),
  };
  auto schema = std::make_shared<Schema>(columns);
  auto *table = TableHeap::Create(bpm, schema.get(), nullptr, nullptr, nullptr);

  for (int i = 0; i < 3; i++) {
    Fields f = {Field(TypeId::kTypeInt, i)};
    Row r(f);
    table->InsertTuple(r, nullptr);
  }

  // iter++：返回旧值，自身前进
  auto it = table->Begin(nullptr);
  auto old = it++;
  ASSERT_FALSE(old == it);  // 前进前后应不同

  disk_mgr->Close();
  remove(db_file_name.c_str());
  delete table;
  delete bpm;
  delete disk_mgr;
}
