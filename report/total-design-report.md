# MiniSQL 设计总报告

组员：单人完成  
课程：数据库系统原理  

---

## 目录

| 章节 | 标题 |
|------|------|
| 第1章 | MiniSQL 总体框架 |
| 第2章 | 内部数据结构 |
| 第3章 | 各模块设计 |
| 第4章 | 各模块接口 |
| 第5章 | 系统测试 |
| 第6章 | 分工说明 |

---

# 第1章 MiniSQL 总体框架

## 1.1 实现功能

MiniSQL 是一个精简型单用户 SQL 引擎，支持通过字符界面输入 SQL 语句实现以下功能：

1. 数据库管理：CREATE/DROP DATABASE，USE，SHOW DATABASES
2. 表管理：CREATE/DROP TABLE，SHOW TABLES
3. 索引管理：CREATE/DROP INDEX，SHOW INDEXES
4. 数据操作：INSERT、DELETE、UPDATE、SELECT
5. 数据类型：INT、CHAR(n)、FLOAT
6. 表定义：最多 32 个属性，支持 UNIQUE 约束和单属性主键
7. 索引：对主键自动建立 B+ 树索引；对 UNIQUE 属性自动建立 B+ 树索引；用户可手动创建/删除索引
8. 查询：支持 AND/OR 连接的多条件查询，支持等值查询和区间查询
9. 批量执行：支持 execfile 执行 SQL 脚本文件
10. 并发控制（Bonus）：支持共享锁/排他锁和死锁检测的 Lock Manager
11. 故障恢复（Bonus）：支持 Redo/Undo 两阶段恢复的 Recovery Manager

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

各层之间通过明确的接口调用。上层的 Planner/Executor 通过 Catalog Manager 获取表结构信息，通过 Record Manager 和 Index Manager 读写数据；Record/Index Manager 通过 Buffer Pool Manager 透明访问磁盘页；Buffer Pool Manager 通过 Disk Manager 完成物理 I/O。

Lock Manager 和 Recovery Manager 是相对独立的模块——前者为事务提供锁管理，后者提供崩溃恢复能力。

## 1.3 设计语言与运行环境

- 编程语言：C++11
- 构建工具：CMake 3.28.1
- 编译器：g++ 11.4.0
- 第三方库：glog（日志），googletest（测试框架），flex/bison（词法语法分析）
- 运行环境：Ubuntu 22.04 LTS (WSL2)，64 位
- 数据页大小：PAGE_SIZE = 4096 字节

---

# 第2章 内部数据结构

## 2.1 数据页（Page）

每个数据页为 4096 字节，是内存与磁盘交互的基本单位。Page 对象是 BufferPoolManager 中页帧池的基本单元：

```
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
物理页 2~N:   Extent 0 的 32704 个数据页
物理页 N+1:   Extent 1 的 BitmapPage
...
```

每个 Extent = 1 张 BitmapPage + BITMAP_SIZE（= `(4096 - 8) × 8 = 32704`）个数据页。逻辑页号从 0 开始连续编号，只给数据页编号（MetaPage 和 BitmapPage 不占逻辑页号）。逻辑页号到物理页号的映射公式：

```
physical = logical + logical / BITMAP_SIZE + 2
```

其中 +1 跳过 MetaPage，再 +1 跳过当前 Extent 的 BitmapPage。

## 2.3 RowId（行标识符）

RowId 为 64 位整数，同时具有逻辑和物理意义：

```
| page_id (32 bit) | slot_num (32 bit) |
```

- `page_id`：记录所在的逻辑页号
- `slot_num`：记录在该页的 Slot 数组中的下标

RowId 是索引 B+ 树叶结点的 Value，也是 TableHeap 记录定位的凭据——通过 `page_id` 找到页，通过 `slot_num` 找到页内偏移。

## 2.4 TablePage—Slotted-page 结构

TablePage 是堆表的基本存储单元，继承 Page，采用经典的 Slotted-page 布局：

```
低地址 → 高地址
| HEADER (24B) | SLOT数组 (8B×N) | ... FREE SPACE ... | Tuple N-1 | ... | Tuple 0 |
                                   ← FreeSpacePointer
```

Header 包含：page_id（4B）、LSN（4B）、PrevPageId（4B）、NextPageId（4B）、FreeSpacePointer（4B）、TupleCount（4B）。

每个 Slot 占 8 字节：4B 偏移量 offset + 4B 大小 size。逻辑删除通过将 size 的最高位（DELETE_MASK = `1 << 31`）置 1 标记，ApplyDelete 时通过 memmove 压实回收空间。

## 2.5 Row/Field/Schema 序列化格式

**Column 序列化**（32 + name.size() 字节）：

```
| MAGIC_NUM(4B) | name_size(4B) | name(variable) | type(4B) | len(4B) | table_ind(4B) | nullable(4B) | unique(4B) |
```

**Schema 序列化**：

```
| MAGIC_NUM(4B) | column_count(4B) | column_0 | ... | column_{N-1} |
```

**Row 序列化**：

```
| field_count(4B) | null_bitmap(ceil(N/8)B) | field_0_data | ... | field_{N-1}_data |
```

null 位图：bit i = 1 表示字段 i 为 null。null 字段在 body 中不占空间。Field 的序列化委托给 Type 子类：

- TypeInt：非 null 写 4B int32_t
- TypeFloat：非 null 写 4B float_t  
- TypeChar：非 null 写 4B 长度前缀 + 实际字符数据

## 2.6 B+ 树结点结构

### BPlusTreePage 基类（28B 头）

```
| PageType(4B) | KeySize(4B) | LSN(4B) | CurrentSize(4B) | MaxSize(4B) | ParentPageId(4B) | PageId(4B) |
```

### InternalPage（内部结点）

继承 28B 头，剩余空间存储键值对数组。每对 = `key_size + sizeof(page_id_t)` 字节。第一个键（KeyAt(0)）始终 INVALID——指针比键多一个，用无效键占位实现统一索引。

对于输入 key K 的路由：若 `KeyAt(i) <= K < KeyAt(i+1)` 则走 `ValueAt(i)`；`K < KeyAt(1)` 走 `ValueAt(0)`；`K >= KeyAt(size-1)` 走 `ValueAt(size-1)`。Lookup 采用二分查找。

### LeafPage（叶结点）

32B 头（比 InternalPage 多 4B 的 `next_page_id_`）。键值对从 index=0 开始全部有效。每对 = `key_size + sizeof(RowId)` 字节。`next_page_id_` 将所有叶子串成单向链表，支持 IndexIterator 顺序遍历和范围扫描。

### GenericKey 与 KeyManager

GenericKey 为柔性数组 `char data[0]`，实际内存通过 `malloc(key_size_)` 分配。KeyManager 持有索引列的 Schema 和 key_size_，提供 SerializeFromKey（Row→字节流）、DeserializeToKey（字节流→Row）、CompareKeys（逐 Field 比较）。

## 2.7 Catalog 元数据

```
逻辑页 0: CatalogMeta（table_id→meta_page, index_id→meta_page 的映射表）
逻辑页 N: TableMetadata（表名/表ID/根页号/Schema）
逻辑页 M: IndexMetadata（索引名/索引ID/所属表ID/key_map_）
```

每个表和索引的元数据独占一个数据页。CatalogMeta 的序列化格式：

```
| MAGIC_NUM(4B) | table_count(4B) | index_count(4B) | N×(table_id+page_id)(8B) | M×(index_id+page_id)(8B) |
```

## 2.8 Lock Manager 数据结构

```
lock_table_: unordered_map<RowId, LockRequestQueue>
LockRequestQueue:
  req_list_: list<LockRequest>          // 等待队列
  cv_: condition_variable               // 条件变量
  is_writing_: bool                     // 是否有排他锁
  sharing_cnt_: int                     // 共享锁计数

waits_for_: unordered_map<txn_id, set<txn_id>>  // 死锁检测等待图
```

---

# 第3章 各模块设计

## 3.1 Disk Manager

### 3.1.1 BitmapPage

管理单个 Extent 内最多 32704 个数据页的分配状态。4096 字节中，前 8 字节为 `page_allocated_` 和 `next_free_page_`，后 4088 字节为位图本体（`bytes[]` 数组），每 bit 对应一个数据页（0=空闲，1=已分配）。

- `AllocatePage(&page_offset)`：外层遍历 4088 字节，字节满（0xFF）则剪枝跳过；内层逐位查找空位，置 1 后通过出参返回偏移量。更新 `next_free_page_` 加速下次查找
- `DeAllocatePage(page_offset)`：位运算清零对应 bit，若释放位置比 `next_free_page_` 更靠前则回指 hint
- `IsPageFree(page_offset)`：位运算检查对应 bit

### 3.1.2 DiskManager

管理整个数据库文件。持有 DiskFileMetaPage 缓存（meta_data_），记录 Extent 数量和每个 Extent 的已用页数。

- `AllocatePage()`：遍历已有 Extent → 读 BitmapPage → 调 BitmapPage::AllocatePage → 写回 → 更新 MetaPage → 返回逻辑页号。所有 Extent 满则创建新 Extent
- `MapPageId(logical)`：`logical + logical / BITMAP_SIZE + 2`
- `DeAllocatePage` / `IsPageFree`：由逻辑页号反推 Extent 和偏移，委托 BitmapPage

### 3.1.3 LRUReplacer
维护淘汰候补队列（`std::list<frame_id_t>` + `std::unordered_set`）。Pin 从名单移除，Unpin 加入尾部，Victim 弹出头部（最久未用）。

### 3.1.4 BufferPoolManager
持有固定大小 Page 数组。两级槽位调度：优先从 `free_list_` 取空槽位，空槽耗尽则从 LRUReplacer 淘汰。对外提供 FetchPage、NewPage、UnpinPage、FlushPage、DeletePage 接口。

## 3.2 Record Manager

### 3.2.1 序列化体系
Column → Schema → Row 三级递进。Column 格式为 32B 定长头+可变长列名，所有整型字段统一 `uint32_t`。Row 序列化：字段数+null 位图+逐字段自描述数据（Type 子类提供的 SerializeTo）。

### 3.2.2 TableHeap
First Fit 插入策略：沿 TablePage 双向链表逐页尝试 `InsertTuple`，所有页满则 NewPage 创建新页并链接。更新采用"原地优先→退化为删旧插新"。删除采用两段式（MarkDelete 逻辑删除→ApplyDelete 物理压实）。

### 3.2.3 TableIterator
沿 TablePage 链表的顺序遍历器。持有当前 Row 的深拷贝。前缀递增：同页内 GetNextTupleRid → 跨页沿 NextPageId 去下一页 → 链表走完变为 End。

## 3.3 Index Manager

### 3.3.1 B+ 树算法

**Insert**：树空→StartNewTree；非空→FindLeafPage→Insert→若超 max_size→Split→InsertIntoParent 递归向上分裂。根分裂时 PopulateNewRoot 创建新根。

**Remove**：FindLeafPage→RemoveAndDeleteRecord→逐级 while 循环处理 underflow：
- Redistribute（node.size + sibling.size >= max_size）：从 sibling 借一个键值对，更新父键
- Merge（右合入左）：MoveAllTo 搬运全部数据→Parent::Remove 删除右兄弟索引→继续检查 parent underflow

**合并方向约定**：始终右合入左、删除右兄弟（CMU 15-445 参考）。旧版 index==0 时反向合入曾导致父键偏移错误，使同父下其他叶子键路由失效。

**max_size 溢出保护**：构造时额外减 1，确保插入时的临时溢出不写出 data_ 边界腐化相邻 Page 对象。

### 3.3.2 IndexIterator
沿 LeafPage 的 `next_page_id_` 链表顺序遍历。构造时 Fetch 目标叶并 pin，operator++ 跨页时 Unpin 旧页 Fetch 新页。

### 3.3.3 ScanKey 范围查询
BPlusTreeIndex::ScanKey 支持全部比较运算符（=、>、<、>=、<=、<>）。> 和 >= 通过 GetBeginIterator(key) 定位起始位置后遍历至 End；< 和 <= 通过 GetBeginIterator 遍历至 key 位置；<> 遍历全部后 erase 等值结果。结果集合通过 `set_intersection` 取交集支持 AND 复合条件。

## 3.4 Catalog Manager

### 3.4.1 设计
三层持久化：CatalogMeta（逻辑页 0）→ TableMetadata/IndexMetadata（各独占一页）。构造时 init=true 则创建空 CatalogMeta；init=false 则从页 0 反序列化后遍历 LoadTable/LoadIndex 恢复全量信息。

### 3.4.2 关键操作
- CreateTable：DeepCopySchema → TableHeap::Create → NewPage 写 TableMetadata → TableInfo 组装 → 登记 → FlushCatalogMetaPage。自动为 unique 列和主键建 B+ 树索引
- CreateIndex：验证列名有效 → NewPage 写 IndexMetadata → IndexInfo::Init（ShallowCopySchema 构建 key_schema_+CreateIndex 创建 B+ 树）→ 登记 → FlushCatalogMetaPage
- DropTable：级联删除所有索引 → 回收堆表页 → 回收元数据页 → 清除 maps → FlushCatalogMetaPage
- DropIndex：回收元数据页 → 清除 maps → delete IndexInfo → FlushCatalogMetaPage

### 3.4.3 BPlusTree 根号恢复
BPlusTree 构造末尾从 IndexRootsPage 读取已有 `root_page_id_`——新索引 `root_page_id_` 保持 INVALID，已有索引正确恢复根页号。

## 3.5 Planner & Executor

### 3.5.1 整体流程
SQL 文本→Parser 生成语法树→ExecuteEngine::Execute 分发：
- DDL（CREATE/DROP/SHOW）：直接调 ExecuteXxx 函数
- DML（SELECT/INSERT/UPDATE/DELETE）：Planner 生成 PlanNode 树→CreateExecutor 构建 Executor 树→Init→while Next 拉取结果→ResultWriter 格式化输出

### 3.5.2 ExecuteEngine DDL 函数
- ExecuteCreateTable：解析 kNodeColumnDefinitionList→构建 Column 和 Schema→CatalogManager::CreateTable
- ExecuteDropTable：CatalogManager::DropTable
- ExecuteCreateIndex：CatalogManager::CreateIndex（AST 结构：index_name, table_name, column_list）
- ExecuteDropIndex：支持无表名形式（遍历所有表查找索引）
- ExecuteShowIndexes：支持无参数（显示所有表的所有索引）和带表名两种形式
- ExecuteExecfile：逐行读文件→yy_scan_string→yyparse→Execute

### 3.5.3 五个火山模型算子
- SeqScanExecutor：TableIterator + 谓词 Evaluate 过滤 + TupleTransfer 列投影
- IndexScanExecutor：IndexScan 递归处理表达式树（LogicExpression→set_intersection 取交集，ComparisonExpression→ScanKey 调索引）→GetTuple 读完整行→必要时再 Evaluate
- InsertExecutor：子 executor 拉取→唯一约束检查→InsertTuple→全部索引 InsertEntry
- UpdateExecutor：子 executor 拉取→GenerateUpdatedTuple→删旧插新索引→UpdateTuple
- DeleteExecutor：子 executor 拉取→全部索引 RemoveEntry→MarkDelete

## 3.6 Recovery Manager

### 3.6.1 设计
纯内存简化恢复模块。LogRec 含 type_、lsn_、prev_lsn_、txn_id_ 及操作数据。静态 `next_lsn_` 和 `prev_lsn_map_` 管理 LSN 链。

### 3.6.2 恢复流程
1. Init：加载 checkpoint 的 persist_data_ 和 active_txns_
2. RedoPhase：按 LSN 重放所有日志，每遇日志更新 `active_txns_[txn_id]=lsn`；遇 Commit 擦除 txn；遇 Abort 沿 prev_lsn_ 链回滚全部操作后擦除 txn
3. UndoPhase：对 RedoPhase 后仍活跃的事务沿 prev_lsn_ 链逆序回滚

## 3.7 Lock Manager (Bonus)

### 3.7.1 锁管理
全局 `lock_table_[RowId] → LockRequestQueue`。LockPrepare 做两阶段锁校验（非 Growing 拒绝加锁）。LockShared 等待 `!is_writing && !is_upgrading`。LockExclusive 等待 `sharing_cnt==0 && !is_writing`，构建等待边。LockUpgrade 等待 `sharing_cnt==1`（只剩自己），升级前检查 `is_upgrading_` 冲突。Unlock 首次调用将 txn 从 Growing 迁至 Shrinking。

### 3.7.2 死锁检测
后台线程定期构建等待图（遍历 lock_table_ 中所有未授予请求→已授予持有者边）→确定性 DFS 找环（最低 txn_id 优先探索）→中止最年轻事务→通知所有 cv 唤醒。

---

# 第4章 各模块接口

## 4.1 Disk Manager 接口

| 接口 | 说明 |
|------|------|
| `AllocatePage() → page_id_t` | 分配一个新数据页，返回逻辑页号 |
| `DeAllocatePage(page_id_t)` | 释放逻辑页号对应的数据页 |
| `IsPageFree(page_id_t) → bool` | 判断逻辑页号是否空闲 |
| `ReadPage(page_id_t, char*)` | 读取指定逻辑页到内存 |
| `WritePage(page_id_t, const char*)` | 将内存数据写入指定逻辑页 |
| `MapPageId(logical) → physical` | 逻辑页号到物理页号映射 |

## 4.2 Buffer Pool Manager 接口

| 接口 | 说明 |
|------|------|
| `FetchPage(page_id_t) → Page*` | 获取指定页（已在内存直接返回，否则从磁盘读） |
| `NewPage(&page_id_t) → Page*` | 分配新页 |
| `UnpinPage(page_id_t, bool is_dirty)` | 释放页引用，标记脏 |
| `FlushPage(page_id_t) → bool` | 指定页落盘 |
| `DeletePage(page_id_t) → bool` | 删除指定页 |
| `FlushAllPages()` | 全部页落盘 |

## 4.3 Record Manager 接口

| 接口 | 说明 |
|------|------|
| `TableHeap::InsertTuple(Row&, Txn*) → bool` | 插入记录，RowId 写入 Row.rid_ |
| `TableHeap::UpdateTuple(Row&, const RowId&, Txn*) → bool` | 更新记录 |
| `TableHeap::GetTuple(Row*, Txn*) → bool` | 按 RowId 读取记录 |
| `TableHeap::MarkDelete(const RowId&, Txn*) → bool` | 逻辑删除 |
| `TableHeap::ApplyDelete(const RowId&, Txn*)` | 物理删除 |
| `TableHeap::Begin(Txn*) → TableIterator` | 表首迭代器 |
| `TableHeap::End() → TableIterator` | 表尾哨兵迭代器 |
| `Row::SerializeTo(char*, Schema*) → uint32_t` | Row 序列化 |
| `Row::DeserializeFrom(char*, Schema*) → uint32_t` | Row 反序列化 |
| `Column::SerializeTo(char*) → uint32_t` | Column 序列化 |
| `Schema::SerializeTo(char*) → uint32_t` | Schema 序列化 |

## 4.4 Index Manager 接口

| 接口 | 说明 |
|------|------|
| `BPlusTree::Insert(GenericKey*, const RowId&, Txn*) → bool` | 插入键值对（Unique Key） |
| `BPlusTree::Remove(const GenericKey*, Txn*)` | 删除键 |
| `BPlusTree::GetValue(const GenericKey*, vector<RowId>&, Txn*) → bool` | 等值查找 |
| `BPlusTree::Begin() → IndexIterator` | 最左叶子迭代器 |
| `BPlusTree::Begin(const GenericKey*) → IndexIterator` | 指定键起始迭代器 |
| `BPlusTree::End() → IndexIterator` | 尾哨兵迭代器 |
| `BPlusTreeIndex::ScanKey(const Row&, vector<RowId>&, Txn*, string compare_operator) → dberr_t` | 键扫描（支持 =, >, <, >=, <=, <>） |
| `BPlusTreeIndex::InsertEntry(const Row&, RowId, Txn*) → dberr_t` | 插入索引条目 |
| `BPlusTreeIndex::RemoveEntry(const Row&, RowId, Txn*) → dberr_t` | 删除索引条目 |

## 4.5 Catalog Manager 接口

| 接口 | 说明 |
|------|------|
| `CreateTable(table_name, schema, txn, &table_info) → dberr_t` | 创建表 |
| `GetTable(table_name, &table_info) → dberr_t` | 按名查表 |
| `GetTable(table_id, &table_info) → dberr_t` | 按 ID 查表 |
| `GetTables(&tables) → dberr_t` | 获取全部表 |
| `DropTable(table_name) → dberr_t` | 删除表（级联删索引） |
| `CreateIndex(table_name, index_name, index_keys, txn, &index_info, index_type) → dberr_t` | 创建索引 |
| `GetIndex(table_name, index_name, &index_info) → dberr_t` | 查索引 |
| `GetTableIndexes(table_name, &indexes) → dberr_t` | 获取表的所有索引 |
| `DropIndex(table_name, index_name) → dberr_t` | 删除索引 |

## 4.6 Execution Engine 接口

| 接口 | 说明 |
|------|------|
| `Execute(pSyntaxNode) → dberr_t` | 执行语法树（分发 DDL 或走 Planner→Executor） |
| `ExecutePlan(plan, result_set, txn, exec_ctx) → dberr_t` | 执行计划树 |
| `ExecuteCreateDatabase/CreateTable/DropTable/...` | 各 DDL 命令的直接执行函数 |

## 4.7 Lock Manager 接口

| 接口 | 说明 |
|------|------|
| `LockShared(Txn*, const RowId&) → bool` | 获取共享锁 |
| `LockExclusive(Txn*, const RowId&) → bool` | 获取排他锁 |
| `LockUpgrade(Txn*, const RowId&) → bool` | 共享锁升级为排他锁 |
| `Unlock(Txn*, const RowId&) → bool` | 释放锁 |
| `AddEdge/RemoveEdge/HasCycle/RunCycleDetection` | 死锁检测 |

## 4.8 Recovery Manager 接口

| 接口 | 说明 |
|------|------|
| `Init(CheckPoint&)` | 从检查点加载状态 |
| `RedoPhase()` | 重放日志，重建崩溃时刻数据库状态 |
| `UndoPhase()` | 回滚未提交事务 |
| `CreateInsertLog/CreateDeleteLog/CreateUpdateLog/CreateBeginLog/CreateCommitLog/CreateAbortLog` | 日志工厂函数 |

---

# 第5章 系统测试

## 5.1 测试统计

全部 7 个实验的课程组测试均通过。自编测试额外覆盖边界条件、错误路径和复杂交互场景。

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

## 5.2 重点测试场景

**实验 #3 B+ 树压力测试**：2000 键随机 shuffle 插入 → 随机 shuffle 删除 1000 → 点查验证。100 次独立运行全部通过。

**实验 #7 死锁检测测试**：2 事务交叉锁（Exclusive(r0)→Exclusive(r1) vs Exclusive(r1)→Exclusive(r0)），后台死锁检测线程正确中止最年轻事务并唤醒阻塞线程。

**验收演示流程测试**：44 条 SQL 逐条验证，涵盖建库建表→建索引→批量插入 3 万条→点查→范围查→多条件查→约束冲突→索引删除对比→更新→删除→删表，全部通过。

---

# 第6章 分工说明

本项目为单人独立完成（solo），涵盖全部七个实验的设计、实现、测试和文档撰写。实验 #7（Lock Manager）和实验 #6（Recovery Manager）包含思考题，已在各实验设计报告中作答。
