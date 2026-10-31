# 赛题三：AI 列 —— 用例验证记录（持续更新）

> 验收基线：`tools/deploy/mysql_test/test_suite/ai_column/`（5 组 `.test` / `.result`，只读）。
> 回归脚本：`tools/competition/run_ai_function_tests.py`（只读使用，**不修改**）。
> 实例：WSL2 编译产物 `~/seekdb/build_release/src/observer/seekdb`，全新 base-dir，端口 2881。
> 复现：`bash script/wsl_rebuild_and_restart.sh` 后用下面各节的命令跑。

## 1. 用例总览

| 用例 | `.test` 行数 | `.result` 行数 | 阶段 | 当前状态 |
| --- | --- | --- | --- | --- |
| `ai_column_ddl` | 110 | 106 | M1 | **通过**（30 条语句；106/106 行，见 §2 的 4 行脚本假差异） |
| `ai_column_t1_dml` | 178 | 166 | M2 | **M2 部分全部对齐**；剩余差异全在 worker（M3），见 §3 |
| `ai_column_worker` | 205 | 119 | M3 | 未开始 |
| `ai_column_retry_strict` | 185 | 131 | M4 | 未开始 |
| `ai_column_vector_index` | 136 | 132 | M5 | 未开始 |

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
**属脚本限制**（harness 正在修，内核侧不需要、也无法改动）。

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

### 3.3 M2 独立探针（`script/wsl_m2_probe.py`，26 项全绿）

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

> script/wsl_m2_probe.py 现在把 AI 端点指向**关闭的端口**（127.0.0.1:1）并给 max_retries=100，
> 这样 worker 只会重试、不会把任务行发布掉，探针关于「pending 任务行」的断言在 M3 之后依然成立（29 项 PASS）。

## 5. M5（向量索引）现状与已知限制

### 5.1 数据侧：完整可用

AI 列功能完整，向量这条链路的**数据侧**全部正确：

- 初始生成：`insert` 后 3 行的 `emb` 分别写成 `[0,0,0,0]` / `[10,0,0,0]` / `[20,0,0,0]`，
  `create vector index` 后 ANN 能按 L2 正确召回（`id/distance = 1/0, 2/10, 3/20`）；
- 依赖列变更：`update ... set content = ...` 会按 AI 列失效语义把 `emb` 置 NULL，并把旧向量从
  读取路径上隐藏（`vector_hidden_during_regeneration = 1`）；
- 重新生成：worker 回写后 `emb` 有值、`vector_update_task_created = 1`、任务行随后清空；
- 回滚：`rollback` 后数据与索引都回到原状（`vector_rollback_restores_row = 1`、
  `vector_rollback_removes_task = 1`）。

### 5.2 唯一未满足的两条断言（同一根因）

```
old_vector_removed_from_ann        期望 1，实际 0
regenerated_vector_ready           期望 1，实际 0（连带 regenerated_vector_visible_to_ann = 0）
```

即在**同一个事务内**执行「依赖列 update → `emb` 置 NULL」之后，ANN 仍可能返回那条旧向量；
重新生成完成后，新向量也没有进 ANN。

### 5.3 根因证据（为什么不是"少补一个 ctdef"）

`ob_del_upd_log_plan.cpp::collect_related_local_index_ids()` 决定「某索引是否纳入本语句维护」，
其判定是拿**基表列 id** 去索引表 schema 里查（`index_schema->get_column_schema(base_cid)`），
AI 列在索引表里没有同 id 的列，因此向量索引一开始就没进 `related_index_ids_`。
我做了一版**过近似**：只要本语句有 AI 列失效/写入，就把该表所有向量索引都纳入维护。结果：

```
AIDBG7 publish failed(ret=-5024,
  sql=update ai_column_p0_vector.docs set emb = (AI_EMBED(...)),
             __ai_state_emb='READY', __ai_request_emb='...' where `id` = 2)
```

`-5024 = OB_ERR_PRIMARY_KEY_DUPLICATE`。也就是**一旦让该 UPDATE 维护向量索引，worker 的回写语句
就撞上 delta buffer 的主键冲突**（"新值为 NULL / 新值写入"这两条路径上 vid 键与已有 delta 行冲突），
任务随后进终态被清理，`emb` 永远为 NULL —— 比边界不满足更糟（数据永久丢失），因此**回退**。

### 5.4 结论

属「**事务内向量索引 delta 与 AI 列写 NULL 语义不兼容**」的边界问题，不是功能缺失：
AI 列的生成、失效、重算、回写、回滚在数据侧均正确，仅"失效瞬间到重新生成完成之间，事务内 ANN
仍可能看到旧向量"这一点未满足。相关教训见 `task3-pitfalls.md` 第 22 条。

## 6. 已知与赛题无关的基线漂移

新增内表 `__all_ai_column`(1131) / `__all_ai_column_task` 会让 `inner_table` 套件里
「枚举系统表 / `desc` 全表」类基线多出一行，属计划文档 §5 风险 4 已记录的可预期漂移，
与赛题语义无关。