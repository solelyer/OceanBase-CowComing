/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "common/mysqlclient/ob_isql_client.h"

namespace oceanbase
{
namespace share
{
namespace schema { class ObColumnSchemaV2; class ObTableSchema; }

struct AiColumnInfo
{
  explicit AiColumnInfo(common::ObIAllocator &allocator)
      : allocator_(allocator), max_retries_(2), state_id_(0), request_id_(0), result_spec_id_(0) {}
  int serialize(common::ObString &result) const;
  int deserialize(const common::ObString &json);
  common::ObIAllocator &allocator_;
  common::ObString expression_;
  common::ObString normalized_expr_;
  int64_t max_retries_;
  uint64_t state_id_;
  uint64_t request_id_;
  uint64_t result_spec_id_;
  common::ObSEArray<uint64_t, 8> dependencies_;
};

int persist_ai_column(common::ObISQLClient &client, const schema::ObColumnSchemaV2 &column,
                      int64_t definition_version = 0);
int persist_ai_columns(common::ObISQLClient &client, const schema::ObTableSchema &table);
int remove_ai_columns(common::ObISQLClient &client, uint64_t table_id, uint64_t column_id = 0);
int64_t ai_column_definition_version(const schema::ObColumnSchemaV2 &column);
} // namespace share
} // namespace oceanbase
