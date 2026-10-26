/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "sql/resolver/ob_resolver_define.h"

namespace oceanbase
{
namespace sql
{
class ObDMLStmt;
class ObRawExpr;
class ObColumnRefRawExpr;
struct ColumnItem;
struct TableItem;
int track_ai_column_read(ObResolverParams &params, ObDMLStmt &statement, const TableItem &table, const ColumnItem &column);
int build_ai_column_request(ObResolverParams &params, ObDMLStmt &statement,
                            uint64_t table_id, ObRawExpr *&expression);
int build_ai_column_default(ObResolverParams &params, ObDMLStmt &statement,
                              const ColumnItem &column, ObRawExpr *&expression, bool &handled);
int check_ai_column_assignment(ObResolverParams &params, ObDMLStmt &statement,
                                 const ObColumnRefRawExpr &column);
} // namespace sql
} // namespace oceanbase
