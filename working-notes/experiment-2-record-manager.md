# 实验 #2：Record Manager 工作记录

## 一、模块结构

Record Manager 负责管理数据表中所有记录（行）的存储、读写和组织。本实验涉及两大类工作：数据的序列化/反序列化——决定 C++ 对象如何在 4096 字节的页中持久化为字节流；堆表操作——以无序堆的形式组织记录，提供插入、更新、删除、查询和遍历功能。

涉及的核心类型关系如下：一张表由 Schema 描述（若干 Column），一行记录是 Row（若干 Field）。Field 读写字节流的能力已经由框架实现（TypeInt/TypeFloat/TypeChar），Column 和 Schema 的序列化按照与 Field 相同的模式——MAGIC_NUM 校验、定长字段、变长字符串——自行设计格式。Row 的序列化按字段级委托实现。堆表由 TablePage（框架已实现）和 TableHeap（本次实现）共同支撑，TableIterator 提供遍历能力。

---

## 二、Column 序列化

### 2.1 格式设计

Column 的序列化需要覆盖全部七个字段：MAGIC_NUM（210928）、列名、类型、长度、表内位置索引、是否可空、是否唯一。列名是变长的，用 uint32_t 前缀记长度再跟实际字符；类型是枚举 TypeId，序列化时显式转为 uint32_t；可空和唯一是 bool，同样转为 uint32_t。所有非字符串字段统一为 4 字节宽度——这样 GetSerializedSize 里每一项都是确定的 sizeof(uint32_t) 或 name_.size()，不存在 sizeof(bool) 或 sizeof(TypeId) 这类平台相关的不确定值。

### 2.2 SerializeTo

入参是 char 指针 buf，返回值是实际写入的字节数。按顺序写：MAGIC_NUM（4 字节）→ name_.size()（4 字节）→ name_.data()（name_.size() 字节）→ static_cast<uint32_t>(type_)（4 字节）→ len_（4 字节）→ table_ind_（4 字节）→ static_cast<uint32_t>(nullable_)（4 字节）→ static_cast<uint32_t>(unique_)（4 字节）。返回 GetSerializedSize()。

### 2.3 GetSerializedSize

逐个字段累加，逻辑和 SerializeTo 的写入过程完全对应：7 个 sizeof(uint32_t) 加 name_.size()。

### 2.4 DeserializeFrom

静态方法，入参是 buf 和出参 column 的指针引用。先读前 4 字节校验 MAGIC_NUM——不匹配则返回 0（不消耗任何字节，column 不被赋值）。匹配则按序读出 name_size、name（用 std::string 的指针+长度构造）、type、len、table_ind、nullable、unique。然后按 type 分支构造：如果是 kTypeChar 走三参数构造（传 len），否则走两参数构造（len 由构造函数根据类型自动设——int 为 4，float 为 4）。返回 column->GetSerializedSize()。

### 2.5 踩坑：类型宽度不一致

初版用 sizeof(TypeId) 和 sizeof(bool) 序列化 type_ 和两个 bool。两者在不同编译器上的宽度不同。改成显式 static_cast<uint32_t> 后读写一致。

### 2.6 踩坑：反序列化未按类型分支构造

初版总是走三参数构造——对于 int/float 列会触发 ASSERT(type == kTypeChar) 崩溃。修成按类型分支：char 走带 len 的构造，其余走不带 len 的构造。

---

## 三、Schema 序列化

### 3.1 格式设计

Schema 是 Column 的容器。序列化格式：MAGIC_NUM（200715，4 字节）→ GetColumnCount()（4 字节）→ 逐列调 column->SerializeTo（每列长度不一，由 Column 自己决定）。

### 3.2 SerializeTo / GetSerializedSize / DeserializeFrom

SerializeTo：写 MAGIC_NUM → 写列数 → 循环列数次，每次调 GetColumn(i)->SerializeTo(buf) 并 buf += 返回值。返回 GetSerializedSize()。

GetSerializedSize：MAGIC_NUM 的 4 字节 + 列数的 4 字节 + 逐列 GetSerializedSize 之和。

DeserializeFrom：读 MAGIC_NUM 校验（不匹配返回 0）→ 读列数 → 循环列数次，每次调 Column::DeserializeFrom(buf, column)，把返回的 column 推入 vector。每次调用后 buf 前进返回值字节数。最终 new Schema(columns, true) 赋值给出参。返回 schema->GetSerializedSize()。

### 3.3 踩坑：返回值重复加 header

初版 SerializeTo 返回 `sizeof(uint32_t) * 2 + GetSerializedSize()`，但 GetSerializedSize 内部已经加了 header 的 8 字节——相当于 header 被算了两次。DeserializeFrom 同理。修正为直接 return GetSerializedSize() 和 return schema->GetSerializedSize()。

---

## 四、Row 序列化

### 4.1 格式设计

Row 的序列化格式在头文件的注释中已给出框架：Header 包含字段数（uint32_t）和 null 位图（ceil(N/8) 字节）。Header 之后是逐个 Field 的序列化数据——只写非 null 字段，null 字段在 body 中不占任何空间。

位图的语义：bit 为 1 表示该字段是 null，为 0 表示非 null。

Field 的序列化已由框架实现——TypeInt 写 4 字节 int32_t，TypeFloat 写 4 字节 float_t，TypeChar 写 4 字节长度前缀加实际字符数据。null 字段的 SerializeTo 返回 0。因此 Row 只需按序调用非 null 字段的 SerializeTo，把它们的返回值累加。

### 4.2 SerializeTo

两趟扫描。第一趟——头信息：写字段数（4 字节），计算位图大小 ceil(N/8)，在 buf 中对应位置用 memset 清零，推进 buf。记录 body_start 指针。第二趟——标记 null 位：遍历所有字段，若某个字段 IsNull()，在位图中对应位置置 1（byte_idx = i/8，bit_idx = i%8，bitmap_start[byte_idx] |= (1 << bit_idx)）。第三趟——写数据：再次遍历，对非 null 字段调 SerializeTo，buf 推进其返回值。总返回值 = sizeof(uint32_t) + null_bitmap_size + (buf - body_start)。

### 4.3 GetSerializedSize

Header：sizeof(uint32_t) + ceil(N/8)。Body：逐字段调 GetSerializedSize——null 字段返回 0，非 null 返回实际大小。两者相加。

### 4.4 DeserializeFrom

读字段数 → 算位图大小 → 读位图 → 逐字段：查位图判断 is_null（bitmap[byte_idx] 右移 bit_idx 位后 & 1），通过 schema->GetColumn(i)->GetType() 获取类型，调 Field::DeserializeFrom(buf, type_id, &field, is_null)，返回值推进 buf。最终返回 GetSerializedSize(schema)。

注意：反序列化开始时调 destroy() 清空已有字段，而非断言 fields_.empty()——后者过于严格，不能兼容 Row 被多次反序列化的场景。

### 4.5 踩坑：位图用字节而非位

初版位图虽然计算了 ceil(N/8) 的大小，但在标记 null 时每个 null 字段写了整整一个字节（`*reinterpret_cast<uint8_t *>(bitmap_) = 1; bitmap_ += sizeof(uint8_t)`），而非按位设置。修正为按位操作——byte_idx = i/8，bit_idx = i%8，用按位或置 1。

### 4.6 踩坑：每个字段前加了冗余 size 前缀

初版对每个非 null 字段写了 `fields_[i]->GetSerializedSize()` 作为前缀再写实际数据。但 Field 的 SerializeTo 已经包含自描述信息（int/float 固定长度，char 自带长度前缀），不需要 Row 层再包一层。去掉后 DeserializeFrom 改用 Field::DeserializeFrom 的返回值推进 buf。

---

## 五、TableHeap

### 5.1 构造——新建表

第一个构造函数（带 schema、txn、log_mgr、lock_mgr 参数）负责创建表的第一页。调 buffer_pool_manager 的 NewPage 拿到新页号和 Page 对象，reinterpret_cast 为 TablePage，调用 Init(page_id, INVALID_PAGE_ID, log_manager, nullptr) 初始化页头——page_id 设为自身，PrevPageId 设为 INVALID（因为是表的第一页），NextPageId 由 Init 内部设为 INVALID，FreeSpacePointer 指向页尾，TupleCount 为 0。first_page_id_ 记录此页号。UnpinPage 标记脏。

第二个构造函数（打开已有表，传 first_page_id）已由框架实现，直接保存 first_page_id。

### 5.2 InsertTuple——First Fit 插入

入参是 row（包含要插入的数据，row.rid_ 被设为出参）和 txn（事务上下文，当前传 nullptr）。返回值 bool。

第一步：检查 row.GetSerializedSize(schema_) 是否超过 TablePage::SIZE_MAX_ROW（单页可容纳的最大记录大小）。超过则直接返回 false——记录跨页暂不支持。

第二步：从 first_page_id_ 开始沿 NextPageId 链表逐页查找。每到一个页：FetchPage（从 BPM 获取该页的 Page 指针），reinterpret_cast 为 TablePage，调用 page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_)。如果返回 true——插入成功——row.rid_ 已在 TablePage 内部被设定，UnpinPage 标记脏，返回 true。如果返回 false——当前页装不下——记录 prev_page_id（用于后面建新页时更新链接），取 page->GetNextPageId()，UnpinPage 不标记脏，继续循环。

第三步：链表走到头（page_id == INVALID_PAGE_ID）说明所有已有页都装不下。调 BPM 的 NewPage 创建新页，reinterpret_cast 为 TablePage，Init 传新 page_id 和 prev_page_id。如果 prev_page_id 有效（不是表的第一页），Fetch prev_page 并调 SetNextPageId(new_page_id) 更新链表，Unpin 标记脏。在新页上调 InsertTuple。Unpin 新页标记脏。返回插入结果。

这里每页只 Fetch 一次，用完立即 Unpin，不像初版在 for 循环条件里也 Fetch 了一次造成"一页被 Fetch 两次，第一次拿到的指针从未 Unpin"的页泄漏。

### 5.3 GetTuple——按 RowId 读取

RowId 的高 32 位是 page_id，低 32 位是 slot_num。Fetch page_id 对应的页，reinterpret_cast，调 page->GetTuple(row, schema_, txn, lock_manager_)——TablePage 内部根据 slot_num 定位到该记录的偏移地址，反序列化填充 row 的 fields_ 向量。Unpin 不标记脏（只读），返回结果。

### 5.4 MarkDelete / RollbackDelete——逻辑删除与回滚

这两个函数在框架中已实现。MarkDelete 把目标 slot 的 size 字段的最高位置 1（DELETE_MASK），后续 GetTuple 和迭代器会跳过此记录。RollbackDelete 清除最高位恢复可见性。注意它们都加了 WLatch/WUnlatch 做页级写锁——虽然当前单线程模式不需要，但保持了接口一致性。

### 5.5 ApplyDelete——物理删除

Fetch 目标页，调 page->ApplyDelete(rid, txn, log_manager_)。TablePage 内部做的是"压实"操作——把被删记录之后的数据前移、更新所有受影响 slot 的 offset、回收 FreeSpacePointer。Unpin 标记脏。

### 5.6 UpdateTuple——原地更新或删旧插新

策略分两阶段。第一阶段——尝试原地更新：Fetch 目标页，读出旧记录到临时 Row old_row（确认旧记录存在且未被删除），调 page->UpdateTuple(row, &old_row, schema_, txn, lock_manager_, log_manager_)。如果成功——新记录大小不超过旧页剩余空间——row.rid_ 被设为和旧记录相同的 RowId，Unpin 标记脏，返回 true。

第二阶段——原地空间不够：Unpin 旧页不标记脏，依次调用 MarkDelete(rid, txn) 标记逻辑删除、ApplyDelete(rid, txn) 物理回收、InsertTuple(row, txn) 将新记录插入到有足够空间的位置（可能在别的页或新建页）。InsertTuple 成功后 row.rid_ 被设为新位置。返回 InsertTuple 的结果。

调用者通过 row.GetRowId() 获取更新后的 RowId——原地更新时不变，删旧插新时变为新位置。

### 5.7 Begin / End——迭代器边界

Begin 从 first_page_id_ 开始逐页沿 NextPageId 查找。对每一页调 GetFirstTupleRid——找到第一条未被删除的记录（size 不为 0 且 DELETE_MASK 未设置）。找到则构造 TableIterator(this, first_rid, txn) 返回。整表为空则返回 End()。

End 返回 TableIterator(this, RowId(INVALID_PAGE_ID, 0), nullptr)——RowId 的 page_id 为 INVALID_PAGE_ID，迭代器内部 row_ 设为 nullptr 作为哨兵。

---

## 六、TableIterator

### 6.1 数据结构

TableIterator 持有三个私有成员：table_heap_ 指针（用于调用 GetTuple 加载数据）、row_ 指针（当前记录的深拷贝，operator* 和 operator-> 返回它的引用/指针）、txn_（事务上下文）。

### 6.2 构造与析构

构造时如果 rid 有效（page_id != INVALID_PAGE_ID）：new Row(rid)，然后调 table_heap_->GetTuple(row_, txn_) 从磁盘加载数据到 row_->fields_ 中。如果 rid 是 INVALID_ROWID（End 哨兵）：row_ 置 nullptr。

析构时 delete row_。拷贝构造和赋值运算符都是深拷贝——如果 source.row_ 非空，new Row(*source.row_)（Row 的拷贝构造也是深拷贝）。

### 6.3 比较运算

两个迭代器相等当且仅当两者 row_ 都为空（都是 End），或者都非空且 RowId 相同。不等即取反。

### 6.4 解引用

operator* 返回 `*row_` 的 const 引用。operator-> 返回 row_ 指针。

### 6.5 前缀递增——移动到下一条有效记录

这段逻辑是迭代器最复杂的部分。分三步。

第一步：从当前 row_ 获取 cur_rid，其 page_id 指向当前所在的页。Fetch 该页，调 GetNextTupleRid(cur_rid, &next_rid) 在当前页中找下一个未被删除的 slot。同时记录该页的 NextPageId。Unpin 当前页。

如果 GetNextTupleRid 返回 true——同页内还有有效记录：更新 row_ 的 RowId 为 next_rid，重新 GetTuple 加载数据，返回 *this。

第二步：同页没有更多记录了。沿 NextPageId 进入 while 循环逐页查找。每页 Fetch → GetFirstTupleRid → 记录 NextPageId → Unpin。如果找到第一条有效记录则更新 row_ 并返回。

第三步：所有页都遍历完毕。delete row_，置 nullptr——迭代器变成 End 哨兵。

### 6.6 后缀递增

保存当前状态的拷贝（TableIterator copy(*this)），自身调前缀 ++ 前进，返回拷贝（前进前的状态）。

---

## 七、测试结果

课程组测试 3 用例 + 自编测试 19 用例，全部通过：

- TupleTest::FieldSerializeDeserializeTest / RowTest
- ColumnStudentTest（4 个）：int/char/float roundtrip、MAGIC_NUM 校验失败
- SchemaStudentTest（1 个）：多列 roundtrip
- RowStudentTest（4 个）：全 null、混合 null、空字符串、SerializeTo 与 GetSerializedSize 一致
- TableHeapTest::TableHeapSampleTest（10000 行插入+读取验证）
- TableHeapStudentTest（10 个）：删除生命周期、两种更新路径、迭代器跨页/空表/跳过删除/后缀递增、超长记录拒绝、非法 page_id 错误路径

---

## 八、验收演示指引

**序列化部分**：展示 Column → Schema → Row 三层级的格式设计，说明 MAGIC_NUM 的作用（反序列化校验），null 位图的按位存取的动机（节省空间，N 个字段只需 ceil(N/8) 字节而非 N 字节）。

**堆表部分**：演示 InsertTuple 的 First Fit 策略——从第一页开始逐页尝试，所有页满才建新页。演示 TablePage 的 Slotted-page 结构——Header 从左往右长，记录从右往左长，中间是空闲空间。演示 MarkDelete（逻辑删除，修改 DELETE_MASK 位）和 ApplyDelete（物理删除，压实回收）的配合。演示 UpdateTuple 的两阶段策略——先尝试原地更新，空间不够就删旧插新。

**迭代器部分**：演示 Begin → operator++ 跨页遍历 → End 的链路，说明迭代器如何通过 GetNextTupleRid/GetFirstTupleRid 跳过已删除记录。

**可能的提问**：
- Row 序列化中 null 字段为什么不在 body 里占空间：位图已标记 null，反序列化时直接构造一个对应类型的空 Field，不需要 body 数据。
- Field 已能自描述，为什么 Row 还需要 Schema 参数：Field 的 DeserializeFrom 需要 type_id 参数——只有 Schema 知道每一列的类型。
- UpdateTuple 删旧插新后 RowId 变了调用者怎么知道：新的 RowId 写入了 row.rid_，调用者读 row.GetRowId() 即可。
