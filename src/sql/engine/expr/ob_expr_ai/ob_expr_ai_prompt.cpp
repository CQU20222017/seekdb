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
#include "ob_expr_ai_prompt.h"
#include "ob_ai_func.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

// a blob value is a byte string as well, it is read through the lob reader
static bool is_lob_type(const ObObjType type)
{
  return ob_is_text_tc(type);
}

ObExprAIPrompt::ObExprAIPrompt(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc,
                    T_FUN_SYS_AI_PROMPT,
                    N_AI_PROMPT,
                    MORE_THAN_ZERO,
                    VALID_FOR_GENERATED_COL,
                    NOT_ROW_DIMENSION)
{
}

ObExprAIPrompt::~ObExprAIPrompt()
{
}

int ObExprAIPrompt::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;
  ObExprResType template_type = types_stack[0];
  if (ob_is_string_tc(template_type.get_type())) {
    types_stack[0].set_calc_type(ObVarcharType);
    types_stack[0].set_collation_type(CS_TYPE_UTF8MB4_BIN);
  } else if (is_lob_type(template_type.get_type())) {
    // A character lob or a blob template is read at runtime through the lob reader, so
    // the declared type and collation must stay untouched: rewriting them would make the
    // engine convert the bytes instead of handing them over verbatim.
  } else {
    // No custom user error here: the baseline expects the standard message of
    // OB_ERR_INVALID_TYPE_FOR_OP ("Invalid data type for the operation").
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
    LOG_WARN("invalid template type", K(ret), K(template_type.get_type()));
  }

  for (int64_t i = 1; i < param_num && OB_SUCC(ret); i++) {
    ObObjType arg_type = types_stack[i].get_type();
    if (ob_is_string_tc(arg_type)) {
      types_stack[i].set_calc_type(ObVarcharType);
      types_stack[i].set_collation_type(CS_TYPE_UTF8MB4_BIN);
    } else if (is_lob_type(arg_type)) {
      // the lob argument is read at runtime, its type and collation are kept
    } else if (ob_is_json(arg_type)) {
      // the json argument is kept as a json value
    } else {
      ret = OB_ERR_INVALID_TYPE_FOR_OP;
      LOG_WARN("invalid data type", K(ret), K(arg_type));
    }
  }

  if (OB_SUCC(ret)) {
    type.set_json();
    type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
  }
  return ret;
}

int ObExprAIPrompt::eval_ai_prompt(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *template_datum = NULL;
  ObExpr *arg0 = expr.args_[0];
  ObString template_str;
  ObJsonArray *args_array = NULL;
  ObJsonObject *prompt_object = NULL;
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);

  MultimodeAlloctor tmp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_PROMPT));

  if (OB_FAIL(tmp_allocator.eval_arg(arg0, ctx, template_datum))) {
  } else if (template_datum->is_null()) {
    // a null template is not a string argument: same error class as any other
    // unsupported type, without a custom message so the standard text is reported
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
    LOG_WARN("null template argument", K(ret), K(arg0->datum_meta_.type_));
  } else if (is_lob_type(arg0->datum_meta_.type_)) {
    // the template may be stored as a lob, the real data has to be read
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, tmp_allocator,
                *template_datum, arg0->datum_meta_, arg0->obj_meta_.has_lob_header(), template_str))) {
    }
  } else {
    template_str = template_datum->get_string();
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(tmp_allocator, args_array))) {
  } else {
    for (int64_t i = 1; i < expr.arg_cnt_ && OB_SUCC(ret); i++) {
      ObDatum *datum = NULL;
      ObExpr *arg = expr.args_[i];
      ObString arg_str;
      ObJsonString *arg_json_str = NULL;
      ObIJsonBase *arg_json_base = NULL;
      if (OB_FAIL(tmp_allocator.eval_arg(arg, ctx, datum))) {
      } else if (ob_is_json(arg->datum_meta_.type_)) {
        // the json argument is kept as a json value
        bool is_null = false;
        if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, tmp_allocator, i, arg_json_base, is_null))) {
        } else if (is_null || OB_ISNULL(arg_json_base)) {
          ret = OB_ERR_INVALID_TYPE_FOR_OP;
          LOG_WARN("null json argument", K(ret), K(i));
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_node_copy(tmp_allocator, arg_json_base, arg_json_base))) {
        } else if (OB_FAIL(args_array->append(static_cast<ObJsonNode *>(arg_json_base)))) {
        }
      } else if (datum->is_null()) {
        ret = OB_ERR_INVALID_TYPE_FOR_OP;
        LOG_WARN("invalid data type", K(ret), K(arg->datum_meta_.type_));
      } else if (is_lob_type(arg->datum_meta_.type_)) {
        // the argument may be stored as a lob, the real data has to be read
        if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, tmp_allocator,
                    *datum, arg->datum_meta_, arg->obj_meta_.has_lob_header(), arg_str))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(tmp_allocator, arg_str, arg_json_str))) {
        } else if (OB_FAIL(args_array->append(arg_json_str))) {
        }
      } else {
        arg_str = datum->get_string();
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(tmp_allocator, arg_str, arg_json_str))) {
        } else if (OB_FAIL(args_array->append(arg_json_str))) {
        }
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncPromptObjectUtils::construct_prompt_object(tmp_allocator, template_str, args_array, prompt_object))) {
  } else {
    ObString raw_str;
    if (OB_FAIL(prompt_object->get_raw_binary(raw_str, &tmp_allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
    }
  }
  return ret;
}



int ObExprAIPrompt::cg_expr(ObExprCGCtx &expr_cg_ctx,
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  int ret = OB_SUCCESS;
  rt_expr.eval_func_ = ObExprAIPrompt::eval_ai_prompt;
  return ret;
}

} // namespace sql
} // namespace oceanbase
