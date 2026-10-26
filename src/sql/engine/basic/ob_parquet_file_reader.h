/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{
struct ParquetSchemaColumn;
struct ParquetRowGroup;
class ParquetColumnReader;

class ParquetFileReader
{
public:
  explicit ParquetFileReader(common::ObIAllocator &allocator);
  ~ParquetFileReader();
  int open(int fd, FileSqlConfig &config);
  int set_projection(const common::ObIArray<int64_t> &columns);
  int rewind();
  int next_row(common::ObIArray<common::ObObj> &values);
  int reserve_page_memory(int64_t bytes);
  void release_page_memory(int64_t bytes) { page_memory_ -= bytes; }
private:
  int read_footer(FileSqlConfig &config);
  int start_row_group();
  void close_readers();
  common::ObIAllocator &allocator_;
  common::ObArenaAllocator metadata_allocator_;
  int fd_;
  int64_t file_size_;
  int64_t footer_start_;
  int64_t group_;
  int64_t group_row_;
  int64_t page_memory_;
  bool projection_set_;
  common::ObSEArray<int64_t, 16> projection_;
  common::ObSEArray<ParquetSchemaColumn *, 16> schema_;
  common::ObSEArray<ParquetRowGroup *, 8> groups_;
  common::ObSEArray<ParquetColumnReader *, 16> readers_;
};
} // namespace sql
} // namespace oceanbase
