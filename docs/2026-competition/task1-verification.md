# 赛题一：AI 函数增强 —— 实际验证记录

本文件记录在 **真实 seekdb 实例上** 跑官方 `ai_function` 基线的结果，用于判断第三方实现
（`D:\incoming-task1` 里的 `f7e48e6be`）的可用性。

## 1. 环境

| 项 | 值 |
| --- | --- |
| 构建 | WSL2 Ubuntu 24.04，`./build.sh release --make -j4`（RelWithDebInfo + Unity） |
| 依赖 | `deps/3rd`（el9 profile，29 个 RPM 解包，7.4G） |
| 产物 | `build_release/src/observer/seekdb`（1.33GB），`5.7.25-OceanBase seekdb-v1.4.0.0` |
| 实例 | `seekdb --nodaemon --port 2881 --base-dir ~/seekdb_run`（首次启动需全新 base-dir） |
| Mock | `tools/competition/ai_function_mock.py start 8000`（OpenAI + Ollama + rerank，768 维向量） |
| 回归 | `python3 tools/competition/run_ai_function_tests.py --host 127.0.0.1 --port 2881 --user root --password= --database test --suite ai_function` |

## 2. 结果（官方基线 `tools/deploy/mysql_test/test_suite/ai_function/r/mysql/`）

| 用例 | 语句数 | 结果 | 说明 |
| --- | --- | --- | --- |
| `ai_prompt` | 101 | **PASS**（250 行逐行一致） | 修复后才能通过，见第 3 节 |
| `ai_function_enhancement` | 52 | **PASS**（101 行一致） | 第三方实现原样通过 |
| `ai_model_ddl` | 42 | 前 37 条一致，尾部 4 条差异 | 差异全部位于 `connect (ai_user, ...)` 之后的**权限**断言（期望 42501），回归脚本是单连接、跳过 connect，属脚本限制而非实现缺陷 |
| `ai_model_endpoint_ddl` | 55 | 失败 | 真实缺陷：`DBA_OB_AI_MODEL_ENDPOINTS` 缺 `MAX_CONCURRENCY` 列（27 处差异由此连锁）；另有同样的权限段脚本限制 |

## 3. 为通过 `ai_prompt` 做的两处修复

第三方实现在官方基线下有 25 条语句不一致，分两类：

1. **错误消息文案**：它用 `LOG_USER_ERROR(OB_ERR_INVALID_TYPE_FOR_OP, "ai_prompt, template must be a string type", "(null)")`
   抛出定制文案，而基线要求标准文案 `ERROR 22000: Invalid data type for the operation`。
   → 去掉 `LOG_USER_ERROR`，只保留 `ret = OB_ERR_INVALID_TYPE_FOR_OP`。
2. **LOB 模板/参数被拒**：`is_text_lob_type()` 里用 `ob_is_blob()` 排除了 blob，
   导致 `ai_prompt(a)`（tinyblob）、`ai_prompt(c)`（mediumblob）报错，而基线要求成功。
   → 放开为所有 text-tc 类型（含 blob），并按 LOB 读取（`ObTextStringHelper::read_real_string_data`）。

修改文件：`src/sql/engine/expr/ob_expr_ai/ob_expr_ai_prompt.cpp`（`calc_result_typeN` 与 `eval_ai_prompt`）。

## 4. 已知缺口（相对官方基线）

- `MAX_CONCURRENCY`：内表 `__all_ai_model_endpoint`、视图 `DBA_OB_AI_MODEL_ENDPOINTS`、
  结构体解析与校验（1~1024 整数，非法值 11116）均未实现。
  这一块在赛题一暂停的 stash 里有完整实现（`git stash list` → `task1-ai-function-enhancement (paused)`），
  恢复后配合 `endpoint DDL` 用例即可闭环。
- 官方 `ai_model_ddl` / `ai_model_endpoint_ddl` 的权限段需要多连接执行（换普通用户），
  当前回归脚本仅支持单连接，需用真 `mysqltest` 或补多连接支持才能验证。

## 5. 如何复现

```bash
# WSL 内
cd ~/seekdb && make -C build_release -j4 seekdb          # 增量编译
bash /mnt/d/seekdb-2026/script/wsl_rebuild_and_restart.sh # 同步+编译+重启实例
cd ~/seekdb && python3 tools/competition/run_ai_function_tests.py \
    --host 127.0.0.1 --port 2881 --user root --password= --database test \
    --suite ai_function --case ai_prompt --case ai_function_enhancement
```