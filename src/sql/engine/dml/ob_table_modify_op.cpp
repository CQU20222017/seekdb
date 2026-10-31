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

#include "ob_table_modify_op.h"
#include "share/ob_autoincrement_service.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "sql/das/ob_das_insert_op.h"
#include "sql/das/ob_das_delete_op.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/dml/ob_dml_service.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
using namespace storage;
using namespace share;
using namespace share::schema;
using namespace observer;
namespace sql
{
int ForeignKeyHandle::do_handle(ObTableModifyOp &op,
                                const ObDMLBaseCtDef &dml_ctdef,
                                ObDMLBaseRtDef &dml_rtdef)
{
  int ret = OB_SUCCESS;
  if (op.need_foreign_key_checks()) {
    const ObExprPtrIArray &old_row = dml_ctdef.old_row_;
    const ObExprPtrIArray &new_row = dml_ctdef.new_row_;
    const bool save_in_ignore_cascading = op.get_exec_ctx().get_das_ctx().in_ignore_cascading_;
    op.get_exec_ctx().get_das_ctx().is_fk_cascading_ = true;
    op.get_exec_ctx().get_das_ctx().in_ignore_cascading_ = dml_ctdef.das_base_ctdef_.is_ignore_;
    if (OB_FAIL(op.check_stack())) {
    }
    for (int i = 0; OB_SUCC(ret) && i < dml_ctdef.fk_args_.count(); i++) {
      const ObForeignKeyArg &fk_arg = dml_ctdef.fk_args_.at(i);
      ObForeignKeyChecker *fk_checker = dml_rtdef.fk_checker_array_.at(i);
      if (OB_SUCC(ret) && !new_row.empty()) {
        if (ACTION_CHECK_EXIST == fk_arg.ref_action_) {
          // insert or update.
          bool is_foreign_key_cascade = ObSQLUtils::is_fk_nested_sql(&op.get_exec_ctx());
          bool in_ignore_cascading = false;
          ObExecContext* parent_ctx = op.get_exec_ctx().get_parent_ctx();
          if (OB_NOT_NULL(parent_ctx)) {
            in_ignore_cascading = parent_ctx->get_das_ctx().in_ignore_cascading_;
          }

          if (is_foreign_key_cascade && !in_ignore_cascading) {
            // nested update can not check parent row.
          } else if (OB_FAIL(check_exist(op, fk_arg, new_row, fk_checker, false, fk_arg.use_das_scan_))) {
          }
        }
      }
      if (OB_SUCC(ret) && !old_row.empty()) {
        if (ACTION_RESTRICT == fk_arg.ref_action_ || ACTION_NO_ACTION == fk_arg.ref_action_) {
          // update or delete.
          bool has_changed = false;
          if (OB_FAIL(value_changed(op, fk_arg.columns_, old_row, new_row, has_changed))) {
          } else if (!has_changed) {
            // nothing.
          } else if (OB_FAIL(check_exist(op, fk_arg, old_row, fk_checker, true, fk_arg.use_das_scan_))) {
          }
        } else if (ACTION_CASCADE == fk_arg.ref_action_) {
          // update or delete.
          bool is_self_ref = false;
          if (OB_FAIL(is_self_ref_row(op.get_eval_ctx(), old_row, fk_arg, is_self_ref))) {
          } else if (new_row.empty() && is_self_ref && op.is_fk_nested_session()) {
            // delete self refercnced row should not cascade delete.
          } else if (OB_FAIL(check_exist_inner_sql(op, fk_arg, old_row, true, true))) {
          } else if (OB_FAIL(cascade(op, fk_arg, old_row, new_row))) {
          } else if (!new_row.empty() && is_self_ref) {
            // we got here only when:
            //  1. handling update operator and
            //  2. foreign key constraint is self reference
            // need to change %new_row and %old_row
            //   (see 
            // xxx_row_res_info helps to restore row. restore row before get_next_row()
            //op.fk_self_ref_row_res_infos_.reset();
            for (int64_t i = 0; OB_SUCC(ret) && i < fk_arg.columns_.count(); i++) {
              const int32_t val_idx = fk_arg.columns_.at(i).idx_;
              const int32_t name_idx = fk_arg.columns_.at(i).name_idx_;
              if (OB_SUCC(ret)) {
                const ObDatum &updated_value = new_row.at(val_idx)->
                                                    locate_expr_datum(op.get_eval_ctx());
                ObDatum &new_row_name_col = new_row.at(name_idx)->
                                                  locate_expr_datum(op.get_eval_ctx());
                ObDatum &old_row_name_col = old_row.at(name_idx)->
                                                  locate_expr_datum(op.get_eval_ctx());

                OX(new_row_name_col = updated_value);
                OX(old_row_name_col = updated_value);
              }
            }
          }
        } else if (ACTION_SET_NULL == fk_arg.ref_action_) {
          if (OB_FAIL(check_exist_inner_sql(op, fk_arg, old_row, true, true))) {
          } else if (OB_FAIL(set_null(op, fk_arg, old_row))) {
          }
        }
      } // if (old_row.is_valid())
    } // for
    //fk cascading end
    op.get_exec_ctx().get_das_ctx().is_fk_cascading_ = false;
    op.get_exec_ctx().get_das_ctx().in_ignore_cascading_ = save_in_ignore_cascading;
  } else {
  }
  return ret;
}

int ForeignKeyHandle::value_changed(ObTableModifyOp &op,
                                    const ObIArray<ObForeignKeyColumn> &columns,
                                    const ObExprPtrIArray &old_row,
                                    const ObExprPtrIArray &new_row,
                                    bool &has_changed)
{
  int ret = OB_SUCCESS;
  has_changed = false;
  if (!old_row.empty() && !new_row.empty()) {
    ObDatum *old_row_col = NULL;
    ObDatum *new_row_col = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && !has_changed && i < columns.count(); ++i) {
      int64_t col_idx = columns.at(i).idx_;
      CK(col_idx < old_row.count());
      CK(col_idx < new_row.count());
      CK(OB_NOT_NULL(old_row.at(col_idx)));
      CK(OB_NOT_NULL(new_row.at(col_idx)));
      OZ(old_row.at(col_idx)->eval(op.get_eval_ctx(), old_row_col));
      OZ(new_row.at(col_idx)->eval(op.get_eval_ctx(), new_row_col));
      OX(has_changed = (false == ObDatum::binary_equal(*old_row_col, *new_row_col)));
    }
  } else {
    has_changed = true;
  }
  return ret;
}

int ForeignKeyHandle::check_exist(ObTableModifyOp &modify_op, const ObForeignKeyArg &fk_arg,
                         const ObExprPtrIArray &row, ObForeignKeyChecker *fk_checker, bool expect_zero, bool use_das_scan)
{
  int ret = OB_SUCCESS;
  DEBUG_SYNC(BEFORE_FOREIGN_KEY_CONSTRAINT_CHECK);
  // insertup and merge_into currently have a core problem when checking foreign keys through das_scan_task.
  // Because both insertup and merge_into have both INSERT and UPDATE semantics,
  // the data may come from the new_row of INSERT or the new_row of UPDATE during the operation.
  // Earlier versions did not consider this problem, so there is a possibility of core
  bool is_insertup = false;
  ObPhyOperatorType op_type = modify_op.get_spec().get_type();
  if (op_type == PHY_INSERT_ON_DUP) {
    is_insertup = true;
  }
  if (use_das_scan && !is_insertup) {
    ret = check_exist_scan_task(modify_op, fk_arg, row, fk_checker);
  } else {
    if (OB_FAIL(check_exist_inner_sql(modify_op, fk_arg, row, expect_zero, true))) {
    } else if (OB_FAIL(check_exist_inner_sql(modify_op, fk_arg, row, expect_zero, false))) {
    }
  }
  return ret;
}

int ForeignKeyHandle::check_exist_scan_task(ObTableModifyOp &modify_op, const ObForeignKeyArg &fk_arg,
                         const ObExprPtrIArray &row, ObForeignKeyChecker *fk_checker)
{
  int ret = OB_SUCCESS;
  bool has_result = false;
  if (OB_ISNULL(fk_checker)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!fk_arg.use_das_scan_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(fk_checker->do_fk_check_single_row(fk_arg.columns_, row, has_result))) {
  } else {
    if (!has_result) {
      ret = OB_ERR_NO_REFERENCED_ROW;
    } else {
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

int ForeignKeyHandle::check_exist_inner_sql(ObTableModifyOp &op,
                                            const ObForeignKeyArg &fk_arg,
                                            const ObExprPtrIArray &row,
                                            bool expect_zero,
                                            bool iter_uncommitted_row)
{
  int ret = OB_SUCCESS;
  const char *select_fmt = "select /*+ no_parallel */ 1 from `%.*s`.`%.*s` where %.*s for update";
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip check exist if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, select_fmt,
                                     database_name.length(), database_name.ptr(),
                                     table_name.length(), table_name.ptr(),
                                     static_cast<int>(where_pos), where_buf))) {
  } else {
    stmt_buf[stmt_pos++] = 0;
  }
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (iter_uncommitted_row) {
      op.get_exec_ctx().get_das_ctx().iter_uncommitted_row_ = true;
    }
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
      } else {
        // must call end_nested_session() if begin_nested_session() success.
        bool is_zero = false;
        if (OB_FAIL(op.execute_read(stmt_buf, res))) {
        } else {
          // must call res.get_result()->close() if execute_read() success.
          if (OB_ISNULL(res.get_result())) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(res.get_result()->next())) {
            if (OB_ITER_END == ret) {
              is_zero = true;
              ret = OB_SUCCESS;
            } else {
            }
          }
          if (OB_SUCC(ret)) {
            if (is_zero != expect_zero) {
              // If it is a self-referencing foreign key new row references new row
              bool is_self_ref = false;
              // The FOR UPDATE check query does not use aggregate functions, so when is_zero is false
              // judge whether only one row is affected by whether the second record can be obtained.
              bool is_affect_only_one = false;
              if (!is_zero && OB_FAIL(res.get_result()->next())) {
                if (OB_ITER_END == ret) {
                  is_affect_only_one = true;
                  ret = OB_SUCCESS;
                } else {
                }
              }
              /**
               *  expect_zero is false when fk_arg.ref_action_ is equal to
               *  ACTION_CHECK_EXIST, if the is_zero != expect_zero condition is
               *  true, then is_zero is true, need to exclude the case of self reference.
               *  other cases return OB_ERR_NO_REFERENCED_ROW.
               *
               *  expect_zero is true when fk_arg.ref_action_ is equal to
               *  ACTION_RESTRICT or ACTION_NO_ACTION, if is_zero != expect_zero condition
               *  is true, then is_zero is false, need to exclude the case of self reference and
               *  only affect one row. other cases return OB_ERR_ROW_IS_REFERENCED.
               */
              if (!iter_uncommitted_row) {
                if (OB_FAIL(is_self_ref_row(op.get_eval_ctx(), row, fk_arg, is_self_ref))) {
                } else if (is_zero && !is_self_ref) {
                  ret = OB_ERR_NO_REFERENCED_ROW;
                } else if (!is_zero) {
                  ret = OB_ERR_ROW_IS_REFERENCED;
                }
              }
            }
          }
          int close_ret = res.get_result()->close();
          if (OB_SUCCESS != close_ret) {
            LOG_WARN("failed to close", K(close_ret));
            if (OB_SUCCESS == ret) {
              ret = close_ret;
            }
          }
        }
        int end_ret = op.end_nested_session();
        if (OB_SUCCESS != end_ret) {
          LOG_WARN("failed to end nested session", K(end_ret));
          if (OB_SUCCESS == ret) {
            ret = end_ret;
          }
        }
      }
    }
  }
  op.get_exec_ctx().get_das_ctx().iter_uncommitted_row_ = false;

  return ret;
}

int ForeignKeyHandle::cascade(ObTableModifyOp &op,
                              const ObForeignKeyArg &fk_arg,
                              const ObExprPtrIArray &old_row,
                              const ObExprPtrIArray &new_row)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (OB_FAIL(op.get_exec_ctx().check_status())) {
  } else if (old_row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, old_row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip cascade if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else {
    if (!new_row.empty()) {
      // update.
      const char *update_fmt = "update `%.*s`.`%.*s` set %.*s where %.*s";
      char *set_buf = nullptr;
      int64_t set_len = 0;
      int64_t set_pos = 0;
      if (OB_FAIL(gen_set(op.get_eval_ctx(), set_buf, set_len, set_pos,
                          fk_arg.columns_, new_row, alloc, op.get_obj_print_params()))) {
      } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, update_fmt,
                                         database_name.length(), database_name.ptr(),
                                         table_name.length(), table_name.ptr(),
                                         static_cast<int>(set_pos), set_buf,
                                         static_cast<int>(where_pos), where_buf))) {
      } else {
        stmt_buf[stmt_pos++] = 0;
      }
    } else {
      // delete.
      const char *delete_fmt = "delete from `%.*s`.`%.*s` where %.*s";
      if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, delete_fmt,
                                  database_name.length(), database_name.ptr(),
                                  table_name.length(), table_name.ptr(),
                                  static_cast<int>(where_pos), where_buf))) {
      } else {
        stmt_buf[stmt_pos++] = 0;
      }
    }
  }
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
    } else {
      if (OB_FAIL(op.execute_write(stmt_buf))) {
      }
      int end_ret = op.end_nested_session();
      if (OB_SUCCESS != end_ret) {
        LOG_WARN("failed to end nested session", K(end_ret));
        if (OB_SUCCESS == ret) {
          ret = end_ret;
        }
      }
    }
  }
  return ret;
}

int ForeignKeyHandle::set_null(ObTableModifyOp &op,
                              const ObForeignKeyArg &fk_arg,
                              const ObExprPtrIArray &old_row)
{
  int ret = OB_SUCCESS;
  
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (old_row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, old_row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip cascade if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else {
    const char *update_fmt = "update `%.*s`.`%.*s` set %.*s where %.*s";
    char *set_buf = nullptr;
    int64_t set_len = 0;
    int64_t set_pos = 0;
    if (OB_FAIL(gen_column_null_value(op.get_eval_ctx(), set_buf, set_len, set_pos, fk_arg.columns_, alloc, op.get_obj_print_params()))) {
    } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, update_fmt,
                                      database_name.length(), database_name.ptr(),
                                        table_name.length(), table_name.ptr(),
                                        static_cast<int>(set_pos), set_buf,
                                        static_cast<int>(where_pos), where_buf))) {
    } else {
      stmt_buf[stmt_pos++] = 0;
    }
  }
  
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
    } else {
      if (OB_FAIL(op.execute_write(stmt_buf))) {
      }
      int end_ret = op.end_nested_session();
      if (OB_SUCCESS != end_ret) {
        LOG_WARN("failed to end nested session", K(end_ret));
        if (OB_SUCCESS == ret) {
          ret = end_ret;
        }
      }
    }
  }

  return ret;
}

int ForeignKeyHandle::gen_set(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                              const ObIArray<ObForeignKeyColumn> &columns,
                              const ObExprPtrIArray &row,
                              ObIAllocator &alloc,
                              const ObObjPrintParams &print_params)
{
  return gen_column_value(eval_ctx, buf, len, pos, columns, row, ", ", alloc, print_params, false);
}

int ForeignKeyHandle::gen_where(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                                const ObIArray<ObForeignKeyColumn> &columns,
                                const ObExprPtrIArray &row,
                                ObIAllocator &alloc,
                                const ObObjPrintParams &print_params)
{
  return gen_column_value(eval_ctx, buf, len, pos, columns, row, " and ", alloc, print_params, true);
}

int ForeignKeyHandle::gen_column_value(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                                       const ObIArray<ObForeignKeyColumn> &columns,
                                       const ObExprPtrIArray &row,
                                       const char *delimiter,
                                       ObIAllocator &alloc,
                                       const ObObjPrintParams &print_params,
                                       bool forbid_null)
{
  const char *column_fmt = "`%.*s` = ";
  int ret = OB_SUCCESS;
  ObDatum *col_datum = NULL;
  if (OB_ISNULL(delimiter) || OB_ISNULL(print_params.tz_info_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (columns.count() <= 0) {
    ret= OB_ERR_UNEXPECTED;
    LOG_ERROR("columns count of fk is zero or less than zero", K(ret), K(columns.count()), K(columns));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
    OZ(row.at(columns.at(i).idx_)->eval(eval_ctx, col_datum));
    if (OB_SUCC(ret)) {
      const ObString &col_name = columns.at(i).name_;
      const ObObjMeta &col_obj_meta = row.at(columns.at(i).idx_)->obj_meta_;
      ObObj col_obj;
      if (forbid_null && col_datum->is_null()) {
        ret = OB_ERR_NULL_VALUE;
        // NO LOG.
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, column_fmt,
                                        col_name.length(), col_name.ptr()))) {
      } else if (OB_FAIL(col_datum->to_obj(col_obj, col_obj_meta))) {
      } else if (OB_FAIL(col_obj.print_sql_literal(buf, len, pos, alloc, print_params))) {
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, "%s", delimiter))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    pos -= STRLEN(delimiter);
    buf[pos++] = 0;
  }
  return ret;
}

int  ForeignKeyHandle::gen_column_null_value(ObEvalCtx &ctx, char *&buf, int64_t &len, int64_t &pos,
                              const common::ObIArray<ObForeignKeyColumn> &columns,
                              common::ObIAllocator &alloc,
                              const common::ObObjPrintParams &print_params)
{
  int ret = OB_SUCCESS;
  const char *column_fmt = "`%.*s` = ";
  const char *delimiter = ", ";
  if (OB_ISNULL(delimiter) || OB_ISNULL(print_params.tz_info_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (columns.count() <= 0) {
    ret= OB_ERR_UNEXPECTED;
    LOG_ERROR("columns count of fk is zero or less than zero", K(ret), K(columns.count()), K(columns));
  }
  ObObj col_obj;
  col_obj.set_null();
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
    if (OB_SUCC(ret)) {
      const ObString &col_name = columns.at(i).name_;
      if (OB_FAIL(databuff_printf(buf, len, pos, alloc, column_fmt,
                                        col_name.length(), col_name.ptr()))) {
      } else if (OB_FAIL(col_obj.print_sql_literal(buf, len, pos, alloc, print_params))) {
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, "%s", delimiter))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    pos -= STRLEN(delimiter);
    buf[pos++] = 0;
  }
  return ret;
}

int ForeignKeyHandle::is_self_ref_row(ObEvalCtx &eval_ctx,
                                      const ObExprPtrIArray &row,
                                      const ObForeignKeyArg &fk_arg,
                                      bool &is_self_ref)
{
  int ret = OB_SUCCESS;
  const common::ObDatumAccessContext *access_ctx = nullptr;
  is_self_ref = fk_arg.is_self_ref_;
  ObDatum *name_col = NULL;
  ObDatum *val_col = NULL;
  if (OB_FAIL(eval_ctx.get_datum_access_ctx(access_ctx))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && is_self_ref && i < fk_arg.columns_.count(); i++) {
    const int32_t name_idx = fk_arg.columns_.at(i).name_idx_;
    const int32_t val_idx = fk_arg.columns_.at(i).idx_;
    ObExprCmpFuncType cmp_func = row.at(name_idx)->basic_funcs_->null_first_cmp_;
    // TODO qubin.qb: uncomment below block revert the defensive check
    //OZ((cmp_func = (row.at(name_idx)->basic_funcs_->null_first_cmp_))
    //   != row.at(val_idx)->basic_funcs_->null_first_cmp_);
    OZ(row.at(name_idx)->eval(eval_ctx, name_col));
    OZ(row.at(val_idx)->eval(eval_ctx, val_col));
    int cmp_ret = 0;
    if (OB_FAIL(cmp_func(*name_col, *val_col, cmp_ret, access_ctx))) {
    } else {
      is_self_ref = (0 == cmp_ret);
    }
  }
  return ret;
}

// make MY_SPEC macro available.
OB_INLINE static const ObTableModifySpec &get_my_spec(const ObTableModifyOp &op)
{
  return op.get_spec();
}

ObTableModifySpec::ObTableModifySpec(common::ObIAllocator &alloc,
                                     const ObPhyOperatorType type)
  : ObOpSpec(alloc, type),
    expr_frame_info_(NULL),
    ab_stmt_id_(nullptr),
    flags_(0),
    das_dop_(0)
{
}

OB_DEF_SERIALIZE(ObTableModifySpec)
{
  int ret = OB_SUCCESS;
  BASE_SER((ObTableModifySpec, ObOpSpec));
  OB_UNIS_ENCODE(flags_);
  OB_UNIS_ENCODE(ab_stmt_id_);
  OB_UNIS_ENCODE(das_dop_);
  return ret;
}

OB_DEF_DESERIALIZE(ObTableModifySpec)
{
  int ret = OB_SUCCESS;
  BASE_DESER((ObTableModifySpec, ObOpSpec));
  OB_UNIS_DECODE(flags_);
  OB_UNIS_DECODE(ab_stmt_id_);
  OB_UNIS_DECODE(das_dop_);
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObTableModifySpec)
{
  int64_t len = 0;
  BASE_ADD_LEN((ObTableModifySpec, ObOpSpec));
  OB_UNIS_ADD_LEN(flags_);
  OB_UNIS_ADD_LEN(ab_stmt_id_);
  OB_UNIS_ADD_LEN(das_dop_);
  return len;
}

OB_SERIALIZE_MEMBER(ObTableModifyOpInput);

ObTableModifyOp::ObTableModifyOp(ObExecContext &ctx,
                                 const ObOpSpec &spec,
                                 ObOpInput *input)
  : ObOperator(ctx, spec, input),
    sql_proxy_(NULL),
    inner_conn_guard_(),
    inner_conn_(NULL),
    saved_conn_(),
    need_foreign_key_check_(false),
    need_close_conn_(false),
    ai_task_table_id_(common::OB_INVALID_ID),
    ai_task_upsert_keys_(),
    ai_task_delete_keys_(),
    ai_task_writer_inited_(false),
    ai_task_ins_ctdef_(ctx.get_allocator()),
    ai_task_ins_rtdef_(),
    ai_task_del_ctdef_(ctx.get_allocator()),
    ai_task_del_rtdef_(),
    ai_task_table_loc_(nullptr),
    ai_task_tablet_loc_(nullptr),
    iter_end_(false),
    dml_rtctx_(eval_ctx_, ctx, *this),
    execute_single_row_(false),
    dml_modify_rows_(ctx.get_allocator()),
    last_store_row_(),
    saved_session_(NULL)
{
  for (int64_t i = 0; i < AI_TASK_SLOT_CNT; ++i) {
    ai_task_col_idx_[i] = -1;
  }
  for (int64_t i = 0; i < ARRAYSIZEOF(ai_task_del_col_idx_); ++i) {
    ai_task_del_col_idx_[i] = -1;
  }
  obj_print_params_ = CREATE_OBJ_PRINT_PARAM(ctx_.get_my_session());
  obj_print_params_.need_cast_expr_ = true;
  // bugfix:
  // in NO_BACKSLASH_ESCAPES, obj_print_sql<ObVarcharType> won't escape.
  // We use skip_escape_ to indicate this case. It will finally be passed to ObHexEscapeSqlStr.
  GET_SQL_MODE_BIT(IS_NO_BACKSLASH_ESCAPES, ctx_.get_my_session()->get_sql_mode(), obj_print_params_.skip_escape_);
}

int ObTableModifyOp::inner_open()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *my_session = GET_MY_SESSION(ctx_);
  if (OB_FAIL(init_foreign_key_operation())) {
  } else if (MY_SPEC.plan_->has_nested_sql() && OB_FAIL(open_inner_conn())) {
  } else if (OB_FAIL(calc_single_table_loc())) {
  } else if (OB_FAIL(check_need_exec_single_row())) {
  } else {
    init_das_dml_ctx();
  }
  return ret;
}

void ObTableModifyOp::clear_dml_evaluated_flag(ObExpr *clear_expr)
{
  if (clear_expr->is_batch_result()) {
    clear_expr->get_evaluated_flags(eval_ctx_).unset(eval_ctx_.get_batch_idx());
  } else {
    clear_expr->get_eval_info(eval_ctx_).clear_evaluated_flag();
  }
  if (0 < clear_expr->parent_cnt_) {
    clear_dml_evaluated_flag(clear_expr->parent_cnt_, clear_expr->parents_);
  }
}

void ObTableModifyOp::clear_dml_evaluated_flag(int64_t parent_cnt, ObExpr **parent_exprs)
{
  for (int64_t i = 0; i < parent_cnt; ++i) {
    clear_dml_evaluated_flag(parent_exprs[i]);
  }
}

void ObTableModifyOp::clear_dml_evaluated_flag()
{
  // Clear the parent expr because the assignment expr may be changed
  for (int64_t i = 0; i < trigger_clear_exprs_.count(); ++i) {
    ObExpr *expr = trigger_clear_exprs_.at(i);
    clear_dml_evaluated_flag(expr);
  }
}

ObDasParallelType ObTableModifyOp::check_das_parallel_type()
{
  ObDasParallelType type = DAS_SERIALIZATION;
  // Local DAS modification runs serially.
  return type;
}

OB_INLINE int ObTableModifyOp::init_das_dml_ctx()
{
  ObSQLSessionInfo *session = GET_MY_SESSION(ctx_);
  const char *label = nullptr;
  switch(MY_SPEC.type_) {
  case PHY_INSERT:
    label = "InsDASCtx";
    break;
  case PHY_UPDATE:
    label = "UpdDASCtx";
    break;
  case PHY_DELETE:
    label = "DelDASCtx";
    break;
  case PHY_LOCK:
    label = "LockDASCtx";
    break;
  default:
    label = "DmlDASCtx";
    break;
  }
  ObMemAttr memattr(label, ObCtxIds::EXECUTE_CTX_ID);
  dml_rtctx_.das_ref_.set_expr_frame_info(nullptr != MY_SPEC.expr_frame_info_
                                        ? MY_SPEC.expr_frame_info_
                                        : &MY_SPEC.plan_->get_expr_frame_info());
  dml_rtctx_.das_ref_.set_mem_attr(memattr);
  dml_rtctx_.das_ref_.get_das_parallel_ctx().set_das_parallel_type(check_das_parallel_type());
  dml_rtctx_.das_ref_.get_das_parallel_ctx().set_das_dop(ctx_.get_das_ctx().get_real_das_dop());
  if (check_das_parallel_type() != DAS_SERIALIZATION) {
    LOG_TRACE("this sql use das parallel submit", K(check_das_parallel_type()));
  }
  return OB_SUCCESS;
}

int ObTableModifyOp::replace_implict_cursor(int64_t affected_rows,
                                            int64_t found_rows,
                                            int64_t matched_rows,
                                            int64_t duplicated_rows)
{
  int ret = OB_SUCCESS;
  bool is_ins_val_opt = ctx_.get_sql_ctx()->is_do_insert_batch_opt();
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (MY_SPEC.ab_stmt_id_ != nullptr && !is_ins_val_opt) {
    ObDatum *stmt_id_datum = nullptr;
    ObImplicitCursorInfo implicit_cursor;
    if (OB_FAIL(prepare_implict_cursor(affected_rows,
                                       found_rows,
                                       matched_rows,
                                       duplicated_rows,
                                       implicit_cursor))) {
    } else if (OB_FAIL(plan_ctx->replace_implicit_cursor_info(implicit_cursor))) {
    }
  }
  return ret;
}

int ObTableModifyOp::prepare_implict_cursor(int64_t affected_rows,
                                            int64_t found_rows,
                                            int64_t matched_rows,
                                            int64_t duplicated_rows,
                                            ObImplicitCursorInfo &implicit_cursor)
{
  int ret = OB_SUCCESS;
  ObDatum *stmt_id_datum = nullptr;
  if (OB_FAIL(MY_SPEC.ab_stmt_id_->eval(eval_ctx_, stmt_id_datum))) {
  } else {
    ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
    int64_t stmt_id = stmt_id_datum->get_int();
    implicit_cursor.stmt_id_ = stmt_id;
    implicit_cursor.found_rows_ += found_rows;
    implicit_cursor.matched_rows_ += matched_rows;
    implicit_cursor.affected_rows_ += affected_rows;
    implicit_cursor.duplicated_rows_ += duplicated_rows;
    implicit_cursor.last_insert_id_ = plan_ctx->get_autoinc_col_value();
  }
  return ret;
}

int ObTableModifyOp::merge_implict_cursor(int64_t affected_rows,
                                          int64_t found_rows,
                                          int64_t matched_rows,
                                          int64_t duplicated_rows)
{
  int ret = OB_SUCCESS;
  bool is_ins_val_opt = ctx_.get_sql_ctx()->is_do_insert_batch_opt();
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (MY_SPEC.ab_stmt_id_ != nullptr && !is_ins_val_opt) {
    ObDatum *stmt_id_datum = nullptr;
    ObImplicitCursorInfo implicit_cursor;
    if (OB_FAIL(prepare_implict_cursor(affected_rows,
                                       found_rows,
                                       matched_rows,
                                       duplicated_rows,
                                       implicit_cursor))) {
    } else if (OB_FAIL(plan_ctx->merge_implicit_cursor_info(implicit_cursor))) {
    }
  }
  return ret;
}

int ObTableModifyOp::inner_switch_iterator()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_switch_iterator())) {
  }

  return ret;
}

int ObTableModifyOp::inner_close()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = ctx_.get_physical_plan_ctx();
  // release cache_handle for auto-increment
  share::ObAutoincrementService &auto_service = share::ObAutoincrementService::get_instance();
  ObIArray<share::AutoincParam> &autoinc_params = plan_ctx->get_autoinc_params();
  for (int64_t i = 0; i < autoinc_params.count(); ++i) {
    if (NULL != autoinc_params.at(i).cache_handle_) {
      auto_service.release_handle(autoinc_params.at(i).cache_handle_);
    }
  }
  close_inner_conn();
  if (dml_rtctx_.das_ref_.has_task()) {
    if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
    } else {
      dml_rtctx_.das_ref_.reset();
    }
  }
  dml_modify_rows_.clear();
  return ret;
}

OB_INLINE int ObTableModifyOp::init_foreign_key_operation()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *phy_plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (OB_ISNULL(phy_plan_ctx)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!phy_plan_ctx->need_foreign_key_checks()) {
    // set fk check even if ret != OB_SUCCESS. see ObTableModifyOp::inner_close()
    need_foreign_key_check_ = false;
  } else {
    need_foreign_key_check_ = MY_SPEC.need_foreign_key_check_;
  }
  return ret;
}

int ObTableModifyOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    iter_end_ = false;
    if (dml_rtctx_.das_ref_.has_task()) {
      if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
      } else {
        dml_rtctx_.reuse();
      }
    }
  }
  if (OB_SUCC(ret)) {
    dml_modify_rows_.clear();
    if (OB_FAIL(calc_single_table_loc())) {
    }
  }
  return ret;
}

int ObTableModifyOp::check_need_exec_single_row() {
  int ret = OB_SUCCESS;
  if (MY_SPEC.is_returning_ && (need_foreign_key_checks() || MY_SPEC.need_trigger_fire_)) {
    execute_single_row_ = true;
  }
  return ret;
}

int ObTableModifyOp::get_gi_task()
{
  int ret = OB_SUCCESS;
  ObGranuleTaskInfo gi_task_info;
  GIPrepareTaskMap *gi_prepare_map = nullptr;
  ObDASCtx &das_ctx = ctx_.get_das_ctx();
  ObTableID table_loc_id = OB_INVALID_ID;
  ObTableID ref_table_id = OB_INVALID_ID;
  ObTabletID tablet_id;
  if (OB_FAIL(MY_SPEC.get_single_table_loc_id(table_loc_id, ref_table_id))) {
  } else if (OB_FAIL(ctx_.get_gi_task_map(gi_prepare_map))) {
  } else if (OB_FAIL(gi_prepare_map->get_refactored(MY_SPEC.id_, gi_task_info))) {
    if (ret != OB_HASH_NOT_EXIST) {
    } else {
      LOG_DEBUG("no prepared task info, set table modify to end",
        K(MY_SPEC.get_id()), K(this), K(lbt()));
      // Current DML operator cannot obtain task from GI, indicating current DML operator iter end
      iter_end_ = true;
      ret = OB_SUCCESS;
    }
  } else if (OB_ISNULL(get_input()->table_loc_) &&
      OB_ISNULL(get_input()->table_loc_ = das_ctx.get_table_loc_by_id(table_loc_id, ref_table_id))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    get_input()->tablet_loc_ = gi_task_info.tablet_loc_;
    LOG_DEBUG("DML operator consume a task", K(ret),
              K(gi_task_info), KPC(get_input()), K(MY_SPEC.id_), K(get_input()));
  }
  return ret;
}

int ObTableModifyOp::calc_single_table_loc()
{
  int ret = OB_SUCCESS;
  if (!MY_SPEC.use_dist_das_) {
    ObTableID table_loc_id = OB_INVALID_ID;
    ObTableID ref_table_id = OB_INVALID_ID;
    ObDASCtx &das_ctx = ctx_.get_das_ctx();
    ObDASTableLoc *table_loc = nullptr;
    if (OB_UNLIKELY(MY_SPEC.gi_above_)) {
      if (OB_FAIL(get_gi_task())) {
      }
    } else if (OB_FAIL(MY_SPEC.get_single_table_loc_id(table_loc_id, ref_table_id))) {
    } else if (OB_ISNULL(table_loc = das_ctx.get_table_loc_by_id(table_loc_id, ref_table_id))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      get_input()->table_loc_ = table_loc;
      get_input()->tablet_loc_ = table_loc->get_first_tablet_loc();
    }
  }
  return ret;
}

int ObTableModifyOp::open_inner_conn()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = NULL;
  if (OB_ISNULL(sql_proxy_ = ctx_.get_sql_proxy())) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(session = ctx_.get_my_session())) {
    ret = OB_NOT_INIT;
  } else if (NULL != session->get_inner_conn()) {
    // do nothing.
  } else if (OB_FAIL(
                 query::ObInnerSQLConnectionAccess::
                     create_connection_with_external_session(
                         session, inner_conn_guard_))) {
  } else if (OB_ISNULL(inner_conn_guard_.get_ptr())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    /**
     * session is the only data struct which can pass through multi layer nested sql,
     * so we put inner conn in session to share it within multi layer nested sql.
     */
    session->set_inner_conn(inner_conn_guard_.get_ptr());
    need_close_conn_ = true;
  }
  if (OB_SUCC(ret)) {
    inner_conn_ = as_inner_sql_connection(session->get_inner_conn());
    if (OB_ISNULL(inner_conn_)) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ObTableModifyOp::record_ai_task_key(common::ObSEArray<AiColumnTaskKey, 8> &keys,
                                        const uint64_t table_id,
                                        const uint64_t column_id,
                                        const common::ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObString stored_predicate;
  AiColumnTaskKey key;
  if (OB_UNLIKELY(predicate.empty())) {
    // nothing to record
  } else if (OB_FAIL(ob_write_string(ctx_.get_allocator(), predicate, stored_predicate))) {
  } else {
    key.column_id_ = column_id;
    key.predicate_ = stored_predicate;
    ai_task_table_id_ = table_id;
    if (OB_FAIL(keys.push_back(key))) {
    }
  }
  return ret;
}

int ObTableModifyOp::record_ai_task_upsert(const uint64_t table_id,
                                          const uint64_t column_id,
                                          const common::ObString &predicate)
{
  return record_ai_task_key(ai_task_upsert_keys_, table_id, column_id, predicate);
}

int ObTableModifyOp::record_ai_task_delete(const uint64_t table_id,
                                          const uint64_t column_id,
                                          const common::ObString &predicate)
{
  return record_ai_task_key(ai_task_delete_keys_, table_id, column_id, predicate);
}

// The logical columns of __all_ai_column_task written by the DML statement. The physical
// column order of the table is resolved at runtime, these names are only used to locate the
// slot of every value inside the DAS write row.
static const char *const AI_TASK_COLUMN_NAMES[] = {
    "table_id",
    "column_id",
    "rowkey_predicate",
    "eligible_ts",
    "state",
    "request_id",
    "retry_count",
    "gmt_create",
    "gmt_modified"};
static_assert(static_cast<int64_t>(ARRAYSIZEOF(AI_TASK_COLUMN_NAMES))
                  == static_cast<int64_t>(ObTableModifyOp::AI_TASK_SLOT_CNT),
              "AI_TASK_COLUMN_NAMES must match AiTaskColumnSlot");

int ObTableModifyOp::init_ai_task_writer()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *my_session = ctx_.get_my_session();
  ObPhysicalPlanCtx *plan_ctx = ctx_.get_physical_plan_ctx();
  ObSchemaGetterGuard *schema_guard = NULL;
  const ObTableSchema *table_schema = NULL;
  ObDASCtx &das_ctx = DAS_CTX(ctx_);
  ObDASTableLocMeta src_loc_meta(ctx_.get_allocator());
  ObDASTableLocMeta *loc_meta = NULL;
  ObDASTabletMapper tablet_mapper;
  ObSEArray<ObTabletID, 1> tablet_ids;
  ObSEArray<ObObjectID, 1> part_ids;
  ObDASInsCtDef &ins_ctdef = ai_task_ins_ctdef_;
  ObDASInsRtDef &ins_rtdef = ai_task_ins_rtdef_;
  if (ai_task_writer_inited_) {
    // already built by a previous flush of the same statement
  } else if (OB_ISNULL(my_session) || OB_ISNULL(plan_ctx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session or plan ctx is null", KR(ret), K(my_session), K(plan_ctx));
  } else if (OB_ISNULL(schema_guard = ctx_.get_virtual_table_ctx().schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema guard is null", KR(ret));
  } else if (OB_FAIL(schema_guard->get_table_schema(OB_ALL_AI_COLUMN_TASK_TID, table_schema))) {
    LOG_WARN("fail to get the ai column task table schema", KR(ret));
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("the ai column task table does not exist", KR(ret));
  } else {
    // 1. the compile time definition, the column order follows the one generated by the
    // data access layer: the rowkey columns first, then the normal columns.
    const ObRowkeyInfo &rowkey_info = table_schema->get_rowkey_info();
    const int64_t rowkey_count = rowkey_info.get_size();
    const int64_t column_count = table_schema->get_column_count();
    int64_t table_schema_version = 0;
    int64_t tenant_schema_version = 0;
    ins_ctdef.table_id_ = OB_ALL_AI_COLUMN_TASK_TID;
    ins_ctdef.index_tid_ = OB_ALL_AI_COLUMN_TASK_TID;
    ins_ctdef.rowkey_cnt_ = rowkey_count;
    ins_ctdef.spk_cnt_ = table_schema->get_shadow_rowkey_info().get_size();
    ins_ctdef.is_ignore_ = false;
    ins_ctdef.is_batch_stmt_ = false;
    ins_ctdef.is_access_vidx_as_master_table_ = false;
    // internal write: the row layout is fully derived from this schema, do not let the
    // schema version of the inner table reject the write
    ins_ctdef.skip_check_schema_version_ = true;
    ins_ctdef.column_ids_.set_capacity(column_count);
    ins_ctdef.column_types_.set_capacity(column_count);
    ins_ctdef.column_accuracys_.set_capacity(column_count);
    if (OB_FAIL(schema_guard->get_schema_version(TABLE_SCHEMA,
                                                 OB_ALL_AI_COLUMN_TASK_TID,
                                                 table_schema_version))) {
      LOG_WARN("fail to get the schema version of the ai column task table", KR(ret));
    } else if (OB_FAIL(schema_guard->get_schema_version(tenant_schema_version))) {
      LOG_WARN("fail to get the tenant schema version", KR(ret));
    } else {
      ins_ctdef.schema_version_ = table_schema_version;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_count; ++i) {
      const ObRowkeyColumn *rowkey_column = rowkey_info.get_column(i);
      const ObColumnSchemaV2 *column = NULL;
      ObObjMeta column_type;
      if (OB_ISNULL(rowkey_column)
          || OB_ISNULL(column = table_schema->get_column_schema(rowkey_column->column_id_))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid rowkey column", KR(ret), K(i));
      } else {
        column_type = column->get_meta_type();
        column_type.set_scale(column->get_accuracy().get_scale());
        if (OB_FAIL(ins_ctdef.column_ids_.push_back(column->get_column_id()))
            || OB_FAIL(ins_ctdef.column_types_.push_back(column_type))
            || OB_FAIL(ins_ctdef.column_accuracys_.push_back(column->get_accuracy()))) {
          LOG_WARN("fail to push the rowkey column of the task table", KR(ret), K(i));
        }
      }
    }
    for (ObTableSchema::const_column_iterator iter = table_schema->column_begin();
         OB_SUCC(ret) && iter != table_schema->column_end(); ++iter) {
      const ObColumnSchemaV2 *column = *iter;
      ObObjMeta column_type;
      if (OB_ISNULL(column)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (column->is_rowkey_column() || column->is_virtual_generated_column()) {
        // already handled or not stored
      } else {
        column_type = column->get_meta_type();
        column_type.set_scale(column->get_accuracy().get_scale());
        if (OB_FAIL(ins_ctdef.column_ids_.push_back(column->get_column_id()))
            || OB_FAIL(ins_ctdef.column_types_.push_back(column_type))
            || OB_FAIL(ins_ctdef.column_accuracys_.push_back(column->get_accuracy()))) {
          LOG_WARN("fail to push a column of the task table", KR(ret));
        }
      }
    }
    // 2. identity projector: the write row is built in exactly the column_ids_ order
    if (OB_SUCC(ret) && OB_FAIL(ins_ctdef.new_row_projector_.prepare_allocate(column_count))) {
      LOG_WARN("fail to prepare the row projector", KR(ret), K(column_count));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
      ins_ctdef.new_row_projector_.at(i) = i;
    }
    // 3. the write plan of the table
    if (OB_SUCC(ret)
        && OB_FAIL(ins_ctdef.table_param_.build(table_schema,
                                                tenant_schema_version,
                                                ins_ctdef.column_ids_))) {
      LOG_WARN("fail to build the dml table param of the task table", KR(ret));
    } else if (OB_SUCC(ret)
               && OB_FAIL(ins_ctdef.table_param_.set_data_table_rowkey_tags(schema_guard,
                                                                            table_schema))) {
      LOG_WARN("fail to set the rowkey tags of the task table", KR(ret));
    } else if (OB_SUCC(ret)) {
      // write the whole row into the redo log
      ins_ctdef.is_total_quantity_log_ = true;
      ins_ctdef.tz_info_ = *my_session->get_tz_info_wrap().get_time_zone_info();
    }
    // 4. locate the slot of every logical column
    for (int64_t slot = 0; OB_SUCC(ret) && slot < AI_TASK_SLOT_CNT; ++slot) {
      const ObColumnSchemaV2 *column = table_schema->get_column_schema(AI_TASK_COLUMN_NAMES[slot]);
      int64_t idx = -1;
      if (OB_ISNULL(column)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the ai column task table lost a column", KR(ret), K(slot));
      } else {
        for (int64_t i = 0; -1 == idx && i < ins_ctdef.column_ids_.count(); ++i) {
          if (ins_ctdef.column_ids_.at(i) == column->get_column_id()) {
            idx = i;
          }
        }
        if (-1 == idx) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("the task column is not in the write column list", KR(ret), K(slot));
        } else {
          ai_task_col_idx_[slot] = idx;
        }
      }
    }
    // 4b. the compile time definition of the delete op: the task row is identified by its
    // rowkey (table_id, column_id, rowkey_predicate) only
    if (OB_SUCC(ret)) {
      ObDASDelCtDef &del_ctdef = ai_task_del_ctdef_;
      del_ctdef.table_id_ = OB_ALL_AI_COLUMN_TASK_TID;
      del_ctdef.index_tid_ = OB_ALL_AI_COLUMN_TASK_TID;
      del_ctdef.rowkey_cnt_ = rowkey_count;
      del_ctdef.spk_cnt_ = ins_ctdef.spk_cnt_;
      // removing a work item that does not exist (any more) is not an error
      del_ctdef.is_ignore_ = true;
      del_ctdef.is_batch_stmt_ = false;
      del_ctdef.is_access_vidx_as_master_table_ = false;
      del_ctdef.skip_check_schema_version_ = true;
      del_ctdef.schema_version_ = ins_ctdef.schema_version_;
      // The task table is written by an internal statement only, so there is no binlog row
      // image to produce, and the delete of a work item that is already gone has to be
      // accepted. `is_main_table_in_fts_ddl_` is the flag which switches off the "the old row
      // must still exist in storage" defensive check for such an internal DML (see
      // ObDMLRunningCtx::check_need_old_row_legitimacy).
      del_ctdef.is_total_quantity_log_ = false;
      del_ctdef.is_main_table_in_fts_ddl_ = true;
      del_ctdef.tz_info_ = ins_ctdef.tz_info_;
      del_ctdef.column_ids_.set_capacity(rowkey_count);
      del_ctdef.column_types_.set_capacity(rowkey_count);
      del_ctdef.column_accuracys_.set_capacity(rowkey_count);
      for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_count; ++i) {
        if (OB_FAIL(del_ctdef.column_ids_.push_back(ins_ctdef.column_ids_.at(i)))
            || OB_FAIL(del_ctdef.column_types_.push_back(ins_ctdef.column_types_.at(i)))
            || OB_FAIL(del_ctdef.column_accuracys_.push_back(ins_ctdef.column_accuracys_.at(i)))) {
          LOG_WARN("fail to push a rowkey column of the task table", KR(ret), K(i));
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(del_ctdef.new_row_projector_.prepare_allocate(rowkey_count))) {
        LOG_WARN("fail to prepare the delete row projector", KR(ret), K(rowkey_count));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_count; ++i) {
        del_ctdef.new_row_projector_.at(i) = i;
      }
      if (OB_SUCC(ret)
          && OB_FAIL(del_ctdef.table_param_.build(table_schema,
                                                  tenant_schema_version,
                                                  del_ctdef.column_ids_))) {
        LOG_WARN("fail to build the dml table param of the delete op", KR(ret));
      } else if (OB_SUCC(ret)
                 && OB_FAIL(del_ctdef.table_param_.set_data_table_rowkey_tags(schema_guard,
                                                                              table_schema))) {
        LOG_WARN("fail to set the rowkey tags of the delete op", KR(ret));
      }
      // the rowkey columns keep their relative order inside the two column lists
      for (int64_t i = 0; OB_SUCC(ret) && i < ARRAYSIZEOF(ai_task_del_col_idx_); ++i) {
        ai_task_del_col_idx_[i] = ai_task_col_idx_[i];
        if (OB_UNLIKELY(ai_task_del_col_idx_[i] < 0 || ai_task_del_col_idx_[i] >= rowkey_count)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected delete column position", KR(ret), K(i),
                   K(ai_task_del_col_idx_[i]));
        }
      }
    }
    // 5. the runtime definition
    if (OB_SUCC(ret)) {
      ins_rtdef.ctdef_ = &ins_ctdef;
      ins_rtdef.timeout_ts_ = plan_ctx->get_ps_timeout_timestamp();
      ins_rtdef.prelock_ = false;
      ins_rtdef.runtime_schema_version_ = plan_ctx->get_runtime_schema_version();
      ins_rtdef.sql_mode_ = my_session->get_sql_mode();
      ins_rtdef.is_for_foreign_key_check_ = false;
      ins_rtdef.is_immediate_row_conflict_check_ = false;
      ins_rtdef.affected_rows_ = 0;
      ins_rtdef.related_ctdefs_ = NULL;
      ins_rtdef.related_rtdefs_ = NULL;
      ins_rtdef.need_fetch_conflict_ = false;
      ins_rtdef.is_duplicated_ = false;
      ins_rtdef.use_put_ = false;
      ins_rtdef.ddl_task_id_ = 0;
      ai_task_del_rtdef_.ctdef_ = &ai_task_del_ctdef_;
      ai_task_del_rtdef_.timeout_ts_ = ins_rtdef.timeout_ts_;
      ai_task_del_rtdef_.prelock_ = false;
      ai_task_del_rtdef_.runtime_schema_version_ = ins_rtdef.runtime_schema_version_;
      ai_task_del_rtdef_.sql_mode_ = ins_rtdef.sql_mode_;
      ai_task_del_rtdef_.is_for_foreign_key_check_ = false;
      ai_task_del_rtdef_.is_immediate_row_conflict_check_ = false;
      ai_task_del_rtdef_.affected_rows_ = 0;
      ai_task_del_rtdef_.related_ctdefs_ = NULL;
      ai_task_del_rtdef_.related_rtdefs_ = NULL;
    }
  }

  // 6. the table / tablet location of the task table. They are allocated by the DAS context
  // of this statement, so they stay valid until the DAS task has been executed.
  if (OB_FAIL(ret)) {
    // give up the partially built definition so that a retry rebuilds it from scratch
    ins_ctdef.column_ids_.reset();
    ins_ctdef.column_types_.reset();
    ins_ctdef.column_accuracys_.reset();
    ins_ctdef.new_row_projector_.reset();
  } else if (OB_ISNULL(ai_task_table_loc_)) {
    src_loc_meta.table_loc_id_ = OB_ALL_AI_COLUMN_TASK_TID;
    src_loc_meta.ref_table_id_ = OB_ALL_AI_COLUMN_TASK_TID;
    if (OB_FAIL(das_ctx.build_table_loc_meta(src_loc_meta, loc_meta))) {
      LOG_WARN("fail to build the table loc meta of the task table", KR(ret));
    } else if (OB_FAIL(das_ctx.extended_table_loc(*loc_meta, ai_task_table_loc_))) {
      LOG_WARN("fail to extend the table loc of the task table", KR(ret));
    }
  }
  if (OB_SUCC(ret) && OB_ISNULL(ai_task_table_loc_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ret)) {
  } else if (OB_NOT_NULL(ai_task_tablet_loc_)) {
    // already extended by a previous flush of this statement
  } else if (OB_FAIL(das_ctx.get_das_tablet_mapper(OB_ALL_AI_COLUMN_TASK_TID, tablet_mapper))) {
    LOG_WARN("fail to get the tablet mapper of the task table", KR(ret));
  } else if (OB_FAIL(tablet_mapper.get_non_partition_tablet_id(tablet_ids, part_ids))) {
    LOG_WARN("fail to get the tablet id of the task table", KR(ret));
  } else if (1 != tablet_ids.count() || 1 != part_ids.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected tablet count of the task table", KR(ret), K(tablet_ids.count()));
  } else if (OB_FAIL(das_ctx.extended_tablet_loc(*ai_task_table_loc_,
                                                 tablet_ids.at(0),
                                                 ai_task_tablet_loc_,
                                                 part_ids.at(0)))) {
    LOG_WARN("fail to extend the tablet loc of the task table", KR(ret));
  } else if (OB_ISNULL(ai_task_tablet_loc_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ins_rtdef.table_loc_ = ai_task_table_loc_;
    ai_task_del_rtdef_.table_loc_ = ai_task_table_loc_;
    ai_task_writer_inited_ = true;
  }
  return ret;
}

int ObTableModifyOp::append_ai_task_insert_rows()
{
  int ret = OB_SUCCESS;
  ObDASInsertOp *insert_op = NULL;
  if (OB_ISNULL(ai_task_tablet_loc_)) {
    ret = OB_NOT_INIT;
  } else if (!dml_rtctx_.das_ref_.has_das_op(ai_task_tablet_loc_, insert_op)) {
    if (OB_FAIL(dml_rtctx_.das_ref_.prepare_das_task(ai_task_tablet_loc_, insert_op))) {
      LOG_WARN("fail to prepare the das insert task of the task table", KR(ret));
    } else if (OB_ISNULL(insert_op)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(insert_op->init_task_info(ObDASWriteBuffer::DAS_ROW_DEFAULT_EXTEND_SIZE))) {
      LOG_WARN("fail to init the das write buffer of the task table", KR(ret));
    } else {
      insert_op->set_das_ctdef(&ai_task_ins_ctdef_);
      insert_op->set_das_rtdef(&ai_task_ins_rtdef_);
    }
  }
  if (OB_SUCC(ret) && OB_NOT_NULL(insert_op)) {
    ret = write_ai_task_rows(*insert_op);
  }
  return ret;
}

int ObTableModifyOp::write_ai_task_rows(ObDASInsertOp &insert_op)
{
  int ret = OB_SUCCESS;
  ObIAllocator &stmt_alloc = ctx_.get_allocator();
  ObPhysicalPlanCtx *plan_ctx = ctx_.get_physical_plan_ctx();
  const int64_t column_count = ai_task_ins_ctdef_.column_ids_.count();
  // The statement time (the value now() answers in this statement), so that the task row is
  // eligible as soon as the transaction commits.
  int64_t eligible_us = ObTimeUtility::current_time();
  // a fresh value for every work item, so that M3 can discard the response of a request
  // that has been superseded by a newer one
  const int64_t request_us = ObTimeUtility::current_time();
  ObDASWriteBuffer &insert_buffer = insert_op.get_insert_buffer();
  ObDASWriteBuffer::DmlShadowRow shadow_row;
  ObObj timestamp_obj;
  if (OB_NOT_NULL(plan_ctx) && plan_ctx->has_cur_time()) {
    eligible_us = plan_ctx->get_cur_time().get_timestamp();
  }
  if (OB_UNLIKELY(column_count <= 0 || !insert_buffer.is_inited())) {
    ret = OB_NOT_INIT;
    LOG_WARN("the ai column task writer is not inited", KR(ret), K(column_count));
  } else if (OB_FAIL(shadow_row.init(dml_rtctx_.get_das_alloc(),
                                    ai_task_ins_ctdef_.column_types_,
                                    false))) {
    LOG_WARN("fail to init the shadow row of the task table", KR(ret));
  } else {
    // eligible_ts is a timestamp(6) column: the value is the statement time in usec
    timestamp_obj.set_timestamp(eligible_us);
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_task_upsert_keys_.count(); ++i) {
      const AiColumnTaskKey &key = ai_task_upsert_keys_.at(i);
      blocksstable::ObDatumRow task_row;
      ObSqlString request_id;
      ObString stored_request_id;
      // The request id identifies one *change of one row*: every AI column of that row shares it,
      // so the worker can fetch all AI columns of the row in one request and publish them
      // atomically (the acceptance case asserts count(distinct request_id) = 1 for the two AI
      // columns of one row). A fresh value per flush lets M3 discard a superseded response.
      if (OB_FAIL(request_id.append_fmt("%lu:%.*s:%ld",
                                        ai_task_table_id_,
                                        static_cast<int>(key.predicate_.length()),
                                        key.predicate_.ptr(),
                                        request_us))) {
        LOG_WARN("fail to build the request id of the task row", KR(ret));
      } else if (OB_FAIL(ob_write_string(stmt_alloc, request_id.string(), stored_request_id))) {
        LOG_WARN("fail to copy the request id of the task row", KR(ret));
      } else if (OB_FAIL(task_row.init(stmt_alloc, column_count))) {
        LOG_WARN("fail to init the task row", KR(ret), K(column_count));
      } else {
        task_row.count_ = static_cast<uint16_t>(column_count);
        for (int64_t j = 0; j < column_count; ++j) {
          task_row.storage_datums_[j].set_null();
        }
        task_row.storage_datums_[ai_task_col_idx_[AI_TASK_SLOT_TABLE_ID]].set_int(
            static_cast<int64_t>(ai_task_table_id_));
        task_row.storage_datums_[ai_task_col_idx_[AI_TASK_SLOT_COLUMN_ID]].set_int(
            static_cast<int64_t>(key.column_id_));
        task_row.storage_datums_[ai_task_col_idx_[AI_TASK_SLOT_ROWKEY_PREDICATE]].set_string(
            key.predicate_);
        task_row.storage_datums_[ai_task_col_idx_[AI_TASK_SLOT_ELIGIBLE_TS]].from_obj(
            timestamp_obj);
        task_row.storage_datums_[ai_task_col_idx_[AI_TASK_SLOT_STATE]].set_int(0);
        task_row.storage_datums_[ai_task_col_idx_[AI_TASK_SLOT_REQUEST_ID]].set_string(
            stored_request_id);
        task_row.storage_datums_[ai_task_col_idx_[AI_TASK_SLOT_RETRY_COUNT]].set_int(0);
        task_row.storage_datums_[ai_task_col_idx_[AI_TASK_SLOT_GMT_CREATE]].from_obj(
            timestamp_obj);
        task_row.storage_datums_[ai_task_col_idx_[AI_TASK_SLOT_GMT_MODIFIED]].from_obj(
            timestamp_obj);
        shadow_row.reuse();
        if (OB_FAIL(shadow_row.shadow_copy(task_row))) {
          LOG_WARN("fail to copy the task row into the shadow row", KR(ret));
        } else {
          ObDASWriteBuffer::DmlRow *stored_row = NULL;
          if (OB_FAIL(insert_buffer.add_row(shadow_row, &stored_row))) {
            LOG_WARN("fail to add the task row into the das write buffer", KR(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObTableModifyOp::append_ai_task_delete_rows()
{
  int ret = OB_SUCCESS;
  ObDASDeleteOp *delete_op = NULL;
  if (OB_ISNULL(ai_task_tablet_loc_)) {
    ret = OB_NOT_INIT;
  } else if (!dml_rtctx_.das_ref_.has_das_op(ai_task_tablet_loc_, delete_op)) {
    if (OB_FAIL(dml_rtctx_.das_ref_.prepare_das_task(ai_task_tablet_loc_, delete_op))) {
      LOG_WARN("fail to prepare the das delete task of the task table", KR(ret));
    } else if (OB_ISNULL(delete_op)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(delete_op->init_task_info(ObDASWriteBuffer::DAS_ROW_DEFAULT_EXTEND_SIZE))) {
      LOG_WARN("fail to init the das delete buffer of the task table", KR(ret));
    } else {
      delete_op->set_das_ctdef(&ai_task_del_ctdef_);
      delete_op->set_das_rtdef(&ai_task_del_rtdef_);
    }
  }
  if (OB_SUCC(ret) && OB_NOT_NULL(delete_op)) {
    ret = write_ai_task_delete_rows(*delete_op);
  }
  return ret;
}

int ObTableModifyOp::write_ai_task_delete_rows(ObDASDeleteOp &delete_op)
{
  int ret = OB_SUCCESS;
  ObDASWriteBuffer &delete_buffer = delete_op.get_write_buffer();
  const int64_t column_count = ai_task_del_ctdef_.column_ids_.count();
  ObDASWriteBuffer::DmlShadowRow shadow_row;
  if (OB_UNLIKELY(column_count <= 0 || !delete_buffer.is_inited())) {
    ret = OB_NOT_INIT;
    LOG_WARN("the ai column task deleter is not inited", KR(ret), K(column_count));
  } else if (OB_FAIL(shadow_row.init(dml_rtctx_.get_das_alloc(),
                                    ai_task_del_ctdef_.column_types_,
                                    false))) {
    LOG_WARN("fail to init the shadow row of the task table", KR(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_task_delete_keys_.count(); ++i) {
      const AiColumnTaskKey &key = ai_task_delete_keys_.at(i);
      blocksstable::ObDatumRow task_row;
      if (OB_FAIL(task_row.init(ctx_.get_allocator(), column_count))) {
        LOG_WARN("fail to init the task row", KR(ret), K(column_count));
      } else {
        task_row.count_ = static_cast<uint16_t>(column_count);
        for (int64_t j = 0; j < column_count; ++j) {
          task_row.storage_datums_[j].set_null();
        }
        task_row.storage_datums_[ai_task_del_col_idx_[AI_TASK_SLOT_TABLE_ID]].set_int(
            static_cast<int64_t>(ai_task_table_id_));
        task_row.storage_datums_[ai_task_del_col_idx_[AI_TASK_SLOT_COLUMN_ID]].set_int(
            static_cast<int64_t>(key.column_id_));
        task_row.storage_datums_[ai_task_del_col_idx_[AI_TASK_SLOT_ROWKEY_PREDICATE]].set_string(
            key.predicate_);
        shadow_row.reuse();
        if (OB_FAIL(shadow_row.shadow_copy(task_row))) {
          LOG_WARN("fail to copy the task row into the shadow row", KR(ret));
        } else {
          ObDASWriteBuffer::DmlRow *stored_row = NULL;
          if (OB_FAIL(delete_buffer.add_row(shadow_row, &stored_row))) {
            LOG_WARN("fail to add the task row into the das delete buffer", KR(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObTableModifyOp::flush_ai_column_tasks()
{
  int ret = OB_SUCCESS;
  if (ai_task_upsert_keys_.empty() && ai_task_delete_keys_.empty()) {
    // fast path: this statement has no AI column work at all
  } else if (OB_UNLIKELY(OB_INVALID_ID == ai_task_table_id_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(init_ai_task_writer())) {
  } else if (!ai_task_delete_keys_.empty() && OB_FAIL(append_ai_task_delete_rows())) {
    // DAS executes the delete tasks before the insert tasks, so a task row that is removed and
    // re-created by the same statement (e.g. a primary key change) ends up with the new rowkey
  } else if (!ai_task_upsert_keys_.empty() && OB_FAIL(append_ai_task_insert_rows())) {
  } else if (OB_FAIL(dml_rtctx_.das_ref_.execute_all_task())) {
    LOG_WARN("fail to write the ai column task rows", KR(ret));
  } else if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
    LOG_WARN("fail to close the ai column task das tasks", KR(ret));
  }
  ai_task_upsert_keys_.reset();
  ai_task_delete_keys_.reset();
  return ret;
}

int ObTableModifyOp::close_inner_conn()
{
  /**
   * we can call it even if open_inner_conn() failed, because only the one who call
   * open_inner_conn() succeed first will do close job by "if (need_close_conn_)".
   */
  int ret = OB_SUCCESS;
  if (need_close_conn_) {
    ObSQLSessionInfo *session = ctx_.get_my_session();
    if (OB_ISNULL(sql_proxy_) || OB_ISNULL(session)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      OX(session->set_inner_conn(NULL));
      OX(inner_conn_guard_.reset());
    }
    need_close_conn_ = false;
  }
  sql_proxy_ = NULL;
  inner_conn_guard_.reset();
  inner_conn_ = NULL;
  return ret;
}

int ObTableModifyOp::begin_nested_session(bool skip_cur_stmt_tables)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(inner_conn_->begin_nested_session(get_saved_session(), saved_conn_,
                                                       skip_cur_stmt_tables))) {
  }
  return ret;
}

int ObTableModifyOp::end_nested_session()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(inner_conn_->end_nested_session(get_saved_session(), saved_conn_))) {
  }
  return ret;
}
int ObTableModifyOp::execute_write(const char *sql)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sql)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(inner_conn_->execute_write(sql, affected_rows))) {
  }
  return ret;
}

int ObTableModifyOp::execute_read(const char *sql, ObMySQLProxy::MySQLResult &res)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_) || OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sql)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(inner_conn_->execute_read(sql, res))) {
  }
 return ret;
}

int ObTableModifyOp::check_stack()
{
  int ret = OB_SUCCESS;
  const int max_stack_deep = 16;
  bool is_stack_overflow = false;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  if (OB_ISNULL(session)) {
    ret = OB_ERR_NULL_VALUE;
  } else if (session->get_nested_count() > max_stack_deep) {
    ret = OB_SIZE_OVERFLOW;
  } else if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
  }
  return ret;
}
OperatorOpenOrder ObTableModifyOp::get_operator_open_order() const
{
  OperatorOpenOrder open_order = OPEN_CHILDREN_FIRST;
  if (spec_.plan_->is_use_px()) {
    // If PX is enabled in DML, there are two scenarios:
    // 1. PDML: in pdml, pdml-op completely does not need its corresponding input to provide runtime parameters, so it will directly return
    // open_order = OPEN_CHILDREN_FIRST
    // 2. DML+PX: In this case, the parameters of the dml run are completely fed by the GI operator above it, for example, such a plan:
    //   PX COORD
    //    PX TRANSMIT
    //      GI (FULL PARTITION WISE)
    //       DELETE
    //        TSC
    //  Such a plan requires GI to drive the deletion, GI rescan delete each time a new Task is iterated,
    //  Pass the information of the corresponding Task to the delete operator.
    open_order = OPEN_CHILDREN_FIRST;
  }
  return open_order;
}

int ObTableModifyOp::submit_all_dml_task()
{
  int ret = OB_SUCCESS;
  if (dml_rtctx_.das_ref_.has_task()) {
    if (dml_rtctx_.need_pick_del_task_first() &&
                OB_FAIL(dml_rtctx_.das_ref_.pick_del_task_to_first())) {
    } else if (OB_FAIL(dml_rtctx_.das_ref_.execute_all_task())) {
    } else if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
    } else if (OB_FAIL(ObDMLService::handle_after_row_processing(this, &get_dml_modify_row_list()))) {
    } else {
      dml_modify_rows_.clear();
      dml_rtctx_.reuse();
    }
  }
  return ret;
}

//The data to be written by DML will be buffered in the DAS Write Buffer
//When the buffer data exceeds 6M,
//needs to be written to the storage to release the memory.
int ObTableModifyOp::discharge_das_write_buffer()
{
  int ret = OB_SUCCESS;
  if (dml_rtctx_.need_submit_all_tasks()) {
    LOG_INFO("DASWriteBuffer full, now to write storage",
             "buffer memory", dml_rtctx_.das_ref_.get_das_alloc().used(),
             K(dml_rtctx_.get_das_task_memory_size()), K(dml_rtctx_.get_das_parallel_task_size()));
    ret = submit_all_dml_task();
  } else if (execute_single_row_) {
    if (REACH_COUNT_INTERVAL(100)) { // print log per 100 times.
    }
    ret = submit_all_dml_task();
  }
  return ret;
}

int ObTableModifyOp::get_next_row_from_child()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(child_->get_next_row())) {
    if (OB_ITER_END != ret) {
    }
  } else {
    LOG_DEBUG("child output row", "row", ROWEXPR2STR(eval_ctx_, child_->get_spec().output_));
  }
  return ret;
}

int ObTableModifyOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  if (iter_end_) {
    ret = OB_ITER_END;
  } else {
    int64_t row_count = 0;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(try_check_status())) {
      } else if (OB_FAIL(get_next_row_from_child())) {
        if (OB_ITER_END != ret) {
        } else {
          iter_end_ = true;
          ret = OB_SUCCESS;
          break;
        }
      } else if (OB_FAIL(write_row_to_das_buffer())) {
      } else if (OB_FAIL(discharge_das_write_buffer())) {
      } else if (MY_SPEC.is_returning_) {
        break;
      }
      row_count ++;
    }

    if (OB_FAIL(ret)) {
      record_err_for_load_data(ret, row_count);
    }

    if (OB_SUCC(ret) && iter_end_ && dml_rtctx_.das_ref_.has_task()) {
      //DML operator reach iter end,
      //now submit the remaining rows in the DAS Write Buffer to the storage
      if (OB_FAIL(submit_all_dml_task())) {
      }
    }
    //to post process the DML info after writing all data to the storage or returning one row
    ret = write_rows_post_proc(ret);
    if (OB_SUCC(ret) && iter_end_) {
      ret = OB_ITER_END;
    }
  }
  return ret;
}

int ObTableModifyOp::perform_batch_fk_check()
{
  DEBUG_SYNC(BEFORE_FOREIGN_KEY_CONSTRAINT_CHECK);
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < fk_checkers_.count(); ++i) {
    bool all_has_result = false;
    ObForeignKeyChecker *fk_checker = fk_checkers_.at(i);
    if (OB_ISNULL(fk_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(fk_checker->do_fk_check_batch(all_has_result))) {
    } else if (!all_has_result) {
      ret = OB_ERR_NO_REFERENCED_ROW;
    }
  }
  return ret;
}
}  // namespace sql
}  // namespace oceanbase
