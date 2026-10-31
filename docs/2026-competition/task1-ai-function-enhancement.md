# 赛题一：AI 函数增强 —— 实现与协作说明

本文件是本仓库为「2026 OceanBase 数据库大赛 · 赛题一」建立的实现说明与分工约定，
所有参与者（含 AI 协作者）以此为准。

## 1. 题目范围

赛题要求在 seekdb SQL 引擎（C++）内补齐 AI 函数能力，不得在应用层绕过，不得对模型名、
输入或响应硬编码，不得破坏现有 AI 函数与普通 SQL。

权威验收物为测试套件：

- `tools/deploy/mysql_test/test_suite/ai_function/t/ai_prompt.test`
- `tools/deploy/mysql_test/test_suite/ai_function/t/ai_function_enhancement.test`
- `tools/deploy/mysql_test/test_suite/ai_function/t/ai_model_endpoint_ddl.test`
- `tools/deploy/mysql_test/test_suite/ai_function/t/ai_model_ddl.test`
- `tools/deploy/mysql_test/test_suite/ai_function/t/ai_model_lower_case_name_mysql{0,1}.test`

对应的期望输出在同目录 `r/mysql/*.result`。**实现目标：让实际输出与 `.result` 完全一致。**

## 2. 能力差距（当前代码 vs 期望）

### 2.1 AI_PROMPT

期望（见 `ai_prompt.result` / `ai_function_enhancement.result`）：

| 行为 | 现状 |
| --- | --- |
| 返回 `{"args": [...], "template": "..."}` 的 JSON 文本 | 已实现（`ObJsonObject` 按键有序输出） |
| JSON 参数（`json_object` / `json_array`）原样嵌入 args | `OB_NOT_SUPPORTED`，需支持 |
| 模板允许 TEXT / LONGTEXT / BLOB 等 LOB 类型 | 仅 `ob_is_string_tc`，需放宽到 `ob_is_string_type` 并按 LOB 读取 |
| 参数允许 TEXT / LONGTEXT 等 | 同上 |
| 参数为 NULL → 5083 | 部分覆盖，需统一 |
| 参数为整数 / 小数 / 日期等 → 5083 | 已覆盖 |
| 空模板 → 1210 | 已覆盖 |

### 2.2 AI_COMPLETE

| 行为 | 现状 |
| --- | --- |
| prompt 可为普通字符串或 `ai_prompt()` 返回的 JSON 对象 | 已有，但 args 含非字符串（JSON 对象/数组）时抛 `OB_NOT_SUPPORTED` |
| 第三参数 config 为 `null` | 当前把 null datum 当字符串解析，会报 JSON 解析错误 |
| config 支持 `temperature` / `max_tokens` | 已通过 `compact_json_object` 合并进 body |
| 响应非空校验 | `parse_output` 缺路径时 `OB_INVALID_DATA`，需保证不会返回空结果 |
| ollama provider（免 API Key、原生协议） | provider 派发缺失、客户端拒绝空 header、endpoint 校验不允许空 access_key |

### 2.3 AI_EMBED

| 行为 | 现状 |
| --- | --- |
| `ai_embed(model, text)` / `ai_embed(model, text, dimension)` | 已实现 |
| 参数为 NULL 的行 → 该行返回 NULL 且语句不报错 | 当前返回错误，需改为 `res.set_null()` |
| 内容为 LONGTEXT 等 LOB | 计算类型被强制成 varchar，需按 LOB 读取 |

### 2.4 AI_RERANK

| 行为 | 现状 |
| --- | --- |
| 第 4 参 doc_key 为 NULL → 按字符串数组处理 | 当前把 NULL datum 当 doc_key，报参数错误 |
| 字符串数组 / 对象数组 + 全局 index 映射 / 稳定排序 | 已实现 |
| 返回结果数量校验 | 需补 |

### 2.5 AI Model Endpoint

| 行为 | 现状 |
| --- | --- |
| `provider` 支持 `ollama`（大小写不敏感，落库保留原样） | `VALID_PROVIDERS` 无 OLLAMA |
| ollama 允许空 `access_key` | `check_valid` 拒绝空 access_key |
| `scope` 缺省为 ALL | 缺省未填充，直接报错 |
| `max_concurrency`：创建/修改/校验(1~1024 整数)/视图列 | 完全没有 |

## 3. 文件归属（避免并行冲突）

| 范围 | 负责人 | 文件 |
| --- | --- | --- |
| provider 派发、prompt object、HTTP 客户端、endpoint 结构体、ai_complete | Lead | `src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.{h,cpp}`、`ob_ai_func_client.{h,cpp}`、`ob_expr_ai_complete.cpp`、`src/share/ai_service/ob_ai_service_struct.{h,cpp}` |
| ai_prompt / ai_embed 表达式 | prompt-embed | `ob_expr_ai_prompt.{h,cpp}`、`ob_expr_ai_embed.{h,cpp}` |
| ai_rerank 表达式 | rerank | `ob_expr_ai_rerank.{h,cpp}` |
| endpoint DDL / 内表 / 视图 | endpoint-ddl | `src/share/inner_table/ob_inner_table_schema_def.py`、`src/observer/ai_service/ob_ai_service_proxy.cpp`、`src/observer/ai_service/ob_ai_service_executor.cpp` |
| 本地 Mock AI Service + 回归脚本 | harness | `tools/competition/ai_function_mock.py`、`tools/competition/run_ai_function_tests.py` |

## 4. 接口约定

### 4.1 prompt object

```json
{"args": ["str", {"k": 1}, [1, true]], "template": "{0}/{1}/{2}"}
```

- `template` 必须是 JSON 字符串且非空；`args` 必须是 JSON 数组；对象只能有这两个键。
- 参数元素允许任意 JSON 类型（字符串、对象、数组、数字、布尔）。
- 占位符 `{N}` 支持重复出现；`N` 越界 → 参数非法（5083）。
- 字符串参数按其原文替换；非字符串参数按其紧凑 JSON 文本替换。

### 4.2 provider 派发（`ObAIFuncUtils::get_*_provider`）

- `OPENAI` / `ALIYUN-OPENAI` / `DEEPSEEK` / `SILICONFLOW` / `HUNYUAN-OPENAI` → OpenAI 协议
- `ALIYUN-DASHSCOPE` → DashScope 协议
- `OLLAMA` → Ollama 原生协议（`/api/generate`、`/api/embed`，无 Authorization 头）
- provider 名称大小写不敏感匹配，落库/展示保留用户输入的大小写。

### 4.3 `ObAiModelEndpointInfo`

新增 `int64_t max_concurrency_`（0 表示未设置），提供
`int64_t get_max_concurrency() const` 与 `void set_max_concurrency(int64_t)`，
随 `deep_copy` / 序列化 / 代理读写一起传递。**结构体由 Lead 维护，其他成员只读使用。**

## 5. 验证方式

1. Windows 全量构建：`.\build.ps1 release --init -j 10`（Lead 负责）。
2. 启动 seekdb 实例，启动本地 Mock AI Service（监听 8000，OpenAI/Ollama 兼容）。
3. `python tools/competition/run_ai_function_tests.py` 逐条执行 `.test` 中的语句并与
   `r/mysql/*.result` 比较。

## 6. 约束

- 不修改 `.test` / `.result` 文件（它们是验收基线）。
- 不针对模型名、输入内容、响应内容做硬编码。
- 新 C++ 类不加 `Ob` 前缀；新头文件用 `#pragma once`。