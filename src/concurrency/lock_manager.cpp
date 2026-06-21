#include "concurrency/lock_manager.h"

#include <iostream>
#include <thread>

#include "common/rowid.h"
#include "concurrency/txn.h"
#include "concurrency/txn_manager.h"

void LockManager::SetTxnMgr(TxnManager *txn_mgr) { txn_mgr_ = txn_mgr; }

void LockManager::LockPrepare(Txn *txn, const RowId &rid) {
  if (txn->GetState() == TxnState::kAborted) {
    throw TxnAbortException(txn->GetTxnId(), AbortReason::kDeadlock);
  }
  if (txn->GetState() != TxnState::kGrowing) {
    txn->SetState(TxnState::kAborted);
    throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockOnShrinking);
  }
  if (lock_table_.find(rid) == lock_table_.end()) {
    lock_table_.emplace(std::piecewise_construct, std::forward_as_tuple(rid),
                        std::forward_as_tuple());
  }
}

void LockManager::CheckAbort(Txn *txn, LockManager::LockRequestQueue &req_queue) {
  if (txn->GetState() == TxnState::kAborted) {
    req_queue.EraseLockRequest(txn->GetTxnId());
    req_queue.cv_.notify_all();
    throw TxnAbortException(txn->GetTxnId(), AbortReason::kDeadlock);
  }
}

bool LockManager::LockShared(Txn *txn, const RowId &rid) {
  std::unique_lock<std::mutex> lock(latch_);
  LockPrepare(txn, rid);
  if (txn->GetIsolationLevel() == IsolationLevel::kReadUncommitted) {
    txn->SetState(TxnState::kAborted);
    throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockSharedOnReadUncommitted);
  }
  auto &queue = lock_table_[rid];
  queue.EmplaceLockRequest(txn->GetTxnId(), LockMode::kShared);
  // Wait if there's an exclusive writer or pending upgrade
  queue.cv_.wait(lock, [&]() {
    CheckAbort(txn, queue);
    return !queue.is_writing_ && !queue.is_upgrading_;
  });
  auto iter = queue.GetLockRequestIter(txn->GetTxnId());
  iter->granted_ = LockMode::kShared;
  queue.sharing_cnt_++;
  txn->GetSharedLockSet().emplace(rid);
  return true;
}

bool LockManager::LockExclusive(Txn *txn, const RowId &rid) {
  std::unique_lock<std::mutex> lock(latch_);
  LockPrepare(txn, rid);
  auto &queue = lock_table_[rid];
  queue.EmplaceLockRequest(txn->GetTxnId(), LockMode::kExclusive);
  // Build wait-for edges to all current holders
  for (auto &req : queue.req_list_) {
    if (req.txn_id_ != txn->GetTxnId() && req.granted_ != LockMode::kNone) {
      AddEdge(txn->GetTxnId(), req.txn_id_);
    }
  }
  // Wait until no one holds the lock
  queue.cv_.wait(lock, [&]() {
    CheckAbort(txn, queue);
    return queue.sharing_cnt_ == 0 && !queue.is_writing_;
  });
  auto iter = queue.GetLockRequestIter(txn->GetTxnId());
  iter->granted_ = LockMode::kExclusive;
  queue.is_writing_ = true;
  txn->GetExclusiveLockSet().emplace(rid);
  // Remove wait-for edges that were added
  for (auto &req : queue.req_list_) {
    if (req.txn_id_ != txn->GetTxnId()) {
      RemoveEdge(txn->GetTxnId(), req.txn_id_);
    }
  }
  return true;
}

bool LockManager::LockUpgrade(Txn *txn, const RowId &rid) {
  std::unique_lock<std::mutex> lock(latch_);
  LockPrepare(txn, rid);
  auto &queue = lock_table_[rid];
  // 若已有其他事务在升级，直接拒绝
  if (queue.is_upgrading_) {
    txn->SetState(TxnState::kAborted);
    throw TxnAbortException(txn->GetTxnId(), AbortReason::kUpgradeConflict);
  }
  queue.is_upgrading_ = true;
  // Add edges to any other shared holders
  for (auto &req : queue.req_list_) {
    if (req.txn_id_ != txn->GetTxnId() && req.granted_ == LockMode::kShared) {
      AddEdge(txn->GetTxnId(), req.txn_id_);
    }
  }
  // 等待：直到自己是唯一的共享持有者（其他共享锁全部释放）
  queue.cv_.wait(lock, [&]() {
    CheckAbort(txn, queue);
    return queue.sharing_cnt_ == 1;  // 只剩自己
  });
  // 成功升级：移除 shared lock，改为 exclusive
  auto iter = queue.GetLockRequestIter(txn->GetTxnId());
  queue.sharing_cnt_--;
  txn->GetSharedLockSet().erase(rid);
  iter->lock_mode_ = LockMode::kExclusive;
  iter->granted_ = LockMode::kExclusive;
  queue.is_writing_ = true;
  queue.is_upgrading_ = false;
  txn->GetExclusiveLockSet().emplace(rid);
  // Remove edges
  for (auto &req : queue.req_list_) {
    if (req.txn_id_ != txn->GetTxnId()) {
      RemoveEdge(txn->GetTxnId(), req.txn_id_);
    }
  }
  return true;
}

bool LockManager::Unlock(Txn *txn, const RowId &rid) {
  std::unique_lock<std::mutex> lock(latch_);
  auto it = lock_table_.find(rid);
  if (it == lock_table_.end()) return false;
  auto &queue = it->second;
  auto iter = queue.GetLockRequestIter(txn->GetTxnId());
  if (iter->granted_ == LockMode::kShared) {
    queue.sharing_cnt_--;
    txn->GetSharedLockSet().erase(rid);
  } else if (iter->granted_ == LockMode::kExclusive) {
    queue.is_writing_ = false;
    txn->GetExclusiveLockSet().erase(rid);
  }
  queue.EraseLockRequest(txn->GetTxnId());
  queue.cv_.notify_all();
  // Two-phase locking: first unlock moves to shrinking
  if (txn->GetState() == TxnState::kGrowing) {
    txn->SetState(TxnState::kShrinking);
  }
  return true;
}

// ==================== Deadlock Detection ====================

void LockManager::AddEdge(txn_id_t t1, txn_id_t t2) {
  waits_for_[t1].insert(t2);
}

void LockManager::RemoveEdge(txn_id_t t1, txn_id_t t2) {
  auto it = waits_for_.find(t1);
  if (it != waits_for_.end()) {
    it->second.erase(t2);
    if (it->second.empty()) waits_for_.erase(it);
  }
}

// DFS helper: returns true if cycle found
static bool DFS(txn_id_t node,
                std::unordered_map<txn_id_t, std::set<txn_id_t>> &graph,
                std::unordered_set<txn_id_t> &visited,
                std::unordered_set<txn_id_t> &in_stack,
                std::vector<txn_id_t> &path) {
  visited.insert(node);
  in_stack.insert(node);
  path.push_back(node);
  if (graph.find(node) != graph.end()) {
    // Explore neighbors in ascending order (deterministic)
    std::vector<txn_id_t> neighbors(graph[node].begin(), graph[node].end());
    std::sort(neighbors.begin(), neighbors.end());
    for (auto neighbor : neighbors) {
      if (in_stack.find(neighbor) != in_stack.end()) {
        // Found cycle — find youngest in cycle
        path.push_back(neighbor);
        return true;
      }
      if (visited.find(neighbor) == visited.end()) {
        if (DFS(neighbor, graph, visited, in_stack, path)) return true;
      }
    }
  }
  path.pop_back();
  in_stack.erase(node);
  return false;
}

bool LockManager::HasCycle(txn_id_t &newest_tid_in_cycle) {
  std::unordered_set<txn_id_t> visited;
  std::unordered_set<txn_id_t> in_stack;
  // Collect all nodes, sorted for deterministic order
  std::vector<txn_id_t> nodes;
  for (auto &p : waits_for_) nodes.push_back(p.first);
  std::sort(nodes.begin(), nodes.end());

  for (auto node : nodes) {
    if (visited.find(node) == visited.end()) {
      std::vector<txn_id_t> path;
      if (DFS(node, waits_for_, visited, in_stack, path)) {
        // Find the youngest (largest) txn_id in the cycle
        newest_tid_in_cycle = *std::max_element(path.begin(), path.end());
        return true;
      }
    }
  }
  return false;
}

void LockManager::DeleteNode(txn_id_t txn_id) {
  waits_for_.erase(txn_id);
  auto *txn = txn_mgr_->GetTransaction(txn_id);
  for (const auto &rid : txn->GetSharedLockSet()) {
    for (const auto &req : lock_table_[rid].req_list_) {
      if (req.granted_ == LockMode::kNone) RemoveEdge(req.txn_id_, txn_id);
    }
  }
  for (const auto &rid : txn->GetExclusiveLockSet()) {
    for (const auto &req : lock_table_[rid].req_list_) {
      if (req.granted_ == LockMode::kNone) RemoveEdge(req.txn_id_, txn_id);
    }
  }
}

void LockManager::RunCycleDetection() {
  while (enable_cycle_detection_) {
    std::this_thread::sleep_for(cycle_detection_interval_);
    std::unique_lock<std::mutex> lock(latch_);
    // Build fresh wait-for graph
    waits_for_.clear();
    for (auto &kv : lock_table_) {
      auto &queue = kv.second;
      for (auto &req : queue.req_list_) {
        if (req.granted_ == LockMode::kNone) {
          for (auto &holder : queue.req_list_) {
            if (holder.txn_id_ != req.txn_id_ && holder.granted_ != LockMode::kNone) {
              waits_for_[req.txn_id_].insert(holder.txn_id_);
            }
          }
        }
      }
    }
    // Detect and break all cycles
    while (true) {
      txn_id_t victim = INVALID_TXN_ID;
      if (!HasCycle(victim)) break;
      // Abort youngest txn in cycle
      auto *txn = txn_mgr_->GetTransaction(victim);
      txn->SetState(TxnState::kAborted);
      DeleteNode(victim);
      // 通知所有队列——等待中的 CheckAbort 会处理 EraseLockRequest
      for (auto &kv : lock_table_) {
        kv.second.cv_.notify_all();
      }
    }
  }
}

std::vector<std::pair<txn_id_t, txn_id_t>> LockManager::GetEdgeList() {
  std::vector<std::pair<txn_id_t, txn_id_t>> result;
  for (auto &p : waits_for_) {
    for (auto &v : p.second) result.emplace_back(p.first, v);
  }
  return result;
}
