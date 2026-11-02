#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <limits>
#include "storage/retrieval/ob_text_daat_iter.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

namespace {
void compare_with_collector(const int64_t dimensions, const uint64_t matches,
                            const int64_t minimum, const ObIArray<double> *weights,
                            const double boost)
{
  ObSRDaaTInnerProductRelevanceCollector collector;
  ASSERT_EQ(OB_SUCCESS, collector.init(minimum));
  for (int64_t i = 0; i < dimensions; ++i) {
    if ((matches & (uint64_t(1) << i)) != 0) {
      double score = 1.0;
      if (weights != nullptr) score *= boost * weights->at(i);
      ASSERT_EQ(OB_SUCCESS, collector.collect_one_dim(i, score));
    }
  }
  double expected = 0.0, actual = 0.0;
  bool expected_valid = false, actual_valid = false;
  ASSERT_EQ(OB_SUCCESS, collector.get_result(expected, expected_valid));
  ASSERT_EQ(OB_SUCCESS, collector.evaluate_membership(
      dimensions, matches, weights, boost, actual, actual_valid));
  EXPECT_EQ(expected_valid, actual_valid);
  if (std::isnan(expected)) {
    EXPECT_TRUE(std::isnan(actual));
  } else {
    EXPECT_EQ(0, std::memcmp(&expected, &actual, sizeof(expected)));
  }
  // The fast operation must leave the generic collector's cleared state intact.
  ASSERT_EQ(OB_SUCCESS, collector.get_result(actual, actual_valid));
  EXPECT_EQ(0.0, actual);
  EXPECT_EQ(0 >= minimum, actual_valid);
}

TEST(NativeMembershipCollector, MatchesOrderedWeightsAndMinimum)
{
  for (int64_t dimensions = 0; dimensions <= 8; ++dimensions) {
    ObSEArray<double, 8> weights;
    for (int64_t i = 0; i < dimensions; ++i) {
      ASSERT_EQ(OB_SUCCESS, weights.push_back((i % 3 == 0) ? -0.125 : (i % 3 == 1) ? 0.0 : 3.25));
    }
    for (int64_t minimum : {-1, 0, 1, 2, 5, 8, 10}) {
      for (uint64_t matches = 0; matches < (uint64_t(1) << dimensions); ++matches) {
        for (double boost : {0.0, 1.0, 2.5}) {
          compare_with_collector(dimensions, matches, minimum, nullptr, boost);
          compare_with_collector(dimensions, matches, minimum, &weights, boost);
        }
      }
    }
  }
}

TEST(NativeMembershipCollector, NonfiniteAndFullWidthMasks)
{
  ObSEArray<double, 4> weights;
  ASSERT_EQ(OB_SUCCESS, weights.push_back(std::numeric_limits<double>::infinity()));
  ASSERT_EQ(OB_SUCCESS, weights.push_back(-std::numeric_limits<double>::infinity()));
  ASSERT_EQ(OB_SUCCESS, weights.push_back(std::numeric_limits<double>::quiet_NaN()));
  ASSERT_EQ(OB_SUCCESS, weights.push_back(-0.0));
  for (uint64_t mask = 0; mask < 16; ++mask) {
    compare_with_collector(4, mask, 2, &weights, 1.0);
  }
  compare_with_collector(64, ~uint64_t(0), 64, nullptr, 1.0);
  compare_with_collector(64, uint64_t(1) << 63, 2, nullptr, 1.0);
}

TEST(NativeMembershipCollector, InvalidShapeAndDefaultCapability)
{
  ObTextDaaTParam param;
  EXPECT_EQ(nullptr, param.membership_collector_);
  ObSRDaaTInnerProductRelevanceCollector collector;
  ASSERT_EQ(OB_SUCCESS, collector.init(1));
  double result = 7.0;
  bool valid = true;
  EXPECT_EQ(OB_INVALID_ARGUMENT, collector.evaluate_membership(-1, 0, nullptr, 1, result, valid));
  EXPECT_EQ(OB_INVALID_ARGUMENT, collector.evaluate_membership(65, 0, nullptr, 1, result, valid));
  EXPECT_EQ(OB_INVALID_ARGUMENT, collector.evaluate_membership(2, 4, nullptr, 1, result, valid));
  ObSEArray<double, 1> weights;
  ASSERT_EQ(OB_SUCCESS, weights.push_back(1));
  EXPECT_EQ(OB_INVALID_ARGUMENT, collector.evaluate_membership(2, 1, &weights, 1, result, valid));
  EXPECT_EQ(7.0, result);
  EXPECT_TRUE(valid);
}
}
