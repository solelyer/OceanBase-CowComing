/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

namespace oceanbase
{
namespace sql
{
class ObTableModifyOp;
class ObExecContext;
class ObPhysicalPlan;
struct ObDMLBaseCtDef;
int enqueue_ai_column_tasks(ObTableModifyOp &operation, const ObDMLBaseCtDef &definition);
int check_ai_column_reads(ObExecContext &context, const ObPhysicalPlan &plan);
} // namespace sql
} // namespace oceanbase
