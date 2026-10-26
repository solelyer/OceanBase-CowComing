/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "ai_column_commands.h"
#include "share/ai_column/ai_column_info.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/dml/ob_update_resolver.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace sql
{
namespace
{
int append_ai_command_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name[i] == '`' && OB_FAIL(sql.append("`"))) {
    } else { ret = sql.append(name.ptr() + i, 1); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

int rewrite_ai_retry_status(ObResolverParams &params, ParseNode *&node,
                             const ParseNode *status, int64_t depth)
{
  int ret = OB_SUCCESS;
  if (depth > 256) { ret = OB_SIZE_OVERFLOW; }
  else if (node != nullptr) {
    if (node->type_ == T_COLUMN_REF && node->num_child_ == 3
        && node->children_[0] == nullptr && node->children_[1] == nullptr
        && node->children_[2] != nullptr && !node->children_[2]->is_input_quoted_
        && ObString(node->children_[2]->str_len_, node->children_[2]->str_value_).case_compare("status") == 0) {
      node = const_cast<ParseNode *>(status);
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
        ret = rewrite_ai_retry_status(params, node->children_[i], status, depth + 1);
      }
    }
  }
  return ret;
}
} // namespace

int build_ai_column_command(ObResolverParams &params, const ParseNode &command, const ParseNode *&native)
{
  int ret = OB_SUCCESS;
  native = nullptr;
  const bool retry = command.type_ == T_RETRY_AI_COLUMN;
  const ParseNode *relation = command.num_child_ > 0 ? command.children_[0] : nullptr;
  const ObTableSchema *table = nullptr;
  ObSqlString query;
  ObSqlString table_name;
  ObString database = params.session_info_->get_database_name();
  if ((retry && command.num_child_ != 4) || (!retry && command.num_child_ != 1)
      || relation == nullptr || relation->num_child_ < 2 || relation->children_[1] == nullptr) {
    ret = OB_INVALID_ARGUMENT;
  } else if (retry && command.children_[3] != nullptr) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "combining AI COLUMN RETRY with another ALTER action");
  } else {
    if (relation->children_[0] != nullptr) {
      database.assign_ptr(relation->children_[0]->str_value_, relation->children_[0]->str_len_);
    }
    const ObString name(relation->children_[1]->str_len_, relation->children_[1]->str_value_);
    if (OB_FAIL(params.schema_checker_->get_table_schema(database, name, false, table))) {
    } else if (table == nullptr) { ret = OB_TABLE_NOT_EXIST; }
    else if (OB_FAIL(append_ai_command_identifier(table_name, database))) {
    } else if (OB_FAIL(table_name.append("."))) {
    } else { ret = append_ai_command_identifier(table_name, name); }
  }
  AiColumnInfo info(*params.allocator_);
  const ObColumnSchemaV2 *column = nullptr;
  if (OB_SUCC(ret) && retry) {
    const ParseNode *target = command.children_[1];
    if (target == nullptr) { ret = OB_INVALID_ARGUMENT; }
    else {
      column = table->get_column_schema(ObString(target->str_len_, target->str_value_));
      if (column == nullptr || !column->is_ai_column()) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
      } else if (OB_FAIL(info.deserialize(column->get_ai_column_info()))) {
      } else if (OB_FAIL(query.append_fmt("UPDATE %s SET ", table_name.ptr()))) {
      } else if (OB_FAIL(append_ai_command_identifier(query, column->get_column_name_str()))) {
      } else if (OB_FAIL(query.append("=NULL,"))) {
      } else if (OB_FAIL(append_ai_command_identifier(query, table->get_column_schema(info.state_id_)->get_column_name_str()))) {
      } else if (OB_FAIL(query.append("=1,"))) {
      } else if (OB_FAIL(append_ai_command_identifier(query, table->get_column_schema(info.request_id_)->get_column_name_str()))) {
      } else if (OB_FAIL(query.append("=UUID_SHORT(),"))) {
      } else if (OB_FAIL(append_ai_command_identifier(query, table->get_column_schema(info.result_spec_id_)->get_column_name_str()))) {
      } else { ret = query.append_fmt("=%ld", ai_column_definition_version(*column)); }
    }
  } else if (OB_SUCC(ret)) {
    ret = query.append_fmt("SELECT column_name,normalized_expr,sync_mode,max_retries,state_column_id,request_column_id,"
                          "result_spec_column_id FROM oceanbase.__all_ai_column WHERE table_id=%lu ORDER BY column_id", table->get_table_id());
  }
  ParseResult parsed;
  ObParser parser(*params.allocator_, params.session_info_->get_sql_mode());
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(parser.parse(query.string(), parsed))) {
  } else if (parsed.result_tree_ == nullptr || parsed.result_tree_->num_child_ != 1) { ret = OB_ERR_UNEXPECTED; }
  else {
    native = parsed.result_tree_->children_[0];
    if (retry && command.children_[2] != nullptr) {
      ObSqlString expression;
      const ParseNode *status = nullptr;
      if (OB_FAIL(expression.append("CASE "))) {
      } else if (OB_FAIL(append_ai_command_identifier(expression, table->get_column_schema(info.state_id_)->get_column_name_str()))) {
      } else if (OB_FAIL(expression.append(" WHEN 1 THEN 'PENDING' WHEN 2 THEN 'READY' ELSE 'FAILED' END"))) {
      } else if (OB_FAIL(ObRawExprUtils::parse_expr_node_from_str(expression.string(), params.session_info_->get_charsets4parser(),
                                     *params.allocator_, status, params.session_info_->get_sql_mode()))) {
      } else {
        ParseNode *predicate = command.children_[2];
        if (OB_FAIL(rewrite_ai_retry_status(params, predicate, status, 0))) {
        } else { const_cast<ParseNode *>(native)->children_[ObUpdateResolver::WHERE] = predicate; }
      }
    }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
