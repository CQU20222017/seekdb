/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>
#include "lib/charset/ob_charset.h"

using namespace oceanbase::common;

namespace {
// The original character-by-character comparison, deliberately without the
// byte-equality or ASCII fast paths. Keep malformed-input fallback and padding policy in
// this oracle so the test checks more than valid Unicode equality.
int reference_compare(const ObCharsetInfo *cs, const std::string &left,
                      const std::string &right, bool distinguish_spaces)
{
  auto *l = reinterpret_cast<const unsigned char *>(left.data());
  auto *r = reinterpret_cast<const unsigned char *>(right.data());
  const auto *le = l + left.size();
  const auto *re = r + right.size();
  auto weight = [cs](ob_wc_t value) {
    if (value > cs->caseinfo->maxchar) return ob_wc_t{OB_CS_REPLACEMENT_CHARACTER};
    const auto *page = cs->caseinfo->page[value >> 8];
    return page == nullptr ? value : ob_wc_t{(cs->state & OB_CS_LOWER_SORT)
        ? page[value & 255].tolower : page[value & 255].sort};
  };
  while (l < le && r < re) {
    ob_wc_t lw = 0, rw = 0;
    int ll = cs->cset->mb_wc(cs, &lw, l, le);
    int rl = cs->cset->mb_wc(cs, &rw, r, re);
    if (ll <= 0 || rl <= 0) {
      int compared = memcmp(l, r, std::min(le - l, re - r));
      return compared != 0 ? compared : static_cast<int>((le - l) - (re - r));
    }
    lw = weight(lw);
    rw = weight(rw);
    if (lw != rw) return lw > rw ? 1 : -1;
    l += ll;
    r += rl;
  }
  if (le - l == re - r) return 0;
  const int direction = le - l > re - r ? 1 : -1;
  const auto *tail = direction == 1 ? l : r;
  const auto *end = direction == 1 ? le : re;
  for (; tail < end; ++tail) {
    if (*tail != ' ') return !distinguish_spaces && *tail < ' ' ? -direction : direction;
  }
  return distinguish_spaces ? direction : 0;
}

void check_pair(const std::string &left, const std::string &right)
{
  const ObCharsetInfo *cs = ObCharset::get_charset(CS_TYPE_UTF8MB4_GENERAL_CI);
  for (bool spaces : {false, true}) {
    const int expected = reference_compare(cs, left, right, spaces);
    const int actual = cs->coll->strnncollsp(
        cs, reinterpret_cast<const unsigned char *>(left.data()), left.size(),
        reinterpret_cast<const unsigned char *>(right.data()), right.size(), spaces);
    EXPECT_EQ(expected, actual) << "lengths " << left.size() << ", " << right.size()
                               << ", distinguish spaces " << spaces;
  }
}

TEST(UTF8SortEqualityTest, collation_padding_and_malformed_sequences)
{
  std::vector<std::string> values{
      "", " ", "  ", "a", "A", "a ", "A  ", "abc", "AbC", "abd", "abD",
      "é", "e", "É", "é", "中文", "中文 ", "中午", "ＡＢＣ", "abc ", "😀", "😁",
      std::string("a\0b", 3), std::string("a\0c", 3), std::string("a\0", 2),
      "a\t", "\x80", "\xc0\xaf", "\xe4\xb8", "\xed\xa0\x80", "\xf4\x90\x80\x80",
      "\xff", "\xff ", "\xff\xff", std::string(4096, 'x'), std::string(4096, 'X')};
  for (const auto &left : values) {
    check_pair(left, left);
    for (const auto &right : values) check_pair(left, right);
  }
}

TEST(UTF8SortEqualityTest, exhaustive_single_bytes_and_exact_length)
{
  for (int l = 0; l <= 255; ++l) {
    for (int r = 0; r <= 255; ++r) {
      check_pair(std::string(1, static_cast<char>(l)), std::string(1, static_cast<char>(r)));
    }
    const std::string value(3, static_cast<char>(l));
    check_pair(value, value + " ");
    check_pair(value + " ", value);
  }
  const auto *cs = ObCharset::get_charset(CS_TYPE_UTF8MB4_GENERAL_CI);
  const unsigned char left[] = {'a', 0, 'b', 'x'};
  const unsigned char right[] = {'a', 0, 'b', 'y'};
  EXPECT_EQ(0, cs->coll->strnncollsp(cs, left, 3, right, 3, false));
  EXPECT_LT(cs->coll->strnncollsp(cs, left, 4, right, 4, false), 0);
}

TEST(UTF8SortEqualityTest, randomized_equal_and_changed_buffers)
{
  uint32_t state = 23;
  auto next = [&state]() { state = state * 1664525U + 1013904223U; return state; };
  for (int round = 0; round < 5000; ++round) {
    std::string left(next() % 257, '\0');
    for (char &byte : left) byte = static_cast<char>(next() >> 24);
    std::string right(left);
    check_pair(left, right);
    if (!right.empty()) right[next() % right.size()] ^= 1;
    check_pair(left, right);
    check_pair(right, left);
    right += " ";
    check_pair(left, right);
  }
}
} // namespace
