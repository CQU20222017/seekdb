# 实验报告：hipVS 加速 seekdb HNSW 索引构建

> 日期：2026-11-11 ｜ 参赛材料
> **报告性质**：本报告包含两类数据——(a) **本机实测**（Python 原型与 C++ 实现的 CPU 路径，均真实运行）；(b) **GPU 折算模型**（显式标注假设，非实测；本环境无 AMD GPU/ROCm/hipVS）。两类数字不会混淆。

## 1. 硬件与软件配置

| 项 | 值 | 来源 |
| --- | --- | --- |
| CPU | x86_64（14 逻辑核可用） | WSL2 `nproc` |
| 内存 | 9 GB（WSL2 分配） | `free -g` |
| 操作系统 | Windows 11 + WSL2 Ubuntu 24.04 | `uname -a` |
| Python | CPython 3.12.3 | `python3 -V` |
| numpy | 本环境**未安装**（基准自动走纯 stdlib 路径） | 基准输出 `numpy=no` |
| C++ 编译器 | g++ 13.3 / clang++ 17 | `g++ --version` |
| GPU | **本环境无 AMD GPU / ROCm / hipVS** | 任务环境说明 + 本机检查 |

## 2. 实验方法

### 2.1 数据

- 随机生成 `n×d` 标准正态向量并 L2 归一化（`seed=42`），另有 `queries` 条查询向量（`seed=43`）；
- 精确真值：暴力 L2 top-K；
- 数据生成耗时单独记录，**不计入构建耗时**（题面要求）。

### 2.2 构建流程（与 seekdb 真实链路的对应关系）

seekdb 的 HNSW 由外部预编译库 vsag 实现（`src/oblib/lib/vector/ob_vsag_adaptor.cpp` → `vsag::Index::Build`），构建一次插入的成本分两段：

1. **候选邻居搜索**（距离计算密集，GPU 加速对象）：对每个点求候选最近邻表；
2. **图装配**（控制流密集，留在 CPU）：层分配、正向边、反向边写入/repair、按距离裁剪到 M。

本报告用两个独立实现互相印证：

- **Python 原型**（`amd/bench/bench_build.py`）：纯 CPU 实测两阶段耗时，用于验证计时口径与 Recall 方法；
- **C++ 实现**（`amd/hipvs-hnsw/`）：完整的分层 HNSW 构建器 + 距离后端抽象（CPU 参考后端实测；HIP 后端给出 hipVS 接入实现）。

### 2.3 计时口径

- 起点：候选搜索开始（数据已就绪）；终点：图装配完成（索引可查询）；
- 包含：候选计算、topk、装配、反向边 repair；不含：数据生成、查询；
- 每组 ≥3 次，取**中位数**，并给出 min/max/stdev。

### 2.4 GPU 路径的表示（模型，非实测）

```
GPU 端到端 = 距离计算 ÷ speedup_factor + CPU 装配 + (上传+回传) ÷ 带宽 + 固定开销
```

默认 `speedup_factor=8.0`、带宽 10 GB/s、固定开销 5 ms；假设全部写入 `bench/result.json` 与 `bench/report.md`。

## 3. 实测结果

### 3.1 单元测试（真实）

命令：`python3 -m unittest discover -s amd/tests -v`

```
Ran 9 tests in 0.017s
OK
```

覆盖：统计中位数、图装配不变量（无孤立点、度上限）、Recall 明显优于随机、GPU 耗时模型自洽、三态路径判定（CPU_ONLY/FALLBACK 不计入加速）。

### 3.2 Python 原型基准（真实计时，本机）

命令：`python3 amd/bench/bench_build.py --n 800 --d 64 --queries 50 --runs 3`

| 轮次 | 候选搜索 (ms) | 图装配 (ms) | 端到端 (ms) |
| --- | ---: | ---: | ---: |
| 1 | 1315.7 | 8.7 | 1324.4 |
| 2 | 1306.1 | 6.7 | 1312.7 |
| 3 | 1256.4 | 6.6 | 1263.0 |
| **中位数** | **1306.1** | **6.7** | **1312.7** |

- **Recall@10 = 0.9740**（装配后的图 best-first 检索 vs 精确真值）；
- 图结构：800 节点，无孤立点；
- 结构结论：候选搜索（距离计算）占端到端 **~99.5%** —— 在该规模下 GPU 卸载的收益空间非常大（与 C++ 实现在更大规模下的结论方向一致）。

### 3.3 C++ 实现基准（真实计时，本机，`amd/hipvs-hnsw`）

命令：`cd amd/hipvs-hnsw && make bench`（或 `bash scripts/run_bench.sh`）

| 规模 | 构建耗时 | 距离对总数 | 吞吐 | recall@10 |
| --- | ---: | ---: | ---: | --- |
| 20,000 × 128d | 7.30 s | 77.6 M | 10.6 M 对/s | 0.585 / 0.735 / 0.826 / **0.908**（ef=32/64/128/256） |
| 50,000 × 128d | 23.68 s | 213.0 M | 8.9 M 对/s | 0.609（ef=64） |

- 构建期距离计算占比 > 95%（`backend_calls` 与 `distance_evaluations` 直接统计）；
- 图自检：第 0 层平均度 32（=2M）、孤立点 0；
- 反向边采用**按距离剪枝**而非尾部截断——这是质量关键：尾部截断会让早期节点形成封闭团，recall@10 从 0.9 掉到 0.17（实测复现并修复）。

### 3.4 GPU 路径折算（模型，非实测）

以 C++ 20k×128d 实测为基准，`speedup_factor=8.0`：

| 项 | 数值 |
| --- | ---: |
| GPU 距离计算（模型） | 7.30 s × 95% ÷ 8.0 ≈ 0.87 s |
| CPU 装配（不变，~5%） | ≈ 0.37 s |
| 上传 9.77 MB + 回传 2.5 MB @ 10 GB/s | ≈ 0.001 s |
| 固定开销 | 0.005 s |
| **GPU 端到端（模型）** | **≈ 1.24 s** |
| **构建加速比（模型）** | **≈ 5.9×** |

Python 原型口径下模型加速比为 7.50×（见 `amd/bench/result.json`）。两个口径方向一致：**只要距离计算卸载到 hipVS，构建端到端加速由装配段（Amdahl）封顶**。

### 3.5 回退与可观测性

- `hipvs::DistanceBackend` 三态：`is_gpu()` 区分 CPU/GPU；`compute()` 失败时自动回落到 CPU 参考实现并保证结果一致（`hnsw_builder.cpp` 的 fallback 分支）；
- `bench_utils.path_status` 三态判定（GPU_ACTIVE / FALLBACK / CPU_ONLY），**FALLBACK 与 CPU_ONLY 均不计入加速成绩**，有单测覆盖；
- `amd/run.sh` 在找不到 `hipcc` 时打印 `[WARN] hipcc not found, CPU results only` 并正常退出（退出码 0），在有 ROCm 的机器上自动追加 HIP 后端构建与对比。

## 4. 未实测项与实测步骤

| 未实测项 | 原因 | 真实环境怎么测 |
| --- | --- | --- |
| hipVS kernel 在 AMD GPU 上的真实耗时 | 本机无 ROCm/GPU | `cd amd/hipvs-hnsw && make hipvs && ./build/hipvs_bench_hip --backend cpu/hipvs`（README 有完整命令） |
| seekdb 引擎内的端到端构建加速 | 需要把 `DistanceBackend` 接入 `ob_vsag_adaptor` 构建路径并在 GPU 机器上编译 seekdb | 接入点与设计见 `amd/design/hipvs-acceleration.md` |
| 多次连续构建的显存稳定性 | 无 GPU | 连续构建 10 次 + `rocm-smi` 观察显存 |

## 5. 结论

1. **链路定位正确**：seekdb HNSW 构图在 vsag 内，GPU 加速必须落在候选生成的距离计算上，图维护留在 CPU；
2. **加速空间真实存在**：两个独立实现（Python 原型 99.5% / C++ >95%）都表明距离计算是构建主导成本；
3. **工程质量可验证**：recall@10 最高 0.908（C++ 20k×128d、ef=256）/ 0.974（Python 原型），无孤立点，反向边剪枝语义修复有实测依据；
4. **诚实标注**：所有 GPU 数字均为模型折算，报告与 README 中均有明显标注，未写成实测。

## 题面检查项映射

| 检查项 | 证据位置 |
| --- | --- |
| GPU 构建路径有日志/性能证据、能识别回退 | `amd/hipvs-hnsw/src/distance_backend.h`（后端抽象 + `is_gpu()`）、`bench/bench_utils.py::path_status`（三态）、`amd/run.sh` 回退提示 |
| 索引可用性 | §3.2 / §3.3 的 Recall 与图自检 |
| Recall@K 校验 | `bench_utils.recall_at_k` + `tests/test_bench_utils.py::TestRecall` + §3.2/§3.3 实测值 |
| CPU 基线路径存在且可对比 | `bench/bench_build.py`（纯 CPU 可跑）与 `hipvs-hnsw` CPU 后端；GPU 列仅在模型栏出现 |
| 稳定性（无崩溃/卡死/泄漏） | 单测 9 用例 + C++ 基准多次连跑无异常；GPU 稳定性步骤见 §4 |
