# 实验 #1：Disk and Buffer Pool Manager 工作记录

## 一、模块关系总览

实验 #1 实现四个组件，从底层到上层依次是 BitmapPage、DiskManager、LRUReplacer、BufferPoolManager。

BitmapPage 管理一个 Extent 内部的页分配状态。DiskManager 持有多个 Extent，负责逻辑页号到物理页号的映射，以及全局的页分配和回收。LRUReplacer 是 BPM 的辅助组件，维护一个淘汰候补队列，按最近使用顺序决定踢谁。BufferPoolManager 是最终面向外部的接口层，用固定大小的 Page 数组缓存磁盘页，对外提供 FetchPage、NewPage、UnpinPage、DeletePage、FlushPage 六个操作。

---

## 二、BitmapPage

BitmapPage 是一个模板类，模板参数是 PageSize。它在磁盘上占用 PageSize 个字节，内存布局和磁盘格式完全一致——前两个 uint32_t 是 page_allocated_ 和 next_free_page_，后面紧跟着 bytes 数组。bytes 数组的长度 MAX_CHARS 等于 PageSize 减去 8（两个 uint32_t 各占 4 字节）。这个类的最大管理容量 GetMaxSupportedSize 等于 MAX_CHARS 乘以 8。

cpp 文件末尾做了显式实例化，PageSize 从 64 到 4096，覆盖了所有可能用到的页大小。

### 2.1 AllocatePage

入参是 page_offset，类型是 uint32_t 的引用——它是出参，函数把分配结果写进这个变量。返回值是 bool，表示成功或失败。

函数体分三个部分。

第一部分，快速失败检查。如果成员变量 page_allocated_ 已经大于等于 GetMaxSupportedSize 的返回值，说明这个 Extent 里所有页都被分配了，直接返回 false。

第二部分，两层循环找空闲位。外层的循环变量是 i，从 0 到 MAX_CHARS 减 1，遍历 bytes 数组的每一个元素。内层循环的变量是 j，从 0 到 7，遍历一个字节的八个位。每一轮迭代检查 bytes[i] 的从低到高的第 j 位是否为零。具体做法是：把 1 左移 j 位得到一个只有第 j 位为 1 的掩码，和 bytes[i] 做按位与。如果结果为 0，说明这一位是空闲的。外层循环里有一个剪枝优化——先用 bytes[i] 和 0xFF 比较，如果相等说明这一字节的八个位全是 1，八个页全被占了，直接 continue 到下一个字节，不用进内层。

找到空闲位之后依次做五件事：第一，把这一位置 1——bytes[i] 按位或上 (1 左移 j 位)。第二，计算 page_offset：i 乘以 8 加 j，写入出参。第三，page_allocated_ 自增一。第四，把 next_free_page_ 设为 page_offset 加 1，作为下次分配的扫描起点。第五，返回 true。

第三部分是兜底的 return false。如果两层循环跑完了都没返回，说明 page_allocated_ 的计数和实际位图状态不一致——正常情况第一部分的检查已经拦住了，这里只是安全网。

出参 page_offset 在被赋值之前的初始值完全不重要——函数进入第二部分后找到空位就直接覆盖它，不会先读它。这也是为什么 DiskManager 里声明 page_offset 时可以不初始化。

### 2.2 DeAllocatePage

入参是 page_offset，值类型 uint32_t，表示要释放的页在 Extent 内的偏移量。返回值是 bool。

第一步，越界检查。如果 page_offset 大于等于 GetMaxSupportedSize，返回 false。

第二步，把 page_offset 拆成字节下标和位下标。字节下标等于 page_offset 除以 8，位下标等于 page_offset 对 8 取模。

第三步，重复释放检查。调 IsPageFreeLow，把字节下标和位下标传进去。如果返回 true——说明这一位已经是 0 了，已经是空闲的——返回 false，不重复释放。

第四步，清除目标位。先构造掩码：把 1 左移位下标位，得到一个只有目标位为 1 的 uint8_t。再按位取反——注意 C++ 里取反操作符会把 uint8_t 整型提升为 int，所以取反结果的高位全是 1，但赋回 uint8_t 时会截断，只保留低 8 位。把 bytes[字节下标] 和取反后的掩码做按位与，结果就是目标位清零而其他位不变。

第五步，page_allocated_ 自减一。

第六步，更新查找提示。如果 page_offset 小于当前的 next_free_page_，把 next_free_page_ 更新为 page_offset——这样下次分配优先从更靠前的空位搜起。返回 true。

### 2.3 IsPageFree 和 IsPageFreeLow

IsPageFree 是公开接口，入参是 page_offset，返回值 bool。第一步越界检查——page_offset 大于等于 GetMaxSupportedSize 时返回 false。第二步拆成字节下标和位下标，转调 IsPageFreeLow。

IsPageFreeLow 是私有方法，入参是字节下标（uint32_t）和位下标（uint8_t）。直接把 1 左移位下标位后和 bytes[字节下标] 做按位与，判断结果是否等于 0——等于 0 就是空闲，返回 true。

---

## 三、DiskManager

DiskManager 管理整个数据库文件的页分配。构造函数打开或创建 db_file，然后把物理页 0（META_PAGE_ID）读到成员变量 meta_data_ 里——meta_data_ 是一个 PAGE_SIZE 字节的 char 数组，缓存着 DiskFileMetaPage 的数据。meta_data_ 的内容在析构/Close 时写回磁盘。

DiskManager 内部使用了递归互斥锁 db_io_latch_，所有涉及读写磁盘的公开方法都先用 scoped_lock 加锁，保证单线程场景下的安全性（多线程场景由上层 BPM 的锁负责）。ReadPage 和 WritePage 是公开的读写接口，入参是逻辑页号，内部先调 MapPageId 转为物理页号再调 ReadPhysicalPage/WritePhysicalPage。

### 3.1 物理文件结构

数据库文件按 PAGE_SIZE 字节切分。物理页 0 是 DiskFileMetaPage——里面存着 num_allocated_pages_（全局已分配页总数）、num_extents_（已有的 Extent 数量）和一个柔性数组 extent_used_page_（每个 Extent 的已用页数，按 Extent ID 索引）。

物理页 0 之后是 Extent 序列。一个 Extent 包含一张 BitmapPage 和 BITMAP_SIZE 个数据页，BITMAP_SIZE 的值等于 BitmapPage 模板实例化后的 GetMaxSupportedSize——大约 32704。Extent i 的 BitmapPage 位于物理页 1 + i × (BITMAP_SIZE + 1)，它的数据页从物理页 2 + i × (BITMAP_SIZE + 1) 开始依次排列。

数据页用逻辑页号连续编号，只给真正存数据的页编号，Meta Page 和 BitmapPage 不占逻辑页号。Extent i 内的第 k 个数据页（k 从 0 开始）对应的逻辑页号是 i × BITMAP_SIZE + k。

### 3.2 MapPageId

入参是逻辑页号，返回值是物理页号。公式是：逻辑页号加上逻辑页号除以 BITMAP_SIZE 再加上 2。

其中逻辑页号除以 BITMAP_SIZE 就是 Extent 编号——有多少个完整 Extent 排在前面。加 2 的来源：加 1 跳过 Meta Page，再加 1 跳过当前 Extent 自己的 BitmapPage。

举个例子验证：逻辑页 0 属于 Extent 0 的第 0 个数据页，物理位置应该是 2。代入公式：0 加 0 加 2 等于 2，正确。

### 3.3 AllocatePage

无入参，返回值是逻辑页号（page_id_t）。函数做两件事——先尝试在已有 Extent 中找空闲页，找不到就新建 Extent。整段逻辑在 scoped_lock 保护下执行。

**已有 Extent 中找空闲页：**

先从 meta_data_ 拿到 DiskFileMetaPage 指针（reinterpret_cast 转换）。声明一个 for 循环，循环变量 extent_id 从 0 到 meta 的 num_extents_ 减 1。

循环体内的第一步：快速跳过已满的 Extent。调 meta 的 GetExtentUsedPage 传入 extent_id，返回值如果大于等于 BITMAP_SIZE，直接 continue。

第二步，找到没满的 Extent 后，算出它的 BitmapPage 在磁盘上的物理位置。公式是 1 加 extent_id 乘 (BITMAP_SIZE 加 1)。

第三步，声明一个 PAGE_SIZE 大小的 char 数组 buf，把物理页的内容读到 buf 里。然后 reinterpret_cast 把它转成 BitmapPage 指针——之所以能这样做，是因为 BitmapPage 的内存布局（两个 uint32_t 加 char 数组）和磁盘上 BitmapPage 的字节布局完全一致，buf 的前 4 字节就是 page_allocated_，接着 4 字节是 next_free_page_，后面就是 bytes 数组。

第四步，声明一个 uint32_t 变量 page_offset（不需要初始化）。调 bitmap 指针的 AllocatePage 方法，把 page_offset 作为出参传进去。

第五步，如果 AllocatePage 返回 true：把 buf 写回磁盘同一物理位置（WritePhysicalPage），更新 meta 的 num_allocated_pages_（加一）和 extent_used_page_ 对应 extent_id 那项（加一）。然后用 extent_id 乘 BITMAP_SIZE 加 page_offset 拼出逻辑页号，返回。

循环结束说明所有 Extent 都满了。

**新建 Extent：**

新 Extent 的 ID 等于 meta 当前的 num_extents_ 值。同样用公式算出它的 BitmapPage 的物理位置：1 加新 ID 乘 (BITMAP_SIZE 加 1)。

声明一个 PAGE_SIZE 字节的 char 数组 buf，用大括号初始化为全零。全零的 buf 被 reinterpret_cast 为 BitmapPage 后，page_allocated_ 为 0、next_free_page_ 为 0、bytes 全部为零——这正是"一个全新空白 Extent"的状态。

在这个全新的 BitmapPage 上调 AllocatePage，传入 page_offset。因为是全零的，必然成功，且 page_offset 会被赋值为 0（分配到第 0 号位）。把 buf 写盘（WritePhysicalPage 到刚才算出的物理位置，文件会自动扩展）。更新 meta：num_extents_ 加一、num_allocated_pages_ 加一、extent_used_page_ 在新 Extent ID 位置上设为 1。返回逻辑页号——新 Extent ID 乘以 BITMAP_SIZE 加 0。

### 3.4 DeAllocatePage

入参是逻辑页号。第一步，从逻辑页号反推 Extent 编号（除以 BITMAP_SIZE）和 Extent 内偏移（对 BITMAP_SIZE 取模）。第二步，算出该 Extent 的 BitmapPage 的物理位置。第三步，读盘到 buf。第四步，reinterpret_cast 为 BitmapPage 指针。第五步，调 DeAllocatePage 传入 page_offset。如果返回 true：写回 buf，meta 的 num_allocated_pages_ 减一，extent_used_page_ 对应项减一。

### 3.5 IsPageFree

入参是逻辑页号。同样反推 Extent 编号和内部偏移，读 BitmapPage，转调 BitmapPage 的 IsPageFree。不修改任何数据，只返回 bool。

---

## 四、LRUReplacer

LRUReplacer 继承自抽象基类 Replacer，实现四个纯虚函数。它在 BPM 构造时被创建，构造参数 num_pages 是缓冲池大小（当前未使用，因为 list 和 set 动态增长，不需要预分配）。

内部有两个私有成员：lru_list_ 是一个 list，元素类型是 frame_id_t，按 Unpin 的时间顺序排列——最前面（front）是最久没被 Unpin 的，最后面（back）是最近刚 Unpin 的。lru_set_ 是一个 unordered_set，元素类型也是 frame_id_t，存的内容和 lru_list_ 完全一致——它的唯一作用是提供 O(1) 的"成员存在性"查询，避免每次 Pin 时遍历 lru_list_ 做 O(n) 查找。

### 4.1 Victim

入参是 frame_id_t 的指针，出参，函数把淘汰结果写进去。返回值是 bool。

先检查 lru_list_ 是否为空。空则返回 false。不空：取 lru_list_ 的 front 写入出参指针指向的位置，调 pop_front 弹出，调 lru_set_ 的 erase 删除同一元素，返回 true。

### 4.2 Pin

入参是 frame_id_t，值类型。

在 lru_set_ 中查找 frame_id——find 返回的迭代器和 end 比较。如果等于 end，说明不在淘汰名单里（可能已经被 Pin 过了，或者从未 Unpin 过），什么也不做。如果找到了：调 lru_list_ 的 remove 传入 frame_id——remove 遍历整个链表删除匹配元素，O(n)。再调 lru_set_ 的 erase 传入 frame_id——O(1)。

### 4.3 Unpin

入参是 frame_id_t，值类型。

在 lru_set_ 中查找。如果找到了——说明已经在淘汰名单里——什么也不做（防御性处理，正常流程 BPM 只在 pin_count 归零时调一次 Unpin）。如果没找到：调 lru_list_ 的 push_back 把 frame_id 放到链表尾部（表示这是最近使用的），调 lru_set_ 的 insert 加入集合。

### 4.4 Size

直接返回 lru_list_ 的 size，即当前可淘汰的槽位数量。

---

## 五、BufferPoolManager

BPM 是实验 #1 最上层的模块。构造时分配 pool_size_ 个 Page 对象的数组 pages_，new 一个 LRUReplacer，然后把 0 到 pool_size_ 减 1 全部放入 free_list_。此时所有槽位都是空的。析构时遍历 page_table_ 把每个在内存的页 FlushPage 写回磁盘，然后 delete pages_ 和 replacer_。

BPM 的所有公开方法都用 scoped_lock 对递归互斥锁 latch_ 加锁，保护 pages_、page_table_、free_list_ 等共享数据。

Page 类的 BPM 相关字段有四个：page_id_（初始值 INVALID_PAGE_ID）、pin_count_（初始 0）、is_dirty_（初始 false）、data_（PAGE_SIZE 字节的 char 数组，构造函数 ResetMemory 归零）。BPM 是 Page 的友元类，可以直接读写这些私有字段。

### 5.1 TryToFindFreePage

私有方法，返回 frame_id_t。供 FetchPage 和 NewPage 复用，封装了"找一个可用的空槽位"的逻辑。逻辑分三级。

第一级：看 free_list_ 是否为空。不空则取 front，pop_front，直接返回这个 frame_id。

第二级：free_list_ 空了，调 replacer_ 的 Victim 方法。声明一个 frame_id_t 变量 frame_id（未初始化），把它的地址传给 Victim。如果 Victim 返回 false——LRU 队列也是空的——跳到第三级。如果返回 true，说明 frame_id 被填入了被淘汰的槽位号。这个槽位里还装着旧页的数据：通过 pages_[frame_id] 拿到那个 Page 对象的引用。调它的 IsDirty 方法——如果返回 true，说明旧页被修改过还没写盘。此时调 disk_manager_ 的 WritePage，把旧页的 page_id_ 和 data_ 写回磁盘，然后把 is_dirty_ 设为 false。不管脏不脏，都要从 page_table_ 里把旧页的 page_id_ 对应的那条记录 erase 掉——旧页从此不在内存了。返回 frame_id。

第三级：free_list_ 和 replacer 都空了，返回 INVALID_FRAME_ID。这意味着缓冲池里的所有槽位都被 pin 着。

### 5.2 FetchPage

入参是逻辑页号 page_id，返回值是指向 Page 的指针。流程分两大支。

**分支一：页已在内存。** 在 page_table_ 里 find(page_id)，如果迭代器和 end 不等——找到了。取出对应的 frame_id。通过 pages_[frame_id] 拿到 Page 引用。pin_count_ 自增一。调 replacer_ 的 Pin 传入 frame_id——如果此槽位在 LRU 淘汰名单里，会被移除；如果不在（已经被 Pin 过），Pin 内部什么也不做。返回这个 Page 对象的地址。

**分支二：页不在内存。** 调 TryToFindFreePage，返回值赋给 frame_id。如果 frame_id 等于 INVALID_FRAME_ID，返回 nullptr——缓冲池已满且没有可淘汰的页，无法读入新页。

拿到槽位后，通过 pages_[frame_id] 拿到 Page 引用。先调 ResetMemory 把 data_ 数组清零——这是清除旧页残留。然后调 disk_manager_ 的 ReadPage，传入 page_id 和 page.GetData()——从磁盘把目标页的内容读到 data_ 里。接下来依次设置：page_id_ 赋为新 page_id、pin_count_ 赋为 1、is_dirty_ 赋为 false。这三行设置完之后，把 page_id 到 frame_id 的映射插入 page_table_——注意顺序：一定是先改 page_id_ 再插入 page_table_。如果反过来，插入时 page.GetPageId() 返回的是旧值，会在 page_table_ 里建一个错误映射。最后调 replacer_ 的 Pin 传入 frame_id，返回页指针。

### 5.3 NewPage

入参是 page_id_t 的引用，出参，函数把新分配的磁盘页号写入。返回值是 Page 指针。

第一步，调 TryToFindFreePage。如果返回 INVALID_FRAME_ID，直接返回 nullptr，不做任何磁盘操作。这是和 FetchPage 一样的逻辑——重要的是它必须在磁盘分配之前：如果反过来，先调 AllocatePage 分了一个磁盘页号，再发现槽位拿不到只能返回 nullptr，被分配的那个磁盘页号就丢了（被标记为已分配，但没有被任何 BPM 槽位引用，永不再用）。

第二步，拿到 frame_id 后，调 AllocatePage（私有方法，内部转发 disk_manager_ 的 AllocatePage）。AllocatePage 返回的逻辑页号赋给 page_id 出参。如果返回 INVALID_PAGE_ID（磁盘满了），返回 nullptr。

第三步，通过 pages_[frame_id] 拿到 Page 引用。先 ResetMemory 清零 data_——新页的内容应是全零。依次设置 page_id_、pin_count_ 为 1、is_dirty_ 为 false。把 page_id 到 frame_id 的映射插入 page_table_。调 replacer_ 的 Pin 传入 frame_id。返回页指针。

### 5.4 UnpinPage

入参是 page_id（逻辑页号）和 is_dirty（bool，表示调用者是否修改过这个页）。返回值是 bool。

第一步，在 page_table_ 里 find(page_id)。如果找不到，返回 false——这个页不在内存里。

第二步，取出 frame_id，拿到 Page 引用。检查 pin_count_ 是否小于等于 0。如果是，说明这个页没有被 pin 着（或者是重复 Unpin），返回 false。

第三步，如果 is_dirty 参数为 true，把 page 的 is_dirty_ 设为 true。注意：这里不用管 is_dirty 为 false 的情况——因为可能有多个调用者先后 Unpin 同一页，其中只要有一个说脏了，这个页就应该被标记为脏。如果 is_dirty 为 false，不主动把已有的脏标记清除。

第四步，pin_count_ 自减一。减完之后判断 pin_count_ 是否等于 0。如果等于 0，调 replacer_ 的 Unpin 传入 frame_id——把这个槽位放入淘汰候补名单。如果不等于 0——比如说从 3 减到 2——说明还有人在用这个页，什么都不做，返回 true。

这里的关键是 Unpin 入队的条件——必须是 pin_count_ 恰好归零的那一刻。如果 pin_count_ 从 2 减到 1 也调了 Unpin，这个页就会在仍被使用时被放进淘汰名单，之后可能被 Victim 赶走，正在使用它的调用者会读到错误数据。

### 5.5 FlushPage

入参是 page_id，返回值是 bool。

第一步，在 page_table_ 里 find(page_id)。找不到返回 false。

第二步，取出 frame_id，拿到 Page 引用。调 disk_manager_ 的 WritePage，传入 page_id 和 page.GetData()，把数据写回磁盘。然后把 page 的 is_dirty_ 设为 false——已经和磁盘一致了，不再是脏页。返回 true。

注意这里不管 is_dirty_ 之前是 true 还是 false 都调了 WritePage——保证调用者的落盘意图一定生效。同时必须清除 is_dirty_ 标记，否则后续淘汰时可能再次写盘，重复 I/O。

### 5.6 DeletePage

入参是 page_id，返回值是 bool。

第一步，在 page_table_ 里 find(page_id)。有两种情况。

**情况 A：页不在内存。** 直接调 DeallocatePage（私有方法，内部转发 disk_manager_ 的 DeAllocatePage，在磁盘的 BitmapPage 中把对应位清零）。返回 true——内存里没有需要清理的，磁盘释放完成即可。

**情况 B：页在内存。** 取出 frame_id，拿到 Page 引用。先检查 pin_count_ 是否大于 0。大于 0 说明有人正在用这个页，不能删，返回 false。

可以删的话：从 page_table_ 中 erase(page_id)，调 replacer_ 的 Pin 传入 frame_id——确保此槽位不在淘汰名单中（如果恰好在的话）。然后复位这个 frame 的 Page 对象：调 ResetMemory 清零 data_，把 page_id_ 赋为 INVALID_PAGE_ID，pin_count_ 赋为 0，is_dirty_ 赋为 false。把 frame_id 加回 free_list_——它现在又是一个干净的空槽位了。最后调 DeallocatePage 从磁盘释放。返回 true。

### 5.7 私有辅助方法

AllocatePage 直接 return disk_manager_ 的 AllocatePage 的返回值。DeallocatePage 直接转发 disk_manager_ 的 DeAllocatePage。IsPageFree 直接转发 disk_manager_ 的 IsPageFree——供外部查询某个逻辑页在磁盘上是否空闲。

CheckAllUnpinned 是一个调试用方法，遍历 pages_ 数组，检查每个 Page 对象的 pin_count_ 是否为 0。如果有不为 0 的，打印错误日志并返回 false。测试框架在每个测试用例结束时会调这个方法确保没有页被遗忘 Unpin。

---

## 六、物理页布局与关键公式推导

数据库文件以 PAGE_SIZE（4096 字节）为单位组织。第 0 页是 DiskFileMetaPage，存储 Extent 级元数据。之后是 Extent 序列。

单个 Extent 包含一张 BitmapPage 和 BITMAP_SIZE 个数据页。BITMAP_SIZE 由 BitmapPage 模板的 GetMaxSupportedSize 静态方法返回，其值为 PAGE_SIZE 减 8（两个 uint32_t 占 8 字节）再乘 8（每字节 8 位）。以 PAGE_SIZE 为 4096 计算，BITMAP_SIZE 为 (4096 - 8) × 8 = 32704。一个 Extent 在磁盘上占地 (1 + 32704) × 4096 ≈ 128 MB。

逻辑页号只给数据页编号，Meta Page 和 BitmapPage 不占编号。逻辑页号到物理页号的映射推导如下：

Extent i 的 BitmapPage 位于物理位置 P_bitmap_i = 1 + i × (BITMAP_SIZE + 1)。这个公式里，1 是跳过 Meta Page，i × (BITMAP_SIZE + 1) 是跳过前面 i 个完整 Extent（每个 Extent 含一张 BitmapPage 加 BITMAP_SIZE 个数据页）。

Extent i 内的第 k 个数据页（k = page_offset）位于物理位置 P_bitmap_i + 1 + k = 2 + i × (BITMAP_SIZE + 1) + k。

逻辑页号 L 与 Extent 编号 i 和内部偏移 k 的关系是：L = i × BITMAP_SIZE + k，其中 i = L / BITMAP_SIZE，k = L % BITMAP_SIZE。

将 i 和 k 代入物理位置公式得到：Physical = 2 + (L / BITMAP_SIZE) × (BITMAP_SIZE + 1) + (L % BITMAP_SIZE) = L + L / BITMAP_SIZE + 2。

这就是 MapPageId 的公式来源。

---

## 七、关键实现决策与踩坑记录

### 7.1 MapPageId 的常量错误

初版公式写成了 logical + logical / BITMAP_SIZE + 1。代入逻辑页 0 得到物理页 1——但物理页 1 是 Extent 0 的 BitmapPage，不是数据页。少加的那个 1 是 Meta Page。正确应该是 +2。

### 7.2 AllocatePage 循环上限的量纲错误

初版用 GetFileSize(file_name_) / PAGE_SIZE 作为 Extent 遍历的上限。这个值返回的是文件总物理页数——新建文件只有一个 Extent 时，这个数约是三万多（由于被读入的 BitmapPage 写盘扩展）。但实际上 Extent 数量是 meta->num_extents_。用前者做上限会导致 Extent ID 远超出实际数量，循环大量迭代都是在读文件外的空白区域（ReadPhysicalPage 返回全零），虽然最后能找到空闲页，但语义完全错误。

### 7.3 读 BitmapPage 使用错误的物理地址

初版把 Extent 编号直接作为物理页号传给 ReadPhysicalPage——extent_id = 0 时读了物理页 0（Meta Page 而非 BitmapPage）。正确公式是 1 + extent_id × (BITMAP_SIZE + 1)。

### 7.4 新建 Extent 覆盖 Meta Page

初版新建 Extent 时，new_extent_id 用 GetFileSize / PAGE_SIZE 计算——空文件时这个值为 0，导致新 BitmapPage 被写到了物理页 0（Meta Page 的位置），覆盖了元数据。正确做法是 new_extent_id 直接取 meta->num_extents_，然后按公式算出物理位置。

### 7.5 NewPage 中磁盘分配和槽位查找的顺序

初版先调 AllocatePage（磁盘分配），再调 TryToFindFreePage（槽位查找）。测试场景中缓冲池满时，AllocatePage 已经分配并占用了磁盘页号，但 TryToFindFreePage 返回 INVALID_FRAME_ID 导致函数返回 nullptr。那些被分配的磁盘页号没有被任何 BPM 槽位持有，永久丢失。后续真正需要新页时分配到的页号比预期的大，测试的页号断言失败。修复是交换两步顺序——先找槽位，确认有坑位才分配磁盘。

### 7.6 FetchPage / NewPage 中 page_table_ 插入顺序

初版在设置 page_id_ 之前就调了 page_table_[page.GetPageId()] = frame_id。GetPageId 返回的是旧 page_id——在 TryToFindFreePage 的 Victim 路径中旧页已从 page_table_ 移除，但 Page 对象的 page_id_ 字段尚未被修改。结果是旧 ID 被重新登记到 page_table_，而新 ID 从未被登记。修复是将 page_id_ 的赋值放在 page_table_ 插入之前。

### 7.7 FetchPage 缺磁盘读取

初版在拿到槽位后只做了 ResetMemory 清零，没有调 disk_manager_->ReadPage。返回的 Page 的 data_ 是全零而非磁盘上的实际内容。

### 7.8 UnpinPage 无条件调 replacer->Unpin

初版在 pin_count_ 自减后无条件调 replacer_->Unpin。pin_count_ 从 2 减到 1 时页面仍被其他调用者持有，却被放入了淘汰名单。修复是加一个 if 判断——仅在 pin_count_ 减到 0 时才调 Unpin。

### 7.9 UnpinPage 调用了不存在的 MarkDirty 方法

初版在 is_dirty 为 true 时调了 page.MarkDirty()，但 Page 类没有这个方法。BPM 是 Page 的友元类，应该直接对 is_dirty_ 赋值。

### 7.10 DeletePage 中磁盘释放时机

初版 DeallocatePage 调用放在了 pin_count_ 检查之前——即使 pin_count_ > 0 无法删除，磁盘上的页也已经被标记为空闲。修复是移到 pin_count_ 检查通过之后。

### 7.11 TryToFindFreePage 的 free_list 路径缺 return

初版在 free_list_ 非空的 if 分支里取了 frame_id、pop_front，但分支末尾没有 return 语句。执行流继续往下走，最后返回了 INVALID_FRAME_ID。相当于 free_list_ 被弹出了一个元素但调用者拿到的是无效值，这个槽位永久丢失。

### 7.12 FlushPage 未清除脏标记

初版 WritePage 之后没有把 is_dirty_ 设为 false。下次这个槽位被淘汰时，即使数据未变，也会再次落盘。

---

## 八、测试结果

编译环境：g++ 11.4.0，cmake 3.28.1，Debug 模式。编译仅有 harmless warnings。

全部四个测试用例通过：

- DiskManagerTest::BitMapPageTest —— 测试 BitmapPage 的分配、释放、查询正确性，包含边界条件
- DiskManagerTest::FreePageAllocationTest —— 测试 DiskManager 的页分配/释放/映射，跨 Extent 场景
- LRUReplacerTest::SampleTest —— 测试 LRU 的 Pin/Unpin/Victim/Size 基本流程
- BufferPoolManagerTest::BinaryDataTest —— 端到端测试：NewPage 创建页 → 写入随机二进制数据 → Unpin + Flush → 缓冲池满后 NewPage 失败 → Unpin 腾出空间后 NewPage 成功且页号连续 → FetchPage 读回数据验证一致性

---

## 九、验收演示指引

### 9.1 测试运行

依次执行三个测试可执行文件展示全部 PASSED。

### 9.2 架构讲解顺序

自底向上：BitmapPage（单 Extent 内的位图管理，AllocatePage 两层循环剪枝逻辑）→ DiskManager（多 Extent 结构，MapPageId 映射推导，AllocatePage 已有 Extent + 新建 Extent 两趟逻辑）→ LRUReplacer（淘汰队列的 Pin/Unpin/Victim 语义，和 free_list_ 的分工）→ BufferPoolManager（槽位三态，六个公开方法的流程，TryToFindFreePage 的三级优先级查找）。

### 9.3 可能追问的问题

**逻辑页号到物理页号为什么 +2？** 一个 +1 是 Meta Page，另一个 +1 是每个 Extent 自己的 BitmapPage，这两类页不储存用户数据，不分配逻辑页号。

**NewPage 为什么要先找槽位后分配磁盘？** 避免缓冲池满时分配了磁盘页号但无法载入内存，造成页号泄漏。

**UnpinPage 里为什么 pin_count_ 减到 0 才调 replacer->Unpin？** pin_count_ 大于 0 意味着页仍被人持有引用，此时放入淘汰名单可能导致页在使用中被 Victim 驱逐。

**TryToFindFreePage 为什么先查 free_list_ 再调 Victim？** free_list_ 是空槽位，不需要淘汰旧页（无脏页落盘开销），速度更快。LRU 淘汰只是后备方案。

**replacer 和 free_list_ 里的 frame 能同时存在吗？** 不能。一个 frame 在任何时刻只处于三种状态之一：free_list_（空槽位）、LRU 队列（装了页但无人用）、两者都不在（装了页且正在被使用）。
