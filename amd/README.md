# AMD × hipVS 赛题：用 hipVS 加速 seekdb 的 HNSW 索引构建

参赛材料目录。目标：用 AMD GPU（hipVS）加速 seekdb 向量索引的**构建**（不是查询），保留 CPU 基线，回退可识别、计时可复现、Recall 可校验。

## 题目摘要

- **赛题**：OceanBase seekdb 向量索引在 AMD GPU 上的构建加速（AMD × hipVS）。
- **要解决的问题**：HNSW 构建的候选邻居搜索是距离计算密集型，CPU 上逐点串行，规模上去后构建时间成为瓶颈；AMD GPU 的批量并行能力可以把这段距离计算卸载到 hipVS。
- **约束（题面）**：保留 CPU 基线；GPU 不可用须显式报错或记录回退；CPU 回退不得计作加速成绩；计时从"数据准备完成"到"索引可查询"，含上传/同步/格式转换；每组 ≥3 次取中位数；用 Recall@K 校验索引可用。
- **交付**：可合入 seekdb 的接入代码（实现 + 集成点）、可复现的测试方法、CPU/GPU 构建性能对比报告（未实测部分明确标注）。

## 方案概览

seekdb 的 HNSW 由外部预编译库 **vsag** 实现（`src/oblib/lib/vector/ob_vsag_adaptor.cpp` → `vsag::Index::Build`）。方案分两层：

1. **GPU 侧（hipVS）**：批量距离计算（一条查询 × 一批候选行），HIP kernel 见 [hipvs-hnsw/src/distance_hipvs.hip](hipvs-hnsw/src/distance_hipvs.hip)；
2. **CPU 侧（构图）**：层级采样、候选选择、反向边按距离剪枝、度裁剪，图结构语义不变，见 [hipvs-hnsw/src/hnsw_builder.cpp](hipvs-hnsw/src/hnsw_builder.cpp)。

接口抽象是 `hipvs::DistanceBackend`（`compute` / `prepare` / `release` / `is_gpu`），GPU 不可用时回退 CPU 参考实现且行为逐字节一致。完整设计见 [design/hipvs-acceleration.md](design/hipvs-acceleration.md)。

## 目录导航

| 路径 | 内容 |
| --- | --- |
| [REPORT.md](REPORT.md) | **实验报告**：环境、方法、实测结果（单测 / Python 原型 / C++ 基准 / Recall）、GPU 模型折算、未实测项 |
| [submission-checklist.md](submission-checklist.md) | 题面检查项的证据索引 |
| [bench/bench_build.py](bench/bench_build.py) | 基准主程序（**纯 Python，无第三方依赖**，任何机器可跑） |
| [bench/bench_utils.py](bench/bench_utils.py) | 统计、Recall、回退三态判定、GPU 耗时模型 |
| [bench/report.md](bench/report.md) / [bench/result.json](bench/result.json) | 最近一次基准的真实输出 |
| [tests/test_bench_utils.py](tests/test_bench_utils.py) | 单元测试（9 用例） |
| [design/hipvs-acceleration.md](design/hipvs-acceleration.md) | 设计文档 |
| [hipvs-hnsw/](hipvs-hnsw/) | **完整 C++ 实现**：分层 HNSW 构建器 + HIP kernel + 基准驱动 + 本机实测数据 |
| [run.sh](run.sh) | 一键自检入口（构建 → 基准 → Recall 校验 → 明确退出码） |

## 复现步骤

### 0. 环境

- Python ≥ 3.8（**不需要 numpy**，没有时自动走纯 stdlib 路径）；
- 可选：g++ / clang++（跑 C++ 实现）；可选：ROCm + hipcc + AMD GPU（跑 GPU 路径）。

### 1. 单元测试（必跑，纯 CPU）

```bash
python3 -m unittest discover -s amd/tests -v
```

预期：`Ran 9 tests ... OK`。

### 2. 基准（必跑，纯 CPU；GPU 列为模型折算）

```bash
python3 amd/bench/bench_build.py --n 800 --d 64 --queries 50 --runs 3
```

输出：CPU 两阶段逐轮耗时、中位数、GPU 模型折算、加速比、Recall@10、构图完整性，并写入 `bench/report.md` 与 `bench/result.json`。GPU 数字全部来自耗时模型，**不是实测**。

### 3. C++ 实现基准（真实计时）

```bash
cd amd/hipvs-hnsw && make bench      # 300 / 20000 / 50000 三档，输出 JSON
```

本机实测：20,000×128d 构建 7.3 s、recall@10 最高 0.908；50,000×128d 构建 23.7 s。详见 [REPORT.md](REPORT.md) §3.3。

### 4. 一键自检

```bash
bash amd/run.sh           # 完整流程；退出码非 0 即失败
bash amd/run.sh --quick   # 小数据快速版
```

### 5. 真实 GPU 构建（需 ROCm 环境）

```bash
cd amd/hipvs-hnsw && make hipvs        # hipcc 编译 HIP 后端
./build/hipvs_bench_hip --rows 20000 --dim 128 --backend cpu    --json results/radeon_cpu.json
./build/hipvs_bench_hip --rows 20000 --dim 128 --backend hipvs  --json results/radeon_hipvs.json
```

两份 JSON 的 `build_seconds` 相除即端到端加速比；`recall_at_k` 应一致（同一浮点表达式 + 确定性构图）。无 AMD 设备时 `--backend hipvs` 会提示并自动回退 CPU。

## 实测状态声明

| 项 | 状态 |
| --- | --- |
| 单元测试（9 用例） | **已实测** |
| Python 原型基准（n=800, d=64） | **已实测**（Recall@10 = 0.9740） |
| C++ HNSW 构建（20k/50k × 128d） | **已实测**（Recall@10 最高 0.908） |
| HIP kernel 语法 | **已校验**（桩头文件主机侧编译通过；修复了 `hipMalloc` 指针类型问题） |
| GPU 构建耗时与加速比 | **未实测**：本环境无 AMD GPU/ROCm；报告数字为耗时模型折算并已标注 |
| seekdb 引擎内集成 | **未合并**：集成点与配置项设计见 design 文档 |