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

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{

namespace
{
// TOKENIZE() derives two values from its literal arguments on every evaluation:
//  - the canonical "name.version" parser name, and
//  - the canonical parser property JSON (a full JSON parse + rebuild + print).
// Both are pure functions of the input strings, and in practice the same arguments are
// evaluated once per row, so memoize the last result per thread to keep the JSON round
// trip out of the hot path.
class ObFtsTokenizeParamCache final
{
public:
  ObFtsTokenizeParamCache()
    : name_alloc_(lib::ObMemAttr("FtsTkParamName")),
      property_alloc_(lib::ObMemAttr("FtsTkParamProp")),
      name_key_(),
      name_value_(),
      property_parser_key_(),
      property_key_(),
      property_value_(),
      name_cached_(false),
      property_cached_(false)
  {
  }
  ~ObFtsTokenizeParamCache() = default;

  bool name_hit(const common::ObString &input, common::ObString &value) const
  {
    const bool hit = name_cached_ && name_key_ == input;
    if (hit) {
      value = name_value_;
    }
    return hit;
  }
  // Returns false when the cache is full and the caller has to fall back to its
  // own allocator instead of caching.
  bool put_name(const common::ObString &input, const char *value, const int64_t value_len,
                common::ObString &stored_value)
  {
    bool stored = false;
    if (name_alloc_.used() < MAX_CACHE_BYTES) {
      char *value_buf = static_cast<char *>(name_alloc_.alloc(MAX(value_len, 1)));
      char *key_buf = OB_NOT_NULL(value_buf)
                          ? static_cast<char *>(name_alloc_.alloc(MAX(input.length(), 1)))
                          : nullptr;
      if (OB_NOT_NULL(value_buf) && OB_NOT_NULL(key_buf)) {
        if (value_len > 0) {
          MEMCPY(value_buf, value, value_len);
        }
        if (input.length() > 0) {
          MEMCPY(key_buf, input.ptr(), input.length());
        }
        name_value_.assign_ptr(value_buf, static_cast<int32_t>(value_len));
        name_key_.assign_ptr(key_buf, input.length());
        name_cached_ = true;
        stored = true;
        stored_value = name_value_;
      }
    }
    if (!stored) {
      // never leave a half-updated key behind: a later lookup must not hit on it
      name_cached_ = false;
    }
    return stored;
  }
  bool property_hit(const common::ObString &parser_name, const common::ObString &input,
                    common::ObString &value) const
  {
    const bool hit = property_cached_ && property_parser_key_ == parser_name
                     && property_key_ == input;
    if (hit) {
      value = property_value_;
    }
    return hit;
  }
  // The rebuilt properties depend on the parser as well as on the user properties, so
  // both are part of the key.
  bool put_property(const common::ObString &parser_name, const common::ObString &input,
                    const common::ObString &value, common::ObString &stored_value)
  {
    bool stored = false;
    const int64_t len = value.length();
    if (property_alloc_.used() < MAX_CACHE_BYTES) {
      char *value_buf = static_cast<char *>(property_alloc_.alloc(MAX(len, 1)));
      char *key_buf = OB_NOT_NULL(value_buf)
                          ? static_cast<char *>(property_alloc_.alloc(MAX(input.length(), 1)))
                          : nullptr;
      char *parser_buf = OB_NOT_NULL(key_buf)
                             ? static_cast<char *>(
                                   property_alloc_.alloc(MAX(parser_name.length(), 1)))
                             : nullptr;
      if (OB_NOT_NULL(value_buf) && OB_NOT_NULL(key_buf) && OB_NOT_NULL(parser_buf)) {
        if (len > 0) {
          MEMCPY(value_buf, value.ptr(), len);
        }
        if (input.length() > 0) {
          MEMCPY(key_buf, input.ptr(), input.length());
        }
        if (parser_name.length() > 0) {
          MEMCPY(parser_buf, parser_name.ptr(), parser_name.length());
        }
        property_value_.assign_ptr(value_buf, static_cast<int32_t>(len));
        property_key_.assign_ptr(key_buf, input.length());
        property_parser_key_.assign_ptr(parser_buf, parser_name.length());
        property_cached_ = true;
        stored = true;
        stored_value = property_value_;
      }
    }
    if (!stored) {
      property_cached_ = false;
    }
    return stored;
  }

private:
  static constexpr int64_t MAX_CACHE_BYTES = 64L << 10;
  common::ObArenaAllocator name_alloc_;
  common::ObArenaAllocator property_alloc_;
  common::ObString name_key_;
  common::ObString name_value_;
  common::ObString property_parser_key_;
  common::ObString property_key_;
  common::ObString property_value_;
  bool name_cached_;
  bool property_cached_;
};

ObFtsTokenizeParamCache &get_tokenize_param_cache()
{
  static thread_local ObFtsTokenizeParamCache cache;
  return cache;
}
} // namespace

ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
  } else if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                     ctx,
                                                     temp_allocator,
                                                     json_result,
                                                     expr_datum))) {
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;

  ObArenaAllocator tmp_parse_alloc(ObMemAttr("Tmp buffer"));

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tokenize_helper.init(&allocator, param.parser_name_, param.properties_))) {
  } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(param.reform_parser_properties(param.properties_))) {
  } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;
  char parser_name_buf[storage::OB_FT_PARSER_NAME_LENGTH];
  ObFtsTokenizeParamCache &cache = get_tokenize_param_cache();

  if (cache.name_hit(input_str, param.parser_name_)) {
    // reuse the name resolved for the previous evaluation
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, sizeof(parser_name_buf)))) {
  } else if (cache.put_name(input_str, parser_name_buf, STRLEN(parser_name_buf), param.parser_name_)) {
    // resolved once, reused by the following rows
  } else {
    char *name_buf = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH));
    if (OB_ISNULL(name_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(name_buf, parser_name_buf, STRLEN(parser_name_buf) + 1);
      param.parser_name_ = ObString::make_string(name_buf);
    }
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else if (is_null || OB_ISNULL(base)) {
      // NULL parser properties mean "no properties": keep the parser defaults instead of
      // dereferencing the missing document.
    } else if (ObJsonNodeType::J_ARRAY != base->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
    } else {
      for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
        ObIJsonBase *node = nullptr;
        if (OB_FAIL(base->get_array_element(i, node))) {
        } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(param.parse_json_param(node))) {
        }
      } // for
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;
  ObFtsTokenizeParamCache &cache = get_tokenize_param_cache();
  ObString formatted;

  // The rebuilt properties only depend on (parser name, user properties), so evaluating
  // them once per distinct argument pair is enough: this used to run a JSON parse, a
  // rebuild and a JSON print for every single row.
  if (cache.property_hit(parser_name_, properties, properties_)) {
    // reused
  } else if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_properties.to_format_json(allocator_, formatted))) {
  } else if (!cache.put_property(parser_name_, properties, formatted, properties_)) {
    // cache full: fall back to the per-evaluation allocator
    properties_ = formatted;
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
