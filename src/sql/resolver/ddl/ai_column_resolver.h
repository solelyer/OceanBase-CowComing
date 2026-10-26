/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "sql/resolver/ob_resolver_define.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
namespace sql
{
int resolve_ai_column_attribute(ObResolverParams &params, share::schema::ObColumnSchemaV2 &column,
                                 const ParseNode &attribute);
int finish_ai_column(ObResolverParams &params, const share::schema::ObTableSchema &source,
                       share::schema::ObColumnSchemaV2 &column, uint64_t &next_id,
                       common::ObIArray<share::schema::ObColumnSchemaV2> &hidden);
int finish_ai_columns(ObResolverParams &params, share::schema::ObTableSchema &table, uint64_t &next_id);
} // namespace sql
} // namespace oceanbase
