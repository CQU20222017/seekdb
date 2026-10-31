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
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

namespace
{

// one rerank result answered by the model, the index is the index of the document
// inside the request the result belongs to
struct ObRerankItem
{
  int64_t index_;
  double score_;
  ObJsonObject *result_obj_;
  TO_STRING_KV(K_(index), K_(score));
};

// the providers do not use the same name for the score field, all the known names
// are accepted so that no provider is bound to a hardcoded field
int get_rerank_score(ObJsonObject *result_obj, double &score)
{
  int ret = OB_SUCCESS;
  static const char *SCORE_KEYS[] = {"relevance_score", "score"};
  score = 0;
  bool is_found = false;
  for (int i = 0; i < ARRAYSIZEOF(SCORE_KEYS) && !is_found; i++) {
    ObJsonNode *score_node = result_obj->get_value(SCORE_KEYS[i]);
    if (OB_ISNULL(score_node)) {
      continue;
    } else if (ObJsonNodeType::J_DOUBLE == score_node->json_type()) {
      score = static_cast<ObJsonDouble *>(score_node)->value();
      is_found = true;
    } else if (ObJsonNodeType::J_INT == score_node->json_type()) {
      score = static_cast<ObJsonInt *>(score_node)->get_int();
      is_found = true;
    } else {
      ret = OB_INVALID_DATA;
      LOG_WARN("the rerank score is not a number", K(ret));
    }
  }
  return ret;
}

// check the results answered by the model, every document has to be scored exactly once
int parse_rerank_items(ObJsonArray *model_result_array, const int64_t doc_count, ObArray<ObRerankItem> &items)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model_result_array)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the rerank result is null", K(ret));
    return ret;
  }
  const int64_t count = model_result_array->element_count();
  if (count != doc_count) {
    ret = OB_INVALID_DATA;
    LOG_WARN("the number of the rerank results does not match the number of the documents",
             K(ret), K(count), K(doc_count));
    return ret;
  }
  ObArray<bool> visited;
  for (int64_t i = 0; OB_SUCC(ret) && i < doc_count; i++) {
    if (OB_FAIL(visited.push_back(false))) {
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
    ObJsonNode *result_node = model_result_array->get_value(i);
    if (OB_ISNULL(result_node) || ObJsonNodeType::J_OBJECT != result_node->json_type()) {
      ret = OB_INVALID_DATA;
      LOG_WARN("the rerank result is not an object", K(ret), K(i));
    } else {
      ObJsonObject *result_obj = static_cast<ObJsonObject *>(result_node);
      ObJsonNode *index_node = result_obj->get_value("index");
      double score = 0;
      if (OB_ISNULL(index_node) || ObJsonNodeType::J_INT != index_node->json_type()) {
        ret = OB_INVALID_DATA;
        LOG_WARN("the rerank result has no valid index", K(ret), K(i));
      } else if (OB_FAIL(get_rerank_score(result_obj, score))) {
      } else {
        const int64_t index = static_cast<ObJsonInt *>(index_node)->get_int();
        if (index < 0 || index >= doc_count) {
          ret = OB_INVALID_DATA;
          LOG_WARN("the index of the rerank result is out of range", K(ret), K(index), K(doc_count));
        } else if (visited.at(index)) {
          ret = OB_INVALID_DATA;
          LOG_WARN("the index of the rerank result is duplicated", K(ret), K(index));
        } else {
          visited.at(index) = true;
          ObRerankItem item;
          item.index_ = index;
          item.score_ = score;
          item.result_obj_ = result_obj;
          if (OB_FAIL(items.push_back(item))) {
          }
        }
      }
    }
  }
  return ret;
}

// the results are ordered by the score in descending order, the documents sharing
// the same score keep the order they have in the request
bool rerank_item_is_greater(const ObRerankItem &left, const ObRerankItem &right)
{
  if (left.score_ != right.score_) {
    return left.score_ > right.score_;
  }
  return left.index_ < right.index_;
}

void sort_rerank_items(ObArray<ObRerankItem> &items)
{
  for (int64_t i = 1; i < items.count(); i++) {
    ObRerankItem item = items.at(i);
    int64_t j = i - 1;
    while (j >= 0 && rerank_item_is_greater(item, items.at(j))) {
      items.at(j + 1) = items.at(j);
      j--;
    }
    items.at(j + 1) = item;
  }
}

} // namespace

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc,
                    T_FUN_SYS_AI_RERANK,
                    N_AI_RERANK,
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL,
                    NOT_ROW_DIMENSION)
{
}

ObExprAIRerank::~ObExprAIRerank()
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid param type", K(ret), K(types_stack[MODEL_IDX]));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid param type", K(ret), K(types_stack[QUERY_IDX]));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid param type", K(ret), K(types_stack[DOC_KEY_IDX]));
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
    LOG_WARN("evaluate parameters failed", K(ret));
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("model id or query or documents is null", K(ret));
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);

    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    ObArray<ObString> header_array;
    ObJsonArray *document_array = nullptr;
    ObJsonArray *result_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    bool is_null_result = false;
    // a null doc key is the same as no doc key: the documents are strings
    const bool has_doc_key = NULL != arg_doc_key && !arg_doc_key->is_null();
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
          j_base, is_null_result))) {
    } else if (OB_ISNULL(j_base)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("j_base is null", K(ret));
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("document_array is not array", K(ret));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_base))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("document_array is null", K(ret));
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("document_array is empty", K(ret));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i++) {
        ObJsonNode *doc_node = document_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("doc_node is null", K(ret));
        } else if (has_doc_key && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("doc_node is not object", K(ret));
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else if (!has_doc_key && doc_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("doc_node is not string", K(ret));
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
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
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_header(temp_allocator, *info, *endpoint_info, header_array))) {
    } else if (has_doc_key) {
      ObString doc_key = arg_doc_key->get_string();
      if (OB_FAIL(eval_ai_rerank_with_doc_key(expr, ctx, temp_allocator, model_id,
                  query, document_array, doc_key, *info, *endpoint_info, header_array, res))) {
      }
    } else {
      // the documents are sent to the model batch by batch, the results of all the
      // batches are merged into one ordered result
      ObArray<ObRerankItem> all_items;
      const int64_t doc_count = document_array->element_count();
      for (int64_t begin = 0; OB_SUCC(ret) && begin < doc_count; begin += RERANK_BATCH_SIZE) {
        const int64_t end = begin + RERANK_BATCH_SIZE < doc_count ? begin + RERANK_BATCH_SIZE : doc_count;
        ObJsonArray *batch_document_array = nullptr;
        ObJsonArray *batch_result_array = nullptr;
        ObArray<ObRerankItem> batch_items;
        if (OB_FAIL(construct_batch_document_array(temp_allocator, document_array, begin, end, batch_document_array))) {
        } else if (OB_FAIL(inner_eval_ai_rerank(temp_allocator, *info, *endpoint_info, header_array,
                     query, batch_document_array, batch_result_array))) {
        } else if (OB_FAIL(parse_rerank_items(batch_result_array, end - begin, batch_items))) {
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < batch_items.count(); i++) {
            ObRerankItem item = batch_items.at(i);
            // the index answered by the model is the index inside the batch
            item.index_ = item.index_ + begin;
            if (NULL != item.result_obj_) {
              ObJsonNode *index_node = item.result_obj_->get_value(INDEX_KEY);
              if (NULL != index_node && ObJsonNodeType::J_INT == index_node->json_type()) {
                static_cast<ObJsonInt *>(index_node)->set_value(item.index_);
              }
            }
            if (OB_FAIL(all_items.push_back(item))) {
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        sort_rerank_items(all_items);
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(temp_allocator, result_array))) {
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < all_items.count(); i++) {
            if (OB_FAIL(result_array->append(all_items.at(i).result_obj_))) {
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        ObString raw_str;
        if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
        } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank_with_doc_key(const ObExpr &expr, ObEvalCtx &ctx, ObIAllocator &allocator,
                                                ObString& model_id, ObString& query, ObJsonArray *document_array,
                                                ObString& doc_key, const ObAIFuncExprInfo &info,
                                                const share::ObAiModelEndpointInfo &endpoint_info,
                                                ObArray<ObString> &header_array, ObDatum &res)
{
  INIT_SUCC(ret);
  ObJsonArray *doc_array = nullptr;
  ObJsonArray *result_array = nullptr;
  ObJsonArray *sorted_document_array = nullptr;
  if (OB_FAIL(get_doc_array_from_documents_array_with_key(allocator, document_array, doc_key, doc_array))) {
  } else {
    // the documents are sent to the model batch by batch as well
    ObArray<ObRerankItem> all_items;
    const int64_t doc_count = doc_array->element_count();
    for (int64_t begin = 0; OB_SUCC(ret) && begin < doc_count; begin += RERANK_BATCH_SIZE) {
      const int64_t end = begin + RERANK_BATCH_SIZE < doc_count ? begin + RERANK_BATCH_SIZE : doc_count;
      ObJsonArray *batch_document_array = nullptr;
      ObJsonArray *batch_result_array = nullptr;
      ObArray<ObRerankItem> batch_items;
      if (OB_FAIL(construct_batch_document_array(allocator, doc_array, begin, end, batch_document_array))) {
      } else if (OB_FAIL(inner_eval_ai_rerank(allocator, info, endpoint_info, header_array,
                   query, batch_document_array, batch_result_array))) {
      } else if (OB_FAIL(parse_rerank_items(batch_result_array, end - begin, batch_items))) {
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < batch_items.count(); i++) {
          ObRerankItem item = batch_items.at(i);
          item.index_ = item.index_ + begin;
          if (OB_FAIL(all_items.push_back(item))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      sort_rerank_items(all_items);
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
      } else {
        // the documents are answered in the order given by the model, the score of
        // the model is added to every document
        for (int64_t i = 0; OB_SUCC(ret) && i < all_items.count(); i++) {
          const ObRerankItem &item = all_items.at(i);
          ObJsonNode *origin_node = document_array->get_value(item.index_);
          if (OB_ISNULL(origin_node) || ObJsonNodeType::J_OBJECT != origin_node->json_type()) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("origin document is null", K(ret), K(item.index_));
          } else {
            ObJsonObject *origin_obj = static_cast<ObJsonObject *>(origin_node);
            ObJsonObject *new_obj = nullptr;
            if (OB_ISNULL(new_obj = static_cast<ObJsonObject *>(origin_obj->clone(&allocator, true)))) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("new_obj is null", K(ret));
            } else {
              ObJsonDouble *score_obj = nullptr;
              if (OB_ISNULL(score_obj = OB_NEWx(ObJsonDouble, &allocator, item.score_))) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
                LOG_WARN("failed to allocate memory for the score", K(ret));
              } else if (OB_FAIL(new_obj->add(MODEL_SCORE_KEY, score_obj))) {
              } else if (OB_FAIL(result_array->append(new_obj))) {
              }
            }
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObString raw_str;
    if (OB_FAIL(result_array->get_raw_binary(raw_str, &allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("document_object_array is null", K(ret));
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("doc_key is empty", K(ret));
  } else {
    ObJsonArray *doc_array_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
    } else {
      int64_t count = document_object_array->element_count();
      ObJsonNode *doc_node = nullptr;
      ObJsonNode *doc_value_node = nullptr;
      ObJsonObject *doc_obj = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        doc_node = document_object_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("doc_node is null", K(ret));
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
        } else if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("doc_node is not object", K(ret));
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          doc_obj = static_cast<ObJsonObject *>(doc_node);
          doc_value_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(doc_value_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("doc_value_node is null", K(ret));
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("doc_value_node is not string", K(ret));
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      doc_array = doc_array_obj;
    }
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator, ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; i < end_idx; i++) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator,
                                          const ObAIFuncExprInfo &info,
                                          const share::ObAiModelEndpointInfo &endpoint_info,
                                          ObArray<ObString> &header_array,
                                          ObString &query,
                                          ObJsonArray *document_array,
                                          ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *res = nullptr;
  ObJsonObject *config_json = nullptr;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient ai_client;
  if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, info, endpoint_info, query, document_array, config_json, body))) {
  } else if (OB_FAIL(ai_client.send_post(allocator, endpoint_info.get_url(), header_array, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint_info, http_response, response))) {
  } else if (response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("response is not array", K(ret));
  } else if (OB_ISNULL(res = static_cast<ObJsonArray *>(response))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("res is null", K(ret));
  } else {
    result_array = res;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx,
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (NULL != model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR
  //     && NULL != expr_cg_ctx.schema_guard_)) {
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
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
