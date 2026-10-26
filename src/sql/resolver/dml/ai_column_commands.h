/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "sql/resolver/ob_resolver_define.h"
namespace oceanbase
{
namespace sql
{
int build_ai_column_command(ObResolverParams &params, const ParseNode &command, const ParseNode *&native);
} // namespace sql
} // namespace oceanbase
