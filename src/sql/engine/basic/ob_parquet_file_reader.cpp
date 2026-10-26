/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "ob_parquet_file_reader.h"
#include <cmath>
#include <unistd.h>
#include <zlib.h>
#include "lib/compress/zstd_1_3_8/ob_zstd_wrapper.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace
{
const int64_t PARQUET_MAX_PAGE = 64L * 1024 * 1024;
const int64_t PARQUET_MAX_FOOTER = 16L * 1024 * 1024;
const int64_t PARQUET_MAX_VALUES = 4L * 1024 * 1024;

uint64_t parquet_little(const char *data, int bytes)
{
  uint64_t value = 0;
  for (int i = 0; i < bytes; ++i) {
    value |= static_cast<uint64_t>(static_cast<unsigned char>(data[i])) << (8 * i);
  }
  return value;
}

int parquet_pread(int fd, int64_t offset, char *buffer, int64_t size)
{
  int ret = OB_SUCCESS;
  int64_t done = 0;
  while (OB_SUCC(ret) && done < size) {
    const ssize_t count = ::pread(fd, buffer + done, size - done, offset + done);
    if (count < 0 && errno == EINTR) {
    } else if (count <= 0) {
      ret = OB_IO_ERROR;
    } else {
      done += count;
    }
  }
  return ret;
}

int parquet_varint(const char *data, int64_t size, int64_t &position, uint64_t &value)
{
  int ret = OB_SUCCESS;
  value = 0;
  bool done = false;
  for (int bit = 0; OB_SUCC(ret) && !done && bit < 70; bit += 7) {
    if (position >= size) {
      ret = OB_INVALID_DATA;
    } else {
      const unsigned char byte = data[position++];
      if (bit == 63 && (byte & 0xfe) != 0) {
        ret = OB_INVALID_DATA;
      } else {
        value |= static_cast<uint64_t>(byte & 0x7f) << bit;
        done = !(byte & 0x80);
      }
    }
  }
  if (!done) { ret = OB_INVALID_DATA; }
  return ret;
}

struct ParquetCompactNode
{
  ParquetCompactNode() : field_(0), type_(0), integer_(0) {}
  ParquetCompactNode *field(int id) const
  {
    ParquetCompactNode *result = nullptr;
    for (int64_t i = 0; result == nullptr && i < children_.count(); ++i) {
      if (children_.at(i)->field_ == id) { result = children_.at(i); }
    }
    return result;
  }
  int64_t number(int id, int64_t fallback = 0) const
  {
    const ParquetCompactNode *value = field(id);
    return value == nullptr ? fallback : value->integer_;
  }
  int field_;
  int type_;
  int64_t integer_;
  ObString string_;
  ObSEArray<ParquetCompactNode *, 8> children_;
};

class ParquetCompactReader
{
public:
  ParquetCompactReader(ObIAllocator &allocator, const char *data, int64_t size)
      : allocator_(allocator), data_(data), size_(size), pos_(0) {}
  ~ParquetCompactReader()
  {
    for (int64_t i = 0; i < nodes_.count(); ++i) {
      nodes_.at(i)->~ParquetCompactNode();
    }
  }
  int64_t memory_size() const
  {
    // Include node objects and the bounded child/pointer arrays. Parent-child
    // edges occur once in the compact tree, plus container growth slack.
    return nodes_.count() * (sizeof(ParquetCompactNode) + 4 * sizeof(ParquetCompactNode *));
  }
  int read(int type, ParquetCompactNode *&node, int depth = 0)
  {
    int ret = OB_SUCCESS;
    if (depth > 32 || nodes_.count() >= 200000 || pos_ > size_) {
      ret = OB_SIZE_OVERFLOW;
    } else if (OB_ISNULL(node = OB_NEWx(ParquetCompactNode, &allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(nodes_.push_back(node))) {
      node->~ParquetCompactNode();
    } else {
      node->type_ = type;
      uint64_t number = 0;
      if (type == 1 || type == 2) {
        node->integer_ = type == 1;
      } else if (type == 3) {
        if (pos_ >= size_) { ret = OB_INVALID_DATA; }
        else { node->integer_ = static_cast<int8_t>(data_[pos_++]); }
      } else if (type >= 4 && type <= 6) {
        if (OB_FAIL(parquet_varint(data_, size_, pos_, number))) {
        } else {
          node->integer_ = static_cast<int64_t>(number >> 1) ^ -static_cast<int64_t>(number & 1);
        }
      } else if (type == 7) {
        if (size_ - pos_ < 8) { ret = OB_INVALID_DATA; }
        else { pos_ += 8; }
      } else if (type == 8) {
        if (OB_FAIL(parquet_varint(data_, size_, pos_, number))) {
        } else if (number > static_cast<uint64_t>(size_ - pos_)) {
          ret = OB_INVALID_DATA;
        } else {
          node->string_.assign_ptr(data_ + pos_, number);
          pos_ += number;
        }
      } else if (type == 9 || type == 10) {
        if (pos_ >= size_) {
          ret = OB_INVALID_DATA;
        } else {
          const unsigned char header = data_[pos_++];
          const int element_type = header & 15;
          number = header >> 4;
          if (number == 15) { ret = parquet_varint(data_, size_, pos_, number); }
          if (number > 200000) { ret = OB_SIZE_OVERFLOW; }
          for (uint64_t i = 0; OB_SUCC(ret) && i < number; ++i) {
            ParquetCompactNode *child = nullptr;
            int actual_type = element_type;
            if (element_type == 1 || element_type == 2) {
              if (pos_ >= size_) { ret = OB_INVALID_DATA; }
              else { actual_type = data_[pos_++]; }
            }
            if (OB_FAIL(ret)) {
            } else if (OB_FAIL(read(actual_type, child, depth + 1))) {
            } else if (OB_FAIL(node->children_.push_back(child))) {
            }
          }
        }
      } else if (type == 11) {
        if (OB_FAIL(parquet_varint(data_, size_, pos_, number))) {
        } else if (number > 100000 || (number > 0 && pos_ >= size_)) {
          ret = OB_INVALID_DATA;
        } else if (number > 0) {
          const unsigned char types = data_[pos_++];
          for (uint64_t i = 0; OB_SUCC(ret) && i < number * 2; ++i) {
            ParquetCompactNode *child = nullptr;
            if (OB_FAIL(read(i % 2 == 0 ? types >> 4 : types & 15, child, depth + 1))) {
            } else if (OB_FAIL(node->children_.push_back(child))) {
            }
          }
        }
      } else if (type == 12) {
        int last = 0;
        bool done = false;
        while (OB_SUCC(ret) && !done) {
          if (pos_ >= size_) {
            ret = OB_INVALID_DATA;
          } else {
            const unsigned char header = data_[pos_++];
            const int field_type = header & 15;
            int id = last + (header >> 4);
            if (field_type == 0) {
              done = true;
            } else {
              if ((header >> 4) == 0) {
                if (OB_FAIL(parquet_varint(data_, size_, pos_, number))) {
                } else { id = static_cast<int>(number >> 1) ^ -static_cast<int>(number & 1); }
              }
              ParquetCompactNode *child = nullptr;
              if (OB_FAIL(ret)) {
              } else if (id <= 0 || node->field(id) != nullptr) {
                ret = OB_INVALID_DATA;
              } else if (OB_FAIL(read(field_type, child, depth + 1))) {
              } else {
                child->field_ = id;
                last = id;
                ret = node->children_.push_back(child);
              }
            }
          }
        }
      } else {
        ret = OB_INVALID_DATA;
      }
    }
    return ret;
  }
  int64_t position() const { return pos_; }
private:
  ObIAllocator &allocator_;
  const char *data_;
  int64_t size_;
  int64_t pos_;
  ObSEArray<ParquetCompactNode *, 16> nodes_;
};

int parquet_snappy(const char *input, int64_t length, char *output, int64_t expected)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  int64_t written = 0;
  uint64_t size = 0;
  if (OB_FAIL(parquet_varint(input, length, pos, size))) {
  } else if (size != expected) {
    ret = OB_INVALID_DATA;
  }
  while (OB_SUCC(ret) && pos < length) {
    const unsigned char tag = input[pos++];
    int64_t count = 0;
    uint64_t offset = 0;
    if ((tag & 3) == 0) {
      count = tag >> 2;
      if (count < 60) {
        ++count;
      } else {
        const int bytes = count - 59;
        if (bytes > 4 || length - pos < bytes) {
          ret = OB_INVALID_DATA;
        } else {
          count = parquet_little(input + pos, bytes) + 1;
          pos += bytes;
        }
      }
      if (OB_SUCC(ret)) {
        if (count > length - pos || count > expected - written) {
          ret = OB_INVALID_DATA;
        } else {
          MEMCPY(output + written, input + pos, count);
          pos += count;
          written += count;
        }
      }
    } else {
      const int bytes = (tag & 3) == 1 ? 1 : ((tag & 3) == 2 ? 2 : 4);
      if (length - pos < bytes) {
        ret = OB_INVALID_DATA;
      } else {
        count = (tag & 3) == 1 ? 4 + ((tag >> 2) & 7) : 1 + (tag >> 2);
        offset = parquet_little(input + pos, bytes);
        if ((tag & 3) == 1) { offset += (tag & 0xe0) << 3; }
        pos += bytes;
        if (offset == 0 || offset > written || count > expected - written) {
          ret = OB_INVALID_DATA;
        } else {
          for (int64_t i = 0; i < count; ++i) { output[written + i] = output[written + i - offset]; }
          written += count;
        }
      }
    }
  }
  if (OB_SUCC(ret) && written != expected) { ret = OB_INVALID_DATA; }
  return ret;
}

int parquet_decompress(int codec, const char *input, int64_t length, char *output, int64_t expected)
{
  int ret = OB_SUCCESS;
  if (codec == 0) {
    if (length != expected) { ret = OB_INVALID_DATA; }
    else { MEMCPY(output, input, length); }
  } else if (codec == 1) {
    ret = parquet_snappy(input, length, output, expected);
  } else if (codec == 2) {
    z_stream stream;
    MEMSET(&stream, 0, sizeof(stream));
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input));
    stream.avail_in = length;
    stream.next_out = reinterpret_cast<Bytef *>(output);
    stream.avail_out = MAX(1, expected);
    if (inflateInit2(&stream, 15 + 32) != Z_OK) {
      ret = OB_INVALID_DATA;
    } else {
      const int status = inflate(&stream, Z_FINISH);
      if (status != Z_STREAM_END || stream.total_out != expected || stream.avail_in != 0) { ret = OB_INVALID_DATA; }
      inflateEnd(&stream);
    }
  } else if (codec == 6) {
    ObArenaAllocator allocator("ParquetZstd");
    zstd_1_3_8::OB_ZSTD_customMem memory;
    memory.opaque = &allocator;
    memory.customAlloc = [](void *context, size_t bytes) -> void * {
      return static_cast<ObIAllocator *>(context)->alloc(bytes);
    };
    memory.customFree = [](void *context, void *pointer) {
      static_cast<ObIAllocator *>(context)->free(pointer);
    };
    size_t size = 0;
    if (OB_FAIL(zstd_1_3_8::ObZstdWrapper::decompress(memory, input, length, output, MAX(1, expected), size))) {
    } else if (size != expected) { ret = OB_INVALID_DATA; }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "Parquet compression codec");
  }
  return ret;
}

class ParquetHybridDecoder
{
public:
  ParquetHybridDecoder() { reset(nullptr, 0, 0); }
  void reset(const char *data, int64_t length, int width)
  {
    data_ = data; size_ = length; width_ = width; position_ = 0;
    remaining_ = 0; bit_ = 0; packed_ = nullptr; repeated_ = 0;
  }
  int next(uint64_t &value)
  {
    int ret = OB_SUCCESS;
    if (width_ < 0 || width_ > 64) {
      ret = OB_INVALID_DATA;
    } else if (remaining_ == 0) {
      uint64_t header = 0;
      if (OB_FAIL(parquet_varint(data_, size_, position_, header))) {
      } else if (header < 2 || (header >> 1) > INT32_MAX) {
        ret = OB_INVALID_DATA;
      } else if (header & 1) {
        remaining_ = (header >> 1) * 8;
        const int64_t bytes = (remaining_ * width_ + 7) / 8;
        if (bytes > size_ - position_) {
          ret = OB_INVALID_DATA;
        } else {
          packed_ = data_ + position_;
          bit_ = 0;
          position_ += bytes;
        }
      } else {
        remaining_ = header >> 1;
        const int bytes = (width_ + 7) / 8;
        if (bytes > size_ - position_) {
          ret = OB_INVALID_DATA;
        } else {
          repeated_ = parquet_little(data_ + position_, bytes);
          position_ += bytes;
          packed_ = nullptr;
          if (width_ < 64 && repeated_ >= (1UL << width_)) { ret = OB_INVALID_DATA; }
        }
      }
    }
    if (OB_SUCC(ret)) {
      value = repeated_;
      if (packed_ != nullptr) {
        value = 0;
        for (int i = 0; i < width_; ++i, ++bit_) {
          value |= static_cast<uint64_t>((static_cast<unsigned char>(packed_[bit_ / 8]) >> (bit_ % 8)) & 1) << i;
        }
      }
      --remaining_;
    }
    return ret;
  }
private:
  const char *data_;
  int64_t size_;
  int width_;
  int64_t position_;
  int64_t remaining_;
  int64_t bit_;
  const char *packed_;
  uint64_t repeated_;
};
} // namespace

struct ParquetSchemaColumn
{
  ParquetSchemaColumn() : physical_(-1), converted_(-1), fixed_length_(0), optional_(false) {}
  FileSqlColumn column_;
  int physical_;
  int converted_;
  int fixed_length_;
  bool optional_;
};

struct ParquetColumnChunk
{
  ParquetColumnChunk() : codec_(0), start_(0), end_(0), count_(0) {}
  int codec_;
  int64_t start_;
  int64_t end_;
  int64_t count_;
};

struct ParquetRowGroup
{
  ParquetRowGroup() : rows_(0) {}
  int64_t rows_;
  ObSEArray<ParquetColumnChunk, 16> columns_;
};

// Column pages are kept independently so advancing one projected column does
// not invalidate other columns' current pages. No row group is materialized.
class ParquetColumnReader
{
public:
  ParquetColumnReader(ParquetFileReader &owner, int fd, const ParquetSchemaColumn &schema,
                       const ParquetColumnChunk &chunk)
      : owner_(owner), fd_(fd), schema_(schema), chunk_(chunk), page_allocator_("ParquetPage"),
        dictionary_allocator_("ParquetDict"), file_pos_(chunk.start_), page_memory_(0),
        dictionary_memory_(0), remaining_(0), seen_(0), encoding_(0), data_(nullptr),
        data_size_(0), data_pos_(0), boolean_bit_(0), has_dictionary_(false) {}
  ~ParquetColumnReader()
  {
    owner_.release_page_memory(page_memory_ + dictionary_memory_);
  }
  int next(ObObj &value);
private:
  int load_page();
  int plain_value(const char *data, int64_t length, int64_t &position, int64_t &boolean_bit, ObObj &value);
  ParquetFileReader &owner_;
  int fd_;
  const ParquetSchemaColumn &schema_;
  const ParquetColumnChunk &chunk_;
  ObArenaAllocator page_allocator_;
  ObArenaAllocator dictionary_allocator_;
  int64_t file_pos_;
  int64_t page_memory_;
  int64_t dictionary_memory_;
  int64_t remaining_;
  int64_t seen_;
  int encoding_;
  const char *data_;
  int64_t data_size_;
  int64_t data_pos_;
  int64_t boolean_bit_;
  bool has_dictionary_;
  ObSEArray<ObObj, 16> dictionary_;
  ParquetHybridDecoder levels_;
  ParquetHybridDecoder indices_;
};

ParquetFileReader::ParquetFileReader(ObIAllocator &allocator)
    : allocator_(allocator), metadata_allocator_("ParquetMeta"), fd_(-1), file_size_(0),
      footer_start_(0), group_(0), group_row_(0), page_memory_(0), projection_set_(false)
{}

ParquetFileReader::~ParquetFileReader()
{
  close_readers();
  for (int64_t i = 0; i < groups_.count(); ++i) { groups_.at(i)->~ParquetRowGroup(); }
}

int ParquetFileReader::reserve_page_memory(int64_t bytes)
{
  int ret = OB_SUCCESS;
  if (bytes < 0 || page_memory_ > 256L * 1024 * 1024 - bytes) {
    ret = OB_SIZE_OVERFLOW;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "Parquet projected pages exceed 256 MiB working memory");
  } else {
    page_memory_ += bytes;
  }
  return ret;
}

int ParquetFileReader::open(int fd, FileSqlConfig &config)
{
  fd_ = fd;
  file_size_ = config.snapshot_.size_;
  return read_footer(config);
}

int ParquetFileReader::read_footer(FileSqlConfig &config)
{
  int ret = OB_SUCCESS;
  char trailer[8];
  char magic[4];
  if (file_size_ < 12) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(parquet_pread(fd_, 0, magic, 4))) {
  } else if (OB_FAIL(parquet_pread(fd_, file_size_ - 8, trailer, 8))) {
  } else if (MEMCMP(magic, "PAR1", 4) != 0 || MEMCMP(trailer + 4, "PAR1", 4) != 0) {
    ret = OB_INVALID_DATA;
  } else {
    const int64_t footer_size = parquet_little(trailer, 4);
    footer_start_ = file_size_ - 8 - footer_size;
    char *footer = nullptr;
    if (footer_size <= 0 || footer_size > PARQUET_MAX_FOOTER || footer_start_ < 4) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(footer = static_cast<char *>(metadata_allocator_.alloc(footer_size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(parquet_pread(fd_, footer_start_, footer, footer_size))) {
    } else {
      ParquetCompactReader decoder(metadata_allocator_, footer, footer_size);
      ParquetCompactNode *root = nullptr;
      if (OB_FAIL(decoder.read(12, root))) {
      } else if (decoder.position() != footer_size || root->field(2) == nullptr || root->field(4) == nullptr
                 || root->field(2)->type_ != 9 || root->field(4)->type_ != 9) {
        ret = OB_INVALID_DATA;
      } else {
        ParquetCompactNode &schema = *root->field(2);
        ParquetCompactNode &groups = *root->field(4);
        const int64_t column_count = schema.children_.count() - 1;
        if (column_count <= 0 || column_count > 1024
            || schema.children_.at(0)->number(5, -1) != column_count) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "Parquet requires a flat, non-repeated schema");
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
          const ParquetCompactNode &node = *schema.children_.at(i + 1);
          ParquetSchemaColumn *column = OB_NEWx(ParquetSchemaColumn, &metadata_allocator_);
          if (OB_ISNULL(column)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (node.field(4) == nullptr || node.field(4)->type_ != 8
                     || node.number(5) != 0 || node.number(3, -1) < 0 || node.number(3) > 1) {
            ret = OB_NOT_SUPPORTED;
          } else {
            column->physical_ = node.number(1, -1);
            column->converted_ = node.number(6, -1);
            column->fixed_length_ = node.number(2);
            column->optional_ = node.number(3) == 1;
            column->column_.name_ = node.field(4)->string_;
            column->column_.nullable_ = column->optional_;
            column->column_.length_ = 16L * 1024 * 1024;
            const ParquetCompactNode *logical = node.field(10);
            if (logical != nullptr && logical->field(6) != nullptr) {
              column->converted_ = 6;
            } else if (logical != nullptr && logical->field(8) != nullptr) {
              const ParquetCompactNode *unit = logical->field(8)->field(2);
              if (unit == nullptr) { ret = OB_INVALID_DATA; }
              else if (unit->field(1) != nullptr) { column->converted_ = 9; }
              else if (unit->field(2) != nullptr) { column->converted_ = 10; }
              else if (unit->field(3) != nullptr) { column->converted_ = 100; }
              else { ret = OB_INVALID_DATA; }
            }
            if (column->physical_ == 0) {
              column->column_.type_ = ObTinyIntType;
              column->column_.boolean_ = true;
            } else if (column->physical_ == 1 || column->physical_ == 2) {
              column->column_.type_ = ObIntType;
              if (column->converted_ == 6) {
                column->column_.type_ = ObDateType;
              } else if (column->converted_ == 9 || column->converted_ == 10 || column->converted_ == 100) {
                column->column_.type_ = ObDateTimeType;
              } else if (column->converted_ == 14) {
                column->column_.type_ = ObVarcharType;
              } else if (column->converted_ == 5) {
                ret = OB_NOT_SUPPORTED;
              }
            } else if (column->physical_ == 3) {
              column->column_.type_ = ObDateTimeType;
            } else if (column->physical_ == 4 || column->physical_ == 5) {
              column->column_.type_ = ObDoubleType;
            } else if (column->physical_ == 6 || (column->physical_ == 7 && column->fixed_length_ > 0)) {
              column->column_.type_ = ObVarcharType;
              if (column->converted_ == 5) { ret = OB_NOT_SUPPORTED; }
            } else {
              ret = OB_NOT_SUPPORTED;
            }
            if (logical != nullptr && logical->field(11) != nullptr) { column->column_.type_ = ObNullType; }
            if (column->column_.name_.empty() || column->column_.name_.length() > 256) { ret = OB_INVALID_DATA; }
            for (int64_t j = 0; OB_SUCC(ret) && j < schema_.count(); ++j) {
              if (schema_.at(j)->column_.name_.case_compare(column->column_.name_) == 0) { ret = OB_INVALID_DATA; }
            }
            if (OB_SUCC(ret)) { ret = schema_.push_back(column); }
            if (OB_SUCC(ret) && !config.inferred_) {
              FileSqlColumn output = column->column_;
              if (OB_FAIL(ob_write_string(config.allocator_, output.name_, output.name_, true))) {
              } else if (OB_FAIL(config.columns_.push_back(output))) {
              }
            }
          }
        }
        int64_t total_rows = 0;
        for (int64_t i = 0; OB_SUCC(ret) && i < groups.children_.count(); ++i) {
          const ParquetCompactNode &node = *groups.children_.at(i);
          ParquetRowGroup *group = OB_NEWx(ParquetRowGroup, &metadata_allocator_);
          if (OB_ISNULL(group)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(groups_.push_back(group))) {
          } else if (node.number(3, -1) < 0 || node.field(1) == nullptr
                     || node.field(1)->children_.count() != column_count) {
            ret = OB_INVALID_DATA;
          } else {
            group->rows_ = node.number(3);
            if (group->rows_ > INT64_MAX - total_rows) { ret = OB_INVALID_DATA; }
            else { total_rows += group->rows_; }
            for (int64_t j = 0; OB_SUCC(ret) && j < column_count; ++j) {
              const ParquetCompactNode &chunk = *node.field(1)->children_.at(j);
              const ParquetCompactNode *metadata = chunk.field(3);
              if ((chunk.field(1) != nullptr && !chunk.field(1)->string_.empty())
                  || metadata == nullptr || metadata->number(1, -1) != schema_.at(j)->physical_
                  || metadata->number(5, -1) != group->rows_) {
                ret = OB_INVALID_DATA;
              } else {
                ParquetColumnChunk column;
                column.codec_ = metadata->number(4, -1);
                column.count_ = metadata->number(5);
                column.start_ = metadata->number(11, metadata->number(9, -1));
                const int64_t size = metadata->number(7, -1);
                if (column.start_ < 4 || size < 0 || column.start_ > footer_start_
                    || size > footer_start_ - column.start_) {
                  ret = OB_INVALID_DATA;
                } else {
                  column.end_ = column.start_ + size;
                  ret = group->columns_.push_back(column);
                }
              }
            }
          }
        }
        if (OB_SUCC(ret) && total_rows != root->number(3, -1)) { ret = OB_INVALID_DATA; }
        if (OB_SUCC(ret) && !config.inferred_) { config.rows_ = total_rows; }
      }
    }
  }
  return ret;
}

int ParquetColumnReader::plain_value(const char *data, int64_t length, int64_t &position,
                                     int64_t &boolean_bit, ObObj &value)
{
  int ret = OB_SUCCESS;
  const int physical = schema_.physical_;
  const int bytes = physical == 1 || physical == 4 ? 4 : (physical == 2 || physical == 5 ? 8 : (physical == 3 ? 12 : 0));
  if (physical == 0) {
    if (boolean_bit / 8 >= length) { ret = OB_INVALID_DATA; }
    else { value.set_tinyint((static_cast<unsigned char>(data[boolean_bit / 8]) >> (boolean_bit % 8)) & 1); ++boolean_bit; }
  } else if (bytes > 0) {
    if (bytes > length - position) {
      ret = OB_INVALID_DATA;
    } else {
      const uint64_t raw = parquet_little(data + position, MIN(bytes, 8));
      int64_t integer = physical == 1 ? static_cast<int32_t>(raw) : static_cast<int64_t>(raw);
      if (physical == 4) {
        const uint32_t bits = raw;
        float number = 0;
        MEMCPY(&number, &bits, 4);
        value.set_double(number);
        if (!std::isfinite(number)) { ret = OB_INVALID_DATA; }
      } else if (physical == 5) {
        double number = 0;
        MEMCPY(&number, &raw, 8);
        value.set_double(number);
        if (!std::isfinite(number)) { ret = OB_INVALID_DATA; }
      } else if (physical == 3) {
        const int64_t day = static_cast<int64_t>(parquet_little(data + position + 8, 4)) - 2440588;
        if (raw >= 86400UL * 1000000000UL || day < -719162 || day > 2932896) { ret = OB_INVALID_DATA; }
        else { value.set_datetime(day * 86400L * 1000000L + raw / 1000); }
      } else if (schema_.column_.type_ == ObDateType) {
        if (integer < -719162 || integer > 2932896) { ret = OB_INVALID_DATA; }
        else { value.set_date(integer); }
      } else if (schema_.column_.type_ == ObDateTimeType) {
        if (schema_.converted_ == 9) {
          if (integer > INT64_MAX / 1000 || integer < INT64_MIN / 1000) { ret = OB_INVALID_DATA; }
          else { integer *= 1000; }
        } else if (schema_.converted_ == 100) {
          const int64_t remainder = integer % 1000;
          integer = integer / 1000 - (integer < 0 && remainder != 0 ? 1 : 0);
        }
        if (integer < -62135596800000000L || integer > 253402300799999999L) { ret = OB_INVALID_DATA; }
        if (OB_SUCC(ret)) { value.set_datetime(integer); }
      } else if (schema_.converted_ == 14) {
        char buffer[32];
        const int count = snprintf(buffer, sizeof(buffer), "%lu", raw);
        ObString text;
        if (OB_FAIL(ob_write_string(page_allocator_, ObString(count, buffer), text))) {
        } else { value.set_varchar(text); value.set_collation_type(CS_TYPE_UTF8MB4_BIN); }
      } else {
        if (physical == 1 && schema_.converted_ >= 11 && schema_.converted_ <= 13) { integer = static_cast<uint32_t>(raw); }
        value.set_int(integer);
      }
      position += bytes;
    }
  } else if (physical == 6 || physical == 7) {
    int64_t size = schema_.fixed_length_;
    if (physical == 6) {
      if (length - position < 4) { ret = OB_INVALID_DATA; }
      else { size = parquet_little(data + position, 4); position += 4; }
    }
    if (OB_SUCC(ret)) {
      if (size < 0 || size > 16L * 1024 * 1024 || size > length - position) { ret = OB_INVALID_DATA; }
      else { value.set_varchar(ObString(size, data + position)); value.set_collation_type(CS_TYPE_UTF8MB4_BIN); position += size; }
    }
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

int ParquetColumnReader::load_page()
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && remaining_ == 0) {
    owner_.release_page_memory(page_memory_);
    page_memory_ = 0;
    page_allocator_.reuse();
    const int64_t header_size = MIN(65536, chunk_.end_ - file_pos_);
    char *header = nullptr;
    if (header_size <= 0) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(header = static_cast<char *>(page_allocator_.alloc(header_size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(parquet_pread(fd_, file_pos_, header, header_size))) {
    } else {
      ParquetCompactReader decoder(page_allocator_, header, header_size);
      ParquetCompactNode *root = nullptr;
      if (OB_FAIL(decoder.read(12, root))) {
      } else {
        const int64_t compressed = root->number(3, -1);
        const int64_t uncompressed = root->number(2, -1);
        const int page_type = root->number(1, -1);
        file_pos_ += decoder.position();
        char *input = nullptr;
        char *output = nullptr;
        if (compressed < 0 || compressed > PARQUET_MAX_PAGE || uncompressed < 0
            || uncompressed > PARQUET_MAX_PAGE || compressed > chunk_.end_ - file_pos_) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(owner_.reserve_page_memory(compressed + uncompressed + header_size + decoder.memory_size()))) {
        } else {
          page_memory_ = compressed + uncompressed + header_size + decoder.memory_size();
          input = static_cast<char *>(page_allocator_.alloc(MAX(1, compressed)));
          output = static_cast<char *>(page_allocator_.alloc(MAX(1, uncompressed)));
          if (input == nullptr || output == nullptr) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(parquet_pread(fd_, file_pos_, input, compressed))) {
          } else if (root->field(4) != nullptr
                     && static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef *>(input), compressed))
                        != static_cast<uint32_t>(root->number(4))) {
            ret = OB_INVALID_DATA;
          }
          file_pos_ += compressed;
        }
        ParquetCompactNode *data_header = page_type == 3 ? root->field(8) : root->field(5);
        int64_t levels_size = 0;
        int64_t data_offset = 0;
        if (OB_FAIL(ret)) {
        } else if (page_type == 3) {
          if (data_header == nullptr || data_header->number(1, -1) < 0
              || data_header->number(2, -1) < 0 || data_header->number(2) > data_header->number(1)
              || data_header->number(3, -1) != data_header->number(1)
              || data_header->number(6) != 0) {
            ret = OB_INVALID_DATA;
          } else {
            levels_size = data_header->number(5, -1);
            data_offset = levels_size;
            if (levels_size < 0 || levels_size > compressed || levels_size > uncompressed
                || (!schema_.optional_ && levels_size != 0)) {
              ret = OB_INVALID_DATA;
            } else {
              MEMCPY(output, input, levels_size);
              ret = parquet_decompress(data_header->number(7, 1) ? chunk_.codec_ : 0,
                      input + levels_size, compressed - levels_size,
                      output + levels_size, uncompressed - levels_size);
            }
          }
        } else if (OB_FAIL(parquet_decompress(chunk_.codec_, input, compressed, output, uncompressed))) {
        }
        if (OB_FAIL(ret)) {
        } else if (page_type == 2) {
          ParquetCompactNode *dict_header = root->field(7);
          if (dict_header == nullptr || dict_header->number(1, -1) < 0
              || dict_header->number(1) > PARQUET_MAX_VALUES || dict_header->number(2, -1) != 0
              || has_dictionary_) {
            ret = OB_INVALID_DATA;
          } else {
            const int64_t count = dict_header->number(1);
            const int64_t budget = uncompressed + count * static_cast<int64_t>(sizeof(ObObj));
            int64_t pos = 0;
            int64_t bit = 0;
            if (OB_FAIL(owner_.reserve_page_memory(budget))) {
            } else { dictionary_memory_ = budget; }
            for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
              ObObj value;
              if (OB_FAIL(plain_value(output, uncompressed, pos, bit, value))) {
              } else if (value.is_string_type()) {
                ObString text;
                if (OB_FAIL(ob_write_string(dictionary_allocator_, value.get_string(), text))) {
                } else { value.set_varchar(text); value.set_collation_type(CS_TYPE_UTF8MB4_BIN); }
              }
              if (OB_SUCC(ret)) { ret = dictionary_.push_back(value); }
            }
            has_dictionary_ = OB_SUCC(ret);
          }
        } else if (page_type == 0 || page_type == 3) {
          if (data_header == nullptr || data_header->number(1, -1) <= 0
              || data_header->number(1) > PARQUET_MAX_VALUES
              || data_header->number(1) > chunk_.count_ - seen_) {
            ret = OB_INVALID_DATA;
          } else {
            remaining_ = data_header->number(1);
            encoding_ = data_header->number(page_type == 3 ? 4 : 2, -1);
            if (schema_.optional_ && page_type == 0) {
              if (data_header->number(3, -1) != 3) {
                ret = OB_NOT_SUPPORTED;
              } else if (uncompressed < 4) {
                ret = OB_INVALID_DATA;
              } else {
                levels_size = parquet_little(output, 4);
                data_offset = 4 + levels_size;
                if (levels_size > uncompressed - 4) { ret = OB_INVALID_DATA; }
              }
            }
            if (OB_SUCC(ret)) {
              levels_.reset(schema_.optional_ ? output + (page_type == 0 ? 4 : 0) : nullptr, levels_size, 1);
              data_ = output + data_offset;
              data_size_ = uncompressed - data_offset;
              data_pos_ = 0;
              boolean_bit_ = 0;
              if (encoding_ == 2 || encoding_ == 8) {
                if (!has_dictionary_) {
                  ret = OB_INVALID_DATA;
                } else if (data_size_ > 0 && static_cast<unsigned char>(data_[0]) > 32) {
                  ret = OB_INVALID_DATA;
                } else {
                  indices_.reset(data_size_ > 0 ? data_ + 1 : nullptr,
                      MAX(0, data_size_ - 1), data_size_ > 0 ? static_cast<unsigned char>(data_[0]) : 0);
                }
              } else if (encoding_ == 3 && schema_.physical_ == 0) {
                if (data_size_ < 4 || parquet_little(data_, 4) != data_size_ - 4) {
                  ret = OB_INVALID_DATA;
                } else { indices_.reset(data_ + 4, data_size_ - 4, 1); }
              } else if (encoding_ != 0) {
                ret = OB_NOT_SUPPORTED;
                LOG_USER_ERROR(OB_NOT_SUPPORTED, "Parquet value encoding");
              }
            }
          }
        } else if (page_type != 1) {
          ret = OB_INVALID_DATA;
        }
      }
    }
  }
  return ret;
}

int ParquetColumnReader::next(ObObj &value)
{
  int ret = OB_SUCCESS;
  if (seen_ >= chunk_.count_) {
    ret = OB_ITER_END;
  } else if (remaining_ == 0 && OB_FAIL(load_page())) {
  } else {
    uint64_t level = 1;
    if (schema_.optional_ && OB_FAIL(levels_.next(level))) {
    } else if (level > 1) {
      ret = OB_INVALID_DATA;
    } else if (level == 0) {
      value.set_null();
    } else if (encoding_ == 0) {
      ret = plain_value(data_, data_size_, data_pos_, boolean_bit_, value);
    } else {
      uint64_t index = 0;
      if (OB_FAIL(indices_.next(index))) {
      } else if (encoding_ == 3 && schema_.physical_ == 0) {
        if (index > 1) { ret = OB_INVALID_DATA; }
        else { value.set_tinyint(index); }
      } else if (index >= dictionary_.count()) {
        ret = OB_INVALID_DATA;
      } else {
        value = dictionary_.at(index);
      }
    }
    if (OB_SUCC(ret)) { --remaining_; ++seen_; }
  }
  return ret;
}

void ParquetFileReader::close_readers()
{
  for (int64_t i = 0; i < readers_.count(); ++i) {
    if (readers_.at(i) != nullptr) {
      OB_DELETEx(ParquetColumnReader, &allocator_, readers_.at(i));
    }
  }
  readers_.reuse();
}

int ParquetFileReader::rewind()
{
  close_readers();
  group_ = 0;
  group_row_ = 0;
  return OB_SUCCESS;
}

int ParquetFileReader::set_projection(const ObIArray<int64_t> &columns)
{
  int ret = projection_.assign(columns);
  projection_set_ = OB_SUCC(ret);
  return ret;
}

int ParquetFileReader::start_row_group()
{
  int ret = OB_SUCCESS;
  close_readers();
  const ParquetRowGroup &group = *groups_.at(group_);
  for (int64_t i = 0; OB_SUCC(ret) && i < schema_.count(); ++i) {
    if (OB_FAIL(readers_.push_back(nullptr))) {
    }
  }
  const int64_t count = projection_set_ ? projection_.count() : schema_.count();
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    const int64_t column = projection_set_ ? projection_.at(i) : i;
    if (column < 0 || column >= schema_.count()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (readers_.at(column) == nullptr) {
      ParquetColumnReader *reader = OB_NEWx(ParquetColumnReader, &allocator_,
                  *this, fd_, *schema_.at(column), group.columns_.at(column));
      if (OB_ISNULL(reader)) { ret = OB_ALLOCATE_MEMORY_FAILED; }
      else { readers_.at(column) = reader; }
    }
  }
  return ret;
}

int ParquetFileReader::next_row(ObIArray<ObObj> &values)
{
  int ret = OB_SUCCESS;
  values.reuse();
  while (group_ < groups_.count() && group_row_ >= groups_.at(group_)->rows_) {
    close_readers();
    ++group_;
    group_row_ = 0;
  }
  if (group_ >= groups_.count()) {
    ret = OB_ITER_END;
  } else if (readers_.empty() && OB_FAIL(start_row_group())) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < schema_.count(); ++i) {
      ObObj value;
      value.set_null();
      if (readers_.at(i) != nullptr && OB_FAIL(readers_.at(i)->next(value))) {
        if (ret == OB_ITER_END) { ret = OB_INVALID_DATA; }
      } else if (OB_FAIL(values.push_back(value))) {
      }
    }
    if (OB_SUCC(ret)) { ++group_row_; }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
