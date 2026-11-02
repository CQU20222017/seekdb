/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */

#include <gtest/gtest.h>
#include <map>
#include "storage/fts/ik/ob_ik_processor.h"
#include <string>
#include "storage/fts/ob_fts_stop_word.h"
#include "data_plane/fts/ob_fts_parser_property.h"
#include "share/text_analysis/ob_text_analyzer.h"

using namespace oceanbase::common;

namespace oceanbase {
namespace storage {
namespace {
TEST(FTSTextAnalysisTest, ik_character_path_matches_generic_decoder)
{
  for (const auto collation : {CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN,
                                CS_TYPE_UTF16_GENERAL_CI}) {
    for (int byte = 0; byte <= 255; ++byte) {
      const char input[] = {static_cast<char>(byte), 'A'};
      for (int size = 1; size <= 2; ++size) {
        ObArenaAllocator allocator;
        int64_t expected_length = 0;
        auto expected_type = ObFTCharUtil::CharType::USELESS;
        int expected = ObCharset::first_valid_char(collation, input, size, expected_length);
        if (expected == OB_SUCCESS) {
          expected = ObFTCharUtil::classify_first_char(collation, input, expected_length, expected_type);
        }
        TokenizeContext ctx(collation, allocator, input, size, false);
        ASSERT_EQ(expected, ctx.init()) << byte << " " << size << " " << collation;
        if (expected == OB_SUCCESS) {
          const char *actual = nullptr;
          uint8_t length = 0;
          auto type = ObFTCharUtil::CharType::USELESS;
          ASSERT_EQ(OB_SUCCESS, ctx.current_char(actual, length));
          ASSERT_EQ(OB_SUCCESS, ctx.current_char_type(type));
          EXPECT_EQ(input, actual);
          EXPECT_EQ(expected_length, length);
          EXPECT_EQ(expected_type, type) << byte;
        }
      }
    }
  }
}

TEST(FTSTextAnalysisTest, normalization_and_grouping_preserve_retained_tokens)
{
  const std::string long_word(4096, 'x');
  const std::string input = "lower MIXED xÉx lower 123 " + long_word + " LOWER";
  const std::map<std::string, int64_t> expected = {
      {"lower", 3}, {"mixed", 1}, {"xéx", 1}, {"123", 1}, {long_word, 1}};
  for (const auto collation : {CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN}) {
    for (const bool grouping : {false, true}) {
      ObArenaAllocator allocator;
      share::ObTextAnalysisCtx ctx;
      ctx.cs_ = ObCharset::get_charset(collation);
      ctx.filter_stopword_ = false;
      ctx.need_grouping_ = grouping;
      share::ObEnglishTextAnalyzer analyzer;
      ASSERT_EQ(OB_SUCCESS, analyzer.init(ctx, allocator));
      ObDatum document;
      document.set_string(input.data(), input.size());
      share::ObITokenStream *stream = nullptr;
      ASSERT_EQ(OB_SUCCESS, analyzer.analyze(document, stream));
      std::map<std::string, int64_t> actual;
      ObDatum token;
      int64_t frequency = 0;
      int ret = OB_SUCCESS;
      while (OB_SUCCESS == (ret = stream->get_next(token, frequency))) {
        actual[std::string(token.ptr_, token.len_)] += frequency;
      }
      EXPECT_EQ(OB_ITER_END, ret);
      EXPECT_EQ(expected, actual);
    }
  }
}

class FTSAggregationTest : public testing::Test
{
protected:
  void SetUp() override
  {
    meta_.set_varchar();
    meta_.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    ASSERT_EQ(OB_SUCCESS, words_.create(10, ObMemAttr("FtsTest")));
  }
  void TearDown() override { words_.destroy(); }
  int64_t count(const char *text)
  {
    ObFTWord key(STRLEN(text), text, meta_);
    int64_t result = -1;
    EXPECT_EQ(OB_SUCCESS, words_.get_refactored(key, result));
    return result;
  }
  ObArenaAllocator allocator_;
  ObFTWordMap words_;
  ObObjMeta meta_;
  ObFTParserProperty property_;
};

TEST_F(FTSAggregationTest, repeated_tokens_accumulate_without_replacing_keys)
{
  ObAddWordFlag flags;
  flags.set_groupby_word();
  ObAddWord add(property_, meta_, flags, allocator_, words_);
  for (int i = 0; i < 500; ++i) {
    ASSERT_EQ(OB_SUCCESS, add.process_word("database", 8, 8, 1));
  }
  EXPECT_EQ(500, count("database"));
  EXPECT_EQ(1, words_.size());
  EXPECT_EQ(500, add.get_add_word_count());
}

TEST_F(FTSAggregationTest, weighted_repeats_preserve_existing_initial_count)
{
  ObAddWordFlag flags;
  flags.set_groupby_word();
  ObAddWord add(property_, meta_, flags, allocator_, words_);
  ASSERT_EQ(OB_SUCCESS, add.process_word("word", 4, 4, 7));
  ASSERT_EQ(OB_SUCCESS, add.process_word("word", 4, 4, 5));
  ASSERT_EQ(OB_SUCCESS, add.process_word("word", 4, 4, 2));
  EXPECT_EQ(8, count("word"));
  EXPECT_EQ(14, add.get_add_word_count());
}

TEST_F(FTSAggregationTest, collation_equivalent_tokens_share_the_same_count)
{
  ObAddWordFlag flags;
  flags.set_groupby_word();
  ObAddWord add(property_, meta_, flags, allocator_, words_);
  ASSERT_EQ(OB_SUCCESS, add.process_word("OceanBase", 9, 9, 1));
  ASSERT_EQ(OB_SUCCESS, add.process_word("oceanbase", 9, 9, 2));
  EXPECT_EQ(1, words_.size());
  EXPECT_EQ(3, count("OCEANBASE"));
}

TEST_F(FTSAggregationTest, non_grouping_mode_keeps_duplicate_error)
{
  ObAddWordFlag flags;
  ObAddWord add(property_, meta_, flags, allocator_, words_);
  ASSERT_EQ(OB_SUCCESS, add.process_word("word", 4, 4, 1));
  EXPECT_EQ(OB_HASH_EXIST, add.process_word("word", 4, 4, 1));
  EXPECT_EQ(1, count("word"));
}

TEST_F(FTSAggregationTest, ascii_stopword_index_preserves_dictionary_collation)
{
  ObStopWordChecker checker;
  ASSERT_EQ(OB_SUCCESS, checker.init());
  const char *cases[] = {
    "a", "ABOUT", "ThE", "www", "und", "the   ", "THE ", "   ", "database",
    "verylongnonstopword", "the1", "1", "th", "they", "theory", "the-word", "the\t",
    "thé", "ＴＨＥ", "数据库", "一", "😀", "tHe!", "a b", "café"
  };
  ObObjMeta dictionary_meta;
  dictionary_meta.set_varchar();
  dictionary_meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  for (const ObCollationType collation : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI}) {
    ObObjMeta input_meta;
    input_meta.set_varchar();
    input_meta.set_collation_type(collation);
    for (const char *text : cases) {
      bool expected = false;
      ObFTWord reference(STRLEN(text), text, dictionary_meta);
      for (const auto &stopword : ob_stop_word_list) {
        expected |= reference == ObFTWord(STRLEN(stopword), stopword, dictionary_meta);
      }
      bool actual = !expected;
      ASSERT_EQ(OB_SUCCESS, checker.check_stopword(ObFTWord(STRLEN(text), text, input_meta), actual));
      EXPECT_EQ(expected, actual) << text << " collation=" << collation;
    }
  }
}

TEST_F(FTSAggregationTest, binary_and_unsupported_collations_retain_existing_behavior)
{
  ObStopWordChecker checker;
  ASSERT_EQ(OB_SUCCESS, checker.init());
  for (const ObCollationType collation : {CS_TYPE_BINARY}) {
    for (const char *text : {"THE", "about", "database", "ThE "}) {
      ObString converted;
      ASSERT_EQ(OB_SUCCESS, ObCharset::charset_convert(allocator_, ObString(text),
          CS_TYPE_UTF8MB4_BIN, collation, converted));
      ObObjMeta input_meta;
      input_meta.set_varchar();
      input_meta.set_collation_type(collation);
      bool actual = false;
      ASSERT_EQ(OB_SUCCESS, checker.check_stopword(
          ObFTWord(converted.length(), converted.ptr(), input_meta), actual));
      EXPECT_EQ(0 != STRCMP(text, "database"), actual);
    }
  }
  // These enum values exist, but seekdb's supported collation set excludes
  // them. Keep the original conversion error instead of treating their bytes
  // as UTF-8 or ASCII.
  for (const ObCollationType collation : {CS_TYPE_UTF16_BIN, CS_TYPE_UTF16LE_BIN, CS_TYPE_LATIN1_SWEDISH_CI}) {
    ObObjMeta input_meta;
    input_meta.set_varchar();
    input_meta.set_collation_type(collation);
    bool actual = false;
    EXPECT_EQ(OB_INVALID_ARGUMENT, checker.check_stopword(ObFTWord(3, "the", input_meta), actual));
  }
}
} // namespace
} // namespace storage
} // namespace oceanbase
