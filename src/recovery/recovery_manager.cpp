#include "recovery/recovery_manager.h"

void RecoveryManager::Init(CheckPoint &last_checkpoint) {
  persist_lsn_ = last_checkpoint.checkpoint_lsn_;
  active_txns_ = last_checkpoint.active_txns_;
  data_ = last_checkpoint.persist_data_;
}

void RecoveryManager::RedoPhase() {
  // 1. 重放所有日志（含 checkpoint 之前的），从 checkpoint 状态重建完整状态
  for (auto &kv : log_recs_) {
    auto &log = kv.second;
    // 更新事务的最后 LSN（用于 UndoPhase 沿链回滚）
    if (log->txn_id_ != INVALID_TXN_ID)
      active_txns_[log->txn_id_] = log->lsn_;
    switch (log->type_) {
      case LogRecType::kInsert:
        data_[log->ins_key_] = log->ins_val_;
        break;
      case LogRecType::kDelete:
        data_.erase(log->del_key_);
        break;
      case LogRecType::kUpdate:
        data_.erase(log->old_key_);
        data_[log->new_key_] = log->new_val_;
        break;
      case LogRecType::kCommit:
        active_txns_.erase(log->txn_id_);
        break;
      case LogRecType::kAbort: {
        // 回滚该事务的所有操作
        lsn_t lsn = log->prev_lsn_;
        while (lsn != INVALID_LSN) {
          auto it = log_recs_.find(lsn);
          if (it == log_recs_.end()) break;
          auto &rl = it->second;
          if (rl->type_ == LogRecType::kInsert) data_.erase(rl->ins_key_);
          else if (rl->type_ == LogRecType::kDelete) data_[rl->del_key_] = rl->del_val_;
          else if (rl->type_ == LogRecType::kUpdate) {
            data_.erase(rl->new_key_);
            data_[rl->old_key_] = rl->old_val_;
          }
          lsn = rl->prev_lsn_;
        }
        active_txns_.erase(log->txn_id_);
        break;
      }
      default:
        break;
    }
  }
}

void RecoveryManager::UndoPhase() {
  // 2. 对 RedoPhase 结束时仍活跃（未 Commit 也未 Abort）的事务，回滚其全部操作
  for (auto &at : active_txns_) {
    lsn_t lsn = at.second;
    while (lsn != INVALID_LSN) {
      auto it = log_recs_.find(lsn);
      if (it == log_recs_.end()) break;
      auto &log = it->second;
      if (log->type_ == LogRecType::kInsert) data_.erase(log->ins_key_);
      else if (log->type_ == LogRecType::kDelete) data_[log->del_key_] = log->del_val_;
      else if (log->type_ == LogRecType::kUpdate) {
        data_.erase(log->new_key_);
        data_[log->old_key_] = log->old_val_;
      }
      lsn = log->prev_lsn_;
    }
  }
  active_txns_.clear();
}
