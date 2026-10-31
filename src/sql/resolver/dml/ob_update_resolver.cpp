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

#define USING_LOG_PREFIX SQL_RESV
#include "lib/allocator/page_arena.h"
#include "share/ai_service/ob_ai_column_schema.h"
#include "sql/resolver/dml/ob_update_resolver.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace sql
{

namespace
{

// Column ids referenced by the AI expression of `ai_column`. The spec of an AI column lives in
// the comment of its result-spec hidden column, see share/ai_service/ob_ai_column_schema.h.
// Mirrors ObDmlCgService::get_ai_dependency_column_ids().
int get_ai_dependency_column_ids(common::ObIAllocator &alloc,
                                 const ObTableSchema &table_schema,
                                 const ObColumnSchemaV2 &ai_column,
                                 common::ObIArray<uint64_t> &dep_column_ids)
{
  int ret = OB_SUCCESS;
  ObString spec_column_name;
  const ObColumnSchemaV2 *spec_column = NULL;
  share::ObAiColumnInfo ai_info;
  dep_column_ids.reuse();
  if (OB_FAIL(share::build_ai_hidden_column_name(alloc,
                                                 share::AI_RESULT_SPEC_COLUMN_PREFIX,
                                                 ai_column.get_column_name_str(),
                                                 spec_column_name))) {
  } else if (OB_ISNULL(spec_column = table_schema.get_column_schema(spec_column_name))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the AI result spec column is missing", KR(ret), K(spec_column_name));
  } else if (OB_FAIL(share::parse_ai_column_spec_text(alloc,
                                                      spec_column->get_comment_str(),
                                                      ai_info))) {
    LOG_WARN("fail to parse the AI column spec", KR(ret), K(spec_column_name));
  } else {
    // dep_columns is a comma separated column name list
    const ObString &deps = ai_info.dep_columns_;
    int64_t pos = 0;
    while (OB_SUCC(ret) && pos < deps.length()) {
      int64_t name_len = 0;
      while (pos + name_len < deps.length() && ',' != deps.ptr()[pos + name_len]) {
        ++name_len;
      }
      ObString dep_name;
      dep_name.assign_ptr(deps.ptr() + pos, static_cast<int32_t>(name_len));
      pos += name_len + 1;
      // trim spaces
      while (!dep_name.empty() && ' ' == dep_name.ptr()[0]) {
        dep_name.assign_ptr(dep_name.ptr() + 1, dep_name.length() - 1);
      }
      while (!dep_name.empty() && ' ' == dep_name.ptr()[dep_name.length() - 1]) {
        dep_name.assign_ptr(dep_name.ptr(), dep_name.length() - 1);
      }
      const ObColumnSchemaV2 *dep_column = NULL;
      if (dep_name.empty()) {
        // empty item, e.g. a trailing comma
      } else if (OB_ISNULL(dep_column = table_schema.get_column_schema(dep_name))) {
        LOG_TRACE("the AI dependency column is gone", K(dep_name));
      } else if (OB_FAIL(dep_column_ids.push_back(dep_column->get_column_id()))) {
      }
    }
  }
  return ret;
}

} // anonymous namespace
ObUpdateResolver::ObUpdateResolver(ObResolverParams &param)
  : ObDelUpdResolver(param)
{
  param.contain_dml_ = true;
}

ObUpdateResolver::~ObUpdateResolver()
{
}

int ObUpdateResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObUpdateStmt *update_stmt = NULL;
  ObSEArray<ObTableAssignment, 2> tables_assign;
  if (T_UPDATE != parse_tree.type_
      || 3 > parse_tree.num_child_
      || OB_ISNULL(parse_tree.children_)
      || OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid parse tree for update", K(parse_tree.type_),
             K(parse_tree.num_child_), K(parse_tree.children_), K((session_info_)));
  } else if (OB_ISNULL(update_stmt = create_stmt<ObUpdateStmt>())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("create update stmt failed");
  } else {
    stmt_ = update_stmt;
    update_stmt->set_ignore(false);
    if (NULL != parse_tree.children_[IGNORE]) {
      update_stmt->set_ignore(true);
      session_info_->set_ignore_stmt(true);
    }
  }

  // resolve outline data hints first
  if (OB_SUCC(ret)) {
    if (OB_FAIL(resolve_outline_data_hints())) {
    }
  }

  // resolve with clause before resolve table items
  if (OB_SUCC(ret)) {
    if (OB_FAIL(resolve_with_clause(parse_tree.children_[WITH_MYSQL]))) {
    }
  }

  // 1. resolve table items
  if (OB_SUCC(ret)) {
    ParseNode *table_node = parse_tree.children_[TABLE];
    if (OB_FAIL(resolve_table_list(*table_node))) {
    }
  }

  if (OB_SUCC(ret)) {
    current_scope_ = T_UPDATE_SCOPE;
    // resolve assignments
    ParseNode *assign_list = parse_tree.children_[UPDATE_LIST];
    if (OB_ISNULL(assign_list) || OB_UNLIKELY(T_ASSIGN_LIST != assign_list->type_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid assign list node", K(assign_list));
    } else if (OB_FAIL(resolve_assignments(*assign_list,
                                           tables_assign,
                                           current_scope_))) {
    } else if (OB_FAIL(add_ai_invalidation_assignments(tables_assign))) {
    }
  }

  if (OB_SUCC(ret)) {
    FOREACH_CNT_X(it, update_stmt->get_table_items(), OB_SUCC(ret)) {
      if (NULL != *it && ((*it)->is_generated_table() || (*it)->is_temp_table())) {
        if (NULL != (*it)->view_base_item_ &&
            OB_FAIL(add_all_column_to_updatable_view(*update_stmt, *(*it)))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(view_pullup_special_column_exprs())) {
      } else if (OB_FAIL(view_pullup_part_exprs())) {
      } else { /*do nothing*/ }
    }
  }

  if (OB_SUCC(ret)) {
    // Parse cascading update columns
    if (OB_FAIL(resolve_additional_assignments(tables_assign,
                                               T_UPDATE_SCOPE))) {
    }
  }

  //add column for table scan, keep rowkey column in the head
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < tables_assign.count(); i++) {
      if (OB_FAIL(generate_update_table_info(tables_assign.at(i)))) {
      } else { /*do nothing*/ }
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(check_multi_update_table_conflict())) {
    } else if (update_stmt->is_ignore() && update_stmt->has_global_index()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "ignore with global index");
    } else { /*do nothing*/ }
  }

  // 3. resolve other clauses
  if (OB_SUCC(ret)) {
    if (OB_FAIL(try_add_remove_const_expr_for_assignments())) {
    } else if (OB_FAIL(resolve_update_constraints())) {
    } else if (OB_FAIL(resolve_hints(parse_tree.children_[HINT]))) {
    } else if (OB_FAIL(resolve_where_clause(parse_tree.children_[WHERE]))) {
    } else if (params_.is_batch_stmt_ && OB_FAIL(generate_batched_stmt_info())) {
    } else if (OB_FAIL(resolve_order_clause(parse_tree.children_[ORDER_BY]))) {
    } else if (OB_FAIL(resolve_limit_clause(parse_tree.children_[LIMIT], true))) {
    } else if (!update_stmt->is_ignore() &&
               OB_FAIL(check_join_update_conflict())) {
    } else if (OB_FAIL(update_stmt->formalize_stmt(session_info_))) {
    } else { /*do nothing*/ }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(check_view_updatable())) {
    } else if (OB_FAIL(update_stmt->check_dml_need_filter_null())) {
    } else if (OB_FAIL(update_stmt->check_dml_source_from_join())) {
    } else if (OB_FAIL(check_safe_update_mode(update_stmt))) {
    } else { /*do nothing*/ }
  }
  return ret;
}

// An AI column is the cached result of an AI expression over its dependency columns, so a
// statement that changes one of those columns (or the rowkey, which moves the pending work item
// to another row key) has to invalidate the cached result. The invalidation is materialized here,
// at resolve time, as an ordinary `<ai_column> = NULL` assignment, and not by resetting the row
// image while the row is executed: a real assignment gives the DML consistent old/new row images
// (the AI column expression is no longer its own old value), a new version of the vector index
// row key, and it lets the ordinary update path maintain every index the AI column takes part in.
int ObUpdateResolver::add_ai_invalidation_assignments(ObIArray<ObTableAssignment> &table_assigns)
{
  int ret = OB_SUCCESS;
  ObUpdateStmt *update_stmt = get_update_stmt();
  ObArenaAllocator ai_alloc(ObModIds::OB_SQL_EXPR);
  if (OB_ISNULL(update_stmt) || OB_ISNULL(schema_checker_) || OB_ISNULL(params_.expr_factory_)
      || OB_ISNULL(params_.allocator_) || OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid update resolver context", KR(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table_assigns.count(); ++i) {
    ObTableAssignment &table_assign = table_assigns.at(i);
    const TableItem *table_item = NULL;
    const ObTableSchema *table_schema = NULL;
    ObSEArray<uint64_t, 8> assigned_ids;
    ObSEArray<uint64_t, 4> dep_column_ids;
    ObSEArray<const ObColumnSchemaV2 *, 4> invalidated_columns;
    bool rowkey_changed = false;
    if (OB_ISNULL(table_item = update_stmt->get_table_item_by_id(table_assign.table_id_))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the table item of the updated table is missing", KR(ret),
               K(table_assign.table_id_));
    } else if (!table_item->is_basic_table()) {
      // an updatable view does not expose the AI columns of its base table
    } else if (OB_FAIL(schema_checker_->get_table_schema(table_item->get_base_table_item().ref_id_,
                                                         table_schema))) {
    } else if (OB_ISNULL(table_schema)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (!table_schema->is_user_table()) {
      // AI columns only exist on user tables
    } else {
      // 1. the column ids this statement writes
      for (int64_t j = 0; OB_SUCC(ret) && j < table_assign.assignments_.count(); ++j) {
        const ObColumnRefRawExpr *column_expr = table_assign.assignments_.at(j).column_expr_;
        const ColumnItem *column_item = NULL;
        uint64_t column_id = OB_INVALID_ID;
        if (OB_ISNULL(column_expr)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("the assigned column expr is null", KR(ret));
        } else if (OB_NOT_NULL(column_item = update_stmt->get_column_item_by_id(
                     column_expr->get_table_id(), column_expr->get_column_id()))) {
          // a view column is assigned, the base column is the one that changes
          column_id = column_item->base_cid_;
        } else {
          column_id = column_expr->get_column_id();
        }
        if (OB_SUCC(ret) && !has_exist_in_array(assigned_ids, column_id)
            && OB_FAIL(assigned_ids.push_back(column_id))) {
        }
      }
      // 2. a rowkey change invalidates every AI column of the table
      if (OB_SUCC(ret)) {
        const ObRowkeyInfo &rowkey_info = table_schema->get_rowkey_info();
        for (int64_t j = 0; OB_SUCC(ret) && !rowkey_changed && j < rowkey_info.get_size(); ++j) {
          const ObRowkeyColumn *rowkey_column = rowkey_info.get_column(j);
          if (OB_ISNULL(rowkey_column)) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            rowkey_changed = has_exist_in_array(assigned_ids, rowkey_column->column_id_);
          }
        }
      }
      // 3. collect the AI columns whose dependency columns are written
      for (ObTableSchema::const_column_iterator iter = table_schema->column_begin();
           OB_SUCC(ret) && iter != table_schema->column_end(); ++iter) {
        const ObColumnSchemaV2 *ai_column = *iter;
        bool invalidated = false;
        if (OB_ISNULL(ai_column)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (!ai_column->is_ai_column()) {
          // skip normal columns
        } else if (rowkey_changed) {
          invalidated = true;
        } else if (OB_FAIL(get_ai_dependency_column_ids(ai_alloc, *table_schema, *ai_column,
                                                        dep_column_ids))) {
        } else {
          for (int64_t j = 0; !invalidated && j < dep_column_ids.count(); ++j) {
            invalidated = has_exist_in_array(assigned_ids, dep_column_ids.at(j));
          }
        }
        // a statement that already writes the AI column itself, i.e. the AI column worker
        // publishing a model result, has nothing to invalidate
        if (OB_SUCC(ret) && invalidated
            && has_exist_in_array(assigned_ids, ai_column->get_column_id())) {
          invalidated = false;
        }
        if (OB_SUCC(ret) && invalidated && OB_FAIL(invalidated_columns.push_back(ai_column))) {
        }
      }
      // 4. materialize the invalidation as `<ai_column> = NULL`
      for (int64_t j = 0; OB_SUCC(ret) && j < invalidated_columns.count(); ++j) {
        if (OB_FAIL(add_ai_column_null_assignment(*table_item, *invalidated_columns.at(j),
                                                  table_assigns))) {
        }
      }
    }
  }
  return ret;
}

int ObUpdateResolver::add_ai_column_null_assignment(const TableItem &table_item,
                                                    const ObColumnSchemaV2 &ai_column,
                                                    ObIArray<ObTableAssignment> &table_assigns)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObColumnRefRawExpr *, 1> column_exprs;
  ObColumnRefRawExpr *ai_column_expr = NULL;
  ObConstRawExpr *null_const_expr = NULL;
  ObRawExpr *null_expr = NULL;
  const ColumnItem *column_item = NULL;
  if (OB_ISNULL(params_.expr_factory_) || OB_ISNULL(params_.allocator_)
      || OB_ISNULL(session_info_) || OB_ISNULL(get_update_stmt())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid update resolver context", KR(ret));
  } else if (OB_FAIL(add_column_to_stmt(table_item, ai_column, column_exprs))) {
  } else if (column_exprs.empty() || OB_ISNULL(ai_column_expr = column_exprs.at(0))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to add the AI column to the statement", KR(ret),
             K(ai_column.get_column_name_str()));
  } else if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_NULL, null_const_expr))) {
  } else if (OB_ISNULL(null_const_expr)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObObjParam null_value;
    null_value.set_null();
    null_value.set_param_meta();
    null_const_expr->set_value(null_value);
    null_expr = null_const_expr;
    // cast NULL to the type of the AI column, exactly like a user written assignment
    if (OB_FAIL(ObRawExprUtils::build_column_conv_expr(*params_.expr_factory_,
                                                       *params_.allocator_,
                                                       *ai_column_expr,
                                                       null_expr,
                                                       session_info_))) {
    } else if (OB_ISNULL(column_item = get_update_stmt()->get_column_item_by_id(
                 table_item.table_id_, ai_column.get_column_id()))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the column item of the AI column is missing", KR(ret),
               K(ai_column.get_column_name_str()));
    } else {
      // is_implicit_: this write is not a user write of an AI column, so it must neither be
      // rejected (see ObDmlCgService::check_ai_column_not_written()) nor printed back
      ObAssignment assignment;
      assignment.column_expr_ = ai_column_expr;
      assignment.expr_ = null_expr;
      assignment.is_implicit_ = true;
      if (OB_FAIL(add_assignment(table_assigns, &table_item, column_item, assignment))) {
      }
    }
  }
  return ret;
}

int ObUpdateResolver::try_add_remove_const_expr_for_assignments()
{
  int ret = OB_SUCCESS;
  ObUpdateStmt *stmt = NULL;
  if (OB_ISNULL(stmt = get_update_stmt()) ||OB_ISNULL(session_info_) ||
      OB_ISNULL(schema_checker_) || OB_ISNULL(params_.expr_factory_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObIArray<ObUpdateTableInfo*> &tables_info = stmt->get_update_table_info();
    for (int64_t i = 0; OB_SUCC(ret) && i < tables_info.count(); i++) {
      const ObTableSchema *table_schema = NULL;
      if (OB_ISNULL(tables_info.at(i))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(schema_checker_->get_table_schema(
                                                           tables_info.at(i)->ref_table_id_,
                                                           table_schema))) {
      } else if (OB_ISNULL(table_schema)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (!table_schema->is_user_table()) {
        /*do nothing*/
      } else {
        const ObIArray<ObForeignKeyInfo> &fk_infos = table_schema->get_foreign_key_infos();
        ObIArray<ObAssignment> &assignments = tables_info.at(i)->assignments_;
        for (int64_t i = 0; OB_SUCC(ret) && i < assignments.count(); ++i) {
          ObAssignment &assign = assignments.at(i);
          if (OB_ISNULL(assign.expr_) || OB_ISNULL(assign.column_expr_)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (assign.expr_->is_const_expr() &&
                     is_parent_col_self_ref_fk(assign.column_expr_->get_column_id(), fk_infos)) {
            ObRawExpr *new_expr = NULL;
            if (OB_FAIL(ObRawExprUtils::build_remove_const_expr(*params_.expr_factory_,
                                                                *session_info_,
                                                                 assign.expr_,
                                                                 new_expr))) {
            } else if (OB_ISNULL(new_expr)) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              assign.expr_ = new_expr;
            }
          }
        }
      }
    }
  }
  return ret;
}

// check if %parent_col_id is parent column of a self reference foreign key
bool ObUpdateResolver::is_parent_col_self_ref_fk(uint64_t parent_col_id,
                                                const ObIArray<ObForeignKeyInfo> &fk_infos)
{
  bool bret = false;
  for (int64_t i = 0; i < fk_infos.count(); i++) {
    const ObForeignKeyInfo &fk_info = fk_infos.at(i);
    if (fk_info.parent_table_id_ == fk_info.child_table_id_) {
      if (has_exist_in_array(fk_info.parent_column_ids_, parent_col_id, NULL)) {
        bret = true;
        break;
      }
    }
  }
  return bret;
}

int ObUpdateResolver::check_safe_update_mode(ObUpdateStmt *update_stmt)
{
  int ret = OB_SUCCESS;
  bool is_sql_safe_updates = false;
  if (OB_ISNULL(params_.session_info_) || OB_ISNULL(update_stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(params_.session_info_->get_sql_safe_updates(is_sql_safe_updates))) {
  } else if (is_sql_safe_updates) {
    /*Update table values in mysql safe mode, needs to meet one of the following two conditions:
    * 1.contains limit;
    * 2.contains where condition, which can extract query range ==> Since extracting query range can only be done during the optimizer phase, therefore here we only
    *   check if there exists a where condition;
    */
    if (!update_stmt->has_limit() && update_stmt->get_condition_exprs().empty()) {
      ret = OB_ERR_SAFE_UPDATE_MODE_NEED_WHERE_OR_LIMIT;
    }
  } else {/*do nothing*/}
  return ret;
}

int ObUpdateResolver::check_multi_update_table_conflict()
{
  int ret = OB_SUCCESS;
  const ObUpdateStmt *update_stmt = get_update_stmt();
  if (OB_ISNULL(update_stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (update_stmt->get_update_table_info().count() <= 1) {
    /*do nothing*/
  } else {
    const ObIArray<ObUpdateTableInfo*> &tables_info = update_stmt->get_update_table_info();
    for (int64_t i = 0; OB_SUCC(ret) && i < tables_info.count() - 1; ++i) {
      if (OB_ISNULL(tables_info.at(i))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        for (int64_t j = i + 1; OB_SUCC(ret) && j < tables_info.count(); ++j) {
          if (OB_ISNULL(tables_info.at(j))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (tables_info.at(i)->table_id_ != tables_info.at(j)->table_id_ &&
                     tables_info.at(i)->ref_table_id_ == tables_info.at(j)->ref_table_id_) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "multiple aliases to same table");
          }
        }
      }
    }
  }
  return ret;
}

int ObUpdateResolver::check_join_update_conflict()
{
  int ret = OB_SUCCESS;
  bool is_join_update = false;
  ObUpdateStmt *update_stmt = NULL;
  if (OB_ISNULL(update_stmt = get_update_stmt())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(is_join_table_update(update_stmt, is_join_update))) {
  } else if (is_join_update && update_stmt->has_order_by()) {
    // Incorrect usage of UPDATE and ORDER BY
    ret = OB_ERR_UPDATE_ORDER_BY;
  } else if (is_join_update && update_stmt->has_limit()) {
    // Incorrect usage of UPDATE and LIMIT
    ret = OB_ERR_UPDATE_LIMIT;
  }
  return ret;
}

int ObUpdateResolver::resolve_table_list(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObUpdateStmt *update_stmt = get_update_stmt();
  TableItem *table_item = NULL;
  ObSelectStmt *ref_stmt = NULL;
  if (OB_UNLIKELY(T_TABLE_REFERENCES != parse_tree.type_)
      || OB_UNLIKELY(parse_tree.num_child_ < 1)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(update_stmt)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid update stmt", K(update_stmt));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < parse_tree.num_child_; ++i) {
    const ParseNode *table_node = parse_tree.children_[i];
    if (OB_ISNULL(table_node)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObDMLResolver::resolve_table(*table_node, table_item))) {
    } else {/*do nothing*/}
    if (OB_SUCC(ret)) {
      if (OB_FAIL(column_namespace_checker_.add_reference_table(table_item))) {
      } else if (OB_FAIL(update_stmt->add_from_item(table_item->table_id_, table_item->is_joined_table()))) {
      } else if (OB_FAIL(check_need_fired_trigger(table_item))) {
      } else {
      /*
        In order to share the same logic with 'select' to generate access path costly, we
        add the table in the udpate stmt in the from_item list as well.
       */
      }
    }
  }
  if (OB_SUCC(ret) && 1 == update_stmt->get_from_item_size()) {
    const TableItem *table_item = update_stmt->get_table_item(update_stmt->get_from_item(0));
    if (OB_ISNULL(table_item)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (table_item->cte_type_ != TableItem::NOT_CTE) {
      ret = OB_ERR_NON_UPDATABLE_TABLE;
      const ObString &table_name = table_item->alias_name_.empty() ? table_item->table_name_ : table_item->alias_name_;
      ObString scope_name = "UPDATE";
      LOG_USER_ERROR(OB_ERR_NON_UPDATABLE_TABLE,
                      table_name.length(), table_name.ptr(),
                      scope_name.length(), scope_name.ptr());
    }
  }
  return ret;
}

int ObUpdateResolver::generate_update_table_info(ObTableAssignment &table_assign)
{
  int ret = OB_SUCCESS;
  void *ptr = NULL;
  ObUpdateStmt *update_stmt = get_update_stmt();
  const ObTableSchema *table_schema = NULL;
  const TableItem *table_item = NULL;
  ObUpdateTableInfo *table_info = NULL;
  uint64_t index_tid[OB_MAX_AUX_TABLE_PER_MAIN_TABLE];
  int64_t gindex_cnt = OB_MAX_AUX_TABLE_PER_MAIN_TABLE;
  int64_t binlog_row_image = ObBinlogRowImage::FULL;
  if (OB_ISNULL(schema_checker_) || OB_ISNULL(params_.session_info_) ||
      OB_ISNULL(allocator_) || OB_ISNULL(update_stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(table_item = update_stmt->get_table_item_by_id(table_assign.table_id_))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_checker_->get_table_schema(
                                                       table_item->get_base_table_item().ref_id_,
                                                       table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_checker_->get_can_write_index_array(table_item->get_base_table_item().ref_id_,
                                                                index_tid, gindex_cnt, true))) {
  } else if (OB_FAIL(params_.session_info_->get_binlog_row_image(binlog_row_image))) {
  } else if (NULL == (ptr = allocator_->alloc(sizeof(ObUpdateTableInfo)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    table_info = new(ptr) ObUpdateTableInfo();
    if (OB_FAIL(table_info->assignments_.assign(table_assign.assignments_))) {
    } else if (OB_FAIL(table_info->part_ids_.assign(table_item->get_base_table_item().part_ids_))) {
    } else {
      if (OB_FAIL(add_all_rowkey_columns_to_stmt(*table_item, table_info->column_exprs_))) {
      } else if (need_all_columns(*table_schema, binlog_row_image)) {
        if (OB_FAIL(add_all_columns_to_stmt(*table_item, table_info->column_exprs_))) {
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < table_assign.assignments_.count(); ++i) {
          ObAssignment &assign = table_assign.assignments_.at(i);
          if (OB_FAIL(add_var_to_array_no_dup(table_info->column_exprs_, assign.column_expr_))) {
          } else if (OB_FAIL(add_index_related_columns_to_stmt(*table_item,
                             assign.column_expr_->get_column_id(), table_info->column_exprs_))) {
          } else { /*do nothing*/ }
        }
      }
      if (OB_SUCC(ret)) {
        table_info->table_id_ = table_item->table_id_;
        table_info->loc_table_id_ = table_item->get_base_table_item().table_id_;
        table_info->ref_table_id_ = table_item->get_base_table_item().ref_id_;
        table_info->table_name_ = table_schema->get_table_name_str();
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(update_stmt->get_update_table_info().push_back(table_info))) {
      } else if (gindex_cnt > 0) {
        update_stmt->set_has_global_index(true);
      } else { /*do nothing*/ }
    }
  }
  return ret;
}

int ObUpdateResolver::check_view_updatable()
{
  int ret = OB_SUCCESS;
  ObUpdateStmt *update_stmt = get_update_stmt();
  if (NULL == update_stmt) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObIArray<ObUpdateTableInfo*> &tables_info = update_stmt->get_update_table_info();
    for (int64_t i = 0; OB_SUCC(ret) && i < tables_info.count(); i++) {
      const TableItem *table_item = NULL;
      if (OB_ISNULL(tables_info.at(i)) ||
          OB_ISNULL(table_item = update_stmt->get_table_item_by_id(tables_info.at(i)->table_id_))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (!table_item->is_generated_table() && !table_item->is_temp_table()) {
        continue;
      } else {
        // check select item subquery
        if (OB_SUCC(ret)) {
          bool has_subquery = false;
          bool has_dependent_subquery = false;
          bool ref_update_table = false;
          if (OB_FAIL(ObResolverUtils::uv_check_select_item_subquery(
              *table_item, has_subquery, has_dependent_subquery, ref_update_table))) {
          } else {
            ret = has_dependent_subquery ? OB_ERR_NON_UPDATABLE_TABLE : OB_SUCCESS;
          }
        }
        if (OB_SUCC(ret)) {
          bool ref_update_table = false;
          if (OB_FAIL(ObResolverUtils::uv_check_where_subquery(*table_item, ref_update_table))) {
          } else {
            ret = ref_update_table ? OB_ERR_NON_UPDATABLE_TABLE : OB_SUCCESS;
          }
        }

        if (OB_SUCC(ret)) {
          bool has_non_inner_join = false;
          if (OB_FAIL(ObResolverUtils::uv_check_has_non_inner_join(*table_item, has_non_inner_join))) {
          } else {
            ret = has_non_inner_join ? OB_ERR_NON_UPDATABLE_TABLE : OB_SUCCESS;
          }
        }

        if (OB_ERR_NON_UPDATABLE_TABLE == ret) {
          ObString upd_str = "UPDATE";
          LOG_USER_ERROR(OB_ERR_NON_UPDATABLE_TABLE,
              table_item->get_table_name().length(), table_item->get_table_name().ptr(),
              upd_str.length(), upd_str.ptr());
        }
      }
    }
  }
  return ret;
}

int ObUpdateResolver::is_join_table_update(const ObDMLStmt *stmt, bool &is_multi_table)
{
  int ret = OB_SUCCESS;
  is_multi_table = false;
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (stmt->get_from_item_size() > 1) {
    is_multi_table = true;
  } else {
    const TableItem *table_item = stmt->get_table_item(stmt->get_from_item(0));
    if (OB_ISNULL(table_item)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (table_item->is_joined_table()) {
      is_multi_table = true;
    } else if ((table_item->is_generated_table() || table_item->is_temp_table()) &&
               OB_FAIL(is_join_table_update(table_item->ref_query_, is_multi_table))) {
    }
  }
  return ret;
}

int ObUpdateResolver::generate_batched_stmt_info()
{
  int ret = OB_SUCCESS;
  //extract all predicate column from condition exprs
  //see the issue: 
  ObSEArray<ObRawExpr*, 4> predicate_columns;
  ObUpdateStmt *update_stmt = get_update_stmt();
  if (OB_ISNULL(update_stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObRawExprUtils::extract_column_exprs(update_stmt->get_condition_exprs(),
                                                          predicate_columns))) {
  } else {
    ObIArray<ObUpdateTableInfo*> &tables_info = update_stmt->get_update_table_info();
    for (int64_t i = 0; OB_SUCC(ret) && i < tables_info.count(); ++i) {
      if (OB_ISNULL(tables_info.at(i))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObIArray<ObAssignment> &assignments = tables_info.at(i)->assignments_;
        for (int64_t j = 0; OB_SUCC(ret) && j < assignments.count(); ++j) {
          ObAssignment &assignment = assignments.at(j);
          ObRawExpr *column_expr = assignment.column_expr_;
          bool contain_case_when = false;
          if (has_exist_in_array(predicate_columns, column_expr)) {
            assignment.is_predicate_column_ = true;
          } else if (OB_FAIL(ObRawExprUtils::check_contain_case_when_exprs(assignment.expr_,
                                                                           contain_case_when))) {
          } else if (contain_case_when) {
            ret = OB_BATCHED_MULTI_STMT_ROLLBACK;
          }
        }

      }
    }
  }
  return ret;
}

int ObUpdateResolver::resolve_update_constraints()
{
  int ret = OB_SUCCESS;
  ObUpdateStmt *update_stmt = get_update_stmt();
  if (OB_ISNULL(update_stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObIArray<ObUpdateTableInfo*> &tables_info = update_stmt->get_update_table_info();
    // resolve view-check exprs
    for (int64_t i = 0; OB_SUCC(ret) && i < tables_info.count(); i++) {
      TableItem *table_item = NULL;
      ObUpdateTableInfo* table_info = tables_info.at(i);
      if (OB_ISNULL(table_info) ||
          OB_ISNULL(table_item = update_stmt->get_table_item_by_id(table_info->table_id_))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(resolve_view_check_exprs(table_item->table_id_, table_item, false, table_info->view_check_exprs_))) {
      } else if (OB_FAIL(resolve_check_constraints(table_item, table_info->check_constraint_exprs_))) {
      } else if (OB_FAIL(ObResolverUtils::prune_check_constraints(table_info->assignments_,
                                                                  table_info->check_constraint_exprs_))) {
      } else {
        // TODO @yibo remove view check exprs in log_del_upd
        for (uint64_t j = 0; OB_SUCC(ret) && j < table_info->view_check_exprs_.count(); ++j) {
          if (OB_FAIL(ObTableAssignment::expand_expr(*params_.expr_factory_,
                                                     table_info->assignments_,
                                                     table_info->view_check_exprs_.at(j)))) {
          }
        }
      }
    }
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
