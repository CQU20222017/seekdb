/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "lib/charset/ob_charset.h"

using namespace oceanbase::common;
namespace {
const ObCollationType collations[] = {CS_TYPE_UTF8MB4_GENERAL_CI,
    CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_UNICODE_CI, CS_TYPE_BINARY};

int direct(ObCollationType type, const char *a, int64_t alen,
           const char *b, int64_t blen, bool spaces)
{
  const auto *cs = ObCharset::get_charset(type);
  return cs->coll->strnncollsp(cs, reinterpret_cast<const unsigned char *>(a), alen,
      reinterpret_cast<const unsigned char *>(b), blen, spaces);
}

// Also run this test alone in a fresh process, before any test fetches a charset.
TEST(CharsetDispatchTest, concurrent_first_use)
{
  std::atomic<bool> start{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int n = 0; n < 8; ++n) {
    threads.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (int round = 0; round < 1000; ++round) {
        for (auto type : collations) {
          int result = ObCharset::strcmpsp(type, "AbC", 3, "abc", 3, false);
          bool ci = type == CS_TYPE_UTF8MB4_GENERAL_CI || type == CS_TYPE_UTF8MB4_UNICODE_CI;
          if ((ci && result != 0) || (!ci && result >= 0)) ++failures;
        }
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto &thread : threads) thread.join();
  EXPECT_EQ(0, failures.load());
}

TEST(CharsetDispatchTest, all_retained_handlers_and_padding)
{
  std::vector<std::string> values{"", " ", "a", "A", "a ", "a  ", "ab", "Ac",
      "é", "e", "É", "é", "中文", "中文 ", "😀", "😁", "\xff", "\xe4\xb8",
      std::string("a\0b", 3), std::string(4096, 'x'), std::string(4096, 'X')};
  for (auto type : collations) {
    for (bool spaces : {false, true}) {
      for (const auto &a : values) {
        for (const auto &b : values) {
          EXPECT_EQ(direct(type, a.data(), a.size(), b.data(), b.size(), spaces),
              ObCharset::strcmpsp(type, a.data(), a.size(), b.data(), b.size(), spaces));
        }
      }
    }
  }
}

TEST(CharsetDispatchTest, null_empty_strings_and_exact_boundaries)
{
  const char a[] = {'a', 0, 'b', 'x'};
  const char b[] = {'a', 0, 'b', 'y'};
  for (auto type : collations) {
    for (bool spaces : {false, true}) {
      EXPECT_EQ(0, ObCharset::strcmpsp(type, nullptr, 0, nullptr, 0, spaces));
      EXPECT_EQ(direct(type, nullptr, 0, " ", 1, spaces),
                ObCharset::strcmpsp(type, nullptr, 0, " ", 1, spaces));
      EXPECT_EQ(direct(type, " ", 1, nullptr, 0, spaces),
                ObCharset::strcmpsp(type, " ", 1, nullptr, 0, spaces));
      EXPECT_EQ(0, ObCharset::strcmpsp(type, a, 3, b, 3, spaces));
      EXPECT_LT(ObCharset::strcmpsp(type, a, 4, b, 4, spaces), 0);
    }
  }
}

TEST(CharsetDispatchTest, invalid_arguments_keep_zero_result)
{
  for (auto type : collations) {
    EXPECT_EQ(0, ObCharset::strcmpsp(type, nullptr, 1, "a", 1, false));
    EXPECT_EQ(0, ObCharset::strcmpsp(type, "a", 1, nullptr, 1, false));
    EXPECT_EQ(0, ObCharset::strcmpsp(type, "a", -1, "b", 1, false));
    EXPECT_EQ(0, ObCharset::strcmpsp(type, "a", 1, "b", -1, false));
  }
  for (int type : {-1, 0, 1, static_cast<int>(CS_TYPE_MAX), static_cast<int>(CS_TYPE_MAX) + 1}) {
    EXPECT_EQ(0, ObCharset::strcmpsp(static_cast<ObCollationType>(type), "a", 1, "b", 1, false));
  }
}
} // namespace
