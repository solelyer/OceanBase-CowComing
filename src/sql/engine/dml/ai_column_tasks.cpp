/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "ai_column_tasks.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "share/ai_column/ai_column_info.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/ob_sql_context.h"
#include "sql/engine/ob_physical_plan.h"
#include "query/session/ob_inner_sql_connection_access.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace sql
{
namespace
{
int get_ai_row_value(ObTableModifyOp &operation, const ObDMLBaseCtDef &definition,
                       bool old, uint64_t column_id, ObObj &value)
{
  int ret = OB_SUCCESS;
  const ExprFixedArray &row = old ? definition.old_row_ : definition.new_row_;
  int64_t position = -1;
  for (int64_t i = 0; position < 0 && i < definition.column_ids_.count(); ++i) {
    if (definition.column_ids_.at(i) == column_id) { position = i; }
  }
  if (position < 0 || position >= row.count()) { ret = OB_ERR_UNEXPECTED; }
  else {
    ObExpr &expression = *row.at(position);
    ObDatum *datum = nullptr;
    if (OB_FAIL(expression.eval(operation.get_eval_ctx(), datum))) {
    } else { ret = datum->to_obj(value, expression.obj_meta_); }
  }
  return ret;
}

int ai_row_predicate(ObTableModifyOp &operation, const ObDMLBaseCtDef &definition,
                       const ObTableSchema &table, bool old, ObSqlString &predicate)
{
  int ret = OB_SUCCESS;
  const ObRowkeyInfo &keys = table.get_rowkey_info();
  char buffer[16384];
  for (int64_t i = 0; OB_SUCC(ret) && i < keys.get_size(); ++i) {
    const ObColumnSchemaV2 *column = table.get_column_schema(keys.get_column(i)->column_id_);
    ObObj value;
    int64_t position = 0;
    if (column == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(get_ai_row_value(operation, definition, old, column->get_column_id(), value))) {
    } else if (value.is_null()) { ret = OB_ERR_UNEXPECTED; }
    else if (i > 0 && OB_FAIL(predicate.append(" AND "))) {
    } else if (OB_FAIL(predicate.append("`"))) {
    } else {
      const ObString &name = column->get_column_name_str();
      for (int64_t j = 0; OB_SUCC(ret) && j < name.length(); ++j) {
        if (name[j] == '`' && OB_FAIL(predicate.append("`"))) {
        } else { ret = predicate.append(name.ptr() + j, 1); }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(predicate.append("` = "))) {
      } else if (OB_FAIL(value.print_sql_literal(buffer, sizeof(buffer), position, operation.get_obj_print_params()))) {
      } else { ret = predicate.append(buffer, position); }
    }
  }
  if (OB_SUCC(ret) && (predicate.empty() || predicate.length() > 4096)) { ret = OB_SIZE_OVERFLOW; }
  return ret;
}
} // namespace

int check_ai_column_reads(ObExecContext &context, const ObPhysicalPlan &plan)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = context.get_my_session();
  ObObj strict;
  if (session == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (session->is_ai_column_internal() || plan.ai_column_reads_.empty()) {
  } else if (OB_FAIL(session->get_sys_variable(SYS_VAR_AI_COLUMN_STRICT_READ, strict))) {
  } else if (strict.get_int() != 0) {
    sqlclient::ObISQLConnectionGuard connection_guard;
    ObIInnerSQLConnection *connection = nullptr;
    ObSQLSessionInfo::StmtSavedValue saved;
    ObIInnerSQLConnection::SavedValue saved_connection;
    bool nested = false;
    const bool previous_nested = context.is_ai_column_nested();
    context.set_ai_column_nested(true);
    if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session, connection_guard))) {
    } else if (OB_ISNULL(connection = as_inner_sql_connection(connection_guard.get_ptr()))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(connection->begin_nested_session(saved, saved_connection, true))) {
    } else { nested = true; session->set_ai_column_internal(true); }
    for (int64_t i = 0; OB_SUCC(ret) && i + 2 < plan.ai_column_reads_.count(); i += 3) {
      const uint64_t table_id = plan.ai_column_reads_.at(i);
      const uint64_t column_id = plan.ai_column_reads_.at(i + 1);
      const uint64_t state_id = plan.ai_column_reads_.at(i + 2);
      const ObTableSchema *table = nullptr;
      const ObDatabaseSchema *database = nullptr;
      ObSchemaGetterGuard *guard = context.get_sql_ctx()->schema_guard_;
      ObSqlString query;
      if (OB_FAIL(guard->get_table_schema(table_id, table))) {
      } else if (table == nullptr || table->get_column_schema(state_id) == nullptr) { ret = OB_SCHEMA_EAGAIN; }
      else if (OB_FAIL(guard->get_database_schema(table->get_database_id(), database))) {
      } else if (database == nullptr) { ret = OB_TABLE_NOT_EXIST; }
      else {
        // Identifiers originate in schema, with MySQL identifier quoting.
        const auto identifier = [&](const ObString &name) -> int {
          int status = query.append("`");
          for (int64_t k = 0; status == OB_SUCCESS && k < name.length(); ++k) {
            if (name[k] == '`') { status = query.append("`"); }
            if (status == OB_SUCCESS) { status = query.append(name.ptr() + k, 1); }
          }
          if (status == OB_SUCCESS) { status = query.append("`"); }
          return status;
        };
        if (OB_FAIL(query.append("SELECT 1 FROM "))) {
        } else if (OB_FAIL(identifier(database->get_database_name_str()))) {
        } else if (OB_FAIL(query.append("."))) {
        } else if (OB_FAIL(identifier(table->get_table_name_str()))) {
        } else if (OB_FAIL(query.append(" WHERE "))) {
        } else if (OB_FAIL(identifier(table->get_column_schema(state_id)->get_column_name_str()))) {
        } else if (OB_FAIL(query.append("<>2 LIMIT 1"))) {
        } else {
          ObISQLClient::ReadResult result;
          if (OB_FAIL(connection->execute_read(query.string(), result))) {
          } else if (OB_SUCC(ret = result.get_result()->next())) {
            ret = OB_AI_COLUMN_NOT_READY;
            LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, table_id, column_id);
          } else if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        }
      }
    }
    if (nested) {
      session->set_ai_column_internal(false);
      const int end_status = connection->end_nested_session(saved, saved_connection);
      if (OB_SUCC(ret)) { ret = end_status; }
    }
    context.set_ai_column_nested(previous_nested);
  }
  return ret;
}

int enqueue_ai_column_tasks(ObTableModifyOp &operation, const ObDMLBaseCtDef &definition)
{
  int ret = OB_SUCCESS;
  ObExecContext &context = operation.get_exec_ctx();
  const uint64_t table_id = definition.das_base_ctdef_.index_tid_;
  ObSQLSessionInfo *session = context.get_my_session();
  const ObTableSchema *table = nullptr;
  ObSEArray<const ObColumnSchemaV2 *, 8> columns;
  if (!definition.is_primary_index_ || session == nullptr || session->is_ai_column_internal()) {
  } else if (context.get_sql_ctx() == nullptr || context.get_sql_ctx()->schema_guard_ == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(context.get_sql_ctx()->schema_guard_->get_table_schema(table_id, table))) {
  } else if (table == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else {
    for (ObTableSchema::const_column_iterator it = table->column_begin(); OB_SUCC(ret) && it != table->column_end(); ++it) {
      if ((*it)->is_ai_column()) { ret = columns.push_back(*it); }
    }
  }
  if (OB_SUCC(ret) && !columns.empty()) {
    ObArenaAllocator allocator("AiColumnQueue");
    ObSqlString old_key;
    ObSqlString new_key;
    const bool deleting = definition.dml_type_ == DAS_OP_TABLE_DELETE;
    const bool inserting = definition.dml_type_ == DAS_OP_TABLE_INSERT;
    bool nested = false;
    const bool previous_nested = context.is_ai_column_nested();
    context.set_ai_column_nested(true);
    if (!inserting && OB_FAIL(ai_row_predicate(operation, definition, *table, true, old_key))) {
    } else if (!deleting && OB_FAIL(ai_row_predicate(operation, definition, *table, false, new_key))) {
    } else if (OB_FAIL(operation.open_inner_conn())) {
    } else if (OB_FAIL(operation.begin_nested_session(true))) {
    } else { nested = true; }
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      const ObColumnSchemaV2 &column = *columns.at(i);
      AiColumnInfo info(allocator);
      ObObj request;
      ObObj old_request;
      bool changed = deleting || inserting;
      if (OB_FAIL(info.deserialize(column.get_ai_column_info()))) {
      } else if (!deleting && OB_FAIL(get_ai_row_value(operation, definition, false, info.request_id_, request))) {
      } else if (!inserting && !deleting) {
        if (OB_FAIL(get_ai_row_value(operation, definition, true, info.request_id_, old_request))) {
        } else { changed = request != old_request; }
      }
      if (OB_SUCC(ret) && changed) {
        if (!inserting) {
          ObSqlString cleanup;
          if (OB_FAIL(cleanup.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND column_id=%lu AND rowkey_predicate=",
                                        table_id, column.get_column_id()))) {
          } else if (OB_FAIL(sql_append_hex_escape_str(old_key.string(), cleanup))) {
          } else { ret = operation.execute_write(cleanup.ptr()); }
        }
        if (OB_SUCC(ret) && !deleting) {
          const uint64_t id = request.is_null() ? 0 : request.is_uint64() ? request.get_uint64() : static_cast<uint64_t>(request.get_int());
          ObSqlString query;
          if (id == 0 || request.is_null()) { ret = OB_ERR_UNEXPECTED; }
          else if (OB_FAIL(query.append_fmt("INSERT INTO oceanbase.__all_ai_column_task"
                    "(table_id,column_id,request_id,rowkey_predicate,definition_version,state,retry_count,eligible_ts) "
                    "VALUES(%lu,%lu,'%lu',", table_id, column.get_column_id(), id))) {
          } else if (OB_FAIL(sql_append_hex_escape_str(new_key.string(), query))) {
          } else if (OB_FAIL(query.append_fmt(",%ld,1,0,NOW(6))", ai_column_definition_version(column)))) {
          } else { ret = operation.execute_write(query.ptr()); }
        }
      }
    }
    if (nested) {
      const int status = operation.end_nested_session();
      if (OB_SUCC(ret)) { ret = status; }
    }
    context.set_ai_column_nested(previous_nested);
  }
  if (OB_FAIL(ret)) { LOG_WARN("AI column task mutation failed", K(ret), K(table_id)); }
  return ret;
}
} // namespace sql
} // namespace oceanbase
