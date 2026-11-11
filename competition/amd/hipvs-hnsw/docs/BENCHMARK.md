# 基准方法与实测数据

## 1. 方法

| 项 | 取值 |
| --- | --- |
| 数据集 | 随机高斯向量，L2 归一化可选；`seed=20261002`（base） / `20261003`（queries），可复现 |
| 规模 | 20,000 × 128d（主口径）、50,000 × 128d（规模校验）、300 × 32d（冒烟） |
| HNSW 参数 | `M=16`、`ef_construction=100`、`ef_search∈{32,64,128,256}`、`k=10` |
| 计时 | 单线程，`std::chrono::steady_clock` 包住整个构建（含层级采样、构图、反向边剪枝） |
| 质量 | `recall@10` 对比暴力精确检索（同一浮点表达式，同一数据） |
| 机器 | x86_64 Linux（WSL2），标量 CPU 参考后端；非 AMD GPU 环境 |

复现：

```bash
cd amd/hipvs-hnsw
bash scripts/run_bench.sh     # 300 / 20000 / 50000 三档
bash scripts/sweep_ef.sh      # recall vs ef_search（20k）
```

## 2. CPU 参考后端实测（本机）

| 规模 | 构建耗时 | 距离对总数 | 吞吐 | recall@10 (ef=32/64/128/256) |
| --- | ---: | ---: | ---: | --- |
| 20,000 × 128d | 7.69 s | 77.6 M | 10.1 M 对/s | 0.585 / 0.735 / 0.826 / 0.908 |
| 50,000 × 128d | 25.64 s | 213.0 M | 8.3 M 对/s | 0.61（ef=64） |
| 300 × 32d（冒烟） | 0.014 s | 0.33 M | 23.2 M 对/s | 1.000（ef=64） |

图结构自检：第 0 层平均度恒为 32（=2M），孤立点 0，最大层级 3～4。

> 数据文件见 `results/cpu_20k_128.json`、`results/cpu_50k_128.json`。

## 3. 加速比推算（GPU 侧）

构建耗时里距离计算占比 **> 95%**，因此

```
speedup ≈ 1 / (0.95 / S + 0.05)
```

| 距离计算加速比 S | 端到端构建加速比 |
| ---: | ---: |
| 5× | 4.2× |
| 10× | 6.9× |
| 20× | 10.0× |

参考量级：RDNA3（gfx1100，Radeon RX 7900 级）在 128 维浮点成对距离上通常可达
**数个 TFLOP/s 级**的有效吞吐，是单核标量 CPU 的 20～50 倍；实际受限于 PCIe 传输与批次大小，
构建期单批次候选数很小（每条边 ≤ 2M），所以关键在于 **把同一层的候选合并成大批次**（本实现
已经把「一个节点的全部邻居」合并为一次后端调用，属于最小可用粒度）。

> 因此 GPU 侧的预期区间是 **3～8 倍端到端加速**，具体数字需在 AMD 设备上按 `README.md` 的
> 命令实测后填入本节表格——这是本作品目前唯一缺口（当前环境没有 AMD GPU）。

## 4. 在 AMD Radeon 上补测（一条命令）

```bash
hipcc -std=c++17 -O3 -DHIPVS_ENABLED --offload-arch=gfx1100 -Isrc \
      src/distance_cpu.cpp src/hnsw_builder.cpp src/bench_main.cpp src/distance_hipvs.hip \
      -o build/hipvs_bench_hip -lhipvs

for backend in cpu hipvs; do
  ./build/hipvs_bench_hip --rows 20000 --dim 128 --backend $backend \
      --json results/radeon_${backend}.json
done
```

把两份 JSON 的 `build_seconds` 相除即为该设备上的端到端加速比，`recall_at_k` 应完全一致
（同一浮点表达式 + 确定性构图），可直接填进第 3 节的表格。

## 5. 与 seekdb 原生构建的对照口径

| 口径 | 说明 |
| --- | --- |
| 同数据、同参数 | 20k×128d、M=16、efC=100，两套实现一致 |
| 只比构建时间 | 查询链路不改动，因此只比构建 |
| 校验 recall | 两套实现的 recall@10 必须同量级，否则加速没有意义 |
| 无 GPU 时 | 必须与今天的行为逐字节一致（CPU 回退） |