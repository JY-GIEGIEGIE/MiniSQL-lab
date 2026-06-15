# 实验 #3：Index Manager 设计报告

## 一、模块概述

### 1.1 模块定位

Index Manager 为 MiniSQL 提供基于磁盘的 B+ 树动态索引结构。它是连接 SQL 查询层（Executor）和物理存储层（Record Manager、Buffer Pool Manager）的关键中间件——执行器通过索引快速定位 RowId，再通过堆表（TableHeap，实验 #2）读取完整记录，无需全表扫描。

### 1.2 索引在 MiniSQL 架构中的位置

```
SQL Parser → Planner & Executor
                │
                ├─ 全表扫描 → TableHeap (实验 #2) → BufferPoolManager (实验 #1) → DiskManager (实验 #1)
                │
                └─ 索引扫描 → BPlusTreeIndex → BPlusTree (本实验)
                                                     │
                                                     ├─ InternalPage / LeafPage (本实验)
                                                     │      └─ reinterpret_cast<Page::data_> (实验 #1)
                                                     ├─ KeyManager (框架提供)
                                                     │      └─ Row / Field / Schema 序列化 (实验 #2)
                                                     └─ BufferPoolManager (实验 #1)
```

### 1.3 子模块划分

| 子模块 | 职责 | 实现状态 |
|--------|------|----------|
| BPlusTreePage | 叶结点和内部结点的公共基类，管理页类型、size、父指针等元数据 | 本实验实现 |
| BPlusTreeInternalPage | 内部结点：存储 m 个分隔键 + m 个 page_id 子指针 | 本实验实现 |
| BPlusTreeLeafPage | 叶结点：存储 m 个索引键 + m 个 RowId，含叶子链表 | 本实验实现 |
| BPlusTree | 树级操作：Insert/Remove/GetValue/Iterator/Split/Merge | 本实验实现 |
| IndexIterator | 沿叶子链表遍历的迭代器 | 本实验实现 |
| KeyManager / GenericKey | 索引键的序列化/反序列化/比较 | 框架已实现 |
| IndexRootsPage | 索引根页号持久化表 | 框架已实现 |
| BPlusTreeIndex | BPlusTree 的上层封装 | 框架已实现 |

### 1.4 与已有模块的接口

**实验 #1（Disk and Buffer Pool Manager）**：B+ 树的每个结点恰好占一个 PAGE_SIZE（4096 字节）的数据页。结点内容的读写通过 `BufferPoolManager::FetchPage`、`NewPage`、`UnpinPage`、`DeletePage` 完成。结点在内存中的表示通过 `reinterpret_cast` 将 `Page::data_` 解释为 BPlusTreePage 的子类对象。所有页操作后必须 Unpin，实验 #1 的 `CheckAllUnpinned` 在测试末尾验证无页泄漏。

**实验 #2（Record Manager）**：B+ 树叶结点的 Value 类型为 `RowId`——高 32 位 `page_id` + 低 32 位 `slot_num`，指向堆表（TableHeap）中的物理记录位置。索引键（GenericKey）是 Row 对象序列化后的定长字节数组——KeyManager 通过实验 #2 的 `Row::SerializeTo`/`DeserializeFrom` 和 `Field` 比较体系实现键的存取与比较。

### 1.5 约束条件

- **唯一键（Unique Key）**：B+ 树不允许多个相同键存在，插入重复键时返回 false
- **结点 = 数据页**：每个 B+ 树结点恰好占用一个 4096 字节的数据页，页大小不可变
- **不定长键**：GenericKey 的字节长度（key_size）在索引创建时确定，运行时不变
- **单线程**：不考虑并发控制，WLatches/RLatches 在框架中已预留但本条实验不使用

---

## 二、页面物理布局设计

### 2.1 BPlusTreePage 基类头

所有 B+ 树结点共享 28 字节的元数据头，存储于 Page::data_ 的前 28 字节：

| 偏移 (B) | 字段 | 类型 | 说明 |
|-----------|------|------|------|
| 0 | page_type_ | IndexPageType(4B) | `LEAF_PAGE=1` 或 `INTERNAL_PAGE=2` |
| 4 | key_size_ | int(4B) | 每个 GenericKey 的字节长度 |
| 8 | lsn_ | lsn_t(4B) | 日志序列号，本实验未使用 |
| 12 | size_ | int(4B) | 当前存储的键值对数量 |
| 16 | max_size_ | int(4B) | 最大键值对容量 |
| 20 | parent_page_id_ | page_id_t(4B) | 父结点逻辑页号，根为 INVALID_PAGE_ID |
| 24 | page_id_ | page_id_t(4B) | 本结点逻辑页号 |

`GetMinSize()` 根据页类型和根状态返回不同的最小容量：根叶返回 1（允许只含单条记录），根内结点返回 2（至少两个子指针才能构成有效分隔），非根结点返回 `max_size_ / 2`（标准半满约束）。

### 2.2 BPlusTreeInternalPage

内部结点不存储实际数据，只存储用于路由的键值和子结点指针。结点大小为 28 字节头 + 键值对数组。

每对键值大小 `pair_size = key_size + sizeof(page_id_t)`。`data_` 数组容量为 `PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE(28)` 字节。

**键值对排列**（size=3 为例）：

| data_ 偏移 | 内容 |
|------------|------|
| 0 | KeyAt(0) = INVALID（占位，不被 Lookup 读取） |
| key_size | ValueAt(0) = 第一个子结点的 page_id |
| pair_size | KeyAt(1) = 第一个有效分隔键 |
| pair_size+key_size | ValueAt(1) = 第二个子结点的 page_id |
| 2×pair_size | KeyAt(2) = 第二个分隔键 |
| 2×pair_size+key_size | ValueAt(2) = 第三个子结点的 page_id |

**B+ 树路由不变性**：对于任意输入键 K，若 `KeyAt(i) <= K < KeyAt(i+1)`（`i >= 1`），则 K 所在记录位于 `ValueAt(i)` 指向的子树中。`K < KeyAt(1)` 时走 `ValueAt(0)`，`K >= KeyAt(size-1)` 时走 `ValueAt(size-1)`。

**Lookup 算法**：采用二分查找（复杂度 O(log size)）。从 index=1 开始（跳过 INVALID 的 KeyAt(0)），找第一个 `KeyAt(mid) > key` 的位置，返回 `ValueAt(mid-1)`。

### 2.3 BPlusTreeLeafPage

叶结点存储实际的索引数据：键（GenericKey）和值（RowId）。结点大小为 32 字节头 + 键值对数组。头比 InternalPage 多 4 字节的 `next_page_id_`，将所有叶结点串成单向链表以支持范围扫描。

每对键值大小 `pair_size = key_size + sizeof(RowId)`。键值对从 index=0 开始全部有效（无 INVALID 占位键）。

**叶子链表**：`next_page_id_` 指向下一个叶结点的 page_id，尾结点为 `INVALID_PAGE_ID`。分裂时新页插入链表中间（recipient 指向本页原后继，本页指向 recipient）。合并时需避免左合入右导致 recipient->NextPageId 自环（检查 `GetNextPageId() != recipient->GetPageId()`）。

---

## 三、键的序列化与比较

### 3.1 GenericKey 与内存管理

GenericKey 定义为 `char data[0]`（柔性数组），实际内存由 `KeyManager::InitKey()` 通过 `malloc(key_size_)` 分配。键数据在 `data` 上的格式与 `Row::SerializeTo` 的输出完全一致（实验 #2 的 Row 序列化格式：字段数 + null 位图 + 各 Field 序列化数据）。

KeyManager 持有 `key_schema_`（Schema*，浅拷贝自表 Schema 中索引列的子集）和 `key_size_`（索引键的总字节长度）。

### 3.2 CompareKeys 的比较语义

`KeyManager::CompareKeys(lhs, rhs)` 将两个 GenericKey 反序列化为 Row 对象，按 `key_schema_` 定义的列顺序逐列比较 Field：

1. 调用 `DeserializeToKey` 将 `lhs->data` 和 `rhs->data` 反序列化为两个临时 Row
2. 对每一列 `i`（0 到 `column_count-1`）：
   - 若 `lhs_field < rhs_field`（`CompareLessThan` 返回 `kTrue`），返回 -1
   - 若 `lhs_field > rhs_field`（`CompareGreaterThan` 返回 `kTrue`），返回 1
3. 所有列均相等，返回 0

比较逻辑依赖实验 #2 中各 Type 子类（TypeInt/TypeFloat/TypeChar）实现的 `CompareLessThan`/`CompareGreaterThan` 虚函数。

---

## 四、核心算法设计

### 4.1 FindLeafPage——所有操作的基础路径

```
FindLeafPage(key, page_id, leftMost):
    page_id = (page_id == INVALID) ? root_page_id_ : page_id
    while true:
        page = FetchPage(page_id)
        node = reinterpret_cast<BPlusTreePage*>(page->GetData())
        if node->IsLeafPage():
            return page  // pinned, caller must Unpin
        internal = reinterpret_cast<InternalPage*>(node)
        next = leftMost ? internal->ValueAt(0) : internal->Lookup(key, KM)
        UnpinPage(node->GetPageId(), false)
        page_id = next
```

每层内部结点 Fetch → 读 → Unpin 后进入下一层，叶子结点 pinned 返回给调用者。

### 4.2 GetValue——点查询

调用 FindLeafPage 到达目标叶，调 LeafPage::Lookup 在 O(log size) 时间内完成键查找，找到则将 RowId 写入 result。无论查没查到都 Unpin 叶（未修改，不脏）。

### 4.3 Insert——插入与递归分裂

**StartNewTree（空树）**：NewPage 分配新页作为根叶 → Init → Insert 第一对键值 → 设 `root_page_id_` → UpdateRootPageId(1)。

**InsertIntoLeaf（非空树）**：
1. FindLeafPage 到目标叶
2. Lookup 查重（Unique Key 约束），有则返回 false
3. Insert 写入键值对
4. 检查是否需要分裂：`new_size > leaf->GetMaxSize()`
5. 若需分裂：Split 创建新叶 → InsertIntoParent 将新叶的最小键和 page_id 插入父结点

**Split**：NewPage 创建新结点（叶或内部） → Init 设置元数据 → MoveHalfTo 将一半键值对搬入新结点 → 返回新结点。

**InsertIntoParent（递归向上）**：
1. old_node 是根：创建新根 InternalPage → PopulateNewRoot → 更新两子 parent → 设 root_page_id_
2. old_node 非根：Fetch 父页 → InsertNodeAfter 插入 (key, new_node.page_id) → 若父页超 max_size 则 Split 父页并递归 InsertIntoParent（key 取 new_parent->KeyAt(1)——新内部页的第一个有效分隔键）

递归在父页不需要分裂处终止。最大递归深度为树高（约 2~4 层）。

### 4.4 Remove——删除与逐级 underflow 处理

**Remove 入口**：
1. FindLeafPage 找叶
2. RemoveAndDeleteRecord 删除键（size 未变则 key 不存在，直接返回）
3. 进入逐级 underflow 处理循环

**逐级 while 循环**（核心重构逻辑）：

```
cur = leaf, is_leaf = true
while true:
    if cur->IsRootPage(): AdjustRoot(cur); Unpin(cur, dirty); return
    if cur->GetSize() >= cur->GetMinSize(): Unpin(cur, dirty); return

    parent = FetchPage(cur->GetParentPageId())
    idx = parent->ValueIndex(cur->GetPageId())
    sib_idx = (idx > 0) ? idx-1 : idx+1  // 优先左兄弟
    sib = FetchPage(parent->ValueAt(sib_idx))

    if cur->size + sib->size >= cur->max_size:
        // Redistribute——借一个键值对
        if is_leaf: 叶版本（搬运键值对 + 更新父分隔键）
        else:       内版本（搬运子结点 + 下行取左最叶拿新分隔键）
        Unpin(sib, cur, parent); return

    // Merge——始终右合入左，删除右兄弟（CMU 15-445 约定）
    if idx > 0:
        右=cur, 左=sib → cur->MoveAllTo(sib) → parent->Remove(idx)
    else:
        右=sib, 左=cur → sib->MoveAllTo(cur) → parent->Remove(1)
    Unpin(被删页); DeletePage(被删页)
    cur = parent; is_leaf = false  // 继续处理父结点
```

**AdjustRoot**：
- 叶根 size=0：删根页，`root_page_id_ = INVALID_PAGE_ID`，树变为空
- 内根 size≤1：取出唯一子结点提升为新根，更新子结点 parent 为 INVALID_PAGE_ID，删旧根

**合并方向的设计决策（来自调试）**：旧版代码在 `idx==0`（cur 为最左子结点）时把 cur（左）合并到 sib（右），执行 `parent->Remove(0)`。Remove(0) 后内部页所有键左移一位——原本有效的 `KeyAt(1)` 移动到 `KeyAt(0)`（应为 INVALID 的位置）。虽然 Lookup 不读 `KeyAt(0)`，但 KeyAt(0) 变为有效键破坏了"第一个键始终无效"的不变性。在后续操作中，此内部页如再次发生 Remove 或 Merge，键位偏移累积会导致同父下其他子树的键路由错误——即找到错误叶子、无法定位应存在的数据。修复方案参考 CMU 15-445 的约定，统一为"右合入左，删右兄弟"——`idx>0` 时 cur 合入 sib（Parent::Remove(idx)），`idx==0` 时 sib 合入 cur（Parent::Remove(1)）。确保被删除的始终是右兄弟，键位移方向一致。

### 4.5 Begin/End——迭代器边界

- **Begin()**：FindLeafPage 传 `leftMost=true` 直达最左叶，返回 `IndexIterator(leaf_id, bpm, 0)`
- **Begin(key)**：FindLeafPage 到目标叶，KeyIndex 定位 key 的起始下标
- **End()**：返回空迭代器（page_id=INVALID_PAGE_ID, page=nullptr）

---

## 五、IndexIterator 设计

IndexIterator 持有 `current_page_id`（当前叶 page_id）、`page`（LeafPage*，pinned）、`item_index`（当前叶内下标）、`buffer_pool_manager`。

**构造**：Fetch 目标 LeafPage，pin 住。

**operator++**：item_index++。若 >= page->GetSize()，沿 `next_page_id_` 去下一页（Unpin 旧页 → Fetch 新页 → item_index=0）。无下一页则 page=nullptr，表示 End。

**operator\***：`page->GetItem(item_index)` 返回 `pair<GenericKey*, RowId>`。

**operator==/!=**：比较 `current_page_id` 和 `item_index`。两个 page 都为 nullptr 的 End 迭代器相等。

---

## 六、关键设计决策

### 6.1 reinterpret_cast 的页面映射

BPlusTreePage 及其子类的成员按顺序紧密排列（标准布局类型），其内存表示与 Page::data_ 的字节序列完全一致。因此无需显式的序列化/反序列化步骤——直接通过 `reinterpret_cast<InternalPage*>(page->GetData())` 即可将磁盘页解释为 C++ 对象。这与实验 #1 中 BitmapPage 的做法相同。

### 6.2 KeyAt(0) 的 INVALID 约定

内部结点 m 个子指针（ValueAt(0)..ValueAt(m-1)）对应 m 个键值对，但只需 m-1 个分隔键。为实现统一的 pair_size 索引，第一个键（KeyAt(0)）被置为 INVALID——Lookup 从 index=1 开始，所有数据搬运函数在初始化后不依赖 KeyAt(0) 的值。这牺牲了一个 key_size 字节的空间，换取了实现简洁性。

### 6.3 max_size 减 1 的溢出保护

插入后 size 可临时达到 max_size + 1（触发分裂的条件）。`data_` 数组的大小恰为 `PAGE_SIZE - HEADER_SIZE`。若 max_size 按 `容量/pair_size` 计算，`(max_size+1)*pair_size` 会超出 `data_` 末尾写入 Page 对象的 `page_id_` 和 `pin_count_` 字段（Page::data_ 后紧跟着 `page_id_`），造成内存腐化。因此 max_size 计算时额外减 1，确保溢出的一对键值始终有合法内存空间。

### 6.4 合并方向约定

如 §4.4 所述，统一的右合入左约定避免了内部页 Remove 后键位偏移的不一致。此约定源自 CMU 15-445 BusTub 的实现规范。

### 6.5 max_size 的初始化

构造时若传入 `UNDEFINED_SIZE`（0），需根据 `key_size` 和页头大小自行计算。叶结点计算公式：`(PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / (key_size + sizeof(RowId)) - 1`。内部结点计算公式：`(PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (key_size + sizeof(page_id_t)) - 1`。

---

## 七、测试方案与结果

### 7.1 课程组测试

| 测试套件 | 用例数 | 覆盖内容 |
|----------|--------|----------|
| BPlusTreeTests | 1 | 2000 键 shuffle 插入 → 点查 → shuffle 删除前半 → 验证已删不存在/未删存在。30 轮次每次随机 shuffle，共计 120000 次操作 |
| BPlusTreeIndexTests | 2 | BPlusTreeIndex 封装层的 InsertEntry/ScanKey/Destroy |
| IndexIteratorTests | 1 | IndexIterator 的遍历和键值对访问 |

课程组 `b_plus_tree_test` 经 100 次独立运行（100×2000×3 = 600,000 次操作），**100/100 次通过**，通过率 100%。

### 7.2 自编测试

| 测试套件 | 用例数 | 覆盖内容 |
|----------|--------|----------|
| CourseSimulation | 20 trials | 完全复现课测流程：shuffle 插入 2000 → shuffle 删除 1000 → 验证 |
| EmptyTreeOperations | 1 | 空树 Begin==End、空树 Remove 不崩溃 |
| InsertAllThenDeleteAll | 1 | 500 键顺序插入 → 全部删除 → 验证树变空且每步 Check 通过 |

### 7.3 编译与运行环境

- 编译器：g++ 11.4.0
- 构建工具：cmake 3.28.1
- 构建类型：Debug（-g -O0）
- 缓冲池大小：DEFAULT_BUFFER_POOL_SIZE = 20480 页

---

## 八、遇到的问题与解决方案

| # | 问题描述 | 原因 | 解决方案 | 来源 |
|---|---------|------|---------|------|
| 1 | Insert 后随机位置 pin_count 显示巨大异常值 | max_size 计算未留溢出空间，插入时写出 data_ 边界腐化相邻 Page 对象 | max_size 公式减 1 | 自测发现 |
| 2 | InternalPage::Lookup 编译失败 | `KeyManager::CompareKeys` 被当成静态方法调用 | 改为 `KM.CompareKeys(...)` | 编译报错 |
| 3 | PopulateNewRoot 后根结点 size 错误 | 漏写 `SetSize(2)` | 补加 | 逻辑审查 |
| 4 | InsertNodeAfter 数据后移覆盖后续数据 | 循环方向从前往后，后移时覆盖了尚未搬走的数据 | 改为从后往前搬 | 逻辑审查 |
| 5 | 分裂后新叶链表断裂 | MoveHalfTo 中 `PairCopy` dest 参数误传对象指针 | 改用 `CopyNFrom(PairPtrAt(start), count)` | 逻辑审查 |
| 6 | 删除一个 key 导致 ~80 个无关 key 同时不可达 | index==0 时左叶合入右叶，`parent->Remove(0)` 后内部页键位偏移不一致，后续操作后同父下其他叶子路由错误 | 统一右合入左，Parent::Remove 始终删右子索引 | 种子 1 精确复现 + CMU 参考 |
| 7 | InternalPage Redistribute 后父分隔键取值偏高 | 用 `sib->KeyAt(1)`（内部结点自身分隔键）而非子树真正最小键 | 下行 `FindLeafPage(nullptr, sib->ValueAt(0), true)` 取左最叶第一键 | 逻辑审查 + 课测不稳定 |

---

## 九、附录：关键常量

| 常量 | 值 | 含义 |
|------|-----|------|
| INTERNAL_PAGE_HEADER_SIZE | 28 | 内部结点元数据头字节数 |
| LEAF_PAGE_HEADER_SIZE | 32 | 叶结点元数据头字节数 |
| UNDEFINED_SIZE | 0 | 构造时若传此值则自行计算 max_size |
| INDEX_ROOTS_PAGE_ID | 1 | IndexRootsPage 所在的逻辑页号 |
| PAGE_SIZE | 4096 | 数据页字节数 |
