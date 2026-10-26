/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SHARE_SCHEMA
#include "ai_column_info.h"
#include "common/json_type/ob_json_tree.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
using namespace common;
namespace share
{
int AiColumnInfo::serialize(ObString &result) const
{
  int ret = OB_SUCCESS;
  ObJsonObject object(&allocator_);
  ObJsonArray dependencies(&allocator_);
  ObJsonString expression(expression_);
  ObJsonString normalized(normalized_expr_);
  ObJsonInt retries(max_retries_);
  ObJsonInt state(state_id_);
  ObJsonInt request(request_id_);
  ObJsonInt spec(result_spec_id_);
  for (int64_t i = 0; OB_SUCC(ret) && i < dependencies_.count(); ++i) {
    ObJsonInt *value = OB_NEWx(ObJsonInt, &allocator_, dependencies_.at(i));
    if (OB_ISNULL(value)) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else { ret = dependencies.append(value); }
  }
  ObJsonBuffer buffer(&allocator_);
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(object.add("expression", &expression))) {
  } else if (OB_FAIL(object.add("normalized_expr", &normalized))) {
  } else if (OB_FAIL(object.add("max_retries", &retries))) {
  } else if (OB_FAIL(object.add("state_id", &state))) {
  } else if (OB_FAIL(object.add("request_id", &request))) {
  } else if (OB_FAIL(object.add("result_spec_id", &spec))) {
  } else if (OB_FAIL(object.add("dependencies", &dependencies))) {
  } else if (OB_FAIL(object.print(buffer, false))) {
  } else { ret = buffer.get_result_string(result); }
  return ret;
}

int AiColumnInfo::deserialize(const ObString &json)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *root = nullptr;
  dependencies_.reuse();
  if (json.empty() || json.length() > 65535) { ret = OB_INVALID_DATA; }
  else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator_, json, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, root))) {
  } else if (OB_ISNULL(root) || root->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_DATA;
  } else {
    ObJsonObject &object = *static_cast<ObJsonObject *>(root);
    const auto number = [&](const char *key, int64_t &value) -> int {
      int status = OB_SUCCESS;
      ObJsonNode *node = object.get_value(key);
      if (node == nullptr || (node->json_type() != ObJsonNodeType::J_INT && node->json_type() != ObJsonNodeType::J_UINT)) {
        status = OB_INVALID_DATA;
      } else { status = node->to_int(value, true); }
      return status;
    };
    ObJsonNode *expression = object.get_value("expression");
    ObJsonNode *normalized = object.get_value("normalized_expr");
    ObJsonNode *deps = object.get_value("dependencies");
    int64_t state = 0, request = 0, spec = 0;
    if (expression == nullptr || expression->json_type() != ObJsonNodeType::J_STRING
        || normalized == nullptr || normalized->json_type() != ObJsonNodeType::J_STRING
        || deps == nullptr || deps->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(number("max_retries", max_retries_))) {
    } else if (OB_FAIL(number("state_id", state))) {
    } else if (OB_FAIL(number("request_id", request))) {
    } else if (OB_FAIL(number("result_spec_id", spec))) {
    } else if (max_retries_ < 0 || max_retries_ > 16 || state < 0 || request < 0 || spec < 0
               || static_cast<ObJsonArray *>(deps)->element_count() > 1024) {
      ret = OB_INVALID_DATA;
    } else {
      state_id_ = state; request_id_ = request; result_spec_id_ = spec;
      expression_ = static_cast<ObJsonString *>(expression)->get_str();
      normalized_expr_ = static_cast<ObJsonString *>(normalized)->get_str();
      for (uint64_t i = 0; OB_SUCC(ret) && i < deps->element_count(); ++i) {
        ObJsonNode *node = static_cast<ObJsonArray *>(deps)->get_value(i);
        int64_t value = 0;
        if (OB_ISNULL(node) || (node->json_type() != ObJsonNodeType::J_INT && node->json_type() != ObJsonNodeType::J_UINT)) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(node->to_int(value, true))) {
        } else if (value < OB_APP_MIN_COLUMN_ID) {
          ret = OB_INVALID_DATA;
        } else { ret = dependencies_.push_back(value); }
      }
    }
  }
  return ret;
}

int persist_ai_column(ObISQLClient &client, const schema::ObColumnSchemaV2 &column, int64_t definition_version)
{
  int ret = OB_SUCCESS;
  if (column.is_ai_column()) {
    ObArenaAllocator allocator("AiColumnMeta");
    AiColumnInfo info(allocator);
    ObDMLSqlSplicer dml;
    int64_t affected = 0;
    UNUSED(definition_version);
    const int64_t version = ai_column_definition_version(column);
    if (OB_FAIL(info.deserialize(column.get_ai_column_info()))) {
    } else if (OB_FAIL(dml.add_pk_column("table_id", column.get_table_id()))) {
    } else if (OB_FAIL(dml.add_pk_column("column_id", column.get_column_id()))) {
    } else if (OB_FAIL(dml.add_column("column_name", ObHexEscapeSqlStr(column.get_column_name_str())))) {
    } else if (OB_FAIL(dml.add_column("definition", ObHexEscapeSqlStr(column.get_ai_column_info())))) {
    } else if (OB_FAIL(dml.add_column("normalized_expr", ObHexEscapeSqlStr(info.normalized_expr_)))) {
    } else if (OB_FAIL(dml.add_column("definition_version", version))) {
    } else if (OB_FAIL(dml.add_column("state_column_id", info.state_id_))) {
    } else if (OB_FAIL(dml.add_column("request_column_id", info.request_id_))) {
    } else if (OB_FAIL(dml.add_column("result_spec_column_id", info.result_spec_id_))) {
    } else if (OB_FAIL(dml.add_column("max_retries", info.max_retries_))) {
    } else if (OB_FAIL(dml.add_column("sync_mode", "async"))) {
    } else {
      ObDMLExecHelper executor(client);
      ret = executor.exec_replace("oceanbase.__all_ai_column", dml, affected);
    }
  }
  return ret;
}

int persist_ai_columns(ObISQLClient &client, const schema::ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  for (schema::ObTableSchema::const_column_iterator it = table.column_begin(); OB_SUCC(ret) && !table.is_index_table() && it != table.column_end(); ++it) {
    if ((*it)->is_ai_column()) {
      schema::ObColumnSchemaV2 column;
      if (OB_FAIL(column.assign(**it))) {
      } else {
        column.set_table_id(table.get_table_id());
        ret = persist_ai_column(client, column, table.get_schema_version());
      }
    }
  }
  return ret;
}

int64_t ai_column_definition_version(const schema::ObColumnSchemaV2 &column)
{
  const ObAccuracy &accuracy = column.get_accuracy();
  const int64_t type[] = {column.get_data_type(), accuracy.get_length(), accuracy.get_precision(),
                         accuracy.get_scale(), column.get_collation_type()};
  uint64_t hash = murmurhash(type, sizeof(type), column.get_ai_column_info().hash());
  for (int64_t i = 0; i < column.get_extended_type_info().count(); ++i) {
    const ObString &value = column.get_extended_type_info().at(i);
    hash = murmurhash(value.ptr(), value.length(), hash);
  }
  return MAX(1, static_cast<int64_t>(hash & INT64_MAX));
}

int remove_ai_columns(ObISQLClient &client, uint64_t table_id, uint64_t column_id)
{
  int ret = OB_SUCCESS;
  int64_t affected = 0;
  const char *tables[] = {"__all_ai_column", "__all_ai_column_task"};
  for (int64_t i = 0; OB_SUCC(ret) && i < 2; ++i) {
    ObSqlString query;
    if (OB_FAIL(query.append_fmt("DELETE FROM oceanbase.%s WHERE table_id=%lu", tables[i], table_id))) {
    } else if (column_id != 0 && OB_FAIL(query.append_fmt(" AND column_id=%lu", column_id))) {
    } else { ret = client.write(query.ptr(), affected); }
  }
  return ret;
}
} // namespace share
} // namespace oceanbase
