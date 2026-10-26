/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstdio>
#include <sys/stat.h>
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string_buffer.h"
#include "common/object/ob_object.h"

namespace oceanbase
{
namespace sql
{
class ObSQLSessionInfo;
class ParquetFileReader;

enum class FileSqlFormat { CSV, JSONL, PARQUET, GLOB };

struct FileSqlColumn
{
  FileSqlColumn() : type_(common::ObNullType), nullable_(false), length_(0), boolean_(false) {}
  common::ObString name_;
  common::ObObjType type_;
  bool nullable_;
  int64_t length_;
  bool boolean_;
};

struct FileSqlSnapshot
{
  FileSqlSnapshot() : device_(0), inode_(0), size_(0), mtime_(0), ctime_(0) {}
  void assign(const struct stat &value);
  bool matches(const struct stat &value) const;
  int64_t device_;
  int64_t inode_;
  int64_t size_;
  int64_t mtime_;
  int64_t ctime_;
};

struct FileSqlConfig
{
  explicit FileSqlConfig(common::ObIAllocator &allocator)
      : allocator_(allocator), format_(FileSqlFormat::CSV), header_(true),
        delimiter_(','), quote_('"'), escape_('"'), rows_(0), inferred_(false) {}
  int serialize(common::ObString &json) const;
  int deserialize(const common::ObString &json);
  int set_option(const common::ObString &name, const common::ObObj &value);
  int set_format(const common::ObString &name);
  common::ObIAllocator &allocator_;
  FileSqlFormat format_;
  common::ObString path_;
  common::ObString nullstr_;
  bool header_;
  char delimiter_;
  char quote_;
  char escape_;
  int64_t rows_;
  bool inferred_;
  FileSqlSnapshot snapshot_;
  common::ObSEArray<FileSqlColumn, 16> columns_;
};

struct FileSqlCell
{
  FileSqlCell() : is_null_(false), type_(common::ObNullType) {}
  common::ObString text_;
  bool is_null_;
  common::ObObjType type_;
};

class FileSqlReader
{
public:
  explicit FileSqlReader(common::ObIAllocator &allocator);
  ~FileSqlReader();
  int open(FileSqlConfig &config, ObSQLSessionInfo &session);
  int open(const common::ObString &json, ObSQLSessionInfo &session);
  int rewind();
  int set_projection(const common::ObIArray<int64_t> &columns);
  int next_row(common::ObIArray<common::ObObj> &values);
  int next_raw(common::ObIArray<FileSqlCell> &values,
               common::ObIArray<common::ObString> &names);
  void close();
  int check_unchanged() const;
  FileSqlConfig &config() { return *config_; }
  static int infer(FileSqlConfig &config, ObSQLSessionInfo &session);
  static int check_enabled(ObSQLSessionInfo &session);
  static int secure_open(common::ObIAllocator &allocator, const common::ObString &path,
                          int &fd, common::ObString &canonical, struct stat &status);
  static int secure_path(common::ObIAllocator &allocator, const common::ObString &path,
                          common::ObString &canonical, bool for_output = false);
  static int detect_type(const common::ObString &text, common::ObObjType &type, bool &boolean);
  static int convert(const FileSqlCell &cell, const FileSqlColumn &column, common::ObObj &value);
private:
  int read_csv(common::ObIArray<FileSqlCell> &cells);
  int read_jsonl(common::ObIArray<FileSqlCell> &cells,
                  common::ObIArray<common::ObString> &names);
  int discover_glob();
  int bad_input(const char *reason) const;
  common::ObIAllocator &allocator_;
  common::ObArenaAllocator row_allocator_;
  FileSqlConfig owned_config_;
  FileSqlConfig *config_;
  FILE *file_;
  int64_t line_;
  int64_t row_;
  int64_t glob_pos_;
  common::ObSEArray<common::ObString, 16> glob_files_;
  ParquetFileReader *parquet_;
};

// COPY publishes a complete CSV by linking a private temporary file. Readers
// never observe a partial target and an existing file is never replaced.
class FileCsvWriter
{
public:
  explicit FileCsvWriter(common::ObIAllocator &allocator)
      : allocator_(allocator), row_allocator_("FileSqlCopy"), config_(allocator),
        directory_(-1), file_(nullptr), published_(false) { temporary_[0] = '\0'; }
  ~FileCsvWriter() { abort(); }
  int open(const common::ObString &metadata, ObSQLSessionInfo &session);
  int write_row(const common::ObIArray<common::ObObj> &values);
  int finish();
  void abort();
  common::ObArenaAllocator &row_allocator() { return row_allocator_; }
private:
  int write_field(const common::ObString &text, bool is_null);
  int write_bytes(const char *bytes, int64_t length);
  common::ObIAllocator &allocator_;
  common::ObArenaAllocator row_allocator_;
  FileSqlConfig config_;
  int directory_;
  FILE *file_;
  common::ObString basename_;
  char temporary_[96];
  bool published_;
};

} // namespace sql
} // namespace oceanbase
