/**
 * Copyright (c) 2025 OceanBase
 * hipVS accelerated HNSW build for seekdb - layered graph builder
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

#include <cstdint>
#include <vector>

#include "distance_backend.h"

namespace hipvs {

struct BuildParams {
  int64_t dim = 128;
  int m = 16;                 ///< max neighbours per node per layer (level 0 uses 2 * m)
  int ef_construction = 100;  ///< candidate list size while inserting
  Metric metric = Metric::L2;
  uint64_t seed = 2026;
};

struct BuildStats {
  int64_t rows = 0;
  int64_t distance_evaluations = 0;  ///< number of (query, candidate) pairs scored
  int64_t backend_calls = 0;         ///< number of batched backend.compute() calls
  int64_t max_level = 0;
  double build_seconds = 0.0;
};

/// Single threaded HNSW builder whose only heavy primitive is
/// DistanceBackend::compute(): one query against a batch of candidate ids.
///
/// Swapping the CPU backend for the hipVS backend therefore moves the whole
/// distance workload to the GPU without touching graph construction logic, which is
/// exactly how the seekdb HNSW build path is wired in docs/DESIGN.md.
class HnswBuilder {
public:
  HnswBuilder(const BuildParams &params, DistanceBackend *backend);
  ~HnswBuilder() = default;

  /// Build the graph over `rows` vectors of `params.dim` floats (row major).
  int build(const float *base, int64_t rows);

  /// k nearest neighbours for a probe vector, using the built graph.
  int search(const float *query, int k, int ef_search, std::vector<uint32_t> &result) const;

  const BuildStats &stats() const { return stats_; }
  const std::vector<std::vector<std::vector<uint32_t>>> &layers() const { return layers_; }

private:
  int insert_point(const float *base, int64_t point_id, int64_t level);
  int search_layer(const float *query,
                   const std::vector<uint32_t> &entry_points,
                   int64_t ef,
                   int64_t layer,
                   std::vector<std::pair<float, uint32_t>> &result) const;
  int select_neighbours(std::vector<std::pair<float, uint32_t>> &candidates, int64_t max_count) const;

  const float *distance(const float *query, const uint32_t *ids, int64_t count, float *out) const;

  BuildParams params_;
  DistanceBackend *backend_ = nullptr;
  const float *base_ = nullptr;
  int64_t rows_ = 0;
  int64_t entry_point_ = -1;
  int64_t max_level_ = -1;
  std::vector<std::vector<std::vector<uint32_t>>> layers_;
  mutable BuildStats stats_;
  mutable std::vector<float> scratch_dist_;
};

}  // namespace hipvs