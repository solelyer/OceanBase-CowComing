/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_file_sql.h"

namespace oceanbase
{
namespace sql
{
int FileSqlExpr::calc_result_type1(ObExprResType &type, ObExprResType &argument,
                                   common::ObExprTypeCtx &context) const
{
  UNUSED(context);
  type = argument;
  return common::OB_SUCCESS;
}

int FileSqlExpr::cg_expr(ObExprCGCtx &context, const ObRawExpr &raw, ObExpr &expression) const
{
  UNUSED(context);
  UNUSED(raw);
  expression.eval_func_ = eval;
  return common::OB_SUCCESS;
}

int FileSqlExpr::eval(const ObExpr &expression, ObEvalCtx &context, common::ObDatum &result)
{
  UNUSED(expression);
  UNUSED(context);
  UNUSED(result);
  return common::OB_NOT_SUPPORTED;
}
} // namespace sql
} // namespace oceanbase
