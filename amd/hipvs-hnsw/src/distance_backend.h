/**
 * Copyright (c) 2025 OceanBase
 * hipVS accelerated HNSW build for seekdb - distance backend interface
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace hipvs {

/// Distance metric used both by the CPU reference path and by the hipVS kernels.
enum class Metric : int {
  L2 = 0,      ///< squared euclidean distance (the HNSW ordering metric used by l2)
  INNER = 1,   ///< negative inner product, so that "smaller is closer"
  COSINE = 2,  ///< 1 - cosine similarity, so that "smaller is closer"
};

/// A pluggable "one query against many candidate rows" distance computer.
///
/// The HNSW insertion path always evaluates a batch of candidates for one query,
/// which is exactly the shape a GPU wants: one query vector stays in registers and
/// each thread block walks a slice of the candidate rows. Keeping this behind an
/// interface lets the builder run unchanged on a machine without a GPU.
class DistanceBackend {
public:
  virtual ~DistanceBackend() = default;

  virtual const char* name() const = 0;
  virtual bool is_gpu() const { return false; }

  /// Compute the distance from `query` to `count` rows of `base`.
  ///
  /// @param query      pointer to dim floats
  /// @param base       row major matrix, `rows * dim` floats
  /// @param ids        row ids to compare against
  /// @param count      number of ids
  /// @param dim        vector dimension
  /// @param metric     distance metric
  /// @param out        caller provided array of `count` floats, ascending is better
  virtual int compute(const float *query,
                      const float *base,
                      const uint32_t *ids,
                      int64_t count,
                      int64_t dim,
                      Metric metric,
                      float *out) = 0;

  /// Optional hook: called once before a build so a backend can upload the data set.
  /// The CPU backend ignores it; the hipVS backend copies `base` to device memory once
  /// instead of once per batch.
  virtual int prepare(const float *base, int64_t rows, int64_t dim, Metric metric)
  {
    (void)base;
    (void)rows;
    (void)dim;
    (void)metric;
    return 0;
  }

  /// Optional hook: release any device memory taken by prepare().
  virtual void release() {}
};

/// CPU reference backend (scalar loop, always available).
std::unique_ptr<DistanceBackend> make_cpu_backend();

#if defined(USE_HIPVS)
/// hipVS backend; returns nullptr when no AMD device is visible at runtime so the
/// caller can fall back to the CPU path without changing the build recipe.
std::unique_ptr<DistanceBackend> make_hipvs_backend(int device_id, int64_t rows, int64_t dim, Metric metric);
#endif

}  // namespace hipvs