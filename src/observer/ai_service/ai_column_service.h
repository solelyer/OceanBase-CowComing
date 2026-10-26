/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "share/ob_thread_pool.h"

namespace oceanbase
{
namespace observer
{
// Persistent jobs are committed with the source row. HTTP never runs in that transaction.
class AiColumnService : public share::ObThreadPool
{
public:
  static AiColumnService &instance();
  int start_service();
  void stop_service();
  void run1() override;
private:
  AiColumnService() : share::ObThreadPool(4), boot_id_(0) {}
  int64_t boot_id_;
};
} // namespace observer
} // namespace oceanbase
