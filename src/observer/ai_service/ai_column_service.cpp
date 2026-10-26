/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SERVER
#include "ai_column_service.h"
#include "observer/ob_inner_sql_connection.h"
#include "observer/ob_inner_sql_result.h"
#include "share/ob_server_struct.h"
#include "share/ai_column/ai_column_info.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "common/json_type/ob_json_tree.h"
#include "common/number/ob_number_v2.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "query/vector/ob_vector_index_util.h"
#include "lib/utility/ob_smart_call.h"
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace observer
{
namespace
{
struct AiJob
{
  uint64_t column_id_ = 0;
  int64_t version_ = 0;
  int64_t state_ = 0;
  int64_t retries_ = 0;
  ObString predicate_;
  ObString payload_;
  bool null_ = true;
};

int quote_ai_identifier(const ObString &name, ObSqlString &sql)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name[i] == '`' && OB_FAIL(sql.append("`"))) {
    } else { ret = sql.append(name.ptr() + i, 1); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

int write_ai_sql(ObInnerSQLConnection &connection, const ObSqlString &sql, int64_t &affected)
{
  int ret = connection.execute_write(sql.string(), affected, true);
  if (OB_FAIL(ret)) { LOG_WARN("AI column internal write failed", K(ret), K(sql)); }
  return ret;
}

int copy_ai_text(sqlclient::ObMySQLResult &result, int64_t index, ObIAllocator &allocator,
                 ObString &text, bool &is_null)
{
  int ret = OB_SUCCESS;
  ObObj value;
  if (OB_FAIL(result.get_obj(index, value))) {
  } else if (OB_FALSE_IT(is_null = value.is_null())) {
  } else if (!is_null) {
    text = value.get_string();
    if (is_lob_storage(value.get_type())) {
      ObInnerSQLResult &native = static_cast<ObInnerSQLResult &>(result);
      ret = sql::ObTextStringHelper::read_real_string_data(native.result_set().get_exec_context(),
          &allocator, value.get_type(), value.get_collation_type(), value.has_lob_header(), text);
    }
    if (OB_SUCC(ret) && text.length() > 16 * 1024 * 1024) { ret = OB_SIZE_OVERFLOW; }
    if (OB_SUCC(ret)) { ret = ob_write_string(allocator, text, text); }
  }
  return ret;
}

int ai_table_name(ObSchemaGetterGuard &guard, const ObTableSchema &table, ObSqlString &name)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *database = nullptr;
  if (OB_FAIL(guard.get_database_schema(table.get_database_id(), database))) {
  } else if (database == nullptr) { ret = OB_TABLE_NOT_EXIST; }
  else if (OB_FAIL(quote_ai_identifier(database->get_database_name_str(), name))) {
  } else if (OB_FAIL(name.append("."))) {
  } else { ret = quote_ai_identifier(table.get_table_name_str(), name); }
  return ret;
}

// One anchor (lowest column id) owns the entire row request, including cached successes.
int claim_ai_group(ObInnerSQLConnection &connection, const ObString &owner,
                    uint64_t &table_id, ObString &request_id, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  bool transaction = false;
  int64_t anchor = 0, affected = 0;
  ObString request;
  if (OB_SUCC(ret)) {
    ObISQLClient::ReadResult result;
    const ObString query = ObString::make_string(
      "SELECT CAST(a.table_id AS UNSIGNED),a.column_id,a.request_id FROM oceanbase.__all_ai_column_task a "
      "WHERE (a.lease_owner IS NULL OR a.lease_expires<NOW(6)) "
      "AND NOT EXISTS (SELECT 1 FROM oceanbase.__all_ai_column_task b WHERE b.table_id=a.table_id "
      "AND b.request_id=a.request_id AND b.column_id<a.column_id) "
      "AND (EXISTS (SELECT 1 FROM oceanbase.__all_ai_column_task b WHERE b.table_id=a.table_id "
      "AND b.request_id=a.request_id AND b.state IN (1,2,3) AND b.eligible_ts<=NOW(6)) "
      "OR NOT EXISTS (SELECT 1 FROM oceanbase.__all_ai_column_task b WHERE b.table_id=a.table_id "
      "AND b.request_id=a.request_id AND b.state NOT IN (4,5))) ORDER BY a.gmt_create LIMIT 1");
    if (OB_FAIL(connection.execute_read(query, result))) {
    } else if (OB_FAIL(result.get_result()->next())) {
    } else if (OB_FAIL(result.get_result()->get_uint(0L, table_id))) {
    } else if (OB_FAIL(result.get_result()->get_int(1L, anchor))) {
    } else if (OB_FAIL(result.get_result()->get_varchar(2L, request))) {
    } else { ret = ob_write_string(allocator, request, request_id); }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(connection.start_transaction())) {
    } else { transaction = true; }
  }
  ObSqlString claim;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(claim.append("UPDATE oceanbase.__all_ai_column_task SET lease_owner="))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(owner, claim))) {
  } else if (OB_FAIL(claim.append_fmt(",lease_expires=DATE_ADD(NOW(6),INTERVAL 60 SECOND) WHERE table_id=%lu AND column_id=%ld AND request_id=",
                                     table_id, anchor))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(request_id, claim))) {
  } else if (OB_FAIL(claim.append(" AND (lease_owner IS NULL OR lease_expires<NOW(6))"))) {
  } else if (OB_FAIL(write_ai_sql(connection, claim, affected))) {
  } else if (affected != 1) { ret = OB_EAGAIN; }
  ObSqlString group;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(group.append("UPDATE oceanbase.__all_ai_column_task SET lease_owner="))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(owner, group))) {
  } else if (OB_FAIL(group.append_fmt(",lease_expires=DATE_ADD(NOW(6),INTERVAL 60 SECOND),"
           "state=IF(state IN (1,2,3) AND eligible_ts<=NOW(6),2,state) WHERE table_id=%lu AND request_id=", table_id))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(request_id, group))) {
  } else { ret = write_ai_sql(connection, group, affected); }
  if (transaction) {
    if (OB_SUCC(ret)) { ret = connection.commit(); }
    else { (void)connection.rollback(); }
  }
  return ret;
}

int load_ai_group(ObInnerSQLConnection &connection, uint64_t table_id, const ObString &request,
                   const ObString &owner, ObIAllocator &allocator, ObIArray<AiJob> &jobs, bool lock)
{
  int ret = OB_SUCCESS;
  ObSqlString query;
  if (OB_FAIL(query.append_fmt("SELECT CAST(column_id AS UNSIGNED),definition_version,state,retry_count,rowkey_predicate,result_payload "
                              "FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND request_id=", table_id))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(request, query))) {
  } else if (OB_FAIL(query.append(" AND lease_owner="))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(owner, query))) {
  } else if (OB_FAIL(query.append(lock ? " ORDER BY column_id FOR UPDATE" : " ORDER BY column_id"))) {
  } else {
    ObISQLClient::ReadResult result;
    if (OB_FAIL(connection.execute_read(query.string(), result))) {
    } else {
      sqlclient::ObMySQLResult &rows = *result.get_result();
      while (OB_SUCC(ret) && OB_SUCC(ret = rows.next())) {
        AiJob job;
        ObString predicate;
        if (OB_FAIL(rows.get_uint(0L, job.column_id_))) {
        } else if (OB_FAIL(rows.get_int(1L, job.version_))) {
        } else if (OB_FAIL(rows.get_int(2L, job.state_))) {
        } else if (OB_FAIL(rows.get_int(3L, job.retries_))) {
        } else if (OB_FAIL(rows.get_varchar(4L, predicate))) {
        } else if (OB_FAIL(ob_write_string(allocator, predicate, job.predicate_))) {
        } else if (OB_FAIL(copy_ai_text(rows, 5, allocator, job.payload_, job.null_))) {
        } else if (jobs.count() >= 1024) { ret = OB_SIZE_OVERFLOW; }
        else { ret = jobs.push_back(job); }
      }
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    }
  }
  return ret;
}

int validate_ai_output(const ObColumnSchemaV2 &column, const ObString &payload,
                        bool is_null, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (is_null) {
  } else if (ob_is_string_type(column.get_data_type()) || ob_is_text_tc(column.get_data_type())) {
    const int64_t length = ObCharset::charset_type_by_coll(column.get_collation_type()) == CHARSET_BINARY
        || is_lob_storage(column.get_data_type()) ? payload.length()
        : ObCharset::strlen_char(CS_TYPE_UTF8MB4_BIN, payload.ptr(), payload.length());
    if (column.get_data_length() >= 0 && length > column.get_data_length()) { ret = OB_SIZE_OVERFLOW; }
  } else if (column.get_data_type() == ObJsonType || column.get_data_type() == ObCollectionSQLType) {
    ObIJsonBase *json = nullptr;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, payload, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json))) {
    } else if (column.get_data_type() == ObCollectionSQLType) {
      int64_t dimension = 0;
      if (OB_FAIL(ObVectorIndexUtil::get_vector_dim_from_extend_type_info(column.get_extended_type_info(), dimension))) {
      } else if (json->json_type() != ObJsonNodeType::J_ARRAY || json->element_count() != dimension) { ret = OB_INVALID_DATA; }
      for (uint64_t i = 0; OB_SUCC(ret) && i < json->element_count(); ++i) {
        ObIJsonBase *element = nullptr;
        double number = 0;
        if (OB_FAIL(json->get_array_element(i, element))) {
        } else if (element->json_type() != ObJsonNodeType::J_INT && element->json_type() != ObJsonNodeType::J_UINT
                   && element->json_type() != ObJsonNodeType::J_DOUBLE && element->json_type() != ObJsonNodeType::J_DECIMAL) { ret = OB_INVALID_DATA; }
        else if (OB_FAIL(element->to_double(number))) {
        } else if (!std::isfinite(number)) { ret = OB_INVALID_DATA; }
      }
    }
  } else if (ob_is_integer_type(column.get_data_type()) || ob_is_numeric_type(column.get_data_type())) {
    char *text = static_cast<char *>(allocator.alloc(payload.length() + 1));
    if (text == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else {
      MEMCPY(text, payload.ptr(), payload.length()); text[payload.length()] = '\0';
      char *end = nullptr;
      errno = 0;
      const ObObjType type = column.get_data_type();
      if (ob_is_integer_type(column.get_data_type())) {
        if (ob_is_uint_tc(column.get_data_type())) {
          const uint64_t value = std::strtoull(text, &end, 10);
          for (int64_t i = 0; i < payload.length(); ++i) {
            if (text[i] == '-') { ret = OB_INVALID_DATA; }
          }
          const int64_t widths[] = {8, 16, 24, 32, 64};
          const int64_t bits = widths[type - ObUTinyIntType];
          if (bits < 64 && value > ((uint64_t(1) << bits) - 1)) { ret = OB_INVALID_DATA; }
        } else {
          const int64_t value = std::strtoll(text, &end, 10);
          const int64_t widths[] = {8, 16, 24, 32, 64};
          const int64_t bits = widths[type - ObTinyIntType];
          if (bits < 64 && (value < -(int64_t(1) << (bits - 1)) || value > ((int64_t(1) << (bits - 1)) - 1))) {
            ret = OB_INVALID_DATA;
          }
        }
      } else {
        double value = std::strtod(text, &end);
        if (!std::isfinite(value) || (ob_is_float_type(type) && fabs(value) > std::numeric_limits<float>::max())
            || ((type == ObUFloatType || type == ObUDoubleType || type == ObUNumberType) && value < 0)) {
          ret = OB_INVALID_DATA;
        }
      }
      if (errno == ERANGE || end == text || end != text + payload.length()) { ret = OB_INVALID_DATA; }
      if (OB_SUCC(ret) && (ob_is_number_tc(type) || type == ObDecimalIntType)) {
        number::ObNumber value;
        if (OB_FAIL(value.from(payload.ptr(), payload.length(), allocator))) {
        } else { ret = value.check_and_round(column.get_data_precision(), column.get_data_scale()); }
      }
    }
  }
  return ret;
}

int infer_ai_job(ObInnerSQLConnection &connection, const ObTableSchema &table, const ObSqlString &name,
                 const ObString &request, const AiJob &job, ObIAllocator &allocator,
                 ObString &payload, bool &is_null, int64_t &max_retries)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *column = table.get_column_schema(job.column_id_);
  AiColumnInfo info(allocator);
  ObSqlString query;
  if (column == nullptr || !column->is_ai_column() || ai_column_definition_version(*column) != job.version_) {
    ret = OB_SCHEMA_EAGAIN;
  } else if (OB_FAIL(info.deserialize(column->get_ai_column_info()))) {
  } else if (OB_FALSE_IT(max_retries = info.max_retries_)) {
  } else if (OB_FAIL(query.append_fmt("SELECT CAST((%.*s) AS CHAR) FROM %s WHERE (%.*s) AND ",
                        info.normalized_expr_.length(), info.normalized_expr_.ptr(), name.ptr(),
                        job.predicate_.length(), job.predicate_.ptr()))) {
  } else if (OB_FAIL(quote_ai_identifier(table.get_column_schema(info.request_id_)->get_column_name_str(), query))) {
  } else if (OB_FAIL(query.append_fmt("=%.*s AND ", request.length(), request.ptr()))) {
  } else if (OB_FAIL(quote_ai_identifier(table.get_column_schema(info.result_spec_id_)->get_column_name_str(), query))) {
  } else if (OB_FAIL(query.append_fmt("=%ld", job.version_))) {
  } else {
    ObISQLClient::ReadResult result;
    if (OB_FAIL(connection.execute_read(query.string(), result, true))) {
    } else if (OB_FAIL(result.get_result()->next())) {
    } else if (OB_FAIL(copy_ai_text(*result.get_result(), 0, allocator, payload, is_null))) {
    } else { ret = validate_ai_output(*column, payload, is_null, allocator); }
  }
  return ret;
}

int save_ai_outcome(ObInnerSQLConnection &connection, uint64_t table_id, const ObString &request,
                     const ObString &owner, const AiJob &job, int outcome, int64_t max_retries,
                     const ObString &payload, bool is_null)
{
  int ret = OB_SUCCESS;
  ObSqlString query;
  int64_t affected = 0;
  const bool retry = outcome != OB_SUCCESS && job.retries_ < max_retries;
  if (OB_FAIL(query.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=%d,retry_count=%ld,"
       "eligible_ts=DATE_ADD(NOW(6),INTERVAL %d SECOND),result_payload=", outcome == OB_SUCCESS ? 4 : retry ? 3 : 5,
       job.retries_ + (retry ? 1 : 0), retry ? 1 : 0))) {
  } else if ((outcome != OB_SUCCESS || is_null) && OB_FAIL(query.append("NULL"))) {
  } else if (outcome == OB_SUCCESS && !is_null && OB_FAIL(sql_append_hex_escape_str(payload, query))) {
  } else if (OB_FAIL(query.append_fmt(",last_error='%d' WHERE table_id=%lu AND column_id=%lu AND state=2 AND request_id=",
                                      outcome, table_id, job.column_id_))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(request, query))) {
  } else if (OB_FAIL(query.append(" AND lease_owner="))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(owner, query))) {
  } else { ret = write_ai_sql(connection, query, affected); }
  return ret;
}

int finish_ai_group(ObInnerSQLConnection &connection, uint64_t table_id, const ObString &request,
                     const ObString &owner, ObIAllocator &allocator)
{
  int ret = connection.start_transaction();
  ObSEArray<AiJob, 8> jobs;
  bool terminal = true;
  int64_t affected = 0;
  if (OB_SUCC(ret)) { ret = load_ai_group(connection, table_id, request, owner, allocator, jobs, true); }
  for (int64_t i = 0; i < jobs.count(); ++i) { terminal = terminal && (jobs.at(i).state_ == 4 || jobs.at(i).state_ == 5); }
  if (OB_SUCC(ret) && terminal && !jobs.empty()) {
    ObSchemaGetterGuard guard;
    const ObTableSchema *table = nullptr;
    ObSqlString name;
    ObSqlString update;
    ObSqlString conditions;
    bool current = true;
    if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    } else if (OB_FAIL(guard.get_table_schema(table_id, table))) {
    } else if (table == nullptr) { current = false; }
    else if (OB_FAIL(ai_table_name(guard, *table, name))) {
    } else if (OB_FAIL(update.append_fmt("UPDATE %s SET ", name.ptr()))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && current && i < jobs.count(); ++i) {
      const AiJob &job = jobs.at(i);
      const ObColumnSchemaV2 *column = table->get_column_schema(job.column_id_);
      AiColumnInfo info(allocator);
      if (column == nullptr || !column->is_ai_column() || ai_column_definition_version(*column) != job.version_) { current = false; }
      else if (OB_FAIL(info.deserialize(column->get_ai_column_info()))) {
      } else if (i > 0 && OB_FAIL(update.append(","))) {
      } else if (OB_FAIL(quote_ai_identifier(column->get_column_name_str(), update))) {
      } else if (OB_FAIL(update.append("="))) {
      } else if ((job.state_ == 5 || job.null_) && OB_FAIL(update.append("NULL"))) {
      } else if (job.state_ == 4 && !job.null_ && OB_FAIL(update.append("CONVERT("))) {
      } else if (job.state_ == 4 && !job.null_ && OB_FAIL(sql_append_hex_escape_str(job.payload_, update))) {
      } else if (job.state_ == 4 && !job.null_ && OB_FAIL(update.append(" USING utf8mb4)"))) {
      } else if (OB_FAIL(update.append(","))) {
      } else if (OB_FAIL(quote_ai_identifier(table->get_column_schema(info.state_id_)->get_column_name_str(), update))) {
      } else if (OB_FAIL(update.append_fmt("=%d", job.state_ == 4 ? 2 : 3))) {
      } else if (OB_FAIL(conditions.append(" AND "))) {
      } else if (OB_FAIL(quote_ai_identifier(table->get_column_schema(info.request_id_)->get_column_name_str(), conditions))) {
      } else if (OB_FAIL(conditions.append_fmt("=%.*s AND ", request.length(), request.ptr()))) {
      } else if (OB_FAIL(quote_ai_identifier(table->get_column_schema(info.result_spec_id_)->get_column_name_str(), conditions))) {
      } else { ret = conditions.append_fmt("=%ld", job.version_); }
    }
    if (OB_SUCC(ret) && current) {
      if (OB_FAIL(update.append_fmt(" WHERE (%.*s)%s", jobs.at(0).predicate_.length(), jobs.at(0).predicate_.ptr(), conditions.ptr()))) {
      } else { ret = write_ai_sql(connection, update, affected); }
    }
    if (OB_SUCC(ret)) {
      ObSqlString cleanup;
      if (OB_FAIL(cleanup.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND request_id=", table_id))) {
      } else if (OB_FAIL(sql_append_hex_escape_str(request, cleanup))) {
      } else if (OB_FAIL(cleanup.append(" AND lease_owner="))) {
      } else if (OB_FAIL(sql_append_hex_escape_str(owner, cleanup))) {
      } else { ret = write_ai_sql(connection, cleanup, affected); }
    }
  } else if (OB_SUCC(ret) && !jobs.empty()) {
    ObSqlString release;
    if (OB_FAIL(release.append_fmt("UPDATE oceanbase.__all_ai_column_task SET lease_owner=NULL,lease_expires=NULL WHERE table_id=%lu AND request_id=", table_id))) {
    } else if (OB_FAIL(sql_append_hex_escape_str(request, release))) {
    } else if (OB_FAIL(release.append(" AND lease_owner="))) {
    } else if (OB_FAIL(sql_append_hex_escape_str(owner, release))) {
    } else { ret = write_ai_sql(connection, release, affected); }
  }
  if (OB_SUCC(ret)) { ret = connection.commit(); }
  else { (void)connection.rollback(); }
  return ret;
}

int run_ai_group(ObInnerSQLConnection &connection, const ObString &owner)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnWorker");
  uint64_t table_id = 0;
  ObString request;
  ObSEArray<AiJob, 8> jobs;
  if (OB_FAIL(claim_ai_group(connection, owner, table_id, request, allocator))) {
    if (ret != OB_ITER_END && ret != OB_EAGAIN) { LOG_WARN("AI column claim failed", K(ret), K(table_id)); }
  } else if (OB_FAIL(load_ai_group(connection, table_id, request, owner, allocator, jobs, false))) {
    LOG_WARN("AI column group read failed", K(ret), K(table_id), K(request));
  } else {
    ObSchemaGetterGuard guard;
    const ObTableSchema *table = nullptr;
    ObSqlString name;
    int schema_status = GCTX.schema_service_->get_runtime_schema_guard(guard);
    if (schema_status == OB_SUCCESS) { schema_status = guard.get_table_schema(table_id, table); }
    if (schema_status == OB_SUCCESS && table != nullptr) { schema_status = ai_table_name(guard, *table, name); }
    for (int64_t i = 0; OB_SUCC(ret) && i < jobs.count(); ++i) {
      const AiJob &job = jobs.at(i);
      if (job.state_ == 2) {
        ObArenaAllocator payload_allocator("AiColumnResult");
        ObString payload;
        bool is_null = true;
        int64_t retries = 0;
        ObSqlString heartbeat;
        int64_t affected = 0;
        if (OB_FAIL(heartbeat.append_fmt("UPDATE oceanbase.__all_ai_column_task SET lease_expires=DATE_ADD(NOW(6),INTERVAL 60 SECOND) WHERE table_id=%lu AND request_id=", table_id))) {
        } else if (OB_FAIL(sql_append_hex_escape_str(request, heartbeat))) {
        } else if (OB_FAIL(heartbeat.append(" AND lease_owner="))) {
        } else if (OB_FAIL(sql_append_hex_escape_str(owner, heartbeat))) {
        } else if (OB_FAIL(write_ai_sql(connection, heartbeat, affected))) {
        }
        bool owned = affected > 0;
        if (OB_SUCC(ret) && !owned) {
          // UPDATE counts changed rows. A timestamp at the same precision can be unchanged
          // while the lease is still ours, so verify ownership before deciding to skip work.
          ObSqlString check;
          ObISQLClient::ReadResult result;
          if (OB_FAIL(check.append_fmt("SELECT 1 FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND column_id=%lu AND state=2 AND request_id=", table_id, job.column_id_))) {
          } else if (OB_FAIL(sql_append_hex_escape_str(request, check))) {
          } else if (OB_FAIL(check.append(" AND lease_owner="))) {
          } else if (OB_FAIL(sql_append_hex_escape_str(owner, check))) {
          } else if (OB_FAIL(connection.execute_read(check.string(), result))) {
          } else if (OB_SUCC(ret = result.get_result()->next())) { owned = true; }
          else if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        }
        if (OB_SUCC(ret) && owned) {
          const int outcome = schema_status != OB_SUCCESS ? schema_status : table == nullptr ? OB_TABLE_NOT_EXIST
               : infer_ai_job(connection, *table, name, request, job, payload_allocator, payload, is_null, retries);
          if (outcome != OB_SUCCESS) { LOG_WARN("AI column inference failed", K(outcome), K(table_id), K(request), K(job.column_id_)); }
          ret = save_ai_outcome(connection, table_id, request, owner, job, outcome, retries, payload, is_null);
        }
      }
    }
    if (OB_SUCC(ret)) { ret = finish_ai_group(connection, table_id, request, owner, allocator); }
  }
  return ret;
}

int recover_ai_leases(ObInnerSQLConnection &connection, int64_t boot_id)
{
  int ret = OB_SUCCESS;
  ObSqlString query;
  int64_t affected = 0;
  if (OB_FAIL(query.append_fmt("UPDATE oceanbase.__all_ai_column_task SET lease_owner=NULL,lease_expires=NULL,"
       "state=IF(state=2,1,state) WHERE lease_owner IS NOT NULL AND lease_owner NOT LIKE '%ld-%%'", boot_id))) {
  } else { ret = write_ai_sql(connection, query, affected); }
  return ret;
}

int backfill_ai_columns(ObInnerSQLConnection &connection)
{
  int ret = OB_SUCCESS;
  ObSEArray<std::pair<uint64_t, uint64_t>, 16> columns;
  {
    ObISQLClient::ReadResult result;
    if (OB_FAIL(connection.execute_read(ObString("SELECT CAST(table_id AS UNSIGNED),CAST(column_id AS UNSIGNED) FROM oceanbase.__all_ai_column"), result))) {
    } else {
      while (OB_SUCC(ret) && OB_SUCC(ret = result.get_result()->next())) {
        uint64_t table = 0, column = 0;
        if (OB_FAIL(result.get_result()->get_uint(0L, table))) {
        } else if (OB_FAIL(result.get_result()->get_uint(1L, column))) {
        } else { ret = columns.push_back(std::make_pair(table, column)); }
      }
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    }
  }
  ObSchemaGetterGuard guard;
  if (OB_SUCC(ret)) { ret = GCTX.schema_service_->get_runtime_schema_guard(guard); }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    ObArenaAllocator allocator("AiColBackfill");
    const ObTableSchema *table = nullptr;
    const ObColumnSchemaV2 *column = nullptr;
    AiColumnInfo info(allocator);
    ObSqlString name;
    ObSqlString query;
    if (OB_FAIL(guard.get_table_schema(columns.at(i).first, table))) {
    } else if (table == nullptr || table->is_index_table()
               || (column = table->get_column_schema(columns.at(i).second)) == nullptr || !column->is_ai_column()) {
      // Metadata can commit before the asynchronous schema refresh reaches this guard.
    } else if (OB_FAIL(info.deserialize(column->get_ai_column_info()))) {
    } else if (OB_FAIL(ai_table_name(guard, *table, name))) {
    } else if (OB_FAIL(query.append_fmt("ALTER TABLE %s RETRY AI COLUMN ", name.ptr()))) {
    } else if (OB_FAIL(quote_ai_identifier(column->get_column_name_str(), query))) {
    } else if (OB_FAIL(query.append(" WHERE "))) {
    } else if (OB_FAIL(quote_ai_identifier(table->get_column_schema(info.request_id_)->get_column_name_str(), query))) {
    } else if (OB_FAIL(query.append("=0 OR "))) {
    } else if (OB_FAIL(quote_ai_identifier(table->get_column_schema(info.result_spec_id_)->get_column_name_str(), query))) {
    } else if (OB_FAIL(query.append_fmt("<>%ld", ai_column_definition_version(*column)))) {
    } else {
      int64_t affected = 0;
      // RETRY's resolver authorizes its generated hidden assignments. Its execution uses
      // the regular transactional task hook, just like an explicit user RETRY statement.
      connection.get_session().set_ai_column_internal(false);
      ret = write_ai_sql(connection, query, affected);
      connection.get_session().set_ai_column_internal(true);
    }
  }
  return ret;
}
} // namespace

AiColumnService &AiColumnService::instance()
{
  static AiColumnService service;
  return service;
}

int AiColumnService::start_service()
{
  boot_id_ = ObTimeUtility::current_time();
  return start();
}

void AiColumnService::stop_service()
{
  stop();
  wait();
}

void AiColumnService::run1()
{
  int ret = OB_SUCCESS;
  lib::set_thread_name("AiColumn");
  sqlclient::ObISQLConnectionGuard guard;
  int status = ObInnerSQLConnection::create_connection_with_owned_session(false, 0, guard);
  if (status == OB_SUCCESS) {
    ObInnerSQLConnection &connection = *static_cast<ObInnerSQLConnection *>(guard.get_ptr());
    // Task and source-row updates use ordinary DML table parameters, not a DDL shadow table.
    connection.get_session().get_ddl_info().set_is_ddl(false);
    connection.get_session().set_ai_column_internal(true);
    connection.set_user_timeout(30000000);
    ObSqlString owner;
    (void)owner.append_fmt("%ld-%lu", boot_id_, get_thread_idx());
    int64_t last_backfill = 0;
    if (get_thread_idx() == 0) {
      status = SMART_CALL_LARGE(recover_ai_leases(connection, boot_id_));
      if (status != OB_SUCCESS) { LOG_WARN("AI column lease recovery failed", K(status)); }
    }
    while (!has_set_stop()) {
      if (get_thread_idx() == 0 && ObTimeUtility::current_time() - last_backfill >= 1000000) {
        status = SMART_CALL_LARGE(backfill_ai_columns(connection));
        last_backfill = ObTimeUtility::current_time();
        if (status != OB_SUCCESS) { LOG_WARN("AI column backfill failed", K(status)); }
      }
      status = SMART_CALL_LARGE(run_ai_group(connection, owner.string()));
      if (status != OB_SUCCESS && status != OB_ITER_END && status != OB_EAGAIN) {
        LOG_WARN("AI column worker iteration failed", K(status));
      }
      if (status != OB_SUCCESS) { ob_usleep(100000); }
    }
  } else { LOG_ERROR("cannot create AI column worker connection", K(status)); }
}
} // namespace observer
} // namespace oceanbase
