#include "record/row.h"

uint32_t Row::SerializeTo(char *buf, Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");

  uint32_t column_count = schema->GetColumnCount();
  uint32_t null_bitmap_size = (column_count + 7) / 8;

  // --- Header part 1: field count ---
  *reinterpret_cast<uint32_t *>(buf) = column_count;
  buf += sizeof(uint32_t);

  // --- Header part 2: null bitmap, initialized to all zeros ---
  char *bitmap_start = buf;
  memset(bitmap_start, 0, null_bitmap_size);
  buf += null_bitmap_size;

  // Record where the body (field data) starts
  char *body_start = buf;

  // First pass: mark null bits in the bitmap
  for (uint32_t i = 0; i < column_count; i++) {
    if (fields_[i]->IsNull()) {
      uint32_t byte_idx = i / 8;
      uint32_t bit_idx = i % 8;
      bitmap_start[byte_idx] |= (1 << bit_idx);
    }
  }

  // Second pass: serialize non-null field data into body
  for (uint32_t i = 0; i < column_count; i++) {
    if (!fields_[i]->IsNull()) {
      buf += fields_[i]->SerializeTo(buf);
    }
  }

  // Total bytes written = header size + body size
  return sizeof(uint32_t) + null_bitmap_size + (buf - body_start);
}

uint32_t Row::DeserializeFrom(char *buf, Schema *schema) {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  // 清空已有字段，防止重复反序列化时残留旧数据
  destroy();

  uint32_t column_count = *reinterpret_cast<uint32_t *>(buf);
  buf += sizeof(uint32_t);

  uint32_t null_bitmap_size = (column_count + 7) / 8;
  char *bitmap = buf;
  buf += null_bitmap_size;

  for (uint32_t i = 0; i < column_count; i++) {
    uint32_t byte_idx = i / 8;
    uint32_t bit_idx = i % 8;
    bool is_null = (bitmap[byte_idx] >> bit_idx) & 1;

    TypeId type_id = schema->GetColumn(i)->GetType();
    Field *field = nullptr;
    buf += Field::DeserializeFrom(buf, type_id, &field, is_null);
    fields_.push_back(field);
  }

  return GetSerializedSize(schema);
}

uint32_t Row::GetSerializedSize(Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");

  uint32_t column_count = schema->GetColumnCount();
  uint32_t null_bitmap_size = (column_count + 7) / 8;
  uint32_t size = sizeof(uint32_t) + null_bitmap_size;  // header

  for (uint32_t i = 0; i < column_count; i++) {
    size += fields_[i]->GetSerializedSize();  // null fields return 0, non-null return actual size
  }
  return size;
}

void Row::GetKeyFromRow(const Schema *schema, const Schema *key_schema, Row &key_row) {
  auto columns = key_schema->GetColumns();
  std::vector<Field> fields;
  uint32_t idx;
  for (auto column : columns) {
    schema->GetColumnIndex(column->GetName(), idx);
    fields.emplace_back(*this->GetField(idx));
  }
  key_row = Row(fields);
}
