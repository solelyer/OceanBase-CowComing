/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "ob_file_scan_utils.h"
#include "ob_parquet_file_reader.h"
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <glob.h>
#include <unistd.h>
#include "common/json_type/ob_json_tree.h"
#include "lib/string/ob_sql_string.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_schema_utils.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace
{
const int64_t FILE_SQL_MAX_FIELD = 16L * 1024 * 1024;
const int64_t FILE_SQL_MAX_RECORD = 64L * 1024 * 1024;
const int64_t FILE_SQL_MAX_COLUMNS = 1024;

int file_sql_json_int(ObIAllocator &allocator, ObJsonObject &object, const char *key, int64_t value)
{
  int ret = OB_SUCCESS;
  ObJsonInt *node = OB_NEWx(ObJsonInt, &allocator, value);
  if (OB_ISNULL(node)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ret = object.add(key, node);
  }
  return ret;
}

int file_sql_json_string(ObIAllocator &allocator, ObJsonObject &object,
                          const char *key, const ObString &value)
{
  int ret = OB_SUCCESS;
  ObJsonString *node = OB_NEWx(ObJsonString, &allocator, value);
  if (OB_ISNULL(node)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ret = object.add(key, node);
  }
  return ret;
}

bool file_sql_valid_utf8(const ObString &text)
{
  bool valid = true;
  const unsigned char *data = reinterpret_cast<const unsigned char *>(text.ptr());
  for (int64_t i = 0; valid && i < text.length();) {
    uint32_t code = data[i++];
    int count = 0;
    uint32_t minimum = 0;
    if (code < 0x80) {
    } else if (code >= 0xc2 && code <= 0xdf) {
      count = 1; minimum = 0x80; code &= 0x1f;
    } else if (code >= 0xe0 && code <= 0xef) {
      count = 2; minimum = 0x800; code &= 0xf;
    } else if (code >= 0xf0 && code <= 0xf4) {
      count = 3; minimum = 0x10000; code &= 7;
    } else {
      valid = false;
    }
    if (i + count > text.length()) {
      valid = false;
    }
    for (int j = 0; valid && j < count; ++j) {
      const unsigned char next = data[i++];
      if ((next & 0xc0) != 0x80) {
        valid = false;
      } else {
        code = (code << 6) | (next & 0x3f);
      }
    }
    valid = valid && (count == 0 || code >= minimum)
            && code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff);
  }
  return valid;
}

int file_sql_digits(const ObString &text, int offset, int length)
{
  int value = 0;
  for (int i = offset; i < offset + length; ++i) {
    if (i >= text.length() || text[i] < '0' || text[i] > '9') {
      value = -1;
      break;
    }
    value = value * 10 + text[i] - '0';
  }
  return value;
}

bool file_sql_datetime(const ObString &text, int64_t &days, int64_t &micros)
{
  bool valid = text.length() >= 10 && text[4] == '-' && text[7] == '-';
  int year = file_sql_digits(text, 0, 4);
  int month = file_sql_digits(text, 5, 2);
  int day = file_sql_digits(text, 8, 2);
  const int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  valid = valid && year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1;
  if (valid) {
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    valid = day <= lengths[month - 1] + (month == 2 && leap ? 1 : 0);
    // Gregorian civil date to days since 1970-01-01, independent of session TZ.
    const int adjusted_year = year - (month <= 2);
    const int era = adjusted_year / 400;
    const int yoe = adjusted_year - era * 400;
    const int shifted_month = month + (month > 2 ? -3 : 9);
    days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100
           + (153 * shifted_month + 2) / 5 + day - 1 - 719468;
    micros = days * 86400L * 1000000L;
  }
  if (valid && text.length() != 10) {
    valid = text.length() >= 19 && (text[10] == ' ' || text[10] == 'T')
            && text[13] == ':' && text[16] == ':';
    const int hour = file_sql_digits(text, 11, 2);
    const int minute = file_sql_digits(text, 14, 2);
    const int second = file_sql_digits(text, 17, 2);
    valid = valid && hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59
            && second >= 0 && second <= 59;
    int64_t fraction = 0;
    if (valid && text.length() > 19) {
      valid = text[19] == '.' && text.length() >= 21 && text.length() <= 26;
      for (int i = 20; valid && i < 26; ++i) {
        fraction *= 10;
        if (i < text.length()) {
          valid = text[i] >= '0' && text[i] <= '9';
          fraction += text[i] - '0';
        }
      }
    }
    micros += (hour * 3600L + minute * 60L + second) * 1000000L + fraction;
  }
  return valid;
}

ObObjType file_sql_widen(ObObjType left, ObObjType right)
{
  ObObjType result = left;
  if (left == ObNullType) {
    result = right;
  } else if (right == ObNullType || left == right) {
  } else if ((left == ObIntType && right == ObDoubleType)
             || (left == ObDoubleType && right == ObIntType)) {
    result = ObDoubleType;
  } else if ((left == ObDateType && right == ObDateTimeType)
             || (left == ObDateTimeType && right == ObDateType)) {
    result = ObDateTimeType;
  } else {
    result = ObVarcharType;
  }
  return result;
}
} // namespace

void FileSqlSnapshot::assign(const struct stat &value)
{
  device_ = value.st_dev;
  inode_ = value.st_ino;
  size_ = value.st_size;
  mtime_ = static_cast<int64_t>(value.st_mtim.tv_sec) * 1000000000L + value.st_mtim.tv_nsec;
  ctime_ = static_cast<int64_t>(value.st_ctim.tv_sec) * 1000000000L + value.st_ctim.tv_nsec;
}

bool FileSqlSnapshot::matches(const struct stat &value) const
{
  FileSqlSnapshot other;
  other.assign(value);
  return device_ == other.device_ && inode_ == other.inode_ && size_ == other.size_
         && mtime_ == other.mtime_ && ctime_ == other.ctime_;
}

int FileSqlConfig::set_format(const ObString &name)
{
  int ret = OB_SUCCESS;
  if (name.case_compare("read_csv") == 0) {
    format_ = FileSqlFormat::CSV;
  } else if (name.case_compare("read_json") == 0 || name.case_compare("read_ndjson") == 0) {
    format_ = FileSqlFormat::JSONL;
  } else if (name.case_compare("read_parquet") == 0) {
    format_ = FileSqlFormat::PARQUET;
  } else if (name.case_compare("glob") == 0) {
    format_ = FileSqlFormat::GLOB;
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

int FileSqlConfig::set_option(const ObString &name, const ObObj &value)
{
  int ret = OB_SUCCESS;
  if (name.case_compare("sample_size") == 0) {
    if (!value.is_integer_type() || value.get_int() != -1) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "File SQL requires sample_size = -1");
    }
  } else if (name.case_compare("header") == 0 && format_ == FileSqlFormat::CSV) {
    if (!value.is_integer_type() || (value.get_int() != 0 && value.get_int() != 1)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      header_ = value.get_int() != 0;
    }
  } else if (name.case_compare("nullstr") == 0 && format_ == FileSqlFormat::CSV) {
    if (!value.is_string_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ret = ob_write_string(allocator_, value.get_string(), nullstr_, true);
    }
  } else if (format_ == FileSqlFormat::CSV
             && (name.case_compare("delim") == 0 || name.case_compare("delimiter") == 0
                 || name.case_compare("quote") == 0 || name.case_compare("escape") == 0)) {
    if (!value.is_string_type() || value.get_string().length() != 1
        || value.get_string()[0] == '\n' || value.get_string()[0] == '\r'
        || value.get_string()[0] == '\0') {
      ret = OB_INVALID_ARGUMENT;
    } else if (name.case_compare("quote") == 0) {
      quote_ = value.get_string()[0];
    } else if (name.case_compare("escape") == 0) {
      escape_ = value.get_string()[0];
    } else {
      delimiter_ = value.get_string()[0];
    }
  } else if (name.case_compare("format") == 0 && format_ == FileSqlFormat::JSONL) {
    if (!value.is_string_type() || value.get_string().case_compare("newline_delimited") != 0) {
      ret = OB_NOT_SUPPORTED;
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unknown File SQL option");
  }
  return ret;
}

int FileSqlConfig::serialize(ObString &json) const
{
  int ret = OB_SUCCESS;
  ObJsonObject root(&allocator_);
  ObJsonArray *columns = OB_NEWx(ObJsonArray, &allocator_, &allocator_);
  if (OB_ISNULL(columns)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(file_sql_json_string(allocator_, root, "path", path_))) {
  } else if (OB_FAIL(file_sql_json_string(allocator_, root, "nullstr", nullstr_))) {
  } else if (OB_FAIL(file_sql_json_int(allocator_, root, "format", static_cast<int>(format_)))) {
  } else if (OB_FAIL(file_sql_json_int(allocator_, root, "header", header_))) {
  } else if (OB_FAIL(file_sql_json_int(allocator_, root, "delimiter", delimiter_))) {
  } else if (OB_FAIL(file_sql_json_int(allocator_, root, "quote", quote_))) {
  } else if (OB_FAIL(file_sql_json_int(allocator_, root, "escape", escape_))) {
  } else if (OB_FAIL(file_sql_json_int(allocator_, root, "device", snapshot_.device_))) {
  } else if (OB_FAIL(file_sql_json_int(allocator_, root, "inode", snapshot_.inode_))) {
  } else if (OB_FAIL(file_sql_json_int(allocator_, root, "size", snapshot_.size_))) {
  } else if (OB_FAIL(file_sql_json_int(allocator_, root, "mtime", snapshot_.mtime_))) {
  } else if (OB_FAIL(file_sql_json_int(allocator_, root, "ctime", snapshot_.ctime_))) {
  } else if (OB_FAIL(file_sql_json_int(allocator_, root, "rows", rows_))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
    const FileSqlColumn &column = columns_.at(i);
    ObJsonObject *node = OB_NEWx(ObJsonObject, &allocator_, &allocator_);
    if (OB_ISNULL(node)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(file_sql_json_string(allocator_, *node, "name", column.name_))) {
    } else if (OB_FAIL(file_sql_json_int(allocator_, *node, "type", column.type_))) {
    } else if (OB_FAIL(file_sql_json_int(allocator_, *node, "nullable", column.nullable_))) {
    } else if (OB_FAIL(file_sql_json_int(allocator_, *node, "length", column.length_))) {
    } else if (OB_FAIL(file_sql_json_int(allocator_, *node, "boolean", column.boolean_))) {
    } else if (OB_FAIL(columns->append(node))) {
    }
  }
  ObJsonBuffer buffer(&allocator_);
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(root.add("columns", columns))) {
  } else if (OB_FAIL(root.print(buffer, false))) {
  } else if (OB_FAIL(buffer.get_result_string(json))) {
  }
  return ret;
}

int FileSqlConfig::deserialize(const ObString &json)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator_, json, ObJsonInType::JSON_TREE,
                                              ObJsonInType::JSON_TREE, base))) {
  } else if (base->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *root = static_cast<ObJsonObject *>(base);
#define FILE_SQL_GET_INT(key, dest) \
    if (OB_SUCC(ret)) { \
      ObJsonNode *node = root->get_value(key); \
      int64_t integer = 0; \
      if (OB_ISNULL(node) || (node->json_type() != ObJsonNodeType::J_INT \
          && node->json_type() != ObJsonNodeType::J_UINT)) { ret = OB_INVALID_DATA; } \
      else if (OB_FAIL(node->to_int(integer, true))) {} \
      else { dest = integer; } \
    }
    int64_t format = 0;
    FILE_SQL_GET_INT("format", format);
    FILE_SQL_GET_INT("header", header_);
    FILE_SQL_GET_INT("delimiter", delimiter_);
    FILE_SQL_GET_INT("quote", quote_);
    FILE_SQL_GET_INT("escape", escape_);
    FILE_SQL_GET_INT("device", snapshot_.device_);
    FILE_SQL_GET_INT("inode", snapshot_.inode_);
    FILE_SQL_GET_INT("size", snapshot_.size_);
    FILE_SQL_GET_INT("mtime", snapshot_.mtime_);
    FILE_SQL_GET_INT("ctime", snapshot_.ctime_);
    FILE_SQL_GET_INT("rows", rows_);
    ObJsonNode *path = root->get_value("path");
    ObJsonNode *nullstr = root->get_value("nullstr");
    ObJsonNode *columns = root->get_value("columns");
    if (OB_FAIL(ret)) {
    } else if (format < 0 || format > static_cast<int>(FileSqlFormat::GLOB)
               || OB_ISNULL(path) || path->json_type() != ObJsonNodeType::J_STRING
               || OB_ISNULL(nullstr) || nullstr->json_type() != ObJsonNodeType::J_STRING
               || OB_ISNULL(columns) || columns->json_type() != ObJsonNodeType::J_ARRAY
               || columns->element_count() > FILE_SQL_MAX_COLUMNS) {
      ret = OB_INVALID_DATA;
    } else {
      format_ = static_cast<FileSqlFormat>(format);
      path_ = static_cast<ObJsonString *>(path)->get_str();
      nullstr_ = static_cast<ObJsonString *>(nullstr)->get_str();
      columns_.reuse();
      ObJsonObject *config_root = root;
      for (uint64_t i = 0; OB_SUCC(ret) && i < columns->element_count(); ++i) {
        ObJsonNode *node = static_cast<ObJsonArray *>(columns)->get_value(i);
        if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_DATA;
        } else {
          root = static_cast<ObJsonObject *>(node);
          FileSqlColumn column;
          ObJsonNode *name = root->get_value("name");
          int64_t type = 0;
          FILE_SQL_GET_INT("type", type);
          FILE_SQL_GET_INT("nullable", column.nullable_);
          FILE_SQL_GET_INT("length", column.length_);
          FILE_SQL_GET_INT("boolean", column.boolean_);
          if (OB_FAIL(ret)) {
          } else if (OB_ISNULL(name) || name->json_type() != ObJsonNodeType::J_STRING
                     || type < ObNullType || type >= ObMaxType) {
            ret = OB_INVALID_DATA;
          } else {
            column.type_ = static_cast<ObObjType>(type);
            column.name_ = static_cast<ObJsonString *>(name)->get_str();
            ret = columns_.push_back(column);
          }
        }
      }
      root = config_root;
      inferred_ = OB_SUCC(ret);
    }
#undef FILE_SQL_GET_INT
  }
  return ret;
}

FileSqlReader::FileSqlReader(ObIAllocator &allocator)
    : allocator_(allocator), row_allocator_("FileSqlRow"), owned_config_(allocator),
      config_(nullptr), file_(nullptr), line_(1), row_(0), glob_pos_(0), parquet_(nullptr)
{}

FileSqlReader::~FileSqlReader()
{
  close();
}

int FileSqlReader::check_enabled(ObSQLSessionInfo &session)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "File SQL is disabled; set enable_file_sql = ON");
  }
  return ret;
}

int FileSqlReader::secure_path(ObIAllocator &allocator, const ObString &path,
                                ObString &canonical, bool for_output)
{
  int ret = OB_SUCCESS;
  ObString secure;
  char root[PATH_MAX + 1];
  char resolved[PATH_MAX + 1];
  ObString terminated;
  if (path.empty() || path.length() >= PATH_MAX || nullptr != MEMCHR(path.ptr(), '\0', path.length())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(share::schema::ObSchemaUtils::get_runtime_varchar_variable(
                       *GCTX.schema_service_, share::SYS_VAR_SECURE_FILE_PRIV, allocator, secure))) {
  } else if (secure.empty() || secure.case_compare("NULL") == 0 || secure.length() >= PATH_MAX) {
    ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  } else if (OB_FAIL(ob_write_string(allocator, secure, terminated, true))) {
  } else if (nullptr == ::realpath(terminated.ptr(), root)) {
    ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  } else if (OB_FAIL(ob_write_string(allocator, path, terminated, true))) {
  } else if (!for_output && nullptr == ::realpath(terminated.ptr(), resolved)) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    if (for_output) {
      char *slash = strrchr(terminated.ptr(), '/');
      const char *leaf = slash == nullptr ? terminated.ptr() : slash + 1;
      ObString basename(leaf);
      if (basename.empty() || basename == "." || basename == "..") {
        ret = OB_INVALID_ARGUMENT;
      } else {
        if (slash != nullptr) {
          *slash = '\0';
        }
        const char *parent = slash == nullptr ? "." : (slash == terminated.ptr() ? "/" : terminated.ptr());
        if (nullptr == ::realpath(parent, resolved)) {
          ret = OB_FILE_NOT_EXIST;
        } else {
          const int64_t length = strlen(resolved);
          if (length + 1 + basename.length() >= PATH_MAX) {
            ret = OB_SIZE_OVERFLOW;
          } else {
            if (length != 1 || resolved[0] != '/') {
              strcat(resolved, "/");
            }
            strncat(resolved, basename.ptr(), basename.length());
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      const int64_t length = strlen(root);
      if (strncmp(root, resolved, length) != 0
          || (length > 1 && resolved[length] != '/' && resolved[length] != '\0')) {
        ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
      } else {
        ret = ob_write_string(allocator, ObString(resolved), canonical, true);
      }
    }
  }
  if (ret == OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS) {
    LOG_USER_ERROR(OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS);
  }
  return ret;
}

int FileSqlReader::secure_open(ObIAllocator &allocator, const ObString &path,
                                int &fd, ObString &canonical, struct stat &status)
{
  int ret = OB_SUCCESS;
  fd = -1;
  if (OB_FAIL(secure_path(allocator, path, canonical))) {
  } else {
    // Walk the canonical absolute path by descriptor: a concurrent symlink
    // substitution in any component cannot redirect this open outside the root.
    int directory = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
      ret = OB_IO_ERROR;
    }
    int64_t start = 1;
    for (int64_t i = 1; OB_SUCC(ret) && i <= canonical.length(); ++i) {
      if (i == canonical.length() || canonical[i] == '/') {
        ObString component;
        if (OB_FAIL(ob_write_string(allocator, ObString(i - start, canonical.ptr() + start), component, true))) {
        } else {
          const bool last = i == canonical.length();
          const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (last ? O_NONBLOCK : O_DIRECTORY);
          const int next = ::openat(directory, component.ptr(), flags);
          ::close(directory);
          directory = next;
          if (directory < 0) {
            ret = errno == ELOOP ? OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS : OB_IO_ERROR;
          }
        }
        start = i + 1;
      }
    }
    if (OB_SUCC(ret)) {
      if (::fstat(directory, &status) != 0 || !S_ISREG(status.st_mode)) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        fd = directory;
        directory = -1;
      }
    }
    if (directory >= 0) {
      ::close(directory);
    }
  }
  return ret;
}

int FileSqlReader::bad_input(const char *reason) const
{
  ObSqlString message;
  int ret = message.append_fmt("File SQL: %s at file '%.*s', row %ld, line %ld",
      reason, config_->path_.length(), config_->path_.ptr(), row_ + 1, line_);
  if (OB_SUCC(ret)) {
    ret = OB_INVALID_DATA;
    FORWARD_USER_ERROR(ret, message.ptr());
  }
  return ret;
}

int FileSqlReader::open(const ObString &json, ObSQLSessionInfo &session)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(owned_config_.deserialize(json))) {
  } else {
    ret = open(owned_config_, session);
  }
  return ret;
}

int FileSqlReader::open(FileSqlConfig &config, ObSQLSessionInfo &session)
{
  int ret = OB_SUCCESS;
  close();
  config_ = &config;
  if (OB_FAIL(check_enabled(session))) {
  } else if (config.format_ == FileSqlFormat::GLOB) {
    ret = discover_glob();
  } else {
    int fd = -1;
    ObString canonical;
    struct stat status;
    if (OB_FAIL(secure_open(allocator_, config.path_, fd, canonical, status))) {
    } else if (config.inferred_ && !config.snapshot_.matches(status)) {
      ::close(fd);
      ret = bad_input("file changed after schema inference");
    } else if (nullptr == (file_ = ::fdopen(fd, "rb"))) {
      ::close(fd);
      ret = OB_IO_ERROR;
    } else {
      config.path_ = canonical;
      config.snapshot_.assign(status);
      if (config.format_ == FileSqlFormat::PARQUET) {
        parquet_ = OB_NEWx(ParquetFileReader, &allocator_, allocator_);
        if (OB_ISNULL(parquet_)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          ret = parquet_->open(::fileno(file_), config);
        }
      }
      if (OB_SUCC(ret)) { ret = rewind(); }
    }
  }
  return ret;
}

void FileSqlReader::close()
{
  if (parquet_ != nullptr) {
    OB_DELETEx(ParquetFileReader, &allocator_, parquet_);
    parquet_ = nullptr;
  }
  if (file_ != nullptr) {
    ::fclose(file_);
    file_ = nullptr;
  }
  glob_files_.reuse();
  glob_pos_ = 0;
  row_ = 0;
  line_ = 1;
}

int FileSqlReader::check_unchanged() const
{
  int ret = OB_SUCCESS;
  struct stat descriptor;
  struct stat path;
  if (file_ != nullptr && (::fstat(::fileno(file_), &descriptor) != 0
      || ::stat(config_->path_.ptr(), &path) != 0
      || !config_->snapshot_.matches(descriptor) || !config_->snapshot_.matches(path))) {
    ret = bad_input("file was replaced or modified during scan");
  }
  return ret;
}

int FileSqlReader::rewind()
{
  int ret = OB_SUCCESS;
  row_ = 0;
  line_ = 1;
  glob_pos_ = 0;
  if (OB_FAIL(check_unchanged())) {
  } else if (parquet_ != nullptr) {
    ret = parquet_->rewind();
  } else if (file_ != nullptr) {
    if (::fseeko(file_, 0, SEEK_SET) != 0) {
      ret = OB_IO_ERROR;
    } else {
      unsigned char prefix[3];
      const size_t count = ::fread(prefix, 1, 3, file_);
      if (count != 3 || prefix[0] != 0xef || prefix[1] != 0xbb || prefix[2] != 0xbf) {
        ::rewind(file_);
      }
      if (config_->format_ == FileSqlFormat::CSV && config_->header_ && config_->inferred_) {
        ObSEArray<FileSqlCell, 16> cells;
        ret = read_csv(cells);
        if (ret == OB_ITER_END) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int FileSqlReader::set_projection(const ObIArray<int64_t> &columns)
{
  int ret = OB_SUCCESS;
  if (parquet_ != nullptr) { ret = parquet_->set_projection(columns); }
  return ret;
}

int FileSqlReader::read_csv(ObIArray<FileSqlCell> &cells)
{
  int ret = OB_SUCCESS;
  cells.reuse();
  bool done = false;
  int64_t record_size = 0;
  while (OB_SUCC(ret) && !done) {
    ObStringBuffer buffer(&row_allocator_);
    int current = ::fgetc(file_);
    const bool quoted = current == static_cast<unsigned char>(config_->quote_);
    bool in_quotes = quoted;
    bool closed = false;
    bool have_data = quoted;
    if (quoted) {
      current = ::fgetc(file_);
    }
    if (current == EOF && cells.empty() && !quoted) {
      ret = ::ferror(file_) ? OB_IO_ERROR : OB_ITER_END;
    }
    while (OB_SUCC(ret) && !done) {
      if (current == EOF) {
        done = true;
        if (::ferror(file_)) {
          ret = OB_IO_ERROR;
        } else if (in_quotes) {
          ret = bad_input("unterminated quoted CSV field");
        }
      } else if (in_quotes && current == config_->quote_) {
        const int next = ::fgetc(file_);
        if (config_->escape_ == config_->quote_ && next == config_->quote_) {
          const char ch = config_->quote_;
          ret = buffer.append(&ch, 1);
          current = ::fgetc(file_);
        } else {
          in_quotes = false;
          closed = true;
          current = next;
        }
      } else if (in_quotes && config_->escape_ != config_->quote_ && current == config_->escape_) {
        const int next = ::fgetc(file_);
        if (next == EOF) {
          ret = bad_input("incomplete CSV escape");
        } else {
          const char ch = static_cast<char>(next);
          ret = buffer.append(&ch, 1);
          if (next == '\n') { ++line_; }
          current = ::fgetc(file_);
        }
      } else if (!in_quotes && current == config_->delimiter_) {
        break;
      } else if (!in_quotes && (current == '\n' || current == '\r')) {
        if (current == '\r') {
          const int next = ::fgetc(file_);
          if (next != '\n' && next != EOF) { ::ungetc(next, file_); }
        }
        ++line_;
        done = true;
      } else if (closed || (!in_quotes && current == config_->quote_)) {
        ret = bad_input("unexpected character outside quoted CSV field");
      } else {
        const char ch = static_cast<char>(current);
        have_data = true;
        if (current == '\n') { ++line_; }
        ret = buffer.append(&ch, 1);
        current = ::fgetc(file_);
      }
      if (buffer.length() > FILE_SQL_MAX_FIELD) {
        ret = bad_input("CSV field exceeds 16 MiB");
      }
    }
    if (OB_SUCC(ret)) {
      FileSqlCell cell;
      if (OB_FAIL(buffer.get_result_string(cell.text_))) {
      } else if (!file_sql_valid_utf8(cell.text_)) {
        ret = bad_input("invalid UTF-8");
      } else {
        cell.is_null_ = !quoted && (cell.text_.empty() || cell.text_ == config_->nullstr_);
        cell.type_ = ObNullType;
        if (OB_FAIL(cells.push_back(cell))) {
        }
        record_size += cell.text_.length();
      }
      if (cells.count() > FILE_SQL_MAX_COLUMNS || record_size > FILE_SQL_MAX_RECORD) {
        ret = bad_input("CSV record exceeds resource limit");
      }
    }
    UNUSED(have_data);
  }
  return ret;
}

int FileSqlReader::read_jsonl(ObIArray<FileSqlCell> &cells, ObIArray<ObString> &names)
{
  int ret = OB_SUCCESS;
  bool found = false;
  cells.reuse();
  names.reuse();
  while (OB_SUCC(ret) && !found) {
    ObStringBuffer buffer(&row_allocator_);
    int ch = ::fgetc(file_);
    if (ch == EOF) {
      ret = ::ferror(file_) ? OB_IO_ERROR : OB_ITER_END;
    }
    bool nonspace = false;
    while (OB_SUCC(ret) && ch != EOF && ch != '\n') {
      const char value = static_cast<char>(ch);
      nonspace |= ch != ' ' && ch != '\t' && ch != '\r';
      if (OB_FAIL(buffer.append(&value, 1))) {
      } else if (buffer.length() > FILE_SQL_MAX_RECORD) {
        ret = bad_input("JSONL record exceeds 64 MiB");
      }
      ch = ::fgetc(file_);
    }
    if (OB_SUCC(ret) && nonspace) {
      ObIJsonBase *base = nullptr;
      if (!file_sql_valid_utf8(buffer.string())) {
        ret = bad_input("invalid UTF-8");
      } else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&row_allocator_, buffer.string(),
                           ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, base))) {
        ret = bad_input("invalid JSONL object");
      } else if (base->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = bad_input("JSONL rows must be flat objects");
      } else if (base->element_count() > FILE_SQL_MAX_COLUMNS) {
        ret = bad_input("JSONL object exceeds 1024 columns");
      } else {
        ObJsonObject *object = static_cast<ObJsonObject *>(base);
        for (uint64_t i = 0; OB_SUCC(ret) && i < object->element_count(); ++i) {
          ObString name;
          ObJsonNode *value = object->get_value(i);
          FileSqlCell cell;
          if (OB_FAIL(object->get_key(i, name))) {
          } else if (name.empty() || name.length() > 256 || nullptr != MEMCHR(name.ptr(), '\0', name.length())) {
            ret = bad_input("invalid JSONL column name");
          } else if (value->json_type() == ObJsonNodeType::J_ARRAY || value->json_type() == ObJsonNodeType::J_OBJECT) {
            ret = bad_input("nested JSONL values are not supported");
          } else if (value->json_type() == ObJsonNodeType::J_NULL) {
            cell.is_null_ = true;
          } else if (value->json_type() == ObJsonNodeType::J_STRING) {
            cell.text_ = static_cast<ObJsonString *>(value)->get_str();
            cell.type_ = ObVarcharType;
          } else {
            ObStringBuffer scalar(&row_allocator_);
            if (OB_FAIL(value->print(scalar, false))) {
            } else if (OB_FAIL(scalar.get_result_string(cell.text_))) {
            } else if (value->json_type() == ObJsonNodeType::J_BOOLEAN) {
              cell.type_ = ObTinyIntType;
            }
          }
          if (OB_FAIL(ret)) {
          } else if (cell.text_.length() > FILE_SQL_MAX_FIELD) {
            ret = bad_input("JSONL field exceeds 16 MiB");
          } else if (OB_FAIL(names.push_back(name))) {
          } else if (OB_FAIL(cells.push_back(cell))) {
          }
        }
        found = true;
      }
    }
    ++line_;
  }
  return ret;
}

int FileSqlReader::next_raw(ObIArray<FileSqlCell> &values, ObIArray<ObString> &names)
{
  int ret = OB_SUCCESS;
  row_allocator_.reuse();
  if (OB_FAIL(check_unchanged())) {
  } else if (config_->format_ == FileSqlFormat::CSV) {
    ret = read_csv(values);
  } else if (config_->format_ == FileSqlFormat::JSONL) {
    ret = read_jsonl(values, names);
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret)) {
    ++row_;
  }
  return ret;
}

int FileSqlReader::detect_type(const ObString &text, ObObjType &type, bool &boolean)
{
  int ret = OB_SUCCESS;
  type = ObVarcharType;
  boolean = false;
  int64_t days = 0;
  int64_t micros = 0;
  if (text.case_compare("true") == 0 || text.case_compare("false") == 0) {
    type = ObTinyIntType;
    boolean = true;
  } else if ((text.length() == 10 || text.length() >= 19) && file_sql_datetime(text, days, micros)) {
    type = text.length() == 10 ? ObDateType : ObDateTimeType;
  } else if (!text.empty()) {
    int64_t offset = text[0] == '-' || text[0] == '+' ? 1 : 0;
    int64_t digits = 0;
    for (; offset < text.length() && text[offset] >= '0' && text[offset] <= '9'; ++offset) { ++digits; }
    const bool integer = digits > 0 && offset == text.length();
    if (offset < text.length() && text[offset] == '.') {
      ++offset;
      for (; offset < text.length() && text[offset] >= '0' && text[offset] <= '9'; ++offset) { ++digits; }
    }
    if (digits > 0 && offset < text.length() && (text[offset] == 'e' || text[offset] == 'E')) {
      ++offset;
      if (offset < text.length() && (text[offset] == '+' || text[offset] == '-')) { ++offset; }
      const int64_t exponent = offset;
      for (; offset < text.length() && text[offset] >= '0' && text[offset] <= '9'; ++offset) {}
      if (exponent == offset) { digits = 0; }
    }
    if (digits > 0 && offset == text.length()) {
      char buffer[256];
      if (text.length() < static_cast<int64_t>(sizeof(buffer))) {
        MEMCPY(buffer, text.ptr(), text.length());
        buffer[text.length()] = '\0';
        char *end = nullptr;
        errno = 0;
        if (integer) {
          ::strtoll(buffer, &end, 10);
          if (errno != ERANGE && end == buffer + text.length()) { type = ObIntType; }
        } else {
          const double value = ::strtod(buffer, &end);
          if (errno != ERANGE && std::isfinite(value) && end == buffer + text.length()) { type = ObDoubleType; }
        }
      }
    }
  }
  return ret;
}

int FileSqlReader::convert(const FileSqlCell &cell, const FileSqlColumn &column, ObObj &value)
{
  int ret = OB_SUCCESS;
  if (cell.is_null_ || column.type_ == ObNullType) {
    value.set_null();
  } else if (column.type_ == ObVarcharType) {
    value.set_varchar(cell.text_);
    value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  } else if (column.type_ == ObTinyIntType) {
    value.set_tinyint(cell.text_.case_compare("true") == 0 ? 1 : 0);
  } else if (column.type_ == ObDateType || column.type_ == ObDateTimeType) {
    int64_t days = 0;
    int64_t micros = 0;
    if (!file_sql_datetime(cell.text_, days, micros)) {
      ret = OB_INVALID_DATA;
    } else if (column.type_ == ObDateType) {
      value.set_date(static_cast<int32_t>(days));
    } else {
      value.set_datetime(micros);
    }
  } else {
    char buffer[256];
    if (cell.text_.length() >= static_cast<int64_t>(sizeof(buffer))) {
      ret = OB_INVALID_DATA;
    } else {
      MEMCPY(buffer, cell.text_.ptr(), cell.text_.length());
      buffer[cell.text_.length()] = '\0';
      char *end = nullptr;
      errno = 0;
      if (column.type_ == ObIntType) {
        const int64_t number = ::strtoll(buffer, &end, 10);
        value.set_int(number);
      } else {
        const double number = ::strtod(buffer, &end);
        value.set_double(number);
        if (!std::isfinite(number)) { ret = OB_INVALID_DATA; }
      }
      if (errno == ERANGE || end != buffer + cell.text_.length()) { ret = OB_INVALID_DATA; }
    }
  }
  return ret;
}

int FileSqlReader::infer(FileSqlConfig &config, ObSQLSessionInfo &session)
{
  int ret = OB_SUCCESS;
  FileSqlReader reader(config.allocator_);
  ObSEArray<FileSqlCell, 16> cells;
  ObSEArray<ObString, 16> names;
  int64_t rows = 0;
  if (OB_FAIL(reader.open(config, session))) {
  } else if (config.format_ == FileSqlFormat::GLOB) {
    FileSqlColumn column;
    column.name_ = "file";
    column.type_ = ObVarcharType;
    column.length_ = PATH_MAX;
    ret = config.columns_.push_back(column);
  } else if (config.format_ == FileSqlFormat::PARQUET) {
    rows = config.rows_;
  } else {
    bool first = true;
    while (OB_SUCC(ret)) {
      ret = reader.next_raw(cells, names);
      if (ret == OB_ITER_END) {
        ret = OB_SUCCESS;
        break;
      } else if (OB_FAIL(ret)) {
        break;
      }
      if (config.format_ == FileSqlFormat::CSV && first) {
        for (int64_t i = 0; OB_SUCC(ret) && i < cells.count(); ++i) {
          FileSqlColumn column;
          ObString base;
          ObSqlString generated;
          if (config.header_) {
            base = cells.at(i).text_;
          } else if (OB_FAIL(generated.append_fmt("column%ld", i))) {
          } else {
            base = generated.string();
          }
          if (OB_SUCC(ret) && base.empty()) {
            ret = generated.append_fmt("column%ld", i);
            base = generated.string();
          }
          if (OB_SUCC(ret) && (base.length() > 256 || nullptr != MEMCHR(base.ptr(), '\0', base.length()))) {
            ret = reader.bad_input("invalid CSV column name");
          }
          ObString candidate = base;
          ObSqlString unique;
          bool collision = true;
          for (int64_t suffix = 2; OB_SUCC(ret) && collision; ++suffix) {
            collision = false;
            for (int64_t j = 0; !collision && j < config.columns_.count(); ++j) {
              collision = config.columns_.at(j).name_.case_compare(candidate) == 0;
            }
            if (collision) {
              unique.reuse();
              if (OB_FAIL(unique.append_fmt("%.*s__%ld", base.length(), base.ptr(), suffix))) {
              } else {
                candidate = unique.string();
              }
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(ob_write_string(config.allocator_, candidate, column.name_, true))) {
          } else if (OB_FAIL(config.columns_.push_back(column))) {
          }
        }
        first = false;
        if (config.header_) { continue; }
      }
      if (config.format_ == FileSqlFormat::CSV && cells.count() != config.columns_.count()) {
        ret = reader.bad_input("CSV column count does not match header");
      } else if (config.format_ == FileSqlFormat::JSONL) {
        for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
          bool found = false;
          for (int64_t j = 0; !found && j < config.columns_.count(); ++j) {
            found = names.at(i).case_compare(config.columns_.at(j).name_) == 0;
          }
          if (!found) {
            FileSqlColumn column;
            column.nullable_ = rows > 0;
            if (config.columns_.count() >= FILE_SQL_MAX_COLUMNS) {
              ret = reader.bad_input("JSONL schema exceeds 1024 columns");
            } else if (OB_FAIL(ob_write_string(config.allocator_, names.at(i), column.name_, true))) {
            } else if (OB_FAIL(config.columns_.push_back(column))) {
            }
          }
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < config.columns_.count(); ++i) {
        FileSqlColumn &column = config.columns_.at(i);
        int64_t index = i;
        if (config.format_ == FileSqlFormat::JSONL) {
          index = -1;
          for (int64_t j = 0; index < 0 && j < names.count(); ++j) {
            if (names.at(j).case_compare(column.name_) == 0) { index = j; }
          }
        }
        if (index < 0 || cells.at(index).is_null_) {
          column.nullable_ = true;
        } else {
          ObObjType type = cells.at(index).type_;
          bool boolean = type == ObTinyIntType;
          if (type == ObNullType && OB_FAIL(detect_type(cells.at(index).text_, type, boolean))) {
          } else {
            column.type_ = file_sql_widen(column.type_, type);
            column.boolean_ = column.type_ == ObTinyIntType;
            column.length_ = MAX(column.length_, cells.at(index).text_.length());
          }
        }
      }
      ++rows;
    }
    if (OB_SUCC(ret)) {
      ret = reader.check_unchanged();
    }
  }
  if (OB_SUCC(ret)) {
    config.rows_ = rows;
    config.inferred_ = true;
  }
  return ret;
}

int FileSqlReader::discover_glob()
{
  int ret = OB_SUCCESS;
  ObString canonical;
  if (OB_FAIL(secure_path(allocator_, config_->path_, canonical, true))) {
  } else {
    glob_t paths;
    MEMSET(&paths, 0, sizeof(paths));
    const int result = ::glob(canonical.ptr(), GLOB_NOSORT, nullptr, &paths);
    if (result != 0 && result != GLOB_NOMATCH) {
      ret = OB_IO_ERROR;
    }
    for (size_t i = 0; OB_SUCC(ret) && i < paths.gl_pathc; ++i) {
      int fd = -1;
      struct stat status;
      ObString file;
      struct stat candidate;
      if (::stat(paths.gl_pathv[i], &candidate) != 0) {
        ret = OB_IO_ERROR;
      } else if (!S_ISREG(candidate.st_mode)) {
      } else if (OB_FAIL(secure_open(allocator_, ObString(paths.gl_pathv[i]), fd, file, status))) {
      } else {
        ::close(fd);
        ret = glob_files_.push_back(file);
      }
    }
    ::globfree(&paths);
  }
  return ret;
}

int FileSqlReader::next_row(ObIArray<ObObj> &values)
{
  int ret = OB_SUCCESS;
  values.reuse();
  if (parquet_ != nullptr) {
    if (OB_FAIL(check_unchanged())) {
    } else { ret = parquet_->next_row(values); }
  } else if (config_->format_ == FileSqlFormat::GLOB) {
    if (glob_pos_ >= glob_files_.count()) {
      ret = OB_ITER_END;
    } else {
      ObObj value;
      value.set_varchar(glob_files_.at(glob_pos_++));
      value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      ret = values.push_back(value);
    }
  } else {
    ObSEArray<FileSqlCell, 16> cells;
    ObSEArray<ObString, 16> names;
    if (OB_FAIL(next_raw(cells, names))) {
    } else if (config_->format_ == FileSqlFormat::CSV && cells.count() != config_->columns_.count()) {
      ret = bad_input("CSV column count changed");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < config_->columns_.count(); ++i) {
        const FileSqlColumn &column = config_->columns_.at(i);
        int64_t index = i;
        if (config_->format_ == FileSqlFormat::JSONL) {
          index = -1;
          for (int64_t j = 0; index < 0 && j < names.count(); ++j) {
            if (names.at(j).case_compare(column.name_) == 0) { index = j; }
          }
        }
        FileSqlCell missing;
        missing.is_null_ = true;
        ObObj value;
        if (OB_FAIL(convert(index < 0 ? missing : cells.at(index), column, value))) {
        } else if (OB_FAIL(values.push_back(value))) {
        }
      }
    }
  }
  return ret;
}

int FileCsvWriter::open(const ObString &metadata, ObSQLSessionInfo &session)
{
  int ret = OB_SUCCESS;
  ObString canonical;
  if (OB_FAIL(FileSqlReader::check_enabled(session))) {
  } else if (OB_FAIL(config_.deserialize(metadata))) {
  } else if (config_.format_ != FileSqlFormat::CSV || config_.delimiter_ == config_.quote_) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(FileSqlReader::secure_path(allocator_, config_.path_, canonical, true))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < config_.nullstr_.length(); ++i) {
      const char ch = config_.nullstr_[i];
      if (ch == config_.delimiter_ || ch == config_.quote_ || ch == '\n' || ch == '\r' || ch == '\0') {
        ret = OB_INVALID_ARGUMENT;
      }
    }
    directory_ = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_ < 0) { ret = OB_IO_ERROR; }
    int64_t start = 1;
    for (int64_t i = 1; OB_SUCC(ret) && i <= canonical.length(); ++i) {
      if (i == canonical.length()) {
        ret = ob_write_string(allocator_, ObString(i - start, canonical.ptr() + start), basename_, true);
      } else if (canonical[i] == '/') {
        ObString component;
        if (OB_FAIL(ob_write_string(allocator_, ObString(i - start, canonical.ptr() + start), component, true))) {
        } else {
          const int next = ::openat(directory_, component.ptr(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
          ::close(directory_);
          directory_ = next;
          if (directory_ < 0) { ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS; }
        }
        start = i + 1;
      }
    }
    struct stat status;
    if (OB_FAIL(ret)) {
    } else if (::fstatat(directory_, basename_.ptr(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
      ret = OB_FILE_ALREADY_EXIST;
    } else if (errno != ENOENT) {
      ret = OB_IO_ERROR;
    } else {
      static uint64_t sequence = 0;
      const uint64_t nonce = __sync_add_and_fetch(&sequence, 1);
      snprintf(temporary_, sizeof(temporary_), ".seekdb-copy-%ld-%lu.tmp", static_cast<long>(::getpid()), nonce);
      const int fd = ::openat(directory_, temporary_, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd < 0) {
        temporary_[0] = '\0';
        ret = OB_IO_ERROR;
      } else if (nullptr == (file_ = ::fdopen(fd, "wb"))) {
        ::close(fd);
        ret = OB_IO_ERROR;
      }
    }
    if (OB_SUCC(ret) && config_.header_) {
      for (int64_t i = 0; OB_SUCC(ret) && i < config_.columns_.count(); ++i) {
        if (i > 0) { ret = write_bytes(&config_.delimiter_, 1); }
        if (OB_SUCC(ret)) { ret = write_field(config_.columns_.at(i).name_, false); }
      }
      if (OB_SUCC(ret)) { ret = write_bytes("\n", 1); }
    }
  }
  if (OB_FAIL(ret)) { abort(); }
  return ret;
}

int FileCsvWriter::write_bytes(const char *bytes, int64_t length)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(file_) || length < 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (length > 0 && ::fwrite(bytes, 1, length, file_) != static_cast<size_t>(length)) {
    ret = OB_IO_ERROR;
  }
  return ret;
}

int FileCsvWriter::write_field(const ObString &text, bool is_null)
{
  int ret = OB_SUCCESS;
  bool quoted = text.empty() || text == config_.nullstr_;
  if (text.length() > FILE_SQL_MAX_FIELD) {
    ret = OB_SIZE_OVERFLOW;
  } else if (is_null) {
    ret = write_bytes(config_.nullstr_.ptr(), config_.nullstr_.length());
  } else {
    for (int64_t i = 0; !quoted && i < text.length(); ++i) {
      quoted = text[i] == config_.delimiter_ || text[i] == config_.quote_
               || text[i] == '\r' || text[i] == '\n';
    }
    if (!quoted) {
      ret = write_bytes(text.ptr(), text.length());
    } else if (OB_FAIL(write_bytes(&config_.quote_, 1))) {
    } else {
      int64_t start = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < text.length(); ++i) {
        if (text[i] == config_.quote_ || text[i] == config_.escape_) {
          if (OB_FAIL(write_bytes(text.ptr() + start, i - start))) {
          } else if (OB_FAIL(write_bytes(&config_.escape_, 1))) {
          } else if (OB_FAIL(write_bytes(text.ptr() + i, 1))) {
          }
          start = i + 1;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(write_bytes(text.ptr() + start, text.length() - start))) {
      } else {
        ret = write_bytes(&config_.quote_, 1);
      }
    }
  }
  return ret;
}

int FileCsvWriter::write_row(const ObIArray<ObObj> &values)
{
  int ret = OB_SUCCESS;
  int64_t record_size = 0;
  char buffer[4096];
  if (values.count() != config_.columns_.count()) { ret = OB_ERR_UNEXPECTED; }
  for (int64_t i = 0; OB_SUCC(ret) && i < values.count(); ++i) {
    const ObObj &value = values.at(i);
    ObString text;
    int64_t position = 0;
    if (value.is_null()) {
    } else if (value.is_string_type()) {
      text = value.get_string();
    } else if (OB_FAIL(value.print_plain_str_literal(buffer, sizeof(buffer), position))) {
    } else {
      text.assign_ptr(buffer, position);
    }
    record_size += text.length();
    if (OB_FAIL(ret)) {
    } else if (record_size > FILE_SQL_MAX_RECORD) {
      ret = OB_SIZE_OVERFLOW;
    } else if (i > 0 && OB_FAIL(write_bytes(&config_.delimiter_, 1))) {
    } else {
      ret = write_field(text, value.is_null());
    }
  }
  if (OB_SUCC(ret)) { ret = write_bytes("\n", 1); }
  return ret;
}

int FileCsvWriter::finish()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(file_) || directory_ < 0 || published_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (::fflush(file_) != 0 || ::fsync(::fileno(file_)) != 0) {
    ret = OB_IO_ERROR;
  } else if (::linkat(directory_, temporary_, directory_, basename_.ptr(), 0) != 0) {
    ret = errno == EEXIST ? OB_FILE_ALREADY_EXIST : OB_IO_ERROR;
  } else {
    published_ = true;
  }
  abort();
  return ret;
}

void FileCsvWriter::abort()
{
  if (file_ != nullptr) { ::fclose(file_); file_ = nullptr; }
  if (directory_ >= 0) {
    if (temporary_[0] != '\0') { ::unlinkat(directory_, temporary_, 0); }
    ::close(directory_);
    directory_ = -1;
  }
  temporary_[0] = '\0';
}

} // namespace sql
} // namespace oceanbase
