/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include "sql/engine/sort/ob_sort_op_impl.h"
#include "sql/engine/ob_exec_context.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase {
namespace sql {
namespace {
using namespace common;
using StoredRow = ObChunkDatumStore::StoredRow;

struct TestRow
{
  alignas(ObDatum) char buffer[sizeof(StoredRow) + 3 * sizeof(ObDatum)];
  int64_t values[3];
  StoredRow *row;
  TestRow(int64_t a, int64_t b, int64_t c) : values{a, b, c}
  {
    row = new (buffer) StoredRow();
    row->cnt_ = 3;
    row->row_size_ = sizeof(buffer);
    for (int i = 0; i < 3; ++i) {
      new (&row->cells()[i]) ObDatum();
      row->cells()[i].ptr_ = reinterpret_cast<char *>(&values[i]);
      row->cells()[i].set_int(values[i]);
    }
  }
};

int compare_nullable_int(const ObDatum &a, const ObDatum &b, int &result,
                         const ObDatumAccessContext *)
{
  if (a.is_null() || b.is_null()) {
    result = a.is_null() == b.is_null() ? 0 : (a.is_null() ? -1 : 1);
  } else {
    result = a.get_int() < b.get_int() ? -1 : (a.get_int() > b.get_int() ? 1 : 0);
  }
  return OB_SUCCESS;
}

int fail_compare(const ObDatum &, const ObDatum &, int &, const ObDatumAccessContext *)
{
  return OB_ERR_UNEXPECTED;
}

class SortCompareAccessTest : public testing::Test
{
protected:
  ObArenaAllocator allocator;
  ObExecContext context{allocator};
  ObSEArray<ObSortFieldCollation, 4> collations;
  ObSEArray<ObSortCmpFunc, 4> functions;
  ObSortOpImpl::Compare compare;

  void key(uint32_t index, bool ascending = true)
  {
    ASSERT_EQ(OB_SUCCESS, collations.push_back(
        ObSortFieldCollation(index, CS_TYPE_BINARY, ascending, NULL_FIRST)));
    ObSortCmpFunc function;
    function.cmp_func_ = compare_nullable_int;
    ASSERT_EQ(OB_SUCCESS, functions.push_back(function));
  }
  void init()
  {
    ASSERT_EQ(OB_SUCCESS, compare.init(&collations, &functions, &context, false));
  }
};

TEST_F(SortCompareAccessTest, nonadjacent_keys_direction_and_window_range)
{
  key(2); key(0, false); key(1);
  init();
  TestRow a(1, 2, 3), b(1, 4, 3);
  EXPECT_TRUE(compare(a.row, b.row));
  EXPECT_FALSE(compare(b.row, a.row));
  compare.set_cmp_range(1, 2);
  EXPECT_FALSE(compare(a.row, b.row));
  EXPECT_FALSE(compare(b.row, a.row));
  compare.set_cmp_range(2, 3);
  EXPECT_TRUE(compare(a.row, b.row));
  collations.at(2).is_ascending_ = false;
  EXPECT_TRUE(compare(b.row, a.row));
  compare.set_cmp_range(2, 2);
  EXPECT_FALSE(compare(a.row, b.row));
  EXPECT_EQ(OB_SUCCESS, compare.get_error_code());
}

TEST_F(SortCompareAccessTest, nulls_equal_keys_and_with_ties)
{
  key(0); key(1, false);
  init();
  TestRow a(1, 2, 3), b(1, 2, 4);
  EXPECT_FALSE(compare(a.row, b.row));
  EXPECT_EQ(0, compare.with_ties_cmp(a.row, b.row));
  a.row->cells()[0].set_null();
  EXPECT_TRUE(compare(a.row, b.row));
  EXPECT_GT(compare.with_ties_cmp(a.row, b.row), 0);
  b.row->cells()[0].set_null();
  b.row->cells()[1].set_int(4);
  EXPECT_TRUE(compare(b.row, a.row));
  EXPECT_GT(compare.with_ties_cmp(b.row, a.row), 0);
  EXPECT_EQ(OB_SUCCESS, compare.get_error_code());
}

TEST_F(SortCompareAccessTest, comparison_failure_stays_visible)
{
  key(0); init();
  TestRow a(1, 2, 3), b(2, 3, 4);
  functions.at(0).cmp_func_ = fail_compare;
  EXPECT_FALSE(compare(a.row, b.row));
  EXPECT_EQ(OB_ERR_UNEXPECTED, compare.get_error_code());
  functions.at(0).cmp_func_ = compare_nullable_int;
  compare(a.row, b.row);
  EXPECT_EQ(OB_ERR_UNEXPECTED, compare.get_error_code());
}

TEST_F(SortCompareAccessTest, empty_and_mismatched_key_arrays)
{
  init();
  TestRow a(1, 2, 3), b(2, 3, 4);
  EXPECT_FALSE(compare(a.row, b.row));
  EXPECT_EQ(0, compare.with_ties_cmp(a.row, b.row));
  compare.reset();
  key(0);
  functions.pop_back();
  EXPECT_EQ(OB_INVALID_ARGUMENT, compare.init(&collations, &functions, &context, false));
}
} // namespace
} // namespace sql
} // namespace oceanbase
