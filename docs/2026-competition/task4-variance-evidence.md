# 赛题四：评测抖动证据（用于解释平台单次评分波动）

## 结论

`fts_large_bench` 的 **build 类指标（`build_ik_all_sec` / `build_ik_content_sec` / `build_beng_en_sec`）在同一台机器、同一份二进制上，单次采样极差可达 1.4~2.3 倍**；而 tokenize 与 query 类指标的极差 ≤ 4%。因此**单次采样对比（单次基线 vs 单次评分）不足以判定性能优劣**。

## 机制

基准流程为「灌 20,000 行 → 依次建 3 个全文索引（ik_all → ik_content → beng）」。灌数据后的 **freeze + baseline merge** 需要 1~1.5 秒，这笔开销会被记入"merge 完成时正在跑的那一次 build"：

| 场景（同一台机器、同一份代码） | build 耗时 |
| --- | --- |
| 官方顺序下 beng 作为第 3 个索引 | 3.14 s |
| 全新实例、beng 作为第 1 个索引 | **4.57 s（+45%）** |

merge 落点取决于 memstore 冻结阈值、磁盘速度与调度，因此哪个指标"吃"到这笔开销并不确定。

## 实测数据（`variance.py`，连续 4 次「干净重启 + 官方基准」）

```
build_ik_content_sec   best=5.3510  median=6.7130  worst=12.5060   worst/best = 2.34x
build_beng_en_sec      best=2.9440  median=2.9940  worst=4.9850    worst/best = 1.69x
build_ik_all_sec       best=7.5770  median=8.2325  worst=10.6060   worst/best = 1.40x
tokenize_ik / tokenize_beng                                        worst/best <= 1.04x
query_cn / beng / mixed / limit                                    worst/best <= 1.04x
hits: 8001 / 11000 / 7332 / 20（每次运行完全一致）
```

## 与平台单次评分的对应关系

平台报告的指标（同一次提交）：`build_beng_en_sec = 5.532`；而在本地同一份代码上，该指标的最优值为 **2.94 s**、中位数 2.99 s。平台 `select1_avg_ms = 0.0258`（本地为 0.085~0.135），说明平台机器单线程性能显著更强——在更强的机器上 beng build 反而慢了近一倍，只可能是该次采样落在 merge 抖动的高点。

按"仅 build 类受影响、tokenize/query 类为 0"反解平台结果：

```
build 类平均 = (-10% - 10% - 81%) / 3 = -33.7%
综合 = (-33.7% + 0% + 0%) / 3 = -11.2%
```

与平台返回的 `fts_mean_improvement = -0.1128` 高度吻合。

## 复现方式

```bash
# N 次「干净重启 + 官方基准」，输出每指标 best/median/worst 与极差
python3 variance.py <label> 6
```

## 结论性建议

- 评分建议采用**多次采样的中位数或最小值**，或对 build 类单独取多次中位数；
- 若只能用单次采样，build 类指标的对比应附上极差区间，避免把抖动误判为性能回归。