/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "ai_column_dml.h"
#include "share/ai_column/ai_column_info.h"
#include "sql/resolver/dml/ob_del_upd_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/ob_sql_context.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace sql
{
int track_ai_column_read(ObResolverParams &params, ObDMLStmt &statement,
                          const TableItem &item, const ColumnItem &column)
{
  int ret = OB_SUCCESS;
  const ObTableSchema *table = nullptr;
  if (params.session_info_->is_ai_column_internal()) {
  } else if (OB_FAIL(params.schema_checker_->get_table_schema(item.ref_id_, table))) {
  } else if (table == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else {
    const ObColumnSchemaV2 *schema = table->get_column_schema(column.column_id_);
    if (schema != nullptr && schema->is_ai_column()) {
      ObIArray<uint64_t> &reads = statement.get_query_ctx()->ai_column_reads_;
      bool exists = false;
      for (int64_t i = 0; !exists && i + 2 < reads.count(); i += 3) {
        exists = reads.at(i) == item.ref_id_ && reads.at(i + 1) == column.column_id_;
      }
      if (!exists) {
        AiColumnInfo info(*params.allocator_);
        if (OB_FAIL(info.deserialize(schema->get_ai_column_info()))) {
        } else if (OB_FAIL(reads.push_back(item.ref_id_))) {
        } else if (OB_FAIL(reads.push_back(column.column_id_))) {
        } else { ret = reads.push_back(info.state_id_); }
      }
    }
  }
  return ret;
}

int build_ai_column_request(ObResolverParams &params, ObDMLStmt &statement,
                            uint64_t table_id, ObRawExpr *&expression)
{
  int ret = OB_SUCCESS;
  expression = nullptr;
  ObQueryCtx *context = statement.get_query_ctx();
  if (context == nullptr) { ret = OB_ERR_UNEXPECTED; }
  for (int64_t i = 0; OB_SUCC(ret) && i < context->ai_column_requests_.count(); ++i) {
    if (context->ai_column_requests_.at(i).first == table_id) {
      expression = context->ai_column_requests_.at(i).second;
    }
  }
  if (OB_SUCC(ret) && expression == nullptr) {
    ObSysFunRawExpr *request = nullptr;
    if (OB_FAIL(params.expr_factory_->create_raw_expr(T_FUN_SYS_UUID_SHORT, request))) {
    } else if (OB_FAIL(request->extend_param_exprs(0))) {
    } else if (OB_FALSE_IT(request->set_func_name("uuid_short"))) {
    } else if (OB_FAIL(request->formalize(params.session_info_))) {
    } else if (OB_FAIL(context->ai_column_requests_.push_back(std::make_pair(table_id, request)))) {
    } else { expression = request; }
  }
  return ret;
}

int build_ai_column_default(ObResolverParams &params, ObDMLStmt &statement,
                              const ColumnItem &column, ObRawExpr *&expression, bool &handled)
{
  int ret = OB_SUCCESS;
  handled = false;
  const TableItem *item = statement.get_table_item_by_id(column.table_id_);
  const ObTableSchema *table = nullptr;
  if (!column.expr_->is_hidden_column()) {
  } else if (item == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(params.schema_checker_->get_table_schema(item->get_base_table_item().ref_id_, table))) {
  } else if (table == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else {
    for (ObTableSchema::const_column_iterator it = table->column_begin(); OB_SUCC(ret) && !handled && it != table->column_end(); ++it) {
      if ((*it)->is_ai_column()) {
        AiColumnInfo info(*params.allocator_);
        if (OB_FAIL(info.deserialize((*it)->get_ai_column_info()))) {
        } else if (column.column_id_ == info.request_id_) {
          handled = true;
          ret = build_ai_column_request(params, statement, column.table_id_, expression);
        } else if (column.column_id_ == info.result_spec_id_) {
          ObConstRawExpr *version = nullptr;
          handled = true;
          if (OB_FAIL(ObRawExprUtils::build_const_int_expr(*params.expr_factory_, ObIntType,
                         ai_column_definition_version(**it), version))) {
          } else { expression = version; }
        }
      }
    }
  }
  return ret;
}

int check_ai_column_assignment(ObResolverParams &params, ObDMLStmt &statement,
                                 const ObColumnRefRawExpr &column)
{
  int ret = OB_SUCCESS;
  const TableItem *item = statement.get_table_item_by_id(column.get_table_id());
  const ObTableSchema *table = nullptr;
  if (params.session_info_->is_ai_column_internal()) {
  } else if (item == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(params.schema_checker_->get_table_schema(item->get_base_table_item().ref_id_, table))) {
  } else if (table == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else {
    const ObColumnSchemaV2 *schema = table->get_column_schema(column.get_column_id());
    if (schema != nullptr && schema->is_ai_column()) {
      ret = OB_NOT_SUPPORTED;
    }
  }
  return ret;
}

int ObDelUpdResolver::resolve_ai_assignments(ObIArray<ObTableAssignment> &assigns)
{
  int ret = OB_SUCCESS;
  ObDMLStmt &statement = *get_stmt();
  if (!session_info_->is_ai_column_internal()) {
    for (int64_t i = 0; OB_SUCC(ret) && i < assigns.count(); ++i) {
      const TableItem *item = statement.get_table_item_by_id(assigns.at(i).table_id_);
      const ObTableSchema *table = nullptr;
      if (item == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(schema_checker_->get_table_schema(item->get_base_table_item().ref_id_, table))) {
      } else if (table == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        const int64_t ordinary_count = assigns.at(i).assignments_.count();
        for (ObTableSchema::const_column_iterator it = table->column_begin(); OB_SUCC(ret) && it != table->column_end(); ++it) {
          const ObColumnSchemaV2 &column = **it;
          if (column.is_ai_column()) {
            AiColumnInfo info(*allocator_);
            bool invalidate = false;
            if (OB_FAIL(info.deserialize(column.get_ai_column_info()))) {
            }
            for (int64_t j = 0; OB_SUCC(ret) && !invalidate && j < ordinary_count; ++j) {
              const uint64_t id = assigns.at(i).assignments_.at(j).column_expr_->get_column_id();
              const ObColumnSchemaV2 *changed = table->get_column_schema(id);
              invalidate = (changed != nullptr && changed->is_rowkey_column()) || has_exist_in_array(info.dependencies_, id);
            }
            if (OB_SUCC(ret) && invalidate) {
              ObRawExpr *request = nullptr;
              if (OB_FAIL(build_ai_column_request(params_, statement, item->table_id_, request))) {
              }
              const uint64_t ids[] = {column.get_column_id(), info.state_id_, info.request_id_, info.result_spec_id_};
              for (int64_t k = 0; OB_SUCC(ret) && k < 4; ++k) {
                const ObColumnSchemaV2 *target = table->get_column_schema(ids[k]);
                ObSEArray<ObColumnRefRawExpr *, 1> columns;
                ObAssignment assignment;
                ObConstRawExpr *constant = nullptr;
                if (target == nullptr) { ret = OB_ERR_UNEXPECTED; }
                else if (OB_FAIL(add_column_to_stmt(*item, *target, columns))) {
                } else if (columns.count() != 1) { ret = OB_ERR_UNEXPECTED; }
                else if (k == 0) {
                  ret = ObRawExprUtils::build_null_expr(*params_.expr_factory_, assignment.expr_);
                } else if (k == 2) { assignment.expr_ = request; }
                else if (OB_FAIL(ObRawExprUtils::build_const_int_expr(*params_.expr_factory_, ObIntType,
                                     k == 1 ? 1 : ai_column_definition_version(column), constant))) {
                } else { assignment.expr_ = constant; }
                if (OB_SUCC(ret)) {
                  assignment.column_expr_ = columns.at(0);
                  assignment.is_implicit_ = true;
                  if (OB_FAIL(ObRawExprUtils::build_column_conv_expr(*params_.expr_factory_, *allocator_,
                          *assignment.column_expr_, assignment.expr_, session_info_))) {
                  } else if (OB_FAIL(assignment.expr_->formalize(session_info_))) {
                  } else { ret = assigns.at(i).assignments_.push_back(assignment); }
                }
              }
            }
          }
        }
      }
    }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
