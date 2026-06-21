# 实验 #6：Recovery Manager 设计报告

## 一、模块概述

Recovery Manager 是一个纯内存的简化数据恢复模块，模拟数据库崩溃后的恢复流程。为了降低耦合度，本模块独立于其他组件——不使用磁盘存储日志，而是用内存中的 LogRec 链表和 unordered_map 模拟 KV Database。恢复过程分为两个阶段：RedoPhase（重放所有日志重建崩溃时刻状态）和 UndoPhase（回滚未提交事务）。

### 1.1 核心概念

- **LogRec**：日志记录，含类型（Insert/Delete/Update/Begin/Commit/Abort）、LSN、前驱 LSN（prev_lsn_）、事务 ID、操作数据
- **LSN（Log Sequence Number）**：全局递增的日志序列号，通过静态变量 `LogRec::next_lsn_` 管理
- **CheckPoint**：检查点，记录某一时刻的数据库快照（persist_data_）和活跃事务列表（active_txns_）
- **active_txns_**：活跃事务映射表（txn_id → 最后一条日志的 LSN），用于 UndoPhase

### 1.2 恢复流程

```
Init(checkpoint)
  → 加载 checkpoint 的 persist_data_ 和 active_txns_
RedoPhase()
  → 按 LSN 顺序重放所有日志到 data_
  → 遇到 Abort 日志时，沿 prev_lsn_ 链回滚该事务的所有操作
  → 遇到 Commit 日志时，从 active_txns_ 中移除该事务
UndoPhase()
  → 对 RedoPhase 结束后仍在 active_txns_ 中的事务（即未 Commit 也未 Abort 的活跃事务）
  → 沿 prev_lsn_ 链逆序回滚其所有操作
  → 清空 active_txns_
```

---

## 二、LogRec 设计

### 2.1 字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| type_ | LogRecType | kInsert/kDelete/kUpdate/kBegin/kCommit/kAbort |
| lsn_ | lsn_t | 全局递增序号 |
| prev_lsn_ | lsn_t | 同事务前一条日志的 LSN，形成回滚链 |
| txn_id_ | txn_id_t | 所属事务 ID |
| ins_key_/ins_val_ | KeyType/ValType | Insert 操作的数据 |
| del_key_/del_val_ | KeyType/ValType | Delete 操作的数据 |
| old_key_/old_val_ | KeyType/ValType | Update 操作的旧值 |
| new_key_/new_val_ | KeyType/ValType | Update 操作的新值 |

### 2.2 LSN 管理

`LogRec::next_lsn_`（静态变量）从 0 开始自增。`LogRec::prev_lsn_map_`（静态 map）维护每个事务的最后一条日志的 LSN。创建日志时自动设置 lsn_ 和 prev_lsn_，更新 map。

### 2.3 CreateXxxLog 函数

六个工厂函数（CreateInsertLog、CreateDeleteLog、CreateUpdateLog、CreateBeginLog、CreateCommitLog、CreateAbortLog）各自填充对应的 type_、txn_id_、操作数据和 LSN 链。BeginLog 的 prev_lsn_ 固定为 INVALID_LSN（事务起始）。

---

## 三、RecoveryManager 算法

### 3.1 Init

保存 checkpoint 的 `persist_lsn_`（用于判断后续日志范围）、`active_txns_`（活跃事务映射）、`data_`（数据快照）作为恢复起点。

### 3.2 RedoPhase

1. 按 LSN 顺序遍历 `log_recs_` 中的每条日志
2. 对每条日志更新 `active_txns_[txn_id] = lsn_`（记录事务的最新 LSN，供 UndoPhase 沿链回滚）
3. Insert → `data_[key] = val`；Delete → `data_.erase(key)`；Update → `data_.erase(old_key); data_[new_key] = val`
4. Commit → `active_txns_.erase(txn_id)`
5. Abort → 沿该日志的 `prev_lsn_` 链逆序回滚该事务所有操作（undo Insert → erase key；undo Delete → restore val；undo Update → restore old），然后 `active_txns_.erase(txn_id)`

### 3.3 UndoPhase

对 RedoPhase 结束后仍在 `active_txns_` 中的事务（即从未 Commit 或 Abort 的悬空事务），沿 `prev_lsn_` 链逆序回滚其所有操作。完成后清空 `active_txns_`。

---

## 四、测试结果

`recovery_manager_test.cpp` 中 RecoveryTest 通过。测试覆盖 11 条日志、3 个事务、checkpoint 前后操作、RedoPhase 和 UndoPhase 的完整恢复流程。

---

## 五、思考题

**如果不独立拆出 Recovery Manager，如何设计真正的故障恢复？**

需要修改的模块和函数：

1. **DiskManager / BufferPoolManager**：每个 Page 头部需增加 LSN 字段（Page 已有 `lsn_`）——在写入 Page 前，将当前事务的 LSN 写入页头；BufferPoolManager::FlushPage 时比较页 LSN 与日志 LSN 决定是否需要 WAL 先行写入
2. **LogRec 持久化**：LogRec 需要序列化写入日志文件（WAL file），而非仅在内存中维护。需新增 `LogManager` 类管理日志文件的写入、读取、checkpoint 生成
3. **TablePage / BPlusTreePage**：插入/删除/更新时需生成对应的 LogRec 并写入 LogManager
4. **CheckPoint 机制**：定期生成——记录当前 `active_txns_` 和 `dirty_page_table_`（哪些页被修改过及对应的 LSN）。checkpoint 本身也需持久化
5. **恢复流程**：启动时读取最近一次 checkpoint → RedoPhase 从 checkpoint LSN 开始重放日志 → 对 checkpoint 时活跃的事务执行 UndoPhase
