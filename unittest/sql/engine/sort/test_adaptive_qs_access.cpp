/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "sql/engine/sort/ob_sort_op_impl.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace {
using StoredRow = ObChunkDatumStore::StoredRow;

struct EncodedRow
{
  alignas(ObDatum) char buffer[sizeof(StoredRow) + sizeof(ObDatum)];
  std::string key;
  StoredRow *row;
  explicit EncodedRow(std::string value) : key(std::move(value))
  {
    row = new (buffer) StoredRow();
    row->cnt_ = 1;
    row->row_size_ = sizeof(buffer);
    new (row->cells()) ObDatum();
    row->cells()[0].set_string(key.data(), key.size());
  }
};

void check_sort(const std::vector<std::string> &keys, int64_t begin, int64_t end)
{
  ObArenaAllocator arena;
  ObSEArray<StoredRow *, 16> rows;
  std::vector<std::unique_ptr<EncodedRow>> owners;
  for (const auto &key : keys) {
    owners.emplace_back(new EncodedRow(key));
    ASSERT_EQ(OB_SUCCESS, rows.push_back(owners.back()->row));
  }
  std::vector<std::string> expected(keys.begin() + begin, keys.begin() + end);
  std::sort(expected.begin(), expected.end());
  ObSortOpImpl::ObAdaptiveQS sorter(rows, arena);
  bool can_encode = false;
  ASSERT_EQ(OB_SUCCESS, sorter.init(rows, arena, begin, end, can_encode));
  ASSERT_TRUE(can_encode);
  sorter.sort(begin, end);
  std::vector<StoredRow *> identities;
  for (int64_t i = 0; i < rows.count(); ++i) {
    if (i < begin || i >= end) {
      ASSERT_EQ(owners[i]->row, rows.at(i));
    } else {
      const ObDatum &datum = rows.at(i)->cells()[0];
      ASSERT_EQ(expected[i - begin], std::string(datum.ptr_, datum.len_));
      identities.push_back(rows.at(i));
    }
  }
  // Equal keys can reorder, but every original row must occur exactly once.
  std::vector<StoredRow *> original;
  for (int64_t i = begin; i < end; ++i) original.push_back(owners[i]->row);
  std::sort(original.begin(), original.end(), std::less<StoredRow *>());
  std::sort(identities.begin(), identities.end(), std::less<StoredRow *>());
  ASSERT_EQ(original, identities);
}

TEST(AdaptiveQSAccessTest, empty_single_and_insertion_boundaries)
{
  for (int count : {0, 1, 2, 15, 16, 17}) {
    std::vector<std::string> keys;
    for (int i = count; i > 0; --i) keys.push_back(std::to_string(i));
    check_sort(keys, 0, keys.size());
  }
}

TEST(AdaptiveQSAccessTest, binary_keys_prefixes_and_duplicate_identities)
{
  std::vector<std::string> keys{"", "a", "aa", "aaa", std::string("\0", 1),
      std::string("a\0", 2), std::string("\xff", 1), "中文", "😀"};
  uint32_t random = 1027;
  for (int i = 0; i < 8192; ++i) {
    std::string key;
    for (int j = 0; j < i % 33; ++j) {
      random = random * 1664525U + 1013904223U;
      key += static_cast<char>(random >> 24);
    }
    keys.push_back(key);
    if (i % 7 == 0) keys.push_back(key);
  }
  check_sort(keys, 0, keys.size());
}

TEST(AdaptiveQSAccessTest, long_common_prefix_and_partial_range)
{
  std::vector<std::string> keys;
  for (int i = 2048; i > 0; --i) {
    keys.push_back(std::string(257, 'p') + std::to_string(i % 63));
  }
  check_sort(keys, 5, keys.size() - 7);
}

TEST(AdaptiveQSAccessTest, ascending_descending_and_equal_partitions)
{
  std::vector<std::string> keys(1025, "identical");
  check_sort(keys, 0, keys.size());
  keys.clear();
  for (int i = 0; i < 4097; ++i) keys.push_back(std::to_string(i));
  std::sort(keys.begin(), keys.end());
  check_sort(keys, 0, keys.size());
  std::reverse(keys.begin(), keys.end());
  check_sort(keys, 0, keys.size());
}

TEST(AdaptiveQSAccessTest, invalid_range_and_null_encoding_fallback)
{
  ObArenaAllocator arena;
  EncodedRow row("a");
  ObSEArray<StoredRow *, 16> rows;
  ASSERT_EQ(OB_SUCCESS, rows.push_back(row.row));
  ObSortOpImpl::ObAdaptiveQS sorter(rows, arena);
  bool can_encode = false;
  EXPECT_EQ(OB_INVALID_ARGUMENT, sorter.init(rows, arena, -1, 1, can_encode));
  EXPECT_EQ(OB_INVALID_ARGUMENT, sorter.init(rows, arena, 0, 2, can_encode));
  row.row->cells()[0].set_null();
  ASSERT_EQ(OB_SUCCESS, sorter.init(rows, arena, 0, 1, can_encode));
  EXPECT_FALSE(can_encode);
}
} // namespace
