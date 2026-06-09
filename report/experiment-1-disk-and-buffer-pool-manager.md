# 实验 #1：Disk and Buffer Pool Manager 设计报告

## 一、模块概述

### 1.1 模块定位

Disk Manager 与 Buffer Pool Manager 位于 MiniSQL 系统架构的底层，负责数据库文件的物理存储管理和内存页缓存。前者管理数据页在磁盘上的分配、回收与读写，后者在内存中维护一个固定大小的页帧池，对外提供透明的页访问接口——上层模块无需关心目标页当前在内存还是磁盘。

### 1.2 子模块划分

本模块包含四个子组件：

| 组件 | 职责 | 依赖 |
|------|------|------|
| BitmapPage | 管理单个 Extent 内所有数据页的分配状态（位图） | 无 |
| DiskManager | 管理数据库文件的整体物理布局、逻辑/物理页号映射、跨 Extent 的页分配回收 | BitmapPage |
| LRUReplacer | 维护缓冲池槽位的淘汰候补队列，按最近最少使用策略选择淘汰目标 | 无 |
| BufferPoolManager | 以固定大小页帧池缓存磁盘页，对外提供 Fetch/New/Unpin/Delete/Flush 接口 | DiskManager, LRUReplacer |

### 1.3 实现范围

本次实验完成了上述四个组件的全部接口实现，并在课程组提供的测试用例基础上，自行编写了 25 个补充测试用例覆盖边界条件与错误路径。全部 29 个测试用例均通过。

---

## 二、物理存储设计

### 2.1 数据库文件布局

数据库文件采用共享表空间设计，所有表和索引数据存放在同一个文件中。文件以 PAGE_SIZE（4096 字节）为基本单位组织，物理页从 0 开始编号。

物理页 0 为 DiskFileMetaPage，存储全局元数据——已分配页总数、Extent 数量、每个 Extent 的已用页数。DiskFileMetaPage 在内存中以 DiskManager 的 meta_data_ 成员缓存，构造时从磁盘读取，析构时写回。

物理页 0 之后是 Extent 序列。一个 Extent 由一张 BitmapPage 和 BITMAP_SIZE 个数据页组成。BITMAP_SIZE 根据 BitmapPage 模板参数 PAGE_SIZE 计算，当 PAGE_SIZE = 4096 时，BITMAP_SIZE = (4096 - 8) × 8 = 32704。因此单个 Extent 在磁盘上占据 1 + 32704 = 32705 个物理页，约 128 MB。

### 2.2 BitmapPage 内存布局

BitmapPage 是模板类，在磁盘上占用恰好 PAGE_SIZE 字节，其内部成员按顺序紧密排列：

- `page_allocated_`（uint32_t，4 字节）：当前 Extent 内已分配的数据页总数
- `next_free_page_`（uint32_t，4 字节）：下次分配时的扫描起始提示，用于加速查找
- `bytes[]`（unsigned char 数组，PAGE_SIZE - 8 字节）：位图本体，每个 bit 对应一个数据页的分配状态（0 为空闲，1 为已分配）

由于 BitmapPage 是标准布局类型（standard-layout），其内存表示与磁盘字节序列完全一致，因此可直接通过 reinterpret_cast 将磁盘读入的 char 缓冲区解释为 BitmapPage 对象进行读写，无需额外的序列化/反序列化步骤。

### 2.3 逻辑页号与物理页号映射

逻辑页号只给数据页连续编号（包含 Meta Page 和 BitmapPage 在内的管理页不参与逻辑编号），从 0 开始。Extent i 内偏移 k（0 ≤ k < BITMAP_SIZE）的数据页，其逻辑页号为 i × BITMAP_SIZE + k。

物理页号的推导过程如下：

1. Extent i 的 BitmapPage 位于物理页 1 + i × (BITMAP_SIZE + 1)，其中 +1 跳过 Meta Page，i × (BITMAP_SIZE + 1) 跳过前 i 个完整 Extent
2. Extent i 内第 k 个数据页位于 BitmapPage 后一页，即物理页 2 + i × (BITMAP_SIZE + 1) + k
3. 将 i = L / BITMAP_SIZE 和 k = L % BITMAP_SIZE 代入：

$$\text{Physical} = L + \lfloor L / \text{BITMAP\_SIZE} \rfloor + 2$$

公式中的 +2 源自两个不占逻辑页号的管理页：Meta Page（物理页 0）和当前 Extent 的 BitmapPage。

---

## 三、BitmapPage 详细设计

### 3.1 模板参数与实例化

BitmapPage 以 PageSize 为模板参数，MAX_CHARS 在编译期计算为 PageSize - 2 × sizeof(uint32_t)，GetMaxSupportedSize 返回 8 × MAX_CHARS。CPP 文件末尾对 PageSize = 64、128、256、512、1024、2048、4096 进行了显式实例化，覆盖了所有可能的页大小。实际运行使用 PageSize = 4096。

### 3.2 AllocatePage

**接口**：`bool AllocatePage(uint32_t &page_offset)`

**功能**：在 bytes 数组的 MAX_CHARS × 8 个位中找到第一个值为 0 的位，置 1 表示已分配，将位序号通过 page_offset 出参返回。

**算法流程**：
1. 快速失败：若 page_allocated_ ≥ GetMaxSupportedSize()，说明 Extent 已满，直接返回 false
2. 外层循环：遍历 bytes 数组，下标 i 从 0 到 MAX_CHARS - 1
3. 剪枝：若 bytes[i] == 0xFF（八位全 1），continue 跳过此字节
4. 内层循环：j 从 0 到 7，若 `(bytes[i] & (1 << j)) == 0` 成立，则：
   - 将该位置 1：`bytes[i] |= (1 << j)`
   - page_offset = i × 8 + j
   - page_allocated_ 自增
   - next_free_page_ = page_offset + 1
   - 返回 true
5. 双循环结束未找到空闲位，返回 false（兜底，正常情况下步骤 1 已拦截）

**时间复杂度**：最坏 O(MAX_CHARS)，平均受益于剪枝优化和 next_free_page_ 提示。

### 3.3 DeAllocatePage

**接口**：`bool DeAllocatePage(uint32_t page_offset)`

**算法流程**：
1. 越界检查：page_offset ≥ GetMaxSupportedSize() 则返回 false
2. 拆分为 byte_index = page_offset / 8，bit_index = page_offset % 8
3. 重复释放检查：调 IsPageFreeLow(byte_index, bit_index)，若已空闲返回 false
4. 位清除：构造掩码 mask = 1 << bit_index，取反后与 bytes[byte_index] 做按位与，将目标位清零
5. page_allocated_ 自减
6. 若 page_offset < next_free_page_，更新 next_free_page_ 为 page_offset（使后续分配优先复用此位置）
7. 返回 true

### 3.4 IsPageFree 与 IsPageFreeLow

IsPageFree 为公开接口，接收 page_offset，越界返回 false，否则拆分为字节/位下标后委托 IsPageFreeLow。IsPageFreeLow 为私有方法，直接通过 `(bytes[byte_index] & (1 << bit_index)) == 0` 判断目标位是否空闲。

---

## 四、DiskManager 详细设计

### 4.1 构造函数与元数据管理

构造函数以数据库文件名作为参数。若文件不存在则创建，若存在则打开。随后将物理页 0（DiskFileMetaPage）读入成员变量 meta_data_（PAGE_SIZE 字节的 char 数组），此后对 Extent 元数据的所有读写都通过 reinterpret_cast<DiskFileMetaPage*>(meta_data_) 进行。析构时或显式调用 Close 时将 meta_data_ 写回物理页 0。

### 4.2 MapPageId

**接口**：`page_id_t MapPageId(page_id_t logical_page_id)`

**实现**：`return logical_page_id + logical_page_id / BITMAP_SIZE + 2`

此方法为私有方法，仅在 ReadPage、WritePage 内部调用，对外部模块透明。

### 4.3 AllocatePage

**接口**：`page_id_t AllocatePage()`

无入参——新页分配到哪个 Extent 的哪个位置完全由 DiskManager 内部决定，调用者只需获取逻辑页号。

**算法流程（两趟策略）**：

第一趟：遍历已有 Extent。
- for extent_id = 0 to meta->num_extents_ - 1：
  - 若 meta->GetExtentUsedPage(extent_id) ≥ BITMAP_SIZE，continue（满）
  - 计算 BitmapPage 物理位置：1 + extent_id × (BITMAP_SIZE + 1)
  - 声明 char buf[PAGE_SIZE]，ReadPhysicalPage 将 BitmapPage 读入 buf
  - reinterpret_cast<BitmapPage<PAGE_SIZE>*>(buf) 得到操作指针
  - 声明 uint32_t page_offset 并调用 bitmap->AllocatePage(page_offset)
  - 成功：WritePhysicalPage 写回 buf；meta->num_allocated_pages_++；meta->extent_used_page_[extent_id]++；返回 extent_id × BITMAP_SIZE + page_offset

第二趟：所有 Extent 满，新建 Extent。
- new_extent_id = meta->num_extents_
- 计算新 BitmapPage 物理位置：1 + new_extent_id × (BITMAP_SIZE + 1)
- 声明 char buf[PAGE_SIZE] = {}（全零，对应全新的全空闲 BitmapPage）
- reinterpret_cast 后调 AllocatePage（必然分配到 offset 0）
- WritePhysicalPage 写盘
- meta->num_extents_++，meta->num_allocated_pages_++，meta->extent_used_page_[new_extent_id] = 1
- 返回 new_extent_id × BITMAP_SIZE + 0

**设计要点**：
- 利用已分配页数检查提前跳过满 Extent，避免无效磁盘 I/O
- 新建 Extent 时利用全零 buffer 直接作为初始 BitmapPage，不需要读盘（ReadPhysicalPage 对超出文件边界的物理页也返回全零，效果等价）
- 逻辑页号跨 Extent 连续增长，BITMAP_SIZE = 32704 使得单一 Extent 可管理约 128 MB 数据，总容量仅受 MetaPage 柔性数组长度限制（可记录 1022 个 Extent，约 128 GB）

### 4.4 DeAllocatePage 与 IsPageFree

DeAllocatePage 接收逻辑页号，反推 extent_id = logical / BITMAP_SIZE 和 page_offset = logical % BITMAP_SIZE，算出 BitmapPage 物理位置，读盘后委托 BitmapPage::DeAllocatePage，成功后写回 BitmapPage 并更新 MetaPage 计数。

IsPageFree 同理，反推后读 BitmapPage，委托 BitmapPage::IsPageFree。

---

## 五、LRUReplacer 详细设计

### 5.1 数据结构

LRUReplacer 使用两个 STL 容器协同维护淘汰候补队列：

- `std::list<frame_id_t> lru_list_`：有序链表，front 为最久未被访问的元素（Victim 候选），back 为最近被 Unpin 的元素
- `std::unordered_set<frame_id_t> lru_set_`：与 lru_list_ 内容同步的哈希集合，提供 O(1) 成员存在性检查

两个容器的内容始终保持一致——插入时同时加入，删除时同时移除。

### 5.2 Victim

**接口**：`bool Victim(frame_id_t *frame_id)`

若 lru_list_ 为空返回 false。否则取 front() 写入 *frame_id，pop_front()，并从 lru_set_ 中 erase 同一元素。

### 5.3 Pin

**接口**：`void Pin(frame_id_t frame_id)`

在 lru_set_ 中查找 frame_id。若不存在——说明此 frame 已不在淘汰名单中（已被 Pin 过或从未 Unpin），直接返回。若存在——从 lru_list_ 中 remove 该元素，并从 lru_set_ 中 erase。

lru_list_.remove 的复杂度为 O(n)，在缓冲池规模（典型值数十到数百）下可接受。使用 lru_set_ 避免了每个 Pin 调用都触发 O(n) 遍历（对于不在队列中的 frame，O(1) 即可判定并跳过）。

### 5.4 Unpin

**接口**：`void Unpin(frame_id_t frame_id)`

在 lru_set_ 中查找。若不存在——正常情况，将 frame_id 通过 push_back 加入 lru_list_ 尾部（表示最近被释放），并 insert 到 lru_set_。若已存在——防御性忽略，正常流程下不会发生（BPM 仅在 pin_count 恰好归零时调用一次 Unpin）。

### 5.5 Size

返回 lru_list_.size()，即当前可淘汰槽位数量。

---

## 六、BufferPoolManager 详细设计

### 6.1 核心数据结构

| 成员 | 类型 | 说明 |
|------|------|------|
| pages_ | Page[pool_size_] | 固定大小页帧数组，下标为 frame_id |
| page_table_ | unordered_map<page_id_t, frame_id_t> | 逻辑页号到槽位号的映射 |
| free_list_ | list<frame_id_t> | 空槽位队列（page_id = INVALID） |
| replacer_ | LRUReplacer* | 淘汰候补队列（page_id 有效但 pin_count = 0） |
| disk_manager_ | DiskManager* | 磁盘读写接口 |
| latch_ | recursive_mutex | 保护上述所有数据结构的互斥锁 |

### 6.2 槽位状态机

一个槽位 frame_id 在任意时刻处于以下三种状态之一：

1. **空闲**（在 free_list_ 中）：page_id_ = INVALID_PAGE_ID，未装载任何数据页。初始状态或经 DeletePage 回收后进入此状态。
2. **活跃**（不在 free_list_ 也不在 lru_list_ 中）：page_id_ 有效，pin_count_ > 0，至少一个调用者持有引用。不可淘汰。
3. **可淘汰**（在 replacer 的 lru_list_ 中）：page_id_ 有效，pin_count_ = 0。可被 Victim 选择淘汰，淘汰时若 is_dirty_ 为 true 需先落盘。

### 6.3 TryToFindFreePage

**功能**：为 FetchPage / NewPage 提供统一的槽位获取逻辑，按两级优先级查找。

**算法**：
1. 若 free_list_ 非空：取 front()，pop_front()，返回此 frame_id
2. 若 free_list_ 空，调用 replacer_->Victim(&frame_id)：
   - 成功：获取被淘汰 frame 对应的 Page 引用。若 IsDirty() 为 true，调 disk_manager_->WritePage 落盘并清除脏标记。从 page_table_ 中 erase 该页的 page_id。返回此 frame_id
   - 失败：free_list_ 和 replacer 均空，返回 INVALID_FRAME_ID，表示缓冲池已耗尽

### 6.4 FetchPage

**接口**：`Page* FetchPage(page_id_t page_id)`

**分支一**：页已在内存。在 page_table_ 中查找 page_id，命中则：pin_count_++，replacer_->Pin(frame_id)（移除淘汰资格），直接返回 Page 指针。

**分支二**：页不在内存。调用 TryToFindFreePage 获取空槽位（失败返回 nullptr）。将槽位对应 Page 的 data_ 清零（ResetMemory），通过 disk_manager_->ReadPage 从磁盘加载目标页内容。依次设置 page_id_ 为新值、pin_count_ 为 1、is_dirty_ 为 false。将映射插入 page_table_。replacer_->Pin(frame_id)。返回 Page 指针。

**关键约束**：page_table_ 插入必须在 page_id_ 赋值之后。若顺序颠倒，插入时 page.GetPageId() 返回的是旧页的 ID，将在 page_table_ 中建立错误映射。

### 6.5 NewPage

**接口**：`Page* NewPage(page_id_t &page_id)`

**与 FetchPage 的核心差异**：（1）不读磁盘，直接使用全零数据区；（2）需要先通过 disk_manager_->AllocatePage 获取新页号。

**算法**：
1. 调用 TryToFindFreePage 获取空槽位（失败返回 nullptr——关键：磁盘分配在此之后）
2. 调用 disk_manager_->AllocatePage 获取新逻辑页号，写入 page_id 出参（失败返回 nullptr）
3. ResetMemory 清零 data_，依次设置 page_id_、pin_count_ = 1、is_dirty_ = false
4. page_table_ 插入映射，replacer_->Pin(frame_id)，返回 Page 指针

**关键设计决策**：步骤 1 必须在步骤 2 之前。若顺序互换——先分配磁盘页号再查找槽位——当 TryToFindFreePage 返回失败时，已分配的磁盘页号已被 BitmapPage 标记为占用，但未进入任何 BPM 槽位，造成页号永久泄漏，后续分配的页号不再连续。这一 bug 曾导致测试失败（期望 page_id = 10，实际得到 20）。

### 6.6 UnpinPage

**接口**：`bool UnpinPage(page_id_t page_id, bool is_dirty)`

1. 在 page_table_ 中查找 page_id，不存在返回 false
2. 若 pin_count_ ≤ 0，返回 false（防重复 Unpin）
3. 若 is_dirty 为 true，将 Page 的 is_dirty_ 设为 true
4. pin_count_ 自减。若减至 0：replacer_->Unpin(frame_id)，将此槽位加入淘汰候补
5. 返回 true

**关键约束**：replacer_->Unpin 仅在 pin_count_ 恰好归零时调用。若 pin_count_ 从 3 降至 2 也调 Unpin，则该页在仍被引用时进入淘汰名单，可能被 Victim 驱逐，其余持有者将访问到错误数据。

### 6.7 FlushPage

**接口**：`bool FlushPage(page_id_t page_id)`

在 page_table_ 中查找 page_id，不存在返回 false。调 disk_manager_->WritePage 将该页的 data_ 写回磁盘，然后将 is_dirty_ 设为 false，返回 true。写盘操作不论 pin 状态均执行。

### 6.8 DeletePage

**接口**：`bool DeletePage(page_id_t page_id)`

**分支 A**：页不在内存（page_table_.find 失败）。调 disk_manager_->DeAllocatePage 从磁盘位图释放该页，返回 true。

**分支 B**：页在内存。若 pin_count_ > 0 返回 false（有人引用，不可删除）。否则：从 page_table_ 中 erase 该映射；调 replacer_->Pin(frame_id) 确保槽位不在淘汰名单；ResetMemory 清零 data_；将 page_id_ 复位为 INVALID_PAGE_ID，pin_count_ 和 is_dirty_ 归零；将 frame_id 归还 free_list_；调 disk_manager_->DeAllocatePage 从磁盘释放。返回 true。

---

## 七、测试方案与结果

### 7.1 课程组提供的测试

| 测试套件 | 测试用例 | 覆盖内容 |
|----------|----------|----------|
| DiskManagerTest | BitMapPageTest | BitmapPage 全页分配/释放/回收，512 字节页大小 |
| DiskManagerTest | FreePageAllocationTest | DiskManager 跨两个 Extent 分配、MetaPage 计数、部分释放 |
| LRUReplacerTest | SampleTest | Pin/Unpin/Victim 基本顺序，重复 Unpin 处理 |
| BufferPoolManagerTest | BinaryDataTest | NewPage/FetchPage/UnpinPage/FlushPage 端到端数据一致性 |

### 7.2 自行补充的测试

| 测试套件 | 测试用例数 | 覆盖内容 |
|----------|-----------|----------|
| BitmapPageStudentTest | 5 | PAGE_SIZE=4096 真实分配、越界拒绝（分配/释放/查询）、重复释放防御、释放后回收复用 |
| DiskManagerStudentTest | 3 | IsPageFree 语义、页号回收、跨 Extent 分配连续正确性 |
| LRUReplacerStudentTest | 7 | Victim 空队列、Pin 不存在元素、重复 Unpin、Pin/Unpin/Victim 交替、frame_id=0 边界、Size 一致性 |
| BufferPoolManagerStudentTest | 10 | DeletePage 完整流程与防误删、槽位回收、满池错误路径、UnpinPage/FlushPage 非法输入拒绝、脏页淘汰后数据完整性、CheckAllUnpinned |

### 7.3 测试结果

编译环境：g++ 11.4.0，cmake 3.28.1，Debug 模式（-g -O0）。全部 29 个测试用例通过，0 失败。编译过程仅有标准库相关的 harmless warnings。

---

## 八、遇到的问题与解决方案

| 序号 | 问题描述 | 原因 | 解决方案 |
|------|---------|------|---------|
| 1 | MapPageId 公式中常数写为 +1 | 忽略了 Meta Page（物理页 0）也占一个位置 | 改为 +2 |
| 2 | AllocatePage 循环上限用 GetFileSize/PAGE_SIZE | 量纲混淆：文件总物理页数 ≠ Extent 数量 | 改用 meta->num_extents_ |
| 3 | 读 BitmapPage 时物理地址传 extent_id | extent_id = 0 时读到 Meta Page | 改用公式 1 + extent_id × (BITMAP_SIZE + 1) |
| 4 | 新建 Extent 时物理地址计算为 0 | GetFileSize/PAGE_SIZE 在空文件时返回 0 | 改用 meta->num_extents_ 作为 Extent ID，代入标准公式 |
| 5 | NewPage 先 AllocatePage 再 TryToFindFreePage | 缓冲池满时页号泄漏 | 交换两步顺序，先确认有槽位再分配磁盘页 |
| 6 | FetchPage/NewPage 中 page_table_ 插入早于 page_id_ 赋值 | GetPageId() 返回旧值 | 先改 page_id_，再插入映射 |
| 7 | FetchPage 缺 ReadPage 调用 | 遗漏磁盘加载 | 在槽位设置阶段加入 ReadPage |
| 8 | UnpinPage 无条件调 replacer_->Unpin | pin_count_ 未归零即放入淘汰名单 | 加判断：仅 pin_count_ == 0 时调用 |
| 9 | DeletePage 变量未声明、DeallocatePage 时机过早 | 编译错误 + 逻辑错误 | 重新组织函数流程 |
| 10 | TryToFindFreePage free_list 路径缺 return | 遗漏返回语句 | 补加 return frame_id |

---

## 九、附录：关键常量

| 常量 | 值 | 含义 |
|------|-----|------|
| PAGE_SIZE | 4096 | 数据页字节数 |
| META_PAGE_ID | 0 | DiskFileMetaPage 所在的物理页号 |
| INVALID_PAGE_ID / INVALID_FRAME_ID | -1 | 无效页号 / 无效槽位号 |
| BITMAP_SIZE | 32704 | 单个 Extent 可容纳的数据页数量 |
| MAX_CHARS | 4088 | BitmapPage 中 bytes 数组长度 |
| MAX_VALID_PAGE_ID | (PAGE_SIZE - 8) / 4 × BITMAP_SIZE | 可支持的最大逻辑页号 |
