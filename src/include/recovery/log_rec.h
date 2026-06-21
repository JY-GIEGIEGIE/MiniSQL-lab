#ifndef MINISQL_LOG_REC_H
#define MINISQL_LOG_REC_H

#include <unordered_map>
#include <utility>

#include "common/config.h"
#include "common/rowid.h"
#include "record/row.h"

enum class LogRecType {
  kInvalid,
  kInsert,
  kDelete,
  kUpdate,
  kBegin,
  kCommit,
  kAbort,
};

using KeyType = std::string;
using ValType = int32_t;

struct LogRec {
  LogRec() = default;

  LogRecType type_{LogRecType::kInvalid};
  lsn_t lsn_{INVALID_LSN};
  lsn_t prev_lsn_{INVALID_LSN};
  txn_id_t txn_id_{INVALID_TXN_ID};
  // data fields for insert/delete/update
  KeyType ins_key_{};
  ValType ins_val_{};
  KeyType del_key_{};
  ValType del_val_{};
  KeyType old_key_{};
  ValType old_val_{};
  KeyType new_key_{};
  ValType new_val_{};

  static std::unordered_map<txn_id_t, lsn_t> prev_lsn_map_;
  static lsn_t next_lsn_;
};

std::unordered_map<txn_id_t, lsn_t> LogRec::prev_lsn_map_ = {};
lsn_t LogRec::next_lsn_ = 0;

typedef std::shared_ptr<LogRec> LogRecPtr;

static LogRecPtr CreateInsertLog(txn_id_t txn_id, KeyType ins_key, ValType ins_val) {
  auto rec = std::make_shared<LogRec>();
  rec->type_ = LogRecType::kInsert;
  rec->txn_id_ = txn_id;
  rec->ins_key_ = ins_key;
  rec->ins_val_ = ins_val;
  rec->lsn_ = LogRec::next_lsn_++;
  rec->prev_lsn_ = LogRec::prev_lsn_map_[txn_id];
  LogRec::prev_lsn_map_[txn_id] = rec->lsn_;
  return rec;
}

static LogRecPtr CreateDeleteLog(txn_id_t txn_id, KeyType del_key, ValType del_val) {
  auto rec = std::make_shared<LogRec>();
  rec->type_ = LogRecType::kDelete;
  rec->txn_id_ = txn_id;
  rec->del_key_ = del_key;
  rec->del_val_ = del_val;
  rec->lsn_ = LogRec::next_lsn_++;
  rec->prev_lsn_ = LogRec::prev_lsn_map_[txn_id];
  LogRec::prev_lsn_map_[txn_id] = rec->lsn_;
  return rec;
}

static LogRecPtr CreateUpdateLog(txn_id_t txn_id, KeyType old_key, ValType old_val,
                                  KeyType new_key, ValType new_val) {
  auto rec = std::make_shared<LogRec>();
  rec->type_ = LogRecType::kUpdate;
  rec->txn_id_ = txn_id;
  rec->old_key_ = old_key;
  rec->old_val_ = old_val;
  rec->new_key_ = new_key;
  rec->new_val_ = new_val;
  rec->lsn_ = LogRec::next_lsn_++;
  rec->prev_lsn_ = LogRec::prev_lsn_map_[txn_id];
  LogRec::prev_lsn_map_[txn_id] = rec->lsn_;
  return rec;
}

static LogRecPtr CreateBeginLog(txn_id_t txn_id) {
  auto rec = std::make_shared<LogRec>();
  rec->type_ = LogRecType::kBegin;
  rec->txn_id_ = txn_id;
  rec->lsn_ = LogRec::next_lsn_++;
  rec->prev_lsn_ = INVALID_LSN;
  LogRec::prev_lsn_map_[txn_id] = rec->lsn_;
  return rec;
}

static LogRecPtr CreateCommitLog(txn_id_t txn_id) {
  auto rec = std::make_shared<LogRec>();
  rec->type_ = LogRecType::kCommit;
  rec->txn_id_ = txn_id;
  rec->lsn_ = LogRec::next_lsn_++;
  rec->prev_lsn_ = LogRec::prev_lsn_map_[txn_id];
  LogRec::prev_lsn_map_[txn_id] = rec->lsn_;
  return rec;
}

static LogRecPtr CreateAbortLog(txn_id_t txn_id) {
  auto rec = std::make_shared<LogRec>();
  rec->type_ = LogRecType::kAbort;
  rec->txn_id_ = txn_id;
  rec->lsn_ = LogRec::next_lsn_++;
  rec->prev_lsn_ = LogRec::prev_lsn_map_[txn_id];
  LogRec::prev_lsn_map_[txn_id] = rec->lsn_;
  return rec;
}

#endif  // MINISQL_LOG_REC_H
