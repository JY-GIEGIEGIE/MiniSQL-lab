#include "executor/execute_engine.h"

extern "C" {
int yyparse(void);
extern FILE *yyin;
#include "parser/minisql_lex.h"
#include "parser/parser.h"
}

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <chrono>
#include <fstream>

#include "common/result_writer.h"
#include "executor/executors/delete_executor.h"
#include "executor/executors/index_scan_executor.h"
#include "executor/executors/insert_executor.h"
#include "executor/executors/seq_scan_executor.h"
#include "executor/executors/update_executor.h"
#include "executor/executors/values_executor.h"
#include "glog/logging.h"
#include "planner/planner.h"
#include "utils/utils.h"

ExecuteEngine::ExecuteEngine() {
  char path[] = "./databases";
  DIR *dir;
  if ((dir = opendir(path)) == nullptr) {
    mkdir("./databases", 0777);
    dir = opendir(path);
  }
  closedir(dir);
}

std::unique_ptr<AbstractExecutor> ExecuteEngine::CreateExecutor(ExecuteContext *exec_ctx,
                                                                const AbstractPlanNodeRef &plan) {
  switch (plan->GetType()) {
    case PlanType::SeqScan:
      return std::make_unique<SeqScanExecutor>(exec_ctx, dynamic_cast<const SeqScanPlanNode *>(plan.get()));
    case PlanType::IndexScan:
      return std::make_unique<IndexScanExecutor>(exec_ctx, dynamic_cast<const IndexScanPlanNode *>(plan.get()));
    case PlanType::Update: {
      auto update_plan = dynamic_cast<const UpdatePlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, update_plan->GetChildPlan());
      return std::make_unique<UpdateExecutor>(exec_ctx, update_plan, std::move(child_executor));
    }
    case PlanType::Delete: {
      auto delete_plan = dynamic_cast<const DeletePlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, delete_plan->GetChildPlan());
      return std::make_unique<DeleteExecutor>(exec_ctx, delete_plan, std::move(child_executor));
    }
    case PlanType::Insert: {
      auto insert_plan = dynamic_cast<const InsertPlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, insert_plan->GetChildPlan());
      return std::make_unique<InsertExecutor>(exec_ctx, insert_plan, std::move(child_executor));
    }
    case PlanType::Values:
      return std::make_unique<ValuesExecutor>(exec_ctx, dynamic_cast<const ValuesPlanNode *>(plan.get()));
    default:
      throw std::logic_error("Unsupported plan type.");
  }
}

dberr_t ExecuteEngine::ExecutePlan(const AbstractPlanNodeRef &plan, std::vector<Row> *result_set, Txn *txn,
                                   ExecuteContext *exec_ctx) {
  auto executor = CreateExecutor(exec_ctx, plan);
  try {
    executor->Init();
    RowId rid{};
    Row row{};
    while (executor->Next(&row, &rid)) {
      if (result_set != nullptr) result_set->push_back(row);
    }
  } catch (const exception &ex) {
    std::cout << "Error Encountered in Executor Execution: " << ex.what() << std::endl;
    if (result_set != nullptr) result_set->clear();
    return DB_FAILED;
  }
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::Execute(pSyntaxNode ast) {
  if (ast == nullptr) return DB_FAILED;
  auto start_time = std::chrono::system_clock::now();
  unique_ptr<ExecuteContext> context(nullptr);
  if (!current_db_.empty()) context = dbs_[current_db_]->MakeExecuteContext(nullptr);
  switch (ast->type_) {
    case kNodeCreateDB: return ExecuteCreateDatabase(ast, context.get());
    case kNodeDropDB:   return ExecuteDropDatabase(ast, context.get());
    case kNodeShowDB:   return ExecuteShowDatabases(ast, context.get());
    case kNodeUseDB:    return ExecuteUseDatabase(ast, context.get());
    case kNodeShowTables:  return ExecuteShowTables(ast, context.get());
    case kNodeCreateTable: return ExecuteCreateTable(ast, context.get());
    case kNodeDropTable:   return ExecuteDropTable(ast, context.get());
    case kNodeShowIndexes: return ExecuteShowIndexes(ast, context.get());
    case kNodeCreateIndex: return ExecuteCreateIndex(ast, context.get());
    case kNodeDropIndex:   return ExecuteDropIndex(ast, context.get());
    case kNodeTrxBegin:    return ExecuteTrxBegin(ast, context.get());
    case kNodeTrxCommit:   return ExecuteTrxCommit(ast, context.get());
    case kNodeTrxRollback: return ExecuteTrxRollback(ast, context.get());
    case kNodeExecFile:    return ExecuteExecfile(ast, context.get());
    case kNodeQuit:        return ExecuteQuit(ast, context.get());
    default: break;
  }
  Planner planner(context.get());
  std::vector<Row> result_set{};
  try {
    planner.PlanQuery(ast);
    ExecutePlan(planner.plan_, &result_set, nullptr, context.get());
  } catch (const exception &ex) {
    std::cout << "Error Encountered in Planner: " << ex.what() << std::endl;
    return DB_FAILED;
  }
  auto stop_time = std::chrono::system_clock::now();
  double duration_time =
      double((std::chrono::duration_cast<std::chrono::milliseconds>(stop_time - start_time)).count());
  std::stringstream ss;
  ResultWriter writer(ss);
  if (planner.plan_->GetType() == PlanType::SeqScan || planner.plan_->GetType() == PlanType::IndexScan) {
    auto schema = planner.plan_->OutputSchema();
    auto num_of_columns = schema->GetColumnCount();
    if (!result_set.empty()) {
      vector<int> data_width(num_of_columns, 0);
      for (const auto &row : result_set)
        for (uint32_t i = 0; i < num_of_columns; i++)
          data_width[i] = max(data_width[i], int(row.GetField(i)->toString().size()));
      int k = 0;
      for (const auto &column : schema->GetColumns())
        data_width[k] = max(data_width[k], int(column->GetName().length())), k++;
      writer.Divider(data_width);
      k = 0; writer.BeginRow();
      for (const auto &column : schema->GetColumns())
        writer.WriteHeaderCell(column->GetName(), data_width[k++]);
      writer.EndRow(); writer.Divider(data_width);
      for (const auto &row : result_set) {
        writer.BeginRow();
        for (uint32_t i = 0; i < schema->GetColumnCount(); i++)
          writer.WriteCell(row.GetField(i)->toString(), data_width[i]);
        writer.EndRow();
      }
      writer.Divider(data_width);
    }
    writer.EndInformation(result_set.size(), duration_time, true);
  } else {
    writer.EndInformation(result_set.size(), duration_time, false);
  }
  std::cout << writer.stream_.rdbuf();
  if (ast->type_ == kNodeSelect) delete planner.plan_->OutputSchema();
  return DB_SUCCESS;
}

void ExecuteEngine::ExecuteInformation(dberr_t result) {
  switch (result) {
    case DB_ALREADY_EXIST:       cout << "Database already exists." << endl; break;
    case DB_NOT_EXIST:           cout << "Database not exists." << endl; break;
    case DB_TABLE_ALREADY_EXIST: cout << "Table already exists." << endl; break;
    case DB_TABLE_NOT_EXIST:     cout << "Table not exists." << endl; break;
    case DB_INDEX_ALREADY_EXIST: cout << "Index already exists." << endl; break;
    case DB_INDEX_NOT_FOUND:     cout << "Index not exists." << endl; break;
    case DB_COLUMN_NAME_NOT_EXIST: cout << "Column not exists." << endl; break;
    case DB_KEY_NOT_FOUND:       cout << "Key not exists." << endl; break;
    case DB_QUIT:                cout << "Bye." << endl; break;
    default: break;
  }
}

// ==================== Database operations (framework implemented) ====================

dberr_t ExecuteEngine::ExecuteCreateDatabase(pSyntaxNode ast, ExecuteContext *context) {
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) != dbs_.end()) return DB_ALREADY_EXIST;
  dbs_.insert(make_pair(db_name, new DBStorageEngine(db_name, true)));
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteDropDatabase(pSyntaxNode ast, ExecuteContext *context) {
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) == dbs_.end()) return DB_NOT_EXIST;
  remove(("./databases/" + db_name).c_str());
  delete dbs_[db_name];
  dbs_.erase(db_name);
  if (db_name == current_db_) current_db_ = "";
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteShowDatabases(pSyntaxNode ast, ExecuteContext *context) {
  // (already implemented, unchanged)
  if (dbs_.empty()) { cout << "Empty set (0.00 sec)" << endl; return DB_SUCCESS; }
  int max_width = 8;
  for (const auto &itr : dbs_)
    if (itr.first.length() > max_width) max_width = itr.first.length();
  cout << "+" << setfill('-') << setw(max_width + 2) << "" << "+" << endl;
  cout << "| " << std::left << setfill(' ') << setw(max_width) << "Database" << " |" << endl;
  cout << "+" << setfill('-') << setw(max_width + 2) << "" << "+" << endl;
  for (const auto &itr : dbs_)
    cout << "| " << std::left << setfill(' ') << setw(max_width) << itr.first << " |" << endl;
  cout << "+" << setfill('-') << setw(max_width + 2) << "" << "+" << endl;
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteUseDatabase(pSyntaxNode ast, ExecuteContext *context) {
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) != dbs_.end()) {
    current_db_ = db_name;
    cout << "Database changed" << endl;
    return DB_SUCCESS;
  }
  return DB_NOT_EXIST;
}

dberr_t ExecuteEngine::ExecuteShowTables(pSyntaxNode ast, ExecuteContext *context) {
  // (already implemented, unchanged)
  if (current_db_.empty()) { cout << "No database selected" << endl; return DB_FAILED; }
  vector<TableInfo *> tables;
  if (dbs_[current_db_]->catalog_mgr_->GetTables(tables) == DB_FAILED) {
    cout << "Empty set (0.00 sec)" << endl; return DB_FAILED;
  }
  string table_in_db("Tables_in_" + current_db_);
  uint max_width = table_in_db.length();
  for (const auto &itr : tables)
    if (itr->GetTableName().length() > max_width) max_width = itr->GetTableName().length();
  cout << "+" << setfill('-') << setw(max_width + 2) << "" << "+" << endl;
  cout << "| " << std::left << setfill(' ') << setw(max_width) << table_in_db << " |" << endl;
  cout << "+" << setfill('-') << setw(max_width + 2) << "" << "+" << endl;
  for (const auto &itr : tables)
    cout << "| " << std::left << setfill(' ') << setw(max_width) << itr->GetTableName() << " |" << endl;
  cout << "+" << setfill('-') << setw(max_width + 2) << "" << "+" << endl;
  return DB_SUCCESS;
}

// ==================== Table / Index DDL ====================

dberr_t ExecuteEngine::ExecuteCreateTable(pSyntaxNode ast, ExecuteContext *context) {
  if (current_db_.empty()) return DB_FAILED;
  string table_name(ast->child_->val_ ? ast->child_->val_ : "");
  auto *col_list = ast->child_->next_;
  vector<Column *> columns;
  vector<string> primary_keys;
  int col_idx = 0;
  for (auto *col_def = col_list->child_; col_def != nullptr; col_def = col_def->next_) {
    if (col_def->type_ == kNodeColumnDefinition) {
      string col_name(col_def->child_->val_ ? col_def->child_->val_ : "");
      string col_type(col_def->child_->next_->val_ ? col_def->child_->next_->val_ : "");
      // unique: either val_=="unique" or a child with val_="unique"
      bool unique = (col_def->val_ && string(col_def->val_) == "unique");
      for (auto *a = col_def->child_->next_->next_; a != nullptr && !unique; a = a->next_) {
        if (a->val_ && string(a->val_) == "unique") unique = true;
      }
      TypeId type_id;
      uint32_t len = 0;
      if (col_type == "int") type_id = kTypeInt;
      else if (col_type == "float") type_id = kTypeFloat;
      else if (col_type == "char") {
        type_id = kTypeChar;
        auto *len_node = col_def->child_->next_->next_;
        len = (len_node && len_node->val_) ? stoi(len_node->val_) : 16;
      } else return DB_FAILED;
      if (type_id == kTypeChar)
        columns.push_back(new Column(col_name, type_id, len, col_idx++, true, unique));
      else
        columns.push_back(new Column(col_name, type_id, col_idx++, true, unique));
    }
    if (col_def->type_ == kNodeColumnList) {
      for (auto *pk_col = col_def->child_; pk_col != nullptr; pk_col = pk_col->next_)
        if (pk_col->val_) primary_keys.push_back(pk_col->val_);
    }
  }
  auto *schema = new Schema(columns);
  TableInfo *table_info = nullptr;
  Txn txn;
  auto result = dbs_[current_db_]->catalog_mgr_->CreateTable(table_name, schema, &txn, table_info);
  if (result != DB_SUCCESS) return result;
  for (auto &col : columns) {
    if (col->IsUnique()) {
      IndexInfo *idx_info = nullptr;
      Txn txn2;
      vector<string> idx_keys{col->GetName()};
      dbs_[current_db_]->catalog_mgr_->CreateIndex(table_name, "u_" + col->GetName(), idx_keys, &txn2, idx_info, "bptree");
    }
  }
  if (!primary_keys.empty()) {
    IndexInfo *pk_idx = nullptr;
    Txn txn3;
    dbs_[current_db_]->catalog_mgr_->CreateIndex(table_name, "pk_" + table_name, primary_keys, &txn3, pk_idx, "bptree");
  }
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteDropTable(pSyntaxNode ast, ExecuteContext *context) {
  if (current_db_.empty()) return DB_FAILED;
  string table_name = ast->child_->val_;
  return dbs_[current_db_]->catalog_mgr_->DropTable(table_name);
}

dberr_t ExecuteEngine::ExecuteShowIndexes(pSyntaxNode ast, ExecuteContext *context) {
  if (current_db_.empty()) return DB_FAILED;
  // show indexes 可能不带表名（ast->child_ 为 null），此时列出所有表的所有索引
  if (ast->child_ == nullptr || ast->child_->val_ == nullptr) {
    vector<TableInfo *> tables;
    dbs_[current_db_]->catalog_mgr_->GetTables(tables);
    cout << "+----------+------------+" << endl;
    cout << "| Table    | Index      |" << endl;
    cout << "+----------+------------+" << endl;
    for (auto *t : tables) {
      vector<IndexInfo *> idxs;
      dbs_[current_db_]->catalog_mgr_->GetTableIndexes(t->GetTableName(), idxs);
      for (auto *idx : idxs)
        printf("| %-8s | %-10s |\n", t->GetTableName().c_str(), idx->GetIndexName().c_str());
    }
    cout << "+----------+------------+" << endl;
    return DB_SUCCESS;
  }
  string table_name(ast->child_->val_);
  vector<IndexInfo *> indexes;
  auto result = dbs_[current_db_]->catalog_mgr_->GetTableIndexes(table_name, indexes);
  if (result != DB_SUCCESS) return result;
  if (indexes.empty()) {
    cout << "Empty set (0.00 sec)" << endl;
    return DB_SUCCESS;
  }
  cout << "+----------+------------+" << endl;
  cout << "| Table    | Index      |" << endl;
  cout << "+----------+------------+" << endl;
  for (auto *idx : indexes) {
    printf("| %-8s | %-10s |\n", table_name.c_str(), idx->GetIndexName().c_str());
  }
  cout << "+----------+------------+" << endl;
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteCreateIndex(pSyntaxNode ast, ExecuteContext *context) {
  if (current_db_.empty()) return DB_FAILED;
  // AST: child_=index_name(kNodeIdentifier), child_->next_=table_name, child_->next_->next_=kNodeColumnList
  string index_name = ast->child_->val_ ? ast->child_->val_ : "";
  string table_name = ast->child_->next_->val_ ? ast->child_->next_->val_ : "";
  auto *col_list = ast->child_->next_->next_;
  vector<string> index_keys;
  for (auto *c = col_list->child_; c != nullptr; c = c->next_) {
    index_keys.push_back(c->val_);
  }
  IndexInfo *index_info = nullptr;
  Txn txn;
  auto result = dbs_[current_db_]->catalog_mgr_->CreateIndex(table_name, index_name, index_keys, &txn, index_info, "bptree");
  if (result != DB_SUCCESS) return result;
  // 将表中已有数据批量插入新建的索引
  TableInfo *ti = nullptr;
  if (dbs_[current_db_]->catalog_mgr_->GetTable(table_name, ti) == DB_SUCCESS) {
    auto *heap = ti->GetTableHeap();
    auto *schema = ti->GetSchema();
    auto *key_schema = index_info->GetIndexKeySchema();
    for (auto it = heap->Begin(nullptr); it != heap->End(); ++it) {
      Row key_row;
      Row tmp = *it;
      tmp.GetKeyFromRow(schema, key_schema, key_row);
      index_info->GetIndex()->InsertEntry(key_row, tmp.GetRowId(), nullptr);
    }
  }
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteDropIndex(pSyntaxNode ast, ExecuteContext *context) {
  if (current_db_.empty()) return DB_FAILED;
  string table_name = ast->child_->val_;
  string index_name = ast->child_->next_->val_;
  return dbs_[current_db_]->catalog_mgr_->DropIndex(table_name, index_name);
}

dberr_t ExecuteEngine::ExecuteTrxBegin(pSyntaxNode ast, ExecuteContext *context) {
  return DB_FAILED;
}

dberr_t ExecuteEngine::ExecuteTrxCommit(pSyntaxNode ast, ExecuteContext *context) {
  return DB_FAILED;
}

dberr_t ExecuteEngine::ExecuteTrxRollback(pSyntaxNode ast, ExecuteContext *context) {
  return DB_FAILED;
}

dberr_t ExecuteEngine::ExecuteExecfile(pSyntaxNode ast, ExecuteContext *context) {
  string filename = ast->child_->val_;
  ifstream infile(filename);
  if (!infile.is_open()) {
    cout << "Failed to open file: " << filename << endl;
    return DB_FAILED;
  }
  const int buf_size = 1024;
  char cmd[buf_size];
  while (infile.getline(cmd, buf_size)) {
    if (cmd[0] == 0 || cmd[0] == '#') continue;
    cout << "minisql > " << cmd << endl;
    YY_BUFFER_STATE bp = yy_scan_string(cmd);
    if (bp == nullptr) continue;
    yy_switch_to_buffer(bp);
    MinisqlParserInit();
    yyparse();
    if (!MinisqlParserGetError()) {
      Execute(MinisqlGetParserRootNode());
    }
    MinisqlParserFinish();
    yy_delete_buffer(bp);
    yylex_destroy();
  }
  infile.close();
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteQuit(pSyntaxNode ast, ExecuteContext *context) {
  return DB_QUIT;
}
