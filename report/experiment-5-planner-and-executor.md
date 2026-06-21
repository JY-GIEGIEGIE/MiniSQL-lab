# 实验 #5：Planner and Executor 设计报告

## 一、模块概述

### 1.1 模块定位

Planner 和 Executor 构成 MiniSQL 的查询处理层——SQL 文本经 Parser 解析为语法树后，Planner 将其转换为执行计划（PlanNode 树），Executor 以火山模型逐行拉取数据执行。本实验中 Planner 由框架完整实现，Executor 的五个核心算子（SeqScan、IndexScan、Insert、Update、Delete）也已在框架中提供完整代码，实际需要学生实现的部分是 ExecuteEngine 中处理 CREATE TABLE、DROP TABLE、CREATE INDEX 等 DDL 语句的解析与执行函数。

### 1.2 整体数据流

```
SQL 文本
  → Parser (flex/bison，框架实现) → 语法树 (SyntaxNode)
    → ExecuteEngine::Execute(ast)
        ├─ DDL (CREATE TABLE, DROP TABLE, ...)
        │     → ExecuteXxx(ast) → CatalogManager (实验 #4) → 直接执行
        │
        └─ DML (SELECT, INSERT, UPDATE, DELETE)
              → Planner (框架实现) → PlanNode 树
                → CreateExecutor → Executor 树
                  → Init() → while Next(&row, &rid) → 结果集
                    → ResultWriter → 格式化输出
```

### 1.3 子模块划分与实现状态

| 子模块 | 职责 | 本实验状态 |
|--------|------|-----------|
| Parser | flex/bison 词法语法分析，生成语法树 | 框架已实现，无需修改 |
| Planner | 语法树→Statement→PlanNode，语义检查 | 框架已实现，无需修改 |
| ExecuteEngine | 主控：分发 DDL/DML，构造 Executor 树，执行并输出结果 | **本实验补全 DDL 函数** |
| SeqScanExecutor | 全表顺序扫描，支持谓词过滤 | 框架已实现 |
| IndexScanExecutor | 索引扫描，多索引取 RowId 交集 | 框架已实现 |
| InsertExecutor | 插入行 + 更新所有索引，检查唯一约束 | 框架已实现 |
| UpdateExecutor | 子 executor 拉取行 → 删旧插新索引 → 原地更新 | 框架已实现 |
| DeleteExecutor | 子 executor 拉取行 → 删除索引条目 → 逻辑删除 | 框架已实现 |
| ValuesExecutor | INSERT 值表达式计算，逐行产出 | 框架已实现 |

---

## 二、ExecuteEngine 的 DDL 函数实现

### 2.1 语法树解析模式

DDL 语句的语法树结构简单——表名/索引名/列名等均为 `kNodeIdentifier` 类型结点，列定义列表为 `kNodeColumnDefinitionList`。解析时沿 `child_` 和 `next_` 链表遍历即可提取所有信息。

### 2.2 ExecuteCreateTable

`CREATE TABLE t1(a int, b char(20) unique, c float, primary key(a, c))` 的语法树结构：

```
kNodeCreateTable
  ├─ child_ → kNodeIdentifier ("t1")          // 表名
  └─ child_->next_ → kNodeColumnDefinitionList
       ├─ child_ → kNodeColumnDefinition       // a int
       │    ├─ child_ → kNodeIdentifier ("a")
       │    └─ child_->next_ → kNodeIdentifier ("int")
       ├─ next_ → kNodeColumnDefinition        // b char(20) unique
       │    ├─ child_ → kNodeIdentifier ("b")
       │    ├─ child_->next_ → kNodeIdentifier ("char")
       │    ├─ child_->next_->next_ → kNodeNumber (20)
       │    └─ ... → kNodeColumnDefinition ("unique")
       ...
```

实现流程：
1. 提取表名 → 遍历 `kNodeColumnDefinitionList` 的 child 链表
2. 对每个 `kNodeColumnDefinition`：提取列名、类型名（int/float/char）、char 列的长度、是否 unique
3. 构造 `vector<Column*>` 和 `Schema`
4. 调用 `CatalogManager::CreateTable`
5. 对每个 `IsUnique()` 的列自动建索引（`"u_" + col_name`），对主键列建索引（`"pk_" + table_name`）

### 2.3 ExecuteDropTable

提取表名 → `CatalogManager::DropTable`。级联删索引由 CatalogManager 内部完成。

### 2.4 ExecuteShowIndexes

提取表名 → `CatalogManager::GetTableIndexes` → 格式化表格输出索引名。

### 2.5 ExecuteCreateIndex

提取表名、索引名、索引列名列表 → 构造 `vector<string> index_keys` → `CatalogManager::CreateIndex(..., "bptree")`。

### 2.6 ExecuteDropIndex

提取表名、索引名 → `CatalogManager::DropIndex`。

### 2.7 ExecuteExecfile

打开文件 → 逐行读取 → 对每行调 `MinisqlParserInit()` → `yy_scan_string` → `yyparse()` → `MinisqlGetParserRootNode()` → `Execute()` → `MinisqlParserFinish()`。

### 2.8 ExecuteQuit

返回 `DB_QUIT`，ExecuteEngine 的调用循环检测到后退出。

---

## 三、五个 Executor 的火山模型设计（框架实现）

### 3.1 火山模型接口

所有 Executor 继承 `AbstractExecutor`，实现两个纯虚函数：

- **Init()**：初始化——从 PlanNode 获取表名，通过 ExecuteContext 拿到 CatalogManager，获取 TableInfo、IndexInfo 等资源
- **Next(Row *row, RowId *rid)**：返回下一行。有数据填 row/rid 返回 true，无数据返回 false

### 3.2 SeqScanExecutor

**Init**：`GetTable(plan_->GetTableName())` 拿 TableInfo → 从 TableHeap 获取 `Begin(transaction)` 迭代器。

**Next**：遍历 TableIterator。若有 predicate（WHERE 条件），调 `predicate->Evaluate(row)` 判断——返回值与 `Field(kTypeInt, 1)` 比较，相等为 true。若输出 Schema 与表 Schema 不同（列投影），通过 `TupleTransfer` 按输出列的 `GetTableInd()` 提取对应字段构造新 Row。

### 3.3 IndexScanExecutor

**Init**：`GetTable` 拿 TableInfo → 调 `IndexScan(predicate)` 获取所有符合条件的 RowId 集合。

**IndexScan(predicate)**：递归解析表达式树：
- `LogicExpression`（AND）：左右子树各递归 IndexScan，对结果 `set_intersection` 取交集
- `ComparisonExpression`（=、<、> 等）：从 predicate 中提取比较值，遍历 `plan_->indexes_` 找到匹配的索引列，调 `index->ScanKey` 拿到 RowId 列表

**Next**：从 `result_` 中用 cursor 逐个取 RowId → `table_heap->GetTuple` 读完整行 → 必要时再 Evaluate（`need_filter_` 为 true 时索引不覆盖所有列） → 返回。

### 3.4 InsertExecutor

**Init**：`GetTable` + `GetTableIndexes` 拿资源。子 executor（ValuesExecutor）Init。

**Next**：从子 executor 拉取一行 → 遍历所有索引检查 unique 约束（`ScanKey` 是否有已有记录） → `table_heap->InsertTuple` 插入 → 遍历所有索引调 `InsertEntry` 更新索引。

### 3.5 UpdateExecutor

**Init**：`GetTable` + `GetTableIndexes`。子 executor（通常为 SeqScanExecutor）Init。

**Next**：从子 executor 拉取旧行 → 调 `GenerateUpdatedTuple(src_row)` 构造新行（遍历 `plan_->GetUpdateAttr()` map，有更新的列取表达式 Evaluate 结果，无更新的列拷贝旧值） → 对每个索引：`RemoveEntry` 删旧键 → `InsertEntry` 插新键 → `table_heap->UpdateTuple(new_row, rid)` 原地更新。

### 3.6 DeleteExecutor

**Init**：`GetTable` + `GetTableIndexes`。子 executor（通常为 SeqScanExecutor）Init。

**Next**：从子 executor 拉取要删的行 → 对每个索引调 `RemoveEntry` → `table_heap->MarkDelete(rid)` 逻辑删除。

---

## 四、与已有模块的接口

| 依赖模块 | 使用方式 |
|----------|----------|
| CatalogManager (实验 #4) | `GetTable`/`GetTableIndexes`/`CreateTable`/`DropTable`/`CreateIndex`/`DropIndex`——所有 Executor 通过它获取表结构 |
| TableHeap (实验 #2) | `InsertTuple`/`UpdateTuple`/`MarkDelete`/`GetTuple`/`Begin`——Executor 的数据操作最终入口 |
| BPlusTreeIndex (实验 #3) | `ScanKey`/`InsertEntry`/`RemoveEntry`——Insert/Update/Delete 时维护索引 |
| BufferPoolManager (实验 #1) | 所有页操作间接依赖，通过 TableHeap 和 BPlusTree 透明使用 |

---

## 五、测试方案与结果

### 5.1 课程组测试

`test/execution/executor_test.cpp` 包含 4 个测试用例，覆盖 SeqScan、IndexScan、Insert、Delete 的基本流程。通过手动构造 PlanNode 并调用 ExecutePlan 验证。

### 5.2 测试结果

全部 4 个测试用例通过。编译环境：g++ 11.4.0，cmake 3.28.1，Debug 模式。

---

## 六、附录：关键语法树结点类型

| 结点类型 | 说明 | 本实验使用 |
|----------|------|-----------|
| kNodeCreateDB / kNodeDropDB / kNodeShowDB / kNodeUseDB | 数据库管理 | 框架已实现 |
| kNodeCreateTable / kNodeDropTable | 建表/删表 | **本实验实现** |
| kNodeCreateIndex / kNodeDropIndex / kNodeShowIndexes | 索引管理 | **本实验实现** |
| kNodeColumnDefinition | 列定义（含 unique 标记） | **本实验实现** |
| kNodeColumnDefinitionList | 列定义列表 | **本实验实现** |
| kNodeIdentifier | 数据库名/表名/列名等标识符 | **本实验实现** |
| kNodeExecFile / kNodeQuit | 批量执行/退出 | **本实验实现** |
