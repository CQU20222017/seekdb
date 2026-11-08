/**
 * Copyright (c) 2025 OceanBase
 * hipVS accelerated HNSW build for seekdb - CPU reference backend
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

#include "distance_backend.h"

#include <cmath>

namespace hipvs {
namespace {

inline float row_distance(const float *a, const float *b, int64_t dim, Metric metric)
{
  float acc = 0.0f;
  switch (metric) {
    case Metric::L2: {
      for (int64_t d = 0; d < dim; ++d) {
        const float diff = a[d] - b[d];
        acc += diff * diff;
      }
      return acc;
    }
    case Metric::INNER: {
      for (int64_t d = 0; d < dim; ++d) {
        acc += a[d] * b[d];
      }
      return -acc;
    }
    case Metric::COSINE: {
      float na = 0.0f;
      float nb = 0.0f;
      for (int64_t d = 0; d < dim; ++d) {
        acc += a[d] * b[d];
        na += a[d] * a[d];
        nb += b[d] * b[d];
      }
      const float denom = std::sqrt(na) * std::sqrt(nb);
      return denom > 0.0f ? 1.0f - acc / denom : 0.0f;
    }
  }
  return acc;
}

class CpuBackend final : public DistanceBackend {
public:
  const char *name() const override { return "cpu-scalar"; }

  int compute(const float *query,
              const float *base,
              const uint32_t *ids,
              int64_t count,
              int64_t dim,
              Metric metric,
              float *out) override
  {
    if (nullptr == query || nullptr == base || nullptr == ids || nullptr == out || count < 0 || dim <= 0) {
      return -1;
    }
    for (int64_t i = 0; i < count; ++i) {
      out[i] = row_distance(query, base + static_cast<int64_t>(ids[i]) * dim, dim, metric);
    }
    return 0;
  }
};

}  // namespace

std::unique_ptr<DistanceBackend> make_cpu_backend()
{
  return std::unique_ptr<DistanceBackend>(new CpuBackend());
}

}  // namespace hipvs