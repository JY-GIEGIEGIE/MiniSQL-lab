# 实验 #7：Lock Manager 设计报告

## 一、模块概述

Lock Manager 是 MiniSQL 的并发控制组件，负责管理事务对数据记录（RowId）的锁请求，支持共享锁（Shared）和排他锁（Exclusive），实现两阶段锁协议（2PL），并通过后台死锁检测线程自动打破等待环。本模块属于 Bonus 内容，独立于验收流程，涉及锁和条件变量的多线程编程。

### 1.1 核心结构

- **LockRequest**：单个锁请求——记录 txn_id、请求锁类型（lock_mode_）和已授予锁类型（granted_）
- **LockRequestQueue**：每个 RowId 对应一个请求队列——包含 `req_list_`（等待队列）、`req_list_iter_map_`（快速定位事务迭代器）、`cv_`（条件变量）、`is_writing_`（是否持有排他锁）、`is_upgrading_`（是否正在升级）、`sharing_cnt_`（共享锁持有计数）
- **lock_table_**：`unordered_map<RowId, LockRequestQueue>`——全局锁表
- **waits_for_**：`unordered_map<txn_id, set<txn_id>>`——等待图，用于死锁检测

---

## 二、锁管理算法

### 2.1 LockPrepare

所有锁操作的前置校验：
1. 若 txn 已 Aborted → 直接抛 `kDeadlock` 异常
2. 若 txn 非 Growing 状态 → 设置 txn 为 Aborted → 抛 `kLockOnShrinking` 异常（两阶段锁：只有 Growing 阶段可获取新锁）
3. 若 lock_table_ 中尚无该 RowId 的队列 → 创建

### 2.2 LockShared

1. LockPrepare 校验
2. READ_UNCOMMITTED 隔离级别不允许读锁 → 抛 `kLockSharedOnReadUncommitted`
3. 将共享锁请求加入队列
4. 等待直到无人持有排他锁且无正在升级（`!is_writing_ && !is_upgrading_`）
5. 授予共享锁：`granted_ = kShared`，`sharing_cnt_++`，加入 `txn->shared_lock_set_`

### 2.3 LockExclusive

1. LockPrepare 校验
2. 将排他锁请求加入队列
3. 构建等待边：对当前所有持有锁的事务添加 `txn_id → holder_id` 边
4. 等待直到无人持有任何锁（`sharing_cnt_ == 0 && !is_writing_`）
5. 移除等待边，授予排他锁：`granted_ = kExclusive`，`is_writing_ = true`，加入 `txn->exclusive_lock_set_`

### 2.4 LockUpgrade

1. LockPrepare 校验
2. 若已有其他事务在升级（`is_upgrading_`）→ 抛 `kUpgradeConflict`
3. 设置 `is_upgrading_ = true`
4. 构建等待边（对其他共享锁持有者）
5. 等待直到自己是唯一的共享持有者（`sharing_cnt_ == 1`）
6. 升级成功：`sharing_cnt_--`，移除 shared_lock_set_，设置 `granted_ = kExclusive`，`is_writing_ = true`，加入 exclusive_lock_set_

**设计要点**：升级成功前不提前移除 shared_lock_set_ 中的记录——确保 Abort 时 TxnManager::ReleaseLocks 能找到并释放该锁。

### 2.5 Unlock

1. 从队列中移除锁请求
2. 更新 `is_writing_` / `sharing_cnt_` / 事务锁集合
3. `cv_.notify_all()` 唤醒等待者
4. 若 txn 状态为 Growing → 变为 Shrinking（首次 Unlock 触发两阶段锁收缩）

### 2.6 CheckAbort

在条件变量的等待 lambda 中周期性检查：若 txn 已被外部设为 Aborted（如死锁检测），则擦除其锁请求、通知队列、抛异常。

---

## 三、死锁检测

### 3.1 等待图维护

- **AddEdge(t1, t2)**：在 `waits_for_[t1]` 中插入 t2
- **RemoveEdge(t1, t2)**：从 `waits_for_[t1]` 中移除 t2，若集合为空则擦除 t1 条目
- **DeleteNode(txn_id)**：擦除 txn_id 的所有出入边，同时移除等待该 txn 的未授予请求的边

### 3.2 HasCycle——确定性 DFS

1. 收集所有节点，按 txn_id 升序排序
2. 按升序从每个未访问节点启动 DFS
3. 探索邻居时按 txn_id 升序（保证确定性）
4. 使用 in_stack 集合检测回边——找到环时记录环中 txn_id 最大者（最年轻事务）
5. 返回第一个发现的环

### 3.3 RunCycleDetection——后台线程

1. 按 `cycle_detection_interval_` 周期睡眠
2. 获取全局锁 `latch_`
3. **每次从头构建等待图**（非增量维护）——遍历 lock_table_ 中所有队列，对每个未授予的请求，添加其到所有已授予持有者的边
4. 循环调 `HasCycle` → 找到环 → 将最年轻事务标记为 Aborted → DeleteNode+通知所有 cv → 继续检查下一个环（直到无环）
5. 释放锁

---

## 四、测试结果

`lock_manager_test.cpp` 全部 10 个测试通过：

| 测试 | 覆盖内容 |
|------|----------|
| SLockInReadUncommittedTest | READ_UNCOMMITTED 拒绝共享锁 |
| TwoPhaseLockingTest | Growing→Shrinking 状态转换、Shrinking 阶段拒绝新锁 |
| UpgradeLockInShrinkingPhase | Shrinking 阶段拒绝升级 |
| UpgradeConflictTest | 两个事务同时升级时后者的冲突拒绝 |
| UpgradeTest | 正常的共享锁升级为排他锁 |
| UpgradeAfterAbortTest | 升级等待中事务被 Abort 的正确处理 |
| BasicCycleTest1/2 | 简单环和复杂环的 DFS 检测正确性 |
| DeadlockDetectionTest1/2 | 后台死锁检测线程的端到端测试 |

---

## 五、思考题

**如果 Lock Manager 不独立出来，如何为 B+ 树实现并发控制？**

需要修改的模块和函数：

1. **BPlusTree 页级锁**：在 `FindLeafPage` 中，从根向下遍历时使用锁耦合（Latch Crabbing）——对子结点加读锁后释放父结点锁；Insert/Remove 写操作对沿途结点加写锁。需利用 BPlusTreePage 已有的 `WLatch()`/`WUnlatch()`/`RLatch()`/`RUnlatch()` 接口（ReaderWriterLatch）

2. **LockManager 集成**：LockManager 当前管理的是 RowId 级别的锁（记录锁）。B+ 树的并发控制需要的是页级锁——需要对 LockManager 扩展支持 page_id 级别的锁，或在 BPlusTree 内部独立管理页锁

3. **死锁检测扩展**：当前死锁检测基于 `waits_for_` 图（txn→txn）。若引入页锁，页级的等待关系（txn 等待 page latch）也需要纳入等待图，否则无法检测涉及页锁和记录锁的混合死锁

4. **隔离级别实现**：不同隔离级别对 B+ 树的锁策略不同——READ_COMMITTED 可尽早释放读锁（减少锁竞争），REPEATABLE_READ 需持有读锁到事务结束。需要在 `FindLeafPage` 中根据 `txn->GetIsolationLevel()` 决定是否在遍历过程中释放读锁

5. **乐观锁（Optimistic Lock Coupling）**：B+ 树的 Insert/Remove 操作可先乐观地假设不会分裂/合并，使用读锁向下遍历，仅在确认需要修改时升级为写锁。如发现页不安全（可能分裂/合并），则释放所有锁，用写锁重新遍历
