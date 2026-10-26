/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/ob_sql_utils.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/parser/ob_parser.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
int ObDMLResolver::resolve_file_sql_table(const ParseNode &node, TableItem *&table)
{
  int ret = OB_SUCCESS;
  FileSqlConfig config(*allocator_);
  ObSEArray<ObString, 8> seen;
  if (node.num_child_ != 3 || OB_ISNULL(node.children_[0])
      || OB_ISNULL(node.children_[1]) || OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(FileSqlReader::check_enabled(*session_info_))) {
  } else if (OB_FAIL(config.set_format(ObString(node.children_[0]->str_len_, node.children_[0]->str_value_)))) {
  } else {
    const ParseNode &arguments = *node.children_[1];
    if (arguments.num_child_ < 1 || arguments.num_child_ > 16) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < arguments.num_child_; ++i) {
      const ParseNode *value_node = arguments.children_[i];
      ObString name;
      if (OB_ISNULL(value_node)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (i > 0) {
        if (value_node->type_ != T_OP_EQ || value_node->num_child_ != 2
            || OB_ISNULL(value_node->children_[0]) || OB_ISNULL(value_node->children_[1])) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          const ParseNode *identifier = value_node->children_[0];
          if (identifier->type_ == T_COLUMN_REF && identifier->num_child_ == 3) {
            if (identifier->children_[0] != nullptr || identifier->children_[1] != nullptr) {
              ret = OB_INVALID_ARGUMENT;
            }
            identifier = identifier->children_[2];
          }
          if (OB_ISNULL(identifier) || identifier->type_ != T_IDENT) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            name.assign_ptr(identifier->str_value_, identifier->str_len_);
            value_node = value_node->children_[1];
          }
          for (int64_t j = 0; OB_SUCC(ret) && j < seen.count(); ++j) {
            if (seen.at(j).case_compare(name) == 0) { ret = OB_INVALID_ARGUMENT; }
          }
          if (OB_SUCC(ret)) { ret = seen.push_back(name); }
        }
      }
      ObRawExpr *expression = nullptr;
      ObObj value;
      bool calculated = false;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(resolve_sql_expr(*value_node, expression))) {
      } else if (OB_FAIL(expression->formalize(session_info_))) {
      } else if (!expression->is_const_expr() && !expression->is_static_scalar_const_expr()) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "File SQL arguments must be constant");
      } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(
                          session_info_->get_cur_exec_ctx(), expression, value, calculated, *allocator_))) {
      } else if (!calculated || value.is_null()) {
        ret = OB_INVALID_ARGUMENT;
      } else if (i == 0) {
        if (!value.is_string_type()) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          ret = ob_write_string(*allocator_, value.get_string(), config.path_, true);
        }
      } else {
        ret = config.set_option(name, value);
      }
    }
  }
  if (OB_SUCC(ret) && (config.delimiter_ == config.quote_ || config.columns_.count() > 1024)) {
    ret = OB_INVALID_ARGUMENT;
  }
  ObString metadata;
  ObSysFunRawExpr *expression = nullptr;
  ObConstRawExpr *argument = nullptr;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(FileSqlReader::infer(config, *session_info_))) {
    LOG_WARN("File SQL schema inference failed", K(ret), K(config.path_));
  } else if (config.columns_.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "File SQL could not infer any columns");
  } else if (OB_FAIL(config.serialize(metadata))) {
  } else if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_FUN_SYS_FILE_SQL, expression))) {
  } else if (OB_FAIL(ObRawExprUtils::build_const_string_expr(*params_.expr_factory_,
                       ObVarcharType, metadata, CS_TYPE_UTF8MB4_BIN, argument))) {
  } else if (OB_FAIL(expression->extend_param_exprs(1))) {
  } else if (OB_FAIL(expression->add_param_expr(argument))) {
  } else if (OB_FALSE_IT(expression->set_func_name("__file_scan"))) {
  } else if (OB_FAIL(expression->formalize(session_info_))) {
  } else if (OB_ISNULL(table = get_stmt()->create_table_item(*allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ObString alias;
    if (node.children_[2] != nullptr) {
      alias.assign_ptr(node.children_[2]->str_value_, node.children_[2]->str_len_);
    } else {
      ret = get_stmt()->generate_func_table_name(*allocator_, alias);
    }
    table->table_name_ = alias;
    table->alias_name_ = alias;
    table->table_id_ = generate_table_id();
    table->type_ = TableItem::FUNCTION_TABLE;
    table->function_table_expr_ = expression;
    if (OB_SUCC(ret) && OB_FAIL(get_stmt()->add_table_item(session_info_, table))) {
    } else if (OB_SUCC(ret)) {
      ObSEArray<ColumnItem, 16> columns;
      ret = resolve_file_sql_columns(*table, columns);
    }
    if (OB_SUCC(ret)) {
      params_.query_ctx_->has_file_sql_ = true;
    }
  }
  return ret;
}

int ObDMLResolver::resolve_file_sql_columns(const TableItem &table, ObIArray<ColumnItem> &columns)
{
  int ret = OB_SUCCESS;
  FileSqlConfig config(*allocator_);
  ObRawExpr *expression = table.function_table_expr_;
  if (OB_ISNULL(expression) || expression->get_param_count() != 1
      || !expression->get_param_expr(0)->is_const_raw_expr()) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(config.deserialize(
      static_cast<ObConstRawExpr *>(expression->get_param_expr(0))->get_value().get_string()))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < config.columns_.count(); ++i) {
      const FileSqlColumn &column = config.columns_.at(i);
      const uint64_t column_id = OB_APP_MIN_COLUMN_ID + i;
      ColumnItem *item = get_stmt()->get_column_item_by_id(table.table_id_, column_id);
      if (item == nullptr) {
        ObObjMeta meta;
        meta.set_type(column.type_);
        meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        meta.set_collation_level(CS_LEVEL_IMPLICIT);
        ObAccuracy accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY[column.type_];
        if (column.type_ == ObVarcharType) {
          accuracy.set_length(MAX(1, column.length_));
        } else if (column.type_ == ObDateTimeType) {
          accuracy.set_scale(MIN(6, MAX(0, column.length_ - 20)));
        }
        ret = resolve_function_table_column_item(table, meta, accuracy,
                                                  column.name_, column_id, item);
      }
      if (OB_SUCC(ret) && OB_NOT_NULL(item)) {
        ret = columns.push_back(*item);
      }
    }
  }
  return ret;
}

int ObSelectResolver::resolve_file_sql_copy(const ParseNode &node)
{
  int ret = OB_SUCCESS;
  FileSqlConfig config(*allocator_);
  ObSEArray<ObString, 8> seen;
  if (node.num_child_ != 3 || OB_ISNULL(node.children_[0]) || OB_ISNULL(node.children_[1])) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(FileSqlReader::check_enabled(*session_info_))) {
  } else {
    const ParseNode &path = *node.children_[1];
    if (OB_FAIL(ob_write_string(*allocator_, ObString(path.str_len_, path.str_value_), config.path_, true))) {
    }
    const ParseNode *options = node.children_[2];
    for (int64_t i = 0; OB_SUCC(ret) && options != nullptr && i < options->num_child_; ++i) {
      const ParseNode *option = options->children_[i];
      if (OB_ISNULL(option) || option->num_child_ != 2
          || OB_ISNULL(option->children_[0]) || OB_ISNULL(option->children_[1])) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        ObString name(option->children_[0]->str_len_, option->children_[0]->str_value_);
        const ParseNode &argument = *option->children_[1];
        ObObj value;
        for (int64_t j = 0; OB_SUCC(ret) && j < seen.count(); ++j) {
          if (seen.at(j).case_compare(name) == 0) { ret = OB_INVALID_ARGUMENT; }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(seen.push_back(name))) {
        } else if (argument.type_ == T_IDENT) {
          value.set_varchar(ObString(argument.str_len_, argument.str_value_));
        } else {
          ObRawExpr *expr = nullptr;
          bool calculated = false;
          if (OB_FAIL(ObResolverUtils::resolve_const_expr(params_, argument, expr, nullptr))) {
          } else if (OB_FAIL(expr->formalize(session_info_))) {
          } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(
                        session_info_->get_cur_exec_ctx(), expr, value, calculated, *allocator_))) {
          } else if (!calculated || value.is_null()) {
            ret = OB_INVALID_ARGUMENT;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (name.case_compare("format") == 0) {
          if (!value.is_string_type() || value.get_string().case_compare("csv") != 0) {
            ret = OB_NOT_SUPPORTED;
          }
        } else if (name.case_compare("sample_size") == 0) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          ret = config.set_option(name, value);
        }
      }
    }
    if (OB_SUCC(ret)) {
      ObString canonical;
      if (OB_FAIL(FileSqlReader::secure_path(*allocator_, config.path_, canonical, true))) {
      } else {
        config.path_ = canonical;
      }
    }
  }
  const ParseNode *source = node.children_[0];
  ParseResult parsed;
  ObParser parser(*allocator_, session_info_->get_sql_mode());
  ObSqlString query;
  if (OB_SUCC(ret) && source->type_ == T_RELATION_FACTOR) {
    if ((source->num_child_ != 2 && source->num_child_ != 3) || OB_ISNULL(source->children_[1])
        || (source->num_child_ == 3 && source->children_[2] != nullptr)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(query.append("SELECT * FROM "))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < 2; ++i) {
        const ParseNode *identifier = source->children_[i];
        if (identifier != nullptr) {
          if (OB_FAIL(query.append("`"))) {
          }
          for (int64_t j = 0; OB_SUCC(ret) && j < identifier->str_len_; ++j) {
            if (identifier->str_value_[j] == '`' && OB_FAIL(query.append("`"))) {
            } else {
              ret = query.append(identifier->str_value_ + j, 1);
            }
          }
          if (OB_SUCC(ret)) { ret = query.append(i == 0 ? "`." : "`"); }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(parser.parse(query.string(), parsed))) {
      } else if (OB_ISNULL(parsed.result_tree_) || parsed.result_tree_->num_child_ != 1) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        source = parsed.result_tree_->children_[0];
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(source) || source->type_ != T_SELECT) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(resolve(*source))) {
  } else if (get_select_stmt()->has_select_into()) {
    ret = OB_NOT_SUPPORTED;
  } else {
    ObSelectStmt &statement = *get_select_stmt();
    for (int64_t i = 0; OB_SUCC(ret) && i < statement.get_select_item_size(); ++i) {
      const SelectItem &item = statement.get_select_item(i);
      FileSqlColumn column;
      column.name_ = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
      column.type_ = item.expr_->get_data_type();
      ret = config.columns_.push_back(column);
    }
    ObSelectIntoItem *into = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(into = OB_NEWx(ObSelectIntoItem, allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(config.serialize(into->file_copy_options_))) {
    } else {
      into->into_type_ = T_INTO_OUTFILE;
      into->outfile_name_.set_varchar(config.path_);
      into->is_single_ = true;
      statement.set_select_into(into);
      params_.query_ctx_->has_file_sql_ = true;
    }
  }
  return ret;
}

int ObSelectResolver::resolve_file_sql_describe(const ParseNode &node)
{
  int ret = OB_SUCCESS;
  ObSelectResolver source(params_);
  ObSqlString query;
  if (node.num_child_ != 1 || OB_ISNULL(node.children_[0])) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(source.resolve(*node.children_[0]))) {
  } else {
    const ObSelectStmt &statement = *source.get_select_stmt();
    for (int64_t i = 0; OB_SUCC(ret) && i < statement.get_select_item_size(); ++i) {
      const SelectItem &item = statement.get_select_item(i);
      const char *type = "VARCHAR";
      bool nullable = true;
      switch (item.expr_->get_data_type()) {
        case ObNullType: type = "NULL"; break;
        case ObIntType: type = "BIGINT"; break;
        case ObTinyIntType: type = "BOOLEAN"; break;
        case ObFloatType:
        case ObDoubleType: type = "DOUBLE"; break;
        case ObDateType: type = "DATE"; break;
        case ObDateTimeType: type = "DATETIME"; break;
        default: break;
      }
      if (item.expr_->is_column_ref_expr()) {
        const ObColumnRefRawExpr &column = *static_cast<const ObColumnRefRawExpr *>(item.expr_);
        const TableItem *table = statement.get_table_item_by_id(column.get_table_id());
        if (table != nullptr && table->is_function_table()
            && table->function_table_expr_->get_expr_type() == T_FUN_SYS_FILE_SQL) {
          FileSqlConfig config(*allocator_);
          const ObRawExpr *argument = table->function_table_expr_->get_param_expr(0);
          if (OB_FAIL(config.deserialize(static_cast<const ObConstRawExpr *>(argument)->get_value().get_string()))) {
          } else {
            const int64_t index = column.get_column_id() - OB_APP_MIN_COLUMN_ID;
            if (index >= 0 && index < config.columns_.count()) {
              nullable = config.columns_.at(index).nullable_;
            }
          }
        }
      }
      const ObString &name = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(query.append(i == 0 ? "SELECT " : " UNION ALL SELECT "))) {
      } else if (OB_FAIL(sql_append_hex_escape_str(name, query))) {
      } else if (OB_FAIL(query.append_fmt(
                     " AS column_name, '%s' AS column_type, '%s' AS `null`, "
                     "NULL AS `key`, NULL AS `default`, NULL AS extra", type, nullable ? "YES" : "NO"))) {
      }
    }
  }
  ParseResult parsed;
  ObParser parser(*allocator_, session_info_->get_sql_mode());
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(parser.parse(query.string(), parsed))) {
  } else if (OB_ISNULL(parsed.result_tree_) || parsed.result_tree_->num_child_ != 1
             || OB_ISNULL(parsed.result_tree_->children_[0])) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = resolve(*parsed.result_tree_->children_[0]);
    params_.query_ctx_->has_file_sql_ = true;
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
