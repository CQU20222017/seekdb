# 赛题三：AI 列（AI COLUMN）—— 现状调研与实施计划

> 本文件记录赛题三的仓库现状、目标语义、分阶段实施计划与风险。
> 验收基线为 `tools/deploy/mysql_test/test_suite/ai_column/` 下的 5 组 `.test` / `.result`。

## 1. 仓库现状（已核实）

| 项 | 结论 |
| --- | --- |
| `src/` 中的 AI 列实现 | **完全没有**：`grep -r "ai_column\|AI_COLUMN\|AiColumn" src/` 零命中 |
| 系统表 `__all_ai_column` / `__all_ai_column_task` | 不存在（`src/share/inner_table/ob_inner_table_schema_def.py` 无定义） |
| 解析器 `AI COLUMN (...)` 语法 | 不存在（`sql_parser_mysql_mode.y` 的 `column_definition` 只有普通列/生成列） |
| 测试基线 | 存在：5 组 `.test` + `.result` + `include/ai_column_mock.py`（端口 19120~19124） |
| 题目文档提到的 `unittest/sql/resolver/test_ai_column_resolver.cpp`、`unittest/share/schema/test_ai_column_schema.cpp` | **不存在** |
| 题目文档提到的 `tools/ai_gateway/` | **不存在** |
| 相关可复用能力 | `AI_COMPLETE` / `AI_EMBED` 表达式、AI Model Endpoint 管理（`DBMS_AI_SERVICE`）、`ObColumnSchemaV2::is_hidden()` 隐藏列、VSAG 向量索引、`DBMS_AI_SERVICE` 的 PL 包、后台 duty task 框架 |

**结论：赛题三不是"补齐/修 bug"，而是要在内核里从零实现一套「异步 AI 列」子系统。**

## 2. 目标语义（由 5 个测试文件定义）

### 2.1 DDL 与元数据（`ai_column_ddl.test`，8 分）
```sql
create table docs (
  id bigint primary key,
  content varchar(200),
  tags json AI COLUMN (AI_COMPLETE('ep', concat('Classify: ', content)))
       WITH (sync_mode='async', max_retries=2),
  emb  vector(4) AI COLUMN (AI_EMBED('embed_ep', content))
       WITH (sync_mode='async', max_retries=3)
);
```
要求：
1. `CREATE TABLE` / `ALTER TABLE ... ADD COLUMN` 支持 AI 列定义，`SHOW CREATE TABLE` / 系统表可见。
2. 元数据落 `oceanbase.__all_ai_column`，至少包含
   `table_id, column_id, column_name, max_retries, state_column_id, request_column_id, result_spec_column_id, normalized_expr(规范化表达式文本), sync_mode, ...`。
3. 每个 AI 列额外生成 3 个隐藏列（`is_hidden = 1`）：状态列、请求标识列、结果规格列；结果列本身对用户可见（`is_hidden = 0`）。
4. 表达式与类型校验：
   - `vector(4) AI COLUMN (AI_EMBED(..., 3))` → **error 7600**（维度不一致）；
   - AI 列依赖另一个 AI 列 → **error 1235**（不支持链式依赖）；
   - JSON 结果类型的列要在 `normalized_expr` 中补充提示词 `Return exactly one valid JSON value...`（测试用 `like '%Return exactly one valid JSON value%'` 校验）。

### 2.2 DML 联动与事务一致性（`ai_column_t1_dml.test`）
- INSERT 生成任务；多行批量 INSERT 生成多条任务，`eligible_ts <= now(6)`。
- 依赖列（`content`）更新：结果列立即变 NULL（同一事务内可见），并生成任务；无关列（`title`）更新不生成任务、不失效。
- 主键更新：旧行消失、新主键行失效，任务里 `rowkey_predicate` 精确为新主键谓词文本（`` `id` = 2 ``）。
- 事务回滚：任务与失效一起回滚，且**不得**触发该事务的模型请求（mock 断言 `no-rollback-value`）。
- DELETE：删除行同时清理该行的 PENDING 任务。
- 任务表 `oceanbase.__all_ai_column_task` 至少包含 `table_id, column_id, rowkey_predicate, eligible_ts, status, ...`。

### 2.3 异步调度与结果写回（`ai_column_worker.test`）
- 逻辑行提交后由后台 worker 调模型；成功写回结果并置 READY；多 AI 列按「同一行同次变更」分组，一个事务发布。
- 自动重试（`max_retries`），超限进入 FAILED；失败结果保持 NULL。
- 请求标识 / 定义版本 / 行归属校验，防止迟到结果覆盖新数据（`slow-old` / `fast-new` 场景）。

### 2.4 条件重试与严格读取（`ai_column_retry_strict.test`）
- `ai_column_strict_read` 会话变量：ON 时语句引用的 AI 列只要有未就绪行即报错（列级检查）。
- 重试语句支持 `status` 伪列（`PENDING/READY/FAILED`），反引号 `` `status` `` 表示同名业务列。
- 默认读取：未完成/最终失败的 AI 列为 NULL。

### 2.5 AI 向量列与索引（`ai_column_vector_index.test`）
- 向量结果写入 `vector` 列并参与 VSAG 索引；失效时移除旧向量，写回后插入新向量；回滚保持一致。

## 3. 分阶段实施计划

| 阶段 | 内容 | 对应验收 | 主要改动面 |
| --- | --- | --- | --- |
| **M1 DDL 与元数据** | 语法 `AI COLUMN (...) WITH (...)`、RAW 表达式保存、`__all_ai_column` + 隐藏列、类型/维度/依赖校验、JSON 提示词补充、`ALTER ... ADD COLUMN` | `ai_column_ddl.test` | `sql_parser_mysql_mode.y`、`sql/resolver/ddl/*`、`share/schema/ob_column_schema.*`（新增 `ObAiColumnSchema`）、`dml/ob_ddl_service*`、`share/inner_table/ob_inner_table_schema_def.py`、`ob_inner_table_schema` 相关视图 |
| **M2 DML 任务生成与事务一致性** | INSERT/UPDATE/DELETE 触发任务、结果失效写 NULL、主键变更、`rowkey_predicate` 生成、`eligible_ts`、回滚一致性（事务内插入任务表行） | `ai_column_t1_dml.test` | `sql/das/*`、`sql/engine/table/ob_table_*_op*`、`storage/access/*`、`__all_ai_column_task` 定义 |
| **M3 异步 worker** | 后台线程领取任务（并发、持久化状态）、按行分组调用 AI 函数、原子发布结果与状态、自动重试/FAILED、请求标识与版本校验 | `ai_column_worker.test` | `observer/ai_service/*`（新增 AI column worker）、`observer/ob_server_duty_task*`、任务表读写代理 |
| **M4 严格读取与条件重试** | `ai_column_strict_read` 会话变量与列级检查、`status` 伪列、按条件重试语句 | `ai_column_retry_strict.test` | `share/system_variable/*`、`sql/resolver/*`（字段/伪列解析）、`sql/executor` 检查钩子、PL 重试接口 |
| **M5 向量列与索引** | 向量写回/失效与 VSAG 索引同步、回滚一致 | `ai_column_vector_index.test` | `storage/ddl/ob_hnsw_embedmgr.*`、`storage/vector_index/*`、写回路径 |
| **M6 专项** | 中断恢复、跨端点同 URL 限流、性能（文档中的配套评测项） | 专项 | — |

## 4. 验证环境（已就绪）

- WSL2 Ubuntu 24.04（20 逻辑核 / 7G 内存 / 945G 空闲）：`deps/3rd` 已按 el9 profile 全量就绪（7.4G），Rust 1.98.1 + cargo 已装好，`cmake/ninja/rpm2cpio/cpio/bison/flex` 已装。
- 构建命令（WSL）：`cd ~/seekdb && ./build.sh release --make -j6`；产物 `build_release/src/observer/seekdb`。
- 源码同步：`bash script/wsl_sync.sh`（从 `D:\seekdb-2026` 镜像到 `~/seekdb`，并把 `.sh/.deps/.py` 的 CRLF 归一，否则 shebang 报 `bash\r`）。
- 测试：mock 服务 `test_suite/ai_column/include/ai_column_mock.py`（19120~19124）＋ 回归脚本 `tools/competition/run_ai_function_tests.py --suite ai_column`。

## 5. 风险

1. **规模**：M1~M5 合计是数千行内核改动，涉及解析器、resolver、schema、DDL、DML、事务、后台任务、系统视图、会话变量与向量索引；单轮会话难以全部完成，建议按阶段交付。
2. **构建耗时**：全量构建约 2~3 小时（7G 内存下 `-j6`），每次语法/语义改动需要增量重编（分钟级）。
3. **回归成本**：`ai_column` 用例依赖真实后台 worker 与 sleep 轮询，跑一次 5 个用例需要分钟级时间且需要实例稳定。
4. **基线漂移**：新增 `__all_ai_column*` 内表会改变 `inner_table` 套件里 `desc` 类基线（与赛题无关，需单独说明）。