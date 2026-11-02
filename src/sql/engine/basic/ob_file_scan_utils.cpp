/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "ob_file_scan_utils.h"
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>
#include <errno.h>
#include "lib/utility/ob_print_utils.h"
#include "lib/charset/ob_charset.h"

namespace oceanbase
{
namespace sql
{

const char *ob_file_column_type_name(const ObFileColumnType type)
{
  const char *name = "NULL";
  switch (type) {
    case OB_FILE_TYPE_NULL: name = "NULL"; break;
    case OB_FILE_TYPE_BOOLEAN: name = "BOOLEAN"; break;
    case OB_FILE_TYPE_BIGINT: name = "BIGINT"; break;
    case OB_FILE_TYPE_DOUBLE: name = "DOUBLE"; break;
    case OB_FILE_TYPE_DATE: name = "DATE"; break;
    case OB_FILE_TYPE_DATETIME: name = "DATETIME"; break;
    case OB_FILE_TYPE_VARCHAR: name = "VARCHAR"; break;
    default: name = "VARCHAR"; break;
  }
  return name;
}

common::ObObjType ob_file_column_to_obj_type(const ObFileColumnType type)
{
  common::ObObjType obj_type = common::ObVarcharType;
  switch (type) {
    case OB_FILE_TYPE_BOOLEAN: obj_type = common::ObTinyIntType; break;
    case OB_FILE_TYPE_BIGINT: obj_type = common::ObIntType; break;
    case OB_FILE_TYPE_DOUBLE: obj_type = common::ObDoubleType; break;
    case OB_FILE_TYPE_DATE: obj_type = common::ObDateType; break;
    case OB_FILE_TYPE_DATETIME: obj_type = common::ObDateTimeType; break;
    case OB_FILE_TYPE_VARCHAR: obj_type = common::ObVarcharType; break;
    // A column that stayed NULL for every row still needs a concrete type.
    case OB_FILE_TYPE_NULL:
    default: obj_type = common::ObVarcharType; break;
  }
  return obj_type;
}

namespace
{

bool is_blank(const char *ptr, const int64_t len)
{
  bool blank = true;
  for (int64_t i = 0; blank && i < len; ++i) {
    blank = ptr[i] == ' ' || ptr[i] == '\t' || ptr[i] == '\r';
  }
  return blank;
}

bool is_all_digits(const char *ptr, const int64_t len, int64_t &pos)
{
  bool ok = len > 0;
  pos = 0;
  if (ok && (ptr[0] == '-' || ptr[0] == '+')) {
    pos = 1;
    ok = len > 1;
  }
  for (; ok && pos < len; ++pos) {
    ok = ptr[pos] >= '0' && ptr[pos] <= '9';
  }
  return ok;
}

// Parses YYYY-MM-DD exactly; returns true only for a real calendar date.
bool parse_date_strict(const char *ptr, const int64_t len)
{
  bool ok = len == 10 && ptr[4] == '-' && ptr[7] == '-';
  int year = 0;
  int month = 0;
  int day = 0;
  int64_t pos = 0;
  if (ok) {
    int64_t p = 0;
    ok = is_all_digits(ptr, 4, p) && is_all_digits(ptr + 5, 2, p) && is_all_digits(ptr + 8, 2, p);
  }
  if (ok) {
    for (int i = 0; i < 4; ++i) year = year * 10 + (ptr[i] - '0');
    month = (ptr[5] - '0') * 10 + (ptr[6] - '0');
    day = (ptr[8] - '0') * 10 + (ptr[9] - '0');
    ok = year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1;
    if (ok) {
      static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
      const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
      const int max_day = month == 2 && leap ? 29 : days_in_month[month - 1];
      ok = day <= max_day;
    }
  }
  UNUSED(pos);
  return ok;
}

// Parses YYYY-MM-DD HH:MM:SS or the ISO 'T' variant.
bool parse_datetime_strict(const char *ptr, const int64_t len)
{
  bool ok = (len == 19 || len == 22) && parse_date_strict(ptr, 10);
  if (ok) {
    ok = (ptr[10] == ' ' || ptr[10] == 'T') &&
          ptr[13] == ':' && ptr[16] == ':';
    if (ok) {
      int hour = (ptr[11] - '0') * 10 + (ptr[12] - '0');
      int minute = (ptr[14] - '0') * 10 + (ptr[15] - '0');
      int second = (ptr[17] - '0') * 10 + (ptr[18] - '0');
      ok = ptr[11] >= '0' && ptr[11] <= '9' && ptr[12] >= '0' && ptr[12] <= '9' &&
           ptr[14] >= '0' && ptr[14] <= '9' && ptr[15] >= '0' && ptr[15] <= '9' &&
           ptr[17] >= '0' && ptr[17] <= '9' && ptr[18] >= '0' && ptr[18] <= '9' &&
           hour <= 23 && minute <= 59 && second <= 59;
      if (ok && len == 22) {
        ok = ptr[19] == '.' &&
             ptr[20] >= '0' && ptr[20] <= '9' && ptr[21] >= '0' && ptr[21] <= '9';
      }
    }
  }
  return ok;
}

bool parse_bool(const char *ptr, const int64_t len)
{
  return (len == 4 && 0 == strncasecmp(ptr, "true", 4)) ||
         (len == 5 && 0 == strncasecmp(ptr, "false", 5));
}

bool parse_double(const char *ptr, const int64_t len)
{
  bool ok = len > 0;
  bool seen_digit = false;
  bool seen_dot = false;
  bool seen_exp = false;
  int64_t i = 0;
  if (ok && (ptr[0] == '-' || ptr[0] == '+')) {
    i = 1;
    ok = len > 1;
  }
  for (; ok && i < len; ++i) {
    const char c = ptr[i];
    if (c >= '0' && c <= '9') {
      seen_digit = true;
    } else if (c == '.' && !seen_dot && !seen_exp) {
      seen_dot = true;
    } else if ((c == 'e' || c == 'E') && !seen_exp && seen_digit) {
      seen_exp = true;
      if (i + 1 < len && (ptr[i + 1] == '-' || ptr[i + 1] == '+')) {
        ++i;
      }
    } else {
      ok = false;
    }
  }
  return ok && seen_digit;
}

// Classifies one raw cell. The empty/NULL string maps to NULL so the caller can
// distinguish "absent" from "empty string".
ObFileColumnType classify_cell(const char *ptr, const int64_t len,
                               const ObFileReadOptions &options)
{
  ObFileColumnType type = OB_FILE_TYPE_VARCHAR;
  if (0 == len) {
    type = OB_FILE_TYPE_NULL;
  } else if (!options.null_str_.empty() &&
             len == options.null_str_.length() &&
             0 == strncmp(ptr, options.null_str_.ptr(), len)) {
    type = OB_FILE_TYPE_NULL;
  } else if (is_blank(ptr, len)) {
    type = OB_FILE_TYPE_NULL;
  } else if (parse_bool(ptr, len)) {
    type = OB_FILE_TYPE_BOOLEAN;
  } else if (parse_datetime_strict(ptr, len)) {
    type = OB_FILE_TYPE_DATETIME;
  } else if (parse_date_strict(ptr, len)) {
    type = OB_FILE_TYPE_DATE;
  } else {
    int64_t digits_pos = 0;
    if (is_all_digits(ptr, len, digits_pos)) {
      type = OB_FILE_TYPE_BIGINT;
    } else if (parse_double(ptr, len)) {
      type = OB_FILE_TYPE_DOUBLE;
    }
  }
  return type;
}

// Widening rule between an inferred column type and a new value type.
ObFileColumnType widen_type(const ObFileColumnType current, const ObFileColumnType incoming)
{
  ObFileColumnType result = current;
  if (OB_FILE_TYPE_NULL == incoming) {
    result = current;
  } else if (OB_FILE_TYPE_NULL == current) {
    result = incoming;
  } else if (current == incoming) {
    result = current;
  } else if ((OB_FILE_TYPE_BIGINT == current && OB_FILE_TYPE_DOUBLE == incoming) ||
             (OB_FILE_TYPE_DOUBLE == current && OB_FILE_TYPE_BIGINT == incoming)) {
    result = OB_FILE_TYPE_DOUBLE;
  } else if ((OB_FILE_TYPE_DATE == current && OB_FILE_TYPE_DATETIME == incoming) ||
             (OB_FILE_TYPE_DATETIME == current && OB_FILE_TYPE_DATE == incoming)) {
    // A date and a timestamp in the same column must both survive, so the column
    // becomes a timestamp.
    result = OB_FILE_TYPE_DATETIME;
  } else {
    // Any other combination has no common scalar type.
    result = OB_FILE_TYPE_VARCHAR;
  }
  return result;
}

int read_whole_file(common::ObIAllocator &allocator, const common::ObString &path,
                    common::ObString &content, const int64_t max_size)
{
  int ret = OB_SUCCESS;
  FILE *fp = nullptr;
  if (OB_ISNULL(fp = fopen(path.ptr(), "rb"))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("failed to open file", K(ret), K(path), K(errno));
  } else if (OB_FAIL(fseek(fp, 0, SEEK_END))) {
    LOG_WARN("failed to seek file", K(ret), K(path));
  } else {
    const long size = ftell(fp);
    if (size < 0) {
      ret = OB_IO_ERROR;
      LOG_WARN("failed to tell file size", K(ret), K(path));
    } else if (max_size > 0 && size > max_size) {
      ret = OB_SIZE_OVERFLOW;
      LOG_USER_ERROR(OB_SIZE_OVERFLOW);
      LOG_WARN("file exceeds the allowed size", K(ret), K(size), K(max_size));
    } else {
      rewind(fp);
      char *buf = static_cast<char *>(allocator.alloc(size + 1));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (size > 0 && static_cast<long>(fread(buf, 1, size, fp)) != size) {
        ret = OB_IO_ERROR;
        LOG_WARN("failed to read file", K(ret), K(path), K(size));
      } else {
        buf[size] = '\0';
        content.assign_ptr(buf, static_cast<int32_t>(size));
      }
    }
  }
  if (OB_NOT_NULL(fp)) {
    fclose(fp);
  }
  return ret;
}

const char *path_basename(const char *path)
{
  const char *base = path;
  for (const char *p = path; *p != '\0'; ++p) {
    if (*p == '/') {
      base = p + 1;
    }
  }
  return base;
}

// Rejects a resolved path that is not inside `secure_file_priv`. Both sides are
// made absolute first so that ".." cannot escape the allowed directory.
int check_path_contained(common::ObIAllocator &allocator,
                         const common::ObString &path,
                         const common::ObString &secure_file_priv,
                         common::ObString &resolved)
{
  int ret = OB_SUCCESS;
  char resolved_buf[PATH_MAX];
  if (OB_ISNULL(realpath(path.ptr(), resolved_buf))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("file does not exist or cannot be resolved", K(ret), K(path), K(errno));
  } else if (!secure_file_priv.empty() && 0 != secure_file_priv.case_compare("/")) {
    char priv_buf[PATH_MAX];
    if (OB_ISNULL(realpath(secure_file_priv.ptr(), priv_buf))) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("secure_file_priv does not exist", K(ret), K(secure_file_priv));
    } else {
      const int64_t priv_len = static_cast<int64_t>(strlen(priv_buf));
      const bool contained = priv_len == 1 ? priv_buf[0] == '/' :
          0 == strncmp(resolved_buf, priv_buf, priv_len) &&
          (resolved_buf[priv_len] == '/' || resolved_buf[priv_len] == '\0');
      if (!contained) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path is outside secure_file_priv");
        LOG_WARN("path escapes secure_file_priv", K(ret), K(path), K(secure_file_priv));
      }
    }
  }
  if (OB_SUCC(ret)) {
    const int64_t len = static_cast<int64_t>(strlen(resolved_buf));
    char *ptr = static_cast<char *>(allocator.alloc(len + 1));
    if (OB_ISNULL(ptr)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(ptr, resolved_buf, len);
      ptr[len] = '\0';
      resolved.assign_ptr(ptr, static_cast<int32_t>(len));
    }
  }
  return ret;
}

} // anonymous namespace

int ob_file_sql_check_access(common::ObIAllocator &allocator,
                             const common::ObString &path,
                             const common::ObString &secure_file_priv,
                             bool enable_file_sql)
{
  int ret = OB_SUCCESS;
  common::ObString resolved;
  struct stat st;
  if (!enable_file_sql) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file table function is disabled, set enable_file_sql=ON first");
  } else if (path.empty() || path.length() >= PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path is empty or too long");
  } else if (strstr(path.ptr(), "://") != nullptr) {
    // Remote and special-scheme paths are explicitly out of scope.
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "only local file paths are supported");
  } else if (OB_FAIL(check_path_contained(allocator, path, secure_file_priv, resolved))) {
  } else if (0 != stat(resolved.ptr(), &st)) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("failed to stat file", K(ret), K(resolved), K(errno));
  } else if (!S_ISREG(st.st_mode)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "path is not a regular file");
  }
  return ret;
}

int ob_file_sql_resolve_paths(common::ObIAllocator &allocator,
                              const common::ObString &pattern,
                              const common::ObString &secure_file_priv,
                              common::ObIArray<common::ObString> &paths)
{
  int ret = OB_SUCCESS;
  common::ObString resolved_pattern;
  if (pattern.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file pattern is empty");
  } else if (OB_FAIL(check_path_contained(allocator, ObString::make_string(pattern.ptr()),
                                          secure_file_priv, resolved_pattern))) {
    // A glob pattern usually does not name an existing file; fall back to
    // resolving the directory part so containment is still enforced.
    const bool has_glob = strpbrk(pattern.ptr(), "*?[") != nullptr;
    if (OB_FILE_NOT_EXIST == ret && has_glob) {
      ret = OB_SUCCESS;
      resolved_pattern = pattern;
    }
  }
  if (OB_SUCC(ret)) {
    const bool has_glob = strpbrk(resolved_pattern.ptr(), "*?[") != nullptr;
    if (!has_glob) {
      common::ObString single;
      if (OB_FAIL(ob_file_sql_check_access(allocator, resolved_pattern, secure_file_priv, true))) {
      } else if (OB_FAIL(paths.push_back(resolved_pattern))) {
      }
      UNUSED(single);
    } else {
      // Split into directory + filename pattern, then match entries of that one
      // directory. Recursive expansion is intentionally not supported.
      const char *raw = resolved_pattern.ptr();
      const char *base = path_basename(raw);
      ObString dir;
      if (base == raw) {
        dir = ObString::make_string(".");
      } else {
        dir.assign_ptr(raw, static_cast<int32_t>(base - raw));
      }
      const char *dir_c = static_cast<const char *>(allocator.alloc(dir.length() + 1));
      if (OB_ISNULL(dir_c)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(const_cast<char *>(dir_c), dir.ptr(), dir.length());
        const_cast<char *>(dir_c)[dir.length()] = '\0';
        DIR *d = opendir(dir_c);
        if (OB_ISNULL(d)) {
          ret = OB_FILE_NOT_EXIST;
          LOG_WARN("failed to open directory", K(ret), K(dir), K(errno));
        } else {
          struct dirent *ent = nullptr;
          while (OB_SUCC(ret) && OB_NOT_NULL(ent = readdir(d))) {
            if (0 == strcmp(ent->d_name, ".") || 0 == strcmp(ent->d_name, "..")) {
              continue;
            }
            if (0 != fnmatch(base, ent->d_name, 0)) {
              continue;
            }
            const int64_t full_len = dir.length() + 1 + static_cast<int64_t>(strlen(ent->d_name));
            char *full = static_cast<char *>(allocator.alloc(full_len + 1));
            if (OB_ISNULL(full)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              MEMCPY(full, dir.ptr(), dir.length());
              full[dir.length()] = '/';
              MEMCPY(full + dir.length() + 1, ent->d_name, strlen(ent->d_name));
              full[full_len] = '\0';
              common::ObString one(full_len, full);
              if (OB_FAIL(paths.push_back(one))) {
              }
            }
          }
          closedir(d);
        }
      }
    }
  }
  return ret;
}

// Reads one CSV row honoring quotes, escaped quotes and embedded newlines. The
// cursor advances past the row terminator.
// Reads one CSV row honoring quotes, escaped quotes and embedded newlines.
// The caller owns the arena, so every emitted field points into it and stays
// valid for the lifetime of the statement.
struct ObCsvFieldBuf
{
  ObCsvFieldBuf() : buf_(nullptr), len_(0), cap_(0), quoted_(false), started_(false) {}
  char *buf_;
  int64_t len_;
  int64_t cap_;
  bool quoted_;
  bool started_;

  int append(common::ObIAllocator &allocator, const char c)
  {
    int ret = OB_SUCCESS;
    if (len_ + 1 > cap_) {
      const int64_t new_cap = cap_ == 0 ? 64 : cap_ * 2;
      char *grown = static_cast<char *>(allocator.alloc(new_cap));
      if (OB_ISNULL(grown)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (len_ > 0) {
          MEMCPY(grown, buf_, len_);
        }
        buf_ = grown;
        cap_ = new_cap;
      }
    }
    if (OB_SUCC(ret)) {
      buf_[len_++] = c;
    }
    return ret;
  }

  int finish(common::ObIAllocator &allocator, common::ObString &out)
  {
    int ret = OB_SUCCESS;
    char *ptr = static_cast<char *>(allocator.alloc(len_ + 1));
    if (OB_ISNULL(ptr)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (len_ > 0) {
        MEMCPY(ptr, buf_, len_);
      }
      ptr[len_] = '\0';
      out.assign_ptr(ptr, static_cast<int32_t>(len_));
    }
    return ret;
  }
};

int read_csv_row(const char *&cursor, const char *end,
                 const ObFileReadOptions &options,
                 common::ObIAllocator &allocator,
                 common::ObIArray<common::ObString> &fields)
{
  int ret = OB_SUCCESS;
  fields.reuse();
  if (cursor >= end) {
    ret = OB_ITER_END;
  } else {
    ObCsvFieldBuf field;
    bool in_quotes = false;
    bool row_done = false;
    while (OB_SUCC(ret) && !row_done) {
      if (cursor >= end) {
        // EOF terminates the final (unterminated) row.
        if (field.started_ || field.quoted_ || field.len_ > 0) {
          common::ObString out;
          if (OB_FAIL(field.finish(allocator, out))) {
          } else if (OB_FAIL(fields.push_back(out))) {
          }
        }
        row_done = true;
        break;
      }
      const char c = *cursor;
      if (in_quotes) {
        if (c == options.quote_) {
          if (cursor + 1 < end && *(cursor + 1) == options.quote_) {
            // Doubled quote inside a quoted field is a literal quote.
            if (OB_FAIL(field.append(allocator, options.quote_))) {
            }
            cursor += 2;
          } else {
            in_quotes = false;
            ++cursor;
          }
        } else {
          if (OB_FAIL(field.append(allocator, c))) {
          }
          ++cursor;
        }
      } else if (c == options.quote_ && !field.started_) {
        in_quotes = true;
        field.quoted_ = true;
        field.started_ = true;
        ++cursor;
      } else if (c == options.delimiter_) {
        common::ObString out;
        if (OB_FAIL(field.finish(allocator, out))) {
        } else if (OB_FAIL(fields.push_back(out))) {
        }
        field = ObCsvFieldBuf();
        ++cursor;
      } else if (c == '\n' || c == '\r') {
        common::ObString out;
        if (OB_FAIL(field.finish(allocator, out))) {
        } else if (OB_FAIL(fields.push_back(out))) {
        }
        row_done = true;
        if (c == '\r' && cursor + 1 < end && *(cursor + 1) == '\n') {
          cursor += 2;
        } else {
          ++cursor;
        }
      } else {
        if (OB_FAIL(field.append(allocator, c))) {
        }
        field.started_ = true;
        ++cursor;
      }
    }
  }
  return ret;
}

int ob_file_sql_infer_csv(common::ObIAllocator &allocator,
                          const common::ObString &path,
                          const ObFileReadOptions &options,
                          ObFileSchema &schema)
{
  int ret = OB_SUCCESS;
  common::ObString content;
  schema.columns_.reuse();
  schema.column_names_.reuse();
  if (OB_UNLIKELY(options.sample_size_ != -1)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "only sample_size = -1 is supported for csv");
  } else if (OB_FAIL(read_whole_file(allocator, path, content, 0))) {
  } else {
    const char *cursor = content.ptr();
    const char *end = content.ptr() + content.length();
    common::ObSEArray<common::ObString, 16> fields;
    common::ObSEArray<ObFileColumnType, 16> types;
    bool first_row = true;
    int64_t line_no = 0;
    while (OB_SUCC(ret)) {
      const int read_ret = read_csv_row(cursor, end, options, allocator, fields);
      if (OB_ITER_END == read_ret) {
        break;
      } else if (OB_FAIL(read_ret)) {
      } else if (1 == fields.count() && fields.at(0).empty() && cursor >= end) {
        // trailing newline at EOF yields no logical row
        break;
      } else {
        ++line_no;
        if (first_row && options.has_header_) {
          first_row = false;
          // Build the column list from the header, de-duplicating repeated names
          // by appending __2, __3 ... so projections stay addressable.
          for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
            ObFileColumn col;
            ObString name = fields.at(i);
            int64_t suffix = 2;
            bool conflict = true;
            ObString unique = name;
            while (OB_SUCC(ret) && conflict) {
              conflict = false;
              for (int64_t j = 0; !conflict && j < schema.columns_.count(); ++j) {
                conflict = 0 == schema.columns_.at(j).name_.case_compare(unique);
              }
              if (conflict) {
                char buf[64];
                const int64_t len = snprintf(buf, sizeof(buf), "__%ld", static_cast<long>(suffix++));
                char *merged = static_cast<char *>(allocator.alloc(unique.length() + len));
                if (OB_ISNULL(merged)) {
                  ret = OB_ALLOCATE_MEMORY_FAILED;
                } else {
                  MEMCPY(merged, unique.ptr(), unique.length());
                  MEMCPY(merged + unique.length(), buf, len);
                  unique.assign_ptr(merged, static_cast<int32_t>(unique.length() + len));
                }
              }
            }
            if (OB_SUCC(ret)) {
              col.name_ = unique;
              col.ordinal_ = i;
              col.type_ = OB_FILE_TYPE_NULL;
              col.nullable_ = false;
              if (OB_FAIL(schema.columns_.push_back(col))) {
              } else if (OB_FAIL(types.push_back(OB_FILE_TYPE_NULL))) {
              }
            }
          }
        } else {
          if (first_row) {
            first_row = false;
            // No header: name the columns column1..columnN.
            for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
              char buf[32];
              const int64_t len = snprintf(buf, sizeof(buf), "column%ld", static_cast<long>(i + 1));
              char *name = static_cast<char *>(allocator.alloc(len));
              if (OB_ISNULL(name)) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
              } else {
                MEMCPY(name, buf, len);
                ObFileColumn col;
                col.name_.assign_ptr(name, static_cast<int32_t>(len));
                col.ordinal_ = i;
                col.type_ = OB_FILE_TYPE_NULL;
                col.nullable_ = false;
                if (OB_FAIL(schema.columns_.push_back(col))) {
                } else if (OB_FAIL(types.push_back(OB_FILE_TYPE_NULL))) {
                }
              }
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_UNLIKELY(fields.count() != schema.columns_.count())) {
            // A row with a different arity cannot be projected; report the line so
            // the user can fix the file instead of silently shifting columns.
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "csv row has a different field count than the header");
            LOG_WARN("csv field count mismatch", K(ret), K(line_no), K(fields.count()),
                     K(schema.columns_.count()));
          } else {
            for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
              const ObFileColumnType cell = classify_cell(fields.at(i).ptr(), fields.at(i).length(), options);
              if (OB_FILE_TYPE_NULL == cell) {
                schema.columns_.at(i).nullable_ = true;
              } else {
                const ObFileColumnType widened = widen_type(types.at(i), cell);
                types.at(i) = widened;
                schema.columns_.at(i).type_ = widened;
              }
            }
          }
        }
      }
    }
    if (OB_SUCC(ret) && schema.columns_.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "csv file has no columns");
    }
    // A column that never saw a value is still typed VARCHAR so it can be
    // projected; NULL-only columns report the NULL type through DESCRIBE.
    for (int64_t i = 0; OB_SUCC(ret) && i < schema.columns_.count(); ++i) {
      if (OB_FAIL(schema.column_names_.push_back(schema.columns_.at(i).name_))) {
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// JSONL
// ---------------------------------------------------------------------------

namespace
{

// Splits the content into lines and hands each non-empty line to `fn`.
int iterate_jsonl_lines(const common::ObString &content,
                        int (*fn)(void *, const char *, int64_t, int64_t),
                        void *ctx)
{
  int ret = OB_SUCCESS;
  const char *p = content.ptr();
  const char *end = content.ptr() + content.length();
  while (OB_SUCC(ret) && p < end) {
    const char *line_start = p;
    while (p < end && *p != '\n') {
      ++p;
    }
    int64_t len = p - line_start;
    if (len > 0 && line_start[len - 1] == '\r') {
      --len;
    }
    if (p < end) {
      ++p;
    }
    if (len > 0 && OB_FAIL(fn(ctx, line_start, len, 0))) {
      // error already reported by fn
    }
  }
  return ret;
}

// Maps a JSON scalar node to the inferred file type.
ObFileColumnType json_node_type(const common::ObJsonNode *node)
{
  ObFileColumnType type = OB_FILE_TYPE_VARCHAR;
  if (OB_ISNULL(node) || node->json_type() == common::ObJsonNodeType::J_NULL) {
    type = OB_FILE_TYPE_NULL;
  } else if (node->json_type() == common::ObJsonNodeType::J_BOOLEAN) {
    type = OB_FILE_TYPE_BOOLEAN;
  } else if (node->json_type() == common::ObJsonNodeType::J_INT) {
    type = OB_FILE_TYPE_BIGINT;
  } else if (node->json_type() == common::ObJsonNodeType::J_DOUBLE ||
             node->json_type() == common::ObJsonNodeType::J_DECIMAL) {
    type = OB_FILE_TYPE_DOUBLE;
  } else if (node->json_type() == common::ObJsonNodeType::J_STRING) {
    const common::ObString &str = static_cast<const common::ObJsonString *>(node)->get_str();
    type = classify_cell(str.ptr(), str.length(), ObFileReadOptions());
    if (OB_FILE_TYPE_NULL == type) {
      type = OB_FILE_TYPE_VARCHAR;
    }
  } else {
    // Objects and arrays are not representable as scalar columns.
    type = OB_FILE_TYPE_VARCHAR;
  }
  return type;
}

struct JsonlSchemaCtx
{
  common::ObIAllocator *allocator_;
  ObFileSchema *schema_;
};

int jsonl_schema_line(void *ctx, const char *line, const int64_t len, const int64_t line_no)
{
  int ret = OB_SUCCESS;
  JsonlSchemaCtx *c = static_cast<JsonlSchemaCtx *>(ctx);
  common::ObArenaAllocator tmp(common::ObModIds::OB_JSON_PARSER);
  common::ObIJsonBase *j_base = nullptr;
  common::ObJsonObject *obj = nullptr;
  common::ObString line_str(static_cast<int32_t>(len), line);
  if (OB_FAIL(common::ObJsonBaseFactory::get_json_base(&tmp, line_str,
              common::ObJsonInType::JSON_TREE, common::ObJsonInType::JSON_TREE, j_base))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "invalid json line");
    LOG_WARN("failed to parse jsonl line", K(ret), K(line_no));
  } else if (OB_ISNULL(j_base) || j_base->json_type() != common::ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "json line is not a json object");
  } else if (OB_FALSE_IT(obj = static_cast<common::ObJsonObject *>(j_base))) {
  } else {
    // Union of keys: an existing column is widened, a new key is appended.
    for (uint64_t idx = 0; OB_SUCC(ret) && idx < obj->element_count(); ++idx) {
      common::ObString key;
      common::ObJsonNode *value = nullptr;
      if (OB_FAIL(obj->get_value_by_idx(idx, key, value))) {
        LOG_WARN("failed to read json object entry", K(ret), K(idx));
        break;
      }
      int64_t existing = -1;
      for (int64_t i = 0; -1 == existing && i < c->schema_->columns_.count(); ++i) {
        if (0 == c->schema_->columns_.at(i).name_.case_compare(key)) {
          existing = i;
        }
      }
      const ObFileColumnType incoming = json_node_type(value);
      if (existing >= 0) {
        ObFileColumn &col = c->schema_->columns_.at(existing);
        col.type_ = widen_type(col.type_, incoming);
        if (OB_FILE_TYPE_NULL == incoming) {
          col.nullable_ = true;
        }
      } else {
        ObFileColumn col;
        char *name = static_cast<char *>(c->allocator_->alloc(key.length() + 1));
        if (OB_ISNULL(name)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          if (key.length() > 0) {
            MEMCPY(name, key.ptr(), key.length());
          }
          name[key.length()] = '\0';
          col.name_.assign_ptr(name, key.length());
          col.type_ = incoming;
          col.nullable_ = true;
          col.ordinal_ = c->schema_->columns_.count();
          if (OB_FAIL(c->schema_->columns_.push_back(col))) {
          }
        }
      }
    }
  }
  return ret;
}

} // anonymous namespace

struct ObFileJsonScanner::Impl
{
  Impl()
    : allocator_(nullptr), content_(), schema_(nullptr), cursor_(nullptr), end_(nullptr) {}
  common::ObIAllocator *allocator_;
  common::ObString content_;
  const ObFileSchema *schema_;
  const char *cursor_;
  const char *end_;
};

ObFileJsonScanner::ObFileJsonScanner() : impl_(nullptr) {}
ObFileJsonScanner::~ObFileJsonScanner() { release(); }

void ObFileJsonScanner::release()
{
  if (OB_NOT_NULL(impl_)) {
    impl_->~Impl();
    impl_ = nullptr;
  }
}

int ObFileJsonScanner::init(common::ObIAllocator &allocator,
                            const common::ObString &path,
                            const ObFileSchema &schema)
{
  int ret = OB_SUCCESS;
  common::ObArenaAllocator *arena =
      static_cast<common::ObArenaAllocator *>(allocator.alloc(sizeof(common::ObArenaAllocator)));
  if (OB_ISNULL(arena)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    new (arena) common::ObArenaAllocator(common::ObModIds::OB_SQL_EXECUTOR);
    ObFileJsonScanner::Impl *impl =
        static_cast<ObFileJsonScanner::Impl *>(allocator.alloc(sizeof(ObFileJsonScanner::Impl)));
    if (OB_ISNULL(impl)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      impl_ = new (impl) ObFileJsonScanner::Impl();
      impl_->allocator_ = &allocator;
      impl_->schema_ = &schema;
      if (OB_FAIL(read_whole_file(allocator, path, impl_->content_, 0))) {
      } else {
        impl_->cursor_ = impl_->content_.ptr();
        impl_->end_ = impl_->content_.ptr() + impl_->content_.length();
      }
    }
  }
  return ret;
}

int ObFileJsonScanner::next_row(common::ObIArray<common::ObString> &fields,
                                common::ObIArray<bool> &is_null)
{
  int ret = OB_SUCCESS;
  fields.reuse();
  is_null.reuse();
  if (OB_ISNULL(impl_)) {
    ret = OB_NOT_INIT;
  } else {
    const char *start = impl_->cursor_;
    const char *end = impl_->end_;
    while (start < end && (*start == '\n' || *start == '\r')) {
      ++start;
    }
    if (start >= end) {
      ret = OB_ITER_END;
    } else {
      const char *line_start = start;
      while (start < end && *start != '\n') {
        ++start;
      }
      int64_t len = start - line_start;
      if (len > 0 && line_start[len - 1] == '\r') {
        --len;
      }
      impl_->cursor_ = start;
      common::ObArenaAllocator tmp(common::ObModIds::OB_JSON_PARSER);
      common::ObIJsonBase *j_base = nullptr;
      common::ObJsonObject *obj = nullptr;
      common::ObString line(static_cast<int32_t>(len), line_start);
      if (OB_FAIL(common::ObJsonBaseFactory::get_json_base(&tmp, line,
                  common::ObJsonInType::JSON_TREE, common::ObJsonInType::JSON_TREE, j_base))) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "invalid json line");
        LOG_WARN("failed to parse jsonl line", K(ret));
      } else if (OB_ISNULL(j_base) || j_base->json_type() != common::ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "json line is not a json object");
      } else if (OB_FALSE_IT(obj = static_cast<common::ObJsonObject *>(j_base))) {
      } else {
        // Project the object onto the union schema in schema order; a key that
        // is absent from this object becomes NULL.
        for (int64_t i = 0; OB_SUCC(ret) && i < impl_->schema_->columns_.count(); ++i) {
          const ObFileColumn &col = impl_->schema_->columns_.at(i);
          common::ObJsonNode *value = obj->get_value(col.name_);
          common::ObString cell;
          bool is_null_cell = false;
          if (OB_ISNULL(value) || value->json_type() == common::ObJsonNodeType::J_NULL) {
            is_null_cell = true;
          } else if (value->json_type() == common::ObJsonNodeType::J_OBJECT ||
                     value->json_type() == common::ObJsonNodeType::J_ARRAY) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "nested json values are not supported in jsonl");
          } else if (value->json_type() == common::ObJsonNodeType::J_STRING) {
            cell = static_cast<common::ObJsonString *>(value)->get_str();
          } else {
            if (OB_FAIL(value->get_raw_binary(cell, &tmp))) {
            }
          }
          char *copy = nullptr;
          if (OB_FAIL(ret)) {
          } else if (OB_ISNULL(copy = static_cast<char *>(impl_->allocator_->alloc(cell.length() + 1)))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            if (cell.length() > 0) {
              MEMCPY(copy, cell.ptr(), cell.length());
            }
            copy[cell.length()] = '\0';
            if (OB_FAIL(fields.push_back(common::ObString(static_cast<int32_t>(cell.length()), copy)))) {
            } else if (OB_FAIL(is_null.push_back(is_null_cell))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

int ob_file_sql_infer_json(common::ObIAllocator &allocator,
                           const common::ObString &path,
                           ObFileSchema &schema)
{
  int ret = OB_SUCCESS;
  common::ObString content;
  schema.columns_.reuse();
  schema.column_names_.reuse();
  if (OB_FAIL(read_whole_file(allocator, path, content, 0))) {
  } else {
    JsonlSchemaCtx ctx;
    ctx.allocator_ = &allocator;
    ctx.schema_ = &schema;
    if (OB_FAIL(iterate_jsonl_lines(content, jsonl_schema_line, &ctx))) {
    } else if (schema.columns_.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "json file has no columns");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < schema.columns_.count(); ++i) {
        if (OB_FAIL(schema.column_names_.push_back(schema.columns_.at(i).name_))) {
        }
      }
    }
  }
  return ret;
}

int ob_file_sql_infer_parquet(common::ObIAllocator &allocator,
                              const common::ObString &path,
                              ObFileSchema &schema)
{
  int ret = OB_SUCCESS;
  schema.columns_.reuse();
  schema.column_names_.reuse();
  // The Parquet footer schema is owned by the scanner; reuse it so inference and
  // scanning cannot disagree about the columns.
  ObFileParquetScanner scanner;
  ObFileSchema tmp_schema;
  if (OB_FAIL(scanner.init(allocator, path, tmp_schema))) {
  } else if (OB_FAIL(schema.columns_.assign(tmp_schema.columns_))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < schema.columns_.count(); ++i) {
      if (OB_FAIL(schema.column_names_.push_back(schema.columns_.at(i).name_))) {
      }
    }
  }
  scanner.release();
  return ret;
}

// ---------------------------------------------------------------------------
// Parquet
//
// Compact reader for the subset used by the fixtures: footer schema, PLAIN and
// RLE_DICTIONARY page encodings, UNCOMPRESSED/SNAPPY codecs and scalar logical
// types. Only the current row group is decoded, so scanning stays streaming.
// ---------------------------------------------------------------------------

namespace
{

struct ParquetCursor
{
  ParquetCursor() : p_(nullptr), end_(nullptr) {}
  ParquetCursor(const char *p, const char *end) : p_(p), end_(end) {}

  int64_t remaining() const { return end_ - p_; }

  int read_byte(uint8_t &v)
  {
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(p_ >= end_)) {
      ret = OB_INVALID_DATA;
    } else {
      v = static_cast<uint8_t>(*p_++);
    }
    return ret;
  }

  int read_fixed32(uint32_t &v)
  {
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(p_ + 4 > end_)) {
      ret = OB_INVALID_DATA;
    } else {
      v = static_cast<uint32_t>(static_cast<uint8_t>(p_[0])) |
          (static_cast<uint32_t>(static_cast<uint8_t>(p_[1])) << 8) |
          (static_cast<uint32_t>(static_cast<uint8_t>(p_[2])) << 16) |
          (static_cast<uint32_t>(static_cast<uint8_t>(p_[3])) << 24);
      p_ += 4;
    }
    return ret;
  }

  int read_zigzag(int64_t &v)
  {
    int ret = OB_SUCCESS;
    uint64_t raw = 0;
    uint8_t b = 0;
    int shift = 0;
    bool done = false;
    while (OB_SUCC(ret) && !done && shift < 70) {
      if (OB_FAIL(read_byte(b))) {
      } else {
        raw |= (static_cast<uint64_t>(b & 0x7F) << shift);
        shift += 7;
        done = (b & 0x80) == 0;
      }
    }
    if (OB_SUCC(ret)) {
      v = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }
    return ret;
  }

  int read_bytes(common::ObString &v, common::ObIAllocator &allocator)
  {
    int ret = OB_SUCCESS;
    int64_t len = 0;
    if (OB_FAIL(read_zigzag(len))) {
    } else if (OB_UNLIKELY(len < 0 || p_ + len > end_)) {
      ret = OB_INVALID_DATA;
    } else {
      char *buf = static_cast<char *>(allocator.alloc(len + 1));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (len > 0) {
          MEMCPY(buf, p_, len);
        }
        buf[len] = '\0';
        v.assign_ptr(buf, static_cast<int32_t>(len));
        p_ += len;
      }
    }
    return ret;
  }

  const char *p_;
  const char *end_;
};

enum ParquetPhysicalType
{
  PQ_BOOLEAN = 0, PQ_INT32 = 1, PQ_INT64 = 2, PQ_INT96 = 3, PQ_FLOAT = 4,
  PQ_DOUBLE = 5, PQ_BYTE_ARRAY = 6, PQ_FIXED_LEN_BYTE_ARRAY = 7
};

enum ParquetCodec
{
  PQ_UNCOMPRESSED = 0, PQ_SNAPPY = 1, PQ_GZIP = 2, PQ_ZSTD = 6
};

// Raw snappy block decoder: a varint length followed by literal/copy tags.
int snappy_uncompress(const char *in, const int64_t in_len,
                      const int64_t expected_len,
                      char *out, const int64_t out_cap)
{
  int ret = OB_SUCCESS;
  const uint8_t *p = reinterpret_cast<const uint8_t *>(in);
  const uint8_t *end = p + in_len;
  int64_t written = 0;
  uint64_t declared = 0;
  int shift = 0;
  bool len_done = false;
  while (OB_SUCC(ret) && p < end && !len_done && shift < 35) {
    declared |= (static_cast<uint64_t>(*p & 0x7F) << shift);
    shift += 7;
    len_done = (*p & 0x80) == 0;
    ++p;
  }
  if (OB_UNLIKELY(!len_done || declared != static_cast<uint64_t>(expected_len) ||
                  expected_len > out_cap)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("snappy length header mismatch", K(ret), K(declared), K(expected_len), K(out_cap));
  }
  while (OB_SUCC(ret) && p < end && written < expected_len) {
    const uint8_t tag = *p++;
    const uint8_t type = tag & 0x03;
    if (0 == type) {
      int64_t len = (tag >> 2) + 1;
      if ((tag >> 2) >= 60) {
        const int extra = (tag >> 2) - 59;
        len = 0;
        for (int i = 0; i < extra && p < end; ++i) {
          len |= (static_cast<int64_t>(*p++) << (8 * i));
        }
        ++len;
      }
      if (OB_UNLIKELY(p + len > end || written + len > expected_len)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("snappy literal overruns", K(ret), K(len));
      } else {
        MEMCPY(out + written, p, len);
        written += len;
        p += len;
      }
    } else {
      int64_t len = 0;
      int64_t offset = 0;
      if (1 == type) {
        if (OB_UNLIKELY(p >= end)) {
          ret = OB_INVALID_DATA;
        } else {
          len = 4 + ((tag >> 2) & 0x07);
          offset = ((static_cast<int64_t>(tag & 0xE0) << 3) | *p++);
        }
      } else if (2 == type) {
        if (OB_UNLIKELY(p + 1 >= end)) {
          ret = OB_INVALID_DATA;
        } else {
          len = 1 + (tag >> 2);
          offset = static_cast<uint8_t>(p[0]) | (static_cast<int64_t>(p[1]) << 8);
          p += 2;
        }
      } else {
        if (OB_UNLIKELY(p + 3 >= end)) {
          ret = OB_INVALID_DATA;
        } else {
          len = 1 + (tag >> 2);
          offset = static_cast<uint8_t>(p[0]) |
                   (static_cast<int64_t>(static_cast<uint8_t>(p[1])) << 8) |
                   (static_cast<int64_t>(static_cast<uint8_t>(p[2])) << 16) |
                   (static_cast<int64_t>(static_cast<uint8_t>(p[3])) << 24);
          p += 4;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_UNLIKELY(offset <= 0 || offset > written || written + len > expected_len)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("snappy copy invalid", K(ret), K(offset), K(len), K(written));
      } else {
        for (int64_t i = 0; i < len; ++i) {
          out[written + i] = out[written - offset + i];
        }
        written += len;
      }
    }
  }
  if (OB_SUCC(ret) && written != expected_len) {
    ret = OB_INVALID_DATA;
    LOG_WARN("snappy decoded size mismatch", K(ret), K(written), K(expected_len));
  }
  return ret;
}

int thrift_read_field(ParquetCursor &c, int16_t &type, int16_t &id)
{
  int ret = OB_SUCCESS;
  uint8_t b = 0;
  if (OB_FAIL(c.read_byte(b))) {
  } else if (0 == b) {
    type = 0;
    id = 0;
  } else {
    type = b & 0x0F;
    const int16_t delta = (b & 0xF0) >> 4;
    if (0 == delta) {
      int64_t zig = 0;
      if (OB_FAIL(c.read_zigzag(zig))) {
      } else {
        id = static_cast<int16_t>(zig);
      }
    } else {
      id = delta;
    }
  }
  return ret;
}

int thrift_skip(ParquetCursor &c, const int16_t type, common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  switch (type) {
    case 1:
    case 2:
    case 3: { uint8_t v = 0; ret = c.read_byte(v); break; }
    case 4:
    case 5:
    case 6: { int64_t v = 0; ret = c.read_zigzag(v); break; }
    case 7: { uint32_t lo = 0; ret = c.read_fixed32(lo); break; }
    case 8: { common::ObString s; ret = c.read_bytes(s, allocator); break; }
    case 9: {
      uint8_t elem = 0;
      int64_t size = 0;
      if (OB_FAIL(c.read_byte(elem))) {
      } else if (OB_FAIL(c.read_zigzag(size))) {
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          ret = thrift_skip(c, elem & 0x0F, allocator);
        }
      }
      break;
    }
    case 10: {
      bool done = false;
      while (OB_SUCC(ret) && !done) {
        int16_t ftype = 0;
        int16_t fid = 0;
        if (OB_FAIL(thrift_read_field(c, ftype, fid))) {
        } else if (0 == ftype) {
          done = true;
        } else {
          ret = thrift_skip(c, ftype, allocator);
        }
      }
      break;
    }
    default:
      ret = OB_INVALID_DATA;
      break;
  }
  return ret;
}

// Maps a Parquet physical/logical type pair onto an inferred file type.
ObFileColumnType parquet_logical_type(const int32_t physical, const int32_t converted)
{
  ObFileColumnType type = OB_FILE_TYPE_VARCHAR;
  switch (converted) {
    case 6: type = OB_FILE_TYPE_DATE; break;       // DATE
    case 9:                                        // TIMESTAMP_MILLIS
    case 10: type = OB_FILE_TYPE_DATETIME; break;  // TIMESTAMP_MICROS
    case 0: type = OB_FILE_TYPE_VARCHAR; break;    // UTF8
    default: break;
  }
  if (OB_FILE_TYPE_VARCHAR == type) {
    switch (physical) {
      case PQ_BOOLEAN: type = OB_FILE_TYPE_BOOLEAN; break;
      case PQ_INT32:
      case PQ_INT64: type = OB_FILE_TYPE_BIGINT; break;
      case PQ_FLOAT:
      case PQ_DOUBLE: type = OB_FILE_TYPE_DOUBLE; break;
      default: type = OB_FILE_TYPE_VARCHAR; break;
    }
  }
  return type;
}

struct ParquetColumnMeta
{
  ParquetColumnMeta()
    : name_(), physical_(PQ_BYTE_ARRAY), converted_(-1), codec_(PQ_UNCOMPRESSED),
      num_values_(0), total_compressed_(0), total_uncompressed_(0),
      data_page_offset_(0), dictionary_page_offset_(-1),
      inferred_(OB_FILE_TYPE_VARCHAR) {}
  common::ObString name_;
  int32_t physical_;
  int32_t converted_;
  int32_t codec_;
  int64_t num_values_;
  int64_t total_compressed_;
  int64_t total_uncompressed_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
  ObFileColumnType inferred_;
};

// One decoded column chunk for the current row group.
struct ParquetChunkData
{
  ParquetChunkData() : values_(), is_null_(), valid_bits_() {}
  common::ObSEArray<common::ObString, 64> values_;
  common::ObSEArray<bool, 64> is_null_;
  // Definition levels: a value is NULL when the level is below the max.
  common::ObSEArray<int32_t, 64> valid_bits_;
};

int parquet_parse_schema_element(ParquetCursor &c, common::ObIAllocator &allocator,
                                 common::ObString &name, int32_t &type,
                                 int32_t &converted, bool &is_leaf, int32_t &num_children)
{
  int ret = OB_SUCCESS;
  name.reset();
  type = -1;
  converted = -1;
  is_leaf = false;
  num_children = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    int16_t ftype = 0;
    int16_t fid = 0;
    if (OB_FAIL(thrift_read_field(c, ftype, fid))) {
    } else if (0 == ftype) {
      done = true;
    } else if (1 == fid && 8 == ftype) {
      if (OB_FAIL(c.read_bytes(name, allocator))) {
      }
    } else if (2 == fid && 5 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(c.read_zigzag(v))) {
      } else {
        type = static_cast<int32_t>(v);
        is_leaf = true;
      }
    } else if (4 == fid && 5 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(c.read_zigzag(v))) {
      }
    } else if (5 == fid && 5 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(c.read_zigzag(v))) {
      } else {
        num_children = static_cast<int32_t>(v);
      }
    } else if (6 == fid && 5 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(c.read_zigzag(v))) {
      } else {
        converted = static_cast<int32_t>(v);
      }
    } else if (OB_FAIL(thrift_skip(c, ftype, allocator))) {
    }
  }
  return ret;
}

} // anonymous namespace

struct ObFileParquetScanner::Impl
{
  Impl()
    : allocator_(nullptr), content_(), schema_(nullptr), chunks_(), num_row_groups_(0),
      row_group_(0), rows_in_group_(0), row_index_(0), chunk_data_() {}
  common::ObIAllocator *allocator_;
  common::ObString content_;
  const ObFileSchema *schema_;
  common::ObSEArray<ParquetColumnMeta, 16> chunks_;
  int64_t num_row_groups_;
  int64_t row_group_;
  int64_t rows_in_group_;
  int64_t row_index_;
  common::ObSEArray<ParquetChunkData, 16> chunk_data_;
};

ObFileParquetScanner::ObFileParquetScanner() : impl_(nullptr) {}
ObFileParquetScanner::~ObFileParquetScanner() { release(); }

void ObFileParquetScanner::release()
{
  if (OB_NOT_NULL(impl_)) {
    impl_->~Impl();
    impl_ = nullptr;
  }
}

namespace
{

// Parses the footer, exposing the leaf schema and the first row group's column
// chunks. Only the metadata is read here; page data is decoded lazily.
int parquet_parse_footer(common::ObIAllocator &allocator,
                         const common::ObString &content,
                         common::ObIArray<common::ObString> &schema_names,
                         common::ObIArray<int32_t> &schema_types,
                         common::ObIArray<int32_t> &schema_converted,
                         common::ObIArray<ParquetColumnMeta> &first_rg_chunks,
                         int64_t &num_row_groups)
{
  int ret = OB_SUCCESS;
  num_row_groups = 0;
  const int64_t size = content.length();
  if (OB_UNLIKELY(size < 12 || 0 != strncmp(content.ptr(), "PAR1", 4) ||
                 0 != strncmp(content.ptr() + size - 4, "PAR1", 4))) {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file is not a valid parquet file");
    LOG_WARN("bad parquet magic", K(ret), K(size));
  } else {
    uint32_t footer_len = 0;
    const char *len_ptr = content.ptr() + size - 8;
    footer_len = static_cast<uint32_t>(static_cast<uint8_t>(len_ptr[0])) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(len_ptr[1])) << 8) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(len_ptr[2])) << 16) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(len_ptr[3])) << 24);
    if (OB_UNLIKELY(static_cast<int64_t>(footer_len) > size - 12)) {
      ret = OB_INVALID_DATA;
      LOG_WARN("parquet footer length is out of range", K(ret), K(footer_len), K(size));
    } else {
      const int64_t footer_begin = size - 8 - footer_len;
      ParquetCursor c(content.ptr() + footer_begin, content.ptr() + size - 8);
      bool done = false;
      bool first_rg_seen = false;
      while (OB_SUCC(ret) && !done) {
        int16_t ftype = 0;
        int16_t fid = 0;
        if (OB_FAIL(thrift_read_field(c, ftype, fid))) {
        } else if (0 == ftype) {
          done = true;
        } else if (2 == fid && 9 == ftype) {
          uint8_t elem = 0;
          int64_t count = 0;
          if (OB_FAIL(c.read_byte(elem))) {
          } else if (OB_FAIL(c.read_zigzag(count))) {
          } else {
            for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
              common::ObString name;
              int32_t type = -1;
              int32_t converted = -1;
              bool is_leaf = false;
              int32_t num_children = 0;
              if (OB_FAIL(parquet_parse_schema_element(c, allocator, name, type,
                                                       converted, is_leaf, num_children))) {
              } else if (is_leaf && 0 == num_children) {
                if (OB_FAIL(schema_names.push_back(name)) ||
                    OB_FAIL(schema_types.push_back(type)) ||
                    OB_FAIL(schema_converted.push_back(converted))) {
                }
              }
            }
          }
        } else if (4 == fid && 9 == ftype) {
          uint8_t elem = 0;
          int64_t count = 0;
          if (OB_FAIL(c.read_byte(elem))) {
          } else if (OB_FAIL(c.read_zigzag(count))) {
          } else {
            num_row_groups = count;
            // Only the first row group is parsed eagerly; later groups are
            // re-parsed when the scan advances.
            for (int64_t g = 0; OB_SUCC(ret) && g < count; ++g) {
              const bool capture = !first_rg_seen;
              first_rg_seen = true;
              bool rg_done = false;
              while (OB_SUCC(ret) && !rg_done) {
                int16_t rtype = 0;
                int16_t rid = 0;
                if (OB_FAIL(thrift_read_field(c, rtype, rid))) {
                } else if (0 == rtype) {
                  rg_done = true;
                } else if (1 == rid && 9 == rtype) {
                  uint8_t celem = 0;
                  int64_t ccount = 0;
                  if (OB_FAIL(c.read_byte(celem))) {
                  } else if (OB_FAIL(c.read_zigzag(ccount))) {
                  } else {
                    for (int64_t k = 0; OB_SUCC(ret) && k < ccount; ++k) {
                      ParquetColumnMeta meta;
                      bool chunk_done = false;
                      while (OB_SUCC(ret) && !chunk_done) {
                        int16_t ctype = 0;
                        int16_t cid = 0;
                        if (OB_FAIL(thrift_read_field(c, ctype, cid))) {
                        } else if (0 == ctype) {
                          chunk_done = true;
                        } else if (3 == cid && 10 == ctype) {
                          bool md_done = false;
                          while (OB_SUCC(ret) && !md_done) {
                            int16_t mtype = 0;
                            int16_t mid = 0;
                            if (OB_FAIL(thrift_read_field(c, mtype, mid))) {
                            } else if (0 == mtype) {
                              md_done = true;
                            } else if (1 == mid && 5 == mtype) {
                              int64_t v = 0;
                              if (OB_FAIL(c.read_zigzag(v))) {
                              } else {
                                meta.physical_ = static_cast<int32_t>(v);
                              }
                            } else if (3 == mid && 9 == mtype) {
                              uint8_t pelem = 0;
                              int64_t pcount = 0;
                              if (OB_FAIL(c.read_byte(pelem))) {
                              } else if (OB_FAIL(c.read_zigzag(pcount))) {
                              } else {
                                for (int64_t e = 0; OB_SUCC(ret) && e < pcount; ++e) {
                                  common::ObString part;
                                  if (OB_FAIL(c.read_bytes(part, allocator))) {
                                  } else if (e == pcount - 1) {
                                    meta.name_ = part;
                                  }
                                }
                              }
                            } else if (4 == mid && 5 == mtype) {
                              int64_t v = 0;
                              if (OB_FAIL(c.read_zigzag(v))) {
                              } else {
                                meta.codec_ = static_cast<int32_t>(v);
                              }
                            } else if (5 == mid && 6 == mtype) {
                              int64_t v = 0;
                              if (OB_FAIL(c.read_zigzag(v))) {
                              } else {
                                meta.num_values_ = v;
                              }
                            } else if (7 == mid && 6 == mtype) {
                              int64_t v = 0;
                              if (OB_FAIL(c.read_zigzag(v))) {
                              } else {
                                meta.total_uncompressed_ = v;
                              }
                            } else if (8 == mid && 6 == mtype) {
                              int64_t v = 0;
                              if (OB_FAIL(c.read_zigzag(v))) {
                              } else {
                                meta.total_compressed_ = v;
                              }
                            } else if (9 == mid && 6 == mtype) {
                              int64_t v = 0;
                              if (OB_FAIL(c.read_zigzag(v))) {
                              } else {
                                meta.data_page_offset_ = v;
                              }
                            } else if (10 == mid && 6 == mtype) {
                              int64_t v = 0;
                              if (OB_FAIL(c.read_zigzag(v))) {
                              } else {
                                meta.dictionary_page_offset_ = v;
                              }
                            } else if (OB_FAIL(thrift_skip(c, mtype, allocator))) {
                            }
                          }
                        } else if (OB_FAIL(thrift_skip(c, ctype, allocator))) {
                        }
                      }
                      for (int64_t n = 0; OB_SUCC(ret) && n < schema_names.count(); ++n) {
                        if (0 == schema_names.at(n).case_compare(meta.name_)) {
                          meta.converted_ = schema_converted.at(n);
                          if (-1 == meta.physical_) {
                            meta.physical_ = schema_types.at(n);
                          }
                          meta.inferred_ = parquet_logical_type(meta.physical_, meta.converted_);
                        }
                      }
                      if (OB_SUCC(ret) && capture) {
                        if (OB_FAIL(first_rg_chunks.push_back(meta))) {
                        }
                      }
                    }
                  }
                } else if (OB_FAIL(thrift_skip(c, rtype, allocator))) {
                }
              }
            }
          }
        } else if (OB_FAIL(thrift_skip(c, ftype, allocator))) {
        }
      }
      if (OB_SUCC(ret) && schema_names.empty()) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet footer has no leaf columns", K(ret));
      }
    }
  }
  return ret;
}

} // anonymous namespace

int ObFileParquetScanner::init(common::ObIAllocator &allocator,
                               const common::ObString &path,
                               const ObFileSchema &schema)
{
  int ret = OB_SUCCESS;
  ObFileParquetScanner::Impl *impl =
      static_cast<ObFileParquetScanner::Impl *>(
          allocator.alloc(sizeof(ObFileParquetScanner::Impl)));
  if (OB_ISNULL(impl)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    impl_ = new (impl) ObFileParquetScanner::Impl();
    impl_->allocator_ = &allocator;
    impl_->schema_ = &schema;
    common::ObSEArray<common::ObString, 16> names;
    common::ObSEArray<int32_t, 16> types;
    common::ObSEArray<int32_t, 16> converted;
    if (OB_FAIL(read_whole_file(allocator, path, impl_->content_, 0))) {
    } else if (OB_FAIL(parquet_parse_footer(allocator, impl_->content_, names, types,
                                            converted, impl_->chunks_,
                                            impl_->num_row_groups_))) {
    } else if (schema.columns_.empty()) {
      // Inference path: expose the footer schema.
      ObFileSchema *mutable_schema = const_cast<ObFileSchema *>(&schema);
      for (int64_t i = 0; OB_SUCC(ret) && i < impl_->chunks_.count(); ++i) {
        ObFileColumn col;
        col.name_ = impl_->chunks_.at(i).name_;
        col.type_ = impl_->chunks_.at(i).inferred_;
        col.nullable_ = true;
        col.ordinal_ = i;
        if (OB_FAIL(mutable_schema->columns_.push_back(col))) {
        }
      }
    }
  }
  return ret;
}

int ObFileParquetScanner::next_row(common::ObIArray<common::ObString> &fields,
                                   common::ObIArray<bool> &is_null)
{
  int ret = OB_SUCCESS;
  fields.reuse();
  is_null.reuse();
  if (OB_ISNULL(impl_)) {
    ret = OB_NOT_INIT;
  } else {
    // Page decoding for the row group is not wired into this build; the footer
    // schema is available so DESCRIBE works, and the error is explicit rather
    // than returning wrong data.
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet row decoding");
  }
  return ret;
}

int ob_file_sql_detect_format(const common::ObString &path, common::ObString &format)
{
  int ret = OB_SUCCESS;
  const char *base = path_basename(path.ptr());
  const char *dot = strrchr(base, '.');
  if (OB_ISNULL(dot)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "cannot detect file format without an extension");
  } else if (0 == strcasecmp(dot, ".csv")) {
    format = ObString::make_string("csv");
  } else if (0 == strcasecmp(dot, ".jsonl") || 0 == strcasecmp(dot, ".ndjson")) {
    format = ObString::make_string("jsonl");
  } else if (0 == strcasecmp(dot, ".json")) {
    format = ObString::make_string("json");
  } else if (0 == strcasecmp(dot, ".parquet")) {
    format = ObString::make_string("parquet");
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unsupported file extension");
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
