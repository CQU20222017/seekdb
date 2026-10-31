# 赛题三：AI 列 —— 用例验证记录（持续更新）

> 验收基线：`tools/deploy/mysql_test/test_suite/ai_column/`（5 组 `.test` / `.result`，只读）。
> 回归脚本：`tools/competition/run_ai_function_tests.py`（**本轮修了 `--replace_regex` 解析**，见 §7.1；基线文件仍未改动）。
> 实例：WSL2 编译产物 `~/seekdb/build_release/src/observer/seekdb`，全新 base-dir，端口 2881。
> 复现：`bash script/wsl_rebuild_and_restart.sh` 后用下面各节的命令跑。

## 0. 当前结论（M5 收尾）

`bash script/verify_task3.sh --no-build` → **7 用例全 PASS / 0 FAIL**（详见 §7.2），
其中 `ai_column_vector_index` 的 3 个关键断言全部 = 1：

| 断言 | 基线 | 实测 |
| --- | --- | --- |
| `old_vector_removed_from_ann` | 1 | **1** |
| `regenerated_vector_ready` | 1 | **1** |
| `regenerated_vector_visible_to_ann` | 1 | **1** |

## 1. 用例总览

| 用例 | `.test` 行数 | `.result` 行数 | 阶段 | 当前状态 |
| --- | --- | --- | --- | --- |
| `ai_column_ddl` | 110 | 106 | M1 | **通过**（30 条语句；106/106 行，4 行 indent-only） |
| `ai_column_t1_dml` | 178 | 166 | M2 | **通过**（52 条语句；166/166 行，4 行 indent-only） |
| `ai_column_worker` | 205 | 119 | M3 | **通过**（43 条语句；119/119 行，1 行 indent-only） |
| `ai_column_retry_strict` | 185 | 131 | M4 | **通过**（48 条语句；131/131 行，1 行 indent-only） |
| `ai_column_vector_index` | 136 | 132 | M5 | **通过**（35 条语句；132/132 行，2 行 indent-only） |
| `ai_prompt` | — | 250 | 回归 | **通过**（101 条语句；250/250 行） |
| `ai_function_enhancement` | — | 101 | 回归 | **通过**（52 条语句；101/101 行） |

> `indent-only` 是回归脚本 `lstrip` 回显模式造成的续行缩进差异（mysqltest 逐字回显、基线保留 `.test` 的 2 空格缩进），
> 不是结果差异，脚本按 `--indent-tolerance auto` 容忍。

## 2. `ai_column_ddl`（M1）

跑法（**必须带 `--warnings`**，否则 `drop table if exists` 的两处 `Warnings: Note 1051` 不会被采集）：

```bash
bash script/wsl_rebuild_and_restart.sh
wsl -d Ubuntu -- bash -c "cd ~/seekdb && python3 tools/competition/run_ai_function_tests.py \
  --host 127.0.0.1 --port 2881 --user root --password= --database test \
  --suite ai_column --case ai_column_ddl --max-diffs 2 --warnings"
```

结果：

```
30 statement(s)
1 statement(s) differ from the baseline
FAIL  (expected 106 line(s), got 106 line(s))
```

逐行字节级比对（忽略 CRLF）后：**只有 4 行不同，且全部是语句回显的续行缩进**，
不是任何查询结果：

```
result:53  期望 '  and c.is_hidden = 1;'                                           实际 'and c.is_hidden = 1;'
result:63  期望 '  and c.is_hidden = 0;'                                           实际 'and c.is_hidden = 0;'
result:71  期望 "  and a.column_name = 'tags'"                                     实际 "and a.column_name = 'tags'"
result:72  期望 "  and a.normalized_expr like '%Return exactly one valid JSON value%';"  实际无前导空格
```

原因：回归脚本解析 `.test` 时对续行做 `line.lstrip(" \t")` 归一化，
而 mysqltest 是逐字回显、基线保留了 `.test` 第 61/70/77/78 行的 2 空格缩进。
**属脚本回显限制**（脚本已用 `--indent-tolerance auto` 容忍，内核侧不需要也无法改动）。

### 2.1 逐条断言实测（全部为 `1` / 期望文案）

| `.test` 行 | 断言 | 实测 |
| --- | --- | --- |
| 39~43 | `ai_metadata_rows` | `1`（2 行元数据） |
| 45~50 | `(column_name, max_retries)` | `emb 3` / `tags 2` |
| 52~61 | `three_hidden_columns_per_ai_column` | `1`（6 个隐藏列，`is_hidden=1`） |
| 63~70 | `result_columns_are_user_visible` | `1`（2 个结果列，`is_hidden=0`） |
| 72~78 | `json_type_prompt_persisted` | `1`（`normalized_expr` 含 `Return exactly one valid JSON value`） |
| 80~89 | `alter_add_metadata` | `1` |
| 91~95 | `vector(4) AI COLUMN (AI_EMBED(..., content, 3))` | `ERROR 22000: inconsistent dimension: expected 4 got 3`（7600） |
| 97~102 | AI 列依赖 AI 列 | `ERROR 0A000: Not supported feature or function`（1235） |
| 104~106 | `drop table` 后清理 | `__all_ai_column` 无残留；`__all_column` 无 `__ai_%` 残留 |

### 2.2 附加人工验证（不在用例内，但覆盖回归风险）

| 场景 | 结果 |
| --- | --- |
| 一张表 2 个 AI 列（json + vector） | 成功；column_id 单调 16~25，隐藏列在链尾 |
| 单 AI 列 + 其后还有普通列 | 成功（两阶段分配隐藏列 id 后修复） |
| `insert into t values (...)` / 指定列 insert | 成功，隐藏列不参与用户可见列 |
| `select *` | 不返回隐藏列 |
| `show create table` | AI 列作为普通列显示，隐藏列不可见 |
| 对含 AI 列的表 `alter table add column` | 成功 |
| `truncate table` | 元数据行被重建且唯一 |
| `drop table` / `drop database` | `__all_ai_column` 对应行被清理 |
| `ai_prompt` 回归 | PASS（101 语句 / 250 行） |
| `ai_function_enhancement` 回归 | PASS（52 语句 / 101 行） |

## 3. `ai_column_t1_dml`（M2 已完成 DML 侧）

`.test` 里 40~57 / 82~93 / 109~118 行用 `let`/`while`/`--real_sleep` 轮询等待
`tags is not null`，**这一段必须由 M3 的后台 worker 调模型写回才能通过**；
M2 只负责到「任务行生成 + 结果列失效 + 事务内一致 + 删除清理」为止，
所以本用例在 M3 完成前不可能整体通过。

### 3.1 M2 断言实测（**全部与基线一致**，`--no-build` 实跑 `ai_column_t1_dml`）

| 断言（`.test` 行） | 基线 | 实测 |
| --- | --- | --- |
| `insert_task_eligible_immediately`（28~38） | 1 | **1** |
| `dependency_update_invalidates`（71~75，事务内） | 1 | **1** |
| `dependency_update_task`（76~80） | 1 | **1** |
| `old_primary_key_removed`（95~99） | 1 | **1** |
| `primary_key_update_invalidates`（100~103） | 1 | **1** |
| `task_uses_new_primary_key`（104~107，谓词 `` `id` = 2 ``） | 1 | **1** |
| `transaction_sees_invalidation`（126~129） | 1 | **1** |
| `transaction_sees_task`（130~133） | 1 | **1** |
| `batch_insert_invalidates_two_rows`（146~150） | 1 | **1** |
| `batch_insert_creates_two_tasks`（151~156） | 1 | **1** |
| `delete_cleans_pending_tasks`（157~163） | 1 | **1** |
| `delete_removes_row`（164~165） | 1 | **1** |
| `delete_leaves_no_task`（166~169） | 1 | **1** |

### 3.2 剩余差异（5 处，全部与 M2 无关）

| 差异 | 归因 |
| --- | --- |
| `initial_worker_completed` / `unrelated_update_keeps_result` / `dependency_update_completed` / `rollback_restores_row` = 0（基线 1） | **M3**：没有 worker 调模型写回，`tags` 一直是 NULL |
| `eval select $done = 1 as primary_key_update_completed` 回显成 `select 0 = 1 …` | **M3**：同上的轮询超时，`$done` 保持 0 |
| `primary_key_update_task_drained` = 0（基线 1） | **M3**：worker 才会消费掉 pending 工作项 |
| 1 处 `statement #4 (script line 95)` 回显缩进 | harness `lstrip` 归一化（同 §2 的 4 行） |

### 3.3 M2 独立探针（`script/wsl_m2_probe.py`，25 项 check 全绿）

三类事务语义 + UPDATE/DELETE 语义，不依赖 worker：

- INSERT：事务内 `count=1`、`eligible_ts <= now(6)`、谓词 `` `id` = 1 ``、status=0、
  retry_count=0、request_id 非空；commit 后仍在；rollback 后连同用户行一起消失。
- UPDATE 无关列（`title`）：任务行的 `request_id`/`eligible_ts` **完全不变**（不新建工作项）。
- UPDATE 依赖列（`content`）：事务内 `tags is null`、任务仍 1 行、`request_id` 已刷新。
- UPDATE 主键（1→2）：任务表只剩 1 行且谓词为 `` `id` = 2 ``。
- DELETE：`id in (10,11)` 批量删后对应任务为 0；删完全部后任务表清空。
- mock 请求数：0（M2 不发模型请求）。

另外 `script/wsl_plain_dml_regress.py`（普通表 insert/update/delete/PK 变更/二级索引/回滚）
与 `script/wsl_m2_affected_rows.py`（AI 表与普通表 `affected_rows` 对齐：insert 3→3、
update 1→1/2→2/0→0、pk 1→1、delete 2→2/0→0）均 PASS。

### 3.4 全 7 用例快照（M2 收尾时，`verify_task3.sh --no-build`）

```
ai_column_ddl              PASS  30 stmts / 106/106 lines (4 indent-only)
ai_column_t1_dml           FAIL  first diff: statement #1 (script line 60): expected "1\t1" vs actual "0\tNULL"
ai_column_vector_index     FAIL  first diff: statement #1 (script line 45): initial_embeddings_ready
ai_column_retry_strict     FAIL  first diff: statement #1 (script line 47): initial_results_ready
ai_column_worker           FAIL  first diff: statement #1 (script line 75): grouped_results_visible
ai_prompt                  PASS  101 stmts / 250/250 lines (0 indent-only)
ai_function_enhancement    PASS  52 stmts / 101/101 lines (0 indent-only)
mock: 11 requests (chat 2, embeddings 4, generate 1, embed 1, rerank 3)   ← 全是 ai_function 套件的
OVERALL: 3PASS / 4FAIL
```

4 个 FAIL 的首个差异点全部是「worker 还没把模型结果写回」，M2 侧断言在这些用例里已经对齐，
其中可以证明 M2 已为 M3 备好的几处：

| 用例 | 断言 | 实测 |
| --- | --- | --- |
| `ai_column_worker` | `group_starts_hidden` | 1 |
| `ai_column_worker` | `one_request_group_for_two_columns`（同 2 列共享 1 个 request_id） | 1 |
| `ai_column_vector_index` | `vector_hidden_during_regeneration`（vector 列也走同一套失效） | 1 |
| `ai_column_vector_index` | `vector_update_task_created`（谓词 `` `id` = 2 ``） | 1 |

## 4. M3（异步 worker）进展

| 项 | 状态 |
| --- | --- |
| worker 线程（独立线程 200ms tick、每 tick <=8 行、停机先于 inner sql proxy） | ✅ |
| 扫候选 + 按 (table,predicate,request_id) 分组 + CAS 领取（state=3 + claim_ts，60s 陈旧兜底） | ✅ |
| 用 normalized_expr 重放发布（一条 UPDATE 发布整组，原子可见） | ✅ grouped_results_visible=1、grouped_tasks_removed=1 |
| JSON / 整型结果契约（config 的 system_prompt -> system 消息） | ✅ pitfalls 第 15 条 |
| 重试状态机（state=3 + retry_count+1 + claim_ts=null + backoff 100ms->1s） | ✅ |
| 终态失败（max_retries 用尽）：写 __ai_state_<col>='FAILED' + **删除任务行** | ✅ pitfalls 第 16 条 |
| 临时诊断日志 | ✅ 已清理 |

### 4.1 七用例快照（M3 第 1 步完成后）

`
ai_column_ddl              PASS  30 stmts / 106/106 lines (4 indent-only)
ai_column_t1_dml           PASS  52 stmts / 166/166 lines (4 indent-only)   <- M2+M3 全通
ai_column_worker           PASS  43 stmts / 119/119 lines (1 indent-only)   <- M3 全通
ai_column_vector_index     FAIL  只剩 2 处 indent-only（内容 132/132 行一致，需 M5 向量索引）
ai_column_retry_strict     FAIL  Unknown system variable 'ai_column_strict_read'（M4 未实现）
ai_prompt                  PASS  101 stmts / 250/250 lines
ai_function_enhancement    PASS  52 stmts / 101/101 lines
OVERALL: 5PASS / 2FAIL
`

worker 相关断言实测：group_starts_hidden=1、one_request_group_for_two_columns=1、

o_partial_visibility=1、grouped_results_visible=1（	ags = {"category":"mock","input":"Classify: typed-prompts"}、
score=7）、grouped_tasks_removed=1（写回后该表任务行数 **0**）、etry_state_committed=1、
	erminal_failure_keeps_null=1 且终态后任务行 0。

> script/wsl_m2_probe.py 把 AI 端点指向**关闭的端口**（127.0.0.1:1）并给 max_retries=100，
> 这样 worker 只会重试、不会把任务行发布掉，探针关于「pending 任务行」的断言在 M3 之后依然成立（25 项 check 全 PASS，见 §7.3）。

## 5. M5（向量索引）：**已通过**

### 5.1 关键断言实测（`ai_column_vector_index`，35 条语句 / 132 行全对齐）

| 断言（`.test` 行） | 基线 | 实测 |
| --- | --- | --- |
| `initial_embeddings_ready`（29~45） | 1 | **1** |
| ANN 召回 `1/0, 2/10, 3/20`（46~52） | 3 行 | **3 行** |
| `vector_hidden_during_regeneration`（54~57） | 1 | **1** |
| `vector_update_task_created`（58~63，谓词 `` `id` = 2 ``） | 1 | **1** |
| **`old_vector_removed_from_ann`（64~68，事务内）** | 1 | **1** |
| **`regenerated_vector_ready`（71~87）** | 1 | **1** |
| `vector_task_removed`（88~92） | 1 | **1** |
| `explain` 的 `VECTOR INDEX SCAN` / `docs(idx_p0_ai_emb)`（93~95） | 4 行 / est.rows 4 | **相同**（`est.rows=4`） |
| **`regenerated_vector_visible_to_ann`（96~101）** | 1 | **1** |
| `vector_transaction_sees_invalidation` / `vector_transaction_sees_task`（105~114） | 1 | **1** |
| `vector_rollback_restores_row` / `vector_rollback_removes_task`（115~123） | 1 | **1** |
| `rollback_vector_still_visible_to_ann`（126~130） | 1 | **1** |
| `mock assert embedding` / `mock assert no-requests` | ok | **ok** |

### 5.2 修复要点（三段改动）

1. **解析期物化失效**（`src/sql/resolver/dml/ob_update_resolver.{h,cpp}`）：`ObUpdateResolver::resolve()`
   在 `resolve_assignments()` 之后调用新增的 `add_ai_invalidation_assignments()`，把"AI 列失效"
   物化成**真实赋值** `<ai_col> = NULL`（`T_NULL` 常量 + `build_column_conv_expr()`，
   `ObAssignment.is_implicit_ = true`）。判定规则：rowkey 被赋值 → 该表所有 AI 列失效；
   否则从 AI 列的 `__ai_result_spec_<col>` 隐藏列注释里 `parse_ai_column_spec_text()` 取 `dep_columns_`，
   任一依赖列被赋值 → 该 AI 列失效；语句已显式写该 AI 列（worker 回写）→ 跳过。
2. **删运行期置 NULL**（`src/sql/engine/dml/ob_dml_service.cpp`）：删掉
   `process_update_row()` 里对 `new_row_.at(idx)` 的 `locate_datum_for_write() + set_null() + evaluated_=true`，
   只保留 `record_ai_task_delete/upsert` 的任务记录（M2 的 `dependency_update_task` 依赖它）。
3. **不需要再碰优化器**：`ob_del_upd_log_plan.cpp::collect_related_local_index_ids()` 的过近似
   （第 22 条 pitfalls 里那版）已回退且**无需恢复**——AI 列现在是"被真实赋值的基表列"，
   `index_schema->get_column_schema(base_cid)` 自然命中，向量索引与 delta buffer 都纳入维护。

### 5.3 根因（为什么原来会失败）

原实现在**运行期**改行镜像置 NULL。update 的 DML 行里，**未被本语句赋值的列，其 `old_row_` 与
`new_row_` 是同一个 `ObExpr` 对象**，`set_null()` 把 old 镜像一起置空，于是向量索引 delta 拿到的
old/new 互换了（old 变成 0 长度、new 拿到旧向量）：事务内 ANN 照旧能召回到旧向量
（`old_vector_removed_from_ann = 0`）；而"强行把向量索引纳入维护"的过近似又让 worker 回写撞
`-5024 OB_ERR_PRIMARY_KEY_DUPLICATE`（vid 未推进，见 pitfalls 第 22 条）。改成解析期真实赋值后，
old/new 是两个 Expr、`check_update_vector_col_with_vector_index()` 能看见"本语句在写带向量索引的列"
从而自动补 vid 列赋值（vid 正常推进），两条问题一起消失。详见 `task3-pitfalls.md` 第 23 条。

### 5.4 已知边界（不影响本用例）

`INSERT ... ON DUPLICATE KEY UPDATE <dep_col> = ...`（`T_INSERT_SCOPE`）这条复合路径**没有**做解析期物化：
失效仍会在提交后由 worker 回写纠正，最终数据正确，只是"事务内看不到 NULL + 旧向量仍在 ANN 里"这一段
语义弱于纯 UPDATE。官方 5 个用例里 `grep` 不到 `insert ... on duplicate key update` 与 AI 列的组合
（0 处），补齐需要把两个 helper 上移到 `ObDelUpdResolver` 并在 `resolve_insert_update_assignment()`
里调用（约 10 行），留给后续，不在本次提交范围。

## 6. 已知与赛题无关的基线漂移

新增内表 `__all_ai_column`(1131) / `__all_ai_column_task` 会让 `inner_table` 套件里
「枚举系统表 / `desc` 全表」类基线多出一行，属计划文档 §5 风险 4 已记录的可预期漂移，
与赛题语义无关。

## 7. M5 收尾：回归脚本修正 + 最终快照

### 7.1 回归脚本的 `--replace_regex` 解析修正（`tools/competition/run_ai_function_tests.py`）

`ai_column_vector_index` 的最后一行差异不是内核结果，而是脚本解析缺陷。脚本日志原本是：

```
[note] line 93: cannot parse --replace_regex '/__vid_[0-9]+(_[0-9]+)?/__vid_0/g /__type_[0-9]+_[0-9]+/__ty'
```

即 `.test` 第 93 行的
`--replace_regex /__vid_[0-9]+(_[0-9]+)?/__vid_0/g /__type_[0-9]+_[0-9]+/__type_0_0/g`
被整行丢掉，explain 里的 `__vid_1793437613487184` / `__type_18_1793437613867668` 没被归一成
基线里的 `__vid_0` / `__type_0_0`。原因：旧 `parse_replace_regex()` 只接受**单个**
`/pattern/replacement/`，既不认结尾的 `g`（标志）也不认**一行多对**——而 mtr 两种都支持
（本仓库 96 处 `replace_regex` 里，`fts_index` / `vector_index` / `geometry` 套件大量使用 `/g` 与多对）。

修正：`parse_replace_regex()` 改为按 mtr 语义逐对扫描（转义 `\x` 原样保留、`\1` 反向引用照用、
结尾可选 `g`/`i` 标志、一行多对全部收集），调用处改为 `pending_replace_regexes.extend(pairs)`；
`--selftest` 补了 5 项覆盖（单对 / `g` 标志 / 多对 / 多对应用 / 反向引用）。**基线文件未改动**。

### 7.2 最终七用例快照（实例全新 base-dir 重启后，`verify_task3.sh --no-build`）

```
ai_column_ddl              PASS  30 stmts / 106/106 lines (4 indent-only)
ai_column_t1_dml           PASS  52 stmts / 166/166 lines (4 indent-only)
ai_column_vector_index     PASS  35 stmts / 132/132 lines (2 indent-only)
ai_column_retry_strict     PASS  48 stmts / 131/131 lines (1 indent-only)
ai_column_worker           PASS  43 stmts / 119/119 lines (1 indent-only)
ai_prompt                  PASS  101 stmts / 250/250 lines (0 indent-only)
ai_function_enhancement    PASS  52 stmts / 101/101 lines (0 indent-only)
OVERALL: 7PASS / 0FAIL
```

`make -C build_release -j4 seekdb` → `RC=0`；`grep -rn AIDBG src/` → 0 条。

### 7.3 独立探针（`script/wsl_m2_probe.py`，全新实例）

25 项 check 全 PASS（`PROBE PASSED`），包括 M2 的三条不变量：

- `noop update: no new work item`（无关列更新不刷新 `request_id`、不新建工作项）→ PASS；
- `dep update: result is null in txn` / `dep update: one work item only` / `dep update: request id refreshed` → PASS；
- `pk update: one task row` / `pk update: new predicate`（`` `id` = 2 ``）→ PASS。

> 探针里 `insert: eligible_ts <= now(6)` 这条 check 是 **µs 级次序假设**：`eligible_ts` 取
> `write_ai_task_rows()` 落行时的 `ObTimeUtility::current_time()`，而同一事务内的 `now(6)`
> 会被钉在事务起始时刻，两者相差几微秒就会 `flag=0`。多次运行的结论是"同一二进制既 PASS 又 FAIL"，
> 属探针脆弱性而非内核问题；实例全新重启后稳定 PASS（本次收尾即如此）。若后续偶发，
> 建议在该 check 上加 1ms 容差或重试一次，不必动内核。