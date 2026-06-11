#include "record/schema.h"

/**
 * TODO: Student Implement
 */
uint32_t Schema::SerializeTo(char *buf) const {
  // magic number + column count + column 1 + column 2 + ...
  *reinterpret_cast<uint32_t *>(buf) = SCHEMA_MAGIC_NUM;
  buf += sizeof(uint32_t);
  *reinterpret_cast<uint32_t *>(buf) = GetColumnCount();
  buf += sizeof(uint32_t);
  for(int i = 0; i < GetColumnCount(); ++i) {
    buf += GetColumn(i)->SerializeTo(buf);
  }
  return GetSerializedSize();
}

uint32_t Schema::GetSerializedSize() const {
  uint32_t size = 0;
  size += sizeof(uint32_t) * 2; // magic number + column count
  for (int i = 0; i < GetColumnCount(); ++i) {
    size += GetColumn(i)->GetSerializedSize();
  }
  return size;
}

uint32_t Schema::DeserializeFrom(char *buf, Schema *&schema) {
  //check magic number
  if (*reinterpret_cast<uint32_t *>(buf) != SCHEMA_MAGIC_NUM) {
    return 0;
  } else {
    buf += sizeof(uint32_t);
    uint32_t column_count = *reinterpret_cast<uint32_t *>(buf); //after magic number, current pointer at column count
    buf += sizeof(uint32_t);
    std::vector<Column *> columns;
    for (int i = 0; i < column_count; ++i) {
      Column *column;
      uint32_t size = Column::DeserializeFrom(buf, column);
      if (size == 0) {
        return 0;
      }
      columns.push_back(column);
      buf += size;
    }
    schema = new Schema(columns, true);
    return schema->GetSerializedSize();
  }
}