#include "index/b_plus_tree.h"
#include "common/instance.h"
#include "gtest/gtest.h"
#include "index/generic_key.h"
#include "utils/utils.h"

// 完全模拟课测流程
TEST(BPlusTreeStudentTest, CourseSimulation) {
  const std::string db = "bp_course_sim.db";
  for (int trial = 0; trial < 20; trial++) {
    remove(db.c_str());
    DBStorageEngine engine(db);
    std::vector<Column *> c = {new Column("int", TypeId::kTypeInt, 0, false, false)};
    Schema *s = new Schema(c);
    KeyManager KP(s, 17);
    BPlusTree tree(0, engine.bpm_, KP);
    const int n = 2000;

    std::vector<GenericKey *> keys, del_seq;
    std::vector<RowId> values;
    for (int i = 0; i < n; i++) {
      GenericKey *k = KP.InitKey();
      std::vector<Field> f = {Field(TypeId::kTypeInt, i)};
      KP.SerializeFromKey(k, Row(f), s);
      keys.push_back(k);
      values.push_back(RowId(i));
      del_seq.push_back(k);
    }
    ShuffleArray(keys);
    ShuffleArray(values);
    ShuffleArray(del_seq);

    for (int i = 0; i < n; i++)
      ASSERT_TRUE(tree.Insert(keys[i], values[i]));
    ASSERT_TRUE(tree.Check()) << "Check after insert, trial " << trial;

    for (int i = 0; i < n/2; i++)
      tree.Remove(del_seq[i]);
    ASSERT_TRUE(tree.Check()) << "Check after remove, trial " << trial;

    for (int i = 0; i < n/2; i++) {
      std::vector<RowId> r;
      ASSERT_FALSE(tree.GetValue(del_seq[i], r)) << "Deleted key found, trial " << trial;
    }
    for (int i = n/2; i < n; i++) {
      std::vector<RowId> r;
      ASSERT_TRUE(tree.GetValue(del_seq[i], r)) << "Non-deleted key missing, trial " << trial;
    }
    for (auto k : keys) free(k);
    delete s;
  }
  remove(db.c_str());
}

// 空树操作：Begin==End，Remove 不崩溃
TEST(BPlusTreeStudentTest, EmptyTreeOperations) {
  const std::string db = "bp_empty.db";
  remove(db.c_str());
  DBStorageEngine engine(db);
  std::vector<Column *> c = {new Column("v", TypeId::kTypeInt, 0, false, false)};
  Schema *s = new Schema(c);
  KeyManager KP(s, 17);
  BPlusTree tree(0, engine.bpm_, KP);

  ASSERT_TRUE(tree.IsEmpty());
  ASSERT_TRUE(tree.Begin() == tree.End());
  // 对空树 Remove 不应崩溃
  GenericKey *k = KP.InitKey();
  std::vector<Field> f = {Field(TypeId::kTypeInt, 42)};
  KP.SerializeFromKey(k, Row(f), s);
  tree.Remove(k);  // 应当直接 return
  ASSERT_TRUE(tree.IsEmpty());
  free(k);
  delete s;
  remove(db.c_str());
}

// 插入大量数据后全部删除，验证树正确地变空
TEST(BPlusTreeStudentTest, InsertAllThenDeleteAll) {
  const std::string db = "bp_alldel.db";
  remove(db.c_str());
  DBStorageEngine engine(db);
  std::vector<Column *> c = {new Column("v", TypeId::kTypeInt, 0, false, false)};
  Schema *s = new Schema(c);
  KeyManager KP(s, 17);
  BPlusTree tree(0, engine.bpm_, KP);
  const int n = 500;

  for (int i = 0; i < n; i++) {
    GenericKey *k = KP.InitKey();
    std::vector<Field> f = {Field(TypeId::kTypeInt, i)};
    KP.SerializeFromKey(k, Row(f), s);
    ASSERT_TRUE(tree.Insert(k, RowId(i)));
    free(k);
  }
  ASSERT_TRUE(tree.Check());
  for (int i = 0; i < n; i++) {
    GenericKey *k = KP.InitKey();
    std::vector<Field> f = {Field(TypeId::kTypeInt, i)};
    KP.SerializeFromKey(k, Row(f), s);
    tree.Remove(k);
    ASSERT_TRUE(tree.Check()) << "Check after remove " << i;
    free(k);
  }
  ASSERT_TRUE(tree.IsEmpty());
  ASSERT_TRUE(tree.Begin() == tree.End());

  delete s;
  remove(db.c_str());
}
