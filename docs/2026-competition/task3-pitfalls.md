# 赛题三：内核坑汇总（M1/M2 实战踩坑记录）

> 记录实现过程中真实踩到、且后续 M3~M5 大概率还会遇到的坑。
> 每条都给出「现象 → 根因 → 修法/规避」，避免重复排查。

## 1. `-4016 OB_ERR_UNEXPECTED`：隐藏列 column_id 与 prev/next 链不同序（M1）

**现象**：`CREATE TABLE` 里只要 AI 列**后面还有别的列**就报 `ERROR HY000: Internal error (4016)`；
AI 列放最后一列则正常。`ALTER TABLE ADD COLUMN` 不受影响。

**根因**：CREATE TABLE 的 resolver 第一遍就给所有用户列预分配 column_id（1..N）。
若在第二遍遍历时**就地**给 AI 列追加 3 个隐藏列，隐藏列拿到 N+1..N+3，而紧随其后的普通列
仍然用预分配的 N+4 之前的号 → prev/next 链上 column_id 不单调 → tablet 创建阶段
`ObStorageSchema`/`ObColumnIterByPrevNextID` 相关断言失败，抛 `OB_ERR_UNEXPECTED`。

**修法**：两阶段分配——先按用户列原顺序把所有列加完，再统一追加隐藏列
（`ObCreateTableResolver::resolve_table_elements` 末尾）。

## 2. AI 规格文本的字段分隔符被自己的转义函数吃掉（M1）

**现象**：落元数据时报 `OB_INVALID_DATA(-4070)`；服务端日志里规格文本看起来完全正确。

**根因**：`build_ai_column_spec_text()` 用 `append_escaped()` 写 `"="` 和 `"\n"`，
于是换行被转义成**字面量反斜杠 + n**，解析端按 `\n` 切行时切不开 → 整段被当成一行。

**修法**：分隔符用新加的 `append_raw()`（不转义），`append_escaped()` 只用于字段值。

## 3. `ObFixedArray` / `UIntFixedArray` 必须先 `set_capacity()` 再 `push_back()`（M2）

**现象**：普通表 DML 完全正常，但只要目标表有 AI 列，`INSERT` 就报
`ERROR HY000: The object is not initialized (4006)`，且**日志里没有任何 WARN/ERROR**。

**根因**：`ObDMLBaseCtDef` 里新增的 `common::ObFixedArray`（`UIntFixedArray`）没有预留容量，
`push_back()` 在未 init / capacity=0 时直接返回 **`OB_NOT_INIT`(4006)**，把计划生成打断。

**修法**：像既有代码那样先 `xxx.set_capacity(n)`（注意它返回 `void`，不能套 `OB_FAIL`）。

## 4. `has_nested_sql` 会被 `QueryCtx` 覆盖（M2）

**现象**：想在 DML 行循环里执行嵌套 SQL，但 `ObTableModifyOp::open_inner_conn()` 从未被调用，
`execute_write()` 返回 `OB_NOT_INIT(4006)`。

**根因**：`ObTableModifyOp::inner_open()` 只在 `MY_SPEC.plan_->has_nested_sql()` 时打开 inner conn；
而 `ObStaticEngineCG::generate()` 末尾（`ob_static_engine_cg.cpp:5715`）会用
`exec_ctx->get_stmt_factory()->get_query_ctx()->has_nested_sql()` **覆盖** plan 上的标志。
所以只在 CG 里 `phy_plan_->set_has_nested_sql(true)` 是无效的。

**修法**：在 CG（`ObDmlCgService::fill_ai_column_ctdef`）里同时
`log_plan->get_optimizer_context().get_query_ctx()->set_has_nested_sql(true)`
（`ObQueryCtx` 有 public setter），plan 上的标志自然就对了；
另加 `ObTableModifyOp::open_inner_conn_if_need()` 作为兜底（`inner_close()` 里
`close_inner_conn()` 本来就会回收）。

## 5. 默认 `syslog_level = EDIAG`：WARN 不落盘（M1/M2 排查效率杀手）

**现象**：代码里 `LOG_WARN` 明明执行了，`seekdb_run/log/seekdb.log` 里一条都搜不到，
会误判成「代码没跑到」。

**根因**：实例默认参数 `syslog_level = EDIAG`，WARN 级别被过滤（也没有 `.wf` 文件）。
运行时 `alter system set syslog_level='WARN'` 对当前这个 seekdb 版本的文件日志**不生效**。

**规避**：临时探针一律用 **`LOG_ERROR`**（EDIAG 可见），并在日志里带固定前缀（如 `AIDBG1`）
便于 `grep`；收尾时清理掉。

## 6. nested SQL 写系统表：触发器可以，`ObTableModifyOp::execute_write()` 当前不行（M2 待解）

**现象**：在 `ObDMLService::process_insert_row` / 语句收尾里调用
`dml_op.execute_write("INSERT INTO oceanbase.__all_ai_column_task ...")` 返回 `-4016`；
**同一句 SQL 在普通会话里成功**，且用**触发器**（`after insert ... insert into
oceanbase.__all_ai_column_task ...`，含 `on duplicate key update`）也**成功**。

**已排除**：
- 会话/连接生命周期（惰性 open + 每语句一个 nested session，`inner_close()` 统一回收）；
- `begin_nested_session(true/false)` 两种参数；
- 写入时机（行循环内 vs 语句收尾 `write_rows_post_proc`）——两种都 4016；
- SQL 语法/upsert/`now(6)`——普通会话与触发器都验证过可用。

**结论（已与 lead 确认）**：不是「系统表能不能被 nested SQL/DAS 写」的问题
（普通会话能成功本身就说明它就是走 DAS 写的），坏的是 **nested session 这一层上下文**。
**决定改走 DAS 路线**：在 DML 收尾时通过 `dml_rtctx_->das_ref_` 追加面向任务表的
`ObDASDeleteOp`（先）+ `ObDASInsertOp`（后，多行一批），复用同一 `tx_desc` 保证同事务；
一个语句最多追加 2 个额外 DAS op。

## 7. 业务 SQL 直接给 AI 结果列赋值必须拒绝（M2 已实现）

**要求**：题目 2.1 第 6 条——业务 SQL 不得直接赋值修改 AI 结果列，否则会和「依赖列变化
→ 结果列置 NULL」的失效规则互相打架。

**实现**：`ObDmlCgService::check_ai_column_not_written()`（在 `fill_ai_column_ctdef` 里调用，
只有表上真的有 AI 列时才跑）：
- `UPDATE ... SET <ai_col> = ...`：遍历 `index_dml_info.assignments_`，跳过 `is_implicit_`
  的内部赋值，命中 AI 结果列即返回 `OB_NOT_SUPPORTED`；
- `INSERT INTO t(..., <ai_col>, ...)`：遍历 `ObInsertStmt::get_values_desc()` 同样拒绝。

**实测**：`update ... set tags = NULL` / `set tags = '{}'` / `insert ... (…, tags, …)` 均返回
`ERROR 1235: assigning an AI column directly not supported`；不写 AI 列的 DML 不受影响。

## 8. UPDATE 里 `old_row_` / `new_row_` 对未赋值列**共享同一个 `ObExpr`**（M2）

**现象**：想照 `init_heap_table_pk_for_ins()` 的写法在 `process_update_row` 里对 AI 结果列
做 `locate_datum_for_write()` + `set_null()`，普通 UPDATE（依赖列）能跑通，但**主键更新**报
`ERROR 4377: fatal internal error in [check_old_row_legitimacy]`。

**根因**：`ObDmlCgService::generate_update_ctdef()` 里 `new_row` 是 `old_row` 的拷贝，只有被
赋值的列替换成 assign 表达式；而 `ObStaticEngineExprCG::generate_rt_expr()` 对同一个 raw expr
直接返回 `raw_expr.rt_expr_`（**同一个对象**）。所以未被赋值的列，`upd_ctdef.old_row_.at(i)`
与 `upd_ctdef.new_row_.at(i)` 是同一个 `ObExpr`，它的 datum 只有一份：给 new row 置 NULL，
old row（以及 `full_row_`、以及 storage 侧“旧行必须与 SQL 旧行一致”的防御校验）一起被改脏。

**修法（已落地）**：失效判定上移到**编译期/语句级**——`collect_ai_invalidation_column_ids()`
在 CG 决定“本语句要失效哪些 AI 列”（命中 `dep_columns` 赋值，或行键变更→全部），运行期只对
这个**静态列表**逐行置 NULL + 记工作项，两边口径一致。
**已知副作用**：`update t set content = content`（赋同值）也会失效重算一次，方向是保守的
（多算不少算），M4/M5 有余力再改回逐行 `binary_equal` 比对。

## 9. DAS delete 删「已不存在的 rowkey」会被防御校验判 `-4377`（M2）

**现象**：主键更新 `update t set id = 2 where id = 1` 报 `4377 check_old_row_legitimacy`；
把两个待删谓词的行先手工造出来后，同一条语句完全正常。

**根因**：任务行的 upsert 用「先 DAS delete 掉同 key、再 DAS insert」实现（DAS 内部保证
delete 聚合先执行）。但 `ObLSTabletService::check_old_row_legitimacy()` 在
`_enable_defensive_check >= 1`（本实例默认 1）时要求 SQL 给出的旧行**必须能在 storage 里找到**
——普通 SQL delete 的行来自扫描、天然存在，而我们是按 rowkey 手工构造的，工作项可能已经被
M3 worker 消费掉（或本语句刚为“新谓词”构造的 key 本来就不存在）→ 直接 4377。

**修法**：`ai_task_del_ctdef_.is_main_table_in_fts_ddl_ = true` + `is_total_quantity_log_ = false`，
并在 `ObDMLRunningCtx::check_need_old_row_legitimacy()`（`src/storage/ob_dml_running_ctx.cpp`）
里核实过：能关掉这条校验的 ctdef 级开关**只有** `is_main_table_in_fts_ddl_`（其余是
`has_udf_column` / `_enable_defensive_check` / 索引表不可读 / `DF_LOCK`，都与我们无关），
`dml_param_.is_ignore_` 只影响唯一键冲突那一支、**不**影响旧行校验。
该 flag 只作用于我们自建的任务表 delete op（不进用户 plan、不影响用户表的任何语义）；
外层语句的 `affected_rows`/`ROW_COUNT()` 已实测与普通表完全一致
（insert 3→3、update 1→1 / 2→2 / 0→0、pk 更新 1→1、delete 2→2 / 0→0）。
代价是语义上“借用”了 FTS 的开关，属可接受的 hack，已在此记录；若将来内核提供
“允许删除不存在的行”的正式开关，应替换掉。

## 10. DAS update 只写 `updated_column_ids_` 里的列（M2）

**现象**：AI 结果列在 `new_row` 里被置 NULL 了，但用户表上的 `tags` 读出来还是旧值。

**根因**：`ObDASUpdCtDef::updated_column_ids_` 只由 `generate_updated_column_ids()`（= 用户 SET
列表）生成，并且在**函数开头就 `reset()`**（`ObFixedArray::reset()` 会把 capacity 也清零），
所以“提前塞进去”无效；而 `generate_das_projector()` 又是按
`minimal_column_ids = updated ∪ rowkey` 决定哪些列有 projector 的，不在里面就是 nop。

**修法**：在 `generate_das_upd_ctdef()` 里先用临时 `ObSEArray` 拼出
`updated = 用户 SET 列表 + 本语句要失效的 AI 列`，再 `updated_column_ids_.reset()` + `assign()`
一次性写回（`ObFixedArray::push_back` 超过 capacity 会返回 `OB_SIZE_OVERFLOW`，不能边算边 push）。
另外只在**数据表自己的 ctdef**（`index_tid == ref_table_id`）上做，索引 ctdef 不能带上。

## 11. 任务表的两条「用例契约」不是随便定的（M2 收尾时对着 `.test` 反推出来的）

把 4 个 `ai_column_*.test` 里所有 `task.xxx` / `__all_ai_column*` 的用法抽出来，得到两条硬约束
（改 schema 或 request_id 生成规则前务必先看这里）：

1. **状态列叫 `state`，不叫 `status`**：`ai_column_worker.test` 直接断言
   `task.state = 3 and task.retry_count = 1`。0 PENDING / 1 READY / 2 FAILED / 3 RETRYING。
2. **`request_id` 的粒度是「一行一次变更」，不是「一行一列一次变更」**：
   `ai_column_worker.test` 断言同一行 2 个 AI 列
   `count(*) = 2 and count(distinct task.request_id) = 1`（一次请求把该行所有 AI 列一起算完，
   结果必须原子可见，`no_partial_visibility`）。所以格式是
   `<table_id>:<rowkey_predicate>:<语句时刻微秒>`——**不能**带 `column_id`。
   （最初按 `task3-m2-plan.md` §9.5 写成带 `column_id` 的格式，会得到 2 个 id，已改。）

`eligible_ts` / `rowkey_predicate` / `retry_count` / `table_id` / `column_id` 的用法与 M2 实现一致。

## 12. M3 worker 的写回必须同时绕过「禁止赋值」**和**整条 AI 钩子（M3 地基）

**为什么**：worker 用内部 `update <用户表> set <ai_col> = (<normalized_expr>) …` 发布结果，
这条语句会走 `process_update_row`。如果只放行「禁止直接写 AI 列」那个校验、却没关掉 M2 的
AI 钩子（置 NULL / 记工作项），那么**每写回一次就会生成一条新任务 → 无限循环**（模型被反复调用，
评测会抓“重复推理”）。

**已落地（地基）**：
- `ObSQLSessionInfo` 新增 `is_ai_column_worker()` / `set_ai_column_worker()`；
- `ObDmlCgService::check_ai_column_not_written()` 对这个标记**提前 return success**；
- `ObDMLService::process_insert_row/process_update_row/process_delete_row` 里的三处 AI 钩子
  都加了 `!is_ai_column_worker_op(dml_op)` 短路。
- 任务表新增 `claim_ts timestamp:6`（在飞标记）：认领时写入、重试/结束清空，
  selector 只挑 `state in (0,3) and eligible_ts <= now(6) and claim_ts is null`，
  避免「在飞」与「待重试」混淆导致**双发模型请求**。

**状态编码（唯一权威，实现里请用常量）**：0 PENDING / 1 READY / 2 FAILED / 3 RETRYING
（3 = 已领取或失败待重试，二者靠 `claim_ts` 区分）；`retry_count > max_retries → 2` 并保留行。

## 13. worker 线程「起来了但一个 tick 都不跑」：`OBSERVER.is_stopped()` 在 init 阶段为真（M3）

**现象**：`lib::Threads::start()` 成功、`run1()` 的入口日志打了，但之后的 tick 日志一条没有，
任务行永远不被领取。

**根因**：`ObAiColumnWorker` 在线程体里用 `while (!has_set_stop() && !OBSERVER.is_stopped())`
做守卫，而线程是在 `ObServer::init_server_runtime()` 里启动的——**那个时刻 server 还没被标记为
started**，`is_stopped()` 恒为真 → 循环体一次都不执行。

**修法**：守卫只用 `has_set_stop()`（`stop()` 会置位），`OBSERVER.is_stopped()` 只在循环体里
「为真就 sleep 后 continue」——既覆盖「还没启动」也覆盖「正在停机」。

## 14. `normalized_expr` 里 JSON 结果契约只是 SQL 注释，重放时**不会进 prompt**（M3 发现，待 M1 修）

**现象**：worker 用 `update <t> set tags = (normalized_expr) …` 重放时，AI 调用成功返回，
但写 `json` 列时报 `-5411 OB_ERR_INVALID_JSON_TEXT`（`tags` 是 `json` 列）。

**根因**：M1 把「结果契约」以 SQL 注释形式存进 `normalized_expr`：
`AI_COMPLETE('ep', concat('Tags: ', content)) /* Return exactly one valid JSON value and nothing else. */`
这段注释满足 DDL 用例的 `normalized_expr like '%Return exactly one valid JSON value%'` 断言，
但它只是注释——**不会出现在发给模型的 prompt 里**，于是模型返回纯文本，写 JSON 列失败。

**修法（M1 侧，两选一）**：
1. 把契约拼进 prompt 参数（例如 `concat(concat('Tags: ', content), ' Return exactly one valid JSON value…')`），
   同时保留注释形式以满足 DDL 用例的 like 断言；或
2. json 结果类型改写 `config` 参数（`{"response_format":{"type":"json_object"}}` 之类），让 provider 直接返回 JSON。
**不要**在 worker 里给结果类型特判——那会让 M5 的 `vector(N)` 列重放再踩一次。

> **已按「config 的 `system_prompt` → system 消息」落地**（第 15 条），JSON 与整型两类都已通。

## 15. mock 契约：**user 消息放数据、system 消息放契约**（M3）

`tools/deploy/mysql_test/test_suite/ai_column/include/ai_column_mock.py` 按 **system 消息内容**决定返回：

| system 消息包含 | 返回 |
| --- | --- |
| `base-10 integer` | `7` |
| `valid JSON value` | `{"category":"mock","input":<user 消息>}` |
| 其它（空） | `mock:<user 消息>` |

`AI_COMPLETE` 原实现里 `prompt_str` 恒为空 → **永远不发 system 消息**，prompt 全进 user，
任何「结果类型契约」都不会生效。落地方式：
- `ObOpenAIComplete::extract_system_prompt()`：把 config 里的字符串成员 `system_prompt` 取出作
  **system 消息**，并从 config 中 `remove()`（不下发服务端）；
- `build_normalized_expr()`：JSON 结果注入 `'{"system_prompt":"Return exactly one valid JSON value…"}'`，
  **整型结果注入 `'{"system_prompt":"Return exactly one base-10 integer and nothing else."}'`**
  （`AI_INTEGER_RESULT_CONTRACT`），自由文本不注入。
- 少了整型这条时，`score bigint` 列会拿到 `mock:Score: …` 文本 → 写回报
  `-4226 OB_ERR_TRUNCATED_WRONG_VALUE_FOR_FIELD`（M3 调试时踩的坑）。

## 16. 终态失败后**任务行必须被删除**（M3，M4 手动重试依赖它）

`.test` 用 `max_retries=0` 的 `fail_docs` 断言：进入 FAILED 后轮询 `count(*) from __all_ai_column_task`
**必须变成 0**。语义是「**重试中保留任务行、终态失败删任务行**」：
- 重试中：`state=3`、`retry_count+1`、`claim_ts=null`、`eligible_ts=now+backoff(100ms→1s)` —— 行保留；
- 终态（`retry_count > max_retries`）：先把 `__ai_state_<col>` 置 `'FAILED'`（best effort，
  隐藏列可能不允许 SQL 赋值，失败只记 WARN），**再删除该任务行**。
M4 的严格读取/手动重试按「用户行状态列 + 任务表里是否还有未完成任务」判断；
手动重试需要把任务行**重新插入**。

## 17. worker 必须用**独立线程**且线程体首行不能依赖 `OBSERVER.is_stopped()`（M3）

**现象**：`lib::Threads::start()` 成功、`run1()` 也进了，但一个 tick 都不跑。
**根因**：线程是在 `ObServer::init_server_runtime()` 里启动的，那一刻 server 还没标记 started，
若用 `while (!has_set_stop() && !OBSERVER.is_stopped())` 做守卫，循环体一次都不执行。
**修法**：守卫只用 `has_set_stop()`，`is_stopped()` 放到循环体里「为真就 sleep 后 continue」。
另外覆盖 `run1()`（不是 `run(int64_t)`）——后者是 `Threads` 内部用来建 worker 上下文的入口。
## 18. 隐藏列对 SQL **完全不可见**：` update t set __ai_state_x='READY' ` 报 1054（M4 关键）

**现象**：想给用户行的 AI 状态隐藏列写 ` 'READY'/'FAILED' `，SQL 报
`1054 Unknown column '__ai_state_tags' in 'field list'`（同样的列在 `__all_column` 里
`is_hidden=1`、`select *` 不返回，解析期就找不到）。

**影响（重要）**：
- M3 里「终态失败先写 `__ai_state_<col>='FAILED'` 再删任务行」的那半步**实际永远失败**
  （只记 WARN，所以不影响用例通过），FAILED 状态目前**没有落在用户行上**。
- 因此严格读取（M4 的 11120）暂时无法「以状态列为主判据」：终态 FAILED 后任务行已被删除，
  只能退化为「任务表有 `state in (0,3)` **或** 该行结果列 IS NULL」来判定未完成。
- 要让状态真正落盘，必须走 **DAS 写路径**（与 M2 的任务表写入同一套：`ObDASUpdateOp` +
  自建 ctdef，`column_ids_ = [状态列] + rowkey`），**不能再走 SQL**。

**但是——内部会话可以解析隐藏列（实测，省掉了整套 DAS 写入）**：
`ObStmtResolver::get_column_schema` 里 `const bool hidden = get_hidden || session_info_->is_inner();`，
也就是说 **`session_info_->is_inner()` 为真时隐藏列放行**。worker 的连接是
`ObInnerSQLConnection`（内部会话），所以在它的 UPDATE 里直接写隐藏列**完全可行**：
实测把发布语句改成 `set tags = (…), __ai_state_tags = 'READY', __ai_request_tags = '<req>'` 之后，
`ai_column_worker`(119/119)、`ai_column_t1_dml`(166/166) **仍然全绿**，证明写入成功。

**结论（M4 采用）**：
- **写**：worker 发布语句里「结果列 + `__ai_state_<col>='READY'` + `__ai_request_<col>`」一条语句写完（原子）；
  终态失败时在**同一条内部连接**上先写 `'FAILED'` 再删任务行。**不需要自建 DAS update**。
- **读**：严格读取 / RETRY 的 `status` 伪列也用**内部连接**查
  `select 1 from <db>.<t> where __ai_state_<col> is null or __ai_state_<col> <> 'READY' limit 1`（内部会话可见隐藏列）。
- 普通用户会话仍然会 1054 —— 这是**特性**，不是缺陷。

## 19. 仓库自带 gen_ob_sys_variables.py 有 Python2 遗留（已修）

**现象**：在 py3 下跑生成器，最后一步 `generate_essential_sys_vars_in_init_cpp` 报
`AttributeError: 'dict' object has no attribute 'iteritems'`。
**修法**：把 `.iteritems()` 改成 `.items()`（3 处）。修完可完整跑通。
**注意（改系统变量时的正确做法）**：
1. 先改 `ob_system_variable_init.json`（选一个未占用的 id，生成器会校验唯一性）；
2. 跑生成器产出派生文件；
3. **`ob_system_variable_init.cpp` 不要用生成器输出**——新版生成器会重排每个变量的字段赋值，
   产生 4000+ 行纯噪音 diff。正确做法是先 `git checkout` 该文件，然后**手工只插入本变量那一个
   `[&](){…}();` block**（并把文件末尾的 `var_amount` +1）。手插后 diff 只有十几行，可评审。
## 20. 严格的「未就绪」判定与 11120 钩子（M4 已落地）

**钩子位置**：`ObTableScanOp::inner_open`，在 `init_table_scan_rtdef()` 之前调用
`check_ai_column_strict_read(ctx, MY_SPEC.ref_table_id_, MY_CTDEF.get_full_acccess_cids())`。
选 scan 的理由：SELECT / UPDATE / DELETE / 子查询都会先开 scan → **报错必然先于任何写入**；
`get_full_acccess_cids()` 含 WHERE/SELECT 的访问列 → 天然是**列级**判定。

**必须的三道闸**（少一道就出事）：
1. `session->is_inner() || session->is_ai_column_worker()` → 直接跳过。钩子内部要跑内部 SQL，
   不跳过就会**无限递归**；
2. `thread_local bool in_ai_strict_read_check` 重入标记兜底；
3. **先读会话变量** `SYS_VAR_AI_COLUMN_STRICT_READ`，OFF 立即返回 —— 默认对所有普通查询**零额外 SQL**
   （「不破坏普通 SQL」是评测项）。

**未就绪判据（状态优先 + 兜底）**：
- 主判据：内部连接查 `select 1 from <db>.<t> where __ai_state_<col> is null or __ai_state_<col> <> 'READY' limit 1`
  （内部会话可见隐藏列，见第 18 条）；NULL = 从未算过，同样算未就绪；
- 兜底：`exists(task where table_id=? and column_id=? and state in (0,3))`
  —— **仅覆盖两个窗口**：DML 刚失效但状态列还没置 PENDING、worker 在飞。
- 命中则 `OB_ERR_AI_COLUMN_UNFINISHED_TASK = -11120`：
  `AI column has unfinished tasks for table_id=%lu, column_id=%lu`（sqlstate HY000）。
  新增错误码要**三处同步**：`ob_errno.h` 里的 `constexpr` + `OB_..._USER_ERROR_MSG` 宏、
  `ob_errno.cpp` 的 `_error_` 表项与 `_errors[]` 注册、`g_all_ob_errnos[]`（连长度一起改）。

## 21. ALTER TABLE ... RETRY AI COLUMN 的语法落点（M4 待做，已侦察清楚）

- 令牌：`sql_parser_mysql_mode.y` 第 262 行的 `%token <node> ... AI ...` 列表里加 `RETRY`；
  非保留关键字规则（约 17570 行 `non_reserved_keyword:`）里加 `| RETRY`；
  关键字表 `non_reserved_keywords_mysql_mode.c` 加 `{"retry", RETRY},`（`AI` 就是这样注册的，第 32 行）。
- 语法：在 `standalone_alter_action`（14859 行起）加一条
  `RETRY AI COLUMN column_name opt_where`，产出自己的节点（不要塞进 AlterTableSchema）。
- 语义（用例全部断言）：非 AI 列 → **1235**；与其它 ALTER 动作混用 → **1235**；
  零命中 → no-op **不建任务行**；READY 行可重试（结果置 NULL + 重插任务）；FAILED 行也可重试。
- WHERE 里的 `status` 是**伪列**：未加反引号 → 翻译成该列的状态隐藏列 `__ai_state_<col>`；
  加了反引号的 `\\status\\` → 保留为业务列。
- 执行（三步，同一内部事务）：select 命中行 rowkey → `update <db>.<t> set <ai列>=NULL,
  __ai_state_<col>='PENDING' where <命中谓词>` → 逐行 `insert into __all_ai_column_task ... on duplicate key update`。
### 21.1 已完成（本轮到语法+校验为止）
`RETRY` 非保留关键字 + `standalone_alter_action` 之外的 `alter_table_action` 规则 + 节点 `T_AI_COLUMN_RETRY = 4921`
+ `ObAlterTableResolver::resolve_ai_column_retry()` 校验（非 AI 列 / 混用其它 ALTER 动作 → 1235，其余 → 尚未实现的 `OB_NOT_SUPPORTED`）。
实测：`alter table t retry ai column result [where ...]` → `1235 retry ai column result not supported`；
非 AI 列 → `1235 retry ai column on a column that is not an AI column not supported`；
`..., add column x int` → `1235 retry ai column mixed with other alter table actions not supported`。

**两个实战坑（改语法必看）**：
1. **"混用"判定不能用 `alter_table_stmt->get_alter_table_action_count()`** —— 它是在遍历动作时**逐个 inc** 的，
   处理第一个动作时恒为 1。要用动作列表节点的总动作数 `node.num_child_`。
2. **parser 改了要确认真的重新生成**：`src/sql/parser/gen_parser.sh` 用 `_MD5`（`.y`+`.l`+`ob_item_type.h`）
   做缓存，生成物 `sql_parser_mysql_mode_{tab,lex}.c` 只在 WSL 树里、不纳入版本管理；
   **验证时必须让实例用新二进制重启**（`verify_task3.sh` 会起新实例），否则会对着旧进程测出 1064 而误判语法没生效。
## 22. 过近似把向量索引纳入 UPDATE 维护 → worker 回写撞 `-5024`（M5 已知限制）

**现象**：`ai_column_vector_index` 里「事务内失效后 ANN 仍返回旧向量」。为了修它，我们让
`ObDelUpdLogPlan::collect_related_local_index_ids()` 在「本语句有 AI 列失效/写入」时把该表所有
向量索引都塞进 `related_index_ids_`（过近似）。结果 worker 的回写语句直接失败：

```
AIDBG7 publish failed(ret=-5024,
  sql=update docs set emb = (AI_EMBED(...)), __ai_state_emb='READY', ... where `id` = 2)
```

`-5024 = OB_ERR_PRIMARY_KEY_DUPLICATE`：向量索引一旦参与这次 UPDATE 的 delta 维护，
「新值为 NULL」和「新值写入」两条路径上取到的 vid 键会和已有 delta 行冲突 → 回写失败 →
任务进终态被删 → `emb` 永久为 NULL（**数据丢失，比原问题更糟**）。已回退。

**根因（两层）**：
1. `collect_related_local_index_ids()` 的判定是**拿基表列 id 去索引表 schema 里查**
   （`index_schema->get_column_schema(base_cid)`）；AI 列在索引表里没有同 id 的列，
   所以向量索引天然不会进 `related_index_ids_`（这也是"普通 `update set v=NULL` 的对照实验
   能通过、AI 列不行"的原因——普通向量列的 base_cid 恰好能命中）；
2. 即使强行纳入，**AI 列写 NULL 与向量索引 delta 的 upsert 语义不兼容**（主键冲突）。

**教训**：
- 不要用"过近似纳入索引"的方式绕这类问题，先确认目标路径的 delta 写入语义；
- 判定某个隐藏/生成列是否影响索引时，不能只看 cid 命中，要按**列语义**映射（基表列 → 索引列）；
- 若继续攻这一处，方向应是「失效时对向量索引做显式 delete delta」而不是让普通 UPDATE 带上索引维护，
  但那会改动 M2 的失效语义，需单独授权与完整回归。

> **后续**：这条最终是**在第 23 条解决的**——不需要"纳入索引"这种过近似：把失效改成解析期
> 真实赋值 `<ai_col> = NULL` 后，AI 列本身就是"被赋值的基表列"，向量索引自然命中
> `collect_related_local_index_ids()`，vid 也随之正常推进，`-5024` 不再出现。过近似保持回退状态。

## 23. 运行期对**共享 ObExpr** 置 NULL 会污染 `old_row_` 镜像 → 向量索引 old/new 互换（M5 已解决）

**现象**：`update docs set content = ...` 在事务内把 `emb` 置空后，`select emb` 确实是 NULL，
但 `old_vector_removed_from_ann = 0`：ANN 仍能召回被更新的那一行。往向量索引 delta 里打日志：

```
delta row #1: is_old_row_=true,  vec_id=10002, vlen=0    <- old 本该是旧向量(16B)，却被置空
delta row #2: is_old_row_=false, vec_id=7,     vlen=16   <- new 本该是 NULL，却拿到旧向量
```

**根因**：M2 的失效实现是在**运行期**做的——`ObDMLService::process_update_row()` 里
`record_ai_column_tasks_for_update()` 对 `upd_ctdef.new_row_.at(idx)` 调
`locate_datum_for_write() + set_null() + evaluated_=true`。问题在于：**当本语句没有给这个 AI 列赋值时，
`old_row_.at(idx)` 与 `new_row_.at(idx)` 是同一个 `ObExpr` 对象**（update 的 old/new 行都指向
`column_exprs_` 里那一个引用）。`set_null()` 把 `old` 镜像一起置空，于是 delta 拿到的
old/new 互换了：旧向量被当成"新值 NULL"删掉、又用旧向量当"新值"插回 → ANN 里旧向量还在。

**修法（正解）**：把失效从"运行期改行镜像"改成**解析期物化成真实赋值** `ai_col = NULL`：

- 落点 `ObUpdateResolver::resolve()`，在 `resolve_assignments()` 之后、`generate_update_table_info()` /
  `resolve_additional_assignments()` 之前调用新增的 `add_ai_invalidation_assignments()`：
  rowkey 被赋值 → 该表所有 AI 列失效；否则用
  `build_ai_hidden_column_name(AI_RESULT_SPEC_COLUMN_PREFIX, ...)` 取 spec 隐藏列，`parse_ai_column_spec_text()`
  拿 `dep_columns_`，任一依赖列在 `assignments_` 里 → 该 AI 列失效；语句已显式写该 AI 列（worker 回写）则跳过；
- 用 `T_NULL` 常量 + `ObRawExprUtils::build_column_conv_expr()` 造 NULL 值表达式，
  `ObAssignment{column_expr_, null_expr, is_implicit_=true}` 交给 `add_assignment()`；
  `is_implicit_` 让 `ObDmlCgService::check_ai_column_not_written()` 跳过（不会误报 1235"禁止写 AI 列"）；
- 删掉 `ob_dml_service.cpp` 里那三行运行期 `set_null()`（保留 `record_ai_task_delete/upsert` 任务记录逻辑，
  M2 的 `dependency_update_task` 依赖它）；`ob_del_upd_log_plan.cpp` 的过近似在第 22 条已回退，此处不需要。

**为什么真实赋值能顺带把索引维护带对**（不用再碰 `collect_related_local_index_ids()`）：
1. `old/new` 变成两个 Expr（old 是列引用、new 是独立的 NULL conv），镜像不再互相污染；
2. 解析期 `check_update_vector_col_with_vector_index()` 现在能看到"本语句在写带向量索引的列"，
   `is_vec_hnsw_vid_column()` 分支自动补 vid 列赋值 → **vid 正常推进**，索引 delta 的 old/new vid 各归其位；
3. `collect_related_local_index_ids()` 的判定是 `index_schema->get_column_schema(base_cid) != nullptr`，
   AI 列现在作为**真实被赋值的基表列**出现，向量索引/其 delta buffer 自然命中，不再需要过近似。

**结果**：`old_vector_removed_from_ann / regenerated_vector_ready / regenerated_vector_visible_to_ann`
全部 = 1，且 explain 的 `est.rows` 也从 2 回到基线的 4。

**教训（可复用）**：
- DML 行里 old/new **共享同一个 `ObExpr`** 是常态（列未被赋值时二者就是 `column_exprs_` 的同一引用）。
  任何"只改新行"的运行期 hack 都必须先确认 old 是否共享——`locate_datum_for_write()` 改的是表达式的结果 buffer，
  不区分 old/new；
- 让引擎自己"看见"这次写（解析期生成真实赋值）比在运行期伪造行镜像更安全：类型推导、列 conv、
  NOT NULL/CHECK/视图检查、索引维护、回滚全部走原有路径；
- 顺带得到的判据：如果某处 hack 需要"临时把值改掉但又不想影响 old"，那基本就是设计错位，
  应该在**解析期**把意图表达进 AST/assignment，而不是在执行期改 datum。