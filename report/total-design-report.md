# MiniSQL 设计总报告

组员：单人完成  
课程：数据库系统原理  

---

# 第1章 MiniSQL 总体框架

## 1.1 实现功能

MiniSQL 是一个精简型单用户 SQL 引擎，支持通过字符界面输入 SQL 语句，实现以下功能：

1. 数据库管理：CREATE/DROP DATABASE，USE，SHOW DATABASES
2. 表管理：CREATE/DROP TABLE，SHOW TABLES
3. 索引管理：CREATE/DROP INDEX，SHOW INDEXES
4. 数据操作：INSERT、DELETE、UPDATE、SELECT
5. 数据类型：INT、CHAR(N)、FLOAT
6. 表定义：最多 32 个属性，支持 UNIQUE 约束和单属性主键
7. 索引：对主键自动建立 B+ 树索引；对 UNIQUE 属性自动建立 B+ 树索引；用户可手动创建/删除索引
8. 查询：支持 AND/OR 连接的多条件查询，支持等值查询和区间查询
9. 批量执行：支持 execfile 执行 SQL 脚本文件
10. 并发控制（Bonus）：支持共享锁/排他锁、锁升级和死锁检测的 Lock Manager
11. 故障恢复：支持 Redo/Undo 两阶段恢复的 Recovery Manager

## 1.2 系统体系结构

MiniSQL 采用分层架构，自上而下共有七层：

```
SQL Parser (flex/bison)          ← 词法与语法分析，生成抽象语法树
       │
Planner & Executor               ← 语义检查，生成执行计划，火山模型执行
       │
       ├─ Catalog Manager        ← 元数据管理（表/索引的定义与持久化）
       ├─ Index Manager          ← B+ 树索引
       └─ Record Manager         ← 堆表记录管理（Row/Field/Schema 序列化）
              │
       Buffer Pool Manager       ← 内存页帧池，LRU 淘汰，脏页落盘
              │
       Disk Manager              ← 数据页分配/回收/读写，逻辑-物理页映射
              │
       Database File             ← 磁盘上的物理文件（共享表空间）
```

各层之间通过明确的接口调用。上层的 Planner/Executor 通过 Catalog Manager 获取表结构信息，通过 Record Manager 和 Index Manager 读写数据；Record/Index Manager 通过 Buffer Pool Manager 透明访问磁盘页；Buffer Pool Manager 通过 Disk Manager 完成物理 I/O。Lock Manager 和 Recovery Manager 是相对独立的模块——前者为事务提供锁管理，后者提供崩溃恢复能力。

## 1.3 设计语言与运行环境

- 编程语言：C++11
- 构建工具：CMake 3.28.1
- 编译器：g++ 11.4.0
- 第三方库：glog（日志），googletest（测试框架），flex/bison（词法语法分析）
- 运行环境：Ubuntu 22.04 LTS (WSL2)，64 位
- 数据页大小：PAGE_SIZE = 4096 字节
- 缓冲池大小：DEFAULT_BUFFER_POOL_SIZE = 20480 页

---

# 第2章 内部数据结构

## 2.1 数据页（Page）

每个数据页为 4096 字节，是内存与磁盘交互的基本单位。Page 对象是 BufferPoolManager 中页帧池的基本单元：

```cpp
class Page {
  char data_[PAGE_SIZE];        // 4096 字节的实际数据
  page_id_t page_id_;           // 逻辑页号
  int pin_count_;                // 被引用的次数（pin count > 0 时不可淘汰）
  bool is_dirty_;                // 是否被修改过（脏页淘汰前需落盘）
  ReaderWriterLatch rwlatch_;   // 读写锁
};
```

不同类型的页通过 `reinterpret_cast` 将 `data_` 解释为对应 C++ 对象：
- BitmapPage：位图管理页（每个 Extent 一张，管理 32704 个数据页的分配状态）
- DiskFileMetaPage：Extent 元信息页（物理页 0）
- TablePage：堆表数据页（Slotted-page 结构）
- BPlusTreePage/InternalPage/LeafPage：B+ 树结点页
- TableMetadata/IndexMetadata 页：Catalog 元数据页

## 2.2 物理文件布局

数据库文件采用共享表空间设计，所有数据存放在同一文件中：

```
物理页 0:     DiskFileMetaPage（num_extents_, extent_used_page_[]）
物理页 1:     Extent 0 的 BitmapPage
物理页 2~N:   Extent 0 的 BITMAP_SIZE 个数据页
物理页 N+1:   Extent 1 的 BitmapPage
...
```

每个 Extent = 1 张 BitmapPage + BITMAP_SIZE 个数据页。BITMAP_SIZE = `(PAGE_SIZE - 2 × sizeof(uint32_t)) × 8`。以 PAGE_SIZE=4096 计算，BITMAP_SIZE = (4096 - 8) × 8 = 32704。单个 Extent 在磁盘上占地 (1 + 32704) × 4096 ≈ 128 MB。

数据页用逻辑页号连续编号，只给真正存数据的页编号，MetaPage 和 BitmapPage 不占逻辑页号。逻辑页号到物理页号的映射公式：

$$\text{Physical} = \text{Logical} + \lfloor \text{Logical} / \text{BITMAP\_SIZE} \rfloor + 2$$

其中 +1 跳过 MetaPage，再 +1 跳过当前 Extent 的 BitmapPage。验证：逻辑页 0 → 物理页 2（Extent 0 的第 0 个数据页，跳过了物理页 0 的 MetaPage 和物理页 1 的 BitmapPage）。

## 2.3 RowId（行标识符）

RowId 为 64 位整数，同时具有逻辑和物理意义：

```
| page_id (32 bit) | slot_num (32 bit) |
```

- `page_id`：记录所在的 TablePage 逻辑页号
- `slot_num`：记录在该页的 Slot 数组中的下标（从 0 开始）

RowId 的作用体现在两个方面：一是在索引中，叶结点存储的键值对是索引键到 RowId 的映射，通过索引键沿 B+ 树查找得到 RowId，即可在堆表中定位记录；二是在堆表中，借助 RowId 中存储的 page_id 和 slot_num，可快速定位到记录位于物理文件的哪个页面和哪个槽位。

## 2.4 TablePage—Slotted-page 结构

TablePage 是堆表的基本存储单元，继承 Page，采用经典的 Slotted-page 布局：

```
低地址 → 高地址
| HEADER (24B) | SLOT数组 (8B×N) | ... FREE SPACE ... | Tuple N-1 | ... | Tuple 0 |
                                   ← FreeSpacePointer
```

固定表头（24B）：

| 偏移 | 字段 | 说明 |
|------|------|------|
| 0 | page_id | 本页逻辑页号 |
| 4 | LSN | 日志序列号 |
| 8 | PrevPageId | 前驱页号（双向链表） |
| 12 | NextPageId | 后继页号 |
| 16 | FreeSpacePointer | 空闲空间起始位置 |
| 20 | TupleCount | 当前 slot 数量 |

每个 Slot 占 8 字节——4 字节 offset（记录在页内的偏移位置）+ 4 字节 size（记录序列化后的字节数）。记录从高地址向低地址生长，插入时 FreeSpacePointer 左移，记录写在 FreeSpacePointer 的位置上。

删除采用两段式：MarkDelete 将对应 slot 的 size 最高位（DELETE_MASK = `1 << 31`）置 1 标记逻辑删除；ApplyDelete 通过 memmove 将后续数据向高地址搬移（压实），回收空间。

## 2.5 Row/Field/Schema 序列化格式

### Column 序列化（32 + name.size() 字节）

| 偏移(B) | 大小(B) | 字段 |
|---------|---------|------|
| 0 | 4 | COLUMN_MAGIC_NUM (210928) |
| 4 | 4 | name_size |
| 8 | name_size | name 字符串 |
| 8+name_size | 4 | type（uint32_t） |
| 12+name_size | 4 | len |
| 16+name_size | 4 | table_ind |
| 20+name_size | 4 | nullable（uint32_t） |
| 24+name_size | 4 | unique（uint32_t） |

所有整型字段统一使用 uint32_t（4 字节），消除 `sizeof(bool)` 和 `sizeof(TypeId)` 的平台差异性。

### Schema 序列化

| 偏移(B) | 大小(B) | 字段 |
|---------|---------|------|
| 0 | 4 | SCHEMA_MAGIC_NUM (200715) |
| 4 | 4 | column_count |
| 8 | 可变 | column_0 序列化数据 |
| ... | ... | ... |
| 可变 | 可变 | column_{N-1} 序列化数据 |

### Row 序列化

```
| field_count (4B) | null_bitmap (ceil(N/8) B) | field_0_data | ... | field_{N-1}_data |
```

null 位图：bit i = 1 表示字段 i 为 null。null 字段在 body 中不占空间。序列化时两趟扫描——第一趟标记 null 位，第二趟写非 null 字段数据。

Field 的序列化委托给 Type 子类，格式由各类型定义：
- TypeInt：非 null 时写 4B int32_t；null 时写 0B
- TypeFloat：非 null 时写 4B float_t；null 时写 0B
- TypeChar：非 null 时写 4B 长度前缀 + 实际字符数据；null 时写 0B

## 2.6 B+ 树结点结构

### BPlusTreePage 基类（28B 头）

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 4 | page_type_ | LEAF_PAGE 或 INTERNAL_PAGE |
| 4 | 4 | key_size_ | 索引键的字节长度 |
| 8 | 4 | lsn_ | 日志序列号（未使用） |
| 12 | 4 | size_ | 当前键值对数量 |
| 16 | 4 | max_size_ | 最大键值对容量 |
| 20 | 4 | parent_page_id_ | 父结点 page_id |
| 24 | 4 | page_id_ | 本结点 page_id |

`GetMinSize()` 根据页类型和根状态返回不同值：根叶返回 1，根内结点返回 2，非根结点返回 `max_size_ / 2`。

### InternalPage（内部结点）

继承 28B 头，剩余 `PAGE_SIZE - 28` 字节存储键值对数组。每对 = `key_size + sizeof(page_id_t)` 字节。size = n 意味着 n 个 value（子指针）+ n 个 key，但 KeyAt(0) 始终为 INVALID——指针比键多一个，用无效键占位实现统一索引。

路由不变性：ValueAt(0) 指向所有键 < KeyAt(1) 的子树；ValueAt(i) 指向 KeyAt(i) ≤ K < KeyAt(i+1) 的子树；ValueAt(size-1) 指向所有键 ≥ KeyAt(size-1) 的子树。Lookup 采用二分查找（从 index=1 开始，跳过 INVALID 的 KeyAt(0)）。

### LeafPage（叶结点）

32B 头（比 InternalPage 多 4B 的 `next_page_id_`）。键值对从 index=0 开始全部有效，无 INVALID 占位键。每对 = `key_size + sizeof(RowId)` 字节。`next_page_id_` 将所有叶子串成单向链表，支持 IndexIterator 顺序遍历和范围扫描。

### GenericKey 与 KeyManager

GenericKey 定义为 `char data[0]`（柔性数组），实际内存通过 `malloc(key_size_)` 分配。KeyManager 持有 `key_schema_`（Schema*，浅拷贝自表 Schema 的索引列子集）和 `key_size_`。

- `SerializeFromKey(key_buf, row, schema)`：将 Row 的字段按 key_schema_ 序列化到 GenericKey::data 中
- `DeserializeToKey(key_buf, row, schema)`：从 GenericKey::data 反序列化出 Row
- `CompareKeys(lhs, rhs)`：将两个 GenericKey 反序列化为 Row，按列顺序逐 Field 调用 `CompareLessThan`/`CompareGreaterThan` 比较，返回 -1/0/1

## 2.7 Catalog 元数据

```
逻辑页 0: CatalogMeta（table_id→meta_page, index_id→meta_page 的映射表）
逻辑页 N: TableMetadata（表名/表ID/根页号/Schema）
逻辑页 M: IndexMetadata（索引名/索引ID/所属表ID/key_map_）
```

每个表和索引的元数据各自独占一个数据页（PAGE_SIZE=4096）。CatalogMeta 独占逻辑页 0（CATALOG_META_PAGE_ID）。

### CatalogMeta 序列化格式

| 偏移(B) | 大小(B) | 字段 |
|---------|---------|------|
| 0 | 4 | CATALOG_METADATA_MAGIC_NUM (89849) |
| 4 | 4 | table count |
| 8 | 4 | index count |
| 12 | N×8 | N 对 (table_id 4B + page_id 4B) |
| 12+N×8 | M×8 | M 对 (index_id 4B + page_id 4B) |

### TableMetadata 序列化格式

| 字段 | 大小 |
|------|------|
| TABLE_METADATA_MAGIC_NUM (344528) | 4B |
| table_id | 4B |
| table_name（长度前缀+数据） | 4B+len |
| root_page_id | 4B |
| schema（Schema::SerializeTo） | 可变 |

### IndexMetadata 序列化格式

| 字段 | 大小 |
|------|------|
| INDEX_METADATA_MAGIC_NUM (344528) | 4B |
| index_id | 4B |
| index_name（长度前缀+数据） | 4B+len |
| table_id | 4B |
| key_map_ size | 4B |
| key_map_ 各元素 | size×4B |

## 2.8 Lock Manager 数据结构

```
lock_table_: unordered_map<RowId, LockRequestQueue>

LockRequestQueue 包含：
  req_list_: list<LockRequest>          // 等待队列
  req_list_iter_map_: unordered_map     // 快速定位迭代器
  cv_: condition_variable               // 条件变量（阻塞/唤醒等待事务）
  is_writing_: bool                     // 是否有排他锁
  is_upgrading_: bool                   // 是否正在升级
  sharing_cnt_: int                     // 共享锁持有计数

LockRequest 包含：
  txn_id_: 事务ID
  lock_mode_: 请求的锁类型（kShared/kExclusive）
  granted_: 已授予的锁类型（kNone/kShared/kExclusive）

waits_for_: unordered_map<txn_id, set<txn_id>>  // 死锁检测等待图
```

## 2.9 Transaction 状态机

```
     _________________________
    |                         v
 GROWING → SHRINKING → COMMITTED   ABORTED
    |__________|________________________^
```

- GROWING：可获取锁，不可释放锁
- SHRINKING：不可获取锁，可释放锁（首次 Unlock 触发转换）
- COMMITTED：事务已提交
- ABORTED：事务已中止

---

# 第3章 各模块设计

## 3.1 实验 #1：Disk and Buffer Pool Manager

### 3.1.1 模块定位

Disk Manager 与 Buffer Pool Manager 位于系统架构底层。Disk Manager 负责数据库文件中数据页的分配、回收和读写；Buffer Pool Manager 在内存中维护固定大小的页帧池，对外提供透明的页访问接口。

### 3.1.2 BitmapPage

BitmapPage 是模板类，模板参数为 PageSize。在磁盘上占用恰好 PageSize 字节，内存布局与磁盘格式完全一致——前两个 uint32_t 是 `page_allocated_` 和 `next_free_page_`，后面紧跟着 `bytes` 数组（`MAX_CHARS = PageSize - 2 × sizeof(uint32_t)` 字节）。GetMaxSupportedSize = 8 × MAX_CHARS。

**AllocatePage(&page_offset)**：出参 page_offset 传入前无需初始化——函数内部找到空位后直接覆盖写入。外层遍历 bytes 数组（字节满 0xFF 时剪枝跳过），内层逐位检查。找到 0 位后置 1，写 page_offset = i×8+j，递增 page_allocated_，更新 next_free_page_ 为 page_offset+1，返回 true。全部满则返回 false。

**DeAllocatePage(page_offset)**：越界检查→拆分为 byte_index/bit_index→检查是否已空闲（防重复释放）→构造掩码取反后按位与清零→递减 page_allocated_→若释放位置比 next_free_page_ 更靠前则回指。

**IsPageFree(page_offset)**：越界返回 false，否则委托 IsPageFreeLow。IsPageFreeLow 直接位运算 `(bytes[byte] & (1 << bit)) == 0`。

CPP 文件末尾显式实例化了 PageSize = 64/128/256/512/1024/2048/4096。

### 3.1.3 DiskManager

DiskManager 管理整个数据库文件。构造函数打开/创建 db_file，将物理页 0 读入 meta_data_ 缓存。ReadPage/WritePage 通过 MapPageId 转换后委托 ReadPhysicalPage/WritePhysicalPage。

**MapPageId(logical)**：`return logical + logical / BITMAP_SIZE + 2`。

**AllocatePage()**：无入参——新页分配到哪个 Extent 的哪个位置完全由内部决定，调用者只需获取逻辑页号。算法分两趟：第一趟遍历已有 Extent（通过 meta->num_extents_），对每个 Extent 检查 `extent_used_page_[i] < BITMAP_SIZE`（未满），读 BitmapPage（物理位置 = `1 + i × (BITMAP_SIZE + 1)`），reinterpret_cast 后调 AllocatePage，成功则写回并更新 MetaPage，返回逻辑页号。第二趟：所有 Extent 满则新建 Extent（全零 buf = 全空闲 BitmapPage），分配第一页后写盘并更新 MetaPage。

**DeAllocatePage / IsPageFree**：由逻辑页号反推 extent_id 和 page_offset，读 BitmapPage，委托对应方法。DeAllocatePage 还需写回并更新计数。

### 3.1.4 LRUReplacer

继承抽象基类 Replacer。内部使用 `std::list<frame_id_t>`（有序队列：front 最久未用，back 最新 Unpin）和 `std::unordered_set<frame_id_t>`（O(1) 成员检查）。

- Victim(&frame_id)：取 lru_list_.front() 写入出参，pop_front，erase set，返回 true。列表空返回 false
- Pin(frame_id)：若在 set 中（O(1) 检查），从 list 和 set 删除
- Unpin(frame_id)：若不在 set 中，加入 list 尾部（最新使用）和 set。已在则防御性忽略
- Size()：返回 lru_list_.size()

### 3.1.5 BufferPoolManager

持有 `Page* pages_[pool_size_]`（下标为 frame_id）、`page_table_`（page_id→frame_id 映射）、`free_list_`（空槽位队列）、LRUReplacer。

槽位状态机：free_list_（空槽位，无有效页）→ LRU list（装了页但无人用，可淘汰）→ 不在两者（装了页且被 pin，不可淘汰）。

**FetchPage(page_id)**：查 page_table_ → 命中则 pin_count_++、Pin replacer、返回 Page*。未命中则 TryToFindFreePage 找槽位 → ReadPage 从磁盘加载 → 设置元数据 → 登记 page_table_ → Pin replacer。

**NewPage(&page_id)**：先 TryToFindFreePage 找槽位 → 成功后再 AllocatePage 分配磁盘页——此顺序是关键：若先分配后找槽位，缓冲池满时页号被标记为占用却未进入任何槽位，造成永久泄漏。

**UnpinPage(page_id, is_dirty)**：查表 → pin_count_≤0 则拒绝 → 设 is_dirty_ → 递减 pin_count_ → 仅归零时 Unpin replacer。

**FlushPage / DeletePage**：WritePage 落盘并清除 dirty / 从 maps 和 replacer 清除，归还 free_list_，DeletePage 还调磁盘 DeAllocatePage。

### 3.1.6 关键设计决策

- max_size 减 1 溢出保护：B+ 树构造时 `(PAGE_SIZE - HEADER) / pair_size - 1`，确保插入时的临时溢出不写出 data_ 边界腐化相邻 Page 的 page_id_ 和 pin_count_
- NewPage 顺序：先槽位再磁盘，防止缓冲池满时页号泄漏
- 逻辑页号映射：+2 = +1（MetaPage）+1（当前 Extent 的 BitmapPage）

---

## 3.2 实验 #2：Record Manager

### 3.2.1 模块定位

Record Manager 负责数据记录的持久化存储与内存表示。Upper层 Executor 通过 TableHeap 插入、删除、更新、查询记录；下层 BufferPoolManager 提供透明页 I/O。同时为 Catalog Manager（实验 #4）和 Index Manager（实验 #3）提供 Schema/Row/Field 的序列化基础设施。

### 3.2.2 类型体系

从最小的数据类型到最大的存储容器，六层抽象：

```
Type (int/float/char 的读写比较基类) — 框架已实现
  ├─ TypeInt / TypeFloat / TypeChar
  │
Field — 一行中单个列的值（TypeId + union + null 标记）— 框架已实现
  │
Column — 表的一列的元数据（列名 + TypeId + 长度 + 是否可空/唯一）
  │
Schema — 若干 Column 的集合，描述整张表或索引的结构
  │
Row — 一行完整记录（多个 Field + RowId）
  │
TableHeap / TablePage — 以无序堆形式组织的物理存储容器
  │
TableIterator — 堆表遍历器
```

### 3.2.3 Column 序列化

七个字段被序列化为 32 + name.size() 字节。所有整型字段统一使用 uint32_t（4 字节），消除 `sizeof(bool)` 和 `sizeof(TypeId)` 的平台差异性。列名字符串采用"4 字节长度前缀 + 实际内容"编码。

DeserializeFrom 校验 MAGIC_NUM 后按序读出各字段。按 type 分支构造：kTypeChar 走三参数构造（传入用户指定的长度），其他类型走两参数构造（长度由构造函数自动设定——int 为 sizeof(int32_t)=4，float 为 sizeof(float_t)=4）。

### 3.2.4 Schema 序列化

MAIGC_NUM(200715) → column_count → 逐列 SerializeTo。GetSerializedSize = sizeof(uint32_t)×2 + sum(column->GetSerializedSize())。DeserializeFrom 返回 schema->GetSerializedSize()（不做 header 的 double count）。

### 3.2.5 Row 序列化

Header（字段数 4B + null_bitmap ceil(N/8)B）→ Body（逐非 null 字段的 SerializeTo 结果）。null 字段在 body 中不占空间。

关键设计：两趟扫描——第一趟写 header 和 null 位图，第二趟写非 null 数据；反序列化前调 destroy() 清空已有字段（支持重复反序列化）。

### 3.2.6 TablePage（框架已实现）

Slotted-page 物理布局（见 §2.4）。TablePage 继承 Page，利用 data_[PAGE_SIZE] 存储实际数据。构建时 FreeSpacePointer 设 PAGE_SIZE，插入时左移，删除时 memmove 压实。逻辑删除通过 DELETE_MASK 标记实现。

### 3.2.7 TableHeap 算法

**InsertTuple**：First Fit 策略——沿 TablePage 双向链表逐页尝试 InsertTuple，所有页满则 NewPage 创建新页并链接。若 row.GetSerializedSize > SIZE_MAX_ROW（单页最大容量）直接返回 false（不支持跨页存储）。

**GetTuple**：RowId.GetPageId() 定位页 → FetchPage → TablePage::GetTuple 反序列化填充 fields_。

**UpdateTuple**：原地更新优先——Fetch 目标页 → TablePage::UpdateTuple 尝试原地更新；若失败（空间不够）→ MarkDelete → ApplyDelete → InsertTuple（删旧插新，RowId 更新）。

**Delete（两段式）**：MarkDelete 置 DELETE_MASK（逻辑删除）；ApplyDelete 压实回收空间（物理删除）。RollbackDelete 清除标记。

**Begin/End**：Begin 从 first_page_id_ 逐页沿 NextPageId 遍历，每页调 GetFirstTupleRid 找第一个未删除记录。End 返回 row_=nullptr 的哨兵。

### 3.2.8 TableIterator

持有 TableHeap*、Row*（当前记录深拷贝）、Txn*。构造时若 rid 有效则 GetTuple 加载数据。前缀 ++ 同页内 GetNextTupleRid → 跨页沿 NextPageId→GetFirstTupleRid → 链表走完变 End。

---

## 3.3 实验 #3：Index Manager

### 3.3.1 模块定位

Index Manager 提供基于磁盘的 B+ 树动态索引结构。通过 KeyManager 将 Row 序列化为 GenericKey 作为键，叶结点 Value 为 RowId。上层 Executor 通过 BPlusTreeIndex（框架封装）进行等值查找和范围扫描。

### 3.3.2 物理布局（见 §2.6）

### 3.3.3 BPlusTreePage 基类方法

- IsLeafPage()：`page_type_ == LEAF_PAGE`
- IsRootPage()：`parent_page_id_ == INVALID_PAGE_ID`
- GetMinSize()：根叶→1，根内→2，非根→max_size_/2
- SetPageType/SetKeySize/SetSize/SetMaxSize/SetPageId/SetParentPageId：直接赋值对应字段

### 3.3.4 InternalPage 方法

- **Init**：设 page_type_=INTERNAL_PAGE，size_=0，各元数据字段
- **Lookup(key, KM)**：二分查找（lft=1, rht=size-1），找第一个 KeyAt(mid)>key 的位置，返回 ValueAt(lft-1)
- **PopulateNewRoot**：SetValueAt(0, old), SetKeyAt(1, new_key), SetValueAt(1, new), SetSize(2)
- **InsertNodeAfter**：ValueIndex 定位 old_value→从后往前搬移数据→在新位置写入→IncreaseSize
- **Remove**：前移覆盖被删位置→IncreaseSize(-1)
- **RemoveAndReturnOnlyChild**：返回 ValueAt(0)，SetSize(0)
- **MoveHalfTo(recipient, bpm)**：从 size/2 起 CopyNFrom 搬给 recipient，本页截断
- **CopyNFrom(src, size, bpm)**：PairCopy→逐个"收养"子结点（更新 parent_page_id）→IncreaseSize
- **MoveAllTo(recipient, middle_key, bpm)**：middle_key+ValueAt(0)放 recipient 末尾作为新旧分隔→逐对 CopyLastFrom→本页清零
- **MoveFirstToEndOf/MoveLastToFrontOf**：Redistribute 用——前者搬第一个子结点到 recipient 末尾（本页前移）；后者搬最后一个到 recipient 开头（recipient 右移后 KeyAt(1) 被 middle_key 覆盖）

### 3.3.5 LeafPage 方法

- **Init**：跟 InternalPage 对称，额外设 next_page_id_ = INVALID_PAGE_ID
- **KeyIndex(key, KM)**：二分查找第一个 KeyAt(i) ≥ key 的位置
- **Lookup(key, value, KM)**：KeyIndex 定位→比较键→写入 value 出参
- **Insert(key, value, KM)**：KeyIndex 定插入位→后移→写入→IncreaseSize
- **RemoveAndDeleteRecord**：KeyIndex 定位→匹配则前移覆盖→IncreaseSize(-1)
- **MoveHalfTo(recipient)**：CopyNFrom 后半→维护叶子链表（recipient 插在本页和原后继之间）
- **CopyNFrom(src, size)**：PairCopy→IncreaseSize
- **MoveAllTo(recipient)**：全部 CopyNFrom→本页清零→recipient.SetNextPageId（防止自环：`if (GetNextPageId() != recipient->GetPageId())`）
- **MoveFirstToEndOf/MoveLastToFrontOf**：Redistribute 用
- **CopyLastFrom/CopyFirstFrom**：末尾追加/头部插入

### 3.3.6 BPlusTree—Insert

**StartNewTree**：NewPage→Init 叶→Insert→设 root_page_id_→Unpin→UpdateRootPageId(1)

**InsertIntoLeaf**：FindLeafPage→Lookup 查重→Insert→超 max_size 则 Split→InsertIntoParent

**Split**：NewPage→Init→MoveHalfTo→返回新页

**InsertIntoParent（递归向上）**：old_node 是根→新根 InternalPage→PopulateNewRoot→设根。非根→Fetch 父页→InsertNodeAfter→超 max_size 则 Split 父页并递归（key 取 new_parent->KeyAt(1)）

### 3.3.7 BPlusTree—Remove

**Remove 入口**：FindLeafPage→RemoveAndDeleteRecord→逐级 while 循环处理 underflow。

**while 循环**：cur 满足 min_size 则结束；否则获取 parent 和 sibling：
- Redistribute（node.size + sibling.size ≥ max_size）：从 sibling 借一个→更新 parent 分隔键
- Merge（右合入左，CMU 15-445 约定）：始终右兄弟合并到左兄弟→parent->Remove 删右兄弟索引→cur=parent 继续循环

**AdjustRoot**：叶根 size=0→删根，树空。内根 size≤1→唯一子结点提升为新根→UpdateRootPageId。

**合并方向的关键性**：旧版在 index=0 时反向操作（左合入右，parent->Remove(0)），内部页键位偏移不一致，导致同父下其他叶子的键路由失效。CMU 15-445 明确约定"始终右合入左，删除右兄弟"，确保被删除的始终是右子，键位移方向一致。

### 3.3.8 BPlusTree—迭代器与范围查询

**Begin()**：FindLeafPage(leftMost=true)→IndexIterator(leaf_id, bpm, 0)

**Begin(key)**：FindLeafPage→KeyIndex 定位→IndexIterator(leaf_id, bpm, idx)

**End()**：空构造 IndexIterator()（INVALID_PAGE_ID）

**ScanKey 范围查询**：BPlusTreeIndex::ScanKey 支持全部比较运算符（=、>、<、>=、<=、<>）。`>`和`>=`通过 GetBeginIterator(key) 定位起始后遍历至 End；`<`和`<=`通过 GetBeginIterator 遍历至 key 位置；`<>`遍历全部后 erase 等值结果。AND 复合条件通过 IndexScanExecutor 的 `set_intersection` 取交集。

### 3.3.9 max_size 的溢出保护

构造时 leaf_max_size_ / internal_max_size_ 的计算减 1，确保 Insert 后临时溢出不超出 data_ 边界腐化 Page::page_id_/pin_count_。

### 3.3.10 BPlusTree 构造的根号恢复

构造函数末尾从 IndexRootsPage 读取已有 root_page_id_——新索引保持 INVALID，已有索引正确恢复。解决重启后索引 B+ 树被视为空树的问题。

### 3.3.11 IndexIterator

构造时 Fetch 目标 LeafPage 并 pin。operator* 返回 `page->GetItem(item_index)`。operator++：item_index++；超 page->GetSize() 则沿 next_page_id_ 去下一页（Unpin 旧、Fetch 新、归零 item_index）；无下一页则 page=nullptr（End）。

---

## 3.4 实验 #4：Catalog Manager

### 3.4.1 模块定位

Catalog Manager 负责管理数据库的所有模式信息（表/索引的元数据），是"元数据中枢"。向上为 Executor/Planner 提供表与索引的查询接口，向下依赖 BufferPoolManager 完成元数据的持久化存储。

### 3.4.2 三层持久化

- CatalogMeta（逻辑页 0）：记录 table_id→meta_page、index_id→meta_page 的映射
- TableMetadata（独立一页）：表名、表ID、根页号、Schema*
- IndexMetadata（独立一页）：索引名、索引ID、所属表ID、key_map_

### 3.4.3 CatalogMeta::GetSerializedSize

4（MAGIC_NUM）+ 4（table count）+ 4（index count）+ table_meta_pages_.size()×8 + index_meta_pages_.size()×8。

### 3.4.4 IndexMetadata::GetSerializedSize

4（MAGIC_NUM）+ 4（index_id）+ 4 + index_name_.length() + 4（table_id）+ 4（key count）+ key_map_.size()×4。

### 3.4.5 IndexInfo::Init(meta_data, table_info, bpm)

1. 保存 meta_data_
2. Schema::ShallowCopySchema(table_schema, meta_data->GetKeyMapping()) 构建 key_schema_——浅拷贝使 Column 指针共享表 Schema 的同一份 Column 对象（IndexInfo 析构时 is_manage_=false，不重复释放）
3. CreateIndex(bpm, "bptree") 创建底层 BPlusTreeIndex

### 3.4.6 CatalogManager 构造

init=true：CatalogMeta::NewInstance()→FlushCatalogMetaPage()。init=false：Fetch CATALOG_META_PAGE_ID→DeserializeFrom→遍历 LoadTable/LoadIndex 逐表逐索引恢复。

### 3.4.7 LoadTable / LoadIndex

LoadTable：Fetch→TableMetadata::DeserializeFrom→TableHeap::Create(bpm, root_page_id, schema)（已有表重载，不 NewPage）→TableInfo::Create→Init→登记。

LoadIndex：Fetch→IndexMetadata::DeserializeFrom→GetTable(table_id) 找所属 TableInfo→IndexInfo::Create→Init→登记。

### 3.4.8 CreateTable / CreateIndex

CreateTable：DeepCopySchema（防止外部 shared_ptr 释放导致悬空指针）→TableHeap::Create→NewPage 写 TableMetadata→TableInfo 组装→登记→FlushCatalogMetaPage。自动为 unique 列和主键建索引。

CreateIndex：验证列名有效（构建 key_map）→NewPage 写 IndexMetadata→IndexInfo::Create→Init→登记→FlushCatalogMetaPage。

### 3.4.9 DropTable / DropIndex

DropTable：级联删除该表所有索引（遍历 index_names_[table_name]）→回收堆表页→回收元数据页→清除 maps→flush→delete。

DropIndex：回收元数据页（DeleteIndexMetaPage）→清除 maps→delete IndexInfo→flush。

### 3.4.10 BPlusTree 构造的根号恢复

构造末尾从 IndexRootsPage 读取已有 root_page_id_。

---

## 3.5 实验 #5：Planner & Executor

### 3.5.1 模块定位

Planner（框架实现）将 Parser 生成的语法树转换为 PlanNode 树；Executor 提供火山模型（Iterator Model）的 Init/Next 接口。本实验主要实现 ExecuteEngine 的 DDL 函数和部分运行时功能。

### 3.5.2 整体流程

```
SQL → Parser → 语法树 → ExecuteEngine::Execute(ast)
  ├─ DDL: ExecuteXxx(ast) → CatalogManager → 直接执行
  └─ DML: Planner::PlanQuery → PlanNode树 → CreateExecutor → Init → while Next → ResultWriter
```

### 3.5.3 ExecuteEngine DDL 函数

**ExecuteCreateTable**：解析 kNodeColumnDefinitionList→构建 vector<Column*> 和 Schema→CatalogManager::CreateTable。unique 列通过 `col_def->val_ == "unique"` 判断；char 列长度从 kNodeNumber 子结点读取；int/float 用两参数构造（不传 len）。

**ExecuteCreateIndex**：AST 顺序为 (index_name, table_name, column_list)。创建后无需重建（索引在插入前创建，随插入自动填充）。

**ExecuteDropIndex**：支持无表名形式（`drop index idx01`）——遍历所有表查找索引归属。

**ExecuteShowIndexes**：支持无参数（列出所有表的所有索引）和带表名两种形式。

**ExecuteExecfile**：逐行读文件→yy_scan_string→yy_switch_to_buffer→MinisqlParserInit→yyparse→MinisqlGetParserRootNode→Execute→MinisqlParserFinish→yy_delete_buffer→yylex_destroy。关键：每行解析后必须 yylex_destroy，否则 lexer 状态残留导致后续行解析失败。

### 3.5.4 五个火山模型算子（框架实现）

**SeqScanExecutor**：TableIterator + predicate->Evaluate 过滤 + TupleTransfer 列投影。

**IndexScanExecutor**：IndexScan 递归处理表达式树——LogicExpression(AND)→两子树 set_intersection 取交集；ComparisonExpression→匹配索引列→ScanKey 返回 RowId 集。Next 从 result_ 中逐个取 RowId→GetTuple→必要时 need_filter_ 再 Evaluate。

**InsertExecutor**：子 executor（ValuesExecutor）拉取值→唯一约束检查（遍历各索引 ScanKey）→InsertTuple→全部索引 InsertEntry。

**UpdateExecutor**：子 executor（SeqScanExecutor）拉取旧行→GenerateUpdatedTuple 构造新行→各索引 RemoveEntry 旧键→InsertEntry 新键→UpdateTuple。

**DeleteExecutor**：子 executor 拉取行→各索引 RemoveEntry→MarkDelete。

---

## 3.6 实验 #6：Recovery Manager

### 3.6.1 模块定位

纯内存简化恢复模块，模拟数据库崩溃后的恢复流程。日志在内存中以 LogRec 链表和 unordered_map 模拟 KV Database。两阶段恢复：RedoPhase 重放日志重建崩溃时刻状态；UndoPhase 回滚未提交事务。

### 3.6.2 LogRec 结构

静态 `next_lsn_`（全局递增）和 `prev_lsn_map_`（每事务最后 LSN）。六个工厂函数（CreateInsertLog/CreateDeleteLog/CreateUpdateLog/CreateBeginLog/CreateCommitLog/CreateAbortLog）各填充对应 type_、txn_id_、操作数据和 LSN 链。BeginLog 的 prev_lsn_ 固定 INVALID（事务起始）。

### 3.6.3 恢复流程

**Init**：加载 checkpoint 的 persist_lsn_、active_txns_、persist_data_。

**RedoPhase**：按 LSN 重放所有日志。每遇日志更新 `active_txns_[txn_id] = lsn`（记录事务最新 LSN，供 UndoPhase 回滚）。Insert→data_[key]=val；Delete→erase；Update→erase old+write new。Commit→erase active_txns_。Abort→沿 prev_lsn_ 链逆序回滚该事务全部操作→erase active_txns_。

**UndoPhase**：对 RedoPhase 后仍在 active_txns_ 的事务（从未 Commit/Abort 的悬空事务），沿 prev_lsn_ 链逆序回滚。

### 3.6.4 思考题：真正的故障恢复设计

**需修改的模块和函数**：

1. **DiskManager/BufferPoolManager**：每个 Page 头部需增加 LSN 字段——写入 Page 前，将当前事务的 LSN 写入页头；FlushPage 时比较页 LSN 与日志 LSN 决定是否需要 WAL 先行写入（Write-Ahead Logging）

2. **LogRec 持久化**：LogRec 需序列化写入日志文件（WAL 文件），而非仅在内存维护。需新增 LogManager 类管理日志文件的写入、读取、checkpoint 生成。LogManager 需实现 `AppendLog(LogRec)`、`Flush()`、`ReadLogs(lsn_start)` 等接口

3. **TablePage/BPlusTreePage**：插入/删除/更新时需生成对应的 LogRec 并通过 LogManager 写入日志文件。需在各操作函数中插入日志记录代码，如 TableHeap::InsertTuple 中调用 `log_mgr->AppendLog(CreateInsertLog(...))`

4. **CheckPoint 机制**：定期生成——记录当前 active_txns_ 和 dirty_page_table_（哪些页被修改过及对应 LSN）。checkpoint 本身也需持久化到磁盘。生成策略：定时（如每 60 秒）或定量（如每 1000 条日志）

5. **恢复流程**：启动时读取最近一次 checkpoint→RedoPhase 从 checkpoint LSN 开始重放所有日志→UndoPhase 回滚 checkpoint 时活跃但未提交的事务。需修改 RecoveryManager 的 Init/RedoPhase/UndoPhase 以处理磁盘级别的恢复

---

## 3.7 实验 #7：Lock Manager (Bonus)

### 3.7.1 模块定位

Lock Manager 是 MiniSQL 的并发控制组件，管理事务对数据记录（RowId）的共享锁和排他锁。实现两阶段锁协议（2PL），后台死锁检测线程自动打破等待环。本模块为 Bonus 内容，涉及锁和条件变量的多线程编程。

### 3.7.2 LockPrepare / CheckAbort

**LockPrepare**：所有锁操作的前置校验。txn 已 Aborted→抛异常；txn 非 Growing→设为 Aborted→抛 LockOnShrinking。若 lock_table_ 无该 RowId 队列则创建。

**CheckAbort**：条件变量等待 lambda 中周期性检查——若 txn 被外部设为 Aborted（如死锁检测），擦除锁请求→notify_all→抛异常。

### 3.7.3 锁操作

**LockShared**：td 校验→READ_UNCOMMITTED 抛异常→加入队列→等待 `!is_writing_ && !is_upgrading_`→授予→sharing_cnt_++→加入 shared_lock_set_。

**LockExclusive**：校验→加入队列→构建等待边（向所有已授予持有者添加 txn_id→holder_id）→等待 `sharing_cnt_ == 0 && !is_writing_`→移除等待边→授予→is_writing_=true→加入 exclusive_lock_set_。

**LockUpgrade**：校验→若 is_upgrading_ 已 true 则抛 UpgradeConflict→设置 is_upgrading_=true→构建等待边→等待 `sharing_cnt_ == 1`（只剩自己）→共享降级→sharing_cnt_--→移除 shared→授予 exclusive→is_writing_=true→is_upgrading_=false→加入 exclusive_lock_set_。关键：升级成功前不提前移除 shared_lock_set_ 条目（确保 Abort 时 ReleaseLocks 能找到并释放）。

**Unlock**：校验→擦除请求→更新 is_writing_/sharing_cnt_/锁集合→notify_all。txn 状态在 Growing 则变为 Shrinking（首次 Unlock 触发两阶段锁收缩）。

### 3.7.4 死锁检测

**AddEdge(t1, t2)**：在 `waits_for_[t1]` 中插入 t2。RemoveEdge 反之。

**HasCycle(DfS)**：收集节点→升序排列→按升序从每个未访问节点启动 DFS→探索邻居时升序（保证确定性）→in_stack 集合检测回边→记录环中最大 txn_id（最年轻事务）→返回第一个环。

**RunCycleDetection**：后台线程定期睡眠→获取全局锁→每次从头构建等待图（遍历 lock_table_ 中所有未授予请求→已授予持有者边）→循环 HasCycle→标记最年轻事务 Aborted→DeleteNode→通知所有 lock_table_ 队列的 cv→继续至无环。

**DeleteNode**：擦除 txn_id 的所有出入边，同时移除等待该 txn 的未授予请求的边。

### 3.7.5 思考题：B+ 树并发控制设计

**需修改的模块和函数**：

1. **BPlusTree 页级锁**：在 FindLeafPage 中，从根向下遍历时使用锁耦合（Latch Crabbing）——对子结点加读锁后释放父结点锁；Insert/Remove 写操作对沿途结点加写锁。需利用 BPlusTreePage 已有的 `WLatch()`/`WUnlatch()`/`RLatch()`/`RUnlatch()` 接口（ReaderWriterLatch）

2. **LockManager 集成**：当前 LockManager 管理的是 RowId 级别的锁（记录锁）。B+ 树的并发控制需要页级锁——需扩展 LockManager 支持 page_id 级别的锁，或在 BPlusTree 内部独立管理页锁

3. **死锁检测扩展**：当前死锁检测基于 `waits_for_` 图（txn→txn）。引入页锁后，页级的等待关系（txn 等待 page latch）也需纳入等待图，否则无法检测涉及页锁和记录锁的混合死锁

4. **隔离级别实现**：不同隔离级别对 B+ 树的锁策略不同——READ_COMMITTED 可尽早释放读锁（减少锁竞争），REPEATABLE_READ 需持有读锁到事务结束。需在 FindLeafPage 中根据 `txn->GetIsolationLevel()` 决定是否在遍历过程中释放读锁

5. **乐观锁（Optimistic Lock Coupling）**：B+ 树的 Insert/Remove 操作可先乐观地假设不会分裂/合并，使用读锁向下遍历，仅在确认需要修改时升级为写锁。如发现页不安全（可能分裂/合并），则释放所有锁，用写锁重新遍历

---

# 第4章 各模块接口

## 4.1 Disk Manager

| 接口 | 说明 |
|------|------|
| `AllocatePage() → page_id_t` | 分配新数据页，返回逻辑页号 |
| `DeAllocatePage(page_id_t)` | 释放逻辑页号 |
| `IsPageFree(page_id_t) → bool` | 判断是否空闲 |
| `ReadPage(page_id_t, char*)` | 读逻辑页到内存 |
| `WritePage(page_id_t, const char*)` | 写内存到逻辑页 |
| `MapPageId(logical) → physical` | 逻辑→物理映射 |

## 4.2 Buffer Pool Manager

| 接口 | 说明 |
|------|------|
| `FetchPage(page_id_t) → Page*` | 获取指定页 |
| `NewPage(&page_id_t) → Page*` | 分配新页 |
| `UnpinPage(page_id_t, bool is_dirty)` | 释放引用 |
| `FlushPage(page_id_t) → bool` | 指定页落盘 |
| `DeletePage(page_id_t) → bool` | 删除指定页 |

## 4.3 Record Manager

| 接口 | 说明 |
|------|------|
| `TableHeap::InsertTuple(Row&, Txn*) → bool` | 插入记录 |
| `TableHeap::UpdateTuple(Row&, const RowId&, Txn*) → bool` | 更新记录 |
| `TableHeap::GetTuple(Row*, Txn*) → bool` | 读记录 |
| `TableHeap::MarkDelete(const RowId&, Txn*) → bool` | 逻辑删除 |
| `TableHeap::ApplyDelete(const RowId&, Txn*)` | 物理删除 |
| `TableHeap::Begin(Txn*) → TableIterator` | 表首迭代器 |
| `TableHeap::End() → TableIterator` | 表尾哨兵 |
| `Row::SerializeTo(char*, Schema*) → uint32_t` | Row 序列化 |
| `Row::DeserializeFrom(char*, Schema*) → uint32_t` | Row 反序列化 |
| `Column::SerializeTo(char*) → uint32_t` | Column 序列化 |
| `Schema::SerializeTo(char*) → uint32_t` | Schema 序列化 |

## 4.4 Index Manager

| 接口 | 说明 |
|------|------|
| `BPlusTree::Insert(GenericKey*, const RowId&, Txn*) → bool` | 插入键值对 |
| `BPlusTree::Remove(const GenericKey*, Txn*)` | 删除键 |
| `BPlusTree::GetValue(const GenericKey*, vector<RowId>&, Txn*) → bool` | 等值查找 |
| `BPlusTree::Begin() → IndexIterator` | 最左迭代器 |
| `BPlusTree::Begin(const GenericKey*) → IndexIterator` | 指定键起始迭代器 |
| `BPlusTree::End() → IndexIterator` | 尾迭代器 |
| `BPlusTreeIndex::ScanKey(const Row&, vector<RowId>&, Txn*, string comp_op) → dberr_t` | 键扫描（= > < >= <= <>） |
| `BPlusTreeIndex::InsertEntry(const Row&, RowId, Txn*) → dberr_t` | 插入条目 |
| `BPlusTreeIndex::RemoveEntry(const Row&, RowId, Txn*) → dberr_t` | 删除条目 |

## 4.5 Catalog Manager

| 接口 | 说明 |
|------|------|
| `CreateTable(name, schema, txn, &info) → dberr_t` | 创建表 |
| `GetTable(name/id, &info) → dberr_t` | 查表 |
| `GetTables(&tables) → dberr_t` | 全部表 |
| `DropTable(name) → dberr_t` | 删表（级联索引） |
| `CreateIndex(table, name, keys, txn, &info, type) → dberr_t` | 创建索引 |
| `GetIndex(table, name, &info) → dberr_t` | 查索引 |
| `GetTableIndexes(table, &idxs) → dberr_t` | 表的所有索引 |
| `DropIndex(table, name) → dberr_t` | 删索引 |

## 4.6 Execution Engine

| 接口 | 说明 |
|------|------|
| `Execute(pSyntaxNode) → dberr_t` | 执行语法树 |
| `ExecutePlan(plan, result, txn, ctx) → dberr_t` | 执行计划树 |
| `ExecuteCreateDatabase/DropDatabase/UseDatabase/...` | 12个DDL函数 |

## 4.7 Lock Manager

| 接口 | 说明 |
|------|------|
| `LockShared(Txn*, const RowId&) → bool` | 共享锁 |
| `LockExclusive(Txn*, const RowId&) → bool` | 排他锁 |
| `LockUpgrade(Txn*, const RowId&) → bool` | 锁升级 |
| `Unlock(Txn*, const RowId&) → bool` | 释放锁 |
| `AddEdge/RemoveEdge/HasCycle/GetEdgeList/RunCycleDetection` | 死锁检测 |

## 4.8 Recovery Manager

| 接口 | 说明 |
|------|------|
| `Init(CheckPoint&)` | 加载检查点 |
| `RedoPhase()` | 重放日志 |
| `UndoPhase()` | 回滚未提交事务 |
| `CreateInsertLog/CreateDeleteLog/CreateUpdateLog/CreateBeginLog/CreateCommitLog/CreateAbortLog` | 工厂函数 |

---

# 第5章 系统测试

## 5.1 测试统计

| 实验 | 课程组测试 | 自编测试 |
|------|----------|----------|
| #1 Disk & Buffer Pool Manager | 4 | 22 |
| #2 Record Manager | 3 | 19 |
| #3 Index Manager | 4 | 3 |
| #4 Catalog Manager | 3 | 3 |
| #5 Planner & Executor | 4 | 0 |
| #6 Recovery Manager | 1 | 0 |
| #7 Lock Manager (Bonus) | 10 | 0 |
| **总计** | **29** | **47** |

全部 76 个测试用例通过。各测试套件内容说明如下：

### 实验 #1：Disk & Buffer Pool Manager

**课程组测试**：
- `disk_manager_test` (2 用例)：BitmapPage 全页分配/释放/回收，DiskManager 跨两个 Extent 分配、MetaPage 计数的正确性
- `lru_replacer_test` (1 用例)：LRUReplacer 的 Pin/Unpin/Victim 基本顺序和重复 Unpin 处理
- `buffer_pool_manager_test` (1 用例)：NewPage/FetchPage/UnpinPage/FlushPage 端到端二进制数据一致性

**自编测试**：
- `disk_manager_student_test` (8 用例，含 BitmapPage 5 + DiskManager 3)：BitmapPage PAGE_SIZE=4096 真实分配、越界拒绝（分配/释放/查询）、重复释放防御、释放后回收复用；DiskManager 的 IsPageFree 语义、页号回收、跨 Extent 分配连续正确性
- `lru_replacer_student_test` (7 用例)：Victim 空队列、Pin 不存在元素、重复 Unpin、Pin/Unpin/Victim 交替、frame_id=0 边界、Size 一致性
- `buffer_pool_manager_student_test` (10 用例)：DeletePage 完整流程与防误删、槽位回收、满池错误路径、UnpinPage/FlushPage 非法输入拒绝、脏页淘汰后数据完整性、CheckAllUnpinned

### 实验 #2：Record Manager

**课程组测试**：
- `tuple_test` (2 用例)：各类型 Field 序列化 roundtrip + null 比较、Row 通过 TablePage Insert→Get→MarkDelete→ApplyDelete 的端到端生命周期
- `table_heap_test` (1 用例)：10000 行插入 + 逐行按 RowId 回读 + 逐字段值比较验证一致性

**自编测试**：
- `tuple_student_test` (9 用例，含 Column 4 + Schema 1 + Row 4)：int/char/float 三种类型 Column 独立序列化 roundtrip、MAGIC_NUM 校验失败、三列 Schema 完整 roundtrip、全 null 行、混合 null 与非 null、空字符串 char、SerializeTo 与 GetSerializedSize 一致性
- `table_heap_student_test` (10 用例)：MarkDelete→RollbackDelete 回滚、MarkDelete→ApplyDelete 物理删除后不可读、UpdateTuple 原地更新、UpdateTuple 删旧插新且 RowId 变化、迭代器跨页遍历、空表 Begin==End、迭代器跳过已删除行、超长记录拒绝（两 char(2047) 超出 SIZE_MAX_ROW）、非法 page_id 的 GetTuple/UpdateTuple、后缀递增 iter++

### 实验 #3：Index Manager

**课程组测试**：
- `b_plus_tree_test` (1 用例)：2000 键随机 shuffle 插入→点查全部→shuffle 删除 1000→验证已删不存在、未删仍存在。100 次独立运行全部通过
- `b_plus_tree_index_test` (2 用例)：BPlusTreeIndex 封装层的 InsertEntry/ScanKey/Destroy
- `index_iterator_test` (1 用例)：IndexIterator 沿叶子链表的遍历和键值对访问

**自编测试**：
- `b_plus_tree_student_test` (3 用例)：完全模拟课测流程 20 trials（shuffle 插入 2000→shuffle 删除 1000→验证）、空树 Begin==End 与 Remove 不崩溃、500 键顺序插入后全部删除验证树正确变空

### 实验 #4：Catalog Manager

**课程组测试**：
- `catalog_test` (3 用例)：CatalogMeta 序列化/反序列化 roundtrip（16 表+24 索引）、CreateTable→GetTable→持久化重启→GetTable 验证、CreateTable→CreateIndex→InsertEntry→ScanKey→持久化重启→GetIndex→ScanKey 验证

**自编测试**：
- `catalog_student_test` (3 用例)：表/索引不存在/列名无效/重复创建的错误返回码、DropTable 级联删除所有索引后 GetIndex 返回 DB_TABLE_NOT_EXIST、GetTables 返回全部表 + GetTableIndexes 按表筛选正确数量

### 实验 #5：Planner & Executor

**课程组测试**：
- `executor_test` (4 用例)：通过手动构造 PlanNode 测试 SeqScanExecutor（谓词过滤+列投影）、IndexScanExecutor（索引扫描+RowId 交并集）、InsertExecutor（唯一检查+多索引更新）、DeleteExecutor（索引同步删除+MarkDelete）

### 实验 #6：Recovery Manager

**课程组测试**：
- `recovery_manager_test` (1 用例)：3 事务×11 条日志×checkpoint 前后操作×RedoPhase+UndoPhase 完整恢复流程（T0 Abort 回滚、T1 Commit 保持、T2 活跃回滚）

### 实验 #7：Lock Manager (Bonus)

**课程组测试**：
- `lock_manager_test` (10 用例)：READ_UNCOMMITTED 拒绝共享锁、两阶段锁 Growing→Shrinking 转换及 Shrinking 阶段拒绝新锁/升级、两个事务同时升级的冲突拒绝、正常共享升级排他、升级等待中事务被 Abort 的正确处理、简单和复杂死锁图的 DFS 检测正确性、2 事务和 4 事务的后台死锁检测端到端测试

【待嵌入】bonus之实验6与实验7测试通过

## 5.2 重点测试场景

## 5.2 重点测试说明

**实验 #3 B+ 树压力测试**：b_plus_tree_test 包含 2000 键随机 shuffle 插入→点查所有→shuffle 删除 1000→验证已删不存在、未删存在。100 次独立运行通过率 100%（课程组测试 100/100）。

【待嵌入】复杂的B+树测试通过

**实验 #7 死锁检测测试**：DeadlockDetectionTest1 测试 2 事务交叉锁（E(r0)→E(r1) vs E(r1)→E(r0)），后台检测线程正确中止最年轻事务并唤醒阻塞线程。DeadlockDetectionTest2 测试 4 事务复杂等待图。

**验收演示综合测试**：44 条 SQL 逐条执行——建库建表→建索引→批量 execfile 插入 3 万条→id=29999/id=0 验证首尾→点查→不等值→投影→多条件范围查→主键冲突→unique 约束→索引删除前后对比→更新→删除→删表。全部通过，无崩溃无错误。

【待嵌入】系统启动-能建数据库并切换使用
【待嵌入】建表
【待嵌入】展示初始自动为pk建立的索引与新建索引功能（对name)
【待嵌入】文件系统与minisql顺利联动-从文件批量插入30k条记录
【待嵌入】点查询成立-且因name上面有索引因而速度极快
【待嵌入】不等值查询也可用-查id不为29999也因索引而快
【待嵌入】支持投影与范围查询
【待嵌入】多条件查询-id小于20000-name小于100
【待嵌入】主键与unique的冲突验证通过
【待嵌入】成功把name上的索引删去-同样的点查询大大变慢
【待嵌入】删除与回插验证成功
【待嵌入】update得到验证
【待嵌入】证实成功删除两张表

---

# 第6章 分工说明

本项目为单人独立完成（solo），涵盖全部七个实验的设计、实现、测试和文档撰写。

各实验的课程组测试与自编测试均已通过。实验 #6（Recovery Manager）和实验 #7（Lock Manager）的思考题已在第 3.6.4 节和第 3.7.5 节详细作答。实验 #7（Lock Manager）本身为 Bonus 模块。实验 #1 的 Clock Replacer Bonus 和实验 #2 的 TableHeap 优化 Bonus 未实现。
