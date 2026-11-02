/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"
#include "ob_text_retrieval_token_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (param.base_param_->relevance_expr_ != nullptr
      && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    membership_collector_ = nullptr != param.membership_collector_
        && param.membership_collector_ == param.relevance_collector_
        ? param.membership_collector_ : nullptr;
    native_dim_iters_ = param.native_dim_iters_;
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
  membership_collector_ = nullptr;
  native_dim_iters_ = false;
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->relevance_expr_ != nullptr) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

int ObTextDaaTIter::do_one_merge_round(int64_t &count)
{
  // A few posting lists are cheaper to merge directly than to push, rebuild
  // and pop a generic heap for every document. Scored and boolean retrieval
  // retain their existing merge path and score accumulation order.
  const int64_t dim_count = dim_iters_->count();
  if (nullptr != iter_param_->relevance_expr_ || dim_count > 4
      || mode_flag_ != sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE
      || function_lookup_mode_) {
    return ObSRDaaTIterImpl::do_one_merge_round(count);
  }

  int ret = OB_SUCCESS;
  // init() allocates these arrays from the same dimension count. A merge
  // round changes only their elements; it never grows or replaces a buffer.
  ObISRDaaTDimIter *const *iters = dim_iters_->get_data();
  const ObDatum **ids = iter_domain_ids_.get_data();
  int64_t *next_iters = next_round_iter_idxes_.get_data();
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t idx = next_iters[i];
    ObISRDaaTDimIter *iter = iters[idx];
    if (OB_ISNULL(iter)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (native_dim_iters_) {
      // Native token iterators advance their posting cursor in one non-virtual
      // step; end-of-list and error semantics match the virtual pair below.
      ret = static_cast<ObTextRetrievalDaaTTokenIter *>(iter)->advance_cached(ids[idx]);
      if (OB_ITER_END == ret) {
        ids[idx] = nullptr;
        ret = OB_SUCCESS;
      } else if (OB_SUCC(ret) && OB_ISNULL(ids[idx])) {
        ret = OB_ERR_UNEXPECTED;
      }
    } else if (OB_FAIL(iter->get_next_row())) {
      if (OB_ITER_END == ret) {
        ids[idx] = nullptr;
        ret = OB_SUCCESS;
      }
    } else if (OB_FAIL(iter->get_curr_id(ids[idx]))) {
    } else if (OB_ISNULL(ids[idx])) {
      ret = OB_ERR_UNEXPECTED;
    }
  }

  ObSRMergeItem minimum;
  minimum.iter_idx_ = -1;
  uint64_t matches = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < dim_count; ++i) {
    if (nullptr != ids[i]) {
      ObSRMergeItem item;
      item.iter_idx_ = i;
      int64_t cmp = -1;
      if (minimum.iter_idx_ >= 0
          && OB_FAIL(merge_cmp_.compare_ids(*ids[i], *ids[minimum.iter_idx_], cmp))) {
      } else if (cmp < 0) {
        minimum = item;
        matches = uint64_t(1) << i;
      } else if (cmp == 0) {
        matches |= uint64_t(1) << i;
      }
    }
  }

  if (OB_SUCC(ret)) {
    next_round_cnt_ = 0;
    if (minimum.iter_idx_ < 0) {
      ret = OB_ITER_END;
    } else {
      if (nullptr != membership_collector_) {
        for (int64_t i = 0; i < dim_count; ++i) {
          if ((matches & (uint64_t(1) << i)) != 0) {
            next_iters[next_round_cnt_++] = i;
          }
        }
        double relevance = 0.0;
        bool need_project = false;
        const ObDatum &id = *ids[minimum.iter_idx_];
        if (OB_FAIL(membership_collector_->evaluate_membership(
            dim_count, matches, iter_param_->dim_weights_, iter_param_->field_boost_,
            relevance, need_project))) {
        } else if (need_project && OB_FAIL(process_collected_row(id, relevance))) {
        } else if (need_project && OB_FAIL(filter_on_demand(count, relevance, need_project))) {
        } else if (need_project && OB_FAIL(cache_result(count, id, relevance))) {
        }
        return ret;
      }
      // Consume all equal heads together. Other heads remain valid until the
      // next round that advances their iterator, including across output batches.
      for (int64_t i = 0; OB_SUCC(ret) && i < dim_count; ++i) {
        if (0 != (matches & (uint64_t(1) << i))) {
          double score = 0.0;
          if (OB_FAIL(iters[i]->get_curr_score(score))) {
          } else {
            if (nullptr != iter_param_->dim_weights_) {
              score *= iter_param_->field_boost_ * iter_param_->dim_weights_->at(i);
            }
            if (OB_FAIL(relevance_collector_->collect_one_dim(i, score))) {
            } else {
              next_iters[next_round_cnt_++] = i;
            }
          }
        }
      }
      double relevance = 0.0;
      bool need_project = false;
      const ObDatum &id = *ids[minimum.iter_idx_];
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(relevance_collector_->get_result(relevance, need_project))) {
      } else if (need_project && OB_FAIL(process_collected_row(id, relevance))) {
      } else if (need_project && OB_FAIL(filter_on_demand(count, relevance, need_project))) {
      } else if (need_project && OB_FAIL(cache_result(count, id, relevance))) {
      }
    }
  }
  return ret;
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  // The qualifying small-dimension unscored natural-language merge spends one
  // output row per round, so the batch loop runs here and each round takes a
  // qualified (non-virtual) call to this class's merge round. Every other mode
  // keeps the base implementation with its virtual rounds; the two loops stay
  // step-for-step identical apart from that dispatch.
  if (nullptr == iter_param_ || nullptr != iter_param_->relevance_expr_
      || nullptr == dim_iters_ || dim_iters_->count() > 4
      || mode_flag_ != sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE
      || function_lookup_mode_) {
    return ObSRDaaTIterImpl::get_next_rows(capacity, count);
  }
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(0 == capacity)) {
    count = 0;
  } else if (0 == dim_iters_->count()) {
    ret = OB_ITER_END;
  } else if (iter_param_->limit_param_->is_valid() && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(ObTextDaaTIter::pre_process())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    } else {
      count = 0;
    }
  } else {
    count = 0;
    const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
    while (OB_SUCC(ret) && count < real_capacity) {
      if (OB_FAIL(ObTextDaaTIter::do_one_merge_round(count))) {
        if (OB_UNLIKELY(OB_ITER_END != ret)) {
        }
      }
    }

    if (OB_SUCC(ret) || OB_ITER_END == ret) {
      if (count > 0 && OB_FAIL(project_results(count))) {
      }
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
