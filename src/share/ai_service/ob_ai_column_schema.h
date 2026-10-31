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

#pragma once

#include <stdlib.h>
#include "lib/ob_define.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/utility/ob_print_utils.h"
#include "share/schema/ob_column_schema.h"

namespace oceanbase
{
namespace share
{

// An AI column is a user visible result column plus three generated hidden columns.
//   <name>                     : the result column, AI_COLUMN_FLAG, is_hidden = 0
//   __ai_state_<name>          : runtime state of the column value (PENDING/READY/FAILED)
//   __ai_request_<name>        : identity/version of the request that produced the value
//   __ai_result_spec_<name>    : the result spec, its comment carries ObAiColumnSpec text
// The hidden columns are never visible to the user, so the spec of an AI column is
// carried by the comment of its result-spec column: that keeps the whole definition
// inside the table schema (i.e. inside __all_column) and needs no extra runtime state.
const char *const AI_STATE_COLUMN_PREFIX = "__ai_state_";
const char *const AI_REQUEST_COLUMN_PREFIX = "__ai_request_";
const char *const AI_RESULT_SPEC_COLUMN_PREFIX = "__ai_result_spec_";

const int64_t AI_STATE_COLUMN_LENGTH = 32;
const int64_t AI_REQUEST_COLUMN_LENGTH = 64;
const int64_t AI_RESULT_SPEC_COLUMN_LENGTH = 1024;

// Result contract appended to the normalized expression of a JSON typed AI column,
// so that the model returns a value that can be stored in a JSON column directly.
const char *const AI_JSON_RESULT_CONTRACT =
    "Return exactly one valid JSON value and nothing else.";

// Same idea for an integer typed AI column: a free text answer would be rejected by the
// column type check (OB_ERR_TRUNCATED_WRONG_VALUE_FOR_FIELD) when the worker writes it back.
const char *const AI_INTEGER_RESULT_CONTRACT =
    "Return exactly one base-10 integer and nothing else.";

// Decoded view of an AI column spec. All ObString fields point into the spec text
// buffer that was passed to parse_ai_column_spec_text().
class ObAiColumnInfo
{
public:
  ObAiColumnInfo() { reset(); }
  ~ObAiColumnInfo() = default;
  void reset()
  {
    ai_expr_.reset();
    normalized_expr_.reset();
    ai_function_.reset();
    result_type_.reset();
    sync_mode_.reset();
    dep_columns_.reset();
    max_retries_ = 0;
  }
  bool is_valid() const { return !ai_expr_.empty() && !ai_function_.empty(); }
  TO_STRING_KV(K_(ai_expr), K_(ai_function), K_(result_type), K_(sync_mode),
               K_(dep_columns), K_(max_retries));
public:
  common::ObString ai_expr_;         // expression exactly as written by the user
  common::ObString normalized_expr_; // expression after AI column normalization
  common::ObString ai_function_;     // lowercase AI function name, e.g. "ai_complete"
  common::ObString result_type_;     // "json", "vector" or "text"
  common::ObString sync_mode_;       // "async" or "sync"
  // Columns referenced by the AI expression, comma separated. DML uses them to decide
  // whether an UPDATE has to invalidate the AI column.
  common::ObString dep_columns_;
  int64_t max_retries_;
};

// Build the name of a generated AI hidden column from the result column name.
inline int build_ai_hidden_column_name(
    common::ObIAllocator &alloc,
    const char *prefix,
    const common::ObString &column_name,
    common::ObString &name)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(prefix) || column_name.empty()) {
    ret = OB_INVALID_ARGUMENT;
    SHARE_LOG(WARN, "invalid argument", KR(ret), KP(prefix), K(column_name));
  } else {
    const int64_t prefix_len = static_cast<int64_t>(strlen(prefix));
    int64_t name_len = prefix_len + static_cast<int64_t>(column_name.length());
    if (name_len > OB_MAX_COLUMN_NAME_LENGTH) {
      name_len = OB_MAX_COLUMN_NAME_LENGTH;
    }
    char *buf = static_cast<char *>(alloc.alloc(name_len));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      SHARE_LOG(WARN, "fail to allocate hidden column name", KR(ret), K(name_len));
    } else {
      MEMCPY(buf, prefix, prefix_len);
      MEMCPY(buf + prefix_len, column_name.ptr(), name_len - prefix_len);
      name.assign_ptr(buf, static_cast<int32_t>(name_len));
    }
  }
  return ret;
}

// Escape a spec field value so that it can be stored in a "key=value\n" text form.
inline int append_escaped(common::ObString &dst, int64_t &pos, const common::ObString &src)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < src.length(); ++i) {
    const char c = src.ptr()[i];
    if ('\\' == c || '\n' == c) {
      if (pos + 2 > dst.length()) {
        ret = OB_BUF_NOT_ENOUGH;
      } else {
        dst.ptr()[pos++] = '\\';
        dst.ptr()[pos++] = ('\\' == c) ? '\\' : 'n';
      }
    } else if (pos + 1 > dst.length()) {
      ret = OB_BUF_NOT_ENOUGH;
    } else {
      dst.ptr()[pos++] = c;
    }
  }
  return ret;
}

// Append raw (not escaped) bytes; used for the "=" and "\n" separators of the spec text.
inline int append_raw(common::ObString &dst, int64_t &pos, const char *src)
{
  int ret = OB_SUCCESS;
  const int64_t len = static_cast<int64_t>(strlen(src));
  if (pos + len > dst.length()) {
    ret = OB_BUF_NOT_ENOUGH;
  } else {
    MEMCPY(dst.ptr() + pos, src, len);
    pos += len;
  }
  return ret;
}

// Serialize the spec of an AI column into the text form stored in the comment of the
// result-spec hidden column. The text is written into memory allocated from `alloc`.
inline int build_ai_column_spec_text(
    common::ObIAllocator &alloc,
    const common::ObString &ai_expr,
    const common::ObString &normalized_expr,
    const common::ObString &ai_function,
    const common::ObString &result_type,
    const common::ObString &sync_mode,
    const common::ObString &dep_columns,
    const int64_t max_retries,
    common::ObString &spec_text)
{
  int ret = OB_SUCCESS;
  // every field may be escaped at most twice its length, plus key/=/\n overhead
  const int64_t buf_len = 2 * (ai_expr.length() + normalized_expr.length()
                               + ai_function.length() + result_type.length()
                               + sync_mode.length() + dep_columns.length())
                          + 256;
  int64_t pos = 0;
  char *buf = static_cast<char *>(alloc.alloc(buf_len));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    spec_text.assign_ptr(buf, static_cast<int32_t>(buf_len));
    const char *const keys[] = {
        "ai_expr", "normalized_expr", "ai_function", "result_type", "sync_mode", "dep_columns"};
    const common::ObString *values[] = {
        &ai_expr, &normalized_expr, &ai_function, &result_type, &sync_mode, &dep_columns};
    for (int64_t i = 0; OB_SUCC(ret) && i < ARRAYSIZEOF(keys); ++i) {
      if (OB_FAIL(append_escaped(spec_text, pos, common::ObString::make_string(keys[i])))
          || OB_FAIL(append_raw(spec_text, pos, "="))
          || OB_FAIL(append_escaped(spec_text, pos, *values[i]))
          || OB_FAIL(append_raw(spec_text, pos, "\n"))) {
      }
    }
    if (OB_SUCC(ret)) {
      char tail[64] = {'\0'};
      int64_t tail_pos = 0;
      if (OB_FAIL(databuff_printf(tail, sizeof(tail), tail_pos, "max_retries=%ld\n", max_retries))) {
      } else if (pos + tail_pos > buf_len) {
        ret = OB_BUF_NOT_ENOUGH;
      } else {
        MEMCPY(buf + pos, tail, tail_pos);
        pos += tail_pos;
      }
    }
    if (OB_SUCC(ret)) {
      spec_text.assign_ptr(buf, static_cast<int32_t>(pos));
    }
  }
  return ret;
}

// Remove the escaping done by append_escaped(); the result is allocated from `alloc`.
inline int unescape_spec_value(
    common::ObIAllocator &alloc,
    const common::ObString &src,
    common::ObString &dst)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  if (src.empty()) {
    dst.reset();
  } else if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(src.length())))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int64_t pos = 0;
    for (int64_t i = 0; i < src.length(); ++i) {
      if ('\\' == src.ptr()[i] && i + 1 < src.length()) {
        ++i;
        buf[pos++] = ('n' == src.ptr()[i]) ? '\n' : src.ptr()[i];
      } else {
        buf[pos++] = src.ptr()[i];
      }
    }
    dst.assign_ptr(buf, static_cast<int32_t>(pos));
  }
  return ret;
}

// Parse the text form produced by build_ai_column_spec_text().
inline int parse_ai_column_spec_text(
    common::ObIAllocator &alloc,
    const common::ObString &spec_text,
    ObAiColumnInfo &info)
{
  int ret = OB_SUCCESS;
  info.reset();
  common::ObString rest = spec_text;
  while (OB_SUCC(ret) && !rest.empty()) {
    int64_t line_len = 0;
    while (line_len < rest.length() && '\n' != rest.ptr()[line_len]) {
      ++line_len;
    }
    common::ObString line;
    line.assign_ptr(rest.ptr(), static_cast<int32_t>(line_len));
    if (line_len == rest.length()) {
      rest.reset();
    } else {
      rest.assign_ptr(rest.ptr() + line_len + 1, static_cast<int32_t>(rest.length() - line_len - 1));
    }
    if (line.empty()) {
      // skip blank line
    } else {
      int64_t eq_pos = 0;
      while (eq_pos < line.length() && '=' != line.ptr()[eq_pos]) {
        ++eq_pos;
      }
      if (eq_pos >= line.length()) {
        ret = OB_INVALID_DATA;
        SHARE_LOG(WARN, "invalid ai column spec line", KR(ret), K(line));
      } else {
        common::ObString key;
        common::ObString raw_value;
        common::ObString value;
        key.assign_ptr(line.ptr(), static_cast<int32_t>(eq_pos));
        raw_value.assign_ptr(line.ptr() + eq_pos + 1, static_cast<int32_t>(line.length() - eq_pos - 1));
        if (OB_FAIL(unescape_spec_value(alloc, raw_value, value))) {
        } else if (key == "ai_expr") {
          info.ai_expr_ = value;
        } else if (key == "normalized_expr") {
          info.normalized_expr_ = value;
        } else if (key == "ai_function") {
          info.ai_function_ = value;
        } else if (key == "result_type") {
          info.result_type_ = value;
        } else if (key == "sync_mode") {
          info.sync_mode_ = value;
        } else if (key == "dep_columns") {
          info.dep_columns_ = value;
        } else if (key == "max_retries") {
          char buf[32] = {'\0'};
          const int64_t len = min(static_cast<int64_t>(value.length()),
                                  static_cast<int64_t>(sizeof(buf) - 1));
          MEMCPY(buf, value.ptr(), len);
          char *end = NULL;
          info.max_retries_ = strtoll(buf, &end, 10);
        } else {
          // unknown key, ignore for forward compatibility
        }
      }
    }
  }
  if (OB_SUCC(ret) && !info.is_valid()) {
    ret = OB_INVALID_DATA;
    SHARE_LOG(WARN, "incomplete ai column spec", KR(ret), K(spec_text));
  }
  return ret;
}

} // namespace share
} // namespace oceanbase