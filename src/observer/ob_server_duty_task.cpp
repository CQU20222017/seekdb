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

#define USING_LOG_PREFIX SERVER
#include "config_bridge.h"
#include "share/config/ob_config_helper.h"
#include "ob_server_duty_task.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/engine/ob_sql_memory_manager.h"
#include "observer/omt/ob_server_runtime.h"
#include "observer/ob_server.h"
#include "observer/ob_inner_sql_connection.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "share/ai_service/ob_ai_column_schema.h"

using namespace oceanbase::common;

namespace oceanbase {
using namespace share;
using namespace share::schema;
using namespace sql;
namespace observer {

ObServerDutyTask::ObServerDutyTask()
  : allocator_(ObModIds::OB_DUTY_TASK)
{
}

void ObServerDutyTask::runTimerTask()
{
  allocator_.reset_remain_one_page();
  update_runtime_settings();
}

int ObServerDutyTask::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObServerDutyTask::update_runtime_settings()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(update_ctx_memory_throttle())) {
  }
}

int ObServerDutyTask::update_ctx_memory_throttle()
{
  int ret = OB_SUCCESS;
  {
    ObCtxMemoryLimitChecker checker;
    uint64_t ctx_id = 0;
    int64_t limit = 0;
    ObMallocAllocator *alloc = ObMallocAllocator::get_instance();
    rust::String limits = config::_ctx_memory_limit();
    if (!checker.check(limits.c_str(), ctx_id, limit)) {
      // do nothing
    } else {
      if (limits.empty()) {
        ctx_id = ObCtxIds::MAX_CTX_ID;
        limit = INT64_MAX; // empty str means no limit, and not care ctx_id.
      }
      for (int i = 0; i < ObCtxIds::MAX_CTX_ID; i++) {
        if (ObCtxIds::WORK_AREA == i ||
            ObCtxIds::META_OBJ_CTX_ID == i) {
          continue;
        }
        auto allocator = alloc->get_ctx_allocator(i);
        if (OB_NOT_NULL(allocator)) {
          if (OB_FAIL(allocator->set_limit(ctx_id == i ? limit : INT64_MAX))) {
          }
        }
      }
    }
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////

int ObSqlMemoryTimerTask::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObSqlMemoryTimerTask::runTimerTask()
{
  int ret = OB_SUCCESS;
  {
    SERVER_MODULE_SCOPE {
      ObSqlMemoryManager *sql_mem_mgr = ::oceanbase::share::server_service<::oceanbase::sql::ObSqlMemoryManager>();
      if (OB_UNLIKELY(nullptr == sql_mem_mgr)) {
      } else if (OB_FAIL(sql_mem_mgr->calculate_global_bound_size())) {
      }
    }
  }
}

//////////////////////////////////////////////////////////////////////////////////////////

ObAiColumnWorker::ObAiColumnWorker()
  : lib::Threads(1),
    allocator_(ObModIds::OB_DUTY_TASK)
{
}

void ObAiColumnWorker::run1()
{
  int ret = OB_SUCCESS;
  while (!has_set_stop()) {
    if (OBSERVER.is_stopped()) {
      // this thread starts during the server init, before the server is marked as started;
      // wait instead of touching the storage (and the same check covers shutdown)
      ob_usleep(static_cast<useconds_t>(TICK_INTERVAL_US));
      continue;
    }
    if (OB_FAIL(do_one_tick())) {
      LOG_WARN("fail to run one ai column worker tick", KR(ret));
    }
    // the tick does not know whether more work is pending, poll again shortly
    ob_usleep(static_cast<useconds_t>(TICK_INTERVAL_US));
  }
  LOG_INFO("ai column worker stopped");
}

int ObAiColumnWorker::do_one_tick()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator &alloc = allocator_;
  ObSEArray<WorkItem, 8> items;
  alloc.reset_remain_one_page();
  if (OB_FAIL(load_candidates(alloc, items))) {
    LOG_WARN("fail to load the ai column candidates", KR(ret));
  } else if (items.empty()) {
    // nothing to do
  } else {
    LOG_TRACE("ai column candidates loaded", "n", items.count());
    // walk the candidates grouped by (table_id, rowkey_predicate, request_id)
    int64_t start = 0;
    while (OB_SUCC(ret) && start < items.count()) {
      int64_t end = start + 1;
      while (end < items.count()
             && items.at(end).table_id_ == items.at(start).table_id_
             && items.at(end).rowkey_predicate_ == items.at(start).rowkey_predicate_
             && items.at(end).request_id_ == items.at(start).request_id_) {
        ++end;
      }
      if (OB_FAIL(process_group(items, start, end))) {
        LOG_WARN("fail to process an ai column work item group", KR(ret), K(start), K(end));
      }
      start = end;
    }
  }
  return ret;
}

int ObAiColumnWorker::load_candidates(common::ObIAllocator &alloc,
                                      common::ObIArray<WorkItem> &items)
{
  int ret = OB_SUCCESS;
  sqlclient::ObISQLConnectionGuard guard;
  ObInnerSQLConnection *conn = NULL;
  ObSqlString sql;
  ObISQLClient::ReadResult res;
  if (OB_FAIL(OBSERVER.get_mysql_proxy().acquire_connection(guard, 0))) {
    LOG_WARN("fail to acquire an inner sql connection", KR(ret));
  } else if (OB_ISNULL(conn = static_cast<ObInnerSQLConnection *>(guard.get_ptr()))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(sql.assign_fmt(
                 "select task.table_id, task.column_id, task.rowkey_predicate, "
                 "task.request_id, task.retry_count, ai.column_name, ai.normalized_expr, "
                 "ai.max_retries, d.database_name, tb.table_name "
                 "from oceanbase.__all_ai_column_task task "
                 "join oceanbase.__all_ai_column ai on ai.table_id = task.table_id "
                 "and ai.column_id = task.column_id "
                 "join oceanbase.__all_table tb on tb.table_id = task.table_id "
                 "join oceanbase.__all_database d on d.database_id = tb.database_id "
                 "where task.state in (0, 3) and task.eligible_ts <= now(6) "
                 "and (task.claim_ts is null "
                 "or task.state = 0 "
                 "or task.claim_ts < date_add(now(6), interval -5 second)) "
                 "order by task.eligible_ts, task.table_id, task.rowkey_predicate, "
                 "task.request_id, task.column_id limit %ld",
                 MAX_TASKS_PER_TICK))) {
    LOG_WARN("fail to build the candidate sql", KR(ret));
  } else if (OB_FAIL(conn->execute_read(sql.ptr(), res))) {
    LOG_WARN("fail to read the ai column work items", KR(ret), K(sql));
  } else {
    ObMySQLResult *result = res.get_result();
    if (OB_ISNULL(result)) {
      ret = OB_ERR_UNEXPECTED;
    }
    while (OB_SUCC(ret)) {
      WorkItem item;
      ObString column_name;
      ObString normalized_expr;
      ObString predicate;
      ObString request_id;
      ObString db_name;
      ObString table_name;
      int64_t table_id = 0;
      int64_t column_id = 0;
      int64_t retry_count = 0;
      int64_t max_retries = 0;
      if (OB_FAIL(result->next())) {
        if (OB_ITER_END != ret) {
          LOG_WARN("fail to read the next ai column work item", KR(ret));
        }
      } else if (OB_FAIL(result->get_int("table_id", table_id))
                 || OB_FAIL(result->get_int("column_id", column_id))
                 || OB_FAIL(result->get_varchar("rowkey_predicate", predicate))
                 || OB_FAIL(result->get_varchar("request_id", request_id))
                 || OB_FAIL(result->get_int("retry_count", retry_count))
                 || OB_FAIL(result->get_varchar("column_name", column_name))
                 || OB_FAIL(result->get_varchar("normalized_expr", normalized_expr))
                 || OB_FAIL(result->get_int("max_retries", max_retries))
                 || OB_FAIL(result->get_varchar("database_name", db_name))
                 || OB_FAIL(result->get_varchar("table_name", table_name))) {
        LOG_WARN("fail to read the candidate columns", KR(ret));
      } else {
        item.table_id_ = static_cast<uint64_t>(table_id);
        item.column_id_ = static_cast<uint64_t>(column_id);
        item.retry_count_ = retry_count;
        item.max_retries_ = max_retries;
        if (OB_FAIL(ob_write_string(alloc, column_name, item.column_name_))
            || OB_FAIL(ob_write_string(alloc, normalized_expr, item.normalized_expr_))
            || OB_FAIL(ob_write_string(alloc, predicate, item.rowkey_predicate_))
            || OB_FAIL(ob_write_string(alloc, request_id, item.request_id_))
            || OB_FAIL(ob_write_string(alloc, db_name, item.database_name_))
            || OB_FAIL(ob_write_string(alloc, table_name, item.table_name_))) {
          LOG_WARN("fail to copy the candidate strings", KR(ret));
        } else if (OB_FAIL(items.push_back(item))) {
          LOG_WARN("fail to push the candidate", KR(ret));
        }
      }
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
    int tmp_ret = res.close();
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("fail to close the read result", K(tmp_ret));
    }
  }
  return ret;
}

int ObAiColumnWorker::process_group(const common::ObIArray<WorkItem> &items,
                                    const int64_t start,
                                    const int64_t end)
{
  int ret = OB_SUCCESS;
  const WorkItem &first = items.at(start);
  sqlclient::ObISQLConnectionGuard guard;
  ObInnerSQLConnection *conn = NULL;
  ObSqlString sql;
  int64_t affected_rows = 0;
  const int64_t group_size = end - start;
  if (OB_FAIL(OBSERVER.get_mysql_proxy().acquire_connection(guard, 0))) {
    LOG_WARN("fail to acquire an inner sql connection", KR(ret));
  } else if (OB_ISNULL(conn = static_cast<ObInnerSQLConnection *>(guard.get_ptr()))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    // the whole publish path runs in the AI column worker session: it is the only session that
    // may write an AI column, and its AI hooks are disabled (see is_ai_column_worker())
    conn->get_session().set_ai_column_worker(true);
    // 1. claim: state 0/3 + no claim mark -> in flight (state 3 + claim_ts)
    if (OB_FAIL(sql.assign_fmt(
            "update oceanbase.__all_ai_column_task set state = 3, claim_ts = now(6) "
            "where table_id = %lu and rowkey_predicate = '%.*s' and request_id = '%.*s' "
            "and state in (0, 3) and (claim_ts is null "
            "or state = 0 "
            "or claim_ts < date_add(now(6), interval -5 second))",
            first.table_id_,
            first.rowkey_predicate_.length(), first.rowkey_predicate_.ptr(),
            first.request_id_.length(), first.request_id_.ptr()))) {
      LOG_WARN("fail to build the claim sql", KR(ret));
    } else if (OB_FAIL(conn->execute_write(sql.ptr(), affected_rows))) {
      LOG_WARN("fail to claim the ai column work items", KR(ret), K(sql));
    } else if (affected_rows != group_size) {
      // Somebody else owns (part of) this group. Whatever we did claim has to be released,
      // otherwise those rows stay claimed and are skipped until the stale window expires.
      LOG_DEBUG("partial claim of an ai column work group", K(group_size), K(affected_rows));
      if (OB_FAIL(sql.assign_fmt(
              "update oceanbase.__all_ai_column_task set claim_ts = null "
              "where table_id = %lu and rowkey_predicate = '%.*s' and request_id = '%.*s' "
              "and claim_ts is not null",
              first.table_id_,
              first.rowkey_predicate_.length(), first.rowkey_predicate_.ptr(),
              first.request_id_.length(), first.request_id_.ptr()))) {
        LOG_WARN("fail to build the claim release sql", KR(ret));
      } else if (OB_FAIL(conn->execute_write(sql.ptr(), affected_rows))) {
        LOG_WARN("fail to release the partial claim", KR(ret), K(sql));
      }
      LOG_TRACE("skip a group that is already claimed", K(group_size), K(affected_rows));
    } else {
      // 2. publish: one statement, every AI column of this row, atomically visible
      ObSqlString set_clause;
      int64_t published = 0;
      int publish_ret = OB_SUCCESS;
      for (int64_t i = start; OB_SUCC(ret) && i < end; ++i) {
        const WorkItem &item = items.at(i);
        // The state / request markers are hidden columns. An ordinary session cannot even
        // resolve them (1054), but this publish runs on an inner SQL connection, and the
        // resolver lets hidden columns through when session_info->is_inner() is true
        // (see ObStmtResolver::get_column_schema). They are written in the very same
        // statement as the result, so a row becomes READY atomically.
        ObString state_column;
        ObString request_column;
        if (OB_FAIL(share::build_ai_hidden_column_name(allocator_,
                                                       share::AI_STATE_COLUMN_PREFIX,
                                                       item.column_name_,
                                                       state_column))) {
          LOG_WARN("fail to build the AI state column name", KR(ret));
        } else if (OB_FAIL(share::build_ai_hidden_column_name(allocator_,
                                                              share::AI_REQUEST_COLUMN_PREFIX,
                                                              item.column_name_,
                                                              request_column))) {
          LOG_WARN("fail to build the AI request column name", KR(ret));
        } else if (OB_FAIL(set_clause.append_fmt("%s%.*s = (%.*s), %.*s = 'READY', %.*s = '%.*s'",
                                                 i > start ? ", " : "",
                                                 item.column_name_.length(),
                                                 item.column_name_.ptr(),
                                                 item.normalized_expr_.length(),
                                                 item.normalized_expr_.ptr(),
                                                 state_column.length(), state_column.ptr(),
                                                 request_column.length(), request_column.ptr(),
                                                 item.request_id_.length(),
                                                 item.request_id_.ptr()))) {
          LOG_WARN("fail to build the publish set list", KR(ret));
        }
      }
      if (OB_FAIL(ret)) {
        publish_ret = ret;
      } else if (OB_FAIL(sql.assign_fmt("update %.*s.%.*s set %.*s where %.*s",
                                        first.database_name_.length(),
                                        first.database_name_.ptr(),
                                        first.table_name_.length(), first.table_name_.ptr(),
                                        set_clause.length(), set_clause.ptr(),
                                        first.rowkey_predicate_.length(),
                                        first.rowkey_predicate_.ptr()))) {
        publish_ret = ret;
      } else if (OB_FAIL(conn->execute_write(sql.ptr(), published))) {
        publish_ret = ret;
        LOG_WARN("fail to publish the ai column value", K(ret), K(sql));
      }
      ret = OB_SUCCESS;
      if (OB_SUCCESS != publish_ret) {
        // 3a. the publish failed: hand the work item back with a small backoff, or give up
        const int64_t next_retry = first.retry_count_ + 1;
        const bool give_up = next_retry > first.max_retries_;
        int64_t backoff_us = 100 * 1000L;
        for (int64_t i = 1; i < next_retry && backoff_us < 1000 * 1000L; ++i) {
          backoff_us *= 2;
        }
        if (backoff_us > 1000 * 1000L) {
          backoff_us = 1000 * 1000L;
        }
        if (give_up) {
          // terminal failure: mark the AI column as FAILED on the user row (best effort, the
          // hidden column may not be writable through SQL) and DROP the work item - a work
          // item never survives a terminal failure, otherwise it would be retried forever
          for (int64_t i = start; OB_SUCC(ret) && i < end; ++i) {
            const WorkItem &item = items.at(i);
            ObString state_column;
            if (OB_FAIL(share::build_ai_hidden_column_name(allocator_,
                                                           share::AI_STATE_COLUMN_PREFIX,
                                                           item.column_name_,
                                                           state_column))) {
              LOG_WARN("fail to build the AI state column name", KR(ret));
            } else if (OB_FAIL(sql.assign_fmt(
                         "update %.*s.%.*s set %.*s = 'FAILED' where %.*s",
                         item.database_name_.length(), item.database_name_.ptr(),
                         item.table_name_.length(), item.table_name_.ptr(),
                         state_column.length(), state_column.ptr(),
                         item.rowkey_predicate_.length(), item.rowkey_predicate_.ptr()))) {
              LOG_WARN("fail to build the failed state sql", KR(ret));
            } else if (OB_FAIL(conn->execute_write(sql.ptr(), affected_rows))) {
              LOG_WARN("fail to mark the AI column as failed", KR(ret), K(sql));
              ret = OB_SUCCESS;
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(sql.assign_fmt(
                       "delete from oceanbase.__all_ai_column_task where table_id = %lu "
                       "and rowkey_predicate = '%.*s' and request_id = '%.*s'",
                       first.table_id_,
                       first.rowkey_predicate_.length(), first.rowkey_predicate_.ptr(),
                       first.request_id_.length(), first.request_id_.ptr()))) {
            LOG_WARN("fail to build the terminal failure cleanup sql", KR(ret));
          } else if (OB_FAIL(conn->execute_write(sql.ptr(), affected_rows))) {
            LOG_WARN("fail to drop the failed ai column work items", KR(ret), K(sql));
          } else {
            LOG_WARN("ai column work item failed for good", K(publish_ret), K(next_retry),
                     K(affected_rows), K(group_size), K(first.request_id_));
          }
        } else if (OB_FAIL(sql.assign_fmt(
                     "update oceanbase.__all_ai_column_task set state = 3, retry_count = %ld, "
                     "claim_ts = null, eligible_ts = date_add(now(6), interval %ld microsecond) "
                     "where table_id = %lu and rowkey_predicate = '%.*s' and request_id = '%.*s'",
                     next_retry, backoff_us, first.table_id_,
                     first.rowkey_predicate_.length(), first.rowkey_predicate_.ptr(),
                     first.request_id_.length(), first.request_id_.ptr()))) {
          LOG_WARN("fail to build the retry sql", KR(ret));
        } else if (OB_FAIL(conn->execute_write(sql.ptr(), affected_rows))) {
          LOG_WARN("fail to release the ai column work items", KR(ret), K(sql));
        } else {
          LOG_TRACE("ai column work item will be retried", K(publish_ret), K(next_retry),
                    K(first.request_id_));
        }
      } else {
        // 3b. the row may have been deleted concurrently (published == 0): not an error, the
        // work item is dropped as well
        LOG_TRACE("ai column work item published", K(published), K(group_size));
        if (OB_FAIL(sql.assign_fmt(
                "delete from oceanbase.__all_ai_column_task where table_id = %lu "
                "and rowkey_predicate = '%.*s' and request_id = '%.*s'",
                first.table_id_,
                first.rowkey_predicate_.length(), first.rowkey_predicate_.ptr(),
                first.request_id_.length(), first.request_id_.ptr()))) {
          LOG_WARN("fail to build the finish sql", KR(ret));
        } else if (OB_FAIL(conn->execute_write(sql.ptr(), affected_rows))) {
          LOG_WARN("fail to drop the finished ai column work items", KR(ret), K(sql));
        }
      }
    }
  }
  return ret;
}

}  // observer
}  // oceanbase
