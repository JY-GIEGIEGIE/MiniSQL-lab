# 实验 #3：Index Manager 工作记录

## 一、模块总览

Index Manager 为 MiniSQL 提供基于磁盘的 B+ 树动态索引。索引存储键值对——键是索引列值序列化后的 GenericKey，值是 RowId（叶结点）或 page_id（内部结点）。上层通过 BPlusTreeIndex 封装类调用 BPlusTree，KeyManager 负责键的序列化和比较，三者共同构成索引子系统。

### 1.1 与已有模块的关系

B+ 树的每个结点占用一个数据页（PAGE_SIZE = 4096 字节）。结点的创建、读取和写入通过 BufferPoolManager（实验 #1）完成——`NewPage` 分配新页，`FetchPage` 读取已有页，`UnpinPage` 释放引用并标记脏，`DeletePage` 回收页。B+ 树不直接调用 DiskManager（实验 #1），所有磁盘 I/O 通过 BPM 透明完成。

B+ 树存储的 Value 是 RowId（实验 #2）——高 32 位 page_id + 低 32 位 slot_num，指向堆表中的具体记录。Key 是 GenericKey——由 KeyManager 将 Row（实验 #2）序列化为定长字节数组后存入结点，比较时通过 KeyManager::CompareKeys 反序列化后逐 Field（实验 #2）比较。

### 1.2 文件清单

| 文件 | 职责 |
|------|------|
| `b_plus_tree_page.h/.cpp` | 叶结点和内部结点的公共基类 |
| `b_plus_tree_internal_page.h/.cpp` | 内部结点：键为 GenericKey，值为 page_id_t |
| `b_plus_tree_leaf_page.h/.cpp` | 叶结点：键为 GenericKey，值为 RowId，含 next_page_id_ 链表 |
| `b_plus_tree.h/.cpp` | B+ 树主体：Insert/Remove/GetValue/Iterator |
| `index_iterator.h/.cpp` | 叶结点层级的顺序迭代器 |
| `generic_key.h` | GenericKey（零长数组）和 KeyManager（序列化/比较） |
| `index_roots_page.h` | 索引根页号持久化表，存储在逻辑页 INDEX_ROOTS_PAGE_ID |
| `b_plus_tree_index.h` | 对 BPlusTree 的上层封装，已由框架实现 |

---

## 二、页面布局与 key-value 的物理存储

### 2.1 BPlusTreePage——基类头（28 字节）

所有 B+ 树结点共享这 28 字节头，通过 `reinterpret_cast<BPlusTreePage*>(page->GetData())` 读写。七个字段在 Page::data_ 上的偏移布局：

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 4 | page_type_ | LEAF_PAGE 或 INTERNAL_PAGE |
| 4 | 4 | key_size_ | 索引键的字节长度 |
| 8 | 4 | lsn_ | 日志序列号（未使用） |
| 12 | 4 | size_ | 当前键值对数量 |
| 16 | 4 | max_size_ | 最大键值对容量 |
| 20 | 4 | parent_page_id_ | 父结点 page_id |
| 24 | 4 | page_id_ | 本结点 page_id |

关键方法：

- **IsLeafPage()**：`page_type_ == LEAF_PAGE`
- **IsRootPage()**：`parent_page_id_ == INVALID_PAGE_ID`
- **GetMinSize()**：根叶返回 1，根内返回 2，非根返回 `max_size_ / 2`。该函数在 Remove 路径用于判断结点是否触发 underflow

### 2.2 InternalPage——内部结点

头 28 字节 + `data_[PAGE_SIZE - 28]`。每对键值占 `key_size + sizeof(page_id_t)` 字节。n 个 size 意味着 n 个 value（子结点指针）和 n 个 key，但第一个 key（KeyAt(0)）始终为 INVALID——为的是"n 个 key 配 n 个 value"的统一索引，不浪费额外逻辑。

物理布局（size=3 为例）：

```
偏移 0:     KeyAt(0) = INVALID
偏移 key_size: ValueAt(0) = 第一个子结点 page_id
偏移 pair_size: KeyAt(1) = 第一个有效分隔键
偏移 pair_size+key_size: ValueAt(1) = 第二个子结点
偏移 2*pair_size: KeyAt(2) = 第二个分隔键
偏移 2*pair_size+key_size: ValueAt(2) = 第三个子结点
```

其中 `pair_size = key_size + sizeof(page_id_t)`。

**KeyAt(index) / ValueAt(index)**：框架已实现。通过 `data_ + index * pair_size + offset` 计算地址，reinterpret_cast 后读写。

**Lookup(key, KM)**：在内部结点中二分查找 key 应走向的子结点。从 index=1 开始（跳过 INVALID 的 KeyAt(0)），找第一个 `KeyAt(mid) > key` 的位置，返回 `ValueAt(mid-1)`。

操作细节：比较条件是 `KM.CompareKeys(key, KeyAt(mid)) < 0` 时收缩右界，否则收缩左界。循环结束后 `lft` 指向第一个大于 key 的位置，对应的子结点指针是 `ValueAt(lft-1)`。若 key 大于等于所有键，lft 被推到 size，`ValueAt(size-1)` 就是最右子结点。

**Init(page_id, parent_id, key_size, max_size)**：设置 `page_type_ = INTERNAL_PAGE`、`size_ = 0`、page_id、parent_id、key_size、max_size。

**PopulateNewRoot(old_value, new_key, new_value)**：根结点分裂后创建新根。只设两对——`ValueAt(0)=old_value`，`KeyAt(1)=new_key`，`ValueAt(1)=new_value`，`size=2`。

**InsertNodeAfter(old_value, new_key, new_value)**：在 old_value 对应的键值对后面插入新对。先通过 `ValueIndex` 找到 old_value 的下标 idx，把 `[idx+1, size)` 的键值对从后往前搬一格（避免覆盖），在 idx+1 位置写入新键值对，size 加一。

**Remove(index)**：删除第 index 个键值对。把 `[index+1, size)` 往前搬一格覆盖，size 减一。

**RemoveAndReturnOnlyChild()**：AdjustRoot 专用。当内部根只剩一个子结点时，返回 `ValueAt(0)` 并将 size 清零。

**MoveHalfTo(recipient, bpm)**：分裂用。从 `size/2` 位置起把后半键值对拷给 recipient（调用 `CopyNFrom`），本页 size 截断到 start。

**CopyNFrom(src, size, bpm)**：把 src 处 size 对键值对拷贝到本页尾部，然后逐个"收养"被搬来的子结点——Fetch 子页、更新其 `parent_page_id_` 为本页 page_id、Unpin 标记脏。

**MoveAllTo(recipient, middle_key, bpm)**：合并用。先把 middle_key（父结点中的分隔键）和本页第一个子结点 `ValueAt(0)` 放入 recipient 末尾作为新旧数据的分隔，再逐个追加密钥对。完成后本页 size 清零。所有被移动子页的 parent 通过收养更新。

**MoveFirstToEndOf / MoveLastToFrontOf**：Redistribute 用。前者将本页第一个子结点搬到 recipient 末尾（配 middle_key），本页数据前移；后者将本页最后一个子结点搬到 recipient 开头（配 middle_key），recipient 数据右移后 `KeyAt(1)` 被 middle_key 覆盖（原 INVALID 键被推到了 KeyAt(1)）。

**CopyLastFrom / CopyFirstFrom**：前者末尾追加一对并收养子页；后者全体右移一格，最前插入 value 并收养子页。

### 2.3 LeafPage——叶结点

头 32 字节（比 InternalPage 多 4 字节 `next_page_id_`）+ `data_[PAGE_SIZE - 32]`。每对键值占 `key_size + sizeof(RowId)` 字节。键值对从 index=0 开始全部有效（无 INVALID 键）。`next_page_id_` 将所有叶子串成单向链表，支持 IndexIterator 的顺序遍历。

**Init(page_id, parent_id, key_size, max_size)**：与 InternalPage 相同但 `page_type_ = LEAF_PAGE`，`next_page_id_ = INVALID_PAGE_ID`。

**KeyIndex(key, KM)**：二分查找第一个 `KeyAt(i) >= key` 的索引 i。用于迭代器起始定位和插入/删除的键值定位。

**Lookup(key, value, KM)**：KeyIndex 定位后比较键是否相等，找到则把 `ValueAt(index)` 写入 value 出参返回 true。

**Insert(key, value, KM)**：KeyIndex 定插入位，将 `[idx, size)` 后移一对，写入新键值对，size 加一。若 key 已存在则更新 value（防 Unique 约束破坏，实际 BPlusTree 层已先查重）。

**RemoveAndDeleteRecord(key, KM)**：KeyIndex 定位置，如果 key 不存在则不变；如果存在则将 `[idx+1, size)` 前移覆盖，size 减一。

**MoveHalfTo(recipient)**：分裂用。从 `size/2` 起后半拷给 recipient，维护叶子链表——recipient 的 `next_page_id_` 设为本页原 `next_page_id_`，本页的 `next_page_id_` 改为 recipient 的 page_id。

**CopyNFrom(src, size)**：纯体力——PairCopy 将 size 对键值对拷到本页尾部，IncreaseSize。

**MoveAllTo(recipient)**：合并用。全部数据拷入 recipient，本页 size 清零，更新 recipient 的 `next_page_id_`（仅当本页不在 recipient 左侧时——即 `GetNextPageId() != recipient->GetPageId()`，避免自环）。

**MoveFirstToEndOf / MoveLastToFrontOf**：Redistribute 用。前者将本页第一对搬到 recipient 末尾后本页前移；后者将本页最后一对搬到 recipient 开头后 recipient 右移。

**CopyLastFrom / CopyFirstFrom**：单对追加。前者末尾加一对；后者全体右移后头部插入一对。

### 2.4 GenericKey 与 KeyManager（框架已实现）

GenericKey 是柔性数组 `char data[0]`——实际内存由 `malloc(key_size_)` 分配。KeyManager 持有 `key_schema_`（索引列组成的 Schema）和 `key_size_`。

- **SerializeFromKey(key_buf, row, schema)**：将 Row 序列化到 GenericKey::data 中
- **DeserializeToKey(key_buf, row, schema)**：从 GenericKey::data 反序列化出 Row
- **CompareKeys(lhs, rhs)**：将两个 GenericKey 反序列化为 Row，逐字段调用 `Field::CompareLessThan/CompareGreaterThan` 比较，返回 -1、0、1

---

## 三、BPlusTree——树级操作

### 3.1 构造与基础

**构造函数**：若 `leaf_max_size` 或 `internal_max_size` 为 `UNDEFINED_SIZE(0)`，则根据 key_size 和 PAGE_SIZE 计算——`(PAGE_SIZE - HEADER) / pair_size - 1`（减 1 留溢出空间，避免 Insert 后超越 data_ 边界腐化相邻 Page 对象）。

**IsEmpty()**：根 page_id 非法或根 size 为 0。

**Destroy(current_page_id)**：递归删除。内部结点先递归删子结点再删自己；叶子直接删。每页先 Unpin 再 DeletePage。入口传 INVALID_PAGE_ID 时从根开始。

**FindLeafPage(key, page_id, leftMost)**：从 page_id 沿 InternalPage::Lookup 一路向下到叶子。每层 Fetch 当前页 → 若是叶子则返回（pinned，调用者负责 Unpin）→ 否则 Lookup 得下一层 page_id → Unpin 当前页 → 继续。leftMost 为 true 时始终走 `ValueAt(0)`。

**UpdateRootPageId(insert_record)**：Fetch `INDEX_ROOTS_PAGE_ID` 页 → `reinterpret_cast<IndexRootsPage*>` → `Insert`（首次建树）或 `Update`（根变更）→ Unpin 脏。

### 3.2 GetValue——点查询

FindLeafPage 找叶 → Lookup 查键 → Unpin 叶 → 找到则 push 进 result。

### 3.3 Insert——插入

整体流程：树空则 StartNewTree，否则 InsertIntoLeaf。

**StartNewTree(key, value)**：NewPage → Init 叶 → Insert → 设 root_page_id_ → Unpin → UpdateRootPageId(1)。

**InsertIntoLeaf(key, value, txn)**：FindLeafPage → Lookup 查重（有则返回 false）→ Insert → 若超过 max_size 则 Split → InsertIntoParent 向父结点插入新叶索引项 → Unpin。

**Split(node)**：NewPage → Init 新页（继承父指针和 key_size/max_size）→ MoveHalfTo 搬一半数据过去 → 返回新页。

**InsertIntoParent(old_node, key, new_node, txn)**：核心难点之一。old_node 是根则创建新根（NewPage → Init → PopulateNewRoot → 设根 → UpdateRootPageId）。非根则 Fetch 父页 → InsertNodeAfter 插入新结点索引 → 若父页超 max_size 则 Split 父页并递归 InsertIntoParent（key 取 new_parent->KeyAt(1)）。

### 3.4 Remove——删除

**Remove(key, txn)**：FindLeafPage 找叶 → RemoveAndDeleteRecord 删键 → 若 size 未变（key 不存在）则 Unpin 返回 → 否则进入 while 循环逐级处理 underflow。

**while 循环的核心逻辑**（本次重构后的最终版本）：

1. 若 cur 是根：AdjustRoot → Unpin → 返回
2. 若 cur 满足 min_size：Unpin → 返回
3. 否则获取 parent，找到 sibling（优先左兄弟，左兄弟不存在则取右兄弟）
4. 判断 redistribute 还是 merge：
   - **Redistribute**（node->size + sibling->size >= max_size）：从 sibling 借一个。叶版本搬运键值对后更新 parent 分隔键；内版本搬运子结点后下行取左最叶第一键更新 parent 分隔键。Unpin 所有相关页 → 返回。
   - **Merge**（右合入左，CMU 参考约定）：始终将右兄弟合并进左兄弟，删除右兄弟。叶版本 MoveAllTo 搬运全部数据；内版本 MoveAllTo 搬运全部子结点（以 parent 分隔键为界）。parent->Remove 删除右兄弟的索引项。merge 后 parent 成为新的 cur，继续循环。

**AdjustRoot(old_root_node)**：叶根 size==0 → 删根，树空。内根 size<=1 → 取出唯一子结点提升为新根，更新子结点 parent 为 INVALID，删旧根。

**关键教训——合并方向**：旧代码在 index==0 时把左兄弟合并到右兄弟，`parent->Remove(0)` 后 KeyAt(0) 获得有效的旧 KeyAt(1)（但 KeyAt(0) 应始终 INVALID），虽 Lookup 不读 KeyAt(0)，但后续 Merge 操作可能使 parent 内部分隔键偏移错误，导致同父下其他叶子的 key 路由失效——这就是"删除 key 71 时 80 个无关 key 同时丢失"的根因。CMU 15-445 明确约定始终右合入左，删除右兄弟，保证分隔键偏移的一致性。

### 3.5 Begin / End——迭代器

**Begin()**：FindLeafPage 传 leftMost=true 找最左叶，返回 `IndexIterator(leaf_id, bpm, 0)`。

**Begin(key)**：FindLeafPage 找 key 所在叶，KeyIndex 定位起始下标，返回 `IndexIterator(leaf_id, bpm, idx)`。

**End()**：返回默认构造的 `IndexIterator()`（`current_page_id = INVALID_PAGE_ID`）。

---

## 四、IndexIterator

构造函数已部分实现。`IndexIterator(page_id, bpm, index)` 会 Fetch 目标 LeafPage 并保存在成员 `page` 中。析构时 Unpin。

**operator\*()**：`return page->GetItem(item_index)`——当前键值对。

**operator++()**：item_index++。若超过 `page->GetSize()`，沿 `next_page_id_` 去下一页，Unpin 旧页 Fetch 新页，item_index 归零。若没有下一页，page 置 nullptr（End 哨兵）。

**operator== / !=**：比较 `current_page_id` 和 `item_index`。

---

## 五、调试历程与关键 bug

本次实验总共遇到并修复了以下 bug，按发现顺序排列：

| # | 位置 | 问题 | 修复 |
|---|------|------|------|
| 1 | BPlusTree 构造 | max_size 未预留溢出空间，Insert 后 size 可达 max+1，写作超出 data_ 数组边界腐化相邻 Page 对象 | 计算公式减 1 |
| 2 | InternalPage::Lookup | `KeyManager::CompareKeys` 写成静态调用（应为 `KM.CompareKeys`）| 修正为实例调用 |
| 3 | InternalPage::PopulateNewRoot | 缺 `SetSize(2)` | 补加 |
| 4 | InternalPage::InsertNodeAfter | 数据后移时循环方向反了（从前往后导致覆盖后续数据）| 改为从后往前搬 |
| 5 | LeafPage::Init | `SetNextPageId(0)` 应为 `INVALID_PAGE_ID` | 修正常量 |
| 6 | LeafPage::MoveHalfTo | `PairCopy` 的 dest 传了 `recipient` 对象指针而非 `recipient->PairPtrAt(0)`| 改用 `CopyNFrom` |
| 7 | Merge 方向 | index==0 时左合入右，`parent->Remove(0)` 导致内部页 KeyAt(0) 变为有效键，后续操作使父键偏移不一致 | 统一为右合入左（CMU 参考） |
| 8 | InternalPage Redistribute 父键 | 父键更新用 `sib->KeyAt(1)`（内部结点分隔键）而非子树真正最小键 | 下行 FindLeafPage 取左最叶 KeyAt(0) |
| 9 | Coalesce Remove 索引 | `index==0` 时应 `parent->Remove(0)` | 修正（旧代码写 `Remove(1)`） |

---

## 六、测试结果

课程组测试：`b_plus_tree_test` 100/100 次通过（shuffle 不可预测场景），`b_plus_tree_index_test` 2/2，`index_iterator_test` 1/1。自编测试：CourseSimulation 20 trials、EmptyTreeOperations、InsertAllThenDeleteAll 均通过。全部确定性 + 随机测试无失败。

---

## 七、验收演示指引

**架构讲解顺序**：GenericKey/KeyManager（键的序列化与比较）→ BPlusTreePage 基类 → InternalPage（子结点指针和分隔键的二分查找）→ LeafPage（实际 RowId 存储和叶子链表）→ BPlusTree Insert（FindLeafPage → InsertIntoLeaf → Split → InsertIntoParent 递归分裂）→ BPlusTree Remove（删除键 → underflow while 循环 → Redistribute 或 Merge → AdjustRoot）→ IndexIterator（叶子链表遍历）。

**可能追问**：为什么 InternalPage 的 KeyAt(0) 是 INVALID、为什么合并必须右合入左、Redistribute 和 Merge 的决策条件、为什么 max_size 要减 1、GenericKey 的零长数组如何分配内存。
