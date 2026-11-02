/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <limits>
#include "storage/retrieval/ob_sparse_daat_iter.h"
#include "sql/engine/ob_exec_context.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;
using namespace oceanbase::storage;

namespace {
class FilterBatchIterator : public ObSRDaaTIterImpl
{
public:
  using ObSRDaaTIterImpl::filter_on_demand;
  using ObSRDaaTIterImpl::project_results;
  void bind(ObSparseRetrievalMergeParam &param, ObIAllocator &allocator)
  {
    iter_param_ = &param;
    set_datum_func_ = ObISparseRetrievalMergeIter::set_datum_int;
    buffered_domain_ids_.set_allocator(&allocator);
    buffered_relevances_.set_allocator(&allocator);
    ASSERT_EQ(OB_SUCCESS, buffered_domain_ids_.prepare_allocate(4));
    ASSERT_EQ(OB_SUCCESS, buffered_relevances_.prepare_allocate(4));
    for (int i = 0; i < 4; ++i) {
      int64_t id = 100 + i;
      ObDatum datum;
      datum.ptr_ = reinterpret_cast<char *>(&id);
      datum.set_int(id);
      ASSERT_EQ(OB_SUCCESS, buffered_domain_ids_[i].from_datum(datum));
      buffered_relevances_[i] = 0.5 + i;
    }
  }
};

class FilterBatchTest : public testing::Test
{
protected:
  ObArenaAllocator allocator;
  ObExecContext context{allocator};
  ObEvalCtx eval{context};
  alignas(ObDatum) char storage[3][512]{};
  char *frames[3]{storage[0], storage[1], storage[2]};
  ObExpr id, score, filter;
  ObExpr *arguments[1]{&score};
  ObSparseRetrievalMergeParam param;
  FilterBatchIterator iterator;

  void SetUp() override
  {
    eval.frames_ = frames;
    eval.set_max_batch_size(4);
    eval.reuse(4);
    ObExpr *expressions[] = {&id, &score, &filter};
    for (int i = 0; i < 3; ++i) {
      ObExpr &expr = *expressions[i];
      expr.frame_idx_ = i;
      expr.datum_off_ = 0;
      expr.eval_info_off_ = 128;
      expr.eval_flags_off_ = 160;
      expr.res_buf_off_ = 256;
      expr.res_buf_len_ = 8;
      expr.batch_result_ = true;
      for (int j = 0; j < 4; ++j) {
        new (reinterpret_cast<ObDatum *>(storage[i]) + j) ObDatum();
      }
      new (storage[i] + expr.eval_info_off_) ObEvalInfo();
      auto *datums = expr.locate_datums_for_update(eval, 4);
      for (int j = 0; j < 4; ++j) datums[j].set_int(-99);
    }
    id.datum_meta_.type_ = ObUInt64Type;
    score.datum_meta_.type_ = ObDoubleType;
    filter.type_ = T_OP_BOOL;
    filter.args_ = arguments;
    filter.arg_cnt_ = 1;
    param.eval_ctx_ = &eval;
    param.id_proj_expr_ = &id;
    param.relevance_proj_expr_ = &score;
    param.filter_expr_ = &filter;
    iterator.bind(param, allocator);
  }
};

TEST_F(FilterBatchTest, decision_preserves_double_semantics_and_batch_context)
{
  ObEvalCtx::BatchInfoScopeGuard guard(eval);
  guard.set_batch_idx(3);
  for (double value : {0.0, -0.0, 0.5, -2.5, std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
    bool accepted = false;
    ASSERT_EQ(OB_SUCCESS, iterator.filter_on_demand(2, value, accepted));
    EXPECT_EQ(value != 0.0, accepted);
    EXPECT_EQ(3, eval.get_batch_idx());
    EXPECT_EQ(-99, filter.locate_batch_datums(eval)[2].get_int());
    EXPECT_FALSE(filter.get_evaluated_flags(eval).at(2));
  }
  bool accepted = false;
  EXPECT_EQ(OB_ERR_UNEXPECTED, iterator.filter_on_demand(4, 1.0, accepted));
  EXPECT_FALSE(accepted);
}

TEST_F(FilterBatchTest, projection_writes_only_returned_rows_and_preserves_projected_bit)
{
  ObEvalCtx::BatchInfoScopeGuard guard(eval);
  guard.set_batch_idx(2);
  for (bool projected : {false, true}) {
    for (int count : {1, 3, 4}) {
      auto *datums = filter.locate_datums_for_update(eval, 4);
      for (int i = 0; i < 4; ++i) datums[i].set_int(-99);
      filter.get_evaluated_flags(eval).reset(4);
      filter.get_eval_info(eval).evaluated_ = false;
      filter.get_eval_info(eval).projected_ = projected;
      ASSERT_EQ(OB_SUCCESS, iterator.project_results(count));
      EXPECT_EQ(2, eval.get_batch_idx());
      EXPECT_TRUE(filter.get_eval_info(eval).evaluated_);
      EXPECT_EQ(projected, filter.get_eval_info(eval).projected_);
      for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(i < count, filter.get_evaluated_flags(eval).at(i));
        if (i < count) {
          EXPECT_EQ(1, datums[i].get_int32());
          EXPECT_EQ(100 + i, id.locate_batch_datums(eval)[i].get_int());
          EXPECT_EQ(0.5 + i, score.locate_batch_datums(eval)[i].get_double());
        } else {
          EXPECT_EQ(-99, datums[i].get_int());
        }
      }
    }
  }
}

TEST_F(FilterBatchTest, nonbatch_boolean_result_keeps_immediate_materialization)
{
  filter.batch_result_ = false;
  for (double value : {0.0, 1.5}) {
    bool accepted = false;
    ASSERT_EQ(OB_SUCCESS, iterator.filter_on_demand(1, value, accepted));
    EXPECT_EQ(value != 0.0, accepted);
    EXPECT_EQ(accepted ? 1 : 0, filter.locate_batch_datums(eval)[0].get_int32());
    EXPECT_TRUE(filter.get_eval_info(eval).evaluated_);
  }
}
} // namespace
