/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/ob_fts_parser_property.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

namespace {
void expect_same(const ObFTParserProperty &left, const ObFTParserProperty &right)
{
  EXPECT_EQ(left.min_token_size_, right.min_token_size_);
  EXPECT_EQ(left.max_token_size_, right.max_token_size_);
  EXPECT_EQ(left.ngram_token_size_, right.ngram_token_size_);
  EXPECT_EQ(left.ik_mode_smart_, right.ik_mode_smart_);
  EXPECT_EQ(left.dict_table_, right.dict_table_);
  EXPECT_EQ(left.stopword_table_, right.stopword_table_);
  EXPECT_EQ(left.quantifier_table_, right.quantifier_table_);
  EXPECT_EQ(left.min_ngram_token_size_, right.min_ngram_token_size_);
  EXPECT_EQ(left.max_ngram_token_size_, right.max_ngram_token_size_);
}

TEST(FTSDefaultsTest, omitted_properties_match_normalized_ddl_defaults)
{
  for (const char *name : {"ik.1", "beng.1", "space.1", "ngram.1", "ngram2.1"}) {
    ObArenaAllocator allocator;
    ObFTParser parser;
    ObString parser_name = ObString::make_string(name);
    ASSERT_EQ(OB_SUCCESS, parser.parse_from_str(parser_name.ptr(), parser_name.length()));
    ObFTParserJsonProps canonical;
    ObString serialized;
    ASSERT_EQ(OB_SUCCESS, canonical.init());
    ASSERT_EQ(OB_SUCCESS, canonical.parse_from_valid_str(ObString()));
    ASSERT_EQ(OB_SUCCESS, canonical.rebuild_props_for_ddl(parser_name, CS_TYPE_UTF8MB4_BIN, false));
    ASSERT_EQ(OB_SUCCESS, canonical.to_format_json(allocator, serialized));
    ObFTParserProperty expected, actual;
    ASSERT_EQ(OB_SUCCESS, expected.parse_for_parser_helper(parser, serialized));
    ASSERT_EQ(OB_SUCCESS, actual.parse_for_parser_helper(parser, ObString()));
    expect_same(expected, actual);
  }
}

TEST(FTSDefaultsTest, omitted_properties_clear_preceding_custom_configuration)
{
  ObFTParser parser;
  ASSERT_EQ(OB_SUCCESS, parser.parse_from_str("ik.1", 4));
  ObFTParserProperty actual, expected;
  ASSERT_EQ(OB_SUCCESS, actual.parse_for_parser_helper(parser, ObString::make_string("{\"ik_mode\":\"max_word\"}")));
  EXPECT_FALSE(actual.ik_mode_smart_);
  ASSERT_EQ(OB_SUCCESS, actual.parse_for_parser_helper(parser, ObString()));
  ASSERT_EQ(OB_SUCCESS, expected.parse_for_parser_helper(parser, ObString::make_string("{\"ik_mode\":\"smart\"}")));
  expect_same(expected, actual);
  ASSERT_EQ(OB_SUCCESS, parser.parse_from_str("beng.1", 6));
  ASSERT_EQ(OB_SUCCESS, actual.parse_for_parser_helper(parser,
      ObString::make_string("{\"min_token_size\":3,\"max_token_size\":70}")));
  ASSERT_EQ(OB_SUCCESS, actual.parse_for_parser_helper(parser, ObString()));
  expect_same(ObFTParserProperty(), actual);
}
} // namespace
