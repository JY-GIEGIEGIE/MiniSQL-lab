#include "catalog/catalog.h"

void CatalogMeta::SerializeTo(char *buf) const {
  ASSERT(GetSerializedSize() <= PAGE_SIZE, "Failed to serialize catalog metadata to disk.");
  MACH_WRITE_UINT32(buf, CATALOG_METADATA_MAGIC_NUM);
  buf += 4;
  MACH_WRITE_UINT32(buf, table_meta_pages_.size());
  buf += 4;
  MACH_WRITE_UINT32(buf, index_meta_pages_.size());
  buf += 4;
  for (auto iter : table_meta_pages_) {
    MACH_WRITE_TO(table_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
  for (auto iter : index_meta_pages_) {
    MACH_WRITE_TO(index_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
}

CatalogMeta *CatalogMeta::DeserializeFrom(char *buf) {
  uint32_t magic_num = MACH_READ_UINT32(buf);
  buf += 4;
  ASSERT(magic_num == CATALOG_METADATA_MAGIC_NUM, "Failed to deserialize catalog metadata from disk.");
  uint32_t table_nums = MACH_READ_UINT32(buf);
  buf += 4;
  uint32_t index_nums = MACH_READ_UINT32(buf);
  buf += 4;
  CatalogMeta *meta = new CatalogMeta();
  for (uint32_t i = 0; i < table_nums; i++) {
    auto table_id = MACH_READ_FROM(table_id_t, buf);
    buf += 4;
    auto table_heap_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->table_meta_pages_.emplace(table_id, table_heap_page_id);
  }
  for (uint32_t i = 0; i < index_nums; i++) {
    auto index_id = MACH_READ_FROM(index_id_t, buf);
    buf += 4;
    auto index_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->index_meta_pages_.emplace(index_id, index_page_id);
  }
  return meta;
}

uint32_t CatalogMeta::GetSerializedSize() const {
  return 4                           // MAGIC_NUM
       + 4                           // table count
       + 4                           // index count
       + table_meta_pages_.size() * 8  // (table_id 4B + page_id 4B) each
       + index_meta_pages_.size() * 8; // (index_id 4B + page_id 4B) each
}

CatalogMeta::CatalogMeta() {}

// ============================================================================

CatalogManager::CatalogManager(BufferPoolManager *buffer_pool_manager, LockManager *lock_manager,
                               LogManager *log_manager, bool init)
    : buffer_pool_manager_(buffer_pool_manager), lock_manager_(lock_manager), log_manager_(log_manager) {
  if (init) {
    // 首次创建数据库：空 CatalogMeta
    catalog_meta_ = CatalogMeta::NewInstance();
    next_table_id_ = 0;
    next_index_id_ = 0;
    FlushCatalogMetaPage();
  } else {
    // 从已有数据库恢复：读 0 号页 → DeserializeFrom → 逐表/逐索引加载
    auto *page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
    catalog_meta_ = CatalogMeta::DeserializeFrom(page->GetData());
    buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, false);
    next_table_id_ = catalog_meta_->GetNextTableId();
    next_index_id_ = catalog_meta_->GetNextIndexId();
    for (auto &p : *catalog_meta_->GetTableMetaPages())
      LoadTable(p.first, p.second);
    for (auto &p : *catalog_meta_->GetIndexMetaPages())
      LoadIndex(p.first, p.second);
  }
}

CatalogManager::~CatalogManager() {
  FlushCatalogMetaPage();
  delete catalog_meta_;
  for (auto &iter : tables_) delete iter.second;
  for (auto &iter : indexes_) delete iter.second;
}

// ==================== Flush / Load ====================

dberr_t CatalogManager::FlushCatalogMetaPage() const {
  auto *page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  if (page == nullptr) return DB_FAILED;
  catalog_meta_->SerializeTo(page->GetData());
  buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, true);
  return DB_SUCCESS;
}

dberr_t CatalogManager::LoadTable(const table_id_t table_id, const page_id_t page_id) {
  auto *page = buffer_pool_manager_->FetchPage(page_id);
  if (page == nullptr) return DB_FAILED;
  TableMetadata *meta = nullptr;
  TableMetadata::DeserializeFrom(page->GetData(), meta);
  buffer_pool_manager_->UnpinPage(page_id, false);
  // 用恢复出的 root_page_id 打开已有 TableHeap
  auto *table_heap = TableHeap::Create(buffer_pool_manager_, meta->GetFirstPageId(),
                                       meta->GetSchema(), log_manager_, lock_manager_);
  auto *info = TableInfo::Create();
  info->Init(meta, table_heap);
  tables_[table_id] = info;
  table_names_[meta->GetTableName()] = table_id;
  return DB_SUCCESS;
}

dberr_t CatalogManager::LoadIndex(const index_id_t index_id, const page_id_t page_id) {
  auto *page = buffer_pool_manager_->FetchPage(page_id);
  if (page == nullptr) return DB_FAILED;
  IndexMetadata *meta = nullptr;
  IndexMetadata::DeserializeFrom(page->GetData(), meta);
  buffer_pool_manager_->UnpinPage(page_id, false);
  // 找到所属 TableInfo（IndexMetadata 存有 table_id，需要在内存里找到对应的表）
  TableInfo *table_info = nullptr;
  if (GetTable(meta->GetTableId(), table_info) != DB_SUCCESS) return DB_FAILED;
  auto *info = IndexInfo::Create();
  info->Init(meta, table_info, buffer_pool_manager_);
  indexes_[index_id] = info;
  index_names_[table_info->GetTableName()][meta->GetIndexName()] = index_id;
  return DB_SUCCESS;
}

// ==================== Table operations ====================

dberr_t CatalogManager::CreateTable(const string &table_name, TableSchema *schema, Txn *txn,
                                    TableInfo *&table_info) {
  if (table_names_.find(table_name) != table_names_.end())
    return DB_TABLE_ALREADY_EXIST;

  table_id_t table_id = catalog_meta_->GetNextTableId();
  // 深拷贝 Schema：TableMetadata 需要持有独立的副本，防止外部释放导致悬空指针
  TableSchema *copied_schema = Schema::DeepCopySchema(schema);

  // 1. 创建 TableHeap（NewPage 分配第一页）
  auto *table_heap = TableHeap::Create(buffer_pool_manager_, copied_schema, txn,
                                       log_manager_, lock_manager_);
  page_id_t root_page_id = table_heap->GetFirstPageId();

  // 2. 创建 TableMetadata 并存入新页
  auto *meta = TableMetadata::Create(table_id, table_name, root_page_id, copied_schema);
  page_id_t meta_page_id;
  auto *meta_page = buffer_pool_manager_->NewPage(meta_page_id);
  if (meta_page == nullptr) return DB_FAILED;
  meta->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(meta_page_id, true);

  // 3. 组装 TableInfo 并登记
  table_info = TableInfo::Create();
  table_info->Init(meta, table_heap);
  tables_[table_id] = table_info;
  table_names_[table_name] = table_id;
  catalog_meta_->table_meta_pages_[table_id] = meta_page_id;
  next_table_id_ = table_id + 1;
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

dberr_t CatalogManager::GetTable(const string &table_name, TableInfo *&table_info) {
  auto it = table_names_.find(table_name);
  if (it == table_names_.end()) return DB_TABLE_NOT_EXIST;
  return GetTable(it->second, table_info);
}

dberr_t CatalogManager::GetTable(const table_id_t table_id, TableInfo *&table_info) {
  auto it = tables_.find(table_id);
  if (it == tables_.end()) return DB_TABLE_NOT_EXIST;
  table_info = it->second;
  return DB_SUCCESS;
}

dberr_t CatalogManager::GetTables(vector<TableInfo *> &tables) const {
  for (auto &p : tables_) tables.push_back(p.second);
  return DB_SUCCESS;
}

dberr_t CatalogManager::DropTable(const string &table_name) {
  auto it = table_names_.find(table_name);
  if (it == table_names_.end()) return DB_TABLE_NOT_EXIST;
  return DropTable(it->second);
}

dberr_t CatalogManager::DropTable(table_id_t table_id) {
  // 删除该表的所有索引（基于表名从 index_names_ 找）
  auto *info = tables_[table_id];
  std::string tname = info->GetTableName();
  if (index_names_.find(tname) != index_names_.end()) {
    std::vector<std::string> idx_names;
    for (auto &p : index_names_[tname]) idx_names.push_back(p.first);
    for (auto &iname : idx_names) DropIndex(tname, iname);
  }
  // 清除 index_names_ 中的表名条目
  index_names_.erase(tname);
  // 删 TableInfo
  info->GetTableHeap()->DeleteTable();
  buffer_pool_manager_->DeletePage(catalog_meta_->table_meta_pages_[table_id]);
  table_names_.erase(tname);
  tables_.erase(table_id);
  catalog_meta_->table_meta_pages_.erase(table_id);
  delete info;
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

// ==================== Index operations ====================

dberr_t CatalogManager::CreateIndex(const std::string &table_name, const string &index_name,
                                    const std::vector<std::string> &index_keys, Txn *txn,
                                    IndexInfo *&index_info, const string &index_type) {
  // 表必须存在
  TableInfo *table_info = nullptr;
  if (GetTable(table_name, table_info) != DB_SUCCESS) return DB_TABLE_NOT_EXIST;
  auto *table_schema = table_info->GetSchema();
  // 验证 index_keys 中的列名都有效，构建 key_map
  std::vector<uint32_t> key_map;
  for (auto &col_name : index_keys) {
    uint32_t col_idx;
    if (table_schema->GetColumnIndex(col_name, col_idx) != DB_SUCCESS)
      return DB_COLUMN_NAME_NOT_EXIST;
    key_map.push_back(col_idx);
  }
  // 索引名不能重复
  if (index_names_[table_name].find(index_name) != index_names_[table_name].end())
    return DB_INDEX_ALREADY_EXIST;

  index_id_t index_id = catalog_meta_->GetNextIndexId();
  // 创建 IndexMetadata 并存入新页
  auto *meta = IndexMetadata::Create(index_id, index_name, table_info->GetTableId(), key_map);
  page_id_t meta_page_id;
  auto *meta_page = buffer_pool_manager_->NewPage(meta_page_id);
  if (meta_page == nullptr) return DB_FAILED;
  meta->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(meta_page_id, true);

  // 初始化 IndexInfo
  index_info = IndexInfo::Create();
  index_info->Init(meta, table_info, buffer_pool_manager_);
  indexes_[index_id] = index_info;
  index_names_[table_name][index_name] = index_id;
  catalog_meta_->index_meta_pages_[index_id] = meta_page_id;
  next_index_id_ = index_id + 1;
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

dberr_t CatalogManager::GetIndex(const std::string &table_name, const std::string &index_name,
                                 IndexInfo *&index_info) const {
  if (table_names_.find(table_name) == table_names_.end()) return DB_TABLE_NOT_EXIST;
  auto tn = index_names_.find(table_name);
  if (tn == index_names_.end()) return DB_TABLE_NOT_EXIST;
  auto in = tn->second.find(index_name);
  if (in == tn->second.end()) return DB_INDEX_NOT_FOUND;
  auto ii = indexes_.find(in->second);
  if (ii == indexes_.end()) return DB_INDEX_NOT_FOUND;
  index_info = ii->second;
  return DB_SUCCESS;
}

dberr_t CatalogManager::GetTableIndexes(const std::string &table_name,
                                        std::vector<IndexInfo *> &indexes) const {
  if (table_names_.find(table_name) == table_names_.end()) return DB_TABLE_NOT_EXIST;
  auto tn = index_names_.find(table_name);
  if (tn == index_names_.end()) return DB_SUCCESS;  // 无索引
  for (auto &p : tn->second) {
    auto ii = indexes_.find(p.second);
    if (ii != indexes_.end()) indexes.push_back(ii->second);
  }
  return DB_SUCCESS;
}

dberr_t CatalogManager::DropIndex(const string &table_name, const string &index_name) {
  auto tn = index_names_.find(table_name);
  if (tn == index_names_.end()) return DB_TABLE_NOT_EXIST;
  auto in = tn->second.find(index_name);
  if (in == tn->second.end()) return DB_INDEX_NOT_FOUND;
  index_id_t index_id = in->second;

  // 回收 IndexMetadata 的页
  catalog_meta_->DeleteIndexMetaPage(buffer_pool_manager_, index_id);
  // 清除登记
  auto *info = indexes_[index_id];
  tn->second.erase(index_name);
  if (tn->second.empty()) index_names_.erase(table_name);
  indexes_.erase(index_id);
  delete info;
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}
