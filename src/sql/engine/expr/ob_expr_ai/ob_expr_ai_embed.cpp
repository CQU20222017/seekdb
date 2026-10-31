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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

// build the request config of the embedding request, the dimension is only sent
// when it is known, otherwise the model decides the dimension of its answer
static int build_embed_config(ObIAllocator &allocator, const int64_t dimension, ObJsonObject *&config)
{
  int ret = OB_SUCCESS;
  config = nullptr;
  if (dimension > 0) {
    ObJsonInt *dim_json = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimension, dim_json))) {
    } else if (OB_FAIL(config->add("dimensions", dim_json))) {
    }
  }
  return ret;
}

ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc,
                        T_FUN_SYS_AI_EMBED,
                        N_AI_EMBED,
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL,
                        NOT_ROW_DIMENSION)
{
}

ObExprAIEmbed::~ObExprAIEmbed()
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("dimension parameter must be an integer, not a decimal or float", K(ret), K(types_stack[DIM_IDX].get_type()));
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
    LOG_WARN("evaluate parameters failed", K(ret));
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    // the null input is propagated to the result
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);

    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    int64_t dim = 0;
    ObJsonObject *config = nullptr;
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("model id or input is empty", K(ret));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    ObAIFuncUtils::ObAIEndpointParams endpoint_params;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      if (arg_dim->is_null()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("dimension parameter is null", K(ret));
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter is null");
        res.set_null();
      } else {
        dim = arg_dim->get_int();
        if (dim <= 0) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("dimension parameter must be a positive integer", K(ret), K(dim));
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
          res.set_null();
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("AI endpoint resolver is unavailable", K(ret));
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else if (OB_FAIL(ObAIFuncUtils::parse_endpoint_params(
                   temp_allocator, endpoint_info->get_parameters(), endpoint_params))) {
    } else if (dim <= 0) {
      // the dimension configured on the endpoint is used as the default one
      dim = endpoint_params.dimensions_;
    }
    if (OB_SUCC(ret) && OB_FAIL(build_embed_config(temp_allocator, dim, config))) {
    }
    if (OB_SUCC(ret)) {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx, const ObBitVector &skip, const int64_t batch_size)
{
  INIT_SUCC(ret);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObDatum *res_datums = expr.locate_batch_datums(ctx);
  if (OB_FAIL(expr.args_[MODEL_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (OB_FAIL(expr.args_[CONTENT_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (expr.arg_cnt_ == 3 && OB_FAIL(expr.args_[DIM_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else {
    ObDatumVector model_datums = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector content_datums = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector dim_datums;
    if (expr.arg_cnt_ == 3) {
      dim_datums = expr.args_[DIM_IDX]->locate_expr_datumvector(ctx);
    }
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    // the rows are grouped by the model and the dimension, so that all the inputs
    // of one request share the same request body
    ObArray<ObString> group_models;
    ObArray<int64_t> group_dims;
    ObArray<int64_t> group_row_begin;
    ObArray<int64_t> group_row_count;
    ObArray<int64_t> row_indexes;
    ObArray<ObString> row_contents;
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; i++) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      }
      ObDatum *model_datum = model_datums.at(i);
      ObDatum *content_datum = content_datums.at(i);
      if (OB_ISNULL(model_datum) || OB_ISNULL(content_datum)) {
        continue;
      }
      if (model_datum->is_null() || content_datum->is_null()) {
        // the null input is propagated to the result
        res_datums[i].set_null();
        eval_flags.set(i);
        continue;
      }
      ObString model_id = model_datum->get_string();
      ObString content = content_datum->get_string();
      if (model_id.empty() || content.empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("model id or input is empty", K(ret));
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
        res_datums[i].set_null();
        eval_flags.set(i);
        continue;
      }
      int64_t dim = 0;
      if (expr.arg_cnt_ == 3) {
        ObDatum *dim_datum = dim_datums.at(i);
        if (OB_ISNULL(dim_datum) || dim_datum->is_null()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("dimension parameter is null", K(ret));
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter is null");
          res_datums[i].set_null();
          eval_flags.set(i);
          continue;
        }
        dim = dim_datum->get_int();
        if (dim <= 0) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("dimension parameter must be a positive integer", K(ret), K(dim));
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
          res_datums[i].set_null();
          eval_flags.set(i);
          continue;
        }
      }
      int64_t group_idx = -1;
      for (int64_t g = 0; g < group_models.count(); g++) {
        if (group_dims.at(g) == dim && 0 == group_models.at(g).compare(model_id)) {
          group_idx = g;
          break;
        }
      }
      if (group_idx < 0) {
        if (OB_FAIL(group_models.push_back(model_id))) {
        } else if (OB_FAIL(group_dims.push_back(dim))) {
        } else if (OB_FAIL(group_row_begin.push_back(row_indexes.count()))) {
        } else if (OB_FAIL(group_row_count.push_back(0))) {
        } else {
          group_idx = group_models.count() - 1;
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(row_indexes.push_back(i))) {
        } else if (OB_FAIL(row_contents.push_back(content))) {
        } else {
          group_row_count.at(group_idx) = group_row_count.at(group_idx) + 1;
        }
      }
    }

    for (int64_t g = 0; OB_SUCC(ret) && g < group_models.count(); g++) {
      ObString model_id = group_models.at(g);
      int64_t dim = group_dims.at(g);
      const int64_t row_begin = group_row_begin.at(g);
      const int64_t row_count = group_row_count.at(g);
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo resolved_endpoint;
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      ObAIFuncUtils::ObAIEndpointParams endpoint_params;
      if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
      } else if (OB_ISNULL(endpoint_resolver)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("AI endpoint resolver is unavailable", K(ret));
      } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                     model_id, temp_allocator, resolved_endpoint))) {
      } else if (OB_FAIL(ObAIFuncUtils::parse_endpoint_params(
                     temp_allocator, resolved_endpoint.get_parameters(), endpoint_params))) {
      } else if (dim <= 0) {
        dim = endpoint_params.dimensions_;
      }
      int64_t max_batch_size = endpoint_params.batch_size_ > 0
                               ? endpoint_params.batch_size_
                               : ObAIFuncUtils::DEFAULT_EMBED_BATCH_SIZE;
      for (int64_t begin = 0; OB_SUCC(ret) && begin < row_count; begin += max_batch_size) {
        const int64_t end = begin + max_batch_size < row_count ? begin + max_batch_size : row_count;
        ObArray<ObString> batch_contents;
        for (int64_t i = begin; i < end; i++) {
          if (OB_FAIL(batch_contents.push_back(row_contents.at(row_begin + i)))) {
            break;
          }
        }
        ObJsonObject *config = nullptr;
        ObArray<ObString> batch_results;
        if (OB_SUCC(ret) && OB_FAIL(build_embed_config(temp_allocator, dim, config))) {
        } else if (OB_SUCC(ret)) {
          ObAIFuncModel model(temp_allocator, *info, resolved_endpoint);
          if (OB_FAIL(model.call_dense_embedding_vector_v2(batch_contents, config, batch_results))) {
          } else if (batch_results.count() != end - begin) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("the number of the results does not match the number of the inputs",
                     K(ret), K(batch_results.count()), K(end - begin));
          } else {
            for (int64_t i = begin; i < end; i++) {
              const int64_t row_index = row_indexes.at(row_begin + i);
              res_datums[row_index].set_string(batch_results.at(i));
              eval_flags.set(row_index);
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx,
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOTNULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR
  //     && OB_NOTNULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    // the rows of one table scan are sent to the model within one request
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
