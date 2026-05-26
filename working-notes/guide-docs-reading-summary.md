# Guide-Docs 通读认识汇总

## 项目概览

MiniSQL 是一个精简型单用户 SQL 引擎，用于教学实验。支持 integer、char(n)、float 三种基本数据类型，表最多 32 个属性，支持 unique 约束和单属性主键。对主键和 unique 属性自动建立 B+ 树索引。支持 and/or 连接的多条件查询（等值和区间查询）、单条插入、单条或多条删除。采用共享表空间设计，所有数据存放在同一个数据库文件中。

## 系统架构（自上而下）

```
SQL Parser（SQL 解析器，flex/bison 实现）
  → Planner & Executor（查询计划器与执行器，火山模型）
    → DB Storage Engine Instance（数据库存储引擎实例，支持多个）
      ├── Catalog Manager（目录/元数据管理器）
      ├── Index Manager（索引管理器）
      └── Record Manager（记录管理器）
        → Buffer Pool Manager（缓冲池管理器，LRU 替换）
          → Disk Manager（磁盘管理器，位图管理页分配）
            → Database File（物理文件）
```

## 各模块要点

### 1. Disk Manager & Buffer Pool Manager（实验#1）
- **Disk Manager**：通过位图（Bitmap Page）管理数据页的分配与回收，采用 Extent 分区 + Disk Meta Page 的二级管理结构以突破单一位图页的容量限制。维护逻辑页号到物理页号的映射，使上层对页不连续透明。
- **Buffer Pool Manager**：以 Page 对象（4KB）为内存容器，通过 LRUReplacer 管理页替换，支持 FetchPage、NewPage、UnpinPage、FlushPage、DeletePage 等操作。被 Pin 的页不允许替换。

### 2. Record Manager（实验#2）
- 管理 Row、Field、Schema、Column 等对象的序列化/反序列化（用于持久化），使用魔数 MAGIC_NUM 校验。
- 以堆表（TableHeap）形式组织记录：TablePage 之间双向链表连接，采用 Slotted-page Structure 支持不定长记录（但要求不跨页）。
- RowId（64 位）：高 32 位 = page_id，低 32 位 = slot_num，是索引和堆表之间的桥梁。
- 插入采用 First Fit 策略，删除采用逻辑删除（Delete Mask）+ 物理删除（ApplyDelete）。
- 提供 TableIterator 迭代器供上层遍历。

### 3. Index Manager（实验#3）
- 实现基于磁盘的 B+ 树动态索引，只支持 Unique Key。
- 三种结点：BPlusTreePage（基类）、BPlusTreeInternalPage（中间结点，存 m 个键和 m+1 个指针，第一个键为 INVALID）、BPlusTreeLeafPage（叶结点，存 m 个键和 m 个 RowId 值）。
- KeyManager 负责 GenericKey 的序列化/反序列化和比较（通过 key_schema 反序列化后逐 Field 比较）。
- IndexIterator：叶结点组成单向链表，支持顺序遍历，用于范围查询。
- 支持结点分裂、合并（Coalesce）、借用（Redistribute）操作。

### 4. Catalog Manager（实验#4）
- 管理所有表和索引的元信息（TableInfo/IndexInfo），通过 TableMetadata/IndexMetadata 序列化持久化到数据库文件。
- CatalogMeta（存储在 CATALOG_META_PAGE_ID = 0 号页）记录各表和索引元信息所在的页号。
- 提供 CreateTable、GetTable、DropTable、CreateIndex、GetIndex、DropIndex 等上层接口。
- 需注意深拷贝/浅拷贝的内存管理，避免 shared_ptr 二次析构。

### 5. Planner & Executor（实验#5）
- **Parser**（已实现）：flex/bison 生成词法/语法分析器，SQL 解析为语法树（SyntaxNode 结构，child + next 的树形组织）。
- **Planner**（已实现）：遍历语法树，通过 Catalog Manager 校验语义，生成 Statement → PlanNode 执行计划。
- **Executor**（需实现）：火山模型（Iterator Model），Init() + Next() 接口。需实现 5 个算子：
  - SeqScanExecutor：全表顺序扫描，支持谓词过滤
  - IndexScanExecutor：单列索引扫描，求各索引 RowId 集合交集，need_filter 判断是否需要额外 Evaluate
  - InsertExecutor：插入行并更新所有索引，检查 unique 约束
  - UpdateExecutor：先删旧行再插新行，更新索引
  - DeleteExecutor：删除行及对应索引条目
- ExecuteEngine 还需实现 CreateDatabase、DropDatabase、ShowDatabases、UseDatabase、ShowTables、CreateTable、DropTable、ShowIndexes、CreateIndex、DropIndex、Execfile、Quit 等直接执行函数。

### 6. Recovery Manager（实验#6，独立模块）
- 纯内存实现，不涉及日志落盘。
- LogRec（日志记录）、CheckPoint（数据库完整状态快照）、RecoveryManager。
- RedoPhase：从 CheckPoint 开始，按日志类型修改 KvDatabase 和活跃事务列表。
- UndoPhase：对未完成的活跃事务回滚。
- 需实现 Insert、Delete、Update、Begin、Commit、Abort 六种日志的创建函数。

### 7. Lock Manager（实验#7，Bonus 模块）
- LockRequest / LockRequestQueue：管理事务的锁请求队列，用条件变量 cv_ 实现阻塞等待。
- 支持 Shared Lock、Exclusive Lock、Lock Upgrade。
- 支持三种隔离级别：READ_UNCOMMITTED、READ_COMMITTED、REPEATABLE_READ。
- 死锁检测：后台线程定期构建等待图（Wait-for Graph），DFS 确定性找环，中止最年轻事务打破死锁。
- 需实现 LockShared、LockExclusive、LockUpgrade、Unlock、LockPrepare、CheckAbort 等函数。

## 验收流程要点

1. 基本操作演示（创建多个数据库、建表、插入 10 万条记录、全表扫描验证）
2. 点查询、多条件查询与投影、唯一约束冲突检测
3. 索引创建/删除操作及索引加速效果对比（要求建索引后查询变快、删索引后变慢）
4. 更新和删除操作验证
5. 设计思路介绍与模块提问抽查（可能问得很深入）

## 关键技术决策

- **共享表空间**：所有表、索引、目录数据在同一文件，简化管理（存在空隙问题）
- **逻辑页号映射**：上层使用连续逻辑页号，Disk Manager 内部映射到不连续物理页号
- **Slotted-page Structure**：支持不定长记录，表头从左扩展，记录从右扩展
- **B+ 树结点与数据页等大**：均为 PAGE_SIZE（4KB），叉数由 page size 和 key size 计算
- **火山模型执行**：一次一行，内存占用小，函数调用开销大但对教学足够清晰
- **Recovery 和 Lock 模块独立**：降低耦合，避免前置模块问题影响后续模块完成
