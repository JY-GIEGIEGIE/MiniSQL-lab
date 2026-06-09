#include "record/column.h"

#include "glog/logging.h"

Column::Column(std::string column_name, TypeId type, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)), type_(type), table_ind_(index), nullable_(nullable), unique_(unique) {
  ASSERT(type != TypeId::kTypeChar, "Wrong constructor for CHAR type.");
  switch (type) {
    case TypeId::kTypeInt:
      len_ = sizeof(int32_t);
      break;
    case TypeId::kTypeFloat:
      len_ = sizeof(float_t);
      break;
    default:
      ASSERT(false, "Unsupported column type.");
  }
}

Column::Column(std::string column_name, TypeId type, uint32_t length, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)),
      type_(type),
      len_(length),
      table_ind_(index),
      nullable_(nullable),
      unique_(unique) {
  ASSERT(type == TypeId::kTypeChar, "Wrong constructor for non-VARCHAR type.");
}

Column::Column(const Column *other)
    : name_(other->name_),
      type_(other->type_),
      len_(other->len_),
      table_ind_(other->table_ind_),
      nullable_(other->nullable_),
      unique_(other->unique_) {}

/**
* TODO: Student Implement
*/
uint32_t Column::SerializeTo(char *buf) const {
  // write magic number for sanity check
  *reinterpret_cast<uint32_t *>(buf) = COLUMN_MAGIC_NUM;
  buf += sizeof(uint32_t);
  // serialize other member variables
  *reinterpret_cast<uint32_t *>(buf) = name_.size();
  buf += sizeof(uint32_t);
  std::memcpy(buf, name_.data(), name_.size());
  buf += name_.size();
  *reinterpret_cast<uint32_t *>(buf) = static_cast<uint32_t>(type_);
  buf += sizeof(uint32_t);
  *reinterpret_cast<uint32_t *>(buf) = len_;
  buf += sizeof(uint32_t);
  *reinterpret_cast<uint32_t *>(buf) = table_ind_;
  buf += sizeof(uint32_t);
  *reinterpret_cast<uint32_t *>(buf) = static_cast<uint32_t>(nullable_);
  buf += sizeof(uint32_t);
  *reinterpret_cast<uint32_t *>(buf) = static_cast<uint32_t>(unique_);
  return GetSerializedSize();
}

/**
 * TODO: Student Implement
 */
uint32_t Column::GetSerializedSize() const {
  // replace with your code here
  return sizeof(uint32_t) +  // magic number
         sizeof(uint32_t) + name_.size() +  // name
         sizeof(uint32_t) +  // type
         sizeof(uint32_t) +  // length
         sizeof(uint32_t) +  // table index
         sizeof(uint32_t) +  // nullable
         sizeof(uint32_t);   // unique
}

/**
 * TODO: Student Implement
 */
uint32_t Column::DeserializeFrom(char *buf, Column *&column) {
  // sanity check, pointer first 4 bytes should be the magic number
  uint32_t magic_num = *reinterpret_cast<uint32_t *>(buf);
  if (magic_num != COLUMN_MAGIC_NUM) {
    return 0;
  }
  buf += sizeof(uint32_t);
  // deserialize other member variables
  uint32_t name_size = *reinterpret_cast<uint32_t *>(buf);
  buf += sizeof(uint32_t);
  std::string name(buf, name_size);
  buf += name_size;
  TypeId type = static_cast<TypeId>(*reinterpret_cast<uint32_t *>(buf));
  buf += sizeof(uint32_t);
  uint32_t len = *reinterpret_cast<uint32_t *>(buf);
  buf += sizeof(uint32_t);
  uint32_t table_ind = *reinterpret_cast<uint32_t *>(buf);
  buf += sizeof(uint32_t);
  bool nullable = static_cast<bool>(*reinterpret_cast<uint32_t *>(buf));
  buf += sizeof(uint32_t);
  bool unique = static_cast<bool>(*reinterpret_cast<uint32_t *>(buf));
  if (type == TypeId::kTypeChar) {
    column = new Column(name, type, len, table_ind, nullable, unique);
  } else {
    // for non-char type, len is determined by type and should be ignored in constructor
    column = new Column(name, type, table_ind, nullable, unique);
  }
  return column->GetSerializedSize();
}
