# 提交自检清单：hipVS 加速 HNSW 构建

> 每一项给出可直接打开的路径与一句话说明；未实测项如实标注。

## 一、交付三件套

| # | 要求 | 本材料证据 | 状态 |
| --- | --- | --- | --- |
| 1 | **实现代码** | [hipvs-hnsw/src/](hipvs-hnsw/src/)：`distance_backend.h`（后端抽象）、`distance_hipvs.hip`（HIP kernel）、`hnsw_builder.{h,cpp}`（构图器）、`distance_cpu.cpp`（CPU 参考）；[hipvs-hnsw/docs/DESIGN.md](hipvs-hnsw/docs/DESIGN.md) 给出 seekdb 集成点（`ob_vsag_adaptor` 构建路径、配置开关、回退） | C++ 实现**本机编译并实测通过**；HIP 分支经桩头文件语法校验；引擎内集成未合并（需 GPU 环境） |
| 2 | **复现步骤** | [README.md](README.md)：环境、单测、基准、一键脚本；`bash amd/run.sh` 一条命令完成"构建→基准→recall 校验→退出码" | 可执行（CPU 部分本机实测） |
| 3 | **实验报告** | [REPORT.md](REPORT.md)：环境表、方法、实测输出（单测 / Python 原型 / C++ 基准 / Recall）、GPU 模型折算、未实测项、检查项映射；[bench/report.md](bench/report.md)：基准自动生成输出 | 完成；GPU 真实数字待补 |

## 二、题面检查项

| # | 检查项 | 证据位置 | 说明 |
| --- | --- | --- | --- |
| 1 | GPU 构建路径有日志/性能证据，能识别回退 | `bench/bench_utils.py::path_status` 三态判定；`hipvs-hnsw/src/distance_backend.h` 的 `is_gpu()` / `prepare()` / `release()`；`amd/run.sh` 的 `[WARN] hipcc not found, CPU results only` | 回退**显式可见**且不计入加速 |
| 2 | 索引可用性（能查、结果合理） | [REPORT.md](REPORT.md) §3.2/§3.3：Recall@10 = 0.974（Python 原型）/ 0.908（C++，ef=256）；图自检孤立点 0 | 原型与 C++ 实现双重验证 |
| 3 | Recall@K 校验 | `bench/bench_utils.py::recall_at_k`；`tests/test_bench_utils.py::TestRecall`；[hipvs-hnsw/src/bench_main.cpp](hipvs-hnsw/src/bench_main.cpp) 的 exact-knn 对照 | 方法 + 单测 + 实测齐全 |
| 4 | CPU 基线路径存在且可对比 | `bench/bench_build.py`（纯 stdlib 可跑）；`hipvs-hnsw` 的 CPU 后端与 `--backend cpu` 开关 | 基线未被"优化"，对比公平 |
| 5 | 稳定性（无崩溃/卡死/泄漏） | 单测 9 用例；C++ 基准 20k/50k 连跑无异常；构图确定性（同一输入同一图） | GPU 侧稳定性步骤见 REPORT §4 |

## 三、文件清单

| 路径 | 作用 |
| --- | --- |
| [README.md](README.md) | 导航与复现 |
| [REPORT.md](REPORT.md) | 实验报告 |
| [run.sh](../amd/run.sh) | 自检入口（构建 + 基准 + 校验 + 退出码） |
| [bench/bench_build.py](bench/bench_build.py) | 基准主程序（纯 Python，无第三方依赖） |
| [bench/bench_utils.py](bench/bench_utils.py) | 统计 / Recall / 回退判定 / GPU 模型 |
| [bench/report.md](bench/report.md) · [bench/result.json](bench/result.json) | 最近一次基准的真实输出 |
| [tests/test_bench_utils.py](tests/test_bench_utils.py) | 单元测试 |
| [design/hipvs-acceleration.md](design/hipvs-acceleration.md) | 设计文档（同 hipvs-hnsw/docs/DESIGN.md） |
| [hipvs-hnsw/](hipvs-hnsw/) | 完整 C++ 实现（builder + HIP kernel + 基准驱动 + 实测 JSON） |

## 四、最终自检命令（真实执行结果）

```text
$ python3 -m unittest discover -s amd/tests -v
Ran 9 tests in 0.017s
OK

$ python3 amd/bench/bench_build.py
[bench] numpy=no  n=800 d=64 queries=50 runs=3
[bench] cpu_median=1312.7 ms  gpu_model=174.9 ms  model_speedup=7.50x  recall@10=0.9740

$ bash amd/run.sh --quick
[OK] recall above the sanity threshold
[WARN] hipcc not found, CPU results only
[OK] done
```

## 五、提交前提醒

- [ ] 若拿到 AMD GPU 环境：`cd amd/hipvs-hnsw && make hipvs`，然后按 [hipvs-hnsw/README.md](hipvs-hnsw/README.md) 的 ROCm 段落补真实数字；
- [ ] 所有"模型/折算/mock"数字不得写成实测；
- [ ] 新增/修改文件均在 `amd/` 下，不触碰 seekdb 源码树。
