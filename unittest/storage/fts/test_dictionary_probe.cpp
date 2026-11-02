/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include "storage/fts/dict/ob_ft_dat_dict.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

namespace {
// Keep the original hash and modulo probe as an independent compatibility
// oracle, including the serialized in-memory dictionary layout.
void legacy_insert(std::vector<char> &storage, const std::string &key, int code)
{
  auto &header = *reinterpret_cast<ObArrayHashMap::Header *>(storage.data());
  uint64_t index = ObString(key.size(), key.data()).hash() % header.capacity_;
  while (header.data[index].used) {
    index = (index + 3) % header.capacity_;
  }
  auto &entry = header.data[index];
  entry.word.set_word(key.data(), key.size());
  entry.code = code;
  entry.used = true;
  ++header.count_;
}

void check_find(const ObArrayHashMap &map, const std::string &key, int expected)
{
  ObFTSingleWord word;
  // Unused bytes must not participate in hashing or comparison.
  memset(word.word, 0xa5, sizeof(word.word));
  ASSERT_EQ(OB_SUCCESS, word.set_word(key.data(), key.size()));
  int code = -1;
  ASSERT_EQ(OB_SUCCESS, map.find(word, code));
  EXPECT_EQ(expected, code);
}

TEST(FTSDictionaryProbeTest, legacy_layout_and_all_character_lengths)
{
  for (const size_t size : {0, 100, 192, 2049}) {
    std::vector<char> old_storage(ObArrayHashMap::estimate_size(size));
    std::vector<char> new_storage(old_storage.size());
    auto &old_map = *reinterpret_cast<ObArrayHashMap *>(old_storage.data());
    auto &new_map = *reinterpret_cast<ObArrayHashMap *>(new_storage.data());
    ASSERT_EQ(OB_SUCCESS, old_map.init(size));
    ASSERT_EQ(OB_SUCCESS, new_map.init(size));
    std::vector<std::string> keys{"", "A", "\xc3\xa9", "\xe4\xb8\xad", "\xf0\x9f\x98\x80"};
    std::set<std::string> seen(keys.begin(), keys.end());
    uint32_t state = 17;
    while (keys.size() < std::min<size_t>(new_map.capacity() / 2, 1000)) {
      const size_t length = 1 + keys.size() % ObCharset::MAX_MB_LEN;
      std::string key(length, '\0');
      for (char &byte : key) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<char>(state >> 24);
      }
      if (seen.insert(key).second) {
        keys.push_back(key);
      }
    }
    for (size_t i = 0; i < keys.size(); ++i) {
      legacy_insert(old_storage, keys[i], i + 1);
      ASSERT_EQ(OB_SUCCESS, new_map.insert(ObString(keys[i].size(), keys[i].data()), i + 1));
    }
    EXPECT_EQ(old_storage, new_storage);
    for (size_t i = 0; i < keys.size(); ++i) {
      check_find(old_map, keys[i], i + 1);
      check_find(new_map, keys[i], i + 1);
    }
    ObFTSingleWord missing;
    const std::string absent(ObCharset::MAX_MB_LEN, '\xff');
    ASSERT_EQ(0U, seen.count(absent));
    ASSERT_EQ(OB_SUCCESS, missing.set_word(absent.data(), absent.size()));
    int code = -37;
    EXPECT_EQ(OB_ENTRY_NOT_EXIST, new_map.find(missing, code));
    EXPECT_EQ(-37, code);
  }
}

TEST(FTSDictionaryProbeTest, collision_chain_wraps_each_boundary)
{
  for (const size_t size : {0, 192}) {
    const size_t capacity = ObArrayHashMap::estimate_capacity(size);
    for (size_t bucket = capacity - 3; bucket < capacity; ++bucket) {
      std::vector<char> storage(ObArrayHashMap::estimate_size(size));
      std::vector<char> legacy(storage.size());
      auto &map = *reinterpret_cast<ObArrayHashMap *>(storage.data());
      ASSERT_EQ(OB_SUCCESS, map.init(size));
      ASSERT_EQ(OB_SUCCESS, reinterpret_cast<ObArrayHashMap *>(legacy.data())->init(size));
      std::vector<std::string> keys;
      for (uint32_t value = 0; keys.size() < 32; ++value) {
        ASSERT_LT(value, 1000000U);
        const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8),
                              static_cast<char>(value >> 16)};
        if (ObString(sizeof(bytes), bytes).hash() % capacity == bucket) {
          keys.emplace_back(bytes, sizeof(bytes));
        }
      }
      for (size_t i = 0; i + 1 < keys.size(); ++i) {
        ASSERT_EQ(OB_SUCCESS, map.insert(ObString(keys[i].size(), keys[i].data()), i));
        legacy_insert(legacy, keys[i], i);
      }
      EXPECT_EQ(storage, legacy);
      for (size_t i = 0; i + 1 < keys.size(); ++i) {
        check_find(map, keys[i], i);
      }
      ObFTSingleWord missing;
      ASSERT_EQ(OB_SUCCESS, missing.set_word(keys.back().data(), keys.back().size()));
      int code = -1;
      EXPECT_EQ(OB_ENTRY_NOT_EXIST, map.find(missing, code));
      EXPECT_EQ(-1, code);
    }
  }
}

TEST(FTSDictionaryProbeTest, character_equality_matches_exact_bytes)
{
  for (int length = 0; length <= ObCharset::MAX_MB_LEN; ++length) {
    for (int byte = 0; byte <= 255; ++byte) {
      const std::string text(length, static_cast<char>(byte));
      ObFTSingleWord left, right;
      memset(left.word, 0x55, sizeof(left.word));
      memset(right.word, 0xaa, sizeof(right.word));
      ASSERT_EQ(OB_SUCCESS, left.set_word(text.data(), text.size()));
      ASSERT_EQ(OB_SUCCESS, right.set_word(text.data(), text.size()));
      EXPECT_TRUE(left == left);
      EXPECT_TRUE(left == right);
      for (int position = 0; position < length; ++position) {
        right.word[position] ^= 1;
        EXPECT_FALSE(left == right);
        EXPECT_FALSE(right == left);
        right.word[position] ^= 1;
      }
      right.word_len = (length + 1) % (ObCharset::MAX_MB_LEN + 1);
      EXPECT_FALSE(left == right);
    }
  }
}
} // namespace
