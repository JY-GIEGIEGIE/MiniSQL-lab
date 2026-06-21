#include "catalog/catalog.h"
#include "common/instance.h"
#include "gtest/gtest.h"

static string db_name = "cat_student_test.db";

// 错误路径：表不存在、索引不存在、重复创建
TEST(CatalogStudentTest, ErrorPaths) {
  remove(db_name.c_str());
  auto db = new DBStorageEngine(db_name, true);
  auto &mgr = db->catalog_mgr_;

  // 获取不存在的表
  TableInfo *ti = nullptr;
  ASSERT_EQ(DB_TABLE_NOT_EXIST, mgr->GetTable("no_such_table", ti));

  // 获取不存在表的索引
  IndexInfo *ii = nullptr;
  ASSERT_EQ(DB_TABLE_NOT_EXIST, mgr->GetIndex("no_such_table", "no_idx", ii));

  // 在不存在的表上建索引
  std::vector<std::string> keys{"id"};
  Txn txn;
  ASSERT_EQ(DB_TABLE_NOT_EXIST, mgr->CreateIndex("no_such_table", "idx", keys, &txn, ii, "bptree"));

  // 建表，然后重复建同名表
  std::vector<Column *> cols = {new Column("id", TypeId::kTypeInt, 0, false, false)};
  auto schema = std::make_shared<Schema>(cols);
  ASSERT_EQ(DB_SUCCESS, mgr->CreateTable("t1", schema.get(), &txn, ti));
  ASSERT_NE(nullptr, ti);
  TableInfo *ti2 = nullptr;
  ASSERT_EQ(DB_TABLE_ALREADY_EXIST, mgr->CreateTable("t1", schema.get(), &txn, ti2));

  // 建索引，然后重复建同名索引
  ASSERT_EQ(DB_SUCCESS, mgr->CreateIndex("t1", "idx1", keys, &txn, ii, "bptree"));
  ASSERT_NE(nullptr, ii);
  IndexInfo *ii2 = nullptr;
  ASSERT_EQ(DB_INDEX_ALREADY_EXIST, mgr->CreateIndex("t1", "idx1", keys, &txn, ii2, "bptree"));

  // 用不存在的列建索引
  std::vector<std::string> bad_keys{"id", "no_col"};
  ASSERT_EQ(DB_COLUMN_NAME_NOT_EXIST, mgr->CreateIndex("t1", "idx2", bad_keys, &txn, ii2, "bptree"));

  // 获取不存在的索引
  ASSERT_EQ(DB_INDEX_NOT_FOUND, mgr->GetIndex("t1", "no_idx", ii2));

  delete db;
  remove(db_name.c_str());
}

// DropTable 级联删索引
TEST(CatalogStudentTest, DropTableCascadesIndexes) {
  remove(db_name.c_str());
  auto db = new DBStorageEngine(db_name, true);
  auto &mgr = db->catalog_mgr_;
  Txn txn;
  TableInfo *ti = nullptr;

  std::vector<Column *> cols = {new Column("id", TypeId::kTypeInt, 0, false, false),
                                new Column("name", TypeId::kTypeChar, 64, 1, false, false)};
  auto schema = std::make_shared<Schema>(cols);
  ASSERT_EQ(DB_SUCCESS, mgr->CreateTable("t", schema.get(), &txn, ti));

  std::vector<std::string> keys1{"id"};
  std::vector<std::string> keys2{"name"};
  IndexInfo *ii1 = nullptr, *ii2 = nullptr;
  ASSERT_EQ(DB_SUCCESS, mgr->CreateIndex("t", "i1", keys1, &txn, ii1, "bptree"));
  ASSERT_EQ(DB_SUCCESS, mgr->CreateIndex("t", "i2", keys2, &txn, ii2, "bptree"));

  // 删表
  ASSERT_EQ(DB_SUCCESS, mgr->DropTable("t"));

  // 验证表已不存在
  ASSERT_EQ(DB_TABLE_NOT_EXIST, mgr->GetTable("t", ti));

  // 验证索引也已级联删除
  IndexInfo *dummy = nullptr;
  ASSERT_EQ(DB_TABLE_NOT_EXIST, mgr->GetIndex("t", "i1", dummy));

  delete db;
  remove(db_name.c_str());
}

// GetTableIndexes 和 GetTables
TEST(CatalogStudentTest, GetTablesAndIndexes) {
  remove(db_name.c_str());
  auto db = new DBStorageEngine(db_name, true);
  auto &mgr = db->catalog_mgr_;
  Txn txn;
  TableInfo *t1 = nullptr, *t2 = nullptr;

  std::vector<Column *> cols = {new Column("id", TypeId::kTypeInt, 0, false, false)};
  auto s1 = std::make_shared<Schema>(cols);
  auto s2 = std::make_shared<Schema>(
      std::vector<Column *>{new Column("x", TypeId::kTypeFloat, 0, false, false)});

  ASSERT_EQ(DB_SUCCESS, mgr->CreateTable("ta", s1.get(), &txn, t1));
  ASSERT_EQ(DB_SUCCESS, mgr->CreateTable("tb", s2.get(), &txn, t2));

  IndexInfo *ii = nullptr;
  std::vector<std::string> keys{"id"};
  ASSERT_EQ(DB_SUCCESS, mgr->CreateIndex("ta", "idx_a", keys, &txn, ii, "bptree"));

  // GetTables
  std::vector<TableInfo *> tables;
  ASSERT_EQ(DB_SUCCESS, mgr->GetTables(tables));
  ASSERT_EQ(2, tables.size());

  // GetTableIndexes
  std::vector<IndexInfo *> indexes;
  ASSERT_EQ(DB_SUCCESS, mgr->GetTableIndexes("ta", indexes));
  ASSERT_EQ(1, indexes.size());

  // tb has no indexes
  indexes.clear();
  ASSERT_EQ(DB_SUCCESS, mgr->GetTableIndexes("tb", indexes));
  ASSERT_EQ(0, indexes.size());

  delete db;
  remove(db_name.c_str());
}
