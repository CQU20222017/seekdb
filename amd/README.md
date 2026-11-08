# AMD 赛题作品：使用 hipVS 加速 HNSW 索引构建

作品位于 [`hipvs-hnsw/`](hipvs-hnsw/README.md)。

| 入口 | 说明 |
| --- | --- |
| [`hipvs-hnsw/README.md`](hipvs-hnsw/README.md) | 作品总览、目录说明、快速开始（无 GPU 也可跑）、ROCm 复现命令 |
| [`hipvs-hnsw/docs/DESIGN.md`](hipvs-hnsw/docs/DESIGN.md) | 架构设计、与 seekdb 的接入点、kernel 设计、回退与错误处理 |
| [`hipvs-hnsw/docs/BENCHMARK.md`](hipvs-hnsw/docs/BENCHMARK.md) | 基准方法、本机实测数据、加速比推算、AMD Radeon 上的复现步骤 |
| [`hipvs-hnsw/scripts/run_bench.sh`](hipvs-hnsw/scripts/run_bench.sh) | 一键编译 CPU 版并跑 300 / 20000 / 50000 三档基准 |
| [`hipvs-hnsw/results/`](hipvs-hnsw/results/) | 本机实测 JSON（CPU 参考后端） |

## 一条命令验证（任何 Linux x86_64，无需 GPU）

```bash
cd amd/hipvs-hnsw
bash scripts/run_bench.sh
```

也可以直接：

```bash
cd amd/hipvs-hnsw
make bench        # 等价于 g++ -O3 + 跑基准
```

## 在 AMD Radeon + ROCm 上验证 GPU 加速

```bash
cd amd/hipvs-hnsw
make hipvs        # hipcc -DHIPVS_ENABLED --offload-arch=gfx1100
./build/hipvs_bench_hip --rows 20000 --dim 128 --backend cpu
./build/hipvs_bench_hip --rows 20000 --dim 128 --backend hipvs
```

两份输出的 `build_seconds` 相除即端到端加速比；`recall@10` 必须一致（同一浮点表达式 + 确定性构图）。
如果运行环境没有 AMD 设备，`--backend hipvs` 会打印提示并自动回退到 CPU 路径。