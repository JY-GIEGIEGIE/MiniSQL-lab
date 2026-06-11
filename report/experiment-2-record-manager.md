# 实验 #2：Record Manager 设计报告

## 一、模块概述

### 1.1 模块定位

Record Manager 负责 MiniSQL 中数据记录（行）的存储、序列化与堆表管理。它是 Catalog Manager 和 Index Manager 的下层依赖，也是 Executor 执行 SQL 语句时直接调用的数据存取层。

本模块涵盖两个子任务：数据对象的序列化与反序列化（Column、Schema、Row 的字节流转换）和堆表操作（以 TableHeap 为核心，支持记录的插入、更新、删除、查询和遍历）。

### 1.2 类型体系

从最小粒度到最大粒度，四个核心类型构成记录管理的类型体系：

- **Column**：表的一列的定义，包含列名、类型（int/float/char）、长度（char 类型的最大字节数）、是否可空、是否唯一
- **Schema**：一组 Column 的集合，描述一张表的结构。通过 `GetColumn(i)->GetType()` 提供每列的类型信息
- **Field**：一行记录中某一列的实际值，存储为 union（int32_t/float_t/char*），支持 null 语义。Field 的序列化/反序列化已由 Type 体系实现
- **Row**：一行完整记录，由多个 Field 组成，附带一个 RowId（64 位标识，高 32 位 page_id + 低 32 位 slot_num）

### 1.3 实现范围

完成 Column、Schema、Row 的 `SerializeTo`、`GetSerializedSize`、`DeserializeFrom` 共计九个函数。完成 TableHeap 的构造、`InsertTuple`、`UpdateTuple`、`ApplyDelete`、`GetTuple`、`Begin`、`End` 共计七个函数。完成 TableIterator 的全部八个运算符。在课程组提供的 3 个测试用例外，自行编写 19 个测试用例覆盖边界条件。全部 22 个测试用例通过。

---

## 二、序列化格式设计

### 2.1 Column 序列化格式

| 偏移 (B) | 大小 (B) | 字段 | 说明 |
|-----------|----------|------|------|
| 0 | 4 | MAGIC_NUM | 固定值 210928，反序列化时校验 |
| 4 | 4 | name_size | 列名的字节长度 |
| 8 | name_size | name | 列名字符串 |
| 8+name_size | 4 | type | 类型枚举值（int=1, float=2, char=3）转为 uint32_t |
| 12+name_size | 4 | len | char 类型的最大长度；int/float 为 sizeof(int32_t)/sizeof(float_t) |
| 16+name_size | 4 | table_ind | 该列在表中的位置索引 |
| 20+name_size | 4 | nullable | 是否可空（1=是，0=否） |
| 24+name_size | 4 | unique | 是否唯一（1=是，0=否） |

总大小：32 + name_size 字节。所有整型字段统一使用 uint32_t 以保证跨平台一致性。

### 2.2 Schema 序列化格式

| 偏移 (B) | 大小 (B) | 字段 |
|-----------|----------|------|
| 0 | 4 | MAGIC_NUM（200715） |
| 4 | 4 | column_count |
| 8 | 可变 | column_0 序列化数据 |
| ... | ... | ... |
| 可变 | 可变 | column_{N-1} 序列化数据 |

### 2.3 Row 序列化格式

Header：

| 偏移 (B) | 大小 (B) | 字段 |
|-----------|----------|------|
| 0 | 4 | field_count（N） |
| 4 | ceil(N/8) | null_bitmap（bit i=1 表示字段 i 为 null） |

Body：依次为每个非 null 字段的序列化数据（null 字段在 body 中不占空间）。Field 的序列化格式由 TypeInt/TypeFloat/TypeChar 各自定义：

- TypeInt：非 null 时写 4 字节 int32_t；null 时写 0 字节
- TypeFloat：非 null 时写 4 字节 float_t；null 时写 0 字节
- TypeChar：非 null 时写 4 字节长度前缀 + 实际字符数据；null 时写 0 字节

### 2.4 序列化格式设计原则

1. **MAGIC_NUM 校验**：Column 和 Schema 在数据流头部包含魔数，反序列化时首先校验，防止将损坏或错误类型的数据解释为 Column/Schema 对象。

2. **定长优先**：除字符串外，所有字段统一为 uint32_t（4 字节），消除 sizeof(bool) 和 sizeof(TypeId) 的平台差异性。

3. **null 位图**：Row 使用紧凑位图（每字段 1 bit）标记空值，而非逐字段标记或每个 null 占一个字节，以最小化存储开销。

4. **委托反序列化**：Row 的反序列化将单个 Field 的反序列化委托给 `Field::DeserializeFrom`，利用已有的类型系统处理三种数据类型的格式差异。

---

## 三、TablePage 的 Slotted-page 结构

TablePage（继承自 Page）是堆表的基本存储单元，采用经典的 Slotted-page 结构。一页 4096 字节分为三部分：

**Header（从低地址向高地址扩展）**：24 字节固定表头（page_id、LSN、PrevPageId、NextPageId、FreeSpacePointer、TupleCount），之后是可变长度的 slot 数组，每个 slot 占 8 字节（4 字节 offset + 4 字节 size）。

**Free Space（中间区域）**：空闲空间，Header 和 Tuples 之间的未用区域。

**Tuples（从高地址向低地址生长）**：已插入的记录数据，紧凑排列在页尾。插入时 FreeSpacePointer 左移，数据写入新位置。删除时执行压实操作（memmove），消除空洞。

通过 RowId（高 32 位 page_id + 低 32 位 slot_num）可在常数时间内定位到任意记录。页之间通过双向链表（PrevPageId/NextPageId）连接，构成堆表的物理存储。

---

## 四、TableHeap 的算法设计

### 4.1 InsertTuple：First Fit 策略

记录插入采用 First Fit 分配策略。从 first_page_id_ 开始沿 NextPageId 链表逐页尝试调用 TablePage::InsertTuple。每页最多被 Fetch 一次，操作完成后立即 Unpin，避免页泄漏。

若当前页装不下，记录 prev_page_id 后去下一页。若链表走完仍未找到足够空间的页，通过 BufferPoolManager::NewPage 分配新页，初始化 TablePage（设置 page_id、PrevPageId、NextPageId=INVALID、FreeSpacePointer=PAGE_SIZE），若 prev_page_id 有效则更新其 NextPageId 建立链表连接。在新页上执行插入。

**边界条件**：（1）插入前检查记录序列化大小是否超过 `TablePage::SIZE_MAX_ROW`（= PAGE_SIZE - 表头 24B - 1个slot 8B），超过则直接拒绝；（2）NewPage 调用可能因缓冲池满而失败，返回 false。

**时间复杂度**：最坏情况需遍历所有已有页 + 创建新页。通常受益于 First Fit 的"首页即命中"特性。

### 4.2 UpdateTuple：原地更新优先，退化为删旧插新

更新策略分为两阶段，优先原地更新以保持 RowId 稳定：

**阶段一（原地更新）**：Fetch 目标页，调用 TablePage::GetTuple 确认旧记录存在且未被删除。调用 TablePage::UpdateTuple 执行原地更新——若新记录大小不超过旧页空闲空间（含旧记录自身占用的可回收空间），数据在页内移动并更新 slot offset；否则返回 false。

**阶段二（删旧插新）**：原地空间不足时，依次调用 MarkDelete（标记逻辑删除，设置 DELETE_MASK）、ApplyDelete（物理删除，压实回收空间）、InsertTuple（将新记录插入有足够空间的位置，可能在其他页或新建页）。调用者通过 `row.GetRowId()` 获取新的 RowId。

### 4.3 删除：MarkDelete + ApplyDelete 两段式

MarkDelete 为逻辑删除——将目标 slot 的 size 字段最高位置 1（DELETE_MASK），记录在遍历时被跳过但不回收空间。支持 RollbackDelete 回滚——清除 DELETE_MASK 恢复可见性。

ApplyDelete 为物理删除——真正回收空间。通过 memmove 将被删记录之后的数据向高地址移动，更新所有受影响 slot 的 offset 值，回收 FreeSpacePointer。

两段式设计支持事务语义——事务执行期间仅 MarkDelete（其他事务不可见），Commit 时 ApplyDelete（物理回收），Abort 时 RollbackDelete（恢复）。

### 4.4 迭代器：TableIterator 的跨页遍历

TableIterator 持有 TableHeap 指针、当前 Row 的深拷贝和事务上下文。Begin 从 first_page_id_ 开始，逐页调用 TablePage::GetFirstTupleRid 查找第一个未被删除的记录。End 返回 row_ 为 nullptr 的哨兵迭代器。

前缀递增（++iter）的逻辑采用三级回退：
1. 同页内：调用 TablePage::GetNextTupleRid 查找下一个未被删除的 slot
2. 跨页：若同页无更多记录，沿 NextPageId 逐页调用 GetFirstTupleRid
3. 结束：若链表走完，置 row_ = nullptr（变成 End）

---

## 五、测试方案与结果

### 5.1 课程组测试

| 测试套件 | 用例数 | 覆盖内容 |
|----------|--------|----------|
| TupleTest | 2 | Field 序列化 roundtrip（int/float/char/null）、Row 通过 TablePage 的完整读写流程 |
| TableHeapTest | 1 | 10000 行插入 + 按 RowId 逐行回读 + 逐字段值比较 |

### 5.2 自行编写的测试

| 测试套件 | 用例数 | 覆盖内容 |
|----------|--------|----------|
| ColumnStudentTest | 4 | int/char/float 三种类型的 Column 独立 serializ roundtrip；MAGIC_NUM 校验失败时返回 0 且不修改出参 |
| SchemaStudentTest | 1 | 三列 Schema（int+char+float）的完整 roundtrip，验证每列的元数据恢复正确 |
| RowStudentTest | 4 | 全 null 行、混合 null 与非 null、空字符串 char 字段、SerializeTo 返回值与 GetSerializedSize 的一致性 |
| TableHeapStudentTest | 10 | MarkDelete→RollbackDelete 回滚链路、MarkDelete→ApplyDelete 物理删除后不可读、UpdateTuple 原地更新、UpdateTuple 删旧插新且 RowId 变化、迭代器跨页遍历、空表 Begin==End、迭代器跳过已删除行、超长记录拒绝、非法 page_id 的 GetTuple/UpdateTuple、后缀递增 iter++ |

### 5.3 测试结果

编译环境：g++ 11.4.0，cmake 3.28.1，Debug 模式。全部 22 个测试用例通过，0 失败。

---

## 六、遇到的问题与解决方案

| 序号 | 问题 | 原因 | 解决方案 |
|------|------|------|---------|
| 1 | DeserializeFrom 对 int/float 列崩溃 | 总是调用 char 专用的三参数构造函数 | 按 type 分支：char 走三参数，其他走两参数 |
| 2 | Column 序列化大小不可控 | sizeof(TypeId) 和 sizeof(bool) 依赖平台 | 统一转为 uint32_t 读写 |
| 3 | Schema 反序列化返回值偏大 | 返回值 double-count header 的 8 字节 | 直接返回 GetSerializedSize() |
| 4 | Row 全 null 行反序列化失败 | 位图实现用字节而非位，mark null 时以字节为单位移动 bitmap_ 指针 | 改用 byte_idx/bit_idx 按位操作 |
| 5 | Row 序列化体过大 | 每个 Field 前额外写了 size 前缀 | 去除前缀，依赖 Field 自描述格式 |
| 6 | DeserializeFrom 断言过严 | `ASSERT(fields_.empty())` 不支持重复反序列化 | 改为 `destroy()` 清空 |
| 7 | InsertTuple 页泄漏 | for 循环条件中 Fetch 了一次，体内又 Fetch 一次，第一次的指针未 Unpin | 改为 while 循环，每页只 Fetch 一次 |
| 8 | InsertTuple 不建新页 | 所有页满时直接返回 false | 添加 NewPage 分支 |
| 9 | 新建表未创建第一页 | 构造函数内有 `ASSERT(false)` 占位 | 实现 NewPage + Init 逻辑 |
| 10 | 测试 InsertTooLargeRow 失败 | VARCHAR_MAX_LEN = 2048，测试用 len=4000 触发了 Field 构造断言 | 改由两个 char(2047) 字段合计超出 SIZE_MAX_ROW |
