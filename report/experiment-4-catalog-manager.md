# 实验 #4：Catalog Manager 设计报告

## 一、模块概述

### 1.1 模块定位

Catalog Manager 负责管理和维护数据库的所有模式信息（表和索引的元数据），是 MiniSQL 系统中最核心的"元数据中枢"。它向上为 Executor/Planner 提供表与索引的查询接口，向下依赖 BufferPoolManager 完成元数据的持久化存储。每个表和索引的元数据独占一个数据页，CatalogMeta 页（逻辑页 0）记录所有元数据页的分配关系。

### 1.2 Catalog 在 MiniSQL 架构中的位置

```
SQL Parser → Planner & Executor
                │
                ├─ 需要表结构 → CatalogManager::GetTable
                ├─ 需要索引   → CatalogManager::GetIndex
                └─ CREATE/DROP → CatalogManager::CreateTable/DropTable/...
                       │
                       ├─ TableMetadata / IndexMetadata → 序列化存储于数据页
                       ├─ CatalogMeta → 元数据目录，存于逻辑页 0
                       ├─ TableInfo / IndexInfo → 内存中的表和索引对象
                       │     ├─ TableHeap (实验 #2)
                       │     └─ BPlusTreeIndex (实验 #3)
                       └─ BufferPoolManager (实验 #1)
```

### 1.3 子模块划分

| 子模块 | 职责 | 实现状态 |
|--------|------|----------|
| CatalogMeta | 元数据目录：记录 table_id→meta_page、index_id→meta_page 的映射，序列化到 0 号页 | 本实验补全 GetSerializedSize |
| TableMetadata | 单表元数据：表名、表 ID、根页号、列结构（Schema*），存于独立页 | 框架已实现 SerializeTo/DeserializeFrom/Create，本实验补全 GetSerializedSize（已由框架完成） |
| IndexMetadata | 单索引元数据：索引名、索引 ID、所属表 ID、索引列映射（key_map_），存于独立页 | 本实验补全 GetSerializedSize |
| TableInfo | 表的内存表示：聚合 TableMetadata + TableHeap | 框架已实现 |
| IndexInfo | 索引的内存表示：聚合 IndexMetadata + key_schema_ + BPlusTreeIndex | 本实验实现 Init |
| CatalogManager | 元数据管理中枢：Create/Drop/Get 接口 + 持久化与恢复 | 本实验完整实现 |

---

## 二、磁盘存储设计

### 2.1 数据页分配策略

每个表和索引的元数据各自独占一个数据页（PAGE_SIZE=4096 字节）。CatalogMeta 独占逻辑页 0（CATALOG_META_PAGE_ID）。表元数据页的分配：

```
逻辑页 0:  CatalogMeta     ← "目录的目录"
逻辑页 3:  TableMetadata   ← 表"students"的元数据（表名/列结构/堆表根页号）
逻辑页 5:  TableHeap 根页  ← 实际记录从这里开始
逻辑页 7:  TableMetadata   ← 表"courses"的元数据
逻辑页 9:  IndexMetadata   ← 索引"idx_name"的元数据（索引名/索引列映射）
```

### 2.2 CatalogMeta 序列化格式（框架已实现 SerializeTo/DeserializeFrom）

| 偏移 (B) | 大小 (B) | 字段 |
|-----------|----------|------|
| 0 | 4 | MAGIC_NUM (89849) |
| 4 | 4 | table 数量 |
| 8 | 4 | index 数量 |
| 12 | N×8 | N 对 (table_id 4B + page_id 4B) |
| 12+N×8 | M×8 | M 对 (index_id 4B + page_id 4B) |

`GetSerializedSize()` = 12 + (table_meta_pages_.size() + index_meta_pages_.size()) × 8。本实验补全此函数。

### 2.3 TableMetadata 序列化格式（框架已实现）

| 字段 | 大小 |
|------|------|
| MAGIC_NUM (344528) | 4B |
| table_id | 4B |
| table_name (长度前缀 + 数据) | 4B + len |
| root_page_id | 4B |
| schema (Schema::SerializeTo) | 可变 |

`GetSerializedSize()` 框架已实现。

### 2.4 IndexMetadata 序列化格式（框架已实现 SerializeTo/DeserializeFrom）

| 字段 | 大小 |
|------|------|
| MAGIC_NUM (344528) | 4B |
| index_id | 4B |
| index_name (长度前缀 + 数据) | 4B + len |
| table_id | 4B |
| key_map_ size | 4B |
| key_map_ 各元素 | size × 4B |

`GetSerializedSize()` = 20 + index_name_.length() + key_map_.size() × 4。本实验补全此函数。

---

## 三、核心数据结构的内存表示

### 3.1 CatalogMeta

两个 map 构成核心状态：

- `table_meta_pages_`：`std::map<table_id_t, page_id_t>`——记录每张表的元数据存储在哪个页
- `index_meta_pages_`：`std::map<index_id_t, page_id_t>`——记录每个索引的元数据存储在哪个页

`GetNextTableId()` 和 `GetNextIndexId()` 从 map 的最后一个 key+1 推导新 ID。

### 3.2 TableInfo 与 IndexInfo

TableInfo 持有 `TableMetadata*`（元数据）和 `TableHeap*`（实际操作对象）。IndexInfo 持有 `IndexMetadata*`（元数据）、`IndexSchema*`（key_schema_，浅拷贝自表 Schema 的索引列子集）和 `Index*`（BPlusTreeIndex，底层 B+ 树对象）。

**IndexInfo::Init(meta_data, table_info, bpm)**：
1. 保存 `meta_data_`
2. 通过 `Schema::ShallowCopySchema(table_schema, meta_data->GetKeyMapping())` 构造 `key_schema_`——浅拷贝使 key_schema_ 中的 Column 指针与表 Schema 共享同一份 Column 对象，IndexInfo 析构时 `delete key_schema_`（is_manage_=false，不释放 Column）
3. 调用 `CreateIndex(bpm, "bptree")` 创建底层 BPlusTreeIndex

### 3.3 CatalogManager 的六个 map

| Map | Key | Value | 用途 |
|-----|-----|-------|------|
| table_names_ | string (表名) | table_id_t | 按名查表 ID |
| tables_ | table_id_t | TableInfo* | 按 ID 查表对象 |
| index_names_ | string (表名) | map<string, index_id_t> | 按"表名+索引名"查索引 ID |
| indexes_ | index_id_t | IndexInfo* | 按 ID 查索引对象 |

---

## 四、核心算法设计

### 4.1 构造函数——两种打开模式

**init = true（新建数据库）**：
- `catalog_meta_ = CatalogMeta::NewInstance()`——空元数据
- `FlushCatalogMetaPage()`——将空目录写回 0 号页
- `next_table_id_ = next_index_id_ = 0`

**init = false（打开已有数据库）**：
1. Fetch `CATALOG_META_PAGE_ID`（0 号页）→ `CatalogMeta::DeserializeFrom` 恢复 catalog_meta_
2. Unpin 0 号页（未修改）
3. `next_table_id_ = catalog_meta_->GetNextTableId()`
4. 遍历 `catalog_meta_->table_meta_pages_`，逐对调 `LoadTable(table_id, page_id)` 恢复所有表
5. 遍历 `catalog_meta_->index_meta_pages_`，逐对调 `LoadIndex(index_id, page_id)` 恢复所有索引

### 4.2 LoadTable——从磁盘页恢复表信息

1. Fetch `page_id` → `TableMetadata::DeserializeFrom` 反序列化出 table_id、table_name、root_page_id、Schema*
2. Unpin 元数据页
3. `TableHeap::Create(bpm, root_page_id, schema, log_mgr, lock_mgr)`——"已有表"重载构造，不 NewPage
4. `TableInfo::Create() → Init(meta, heap)` 组装
5. 登记 `tables_[table_id]` 和 `table_names_[table_name]`

### 4.3 LoadIndex——从磁盘页恢复索引信息

1. Fetch `page_id` → `IndexMetadata::DeserializeFrom` 反序列化
2. Unpin 元数据页
3. 通过 `GetTable(meta->GetTableId())`（私有按 ID 查）找到索引所属的 TableInfo（需要表 Schema 来构造 key_schema_）
4. `IndexInfo::Create() → Init(meta, table_info, bpm)` 组装
5. 登记 `indexes_[index_id]` 和 `index_names_[table_name][index_name]`

### 4.4 CreateTable(table_name, schema, txn, &table_info)

1. 查重：`table_names_.find(table_name)` 已有返回 `DB_TABLE_ALREADY_EXIST`
2. 深拷贝 Schema（`Schema::DeepCopySchema`）——防止外部 shared_ptr 释放后悬空
3. `TableHeap::Create(bpm, schema, txn, log_mgr, lock_mgr)` 创建新堆表（NewPage 分配第一页）
4. `TableMetadata::Create(table_id, name, root_page_id, schema)` 构造元数据
5. NewPage 分配元数据页 → SerializeTo 写入 → Unpin 标记脏
6. `TableInfo::Create() → Init(meta, heap)` 组装
7. 登记所有 maps → `FlushCatalogMetaPage()` 持久化

### 4.5 CreateIndex(table_name, index_name, index_keys, txn, &index_info, "bptree")

1. 查表存在
2. 对 `index_keys` 逐列名调 `schema->GetColumnIndex` 构建 `key_map`（`vector<uint32_t>`）
3. 任一列名无效返回 `DB_COLUMN_NAME_NOT_EXIST`
4. 查重索引名
5. NewPage 分配元数据页 → `IndexMetadata::Create` → SerializeTo → Unpin 脏
6. `IndexInfo::Create() → Init(meta, table_info, bpm)` 组装
7. 登记 → `FlushCatalogMetaPage()`

### 4.6 DropTable(table_name)

1. 查表存在
2. 遍历 `index_names_[table_name]` 获取该表所有索引，逐个调 `DropIndex`
3. 清除 `index_names_` 中该表名的空条目
4. `table_info->GetTableHeap()->DeleteTable()` 回收堆表所有页
5. `DeletePage` 回收元数据页
6. 从 `catalog_meta_->table_meta_pages_`、`table_names_`、`tables_` 清除
7. `delete table_info`
8. `FlushCatalogMetaPage()`

### 4.7 DropIndex(table_name, index_name)

1. 查表+查索引存在
2. `catalog_meta_->DeleteIndexMetaPage(bpm, index_id)` 回收元数据页（内部 DeletePage + 从 map 擦除）
3. 从 `index_names_[table_name]` 和 `indexes_` 擦除（若表名下索引 map 为空，同时擦除该表名条目）
4. `delete index_info`
5. `FlushCatalogMetaPage()`

### 4.8 FlushCatalogMetaPage——持久化中枢

Fetch 0 号页 → `catalog_meta_->SerializeTo(page->GetData())` → Unpin 标记脏。每次修改 catalog_meta_ 后必须调用（CreateTable/DropTable/CreateIndex/DropIndex）。

### 4.9 BPlusTree 构造的根加载修正

原有 BPlusTree 构造不会从 IndexRootsPage 恢复 `root_page_id_`，导致重启数据库后已存在的索引 B+ 树被当作空树。修正：构造末尾 Fetch `INDEX_ROOTS_PAGE_ID` → `IndexRootsPage::GetRootId(index_id_, &root_page_id_)` → Unpin。新创建索引时 IndexRootsPage 无该条目，`root_page_id_` 保持 `INVALID_PAGE_ID`；已有索引时正确恢复根页号。

---

## 五、测试方案与结果

### 5.1 课程组测试

| 测试用例 | 覆盖内容 |
|----------|----------|
| CatalogMetaTest | CatalogMeta 的序列化/反序列化 roundtrip：16 表 + 24 索引 |
| CatalogTableTest | CreateTable → GetTable → 持久化重启 → GetTable 验证 |
| CatalogIndexTest | CreateTable → CreateIndex→ InsertEntry → ScanKey → 持久化重启 → GetIndex → ScanKey 验证 |

### 5.2 自编测试

| 测试用例 | 覆盖内容 |
|----------|----------|
| ErrorPaths | 表不存在/索引不存在/列名无效/重复创建表/重复创建索引的错误返回码 |
| DropTableCascadesIndexes | 删表后级联删除所有索引，GetIndex 返回 DB_TABLE_NOT_EXIST |
| GetTablesAndIndexes | GetTables 返回全部表、GetTableIndexes 返回正确数量的索引 |

### 5.3 测试结果

全部 6 个测试用例通过（课程组 3 + 自编 3）。编译环境：g++ 11.4.0，cmake 3.28.1，Debug 模式。B+ 树测试在构造函数修改后同步验证通过。

---

## 六、与已有模块的接口

| 依赖模块 | 使用方式 |
|----------|----------|
| BufferPoolManager (实验 #1) | NewPage 分配元数据页、FetchPage 读已有页、UnpinPage 标记脏/只读、DeletePage 回收页 |
| TableHeap (实验 #2) | `TableHeap::Create(bpm, schema, ...)` 新建表；`TableHeap::Create(bpm, root_page_id, schema, ...)` 打开已有表；`DeleteTable()` 回收所有页 |
| Schema / Column (实验 #2) | `DeepCopySchema` 独立复制表 Schema；`ShallowCopySchema` 浅拷贝索引列子集 |
| BPlusTreeIndex / BPlusTree (实验 #3) | IndexInfo::Init 通过 `CreateIndex` 实例化 BPlusTreeIndex；BPlusTree 构造从 IndexRootsPage 恢复根页号 |

---

## 七、遇到的问题与解决方案

| # | 问题 | 原因 | 解决方案 |
|---|------|------|---------|
| 1 | 重启后已有索引 ScanKey 失败 | BPlusTree 构造不加载根页号，root_page_id_ 保持 INVALID | 构造末尾从 IndexRootsPage 读取已有根页号 |
| 2 | IndexInfo::Init 重定义 | 头文件 inline 定义与 .cpp 重复 | 保留头文件 inline，删除 .cpp 重复 |
| 3 | DropTable 后 GetIndex 返回 DB_INDEX_NOT_FOUND 而非 DB_TABLE_NOT_EXIST | index_names_ 中仍残留空 map 条目 | DropTable 和 DropIndex 中清理空条目 |
| 4 | TableMetadata::GetRootPageId 编译错误 | 方法名实际为 GetFirstPageId | 修正方法名 |
| 5 | CreateIndex(string) 参数传入 "BPlusTree" | CreateIndex 内部判断 `index_type == "bptree"`（小写） | 修正为 "bptree" |

---

## 八、附录：关键常量

| 常量 | 值 | 含义 |
|------|-----|------|
| CATALOG_META_PAGE_ID | 0 | CatalogMeta 所在的逻辑页号 |
| CATALOG_METADATA_MAGIC_NUM | 89849 | CatalogMeta 魔数 |
| TABLE_METADATA_MAGIC_NUM | 344528 | TableMetadata 魔数 |
| INDEX_METADATA_MAGIC_NUM | 344528 | IndexMetadata 魔数 |
| PAGE_SIZE | 4096 | 数据页字节数 |
| DB_SUCCESS | 0 | 操作成功 |
| DB_TABLE_ALREADY_EXIST | 1 | 表已存在 |
| DB_TABLE_NOT_EXIST | 5 | 表不存在 |
| DB_INDEX_NOT_FOUND | 7 | 索引未找到 |
| DB_COLUMN_NAME_NOT_EXIST | 9 | 列名不存在 |
