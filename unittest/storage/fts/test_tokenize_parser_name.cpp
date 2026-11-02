/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>
#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_string.h"
#include "sql/engine/expr/ob_expr_operator.h"
#include "data_plane/fts/ob_fts_parser_property.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#define private public
#include "sql/engine/expr/ob_expr_tokenize.h"
#undef private

using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::sql;
namespace {
void compare_with_serializer(const std::string &input)
{
  ObString name(input.size(), input.data());
  ObFTParser reference;
  const int expected_ret = reference.init(name);
  ObExprTokenize::TokenizeParam param;
  const int actual_ret = ObExprTokenize::construct_ft_parser_inner_name(name, param);
  ASSERT_EQ(expected_ret, actual_ret) << input.size();
  if (expected_ret == OB_SUCCESS) {
    char expected[OB_FT_PARSER_NAME_LENGTH] = {};
    ASSERT_EQ(OB_SUCCESS, reference.serialize_to_str(expected, sizeof(expected)));
    EXPECT_EQ(std::strlen(expected), static_cast<size_t>(param.parser_name_.length()));
    EXPECT_EQ(std::string(expected), std::string(param.parser_name_.ptr(), param.parser_name_.length()));
    EXPECT_EQ('\0', param.parser_name_.ptr()[param.parser_name_.length()]);
  }
}

TEST(FTSParserNameSerializationTest, builtin_and_invalid_input_matches_general_serializer)
{
  for (int i = 0; i < ObFTParser::FTP_MAX; ++i) {
    const std::string name = ObFTParser::NAME_STR[i];
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
    compare_with_serializer(name);
    compare_with_serializer(upper);
    std::string mixed = name;
    if (!mixed.empty()) mixed[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(mixed[0])));
    compare_with_serializer(mixed);
    compare_with_serializer(" " + name);
    compare_with_serializer(name + " ");
    compare_with_serializer("\t" + name + "\n");
    compare_with_serializer(name + std::string("\0junk", 5));
    compare_with_serializer(name + std::string(1, '\0'));
  }
  for (const std::string &name : {std::string(), std::string("\0", 1), std::string("bogus"),
                                   std::string("ik.1"), std::string(63, 'x'), std::string(64, 'x'),
                                   std::string(65, 'x'), std::string("\xff\xfe", 2)})
    compare_with_serializer(name);
}

TEST(FTSParserNameSerializationTest, public_serializer_preserves_nondefault_version)
{
  for (int i = 0; i < ObFTParser::FTP_MAX; ++i) {
    for (const int version : {0, 2, 17}) {
      const std::string serialized = std::string(ObFTParser::NAME_STR[i]) + "." + std::to_string(version);
      ObFTParser parser;
      ASSERT_EQ(OB_SUCCESS, parser.parse_from_str(serialized.data(), serialized.size()));
      ASSERT_EQ(version, parser.get_parser_version());
      char buf[OB_FT_PARSER_NAME_LENGTH] = {};
      ASSERT_EQ(OB_SUCCESS, parser.serialize_to_str(buf, sizeof(buf)));
      EXPECT_EQ(serialized, std::string(buf));
    }
  }
}
} // namespace
