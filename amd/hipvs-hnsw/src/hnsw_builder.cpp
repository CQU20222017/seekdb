/**
 * Copyright (c) 2025 OceanBase
 * hipVS accelerated HNSW build for seekdb - layered graph builder implementation
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

#include "hnsw_builder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <queue>
#include <random>

namespace hipvs {
namespace {

/// Deterministic level sampler: level = floor(-ln(u) * mL), mL = 1 / ln(M).
int sample_level(uint64_t point_id, int m)
{
  // splitmix64 gives a stable uniform value per point id, so a rebuild of the same
  // data set always produces the same graph (the benchmark relies on that).
  uint64_t z = point_id + 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  const double u = (static_cast<double>(z >> 11) + 0.5) / 9007199254740992.0;
  const double ml = 1.0 / std::log(static_cast<double>(m));
  return static_cast<int>(-std::log(u) * ml);
}

struct MinHeapCmp {
  bool operator()(const std::pair<float, uint32_t> &a, const std::pair<float, uint32_t> &b) const
  {
    return a.first > b.first;
  }
};

struct MaxHeapCmp {
  bool operator()(const std::pair<float, uint32_t> &a, const std::pair<float, uint32_t> &b) const
  {
    return a.first < b.first;
  }
};

}  // namespace

HnswBuilder::HnswBuilder(const BuildParams &params, DistanceBackend *backend)
    : params_(params), backend_(backend)
{
}

const float *HnswBuilder::distance(const float *query, const uint32_t *ids, int64_t count, float *out) const
{
  if (nullptr == backend_ || count <= 0) {
    return out;
  }
  if (backend_->compute(query, base_, ids, count, params_.dim, params_.metric, out) != 0) {
    // A backend failure (for example a lost device) degrades to the CPU reference
    // implementation instead of aborting the build.
    static thread_local std::unique_ptr<DistanceBackend> fallback = make_cpu_backend();
    fallback->compute(query, base_, ids, count, params_.dim, params_.metric, out);
  }
  stats_.backend_calls += 1;
  stats_.distance_evaluations += count;
  return out;
}

int HnswBuilder::search_layer(const float *query,
                              const std::vector<uint32_t> &entry_points,
                              int64_t ef,
                              int64_t layer,
                              std::vector<std::pair<float, uint32_t>> &result) const
{
  int ret = 0;
  if (nullptr == base_ || layer < 0 || static_cast<size_t>(layer) >= layers_.size()) {
    ret = -1;
  } else {
    std::vector<uint8_t> visited(static_cast<size_t>(rows_), 0);
    std::priority_queue<std::pair<float, uint32_t>,
                        std::vector<std::pair<float, uint32_t>>,
                        MinHeapCmp>
        candidates;  // nearest first
    std::priority_queue<std::pair<float, uint32_t>,
                        std::vector<std::pair<float, uint32_t>>,
                        MaxHeapCmp>
        best;  // farthest first

    std::vector<uint32_t> ids;
    std::vector<float> dists;
    for (const uint32_t ep : entry_points) {
      if (ep >= static_cast<uint32_t>(rows_) || visited[ep]) {
        continue;
      }
      ids.clear();
      ids.push_back(ep);
      dists.assign(1, 0.0f);
      distance(query, ids.data(), 1, dists.data());
      visited[ep] = 1;
      candidates.emplace(dists[0], ep);
      best.emplace(dists[0], ep);
    }
    while (!candidates.empty()) {
      const std::pair<float, uint32_t> current = candidates.top();
      candidates.pop();
      if (!best.empty() && current.first > best.top().first && static_cast<int64_t>(best.size()) >= ef) {
        break;
      }
      const std::vector<uint32_t> &neighbours = layers_[static_cast<size_t>(layer)][current.second];
      ids.clear();
      ids.reserve(neighbours.size());
      for (const uint32_t nb : neighbours) {
        if (nb < static_cast<uint32_t>(rows_) && !visited[nb]) {
          visited[nb] = 1;
          ids.push_back(nb);
        }
      }
      if (ids.empty()) {
        continue;
      }
      // The whole neighbour set of one node is scored in a single backend call: on the
      // GPU this is one kernel launch over up to 2 * M rows.
      dists.assign(ids.size(), 0.0f);
      distance(query, ids.data(), static_cast<int64_t>(ids.size()), dists.data());
      for (size_t i = 0; i < ids.size(); ++i) {
        const float d = dists[i];
        if (static_cast<int64_t>(best.size()) < ef || d < best.top().first) {
          candidates.emplace(d, ids[i]);
          best.emplace(d, ids[i]);
          if (static_cast<int64_t>(best.size()) > ef) {
            best.pop();
          }
        }
      }
    }
    result.clear();
    result.reserve(best.size());
    while (!best.empty()) {
      result.push_back(best.top());
      best.pop();
    }
    std::sort(result.begin(), result.end());
  }
  return ret;
}

int HnswBuilder::select_neighbours(std::vector<std::pair<float, uint32_t>> &candidates, int64_t max_count) const
{
  std::sort(candidates.begin(), candidates.end());
  if (static_cast<int64_t>(candidates.size()) <= max_count) {
    return 0;
  }
  // Plain top-M truncation builds long range edges to everything nearby and makes the
  // graph hard to navigate. Apply the usual diversity rule instead: a candidate is kept
  // only when it is closer to the query than to every already kept neighbour. Rejected
  // candidates are dropped, which is what keeps clusters connected.
  std::vector<std::pair<float, uint32_t>> kept;
  kept.reserve(static_cast<size_t>(max_count));
  std::vector<uint32_t> kept_ids;
  kept_ids.reserve(static_cast<size_t>(max_count));
  for (const auto &candidate : candidates) {
    if (static_cast<int64_t>(kept.size()) >= max_count) {
      break;
    }
    bool good = true;
    if (!kept_ids.empty()) {
      const float *cand_row = base_ + static_cast<int64_t>(candidate.second) * params_.dim;
      std::vector<float> to_kept(kept_ids.size(), 0.0f);
      distance(cand_row, kept_ids.data(), static_cast<int64_t>(kept_ids.size()), to_kept.data());
      for (size_t i = 0; i < kept_ids.size() && good; ++i) {
        good = (to_kept[i] > candidate.first);
      }
    }
    if (good) {
      kept.push_back(candidate);
      kept_ids.push_back(candidate.second);
    }
  }
  // Fall back to plain truncation when the rule rejected too much (mirrors what the
  // reference implementations do when the graph is still tiny).
  if (static_cast<int64_t>(kept.size()) < max_count && static_cast<int64_t>(candidates.size()) > static_cast<int64_t>(kept.size())) {
    for (const auto &candidate : candidates) {
      if (static_cast<int64_t>(kept.size()) >= max_count) {
        break;
      }
      if (std::find(kept_ids.begin(), kept_ids.end(), candidate.second) == kept_ids.end()) {
        kept.push_back(candidate);
        kept_ids.push_back(candidate.second);
      }
    }
  }
  candidates.swap(kept);
  return 0;
}

int HnswBuilder::insert_point(const float *base, int64_t point_id, int64_t level)
{
  int ret = 0;
  const float *query = base + point_id * params_.dim;
  if (entry_point_ < 0) {
    entry_point_ = point_id;
    max_level_ = level;
  } else {
    std::vector<uint32_t> entry(1, static_cast<uint32_t>(entry_point_));
    for (int64_t layer = max_level_; ret == 0 && layer > level; --layer) {
      std::vector<std::pair<float, uint32_t>> nearest;
      if (search_layer(query, entry, 1, layer, nearest) != 0) {
        ret = -1;
      } else if (!nearest.empty()) {
        entry.assign(1, nearest.front().second);
      }
    }
    for (int64_t layer = std::min(level, max_level_); ret == 0 && layer >= 0; --layer) {
      std::vector<std::pair<float, uint32_t>> nearest;
      if (search_layer(query, entry, params_.ef_construction, layer, nearest) != 0) {
        ret = -1;
      } else {
        std::vector<std::pair<float, uint32_t>> selected = nearest;
        select_neighbours(selected, layer == 0 ? 2 * params_.m : params_.m);
        std::vector<uint32_t> &links = layers_[static_cast<size_t>(layer)][point_id];
        links.clear();
        for (const auto &item : selected) {
          links.push_back(item.second);
        }
        // add the reverse edges, keeping the per node degree bounded
        for (const auto &item : selected) {
          std::vector<uint32_t> &back_links = layers_[static_cast<size_t>(layer)][item.second];
          const size_t limit = static_cast<size_t>(layer == 0 ? 2 * params_.m : params_.m);
          bool present = false;
          for (size_t i = 0; i < back_links.size() && !present; ++i) {
            present = (back_links[i] == static_cast<uint32_t>(point_id));
          }
          if (!present) {
            back_links.push_back(static_cast<uint32_t>(point_id));
          }
          if (back_links.size() > limit) {
            // Prune the owner's list by distance to the owner instead of dropping the tail:
            // truncating the tail would discard every freshly added bridge and leave the
            // early nodes as a closed clique that later points can never navigate back to.
            // All candidates of one owner are scored in a single backend call, which is the
            // shape the hipVS kernel wants (one row pointer, many rows).
            const float *owner = base + item.second * params_.dim;
            std::vector<float> owner_dists(back_links.size(), 0.0f);
            distance(owner, back_links.data(), static_cast<int64_t>(back_links.size()), owner_dists.data());
            std::vector<std::pair<float, uint32_t>> scored;
            scored.reserve(back_links.size());
            for (size_t i = 0; i < back_links.size(); ++i) {
              scored.emplace_back(owner_dists[i], back_links[i]);
            }
            std::sort(scored.begin(), scored.end());
            scored.resize(limit);
            back_links.clear();
            for (const auto &entry_link : scored) {
              back_links.push_back(entry_link.second);
            }
          }
        }
        if (!nearest.empty()) {
          entry.clear();
          for (const auto &item : nearest) {
            entry.push_back(item.second);
          }
        }
      }
    }
    if (ret == 0 && level > max_level_) {
      max_level_ = level;
      entry_point_ = point_id;
    }
  }
  return ret;
}

int HnswBuilder::build(const float *base, int64_t rows)
{
  int ret = 0;
  if (nullptr == base || rows <= 0 || params_.dim <= 0 || nullptr == backend_) {
    ret = -1;
  } else {
    const auto started = std::chrono::steady_clock::now();
    base_ = base;
    rows_ = rows;
    stats_ = BuildStats();
    stats_.rows = rows;
    entry_point_ = -1;
    max_level_ = -1;
    layers_.clear();

    if (backend_->prepare(base, rows, params_.dim, params_.metric) != 0) {
      ret = -1;
    }

    // First pass: levels, so the layer vector can be sized once.
    std::vector<int> levels(static_cast<size_t>(rows));
    int64_t top = 0;
    for (int64_t i = 0; ret == 0 && i < rows; ++i) {
      levels[static_cast<size_t>(i)] = sample_level(static_cast<uint64_t>(i), params_.m);
      top = std::max<int64_t>(top, levels[static_cast<size_t>(i)]);
    }
    if (ret == 0) {
      layers_.resize(static_cast<size_t>(top + 1));
      for (auto &layer : layers_) {
        layer.resize(static_cast<size_t>(rows));
      }
      for (int64_t i = 0; ret == 0 && i < rows; ++i) {
        ret = insert_point(base, i, levels[static_cast<size_t>(i)]);
      }
    }
    stats_.max_level = top;
    backend_->release();
    const auto finished = std::chrono::steady_clock::now();
    stats_.build_seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(finished - started).count();
  }
  return ret;
}

int HnswBuilder::search(const float *query, int k, int ef_search, std::vector<uint32_t> &result) const
{
  int ret = 0;
  result.clear();
  if (nullptr == query || k <= 0 || entry_point_ < 0 || layers_.empty()) {
    ret = -1;
  } else {
    std::vector<uint32_t> entry(1, static_cast<uint32_t>(entry_point_));
    for (int64_t layer = max_level_; ret == 0 && layer > 0; --layer) {
      std::vector<std::pair<float, uint32_t>> nearest;
      if (search_layer(query, entry, 1, layer, nearest) != 0) {
        ret = -1;
      } else if (!nearest.empty()) {
        entry.assign(1, nearest.front().second);
      }
    }
    std::vector<std::pair<float, uint32_t>> nearest;
    if (ret == 0) {
      ret = search_layer(query, entry, std::max<int64_t>(ef_search, k), 0, nearest);
    }
    if (ret == 0) {
      for (int64_t i = 0; i < k && static_cast<size_t>(i) < nearest.size(); ++i) {
        result.push_back(nearest[static_cast<size_t>(i)].second);
      }
    }
  }
  return ret;
}

}  // namespace hipvs