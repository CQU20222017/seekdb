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

#ifndef OB_SERVER_DUTY_TASK_H
#define OB_SERVER_DUTY_TASK_H
#include <stdint.h>
#include "lib/task/ob_timer.h"
#include "lib/allocator/page_arena.h"
#include "lib/thread/threads.h"

namespace oceanbase {
namespace observer {

class ObServerDutyTask
    : private common::ObTimerTask
{
  static constexpr int64_t SCHEDULE_PERIOD = 10 * 1000L * 1000L;
public:
  int schedule(common::ObTimer &timer);
  ObServerDutyTask();
private:
  void runTimerTask() override;
  void update_runtime_settings();

private:
  // Apply context-memory limits from the runtime configuration.
  int update_ctx_memory_throttle();
  common::ObArenaAllocator allocator_;
};

class ObSqlMemoryTimerTask : private common::ObTimerTask
{
public:
  int schedule(common::ObTimer &timer);
private:
  void runTimerTask() override;
private:
  static constexpr int64_t SCHEDULE_PERIOD = 3 * 1000L * 1000L;
};

// Publishes the result of the pending AI column work items (__all_ai_column_task) back into the
// user table. A dedicated thread is used on purpose: evaluating an AI expression issues an HTTP
// request that can take tens of seconds, so it must never run on a timer / duty-task thread.
// One work item group = (table_id, rowkey_predicate, request_id); every AI column of a row is
// evaluated by ONE statement and published in ONE transaction, so the results of a row become
// visible atomically (no partial visibility).
class ObAiColumnWorker : public lib::Threads
{
public:
  ObAiColumnWorker();
  virtual ~ObAiColumnWorker() {}
  void run1() override;
  static const int64_t TICK_INTERVAL_US = 200 * 1000L;  // idle poll interval
  static const int64_t MAX_TASKS_PER_TICK = 8;          // batch limit
  static const int64_t AI_CALL_TIMEOUT_US = 30 * 1000L * 1000L;
private:
  // One pending AI column work item as read from __all_ai_column_task.
  struct WorkItem
  {
    WorkItem()
      : table_id_(common::OB_INVALID_ID), column_id_(common::OB_INVALID_ID),
        retry_count_(0), max_retries_(0) {}
    uint64_t table_id_;
    uint64_t column_id_;
    int64_t retry_count_;
    int64_t max_retries_;
    common::ObString column_name_;
    common::ObString normalized_expr_;
    common::ObString rowkey_predicate_;
    common::ObString request_id_;
    common::ObString database_name_;
    common::ObString table_name_;
  };
  int do_one_tick();
  int load_candidates(common::ObIAllocator &alloc, common::ObIArray<WorkItem> &items);
  // claim/publish/delete one group of items sharing (table_id, rowkey_predicate, request_id)
  int process_group(const common::ObIArray<WorkItem> &items,
                    const int64_t start,
                    const int64_t end);
private:
  common::ObArenaAllocator allocator_;
};

}  // observer
}  // oceanbase

#endif /* OB_SERVER_DUTY_TASK_H */
