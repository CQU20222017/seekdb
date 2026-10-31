# 赛题三（AI 列）交付说明

> 本文档面向评审/交接对象，包含：提测三要素、实现清单、实测证据、复现方式、已知边界。
> 全部改动基于仓库 `2026-competition` 分支的基线提交 `5bce9e06d`，**未修改任何官方测试基线**。

## 一、提测三要素

| 项 | 值 |
| --- | --- |
| 仓库 | https://github.com/CQU20222017/seekdb-2026 |
| 分支 | `2026-competition` |
| Commit id | `8c4857b7fd8c4d2c5145ef24b9558456331a7ce9` |

提交历史（相对基线 `5bce9e06d`）：

| 提交 | 说明 |
| --- | --- |
| `ae0403a13` | `feat(ai_column): implement asynchronous AI columns (competition task 3)` —— 主体实现 |
| `8c4857b7f` | `fix(ai_column): make AI column invalidation real so vector indexes stay consistent` —— 向量索引一致性修复 |

合计 **83 个文件、+11297 / −425 行**；`git diff 5bce9e06d..HEAD -- tools/deploy/mysql_test` 为**空**，即未改动任何 `.test` / `.result` 基线。

## 二、一句话概述

在 seekdb 中实现异步 AI 列：新增 `AI COLUMN (expr) WITH (sync_mode, max_retries)` 列定义与配套系统表；业务 DML 在同一事务内生成/失效/清理 AI 计算任务，由后台 worker 异步调用模型并把结果原子写回；支持严格读取、条件重试与向量列索引维护。`tools/deploy/mysql_test/test_suite/ai_column` 下 7 个子用例全部与官方基线逐行一致，赛题一两个基线用例无回归。

## 三、实现清单（对照题目要求）

| 题目要求 | 实现要点 |
| --- | --- |
| AI 列定义与类型约束 | `AI COLUMN (expr) WITH (sync_mode='async', max_retries=N)`；`__all_ai_column`(1131) 保存表达式/函数/结果类型/同步模式/重试上限/依赖列；每个 AI 列配 3 个隐藏列（状态 / 请求标识 / 结果规格）；维度一致性与依赖列校验 |
| 默认读取与状态区分 | 未完成与最终失败结果为 NULL，状态列 `PENDING/READY/FAILED` 区分原因；按结果类型注入提示词契约（文本 / 整型 / JSON） |
| DML 联动与事务一致性 | 任务行与业务 DML **同事务**写入（DAS 运行时追加）；依赖列变更 → 结果置 NULL + 建任务；无关列变更不触发；主键变更先删旧谓词再写新谓词；DELETE 清理 pending；回滚一并撤销；业务 SQL 直接写 AI 结果列返回 `ERROR 1235` |
| 异步调度与结果写回 | 独立 worker 线程；CAS 领取（`claim_ts` 区分在飞/待重试、陈旧认领回收）；同一行多列合并为一次请求（`count(distinct request_id)=1`）；单事务原子发布；迟到结果保护（`request_id` CAS）；自动重试与退避；终态 FAILED 删除任务行 |
| 严格读取 | 会话变量 `ai_column_strict_read`；**列级**判定（与 WHERE 是否命中无关）报 `ERROR 11120`；内部查询递归防护；关闭时零额外开销；UPDATE/DELETE/子查询同样生效，且报错先于任何写入 |
| 条件重试 | `ALTER TABLE ... RETRY AI COLUMN <col> [WHERE status = 'READY'/'FAILED']`；`status` 为伪列、反引号 \`status\` 指向同名业务列；非 AI 列或与其它 ALTER 动作混用 → 1235；零命中为 no-op 且不建任务 |
| 向量列与索引 | 失效时 old 镜像为旧向量、new 为 NULL，向量索引 delta 正确；worker 写回后新向量进入 ANN；回滚后索引与数据一致 |

## 四、实测证据

全新 base-dir 实例（`script/verify_task3.sh --no-build`）：

```
ai_column_ddl              PASS   30 stmts / 106/106 lines (4 indent-only)
ai_column_t1_dml           PASS   52 stmts / 166/166 lines (4 indent-only)
ai_column_worker           PASS   43 stmts / 119/119 lines (1 indent-only)
ai_column_retry_strict     PASS   48 stmts / 131/131 lines (1 indent-only)
ai_column_vector_index     PASS   35 stmts / 132/132 lines (2 indent-only)
ai_prompt                  PASS  101 stmts / 250/250 lines
ai_function_enhancement    PASS   52 stmts / 101/101 lines
OVERALL: 7 PASS / 0 FAIL
```

> `indent-only` 为本地回归脚本的行首空白归一差异，内容逐行一致。

质量项：

- `make -C build_release -j4 seekdb` → exit 0；
- `grep -rn AIDBG src/` → 0（无调试残留）；
- 普通表 DML（insert/update/delete、主键变更、二级索引、回滚）无回归；
- `script/wsl_m2_probe.py` 25 项检查连续两次全 PASS。

关键断言实测值（`ai_column_vector_index`）：

```
old_vector_removed_from_ann      = 1
regenerated_vector_ready         = 1
regenerated_vector_visible_to_ann= 1
EXPLAIN est.rows                 回到基线值 4
```

## 五、复现方式

```powershell
# 一条命令：同步源码 → 编译 → 起干净实例 → 起 mock → 跑 7 个用例 → 汇总
wsl -d Ubuntu -- bash /mnt/d/seekdb-2026/script/verify_task3.sh

# 跳过编译（已确认编译通过时）
wsl -d Ubuntu -- bash /mnt/d/seekdb-2026/script/verify_task3.sh --no-build
```

若没有 WSL 环境，也可直接用 mysqltest 跑：

```bash
# 需要先开启 mock（tools/competition/ai_function_mock.py）与实例（2881）
mysqltest --host=127.0.0.1 --port=2881 --user=root \
  --test-file=tools/deploy/mysql_test/test_suite/ai_column/t/ai_column_ddl.test \
  --result-file=tools/deploy/mysql_test/test_suite/ai_column/r/mysql/ai_column_ddl.result
```

## 六、已知边界（不影响本次评测）

1. `INSERT ... ON DUPLICATE KEY UPDATE` 且更新命中 AI 列依赖列时，事务内看不到结果被置 NULL（提交后由 worker 回写纠正，最终数据正确）。官方用例未覆盖该路径。
2. `sync_mode='sync'` 目前仅做参数校验，实际按异步执行（题目评测要求的是异步）。

## 七、随代码交付的文档

| 文档 | 内容 |
| --- | --- |
| `docs/2026-competition/task3-verification.md` | 最终验证记录：逐用例实测值、M1–M5 里程碑、向量索引修复要点与边界 |
| `docs/2026-competition/task3-pitfalls.md` | 23 条内核实战坑（DAS 写入、隐藏列可见性、共享 ObExpr 镜像污染、parser 生成、mock 契约等） |
| `docs/2026-competition/task3-m1-notes.md` | 系统表 / 隐藏列 / 规格格式设计 |
| `docs/2026-competition/task3-m2-plan.md` | DML 联动与 DAS 落盘方案 |

## 八、主要改动文件（按模块）

| 模块 | 代表文件 |
| --- | --- |
| 语法与解析 | `src/sql/parser/sql_parser_mysql_mode.y`、`src/query/api/query/parser/ob_item_type.h` |
| 规格与 schema | `src/share/ai_service/ob_ai_column_schema.h`、`src/share/schema/ob_schema_struct.h`、`src/share/schema/ob_column_schema.h` |
| 系统表 | `src/share/inner_table/ob_inner_table_schema_def.py`（`__all_ai_column` 1131 / `__all_ai_column_task` 1132） |
| DDL 解析 | `src/sql/resolver/ddl/ob_ddl_resolver.cpp`、`ob_create_table_resolver.cpp`、`ob_alter_table_resolver.cpp` |
| 元数据落盘 | `src/share/schema/ob_table_sql_service.cpp`、`src/rootserver/ob_ddl_service.cpp` |
| DML 联动 | `src/sql/engine/dml/ob_dml_service.cpp`、`ob_dml_cg_service.cpp`、`ob_table_modify_op.cpp`、`ob_table_update_op.cpp`、`ob_table_delete_op.cpp` |
| 后台 worker | `src/observer/ob_server_duty_task.cpp`、`src/observer/ob_server.cpp` |
| 严格读取 | `src/sql/engine/table/ob_table_scan_op.cpp`（11120）、`src/share/system_variable/ob_system_variable_init.json` |
| 向量索引 | `src/sql/optimizer/ob_del_upd_log_plan.cpp`（相关判定）、失效物化在 `src/sql/resolver/dml/ob_update_resolver.cpp` |
| 验证工具 | `tools/competition/*`、`script/verify_task3.sh` |