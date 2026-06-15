# 实验 #2：Record Manager 设计报告

## 一、模块概述

### 1.1 模块定位

Record Manager 是 MiniSQL 数据存取层的核心——它负责数据记录（行）的持久化存储与内存表示。上层 Executor 通过 TableHeap 插入、删除、更新、查询记录；下层的 BufferPoolManager 和 DiskManager（实验 #1）为它提供透明的页级 I/O。本模块同时为 Catalog Manager（实验 #4）和 Index Manager（实验 #3）提供 Schema/Row/Field 的序列化基础设施——索引键的构造和比较依赖于本模块定义的 Field 类型系统和 Row 序列化格式。

### 1.2 类型体系与层级架构

从最小的数据类型到最大的存储容器，Record Manager 定义了六层抽象：

```
Type (int/float/char 的读写比较基类)
  ├─ TypeInt / TypeFloat / TypeChar ── 各类型的序列化/反序列化/比较（框架已实现）
  │
Field ── 一行中单个列的值（TypeId + union + null 标记）
  │
Column ── 表的一列的元数据（列名 + TypeId + 长度 + 是否可空/唯一）
  │
Schema ── 若干 Column 的集合，描述整张表或索引的结构
  │
Row ── 一行完整记录（多个 Field + RowId）
  │
TableHeap / TablePage ── 以无序堆形式组织的物理存储容器
  │
TableIterator ── 堆表遍历器
```

上层依赖链：`SQL Executor → TableHeap → TablePage → BufferPoolManager → DiskManager`

下方支撑关系：`Column/Schema/Row/Field 序列化 → Catalog Manager`，`Row/Field 比较 → Index Manager`

### 1.3 子模块划分

| 子模块 | 职责 | 实现状态 |
|--------|------|----------|
| Column | 列的元数据定义、序列化/反序列化 | 本实验实现 |
| Schema | 多列的集合、浅拷贝/深拷贝、序列化/反序列化 | 本实验实现 |
| Row | 一行记录的构造、序列化/反序列化、字段访问 | 本实验实现 |
| Field | 单列值，含 null 语义（已由框架实现，作为参考） | 框架已实现 |
| Type / TypeInt / TypeFloat / TypeChar | 各类型的序列化与比较（已实现，Field 序列化依赖） | 框架已实现 |
| TablePage | 单页内的 Slotted-page 记录管理 | 框架已实现 |
| TableHeap | 跨页的 First Fit 插入、更新、删除、迭代器 | 本实验实现 |
| TableIterator | 沿 TablePage 链表的顺序遍历器 | 本实验实现 |

### 1.4 与已有模块的接口

**实验 #1（Disk and Buffer Pool Manager）**：TableHeap 的所有页操作通过 BufferPoolManager 完成——`NewPage` 创建新 TablePage、`FetchPage` 读取已有页、`UnpinPage(page_id, bool is_dirty)` 释放引用并标记是否被修改、`DeletePage` 回收页。TablePage 的 4096 字节 `data_` 区域通过 BPM 的 Page 对象承载，读写透明。

**实验 #3（Index Manager）**：RowId 作为索引叶结点的 Value（`RowId` 高 32 位 page_id + 低 32 位 slot_num），索引通过 KeyManager 将 Row 序列化为 GenericKey。Field 的比较函数（`CompareEquals`/`CompareLessThan` 等）直接支撑索引键比较。

---

## 二、序列化格式设计

### 2.1 Column 序列化格式

Column 的七个字段被序列化为 32 + name.size() 字节的连续流：

| 偏移 (B) | 大小 (B) | 字段 | 说明 |
|-----------|----------|------|------|
| 0 | 4 | MAGIC_NUM | 固定值 210928，反序列化校验 |
| 4 | 4 | name_size | 列名字符串的字节长度 |
| 8 | name_size | name | UTF-8 列名 |
| 8+name_size | 4 | type | 枚举值（1=int, 2=float, 3=char）显式转为 uint32_t |
| 12+name_size | 4 | len | char 列的最大字节数；int/float 为 sizeof(int32_t)/sizeof(float_t) = 4 |
| 16+name_size | 4 | table_ind | 列在表中的位置索引（0-based） |
| 20+name_size | 4 | nullable | 是否可为 NULL（1=是, 0=否） |
| 24+name_size | 4 | unique | 是否唯一（1=是, 0=否） |

**设计原则**：所有整型字段统一使用 `uint32_t`（4 字节）而非平台相关的 `sizeof(bool)` 或 `sizeof(TypeId)`。列名字符串采用"4 字节长度前缀 + 实际内容"的可变长编码，与 `common/macros.h` 中 `MACH_STR_SERIALIZED_SIZE` 宏一致。

**反序列化时的构造分支**：`kTypeChar` 走三参数构造（传入用户指定的长度），其他类型走两参数构造（长度由构造函数根据类型自动设定——int 为 sizeof(int32_t)=4，float 为 sizeof(float_t)=4）。

### 2.2 Schema 序列化格式

Schema 是 Column 的容器，序列化为"魔数 + 列计数 + 各列序列化数据"的递归结构：

| 偏移 (B) | 大小 (B) | 字段 |
|-----------|----------|------|
| 0 | 4 | MAGIC_NUM（200715） |
| 4 | 4 | column_count |
| 8 | 可变 | column_0 的序列化数据 |
| ... | ... | ... |
| 取决于各列 | 可变 | column_{N-1} 的序列化数据 |

`GetSerializedSize()` 为 `sizeof(uint32_t)*2 + sum(column->GetSerializedSize())`。反序列化时校验 MAGIC_NUM、读列数、循环调 `Column::DeserializeFrom` 逐列恢复，最后 `new Schema(columns, true)` 构造（`is_manage_=true` 表示 Schema 析构时负责 delete 所有 Column 指针）。

### 2.3 Row 序列化格式

Row 的序列化分 Header 和 Body 两部分。Header 包含字段数和 null 位图；Body 依次为每个非 null 字段的序列化数据。

**Header 格式**：

| 偏移 (B) | 大小 (B) | 字段 |
|-----------|----------|------|
| 0 | 4 | field_count（N） |
| 4 | ceil(N/8) | null_bitmap（bit i=1 表示字段 i 为 null） |

**Body 格式**：遍历字段 i = 0..N-1，若 `fields_[i]->IsNull() == false`，写入 `fields_[i]->SerializeTo(buf)` 的结果；null 字段在 body 中不占任何空间。

**Field 的自描述能力**：TypeInt/TypeFloat 各写 4 字节定长数据；TypeChar 写"4 字节长度前缀 + 实际字符数据"。因此 Row 层不需要在 body 中为每个字段额外加 size 前缀——反序列化时 `Field::DeserializeFrom` 的返回值就是该字段占用的字节数。

**null 位图**：使用紧凑的按位编码而非逐字节标记。字段 i 对应 `bitmap[i/8]` 的第 `i%8` 位。序列化时两趟扫描——第一趟标记所有 null 位，第二趟写入非 null 数据。反序列化时先读位图，再逐字段调 `Field::DeserializeFrom(buf, type_id, &field, is_null)`。反序列化前调 `destroy()` 清空已有字段，支持 Row 被多次反序列化。

### 2.4 Field 序列化参考（框架实现）

框架已实现三种类型的 Field 序列化，供 Column/Schema/Row 设计参考：

- **TypeInt::SerializeTo**：若不为 null，`MACH_WRITE_TO(int32_t, buf, value_.integer_)` 写 4 字节
- **TypeFloat::SerializeTo**：若不为 null，`MACH_WRITE_TO(float_t, buf, value_.float_)` 写 4 字节
- **TypeChar::SerializeTo**：若不为 null，先写 4 字节长度 `len`，再 memcpy 实际字符数据。null 时返回 0
- 对应的 `DeserializeFrom` 读取格式一致的字节流，`manage_data=true` 时 Field 构造内 `new char[len]` 深拷贝字符串数据

---

## 三、TablePage 的 Slotted-page 结构

### 3.1 物理布局

TablePage 继承 Page，利用其 `data_[PAGE_SIZE]` 存储实际数据。布局为经典的 Slotted-page：

```
低地址                                                   高地址
| HEADER (24B) | SLOT 数组 (8B×N) | ... FREE SPACE ... | Tuple N-1 | ... | Tuple 0 |
                                    ← FreeSpacePointer (向低地址生长)
```

### 3.2 固定表头（24 字节）

| 偏移 | 字段 | 说明 |
|------|------|------|
| 0 | page_id | 本页逻辑页号 |
| 4 | LSN | 日志序列号 |
| 8 | PrevPageId | 前驱页号（双向链表） |
| 12 | NextPageId | 后继页号 |
| 16 | FreeSpacePointer | 空闲空间起始位置 |
| 20 | TupleCount | 当前 slot 数量 |

### 3.3 Slot 数组

每个 slot 占 8 字节——4 字节 offset（记录在页内的偏移位置）+ 4 字节 size（记录序列化后的字节数）。删除采用逻辑删除——size 最高位（DELETE_MASK = `1 << 31`）置 1 标记。`IsDeleted(size)` 返回 `(size & DELETE_MASK) != 0` 或 `size == 0`。

### 3.4 记录写入与删除

**InsertTuple**：计算 `row.GetSerializedSize(schema)` → 检查剩余空间（`FreeSpacePointer - header - slots >= serialized_size + SIZE_TUPLE`）→ 优先复用已删除的 slot（size==0）→ FreeSpacePointer 左移 → 在 FreeSpacePointer 处写 row → 记录 offset 和 size。

**MarkDelete（逻辑删除）**：将对应 slot 的 size 最高位置 1。后续 `GetTuple` 和 `GetFirstTupleRid`/`GetNextTupleRid` 检查 `IsDeleted` 跳过。

**ApplyDelete（物理删除）**：将删除记录后的数据通过 memmove 往高地址搬移（压实），更新所有受影响 slot 的 offset，回收 FreeSpacePointer。

**UpdateTuple**：原地更新优先——若新记录不大于旧空间，直接在原位覆盖并更新 size/offset。若新记录更大但页内剩余空间足够，通过 memmove 重排实现。若页内空间不足，返回 false 让上层 TableHeap 走"删旧插新"路径。

框架已完整实现 TablePage 的以上全部操作，本实验只需理解其接口语义即可调用。

---

## 四、TableHeap 的算法设计

### 4.1 构造——新建表的第一页

带 Schema 参数的构造：`NewPage` 分配第一页 → `reinterpret_cast<TablePage*>` → `Init(page_id, INVALID_PAGE_ID, log_mgr, txn)` 初始化页头 → `first_page_id_` 记录此页号 → Unpin 标记脏。

带 `first_page_id` 参数的构造（打开已有表）已由框架直接赋值实现。

### 4.2 InsertTuple——First Fit 策略

向堆表插入一条记录，采用 First Fit 分配策略。

1. 若 `row.GetSerializedSize(schema_) > TablePage::SIZE_MAX_ROW`（单页最大记录容量），直接返回 false——不支持跨页
2. 从 `first_page_id_` 开始，沿 `NextPageId` 链表逐页 Fetch 尝试 `page->InsertTuple(row, ...)`
3. 某页插入成功 → 该页 Unpin（脏）→ `row.rid_` 已被 TablePage 内部设置 → 返回 true
4. 所有已有页都装不下 → `NewPage` 分配新页 → Init → 更新原尾页的 `NextPageId` 指向新页 → 在新页上 Insert → Unpin → 返回结果

每页仅 Fetch 一次，用完立即 Unpin，避免页泄漏。时间复杂度在最坏情况下遍历所有页。

### 4.3 GetTuple——按 RowId 定位

RowId 的 `GetPageId()` 获取记录所在页号 → FetchPage → 调 `page->GetTuple(row, schema_, ...)` → TablePage 根据 `GetSlotNum()` 定位 offset → `row->DeserializeFrom` 填充 fields_ → Unpin（不脏，只读）。

### 4.4 MarkDelete / ApplyDelete / RollbackDelete——两段式删除

删除采用两段式设计以支持事务语义：

- **MarkDelete**：逻辑删除，仅将 slot 的 size 最高位置 1。后续遍历与查询跳过此记录，但空间未回收。可通过 RollbackDelete 回滚（清除标记）。
- **ApplyDelete**：物理删除，真正回收空间。Fetch 目标页 → 调 `page->ApplyDelete(rid, ...)` 压实数据、更新 offset → Unpin 脏。
- **RollbackDelete**：撤销 MarkDelete。Fetch 目标页 → `page->RollbackDelete` 清除 DELETE_MASK。

### 4.5 UpdateTuple——原地优先，退化为删旧插新

更新策略分两阶段：

**阶段一——原地更新**：Fetch 目标页 → 读出旧记录确认存在且未删除 → 调 `page->UpdateTuple(row, &old_row, ...)` → 若成功，row.rid_ 保持原 RowId，Unpin 脏 → 返回 true。

**阶段二——删旧插新**：原地空间不足时解除旧页 pin → 依次调用 `MarkDelete(rid)` → `ApplyDelete(rid)` → `InsertTuple(row)`。新记录可能被插入到其他页或新建页，调用者通过 `row.GetRowId()` 获取新 RowId。

### 4.6 Begin / End——迭代器边界

**Begin**：从 `first_page_id_` 逐页沿 `NextPageId` 遍历，每页调 `GetFirstTupleRid` 找第一个未被逻辑删除的记录。找到则构造 `TableIterator(this, rid, txn)`；整表为空则返回 End。

**End**：`TableIterator(nullptr, RowId(INVALID_PAGE_ID, 0), nullptr)`——哨兵迭代器，内部 `row_` 为 nullptr。

---

## 五、TableIterator 设计

### 5.1 数据结构

TableIterator 持有三个私有成员：
- `TableHeap *table_heap_`：所属堆表，用于 `GetTuple` 和 BPM 访问
- `Row *row_`：当前指向的记录（深拷贝，operator* 和 operator-> 返回其引用/指针）
- `Txn *txn_`：事务上下文

### 5.2 构造与生命周期

- 构造：若 `rid` 有效，`new Row(rid)` 后 `table_heap_->GetTuple(row_, txn_)` 加载完整数据；若 rid 为 INVALID_ROWID（End 哨兵），`row_` 置 nullptr
- 拷贝构造和赋值运算符均为深拷贝——`new Row(*other.row_)` 调用 Row 的拷贝构造
- 析构：`delete row_`

### 5.3 遍历逻辑

**operator== / !=**：两迭代器相等当且仅当 `row_` 均为 nullptr（同为 End）或两者 RowId 相同。

**operator\***：返回 `*row_` 的 const 引用。**operator->**：返回 `row_` 指针。

**前缀 ++（++iter）**：
1. 在当前页内调 `GetNextTupleRid` 找下一个未删除的 slot
2. 同页内找到 → 更新 row_ 的 RowId → 重新 `GetTuple` 加载数据 → 返回 *this
3. 同页没有→沿 `NextPageId` 去下一页 → 在新页调 `GetFirstTupleRid` 找第一条有效记录
4. 所有页遍历完毕 → `delete row_; row_ = nullptr`（变为 End）

**后缀 ++（iter++）**：保存 `TableIterator copy(*this)`，调 `++(*this)` 前进，返回拷贝。

---

## 六、测试方案与结果

### 6.1 课程组测试

| 测试套件 | 用例 | 覆盖内容 |
|----------|------|----------|
| TupleTest::FieldSerializeDeserializeTest | 各类型 Field 序列化 roundtrip + null 比较 | TypeInt/TypeFloat/TypeChar 全类型覆盖 |
| TupleTest::RowTest | Row 通过 TablePage InsertTuple → GetTuple → MarkDelete → ApplyDelete | 端到端 row 生命周期 |
| TableHeapTest::TableHeapSampleTest | 10000 行插入 → 逐行回读 → 逐字段值比较 | 大规模数据一致性 |

### 6.2 自编测试

| 测试套件 | 用例数 | 覆盖内容 |
|----------|--------|----------|
| ColumnStudentTest | 4 | 三种类型 Column 独立 roundtrip、MAGIC_NUM 校验失败返回 0 |
| SchemaStudentTest | 1 | 三列 Schema（int+char+float）完整 roundtrip |
| RowStudentTest | 4 | 全 null 行、混合 null/非 null、空字符串 char、SerializeTo 与 GetSerializedSize 一致性 |
| TableHeapStudentTest | 10 | MarkDelete→ApplyDelete 后不可读、RollbackDelete 恢复、UpdateTuple 原地/删旧插新、迭代器跨页/空表/跳过删除/后缀递增、超长记录拒绝、非法 page_id 拒绝 |

### 6.3 测试结果

全部 22 个测试用例通过（课程组 3 + 自编 19）。编译环境：g++ 11.4.0，cmake 3.28.1，Debug 模式。编译仅有 harmless warnings。

---

## 七、遇到的问题与解决方案

| # | 问题 | 原因 | 解决方案 |
|----|------|------|---------|
| 1 | Column 反序列化对 int/float 列崩溃 | 总是调用 char 专用的三参数构造（含 `ASSERT(type==kTypeChar)`） | 按 type 分支：char 走三参数，int/float 走两参数自动设 len |
| 2 | Column 序列化大小跨平台不确定 | `sizeof(TypeId)` 和 `sizeof(bool)` 依赖编译器 | 统一显式转为 `uint32_t`（4 字节）读写 |
| 3 | Schema 反序列化返回值偏大 | `return sizeof(uint32_t)*2 + schema->GetSerializedSize()` 重复加了头部 | 直接 `return schema->GetSerializedSize()` |
| 4 | Row 全 null 行反序列化后数据错误 | 位图实现用一字节标记一个 null，而非一位 | 改用 `byte_idx=i/8, bit_idx=i%8` 字段级按位操作 |
| 5 | Row 序列化过大 | 每个 Field 前额外写了 `GetSerializedSize()` 作为 size 前缀（Field 已自描述） | 去除前缀，反序列化用 `Field::DeserializeFrom` 返回值推进 buf |
| 6 | InsertTuple 页泄漏 | for 循环条件中 Fetch 了一次，循环体内又 Fetch 一次，第一次的指针未 Unpin | 改为 while 循环，每页只 Fetch 一次 |
| 7 | 新建表未创建第一页 | 构造函数内有 `ASSERT(false, "Not implemented yet.")` | NewPage + TablePage::Init + Unpin |
| 8 | 测试超长记录拒绝失败 | Field 构造触发 `VARCHAR_MAX_LEN` 断言 | 改由两个 `char(2047)` 字段合计超越 SIZE_MAX_ROW |
| 9 | FetchPage 为 nullptr 时 InsertTuple 崩溃 | 未检查 NewPage 返回值 | 加 nullptr 检查后 return false |
| 10 | Row::DeserializeFrom 不支持重复反序列化 | `ASSERT(fields_.empty())` 过严 | 改为 `destroy()` 先清空 |

---

## 八、附录：关键常量

| 常量 | 值 | 含义 |
|------|-----|------|
| PAGE_SIZE | 4096 | 数据页字节数 |
| SIZE_TABLE_PAGE_HEADER | 24 | TablePage 固定表头字节数 |
| SIZE_TUPLE | 8 | 单个 slot 元数据字节数（4B offset + 4B size） |
| DELETE_MASK | `1 << 31` | 逻辑删除标记位 |
| SIZE_MAX_ROW | `PAGE_SIZE - SIZE_TABLE_PAGE_HEADER - SIZE_TUPLE` | 单条记录最大字节数 |
| COLUMN_MAGIC_NUM | 210928 | Column 序列化魔数 |
| SCHEMA_MAGIC_NUM | 200715 | Schema 序列化魔数 |
| VARCHAR_MAX_LEN | `PAGE_SIZE / 2 = 2048` | char 字段最大长度 |
| INVALID_PAGE_ID | -1 | 无效页号 |
| INVALID_ROWID | `RowId(INVALID_PAGE_ID, 0)` | 无效行标识 |
