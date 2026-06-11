#include <cstring>

#include "common/instance.h"
#include "gtest/gtest.h"
#include "page/table_page.h"
#include "record/field.h"
#include "record/row.h"
#include "record/schema.h"

// =============================================================================
// Column 独立序列化 roundtrip 测试
// =============================================================================

TEST(ColumnStudentTest, IntColumnRoundTrip) {
  Column src("age", TypeId::kTypeInt, 0, false, false);
  char buf[256];
  uint32_t written = src.SerializeTo(buf);
  ASSERT_EQ(written, src.GetSerializedSize());

  Column *dst = nullptr;
  uint32_t read = Column::DeserializeFrom(buf, dst);
  ASSERT_NE(nullptr, dst);
  ASSERT_EQ(written, read);
  ASSERT_EQ(src.GetName(), dst->GetName());
  ASSERT_EQ(src.GetType(), dst->GetType());
  ASSERT_EQ(src.GetLength(), dst->GetLength());
  ASSERT_EQ(src.IsNullable(), dst->IsNullable());
  ASSERT_EQ(src.IsUnique(), dst->IsUnique());
  delete dst;
}

TEST(ColumnStudentTest, CharColumnRoundTrip) {
  Column src("name", TypeId::kTypeChar, 64, 1, true, false);
  char buf[256];
  src.SerializeTo(buf);
  Column *dst = nullptr;
  Column::DeserializeFrom(buf, dst);
  ASSERT_NE(nullptr, dst);
  ASSERT_EQ(src.GetName(), dst->GetName());
  ASSERT_EQ(src.GetType(), dst->GetType());
  ASSERT_EQ(src.GetLength(), dst->GetLength());
  ASSERT_EQ(true, dst->IsNullable());
  ASSERT_EQ(false, dst->IsUnique());
  delete dst;
}

TEST(ColumnStudentTest, FloatUniqueColumnRoundTrip) {
  Column src("score", TypeId::kTypeFloat, 2, false, true);
  char buf[256];
  src.SerializeTo(buf);
  Column *dst = nullptr;
  Column::DeserializeFrom(buf, dst);
  ASSERT_NE(nullptr, dst);
  ASSERT_EQ(true, dst->IsUnique());
  delete dst;
}

TEST(ColumnStudentTest, WrongMagicNumReturnsZero) {
  char buf[256];
  memset(buf, 0xFF, 256);  // 全是 0xFF，MAGIC_NUM 肯定不匹配
  Column *dst = reinterpret_cast<Column *>(0xDEAD);
  uint32_t read = Column::DeserializeFrom(buf, dst);
  ASSERT_EQ(0, read);  // 不消耗任何字节
}

// =============================================================================
// Schema 独立序列化 roundtrip 测试
// =============================================================================

TEST(SchemaStudentTest, MultiColumnSchemaRoundTrip) {
  std::vector<Column *> columns = {
      new Column("id", TypeId::kTypeInt, 0, false, false),
      new Column("name", TypeId::kTypeChar, 64, 1, true, false),
      new Column("balance", TypeId::kTypeFloat, 2, true, false),
  };
  Schema schema(columns, true);

  char buf[1024];
  uint32_t written = schema.SerializeTo(buf);
  ASSERT_EQ(written, schema.GetSerializedSize());
  ASSERT_GT(written, 0);

  Schema *restored = nullptr;
  uint32_t read = Schema::DeserializeFrom(buf, restored);
  ASSERT_NE(nullptr, restored);
  ASSERT_EQ(written, read);
  ASSERT_EQ(3, restored->GetColumnCount());
  ASSERT_EQ("id", restored->GetColumn(0)->GetName());
  ASSERT_EQ(TypeId::kTypeInt, restored->GetColumn(0)->GetType());
  ASSERT_EQ("name", restored->GetColumn(1)->GetName());
  ASSERT_EQ(TypeId::kTypeChar, restored->GetColumn(1)->GetType());
  ASSERT_EQ(64, restored->GetColumn(1)->GetLength());
  ASSERT_EQ("balance", restored->GetColumn(2)->GetName());
  ASSERT_EQ(TypeId::kTypeFloat, restored->GetColumn(2)->GetType());
  delete restored;
}

// =============================================================================
// Row 序列化边界条件测试
// =============================================================================

TEST(RowStudentTest, AllNullFieldsRoundTrip) {
  std::vector<Column *> columns = {
      new Column("a", TypeId::kTypeInt, 0, true, false),
      new Column("b", TypeId::kTypeFloat, 1, true, false),
      new Column("c", TypeId::kTypeChar, 32, 2, true, false),
  };
  Schema schema(columns, true);

  std::vector<Field> fields = {Field(TypeId::kTypeInt), Field(TypeId::kTypeFloat),
                               Field(TypeId::kTypeChar)};
  Row row(fields);
  // 验证所有字段都是 null
  ASSERT_TRUE(row.GetField(0)->IsNull());
  ASSERT_TRUE(row.GetField(1)->IsNull());
  ASSERT_TRUE(row.GetField(2)->IsNull());

  // 序列化+反序列化
  char buf[256];
  uint32_t written = row.SerializeTo(buf, &schema);
  ASSERT_EQ(written, row.GetSerializedSize(&schema));
  // 全 null 行只有 header，body 为 0 字节
  ASSERT_GT(written, 0);
  ASSERT_LT(written, 64);  // 应该远小于有数据的行

  Row restored;
  restored.DeserializeFrom(buf, &schema);
  ASSERT_EQ(3, restored.GetFieldCount());
  ASSERT_TRUE(restored.GetField(0)->IsNull());
  ASSERT_TRUE(restored.GetField(1)->IsNull());
  ASSERT_TRUE(restored.GetField(2)->IsNull());
}

TEST(RowStudentTest, MixNullAndNonNullRoundTrip) {
  std::vector<Column *> columns = {
      new Column("id", TypeId::kTypeInt, 0, false, false),
      new Column("name", TypeId::kTypeChar, 64, 1, true, false),
      new Column("score", TypeId::kTypeFloat, 2, true, false),
  };
  Schema schema(columns, true);

  std::vector<Field> fields = {
      Field(TypeId::kTypeInt, 42),    // id = 42 (not null)
      Field(TypeId::kTypeChar),       // name = null
      Field(TypeId::kTypeFloat, 3.14f),  // score = 3.14 (not null)
  };
  Row row(fields);
  ASSERT_FALSE(row.GetField(0)->IsNull());
  ASSERT_TRUE(row.GetField(1)->IsNull());
  ASSERT_FALSE(row.GetField(2)->IsNull());

  char buf[256];
  row.SerializeTo(buf, &schema);

  Row restored;
  restored.DeserializeFrom(buf, &schema);
  ASSERT_EQ(3, restored.GetFieldCount());
  ASSERT_EQ(CmpBool::kTrue, restored.GetField(0)->CompareEquals(fields[0]));
  ASSERT_TRUE(restored.GetField(1)->IsNull());
  ASSERT_EQ(CmpBool::kTrue, restored.GetField(2)->CompareEquals(fields[2]));
}

TEST(RowStudentTest, EmptyCharFieldRoundTrip) {
  std::vector<Column *> columns = {
      new Column("text", TypeId::kTypeChar, 256, 0, true, false),
  };
  Schema schema(columns, true);

  // 空字符串（长度 0）
  std::vector<Field> fields = {
      Field(TypeId::kTypeChar, const_cast<char *>(""), 0, false),
  };
  Row row(fields);
  ASSERT_EQ(0, row.GetField(0)->GetLength());

  char buf[256];
  row.SerializeTo(buf, &schema);

  Row restored;
  restored.DeserializeFrom(buf, &schema);
  ASSERT_EQ(1, restored.GetFieldCount());
  ASSERT_EQ(0, restored.GetField(0)->GetLength());
  ASSERT_FALSE(restored.GetField(0)->IsNull());
}

TEST(RowStudentTest, GetSerializedSizeMatchesSerializeTo) {
  std::vector<Column *> columns = {
      new Column("a", TypeId::kTypeInt, 0, false, false),
      new Column("b", TypeId::kTypeChar, 128, 1, true, false),
  };
  Schema schema(columns, true);

  char data[] = "hello world";
  std::vector<Field> fields = {
      Field(TypeId::kTypeInt, 999),
      Field(TypeId::kTypeChar, data, strlen(data), true),
  };
  Row row(fields);

  char dummy[256];
  ASSERT_EQ(row.GetSerializedSize(&schema), row.SerializeTo(dummy, &schema));
}
