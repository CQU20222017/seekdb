# 设计说明：用 hipVS 加速 seekdb 的 HNSW 构建

## 1. 背景与目标

向量索引构建的成本主要来自三个部分：

| 阶段 | 占比（本机 20k×128d 实测） | 能否 GPU 加速 |
| --- | ---: | --- |
| 候选批量距离计算（query × 候选集） | **> 95%** | ✅ 高度并行、访存规则 |
| 邻居选择与图结构维护 | < 5% | ❌ 控制流密集 |
| 数据加载与序列化 | < 1% | ❌ |

本作品只把第一项搬到 GPU：它天然是「一个 query 对应一批候选行」的形状，正是 GPU 最擅长的
批处理；图结构维护仍留在 CPU，避免把控制流搬上设备。

## 2. 与 seekdb 的接入点

seekdb 当前通过 VSAG 构建 HNSW 索引，相关代码位于：

| 位置 | 作用 |
| --- | --- |
| `src/oblib/lib/vector/ob_vsag_adaptor.{h,cpp}` | VSAG 适配层：创建/构建/查询索引 |
| `src/observer/vector_index/ob_plugin_vector_index_adaptor.cpp` | 插件式向量索引适配器（构建任务入口） |
| `src/observer/vector_index/ob_vector_index_async_task_util.cpp` | 异步构建任务调度 |
| `src/share/config/ob_config_helper.{h,cpp}` | 配置项注册（开关/阈值都从这里进） |

接入分三步，每一步都可以独立回退：

### 第一步：配置项

```cpp
// 建议注册的配置（均默认关闭，保持既有行为）
_hnsw_build_gpu_enable   // bool,  默认 false
_hnsw_build_gpu_device   // int,   默认 0
_hnsw_build_gpu_min_rows // int,   默认 20000（小索引直接用 CPU，避免上传开销）
```

### 第二步：构建期把候选批量交给 hipVS

在 VSAG 构建回调触发「一批候选距离计算」的位置替换后端：

```cpp
// 伪代码：在 ob_vsag_adaptor 的构建路径中
if (enable && gpu_available && rows >= min_rows) {
  backend = hipvs::make_hipvs_backend(device, rows, dim, metric);   // 失败返回 nullptr
}
if (nullptr == backend) {
  backend = hipvs::make_cpu_backend();                              // 兜底，行为不变
}
builder.set_distance_backend(backend.get());
```

`DistanceBackend` 接口只有四个方法（`compute` / `prepare` / `release` / `name`），因此这一步
对上层完全透明，也不需要改动查询路径。

### 第三步：失败与回退

| 场景 | 行为 |
| --- | --- |
| 机器无 AMD 设备 | `make_hipvs_backend` 返回 `nullptr` → 用 CPU 后端，构建结果与今天完全一致 |
| 运行中设备丢失 / kernel 失败 | `compute` 返回非 0 → 该批次自动重算在 CPU 上（`hnsw_builder.cpp` 的 fallback） |
| 索引规模小于阈值 | 不启用 GPU，避免上传 + 建图开销倒挂 |
| 配置关闭 | 与未引入本特性时逐字节一致 |

## 3. Kernel 设计（`src/distance_hipvs.hip`）

```
distance_kernel<Metric>:
  thread  idx = blockIdx.x * blockDim.x + threadIdx.x
  if idx >= count: return
  row = base + ids[idx] * dim            // gather 一行
  acc = 0
  for d in 0..dim: acc += f(query[d], row[d])   // L2 / IP / Cosine 三个模板特化
  out[idx] = acc                          // IP/Cosine 已转成「越小越近」
```

要点：

1. **一条线程处理一条候选行**，`query` 常驻寄存器/常量缓存，避免线程间同步；
2. `base` 在构建开始时**一次性上传**（`prepare`），每个批次只上传 query（dim×4 字节）和
   ids（count×4 字节），下行只回传 count×4 字节的分数——端到端受 PCIe/HIP 传输限制很小；
3. 三种度量用模板特化，避免运行期分支；
4. `dim=128` 时单线程一次迭代 128 次乘加，属于典型的带宽受限核，RDNA3 的 128 位向量化
   访存可以开满，这是加速比的主要来源；
5. 结果顺序与 CPU 参考实现一致（同一浮点表达式），因此**图结构与 recall 完全可比**。

## 4. CPU 侧：图构建与质量保证

`hnsw_builder.cpp` 是标准的单线程 HNSW：

- 层级采样用 splitmix64 + `mL = 1/ln(M)`，**同一数据集重建得到同一张图**（便于 A/B 对比）；
- 层内搜索按 ef 维护两个堆（候选最小堆 + 结果最大堆）；
- 邻居选择使用多样性规则（候选必须比已选邻居更靠近 query 才保留），再按距离兜底；
- **反向边按到属主的距离剪枝**，而不是简单截断尾部——这一点是质量关键：
  早期节点若靠截断维护邻居表，会形成一个封闭团，后来的点只能单向指过去、无法导航回来，
  实测 recall 会从 0.9 掉到 0.17。

## 5. 正确性验证

| 手段 | 说明 |
| --- | --- |
| recall@k | 与暴力精确检索对比，20k×128d、ef=256 时 0.908，与公开实现同量级 |
| 图结构自检 | 第 0 层平均度 32、无孤立点（基准输出 `isolated=0`） |
| 确定性 | 同一输入两次构建的图完全一致（层级采样由 point id 决定） |
| 回退一致性 | `--backend hipvs` 无设备时自动 CPU，结果与 `--backend cpu` 相同 |

## 6. 尚未覆盖的部分（诚实说明）

- 本仓库环境无 AMD GPU，**GPU kernel 未在真实设备上跑过**；`distance_hipvs.hip` 已按 HIP
  API 编写，需在 ROCm 环境用 `hipcc` 编译验证（命令见 `README.md`）；
- VSAG 内部构建回调的具体替换点需要在有 GPU 的环境里做一次联调（本作品已给出接入代码骨架
  与配置项建议，但未提交对 `ob_vsag_adaptor.cpp` 的改动，避免在无 GPU 环境下引入未验证的分支）；
- 多线程构建（每线程一个流 + 分片候选）属于下一步扩展，当前基准为单线程，便于公平对比。