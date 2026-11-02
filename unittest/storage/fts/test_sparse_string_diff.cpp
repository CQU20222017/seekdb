/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <numeric>
#include <string>
#include <vector>
#include "storage/blocksstable/encoding/ob_string_diff_decoder.h"

using namespace oceanbase::common;
using namespace oceanbase::blocksstable;

namespace {
class TestStringRows final : public ObIRowIndex
{
public:
  int get(const int64_t row, const char *&data, int64_t &length) const override
  {
    if (row == failing_row_) { return OB_ERROR; }
    data = rows_.at(row).data(); length = lengths_.at(row); return OB_SUCCESS;
  }
  int batch_get(const int32_t *, const int64_t, const bool, const char **, ObDatum *) const override
  { return OB_NOT_SUPPORTED; }
  int batch_get(const int32_t *, const int64_t, const bool, const char **, uint32_t *) const override
  { return OB_NOT_SUPPORTED; }
  std::vector<std::vector<char>> rows_;
  std::vector<int64_t> lengths_;
  int64_t failing_row_ = -1;
};

class StringDiffFixture
{
public:
  void build(int size, int pattern, bool hex, bool fixed, bool nullable)
  {
    fixed_ = fixed; nullable_ = nullable;
    const int rows = 65;
    micro_.row_count_ = rows;
    micro_.extend_value_bit_ = nullable ? 1 : 0;
    micro_.var_column_count_ = fixed ? 0 : 1;
    column_.type_ = ObColumnHeader::STRING_DIFF;
    column_.attr_ = (fixed ? ObColumnHeader::FIX_LENGTH : 0)
        | (nullable ? ObColumnHeader::HAS_EXTEND_VALUE : 0);
    std::vector<bool> different(size, false);
    for (int i = 0; i < size; ++i) {
      different[i] = pattern == 1 ? i == 0 : pattern == 2 ? i == size-1
          : pattern == 3 ? i < 128 && i % 2 == 0 : pattern == 4 ? i < 16
          : pattern == 5 ? i < 17 : pattern == 6;
    }
    std::vector<ObStringDiffHeader::DiffDesc> descriptors;
    for (int i = 0; i < size; ++i) {
      if (descriptors.empty() || descriptors.back().diff_ != different[i]
          || descriptors.back().count_ == 127) {
        ObStringDiffHeader::DiffDesc part;
        part.diff_ = different[i]; part.count_ = 1; descriptors.push_back(part);
      } else {
        ++descriptors.back().count_;
      }
    }
    const int changed = std::count(different.begin(), different.end(), true);
    const int row_bytes = hex ? (changed+1)/2 : changed;
    const int common_bytes = size-changed;
    const int metadata_size = sizeof(ObStringDiffHeader)+descriptors.size()+(hex ? 16 : 0)+common_bytes;
    const int bitmap_bytes = fixed && nullable ? (rows+7)/8 : 0;
    block_.assign(metadata_size+bitmap_bytes+rows*row_bytes+16, 0);
    auto *header = new (block_.data()) ObStringDiffHeader;
    header->string_size_ = size;
    header->diff_desc_cnt_ = descriptors.size();
    header->hex_char_array_size_ = hex ? 16 : 0;
    header->offset_ = !fixed && nullable ? 1 : 0;
    header->length_ = fixed ? row_bytes : 0;
    column_.offset_ = 0; column_.length_ = metadata_size;
    std::copy(descriptors.begin(), descriptors.end(), header->diff_descs_);
    unsigned char alphabet[16];
    for (int i = 0; i < 16; ++i) {
      alphabet[i] = i == 0 ? 0 : 0x80+i;
      if (hex) { header->hex_char_array()[i] = alphabet[i]; }
    }
    int common = 0;
    for (int i = 0; i < size; ++i) {
      if (!different[i]) { header->common_data()[common++] = static_cast<char>(i*13); }
    }
    row_index_.rows_.clear(); row_index_.lengths_.clear(); expected_.clear();
    for (int row = 0; row < rows; ++row) {
      expected_.emplace_back(size, '\0');
      const int prefix = !fixed && nullable ? 1 : 0;
      std::vector<char> bytes(prefix+row_bytes+16, 0);
      if (prefix && is_null(row)) { bytes[0] = 1; }
      if (fixed && nullable && is_null(row)) {
        auto *bitmap = reinterpret_cast<unsigned char *>(block_.data()+metadata_size);
        bitmap[row/8] |= 1 << (row%8);
      }
      int varying = 0;
      for (int i = 0; i < size; ++i) {
        if (!different[i]) {
          expected_[row][i] = static_cast<char>(i*13);
        } else {
          const unsigned char index = (row*17+i*3) % 16;
          const unsigned char value = hex ? alphabet[index] : (row*37+i*29) % 256;
          expected_[row][i] = static_cast<char>(value);
          if (hex) {
            auto &byte = bytes[prefix+varying/2];
            byte = static_cast<unsigned char>(byte) | (index << (varying%2 == 0 ? 4 : 0));
          } else {
            bytes[prefix+varying] = static_cast<char>(value);
          }
          ++varying;
        }
      }
      if (fixed) {
        std::copy_n(bytes.data(), row_bytes,
                    block_.data()+metadata_size+bitmap_bytes+row*row_bytes);
      }
      row_index_.rows_.push_back(std::move(bytes));
      row_index_.lengths_.push_back(prefix+row_bytes);
    }
    context_.micro_block_header_ = &micro_;
    context_.col_header_ = &column_;
    context_.allocator_ = &allocator_;
    context_.cache_attributes_[ObColumnHeader::FIX_LENGTH] = fixed;
    context_.cache_attributes_[ObColumnHeader::HAS_EXTEND_VALUE] = nullable;
    context_.cache_attributes_[ObColumnHeader::BIT_PACKING] = false;
    decoder_.reuse();
    ASSERT_EQ(OB_SUCCESS, decoder_.init(micro_, column_, block_.data()));
  }

  bool is_null(int row) const { return nullable_ && row % 5 == 1; }
  void check(const std::vector<int32_t> &ids, bool scalar = true)
  {
    std::vector<ObDatum> output(ids.size());
    ASSERT_EQ(OB_SUCCESS, decoder_.batch_decode(context_, &row_index_, ids.data(), nullptr,
                                              ids.size(), output.data()));
    for (size_t i = 0; i < ids.size(); ++i) {
      const int row = ids[i];
      ASSERT_EQ(is_null(row), output[i].is_null()) << row;
      if (!is_null(row)) {
        EXPECT_EQ(expected_[row], std::string(output[i].ptr_, output[i].len_));
      }
      if (scalar) {
        ObDatum one;
        auto &bytes = row_index_.rows_[row];
        ObBitStream bits(reinterpret_cast<unsigned char *>(bytes.data()), row_index_.lengths_[row]);
        ASSERT_EQ(OB_SUCCESS, decoder_.decode(context_, one, row, bits, bytes.data(),
                                             row_index_.lengths_[row]));
        ASSERT_EQ(is_null(row), one.is_null());
        if (!is_null(row)) {
          EXPECT_EQ(expected_[row], std::string(one.ptr_, one.len_));
        }
      }
    }
    // A later decode must not overwrite the preceding batch's owned bytes.
    ObDatum later[2];
    const int32_t later_ids[] = {64, 0};
    ASSERT_EQ(OB_SUCCESS, decoder_.batch_decode(context_, &row_index_, later_ids, nullptr, 2, later));
    for (size_t i = 0; i < ids.size(); ++i) {
      if (!is_null(ids[i])) {
        EXPECT_EQ(expected_[ids[i]], std::string(output[i].ptr_, output[i].len_));
      }
    }
  }
  ObArenaAllocator allocator_;
  ObMicroBlockHeader micro_;
  ObColumnHeader column_;
  ObColumnDecoderCtx context_;
  ObStringDiffDecoder decoder_;
  TestStringRows row_index_;
  std::vector<char> block_;
  std::vector<std::string> expected_;
  bool fixed_ = false;
  bool nullable_ = false;
};

TEST(SparseStringDiffTest, fixed_plain_strings_and_fallback_boundaries)
{
  for (int size : {0, 1, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 256}) {
    for (int pattern = 0; pattern <= 6; ++pattern) {
      StringDiffFixture fixture;
      fixture.build(size, pattern, false, true, false);
      fixture.check({64, 0, 32, 1, 63, 17, 32});
      fixture.check({33});
    }
  }
}

TEST(SparseStringDiffTest, hex_nibbles_nulls_and_common_only_strings)
{
  for (int size : {1, 3, 15, 16, 17, 32, 64, 128, 129}) {
    for (int pattern = 0; pattern <= 6; ++pattern) {
      StringDiffFixture fixture;
      fixture.build(size, pattern, true, true, true);
      fixture.check({1, 64, 31, 0, 63, 2, 5, 17, 64});
    }
  }
}

TEST(SparseStringDiffTest, variable_columns_match_scalar_and_original_bytes)
{
  for (bool hex : {false, true}) {
    for (bool nullable : {false, true}) {
      for (int size : {0, 1, 16, 17, 40, 128, 129}) {
        for (int pattern : {0, 2, 3, 4, 5, 6}) {
          StringDiffFixture fixture;
          fixture.build(size, pattern, hex, false, nullable);
          fixture.check({64, 1, 63, 0, 31, 17, 17, 2});
        }
      }
    }
  }
}

TEST(SparseStringDiffTest, output_batches_and_repeated_row_ids)
{
  StringDiffFixture fixture;
  fixture.build(40, 3, true, true, true);
  for (int capacity : {1, 2, 17, 64, 257}) {
    std::vector<int32_t> ids(capacity);
    for (int i = 0; i < capacity; ++i) { ids[i] = (i*13+7)%65; }
    fixture.check(ids);
  }
  fixture.build(40, 4, false, false, true);
  std::vector<int32_t> ids(65);
  std::iota(ids.rbegin(), ids.rend(), 0);
  fixture.check(ids);
}

TEST(SparseStringDiffTest, metadata_relocation_and_reuse)
{
  StringDiffFixture fixture;
  fixture.build(32, 4, true, true, false);
  auto relocated = fixture.block_;
  ASSERT_EQ(OB_SUCCESS, fixture.decoder_.update_pointer(fixture.block_.data(), relocated.data()));
  std::fill(fixture.block_.begin(), fixture.block_.end(), 0);
  fixture.check({64, 0, 33, 17});
  fixture.build(128, 4, false, false, true);
  fixture.check({1, 64, 0, 2});
}

class FailedStringAllocation final : public ObIAllocator
{
public:
  void *alloc(const int64_t) override { return nullptr; }
  void *alloc(const int64_t, const ObMemAttr &) override { return nullptr; }
  void free(void *) override {}
};

TEST(SparseStringDiffTest, errors_propagate_and_next_call_can_recover)
{
  StringDiffFixture fixture;
  fixture.build(40, 4, false, false, false);
  const int32_t ids[] = {0, 17, 64};
  ObDatum output[3];
  ObStringDiffDecoder not_initialized;
  EXPECT_EQ(OB_NOT_INIT, not_initialized.batch_decode(fixture.context_, &fixture.row_index_, ids,
                                                     nullptr, 3, output));
  FailedStringAllocation failed;
  fixture.context_.allocator_ = &failed;
  EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, fixture.decoder_.batch_decode(fixture.context_,
      &fixture.row_index_, ids, nullptr, 3, output));
  fixture.context_.allocator_ = &fixture.allocator_;
  fixture.row_index_.failing_row_ = 17;
  EXPECT_EQ(OB_ERROR, fixture.decoder_.batch_decode(fixture.context_, &fixture.row_index_, ids,
                                                  nullptr, 3, output));
  fixture.row_index_.failing_row_ = -1;
  fixture.check({0, 17, 64});
}
} // namespace
