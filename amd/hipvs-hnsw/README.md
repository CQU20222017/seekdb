# 使用 hipVS 加速 HNSW 索引构建

赛题作品：**在 seekdb 的 HNSW 索引构建链路中引入 AMD Radeon GPU（hipVS）加速**，把构建期
最重的「一条查询 vs 一批候选」距离计算搬到 GPU 上，同时保留纯 CPU 回退路径，保证没有 GPU
的机器行为不变。

## 本目录内容

| 路径 | 说明 |
| --- | --- |
| [`docs/DESIGN.md`](docs/DESIGN.md) | 架构设计：seekdb HNSW 构建链路、hipVS 接入点、kernel 设计、数据布局、回退与错误处理 |
| [`docs/BENCHMARK.md`](docs/BENCHMARK.md) | 基准方法（数据集/参数/指标）、本机实测数据、AMD Radeon 上的复现步骤 |
| `src/distance_backend.h` | 距离计算后端接口 `hipvs::DistanceBackend`（构建期唯一的重负载原语） |
| `src/distance_cpu.cpp` | CPU 参考实现（标量，作为基线 + 兜底） |
| `src/distance_hipvs.hip` | hipVS/HIP 实现（一个线程一条候选行，query 常驻寄存器） |
| `src/hnsw_builder.{h,cpp}` | 分层图构建：层级采样、层内搜索、多样性邻居选择、反向边按距离剪枝 |
| `src/bench_main.cpp` | 基准驱动：生成数据、构建、recall@k 校验、输出 JSON |
| `scripts/run_bench.sh` | 一键：编译 CPU 版本 + 跑 300/20k/50k 三档基准 |
| `scripts/sweep_ef.sh` | recall 与 `ef_search` 的权衡曲线 |
| `results/` | 本机实测 JSON（CPU 路径） |

## 快速开始（无 GPU 也能验证完整逻辑）

```bash
cd amd/hipvs-hnsw
bash scripts/run_bench.sh          # 编译 CPU 版并跑 300 / 20000 / 50000 三档
bash scripts/sweep_ef.sh           # recall vs ef_search
```

本机（x86_64, 单线程, 标量）实测：

| 规模 | 构建耗时 | 距离计算量 | 吞吐 | recall@10 (ef=64 / 128 / 256) |
| --- | ---: | ---: | ---: | ---: |
| 20,000 × 128d | 7.69 s | 77.6 M 对 | 10.1 M 对/s | 0.735 / 0.826 / 0.908 |
| 50,000 × 128d | 25.6 s | 213.0 M 对 | 8.3 M 对/s | 0.61（ef=64） |

> 构建期距离计算占比 > 95%，所以只要把这一个原语搬到 GPU，构建耗时就近似按距离吞吐线性下降；
> 详见 `docs/BENCHMARK.md` 的加速比推算与 GPU 侧测法。

## 在 AMD Radeon 上启用 hipVS

```bash
# 1. 安装 ROCm（Radeon 需 6.x，含 hipcc / rocblas / hipBLASLt 可选）
#    确认 hipVS 头与库路径，例如 /opt/rocm/include/hipvs 与 -lhipvs

# 2. 用 hipcc 编译带 GPU 后端的基准
hipcc -std=c++17 -O3 -DHIPVS_ENABLED --offload-arch=gfx1100 \
      -Isrc src/distance_cpu.cpp src/hnsw_builder.cpp src/bench_main.cpp \
      src/distance_hipvs.hip -o build/hipvs_bench_hip -lhipvs

# 3. 同一数据、同一参数跑两次：CPU 基线 与 hipVS
./build/hipvs_bench_hip --rows 20000 --dim 128 --backend cpu   --json results/radeon_cpu.json
./build/hipvs_bench_hip --rows 20000 --dim 128 --backend hipvs --json results/radeon_hipvs.json
```

`--backend hipvs` 时，若运行期看不到 AMD 设备，程序会打印
`[warn] no AMD device visible, falling back to the CPU backend` 并继续跑 CPU 路径——这与
seekdb 侧「GPU 不可用即回退」的策略一致。

## 与 seekdb 的接入关系

- 构建期的重负载是 `候选批量距离计算`，对应 `src/oblib/lib/vector/ob_vsag_adaptor.cpp` 里
  VSAG 构建回调触发的距离计算；
- 本作品把该原语抽象成 `hipvs::DistanceBackend`，并给出可直接接入 seekdb 的三步方案
  （配置开关 → 构建期把一批候选交给 hipVS → 失败/无设备时回退 CPU），详见 `docs/DESIGN.md`
  的「与 seekdb 的接入点」一节；
- 索引构建完成后写回的图结构与查询语义不变（分层 HNSW + 单调搜索），因此查询路径无需改动。

## 许可

本目录代码遵循仓库根目录的 Apache License 2.0。