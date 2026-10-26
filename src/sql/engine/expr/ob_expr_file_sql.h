/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "sql/engine/expr/ob_expr_operator.h"

namespace oceanbase
{
namespace sql
{
// A resolved file table carries its immutable schema and file snapshot in one
// constant argument. ObFunctionTableOp consumes it; scalar execution is invalid.
class FileSqlExpr : public ObFuncExprOperator
{
public:
  explicit FileSqlExpr(common::ObIAllocator &allocator)
      : ObFuncExprOperator(allocator, T_FUN_SYS_FILE_SQL, "__file_scan", 1,
                           NOT_VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION) {}
  int calc_result_type1(ObExprResType &type, ObExprResType &argument,
                        common::ObExprTypeCtx &context) const override;
  int cg_expr(ObExprCGCtx &context, const ObRawExpr &raw, ObExpr &expression) const override;
  static int eval(const ObExpr &expression, ObEvalCtx &context, common::ObDatum &result);
};
} // namespace sql
} // namespace oceanbase
