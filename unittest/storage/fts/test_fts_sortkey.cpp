/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>
#include "data_plane/ob_order_perserving_encoder.h"
#include "lib/charset/ob_charset.h"

using namespace oceanbase::common;
using namespace oceanbase::share;

namespace {
int sign(int value) { return (value > 0) - (value < 0); }

std::string encoded(const std::string &value, ObCollationType cs, bool ascending)
{
  ObDatum datum;
  datum.set_string(value.data(), value.size());
  ObEncParam param;
  param.type_ = ObVarcharType;
  param.cs_type_ = cs;
  param.is_asc_ = ascending;
  param.is_nullable_ = true;
  param.is_null_first_ = true;
  unsigned char buffer[16384];
  int64_t length = 0;
  EXPECT_EQ(OB_SUCCESS, ObSortkeyConditioner::process_key_conditioning(
      datum, buffer, sizeof(buffer), length, param));
  EXPECT_TRUE(param.is_valid_uni_);
  return std::string(reinterpret_cast<char *>(buffer), length);
}

TEST(FTSSortkeyTest, varchar_encoding_preserves_collation_order)
{
  std::vector<std::string> values{
      "", " ", "  ", "a", "A", "a ", "a  ", "a b", "a  b", "a!", "ab",
      "é", "É", "e", "é", "中文", "中文 ", "中午", "ＡＢＣ", "abc", "😀", "😁",
      std::string("\0", 1), std::string("\0\0", 2), std::string("a\0", 2),
      std::string("a\0b", 3), "a\t", "\t", std::string(84, 'z'), std::string(84, 'Z')};
  uint32_t state = 37;
  for (int i = 0; i < 150; ++i) {
    std::string value;
    for (int j = 0; j < i % 85; ++j) {
      state = state * 1664525U + 1013904223U;
      value += static_cast<char>(state % 128);
    }
    values.push_back(value);
  }
  for (auto cs : {CS_TYPE_BINARY, CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI}) {
    for (bool ascending : {false, true}) {
      std::vector<std::string> keys;
      for (const auto &value : values) keys.push_back(encoded(value, cs, ascending));
      for (size_t i = 0; i < values.size(); ++i) {
        for (size_t j = 0; j < values.size(); ++j) {
          int expected = sign(ObCharset::strcmpsp(cs, values[i].data(), values[i].size(),
                                                values[j].data(), values[j].size(), false));
          int actual = sign(keys[i].compare(keys[j]));
          ASSERT_EQ(ascending ? expected : -expected, actual)
              << "collation " << cs << " pair " << i << "," << j;
        }
      }
    }
  }
}

TEST(FTSSortkeyTest, composite_word_and_binary_document_id_keep_field_boundaries)
{
  const std::vector<std::string> words{"a", "A", "a ", "ab", "é", "e", "中文"};
  const std::vector<std::string> ids{"", std::string("\0", 1), std::string("\0\1", 2),
                                    std::string("\1\0", 2), "a", "aa"};
  for (const auto &left : words) {
    for (const auto &right : words) {
      for (const auto &lid : ids) {
        for (const auto &rid : ids) {
          int expected = ObCharset::strcmpsp(CS_TYPE_UTF8MB4_GENERAL_CI,
              left.data(), left.size(), right.data(), right.size(), false);
          if (expected == 0) expected = lid.compare(rid);
          auto lkey = encoded(left, CS_TYPE_UTF8MB4_GENERAL_CI, true) + encoded(lid, CS_TYPE_BINARY, true);
          auto rkey = encoded(right, CS_TYPE_UTF8MB4_GENERAL_CI, true) + encoded(rid, CS_TYPE_BINARY, true);
          ASSERT_EQ(sign(expected), sign(lkey.compare(rkey)));
        }
      }
    }
  }
}
} // namespace
