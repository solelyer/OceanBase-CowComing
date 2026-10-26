/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "ai_column_resolver.h"
#include "share/ai_column/ai_column_info.h"
#include "share/schema/ob_ai_model_mgr.h"
#include "query/vector/ob_vector_index_util.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/printer/ob_raw_expr_printer.h"
#include "common/json_type/ob_json_tree.h"
#include "sql/ob_sql_utils.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace sql
{
int resolve_ai_column_attribute(ObResolverParams &params, ObColumnSchemaV2 &column, const ParseNode &attribute)
{
  int ret = OB_SUCCESS;
  AiColumnInfo info(*params.allocator_);
  ObSEArray<ObString, 4> seen;
  if (column.is_ai_column() || attribute.num_child_ != 2 || OB_ISNULL(attribute.children_[0])) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const ParseNode &expression = *attribute.children_[0];
    info.expression_.assign_ptr(expression.str_value_, expression.str_len_);
    const ParseNode *options = attribute.children_[1];
    for (int64_t i = 0; OB_SUCC(ret) && options != nullptr && i < options->num_child_; ++i) {
      const ParseNode *option = options->children_[i];
      if (OB_ISNULL(option) || option->type_ != T_OP_EQ || option->num_child_ != 2
          || OB_ISNULL(option->children_[0]) || OB_ISNULL(option->children_[1])) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const ParseNode *identifier = option->children_[0];
        if (identifier->type_ == T_COLUMN_REF && identifier->num_child_ == 3
            && identifier->children_[0] == nullptr && identifier->children_[1] == nullptr) {
          identifier = identifier->children_[2];
        }
        ObRawExpr *value = nullptr;
        if (identifier == nullptr || identifier->type_ != T_IDENT) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(ObResolverUtils::resolve_const_expr(params, *option->children_[1], value, nullptr))) {
        } else if (!value->is_const_raw_expr()) {
          ret = OB_NOT_SUPPORTED;
        } else {
          const ObString name(identifier->str_len_, identifier->str_value_);
          const ObObj &object = static_cast<ObConstRawExpr *>(value)->get_value();
          for (int64_t j = 0; OB_SUCC(ret) && j < seen.count(); ++j) {
            if (seen.at(j).case_compare(name) == 0) { ret = OB_INVALID_ARGUMENT; }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(seen.push_back(name))) {
          } else if (name.case_compare("sync_mode") == 0) {
            if (!object.is_string_type() || object.get_string().case_compare("async") != 0) {
              ret = OB_NOT_SUPPORTED;
            }
          } else if (name.case_compare("max_retries") == 0) {
            if (!object.is_integer_type() || object.get_int() < 0 || object.get_int() > 16) {
              ret = OB_INVALID_ARGUMENT;
            } else { info.max_retries_ = object.get_int(); }
          } else { ret = OB_INVALID_ARGUMENT; }
        }
      }
    }
    ObString definition;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(info.serialize(definition))) {
    } else { ret = column.set_ai_column_info(definition); }
  }
  return ret;
}

namespace
{
int check_ai_input(const ObRawExpr &expression, int depth = 0)
{
  int ret = OB_SUCCESS;
  if (depth > 64) { ret = OB_SIZE_OVERFLOW; }
  else if (expression.is_query_ref_expr() || expression.is_aggr_expr() || expression.is_win_func_expr()
           || expression.get_expr_type() == T_FUN_UDF || expression.has_flag(IS_RAND_FUNC)
           || expression.get_expr_type() == T_FUN_SYS_AI_COMPLETE || expression.get_expr_type() == T_FUN_SYS_AI_EMBED
           || expression.get_expr_type() == T_FUN_SYS_AI_RERANK || expression.has_flag(IS_STATE_FUNC)
           || expression.get_expr_type() == T_OP_GET_USER_VAR || expression.get_expr_type() == T_OP_GET_SYS_VAR) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN input must be a deterministic expression of ordinary columns");
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < expression.get_param_count(); ++i) {
    if (OB_ISNULL(expression.get_param_expr(i))) { ret = OB_ERR_UNEXPECTED; }
    else { ret = check_ai_input(*expression.get_param_expr(i), depth + 1); }
  }
  return ret;
}

int print_ai_argument(ObResolverParams &params, ObRawExpr &expression, ObSqlString &query)
{
  int ret = OB_SUCCESS;
  const int64_t capacity = 65536;
  char *buffer = static_cast<char *>(params.allocator_->alloc(capacity));
  int64_t position = 0;
  ObRawExprPrinter printer(buffer, capacity, &position, params.schema_checker_->get_schema_guard(),
                             params.session_info_->get_timezone_info());
  if (buffer == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  else if (OB_FAIL(printer.do_print(&expression, T_NONE_SCOPE, true))) {
  } else { ret = query.append(buffer, position); }
  return ret;
}
} // namespace

int finish_ai_column(ObResolverParams &params, const ObTableSchema &source,
                       ObColumnSchemaV2 &column, uint64_t &next_id, ObIArray<ObColumnSchemaV2> &hidden)
{
  int ret = OB_SUCCESS;
  AiColumnInfo info(*params.allocator_);
  ObRawExpr *expression = nullptr;
  ObSEArray<ObQualifiedName, 8> names;
  if (OB_FAIL(info.deserialize(column.get_ai_column_info()))) {
  } else if (column.is_generated_column() || !column.is_nullable() || column.is_autoincrement()
             || !column.get_cur_default_value().is_null() || column.is_rowkey_column()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN must be a nullable result column without DEFAULT or key constraints");
  } else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(info.expression_, *params.expr_factory_,
                        *params.session_info_, expression, names, &source, nullptr,
                        params.schema_checker_, ObResolverUtils::DISABLE_CHECK))) {
  } else if (expression->get_param_count() < 2 || expression->get_param_count() > 3
             || (expression->get_expr_type() != T_FUN_SYS_AI_COMPLETE
                 && expression->get_expr_type() != T_FUN_SYS_AI_EMBED)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN requires AI_COMPLETE or AI_EMBED");
  }
  info.dependencies_.reuse();
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    const ObQualifiedName &name = names.at(i);
    const ObColumnSchemaV2 *dependency = source.get_column_schema(name.col_name_);
    if (name.is_sys_func() || !name.database_name_.empty() || !name.tbl_name_.empty()
        || dependency == nullptr || dependency->is_hidden()) {
      ret = OB_ERR_BAD_FIELD_ERROR;
    } else if (dependency->is_ai_column() || dependency->is_generated_column()) {
      ret = OB_NOT_SUPPORTED;
    } else if (OB_FAIL(ObRawExprUtils::init_column_expr(*dependency, params.session_info_, *name.ref_expr_))) {
    } else if (OB_FAIL(add_var_to_array_no_dup(info.dependencies_, dependency->get_column_id()))) {
    }
  }
  ObObj model_value;
  bool calculated = false;
  const ObAiModelSchema *model = nullptr;
  const bool embed = expression != nullptr && expression->get_expr_type() == T_FUN_SYS_AI_EMBED;
  int64_t dimension = 0;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(check_ai_input(*expression->get_param_expr(1)))) {
  } else if (OB_FAIL(expression->get_param_expr(0)->formalize(params.session_info_))) {
  } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(params.session_info_->get_cur_exec_ctx(),
                       expression->get_param_expr(0), model_value, calculated, *params.allocator_))) {
  } else if (!calculated || !model_value.is_string_type() || model_value.get_string().empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(params.schema_checker_->get_schema_guard()->get_ai_model_schema(model_value.get_string(), model))) {
  } else if (model == nullptr) {
    ret = OB_AI_FUNC_MODEL_NOT_FOUND;
    LOG_USER_ERROR(OB_AI_FUNC_MODEL_NOT_FOUND, model_value.get_string().length(), model_value.get_string().ptr());
  } else if (model->get_type() != (embed ? EndpointType::DENSE_EMBEDDING : EndpointType::COMPLETION)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (embed && !column.is_collection()) {
    ret = OB_NOT_SUPPORTED;
  } else if (embed && OB_FAIL(ObVectorIndexUtil::get_vector_dim_from_extend_type_info(column.get_extended_type_info(), dimension))) {
  } else if (!embed && !(column.is_json() || column.is_string_type() || is_lob_storage(column.get_data_type())
                        || ob_is_integer_type(column.get_data_type()) || ob_is_numeric_type(column.get_data_type()))) {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret) && embed && expression->get_param_count() == 3) {
    ObObj value;
    calculated = false;
    if (OB_FAIL(expression->get_param_expr(2)->formalize(params.session_info_))) {
    } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(params.session_info_->get_cur_exec_ctx(),
                         expression->get_param_expr(2), value, calculated, *params.allocator_))) {
    } else if (!calculated || !value.is_integer_type() || value.get_int() != dimension) {
      ret = OB_ERR_INVALID_VECTOR_DIM;
      LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<uint32_t>(dimension),
                     static_cast<uint32_t>(calculated && value.is_integer_type() ? value.get_int() : 0));
    }
  }
  ObSqlString normalized;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(expression->get_param_expr(1)->formalize(params.session_info_))) {
  } else if (OB_FAIL(normalized.append(embed ? "AI_EMBED(" : "AI_COMPLETE("))) {
  } else if (OB_FAIL(print_ai_argument(params, *expression->get_param_expr(0), normalized))) {
  } else if (OB_FAIL(normalized.append(", "))) {
  } else if (OB_FAIL(print_ai_argument(params, *expression->get_param_expr(1), normalized))) {
  } else if (embed) {
    ret = normalized.append_fmt(", %ld)", dimension);
  } else {
    ObIJsonBase *config = nullptr;
    if (expression->get_param_count() == 3) {
      ObObj value;
      calculated = false;
      if (OB_FAIL(expression->get_param_expr(2)->formalize(params.session_info_))) {
      } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(params.session_info_->get_cur_exec_ctx(),
                         expression->get_param_expr(2), value, calculated, *params.allocator_))) {
      } else if (!calculated || !value.is_string_type()) { ret = OB_INVALID_ARGUMENT; }
      else { ret = ObJsonBaseFactory::get_json_base(params.allocator_, value.get_string(),
                              ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, config); }
    } else { config = OB_NEWx(ObJsonObject, params.allocator_, params.allocator_); }
    ObString hint;
    if (column.is_json()) { hint = "Return exactly one valid JSON value. Do not include markdown or explanation."; }
    else if (ob_is_integer_type(column.get_data_type())) { hint = "Return exactly one base-10 integer. Do not include markdown or explanation."; }
    else if (ob_is_numeric_type(column.get_data_type())) { hint = "Return exactly one finite numeric value. Do not include markdown or explanation."; }
    ObJsonObject *object = nullptr;
    if (OB_FAIL(ret)) {
    } else if (config == nullptr || config->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_INVALID_ARGUMENT; }
    else {
      object = static_cast<ObJsonObject *>(config);
      const char *protected_keys[] = {"model", "messages", "input", "prompt", "system", "stream"};
      for (int64_t key = 0; OB_SUCC(ret) && key < 6; ++key) {
        if (object->get_value(protected_keys[key]) != nullptr) { ret = OB_INVALID_ARGUMENT; }
      }
      if (OB_SUCC(ret) && !hint.empty()) {
        ObJsonNode *existing = object->get_value("system_prompt");
        ObSqlString combined;
        if (existing != nullptr && existing->json_type() != ObJsonNodeType::J_STRING) { ret = OB_INVALID_ARGUMENT; }
        else if (existing != nullptr && OB_FAIL(combined.append(static_cast<ObJsonString *>(existing)->get_str()))) {
        } else if (OB_FAIL(combined.append_fmt("\n%.*s", hint.length(), hint.ptr()))) {
        } else if (existing != nullptr && OB_FAIL(object->remove("system_prompt"))) {
        } else {
          ObString system;
          ObJsonString *node = nullptr;
          if (OB_FAIL(ob_write_string(*params.allocator_, combined.string(), system))) {
          } else if (OB_ISNULL(node = OB_NEWx(ObJsonString, params.allocator_, system))) { ret = OB_ALLOCATE_MEMORY_FAILED; }
          else { ret = object->add("system_prompt", node); }
        }
      }
      ObJsonBuffer buffer(params.allocator_);
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(object->print(buffer, false))) {
      } else if (OB_FAIL(normalized.append(", "))) {
      } else if (OB_FAIL(normalized.append("'"))) {
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < buffer.length(); ++i) {
          const char ch = buffer.ptr()[i];
          if (ch == '\\') { ret = normalized.append("\\\\"); }
          else if (ch == '\'') { ret = normalized.append("''"); }
          else { ret = normalized.append(&ch, 1); }
        }
        if (OB_SUCC(ret)) { ret = normalized.append("')"); }
      }
    }
  }
  if (OB_SUCC(ret)) {
    const ObColumnSchemaV2 *previous = source.get_column_schema(column.get_column_id());
    bool redefinition = false;
    if (previous != nullptr && previous->is_ai_column()) {
      AiColumnInfo original(*params.allocator_);
      if (OB_FAIL(original.deserialize(previous->get_ai_column_info()))) {
      } else if (original.state_id_ != 0) {
        redefinition = true;
        info.state_id_ = original.state_id_;
        info.request_id_ = original.request_id_;
        info.result_spec_id_ = original.result_spec_id_;
      }
    }
    const char *suffixes[] = {"state", "request", "spec"};
    uint64_t *ids[] = {&info.state_id_, &info.request_id_, &info.result_spec_id_};
    for (int64_t i = 0; OB_SUCC(ret) && !redefinition && i < 3; ++i) {
      ObColumnSchemaV2 state;
      ObSqlString name;
      ObObj value;
      *ids[i] = ++next_id;
      state.set_column_id(*ids[i]);
      state.set_table_id(column.get_table_id());
      state.set_is_hidden(true);
      state.set_nullable(false);
      state.set_data_type(i == 1 ? ObUInt64Type : ObIntType);
      state.set_collation_type(CS_TYPE_BINARY);
      state.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[state.get_data_type()]);
      if (i == 1) { value.set_uint64(0); } else { value.set_int(i == 0 ? 1 : 0); }
      if (OB_FAIL(name.append_fmt("__ai_%lu_%s", column.get_column_id(), suffixes[i]))) {
      } else if (source.get_column_schema(name.string()) != nullptr) { ret = OB_ERR_COLUMN_DUPLICATE; }
      else if (OB_FAIL(state.set_column_name(name.string()))) {
      } else if (OB_FAIL(state.set_cur_default_value(value, false))) {
      } else if (OB_FAIL(state.set_orig_default_value(value))) {
      } else { ret = hidden.push_back(state); }
    }
    ObString definition;
    info.normalized_expr_ = normalized.string();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(info.serialize(definition))) {
    } else if (definition.length() > 65535) { ret = OB_SIZE_OVERFLOW; }
    else { ret = column.set_ai_column_info(definition); }
  }
  return ret;
}

int finish_ai_columns(ObResolverParams &params, ObTableSchema &table, uint64_t &next_id)
{
  int ret = OB_SUCCESS;
  ObSEArray<uint64_t, 8> columns;
  for (ObTableSchema::const_column_iterator it = table.column_begin(); OB_SUCC(ret) && it != table.column_end(); ++it) {
    if ((*it)->is_ai_column()) { ret = columns.push_back((*it)->get_column_id()); }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    ObSEArray<ObColumnSchemaV2, 4> hidden;
    ObColumnSchemaV2 *column = table.get_column_schema(columns.at(i));
    if (OB_FAIL(finish_ai_column(params, table, *column, next_id, hidden))) {
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < hidden.count(); ++j) { ret = table.add_column(hidden.at(j)); }
  }
  if (OB_SUCC(ret) && !columns.empty()) { table.set_max_used_column_id(next_id); }
  return ret;
}
} // namespace sql
} // namespace oceanbase
