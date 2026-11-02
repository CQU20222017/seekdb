# 赛题四（全文索引性能优化）交付说明

## 一、结论

在**保证分词与检索语义完全不变**的前提下，对全文索引的分词热路径与词表查找路径完成 5 项优化，本地 4 次干净重启实测：

| 类别 | 指标 | 优化前 | 优化后（4 次均值） | 提升 |
| --- | --- | --- | --- | ---: |
| 构建 | `build_ik_all_sec` | 8.19 | 7.81 | −4.7% |
| 构建 | `build_ik_content_sec` | 6.35 | 5.35 | **−15.8%** |
| 构建 | `build_beng_en_sec` | 3.29 | 2.99 | **−9.1%** |
| 分词 | `tokenize_ik_avg_ms` | 0.2247 | 0.1591 | **−29.2%**（扣语句地板 −49.3%） |
| 分词 | `tokenize_beng_avg_ms` | 0.1527 | 0.1120 | **−26.7%** |
| 查询 | `query_cn_avg_ms` | 1.66 | 1.424 | **−14.2%** |
| 查询 | `query_beng_avg_ms` | 2.44 | 2.112 | **−13.4%** |
| 查询 | `query_mixed_avg_ms` | 1.87 | 1.643 | **−12.2%** |
| 查询 | `query_limit_avg_ms` | 2.29 | 1.903 | **−16.9%** |

三类等权粗算：构建 ≈ 6.7%、分词 ≈ 28.0%、查询 ≈ 14.2% → 综合 ≈ 16.3%。

> 注意：本地机器与 CI 环境差异极大（CI 基线的 `build_ik_all` 为 35.28s，本地为 8.19s），因此上表是**同机对比**的结论；CI 上的绝对分数以官方固定基线为准。

## 二、5 项优化

| # | 位置 | 问题 | 改法 |
| --- | --- | --- | --- |
| ① | `src/storage/fts/ob_fts_stop_word.cpp` | 每次停用词检查都构造 `ObArenaAllocator`，且在 collation 相同时仍走 `charset_convert` | 相同 collation 直接短路；需要转换时复用 thread_local arena |
| ② | `src/sql/engine/expr/ob_expr_tokenize.cpp` | 每行都对 parser properties 做 JSON parse + rebuild + 序列化，`ObFTParseHelper::init` 再 parse 一次 | 线程内 memo 缓存 `(parser name, user props) → (canonical name, canonical props)` |
| ③ | `src/storage/fts/dict/ob_ft_range_dict.{h,cpp}` | 首字符定位用 collation `strcmp` 线性扫描 | `utf8mb4_bin` 下用 `memcmp`；区间有序时二分，**无序自动退回线性**（行为等价） |
| ④ | `src/storage/fts/ik/ob_ik_char_util.h` | 每字符 `decode_unicode` 6 次（is_alpha / arabic / chinese / other_cjk / surrogate ×2） | 解码一次，复用同一批谓词 |
| ⑤ | `src/sql/engine/expr/ob_expr_tokenize.cpp` | `tokenize('abc','ik',NULL)` 触发 observer SIGSEGV（NULL properties 未判空） | 判空，NULL 视为"无 properties" |

## 三、正确性验证（三项全部通过）

1. **TOKENIZE 逐字节一致**：固定基准文本（中文 243 字符、英文 151 字符）优化前后 token 数组完全相同，连续 200 次求值稳定，`output=all` 与默认模式切换不串缓存；
2. **`script/fts_regress_replay.py` PASS（0 failure）**：按官方 `.result` 逐条重放 24 条断言 —— 11 条 `MATCH` 精确命中行、15 行 `tokenize()` 输出、14 条错误文案（无效 parser / 非法 output / 非法 JSON / ngram 越界 / ik_mode 拒绝等）；
3. **命中数锚点不变**：`query_cn_hits=8001`、`query_beng_hits=11000`、`query_mixed_hits=7332`、`query_limit_hits=20`，4 次运行全部一致。

## 四、被否掉的方向（含证据，供后续参考）

| 候选 | 结论 |
| --- | --- |
| `DEFAULT_KEY_PER_RANGE` 50000 → 12500（把一次性 DAT 构建切给更多并行线程） | **回退**。20k 行 `build_ik_all` 看似 −8%，但 10k 行对照实验 `4.664 → 4.716`（+1.1%），且不受影响的 `build_beng_en` 也漂了 +3.4% → 判定为噪声。根因：词表 DAT 构建的 **Phase 1（灌 trie）是单线程串行**，调小 range 只缩短并行的 Phase 2，收益被 Phase 1 吃掉 |
| 停用词检查的 `ObArenaAllocator` 是"8KB malloc" | 前提错误：`ObCharset::charset_convert` 在 collation 相同时直接短路，实际仅空 arena 构造开销（~215ns/token） |
| A′（词表单例复用）/ C′（produce 微优化） | 净耗时确有小幅收益，但**指标级影响仅 −2~−4%，落在 ±5~8% 测量噪声内**，不值得引入缓存失效/线程安全复杂度 |
| D′（build 每行 DAS 插入路径） | 单行 0.2ms 是 build 大头，但属共享存储/DAS 路径，风险远大于收益 |

## 五、复现方式

```bash
# 干净重启协议（每次测量前必做，否则同一实例连续跑会慢 30~70%）
pkill -x seekdb && rm -rf ~/seekdb_run
bash /mnt/d/seekdb-2026/script/wsl_start_instance.sh

# 官方基准（跑完自动评分）
bash /mnt/d/seekdb-2026/script/wsl_fts_bench.sh <label> 20000 500 3000 200 3 30 0

# 正确性回归（约 10s）
python3 script/fts_regress_replay.py
```

## 六、剩余空间（如继续投入）

| 方向 | 指标级预期 | 风险 |
| --- | --- | --- |
| `ik_produce` 深度重构（当前 43µs/次，占 IK 分词 47%） | tokenize 类 −26%（综合 +4~6 分） | **高**：仲裁/切分语义核心，一旦切词偏差即正确性归零 |
| 倒排扫描每命中 125ns 的取列/解码 | 查询类再 −15%（综合 +5 分） | **高**：DAS/扫描共享路径 |
| `LIMIT` 子查询反而比全量更慢（1.85/2.14ms vs 1.28ms） | `query_limit` 单指标可能大幅改善 | **高**：优化器/plan 层 |