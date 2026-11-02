/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 *
 * Equivalence tests for the specialized binary document-id compare: every
 * pair must return exactly the registered CS_TYPE_BINARY comparator's result.
 */
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>
#include "share/datum/ob_datum_funcs.h"
#include "storage/retrieval/fts_binary_docid_compare.h"

using namespace oceanbase::common;

namespace {

ObDatum make_str(const char *data, int32_t len)
{
  ObDatum datum;
  datum.ptr_ = data;
  datum.len_ = len;
  datum.set_none();
  return datum;
}

ObDatum make_null()
{
  ObDatum datum;
  datum.set_null();
  return datum;
}

ObDatumCmpFuncType reference_cmp()
{
  return ObDatumFuncs::get_basic_func(ObVarcharType, CS_TYPE_BINARY)->null_first_cmp_;
}

class BinaryDocIdCompareTest : public testing::Test
{
protected:
  // Consumers of the comparator read only the sign (equal / less / greater).
  // The registered comparator normalizes to +-1 while the specialized path
  // may return the raw byte difference, so equivalence is sign equality.
  static int sign(int value) { return (value > 0) - (value < 0); }

  void expect_same(const ObDatum &l, const ObDatum &r)
  {
    int expected = 0;
    ASSERT_EQ(OB_SUCCESS, reference_cmp()(l, r, expected, nullptr));
    int actual = 12345;
    ASSERT_EQ(OB_SUCCESS, oceanbase::storage::compare_binary_document_ids(l, r, actual, nullptr));
    ASSERT_EQ(sign(expected), sign(actual)) << "expected " << expected << " actual " << actual;
    if (l.is_null() || r.is_null()) {
      // The null branch must stay exactly normalized like the reference.
      ASSERT_EQ(expected, actual);
    }
  }
};

TEST_F(BinaryDocIdCompareTest, structured_pairs_match_registered_comparator)
{
  std::vector<std::string> values = {
      std::string(),
      std::string("a"),
      std::string("b"),
      std::string("ab"),
      std::string(8, 'x'),
      std::string(7, 'x') + "y",
      std::string(8, '\xff'),
      std::string(8, '\x80'),
      std::string("doc\x00id08"),
      std::string(16, 'x'),
      std::string(15, 'x') + "y",
      std::string(16, '\xff'),
      std::string("0123456789abcdef"),
      std::string("0123456789abcdee"),
      std::string(17, 'z'),
      std::string("tail spaces  "),
      std::string("tail spaces   "),
  };
  for (const std::string &l : values) {
    for (const std::string &r : values) {
      expect_same(make_str(l.data(), static_cast<int32_t>(l.size())),
                  make_str(r.data(), static_cast<int32_t>(r.size())));
    }
  }
  expect_same(make_str(nullptr, 0), make_null());
  expect_same(make_null(), make_str(nullptr, 0));
  expect_same(make_null(), make_null());
  expect_same(make_str("abcd", 4), make_null());
  expect_same(make_null(), make_str("abcd", 4));
}

TEST_F(BinaryDocIdCompareTest, unaligned_buffers_match_registered_comparator)
{
  std::string backing(64, '\0');
  std::mt19937 rng(20261029);
  std::uniform_int_distribution<int> byte_dist(0, 255);
  for (int round = 0; round < 2000; ++round) {
    for (char &c : backing) {
      c = static_cast<char>(byte_dist(rng));
    }
    const int l_off = 1 + (round % 7);
    const int r_off = 1 + ((round / 7) % 7);
    const int l_len = round % 25;
    const int r_len = (round / 25) % 25;
    expect_same(
        make_str(backing.data() + l_off, l_len),
        make_str(backing.data() + r_off, r_len));
  }
}

TEST_F(BinaryDocIdCompareTest, randomized_pairs_match_registered_comparator)
{
  std::mt19937 rng(880301);
  std::uniform_int_distribution<int> len_dist(0, 24);
  std::uniform_int_distribution<int> byte_dist(0, 255);
  std::uniform_int_distribution<int> alphabet_dist(0, 3);
  const std::string alphabet = "ab\x00\xff";
  for (int round = 0; round < 100000; ++round) {
    const int l_len = len_dist(rng);
    const int r_len = len_dist(rng);
    std::string l(l_len, '\0');
    std::string r(r_len, '\0');
    // A small alphabet plus random bytes produces long equal prefixes, which
    // exercise the fixed-width fast path and the length tiebreak together.
    for (int i = 0; i < l_len; ++i) {
      l[i] = (i % 5 == 0) ? alphabet[alphabet_dist(rng)] : static_cast<char>(byte_dist(rng));
    }
    for (int i = 0; i < r_len; ++i) {
      r[i] = (i % 5 == 0) ? alphabet[alphabet_dist(rng)] : static_cast<char>(byte_dist(rng));
    }
    if (round % 50 == 0) {
      expect_same(make_str(l.data(), l_len), make_null());
      expect_same(make_null(), make_str(r.data(), r_len));
    } else {
      expect_same(make_str(l.data(), l_len), make_str(r.data(), r_len));
    }
  }
}

}  // namespace
