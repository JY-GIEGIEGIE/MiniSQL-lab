# MiniSQL 个人详细设计报告

## 概述

本报告汇总 MiniSQL 数据库系统全部七个实验的设计与实现。MiniSQL 是一个精简型单用户 SQL 引擎，支持基本的增删改查操作和 B+ 树索引。系统采用分层架构——底层为磁盘与缓冲池管理，中层为记录管理与索引，上层为查询执行引擎，侧翼为元数据目录、故障恢复与并发控制。

---

## 一、系统架构总览

```
SQL Parser (flex/bison) ── 框架提供
  │
Planner & Executor ── 实验 #5
  │    ├─ 5 个火山模型算子 (SeqScan/IndexScan/Insert/Update/Delete)
  │    └─ DDL 执行函数 (CreateTable/DropTable/CreateIndex...)
  │
Catalog Manager ── 实验 #4
  │    表/索引元数据的持久化与查询
  │    ├─ TableMetadata / IndexMetadata
  │    └─ CatalogMeta (逻辑页 0)
  │
Record Manager ── 实验 #2                Index Manager ── 实验 #3
  │    Row/Field/Schema 序列化              │    磁盘 B+ 树
  │    TableHeap (Slotted-page 堆表)       │    InternalPage / LeafPage
  │    TableIterator                        │    IndexIterator
  │                                        │
Buffer Pool Manager ── 实验 #1
  │    LRUReplacer / 页帧池 / 脏页落盘
  │
Disk Manager ── 实验 #1
       BitmapPage / Extent / 逻辑-物理页映射

Lock Manager ── 实验 #7 (Bonus)
      共享锁/排他锁/升级/死锁检测

Recovery Manager ── 实验 #6
      LogRec / RedoPhase / UndoPhase
```

---

## 二、各实验设计要点

### 2.1 实验 #1：Disk and Buffer Pool Manager

**核心设计**：
- **BitmapPage**：位图管理单个 Extent 内最多 32704 个数据页的分配状态。采用双层循环（字节级剪枝 + 位级检测），通过 `next_free_page_` 加速下次分配
- **DiskManager**：多 Extent 结构突破单一位图容量限制。DiskFileMetaPage 位于物理页 0，每个 Extent = 1 张 BitmapPage + BITMAP_SIZE 个数据页。逻辑页号到物理页号的映射公式 `physical = logical + logical/BITMAP_SIZE + 2`，其中 +1 为 Meta Page，+1 为 BitmapPage
- **LRUReplacer**：维护淘汰候补队列，`std::list` + `std::unordered_set` 协同，Pin/Unpin/Victim 操作遵循最近最少使用原则
- **BufferPoolManager**：两级槽位调度（free_list_ + LRU Replacer），Page 对象的 pin_count_ 和 is_dirty_ 跟踪使用状态。NewPage 必须先找槽位后分配磁盘页（避免缓冲池满时页号泄漏）

**关键 bug**：MapPageId 常数写为 +1（漏了 Meta Page）；NewPage 顺序颠倒导致页号泄漏；TryToFindFreePage 漏 return 导致劣化。

**实现文件**：`src/page/bitmap_page.cpp`, `src/storage/disk_manager.cpp`, `src/buffer/lru_replacer.cpp`, `src/buffer/buffer_pool_manager.cpp`

### 2.2 实验 #2：Record Manager

**核心设计**：
- **序列化体系**：Column → Schema → Row 三级递进。Column 格式为 32B 定长头 + 可变长列名；Schema 为 Column 序列的递归包装；Row 为字段数 + ceil(N/8) 字节 null 位图 + 逐字段序列化数据
- **Slotted-page 堆表**：TablePage 继承 Page，24B 固定头 + 可变 Slot 数组 + 记录数据（高地址向低地址生长）。插入时 FreeSpacePointer 左移，删除通过标记 DELETE_MASK 逻辑删除，ApplyDelete 压实回收
- **TableHeap**：First Fit 插入策略，沿 TablePage 双向链表查找。更新采用原地优先 + 退化为删旧插新。TableIterator 沿 NextPageId 跨页遍历
- **Row 序列化**：两趟扫描——第一趟写 header 和 null 位图，第二趟写非 null 字段数据。null 字段在 body 中不占空间。反序列化前 `destroy()` 清空以支持重复反序列化

**关键 bug**：Column 反序列化未按类型分支（char 走三参数构造，int/float 走两参数）；Row 位图误用字节而非位；InsertTuple 在 for 条件里重复 Fetch 导致页泄漏。

**实现文件**：`src/record/column.cpp`, `src/record/schema.cpp`, `src/record/row.cpp`, `src/storage/table_heap.cpp`, `src/storage/table_iterator.cpp`

### 2.3 实验 #3：Index Manager

**核心设计**：
- **BPlusTreePage 基类**：28B 固定头（page_type_, key_size_, size_, max_size_, parent_page_id_, page_id_），通过 `reinterpret_cast` 映射到 Page::data_
- **InternalPage**：键值对 `key_size + sizeof(page_id_t)` 排列，KeyAt(0) 始终 INVALID（指针比键多一个）。Lookup 为二分查找（`lft <= rht`，返回 `ValueAt(lft-1)`）
- **LeafPage**：32B 头（含 next_page_id_ 实现叶子链表），键值对从 index=0 全部有效。KeyIndex 为二分查找第一个 `KeyAt(i) >= key` 的位置
- **BPlusTree**：Insert 通过 StartNewTree → InsertIntoLeaf → Split → InsertIntoParent 递归向上分裂。Remove 通过 FindLeafPage → RemoveAndDeleteRecord → while 循环逐级处理 underflow（Redistribute 或 Merge）。Merge 遵循 CMU 15-445 的"始终右合入左、删除右兄弟"约定
- **max_size 溢出保护**：构造时公式额外减 1，确保 Insert 时的临时溢出不写出 data_ 边界腐化相邻 Page::pin_count_

**关键 bug**：Merge 方向不一致（index==0 时左合入右 → 父键偏移错误导致 80+ 无关 key 丢失）；InternalPage::Redistribute 父键更新用 KeyAt(1) 而非子树最小键；max_size 未留溢出空间导致内存腐化。

**实现文件**：`src/page/b_plus_tree_page.cpp`, `src/page/b_plus_tree_internal_page.cpp`, `src/page/b_plus_tree_leaf_page.cpp`, `src/index/b_plus_tree.cpp`, `src/index/index_iterator.cpp`

### 2.4 实验 #4：Catalog Manager

**核心设计**：
- **三层持久化**：CatalogMeta（逻辑页 0）记录 `(table_id, index_id) → page_id` 的映射关系；每表和每索引的元数据各自独占一个数据页
- **CatalogManager 构造**：init=true 时创建空 CatalogMeta；init=false 时从页 0 反序列化 + 遍历 LoadTable/LoadIndex 逐一恢复
- **CreateTable 流程**：DeepCopySchema → TableHeap::Create → NewPage 存 TableMetadata → TableInfo 组装 → 登记 → FlushCatalogMetaPage。自动为 unique 列和主键创建索引
- **BPlusTree 根号恢复**：构造时从 IndexRootsPage 读取已有 `root_page_id_`（初始为空树则不设置）

**关键 bug**：BPlusTree 构造不加载根号导致重启后索引为空；DropTable 后 index_names_ 残留空条目导致 GetIndex 返回码错误。

**实现文件**：`src/catalog/catalog.cpp`, `src/catalog/indexes.cpp`, `src/include/catalog/indexes.h`

### 2.5 实验 #5：Planner and Executor

**核心设计**：
- **DDL 执行**：ExecuteCreateTable 解析语法树（kNodeColumnDefinitionList）构建 Column 和 Schema → 调 CatalogManager。Execfile 复用 Parser 的 MinisqlParserInit/MinislGetParserRootNode 逐行执行
- **五个火山模型算子**（框架已实现）：SeqScanExecutor（TableIterator + 谓词 Evaluate）、IndexScanExecutor（多索引扫描取 RowId 交集）、InsertExecutor（子 executor 拉取 + 唯一检查 + 全部索引更新）、UpdateExecutor（GenerateUpdatedTuple + 删旧插新索引）、DeleteExecutor（RemoveEntry + MarkDelete）
- **ExecuteEngine::Execute 主流程**（框架已实现）：根据语法树类型分发 DDL 或走 Planner → Executor 树 → Init → while Next 拉取 → ResultWriter 输出

**实现文件**：`src/executor/execute_engine.cpp`（仅 DDL 函数部分）

### 2.6 实验 #6：Recovery Manager

**核心设计**：
- **LogRec**：含 type_、lsn_、prev_lsn_、txn_id_ 及操作数据。静态 `next_lsn_` 和 `prev_lsn_map_` 管理全局 LSN 分配和事务内链
- **RedoPhase**：从 checkpoint 的 data_ 加载初始状态 → 按 LSN 顺序重放所有日志 → 遇到 Abort 时沿 prev_lsn_ 链回滚该事务全部操作
- **UndoPhase**：对 RedoPhase 后仍在 active_txns_ 的事务（即从未 Commit/Abort 的悬空事务），沿 prev_lsn_ 链回滚
- **关键细节**：RedoPhase 中每遇日志更新 `active_txns_[txn_id] = lsn`（确保 UndoPhase 从最新 LSN 回溯）；Abort 日志不清除 active_txns_（留给 RedoPhase 内置的回滚处理）

**实现文件**：`src/recovery/recovery_manager.cpp`, `src/include/recovery/log_rec.h`

### 2.7 实验 #7：Lock Manager (Bonus)

**核心设计**：
- **LockPrepare/CheckAbort**：两阶段锁的前置校验（非 Growing 禁止加锁）和等待期间的中止检查
- **共享/排他/升级**：LockShared 等待 `!is_writing && !is_upgrading`；LockExclusive 等待 `sharing_cnt == 0 && !is_writing`；LockUpgrade 等待 `sharing_cnt == 1`（只剩自己）。升级成功前保持 shared_lock_set 记录（确保 Abort 时 ReleaseLocks 能找到）
- **死锁检测**：后台线程定期构建等待图（遍历 lock_table_ 中所有未授予请求 → 已授予持有者边）→ 确定性 DFS 找环（最低 ID 优先探索）→ 中止最年轻事务 → 通知所有 cv 唤醒等待者
- **Unlock**：首次解锁将 txn 从 Growing 迁至 Shrinking（两阶段锁协议）

**实现文件**：`src/concurrency/lock_manager.cpp`

---

## 三、关键设计决策汇总

| 决策 | 涉及实验 | 说明 |
|------|----------|------|
| reinterpret_cast 页面映射 | #1, #3 | BitmapPage 和 BPlusTreePage 均为标准布局类型，可直接将 Page::data_ 解释为对象 |
| max_size 减 1 溢出保护 | #3 | Insert 后临时 max+1 不写出 data_ 边界腐化 Page 成员 |
| 合并方向统一（右合入左） | #3 | CMU 15-445 约定，避免 Parent::Remove 后键偏移不一致 |
| DeepCopy vs ShallowCopy | #4 | 表元数据持 DeepCopy（独立生命周期），索引 key_schema_ 用 ShallowCopy（共享 Column） |
| 先槽位后分配 | #1 | NewPage 避免缓冲池满时磁盘页号泄漏 |
| BITMAP_SIZE × 8 位 | #1 | 4088 字节 × 8 = 32704 数据页/Extent |
| 两趟序列化 | #2 | Row 先写位图再写字段——null 字段在 body 中不占空间 |

---

## 四、测试覆盖

全部 7 个实验的课程组测试均 100% 通过。自编测试额外覆盖边界条件、错误路径和复杂交互场景。测试统计：

| 实验 | 课程测试 | 自编测试 |
|------|----------|----------|
| #1 | 4 | 22 |
| #2 | 3 | 19 |
| #3 | 4 | 3 |
| #4 | 3 | 3 |
| #5 | 4 | 0 |
| #6 | 1 | 0 |
| #7 | 10 | 0 |
| **总计** | **29** | **47** |

---

## 五、开发经验总结

1. **页面内存布局的安全性**：实验 #1 和 #3 中，Page::data_ 与 C++ 对象的 reinterpret_cast 映射是高性能但高风险的——溢出写入会直接腐化相邻对象（page_id_、pin_count_），且表现为难以定位的随机崩溃。解决方案是严格计算容量并预留溢出空间。

2. **B+ 树删除的复杂度远超插入**：实验 #3 是最耗时的模块。插入只需处理向上的递归分裂，而删除需要处理 Leaf/Internal 两层的 Redistribute/Merge 决策、父键同步更新、递归向上的级联合并。最终通过参考 CMU 15-445 的合并方向约定修复了密钥丢失问题。

3. **两阶段锁协议的正确实现**：实验 #7 中 LockUpgrade 升级成功前不能提前移除 shared_lock_set 条目——否则 Abort 回调的 ReleaseLocks 遍历锁集合时找不到该锁，导致等待线程永久阻塞。

4. **序列化一致性的重要性**：实验 #2 的 Column/Schema/Row 互相依赖的序列化格式必须精确一致——一处 `sizeof(TypeId)` 的跨平台差值会导致整个 Schema 反序列化失败。
