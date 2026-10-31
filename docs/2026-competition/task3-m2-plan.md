# 赛题三 M2（DML 联动与事务一致性）—— 实现路径（待 lead 确认）

> 验收用例：`tools/deploy/mysql_test/test_suite/ai_column/t/ai_column_t1_dml.test`（178 行）。
> 注意：该用例 40~57 / 82~93 / 109~118 行用 `let/while/--real_sleep` 轮询等
> `tags is not null`，**这一段依赖 M3 的 worker 调模型写回**。M2 只覆盖到
> 「任务行生成 + 结果列失效 + 事务内一致 + 删除清理」，用例整体通过需要 M3。

## 1. 关键结论（已核实的代码事实）

| 事实 | 位置 |
| --- | --- |
| UPDATE 的 `old_row_` / `new_row_` 是**整行**（`new_row` 先复制 `old_row`，再把被赋值的列替换掉） | `src/sql/code_generator/ob_dml_cg_service.cpp:306-328` |
| 主键/分区键更新会自动生成 **DAS delete + insert** 的 ctdef | 同上 `:375-379` |
| 每条赋值列都有 `projector_index_`，可用来在整行里定位该列的 datum | `ObUpdCtDef::assign_columns_` |
| 已有「逐行判断某列是否真的变了」的现成实现（`upd_rtdef.is_row_changed_`） | `src/sql/engine/dml/ob_dml_service.cpp:458-507` |
| 已有「逐行把 `new_row_` 某列 datum 改成 NULL」的现成写法 | `ob_dml_service.cpp:580-596`（`init_heap_table_pk_for_ins`：`locate_datum_for_write` + `set_null()` + `evaluated_=true`） |
| 已有「在 DML 事务内执行嵌套 SQL」的现成机制 | `ObTableModifyOp::execute_write()`（`ob_table_modify_op.cpp:990`），FK 级联/触发器就是这么做的 |

## 2. 任务表 `__all_ai_column_task`（table_id 1132）

行粒度：**一行 = (table, AI 列, 行键谓词)**，即同一行同一 AI 列重复失效时**覆盖**（upsert），
这样用例的 `count(*)=1`（单行更新一次）与 `count(*)=2`（两行批量插入）都能对上，
且天然去重（M3 worker 不会对同一行重复调模型）。

```
rowkey_columns:
  table_id            int
  column_id           int
  rowkey_predicate    varchar:1024     -- 精确文本，如 `id` = 2
normal_columns:
  eligible_ts         timestamp:6      -- 语句执行时刻（提交后即 <= now(6)）
  status              int   default 0  -- 0 PENDING / 1 READY / 2 FAILED（M3/M4 用）
  request_id          varchar:64       -- M3 写入请求标识
  retry_count         int   default 0
  gmt_create/gmt_modified（gm_columns）
```
`eligible_ts` 用 `timestamp:6`（生成器支持 `timestamp:<scale>` → `ObTimestampType`，scale 6），
所以 `eligible_ts <= now(6)` 是时间比较，能直接满足用例 32~37 行的断言。

## 3. 钩子位置（三层，各司其职）

### 3.1 判定「这条语句要不要失效 AI 列」——语句级（静态）

在 UPDATE 的编译期（`ObDmlCgService::generate_update_ctdef`，可从 `IndexDMLInfo`
拿到表 schema 与赋值列表）判断：

- 赋值列表命中某 AI 列**依赖列集合**或**主键/分区键列** → 该 AI 列需要失效；
- 否则该 AI 列不动（这就是用例 60~69 行「改 `title` 不失效、不建任务」）。

判定结果 + 依赖列的 projector index + AI 结果列/隐藏列的列 id 一起塞进
`ObUpdCtDef`/`ObInsCtDef`/`ObDelCtDef` 的新字段（纯内存计划结构，不涉及持久化）。

### 3.2 逐行失效（结果列写 NULL）——`ObDMLService::process_update_row`

在 `prepare_new_row` 之后、`ObDMLService::update_row(...)` 把行写进 DAS buffer 之前：

1. 用 `old_row_`/`new_row_` 逐列比较，判断依赖列是否**真的变了**（复用 `is_row_changed_`
   那套 `ObDatum::binary_equal`；lob 用 `check_lob_column_changed`）；
2. 变了（或主键变了）→ 对每个需要失效的 AI 列执行
   `expr->locate_datum_for_write(eval_ctx); datum.set_null(); eval_ctx 标记 evaluated_ = true;`
   —— 与 `init_heap_table_pk_for_ins` 完全同款；
3. 记下该行需要一条任务（rowkey 用**新**主键值）。

因为失效是「这条 UPDATE 自己写出去的那一行」的一部分，所以：
- **同一事务内可见**（用例 73~74 行、128~129 行）；
- **回滚自动还原**（用例 136~137 行）；
- 不需要第二条语句、也不会有二次写同一张表的问题。

### 3.3 任务行写入——`ObTableModifyOp::execute_write`（同一事务的嵌套 SQL）

在每行处理完成后（行级）或语句收尾时，用现成的 `execute_write()` 执行内嵌 DML：

- INSERT / 失效的 UPDATE：`insert into __all_ai_column_task(...) values(...) on duplicate key update eligible_ts=..., status=0, request_id='', retry_count=0`
  （`on duplicate key update` 实现「覆盖式去重」）
- DELETE：`delete from __all_ai_column_task where table_id=? and column_id=? and rowkey_predicate=?`

`execute_write` 走 `begin_nested_session(get_saved_session(), ...)`，**与外层语句共享事务**，
所以提交/回滚天然一致；且它写的是**另一张表**（任务表），不会与被写的主表冲突。

### 3.4 INSERT / DELETE

- INSERT：`ObDMLService::process_insert_row` 成功且未 skipped 时，按 `new_row_` 的前
  `rowkey_cnt_` 列生成行键谓词 → 每行 1 条任务（结果列本来就是 NULL，不需要额外动作）。
  多行 INSERT 每行一条 → 用例 151~156 行的 `count(*)=2`。
- DELETE：`ObDMLService::process_delete_row`（批量删除时逐行触发）按被删行的 rowkey
  删除对应待办任务 → 用例 157~163 行。

## 4. `rowkey_predicate` 文本格式

从行键 datum 直接渲染，**不经过 SQL 反解析**：
`<反引号>列名<反引号> = <字面量>`，多列用 ` and ` 连接。字面量规则：
- 整数族：十进制（`2`、`10`）；
- 字符串族：单引号包裹并按需转义单引号；
- 其它（datetime 等）：按 SQL 字面量输出。

用例 106 / 156 / 163 行要求精确 `` `id` = 2 `` / `` `id` = 10 ``，空格与反引号都按上面格式产出。

## 5. 依赖列集合（M1 需要补一小块）

M1 的 `ObAiColumnSpec` **没有**保存「表达式引用了哪些列」。M2 在
`ObDDLResolver::resolve_ai_column_spec` 里补：遍历 AI 表达式里的 `T_COLUMN_REF`
（M1 已有同样的遍历用于「AI 列不能依赖 AI 列」检查），把列名收集起来写进规格文本
（新增一行 `dep_columns=content,title`）。这是纯 M1 规格扩展，不影响已通过的 `ai_column_ddl`。

编译期把 `dep_columns` 的名字映射成列 id / projector index 后塞进 ctdef。

## 6. 回滚一致性与「不发模型请求」

- 任务行与外层 DML **同事务**：`rollback` 时任务行一起没了（用例 138~142 行）；
- M3 worker 是**独立会话**扫描任务表，只能看到已提交行 → 回滚事务里的
  `rollback-value` 永远不会被 worker 请求到（用例 144 行 `no-rollback-value`）；
- `eligible_ts` 取语句执行时刻，提交后立刻 `<= now(6)`，满足「提交即可执行」。

## 7. 改动面（预估）

| 文件 | 改动 |
| --- | --- |
| `src/share/inner_table/ob_inner_table_schema_def.py` | 新增 `__all_ai_column_task`(1132) |
| `src/sql/resolver/ddl/ob_ddl_resolver.cpp` + `ob_ai_column_schema.h` | 规格里补 `dep_columns` |
| `src/sql/engine/dml/ob_dml_ctdef.h`（`ObUpdCtDef/ObInsCtDef/ObDelCtDef`） | 新增 `ai_columns_` 计划期描述 |
| `src/sql/code_generator/ob_dml_cg_service.cpp` | 填充 `ai_columns_`（含依赖列 projector index、是否命中主键） |
| `src/sql/engine/dml/ob_dml_service.cpp` | 逐行失效 + 任务行生成/清理（`process_insert_row` / `process_update_row` / `process_delete_row`） |
| `src/share/schema/ob_table_sql_service.cpp` | `drop table` 时清理任务行 |

**不需要**动存储引擎，**不需要**动 `src/sql/engine/expr/ob_expr_ai/**`。
（若要新建 .cpp 文件就必须改 `src/sql/sql_source_inventory.bzl`；我倾向于把逻辑放进
现有的 `ob_dml_service.cpp`，避免动构建清单。）

## 8. 建议实施顺序（每步可独立编译验证）

1. 任务表定义 + DDL 清理（`__all_ai_column_task`）；
2. 规格补 `dep_columns`（改 `ai_column_ddl` 后要回归确认它仍通过）；
3. INSERT 路径：任务行生成（用 `wsl_m1_sql.py` 手工验证 `count(*)=1/2`、`eligible_ts <= now(6)`）；
4. UPDATE 路径：依赖列失效 + 主键更新（含 delete+insert ctdef 场景）；
5. DELETE 路径：清理待办任务；
6. 手工跑 `ai_column_t1_dml.test` 里**不依赖 worker** 的断言（28~38、71~80、95~107、126~142、146~164、166~172）。

## 9. DAS 写任务行：落地配方（已确认的 API 与字段清单）

### 9.1 为什么必须走 DAS
`ObTableModifyOp::execute_write()` 在 DML 里恒返回 `-4016` 且无日志；同一句 SQL 在普通会话/
触发器里都成功（详见 `task3-pitfalls.md` 第 6 条）。→ 旧 nested SQL 路径**必须整段删除**。

### 9.2 运行时建 loc（照抄 `ObForeignKeyChecker::init_foreign_key_checker()`，`ob_fk_checker.cpp:214-250`）
```cpp
das_ref_.set_expr_frame_info(expr_frame_info);       // 无 expr frame 可传 nullptr
das_ref_.set_mem_attr(mem_attr);                     // label 用 "SqlAiColTask"
DAS_CTX(das_ref_.get_exec_ctx()).extended_table_loc(loc_meta, table_loc_);   // ObDASCtx::extended_table_loc
DAS_CTX(das_ref_.get_exec_ctx()).extended_tablet_loc(*table_loc_, tablet_id, tablet_loc);
das_ref_.prepare_das_task(tablet_loc, insert_op);    // ObDASRef::prepare_das_task<ObDASInsertOp>
insert_op->init_task_info(ObDASWriteBuffer::DAS_ROW_DEFAULT_EXTEND_SIZE);
insert_op->set_das_ctdef(&ins_ctdef_);
insert_op->set_das_rtdef(&ins_rtdef_);
```
- `ObDASTableLocMeta` 可用 `ObDASCtx::build_table_loc_meta()` 造（`ob_das_context.h:127`），`ref_table_id` 填
  `OB_ALL_AI_COLUMN_TASK_TID`；
- tablet id 用 `ObDASCtx::get_das_tablet_mapper(ref_table_id, mapper)` 取（内表不分区，取唯一 tablet）；
- tx_desc 由 `ObDASRef::create_das_task()` 内部 `set_trans_desc(session->get_tx_desc())` 自动设为
  **语句自己的事务** → 天然同事务。

### 9.3 `ObDASInsCtDef` 最小集（`ob_das_dml_ctx_define.h:45-172`）
必须填的字段（其余留构造默认）：
```
table_id_ / index_tid_   = OB_ALL_AI_COLUMN_TASK_TID
rowkey_cnt_             = 3            // (table_id, column_id, rowkey_predicate)
spk_cnt_                = 0
schema_version_         = 任务表 schema_version
column_ids_             = 7 列，按任务表 schema 的 column_id 顺序：
                          table_id, column_id, rowkey_predicate,
                          eligible_ts, status, request_id, retry_count
column_types_           = 与上面对应的 ObObjMeta
column_accuracys_       = 与上面对应的 accuracy（varchar 长度、timestamp scale=6）
table_param_            = data_plane::ObDmlTablePlan（内表可用默认，必要时按 schema 填）
is_ignore_              = false
ObDASInsCtDef 额外：table_rowkey_cids_ / table_rowkey_types_（前 3 列）
ObDASInsRtDef.table_loc_ = 上面建好的任务表 loc
```
`DmlShadowRow`/`ObDatumRow` 的列顺序必须与 `column_ids_` **完全一致**；
`eligible_ts` 用 `ObObj(ObTimestampType, scale 6)` → `datum.from_obj(obj)`，不要手塞 int64。

### 9.4 行数据入口
`ObDASWriteBuffer::add_row(const DmlShadowRow &sr, DmlRow **stored_row)` +
`DmlShadowRow::init(alloc, col_types, strip_lob_locator=false)` +
`shadow_copy(const blocksstable::ObDatumRow &row)` → `insert_op->get_insert_buffer().add_row(sr)`。
一个语句只建 1 个 insert op（`has_das_op` 去重），多行全部 `add_row` 进同一 buffer。

### 9.5 三个必须注意的点
1. 进 datum 的字符串（`rowkey_predicate` / `request_id`）必须用**语句级 allocator**
   （`ctx_.get_allocator()`）；用行循环里的临时 buffer 会在 `shadow_copy` 之后悬空。
2. `request_id` 每次变更都要不同，且**粒度是「一行一次变更」**（M3 靠它做「最新请求胜出」，
   用例还断言同一行 2 个 AI 列 `count(distinct request_id) = 1`）：
   `<table_id>:<rowkey_predicate>:<语句时刻微秒>`，**不要**带 `column_id`（见 `task3-pitfalls.md` 第 11 条）。
3. 探针验收三条：① commit 前 `select count(*)` = 1；② commit 后仍在；③ rollback 后为 0 且 mock 无请求。

## 10. 当前实现状态（截止本轮）

| 项 | 状态 |
| --- | --- |
| `__all_ai_column_task`(1132) + drop table 清理 | ✅ 完成 |
| `dep_columns` 端到端 | ✅ 完成（实测 `dep_columns='content'`） |
| 计划期 AI 描述（`ObDMLBaseCtDef.ai_*` + `fill_ai_column_ctdef`） | ✅ 完成并进 SERIALIZE_MEMBER |
| 逐行行键谓词（`` `id` = 1 ``） | ✅ 生成正确 |
| 业务 SQL 直接写 AI 结果列 → `OB_NOT_SUPPORTED` | ✅ 完成并实测 |
| 任务行**落盘** | ✅ 完成：改为 **DAS 运行时追加**（见 §11），INSERT 单行/多行都通 |
| UPDATE 依赖列失效置 NULL + 重发工作项 | ✅ 完成（失效判定在 CG 语句级，见坑 8/10） |
| UPDATE 无关列不失效、不建任务 | ✅ 完成（实测任务行 request_id/eligible_ts 不变） |
| UPDATE 主键变更：旧谓词移除 + 新谓词写入 | ✅ 完成（最终只剩 1 行且指向新谓词） |
| DELETE 清理 pending 任务 | ✅ 完成（`id in (10,11)` 批量删后任务为 0） |
| 事务一致性（同事务、commit 留、rollback 消失） | ✅ 完成（探针三态全绿） |

### 已知限制（M3/M4 接手请注意）

1. **`INSERT ... ON DUPLICATE KEY UPDATE` / `REPLACE`**：两者走 `ObTableInsertUpOp` /
   `ObTableReplaceOp` 自己的行循环，**没有** `write_rows_post_proc` 钩子，目前不会 flush
   任务行（即这两类语句在 AI 表上不会生成/失效工作项）。t1 用例不覆盖，M3 后再补。
2. **失效粒度是“语句级”**：`update t set content = content`（赋同值）也会失效一次（保守方向，
   多算不少算）。原因见 `task3-pitfalls.md` 第 8 条（old_row/new_row 共享 ObExpr）。
3. **内部 DML 借用了 `is_main_table_in_fts_ddl_`** 来关闭“旧行必须仍存在”的防御校验，
   原因与核实过程见 `task3-pitfalls.md` 第 9 条。
4. **对内核公共代码的一处小扩展**：`src/sql/das/ob_das_delete_op.h` 增加了
   `ObDASWriteBuffer &get_write_buffer()`（insert op 本来就有 `get_insert_buffer()`），
   因为我们要用“不带表达式、直接 add_row”的方式填 delete buffer。
5. **`write_ai_task_rows()` 每行都 `ObDatumRow::init(ctx_.get_allocator(), …)`**：多行语句
   （如 1 万行批量 INSERT）会多出 1 万次语句级小分配，功能正确但有优化空间——把 `ObDatumRow`
   提到循环外 + 每行 `reuse()` 即可，留给后续清理（不影响任何用例）。

## 11. 最终落地方案（DAS 版，替换 §9 的草稿）

一个语句最多 **2 个**额外 DAS op，全部通过 `dml_rtctx_.das_ref_` 追加并共享语句事务：

1. `ObTableModifyOp::init_ai_task_writer()`：懒建 `ObDASInsCtDef/ObDASDelCtDef` +
   `ObDASInsRtDef/ObDASDelRtDef` + 任务表 `ObDASTableLoc/ObDASTabletLoc`。列清单在**运行时**
   从任务表 schema 反推（rowkey 先、其余按 column_id，identity `new_row_projector_`），
   `table_param_.build()` 一次到位；对象都是 op 成员、内存来自 `ctx_.get_allocator()`/DAS ctx，
   活到 `execute_all_task()` 之后。
2. `append_ai_task_insert_rows()` / `append_ai_task_delete_rows()`：`prepare_das_task` →
   `init_task_info` → `set_das_ctdef/rtdef` → 逐行 `blocksstable::ObDatumRow`
   → `DmlShadowRow::init/shadow_copy` → `add_row`；字符串用语句级 allocator。
3. `flush_ai_column_tasks()`：`execute_all_task()` + `close_all_task()`（DAS 内部先跑 delete
   聚合，所以“先删旧谓词再写新谓词”的顺序天然成立）；由 `ObTableInsertOp` /
   `ObTableUpdateOp` / `ObTableDeleteOp` 的 `write_rows_post_proc()` 在语句收尾时调用一次。
4. 字段取值：`eligible_ts = plan_ctx->get_cur_time()`（= 本语句 `now(6)`，保证
   `eligible_ts <= now(6)` 在事务内成立）；`request_id = <table_id>:<column_id>:<谓词>:<us>`，
   每次 flush 重新生成（M3 迟到保护）；`status=0`、`retry_count=0`、`gmt_create/gmt_modified`
   也写语句时刻。