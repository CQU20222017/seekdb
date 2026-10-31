# 赛题三 M1（DDL 语法与元数据）—— 代码侦察与实现计划

> 目标：让 `tools/deploy/mysql_test/test_suite/ai_column/t/ai_column_ddl.test` 的输出与
> `r/mysql/ai_column_ddl.result` 逐行一致。该用例不发模型请求，只覆盖「语法 + schema + 系统表 + 校验」。
> 本文记录 M1 的实现思路、改动面与关键决策，供 M2~M5 参考。

## 1. 验收用例逐条拆解

| 行 | 断言 | 需要的实现 |
| ---- | ---- | ---- |
| 28~37 | `create table ... tags json AI COLUMN (AI_COMPLETE(...)) WITH (sync_mode='async', max_retries=2)` 成功 | 解析器 + resolver |
| 39~43 | `__all_ai_column` 里该表有 2 行 | 系统表 + DDL 期间写行 |
| 45~50 | `(column_name, max_retries)` = `emb 3` / `tags 2` | `max_retries` 落元数据 |
| 52~61 | 3 个隐藏列 × 2 个 AI 列 = 6，且 `is_hidden = 1` | 每个 AI 列生成 3 个隐藏列 |
| 63~70 | 结果列本身 `is_hidden = 0` 共 2 行 | 结果列是普通可见列 |
| 72~78 | `tags.normalized_expr like '%Return exactly one valid JSON value%'` | JSON 结果类型的规范化表达式带结果契约 |
| 80~89 | `alter table ... add column ... AI COLUMN (...) WITH (...)` 后元数据 1 行 | `ALTER TABLE ADD COLUMN` 同样落地 |
| 91~95 | `vector(4) AI COLUMN (AI_EMBED(..., content, 3))` → **error 7600**，文案 `inconsistent dimension: expected 4 got 3` | 维度校验 |
| 97~102 | AI 列引用 AI 列 → **error 1235**，文案 `Not supported feature or function` | 链式依赖校验 |
| 104~106 | `drop table` 后无残留（后续用例/`drop database` 不报错） | 元数据随 DDL 事务删除 |

两个错误码对应关系（已核实）：
- `7600` = `OB_ERR_INVALID_VECTOR_DIM`（`src/oblib/lib/ob_errno.h:381`），sqlstate `22000`，
  `str_user_error = "inconsistent dimension: expected %u got %u"`
  → `LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, expected_dim, got_dim)`。
- `1235` = `OB_NOT_SUPPORTED`，sqlstate `0A000`。基线文案是**裸的** `Not supported feature or function`，
  即不能带 `LOG_USER_ERROR` 的参数（否则会变成 `<arg> not supported`），
  所以直接 `ret = OB_NOT_SUPPORTED;`。

## 2. 关键决策（以及为什么这样选）

### 2.1 AI 列的 schema 表示：结果列 + 3 个隐藏列

- 结果列（`tags` / `emb`）就是普通可写列，`is_hidden = 0`，类型由 DDL 声明（json / vector(4) / varchar…）。
- 额外 3 个隐藏列（`set_is_hidden(true)`），按名字由结果列名派生：
  - `__ai_state_<col>`：状态列（MVP 用 `varchar(32)`，M4 的 `status` 伪列读它）
  - `__ai_request_<col>`：请求标识列（`varchar(64)`）
  - `__ai_result_spec_<col>`：结果规格列（`varchar(1024)`，同时是 AI 规格的**载体**）
- 生成方式完全复用仓库既有隐藏列套路（参考 `ObFtsIndexBuilderUtil::generate_doc_id_column`
  / `ObVecIndexBuilderUtil::generate_vid_column`）：`set_column_id` / `set_data_type` /
  `set_data_length` / `set_collation_type` / `set_prev_column_id(UINT64_MAX)` /
  `set_next_column_id(UINT64_MAX)`，最后 `table_schema.add_column(...)`。

### 2.2 AI 规格如何从 resolver 传到 DDL 落盘

问题：`__all_ai_column` 的行必须在 **DDL 事务内**写（这样 `drop table` / 回滚才一致），
写行的位置是 `ObTableSqlService`，那里只拿得到 `ObTableSchema`；而 `max_retries`、
`normalized_expr` 这类信息在 resolver 里。

结论：**把 AI 规格编码进 result-spec 隐藏列的 `comment_`**。
理由：
1. `comment` 是 `__all_column` 的既有列（`longtext`），隐藏列的 comment 对用户完全不可见
   （不出现在 `desc` / `show create table` / `select *`），不会污染用户视图；
2. 不需要改 `__all_column` 的表结构（否则会大面积漂移 `inner_table` 基线），
   也不需要给 `ObTableSchema` 加不可持久化的成员；
3. 规格随表 schema 一起持久化，DDL 之后（含重启）都能从 `ObTableSchema` 反解出来。

规格文本用简单的 `key=value` 行格式（不用 JSON，避免额外 JSON 依赖）：
```
ai_expr=AI_COMPLETE('p0_ddl_complete', concat('Classify: ', content))
normalized_expr=AI_COMPLETE(...) /* Return exactly one valid JSON value and nothing else. */
result_type=json
max_retries=2
sync_mode=async
func=ai_complete
```
`\n` 在写入前转义成 `\\n`（表达式里几乎不会有换行，但仍做转义）。

> 后续 M2/M3 若需要 schema 层快速判定 AI 列，正解是给 `ObSchemaMgr` 加一个
> `ObAiColumnMgr`（照 `ObAiModelMgr` 的样子从 `__all_ai_column` 加载）。M1 不做，
> 因为 `ai_column_ddl.test` 只通过 SQL 读系统表。

### 2.3 隐藏列/结果列的识别：专有 column flag

在 `ob_schema_struct.h` 增加（`column_flags` 的低 32 位会落在 `__all_column.column_flags`）：
```cpp
#define AI_COLUMN_FLAG (INT64_C(1) << 7)            // 结果列：这是一个 AI 列
#define AI_STATE_COLUMN_FLAG (INT64_C(1) << 9)      // 状态隐藏列
#define AI_REQUEST_COLUMN_FLAG (INT64_C(1) << 10)   // 请求标识隐藏列
#define AI_RESULT_SPEC_COLUMN_FLAG (INT64_C(1) << 11) // 结果规格隐藏列
```
（bit 7/9/10/11 目前空闲，见 `ob_schema_struct.h` 现有分配。）

### 2.4 解析器：不新增关键字

`AI` 已经是**非保留关键字**（`sql_parser_mysql_mode.y:262` 的 token 声明、
`non_reserved_keywords_mysql_mode.c:32` 的词表、`unreserved_keyword_normal` 第 17500 行），
`COLUMN` 是保留关键字，`WITH` 也已存在。**所以词法/关键字表一行都不用改。**

`column_definition` 增加一个分支，复用**生成列的 7 child 布局**，把第 7 个 child
（生成列用来放 `opt_generated_keyname` 的槽位）换成新的 `T_AI_COLUMN_DEFINITION` 节点：

```
T_COLUMN_DEFINITION (7 children)
  [0] column_definition_ref
  [1] data_type
  [2] T_COLUMN_ATTRIBUTES
  [3] NULL                     ← 生成列放表达式；AI 列不用
  [4] NULL                     ← 生成列放 T_STORED_COLUMN；AI 列不用
  [5] opt_position_column
  [6] T_AI_COLUMN_DEFINITION   ← AI 列标记 + 载荷
        [0] expr（dup_expr_string 后的原始表达式文本）
        [1] T_LINK_NODE 列表，每个节点是 (relation_name, value)
```
复用 7 child 的好处：`ObCreateTableResolver` / `ObAlterTableResolver` 的
`GEN_COLUMN_DEFINITION_NUM_CHILD == node->num_child_` 分支（决定 `pos_node = children_[5]`）
不用动，各处对 `T_COLUMN_DEFINITION` 的遍历也完全不受影响。
判别 AI 列只需 `is_ai_column_node(node) = (7 == num_child_ && children_[6] 非空 &&
children_[6]->type_ == T_AI_COLUMN_DEFINITION)`。

`AI COLUMN ( expr ) WITH ( k=v, ... )` 的语法产生式（`WITH` 子句可选）：
```
column_definition_ref data_type AI COLUMN '(' expr ')' opt_ai_column_option_list
    opt_column_attribute_list opt_position_column
```

### 2.5 校验落点（resolver）

在 `ObDDLResolver::resolve_column_definition` 里，紧跟类型解析之后插入 AI 分支：
1. 表达式必须是 `AI_COMPLETE` / `AI_EMBED` / `AI_RERANK` 之类的 AI 函数调用（`T_FUN_SYS` 且函数名在
   允许集合内），否则 `OB_NOT_SUPPORTED`（裸文案）；
2. `vector(N)` 结果列 + `AI_EMBED(..., dim)`：若第 3 个参数是整型常量且 `dim != N`，
   `LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, N, dim)` → error 7600；
3. 表达式引用的任何列若本身是 AI 列（`column.is_ai_column()`），`ret = OB_NOT_SUPPORTED`（error 1235）；
4. 生成 `normalized_expr`：JSON 结果类型追加结果契约注释
   `/* Return exactly one valid JSON value and nothing else. */`（保持可被再次解析）；
5. 解析 `WITH (...)`：`sync_mode` ∈ {`async`, `sync`}、`max_retries` 为非负整数；未知键报错。

### 2.6 DDL 落盘（系统表 `__all_ai_column`）

- 定义在 `src/share/inner_table/ob_inner_table_schema_def.py`：
  `table_id = 1131`（1128/1129/1130 已被 `__all_ai_model*` 占用），
  rowkey `(table_id, column_id)`，列：
  `column_name, max_retries, sync_mode, state_column_id, request_column_id,
   result_spec_column_id, normalized_expr, ai_expr, ai_function, result_type`。
  表结构由 CMake configure 阶段的 `generate_inner_table_schema.py` 生成；
  `CMakeLists.txt` 已把 def.py 放进 `CMAKE_CONFIGURE_DEPENDS`，改完直接 `make` 会自动重跑。
- 写/删动作放在 `ObTableSqlService`（它已经有 `exec_insert` / `exec_delete` /
  `delete_from_all_column` 等现成工具）：
  - `add_ai_columns_for_table(sql_client, table)`：扫描 `table` 里 `is_ai_column()` 的列，
    反解 result-spec 隐藏列里的规格，逐个 `insert` 到 `__all_ai_column`；
  - `delete_from_all_ai_column(sql_client, table_id)`：按 table_id 删除。
- 调用点：
  - 建表：`ObTableSqlService::batch_create_table`（`create_table` / `truncate` 都走它）；
  - `alter table add column`：`ObDDLService::alter_table_column` 的列循环结束后；
  - `drop table`：`ObTableSqlService::drop_table`。
  三处都在同一个 DDL 事务里，`drop table`/回滚自动清理。

## 3. 改动文件清单

| 文件 | 改动 |
| --- | --- |
| `src/query/api/query/parser/ob_item_type.h` | 新增 `T_AI_COLUMN_DEFINITION = 3385` |
| `src/sql/parser/sql_parser_mysql_mode.y` | `column_definition` 新分支 + `opt_ai_column_option_list` / `ai_column_option(_list)` / `%type` |
| `src/share/schema/ob_schema_struct.h` | 4 个 `AI_*_COLUMN_FLAG` |
| `src/share/schema/ob_column_schema.h` | `is_ai_column()` / `is_ai_state_column()` / `is_ai_request_column()` / `is_ai_result_spec_column()` |
| `src/share/ai_service/ob_ai_column_schema.h`（新，header-only） | AI 规格结构体 + 规格文本编解码 + 隐藏列命名/查找 |
| `src/sql/resolver/ddl/ob_ddl_resolver.{h,cpp}` | `resolve_column_definition` 的 AI 分支、维度/依赖校验、`normalized_expr`、隐藏列生成 |
| `src/sql/resolver/ddl/ob_create_table_resolver.cpp` | 结果列 `add_column` 后追加 3 个隐藏列 |
| `src/sql/resolver/ddl/ob_alter_table_resolver.cpp` | `add column` 时追加 3 个隐藏列 |
| `src/share/inner_table/ob_inner_table_schema_def.py` | `__all_ai_column`（1131） |
| `src/share/schema/ob_table_sql_service.{h,cpp}` | `add_ai_columns_for_table` / `delete_from_all_ai_column` + 建表/drop 表挂钩 |
| `src/rootserver/ob_ddl_service.cpp` | `alter_table_column` 末尾挂钩 |

不动：`src/sql/engine/expr/ob_expr_ai/**`、`tools/deploy/mysql_test/**`、`tools/competition/**`。

## 4. 分步验证顺序

1. 语法（`.y`）→ 编译 → 手工 `create table` 能解析（先不接 schema）。
2. 结果列 + 隐藏列 + 系统表定义 → 编译 → `desc __all_ai_column` 可见。
3. 写元数据行 → `select * from __all_ai_column` 有行。
4. 校验（7600 / 1235）+ `alter table add column`。
5. 跑 `ai_column_ddl` 全用例，逐条收敛差异。

## 5. 实施结果（已落地）

`ai_column_ddl` 全 30 条语句执行完毕，输出与基线 **106 行 / 106 行**，逐行字节级比对只剩 4 行差异，
且这 4 行全部是**语句回显的缩进**（见 §6），没有任何结果差异：

- `ai_metadata_rows` = 1、`(emb,3)/(tags,2)`、`three_hidden_columns_per_ai_column` = 1、
  `result_columns_are_user_visible` = 1、`json_type_prompt_persisted` = 1、`alter_add_metadata` = 1 全部为 `1`；
- `bad_dimension` → `ERROR 22000: inconsistent dimension: expected 4 got 3`（7600）；
- `bad_dependency` → `ERROR 0A000: Not supported feature or function`（1235）；
- `drop table` 之后 `__all_ai_column` 无残留、`__all_column` 里也没有 `__ai_%` 隐藏列残留。

### 实现过程中踩到的两个坑（供给后续 M2~M5 参考）

1. **规格文本的字段分隔符被自己的转义函数吃掉了**：`build_ai_column_spec_text` 最初用
   `append_escaped()` 写 `"="` 和 `"\n"`，把换行转义成了字面 `\n`，导致解析端拿不到行边界。
   现在 `append_raw()` 负责分隔符、`append_escaped()` 只负责字段值。
2. **隐藏列 column_id 必须与 prev/next 链同序**：最初在「遍历每个列定义」时就地为 AI 列追加
   3 个隐藏列，隐藏列拿到了 `N+1..N+3`，而后续普通列用的是第一遍预分配的 `N+4` 之前的号，
   造成「链上 column_id 不单调」→ `CREATE TABLE` 在 tablet 创建阶段报 `-4016 OB_ERR_UNEXPECTED`。
   现象很有欺骗性：**AI 列是最后一列时正常，AI 列后面还有列就失败**。
   修法：CREATE TABLE 里改成「先把所有用户列加完，再统一追加隐藏列」（两阶段分配）；
   `ALTER TABLE ADD COLUMN` 天然是列表顺序分配 id，本来就同序，无需特殊处理。

## 6. 与基线剩余的差异（脚本限制，非实现缺陷）

`tools/competition/run_ai_function_tests.py` 在解析 `.test` 时会
`line.lstrip(" \t")` 归一化续行（脚本自带用例 `continuation line outside string stripped`），
而基线 `.result` 保留了 `.test` 里 4 处续行的 2 空格缩进：

```
ai_column_ddl.result:53   '  and c.is_hidden = 1;'        ← 脚本第 61 行
ai_column_ddl.result:63   '  and c.is_hidden = 0;'        ← 脚本第 70 行
ai_column_ddl.result:71   "  and a.column_name = 'tags'"  ← 脚本第 77 行
ai_column_ddl.result:72   "  and a.normalized_expr like '%Return exactly one valid JSON value%';"
```

这 4 行是**语句回显**，不是查询结果；内核侧无法影响，属于回归脚本的归一化行为。
另外脚本默认不采集 `SHOW WARNINGS`（`--warnings` 关闭），不加 `--warnings` 时
`drop table if exists` 的两处 `Warnings: Note 1051 ...` 也会缺失；加 `--warnings` 后完全一致
（服务端 `show warnings` 确实返回 `Note 1051 Unknown table ...`，行为正确）。