/**
 * Copyright (c) 2025 OceanBase
 * hipVS accelerated HNSW build for seekdb - hipVS backend (HIP)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// This translation unit is only part of the build when hipVS is enabled
// (-DHIPVS_ENABLED=ON, compiled with hipcc). The builder itself never includes it:
// it talks to the DistanceBackend interface only, so a CPU only build stays clean.

#include "distance_backend.h"

#include <hip/hip_runtime.h>

#include <cstdio>
#include <cstdlib>

namespace hipvs {
namespace {

/// One thread per candidate row: the query stays in registers / LDS while the block
/// walks the gathered rows. For dim=128 this is fully bandwidth bound on RDNA3, which
/// is the property the whole acceleration relies on.
template <Metric M>
__global__ void distance_kernel(const float *__restrict__ query,
                                const float *__restrict__ base,
                                const uint32_t *__restrict__ ids,
                                int64_t count,
                                int64_t dim,
                                float *__restrict__ out)
{
  const int64_t idx = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (idx >= count) {
    return;
  }
  const float *row = base + static_cast<int64_t>(ids[idx]) * dim;
  float acc = 0.0f;
  if (M == Metric::L2) {
    for (int64_t d = 0; d < dim; ++d) {
      const float diff = query[d] - row[d];
      acc += diff * diff;
    }
  } else if (M == Metric::INNER) {
    for (int64_t d = 0; d < dim; ++d) {
      acc += query[d] * row[d];
    }
    acc = -acc;
  } else {
    float na = 0.0f;
    float nb = 0.0f;
    for (int64_t d = 0; d < dim; ++d) {
      acc += query[d] * row[d];
      na += query[d] * query[d];
      nb += row[d] * row[d];
    }
    const float denom = sqrtf(na) * sqrtf(nb);
    acc = denom > 0.0f ? 1.0f - acc / denom : 0.0f;
  }
  out[idx] = acc;
}

class HipBackend final : public DistanceBackend {
public:
  HipBackend(int device_id, int64_t rows, int64_t dim, Metric metric)
      : device_id_(device_id), rows_(rows), dim_(dim), metric_(metric)
  {
  }

  ~HipBackend() override { release(); }

  const char *name() const override { return "hipvs-rdna3"; }
  bool is_gpu() const override { return true; }

  int prepare(const float *base, int64_t rows, int64_t dim, Metric metric) override
  {
    int ret = 0;
    rows_ = rows;
    dim_ = dim;
    metric_ = metric;
    if (hipSetDevice(device_id_) != hipSuccess) {
      return -1;
    }
    const size_t bytes = static_cast<size_t>(rows) * static_cast<size_t>(dim) * sizeof(float);
    if (hipMalloc(&d_base_, bytes) != hipSuccess) {
      return -1;
    }
    if (hipMemcpy(d_base_, base, bytes, hipMemcpyHostToDevice) != hipSuccess) {
      return -1;
    }
    return ret;
  }

  void release() override
  {
    if (nullptr != d_base_) {
      hipFree(d_base_);
      d_base_ = nullptr;
    }
  }

  int compute(const float *query,
              const float *base,
              const uint32_t *ids,
              int64_t count,
              int64_t dim,
              Metric metric,
              float *out) override
  {
    (void)base;
    if (nullptr == d_base_ || count <= 0 || dim != dim_) {
      return -1;
    }
    int ret = 0;
    const size_t qbytes = static_cast<size_t>(dim) * sizeof(float);
    const size_t ibytes = static_cast<size_t>(count) * sizeof(uint32_t);
    const size_t obytes = static_cast<size_t>(count) * sizeof(float);
    float *d_query = nullptr;
    uint32_t *d_ids = nullptr;
    float *d_out = nullptr;
    if (hipMalloc(&d_query, qbytes) != hipSuccess || hipMalloc(&d_ids, ibytes) != hipSuccess ||
        hipMalloc(&d_out, obytes) != hipSuccess) {
      ret = -1;
    } else if (hipMemcpy(d_query, query, qbytes, hipMemcpyHostToDevice) != hipSuccess ||
               hipMemcpy(d_ids, ids, ibytes, hipMemcpyHostToDevice) != hipSuccess) {
      ret = -1;
    } else {
      const int threads = 256;
      const int blocks = static_cast<int>((count + threads - 1) / threads);
      switch (metric) {
        case Metric::L2:
          hipLaunchKernelGGL(distance_kernel<Metric::L2>, dim3(blocks), dim3(threads), 0, 0,
                             d_query, d_base_, d_ids, count, dim, d_out);
          break;
        case Metric::INNER:
          hipLaunchKernelGGL(distance_kernel<Metric::INNER>, dim3(blocks), dim3(threads), 0, 0,
                             d_query, d_base_, d_ids, count, dim, d_out);
          break;
        default:
          hipLaunchKernelGGL(distance_kernel<Metric::COSINE>, dim3(blocks), dim3(threads), 0, 0,
                             d_query, d_base_, d_ids, count, dim, d_out);
          break;
      }
      if (hipGetLastError() != hipSuccess) {
        ret = -1;
      } else if (hipDeviceSynchronize() != hipSuccess) {
        ret = -1;
      } else if (hipMemcpy(out, d_out, obytes, hipMemcpyDeviceToHost) != hipSuccess) {
        ret = -1;
      }
    }
    if (nullptr != d_query) { hipFree(d_query); }
    if (nullptr != d_ids) { hipFree(d_ids); }
    if (nullptr != d_out) { hipFree(d_out); }
    return ret;
  }

private:
  int device_id_;
  int64_t rows_;
  int64_t dim_;
  Metric metric_;
  float *d_base_ = nullptr;
};

}  // namespace

std::unique_ptr<DistanceBackend> make_hipvs_backend(int device_id, int64_t rows, int64_t dim, Metric metric)
{
  int count = 0;
  if (hipGetDeviceCount(&count) != hipSuccess || count <= 0) {
    return nullptr;
  }
  if (device_id < 0 || device_id >= count) {
    device_id = 0;
  }
  std::unique_ptr<HipBackend> backend(new HipBackend(device_id, rows, dim, metric));
  return std::unique_ptr<DistanceBackend>(backend.release());
}

}  // namespace hipvs