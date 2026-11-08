/**
 * Copyright (c) 2025 OceanBase
 * hipVS accelerated HNSW build for seekdb - benchmark driver
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

// Builds an HNSW index over a synthetic data set, reports build time / distance
// throughput and checks the resulting graph against exact search (recall@k).
//
// usage: hipvs_bench [--rows N] [--dim D] [--m M] [--ef-construction E]
//                    [--queries Q] [--k K] [--ef-search S] [--backend cpu|hipvs]
//                    [--metric l2|ip|cosine] [--json out.json]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "distance_backend.h"
#include "hnsw_builder.h"

namespace {

struct Options {
  int64_t rows = 20000;
  int64_t dim = 128;
  int m = 16;
  int ef_construction = 100;
  int queries = 200;
  int k = 10;
  int ef_search = 64;
  std::string backend = "cpu";
  std::string metric = "l2";
  std::string json_path;
};

hipvs::Metric parse_metric(const std::string &name)
{
  if ("ip" == name || "inner" == name) {
    return hipvs::Metric::INNER;
  }
  if ("cosine" == name) {
    return hipvs::Metric::COSINE;
  }
  return hipvs::Metric::L2;
}

void usage()
{
  std::printf("hipvs_bench [--rows N] [--dim D] [--m M] [--ef-construction E] [--queries Q]\n"
              "            [--k K] [--ef-search S] [--backend cpu|hipvs] [--metric l2|ip|cosine]\n"
              "            [--json out.json]\n");
}

bool parse_args(int argc, char **argv, Options &opt)
{
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = (i + 1) < argc;
    if ("--rows" == arg && has_value) {
      opt.rows = std::atoll(argv[++i]);
    } else if ("--dim" == arg && has_value) {
      opt.dim = std::atoll(argv[++i]);
    } else if ("--m" == arg && has_value) {
      opt.m = std::atoi(argv[++i]);
    } else if ("--ef-construction" == arg && has_value) {
      opt.ef_construction = std::atoi(argv[++i]);
    } else if ("--queries" == arg && has_value) {
      opt.queries = std::atoi(argv[++i]);
    } else if ("--k" == arg && has_value) {
      opt.k = std::atoi(argv[++i]);
    } else if ("--ef-search" == arg && has_value) {
      opt.ef_search = std::atoi(argv[++i]);
    } else if ("--backend" == arg && has_value) {
      opt.backend = argv[++i];
    } else if ("--metric" == arg && has_value) {
      opt.metric = argv[++i];
    } else if ("--json" == arg && has_value) {
      opt.json_path = argv[++i];
    } else if ("--help" == arg || "-h" == arg) {
      usage();
      return false;
    } else {
      std::printf("unknown or incomplete option: %s\n", arg.c_str());
      usage();
      return false;
    }
  }
  return true;
}

std::vector<float> make_vectors(int64_t rows, int64_t dim, uint32_t seed, bool normalize)
{
  std::mt19937 rng(seed);
  std::normal_distribution<float> gauss(0.0f, 1.0f);
  std::vector<float> data(static_cast<size_t>(rows * dim));
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = gauss(rng);
  }
  if (normalize) {
    for (int64_t r = 0; r < rows; ++r) {
      float *row = data.data() + r * dim;
      float norm = 0.0f;
      for (int64_t d = 0; d < dim; ++d) {
        norm += row[d] * row[d];
      }
      norm = std::sqrt(norm);
      if (norm > 0.0f) {
        for (int64_t d = 0; d < dim; ++d) {
          row[d] /= norm;
        }
      }
    }
  }
  return data;
}

/// Exact k nearest neighbours, used only to score recall of the graph.
void exact_knn(const float *base,
               int64_t rows,
               int64_t dim,
               hipvs::Metric metric,
               const float *query,
               int k,
               std::vector<uint32_t> &out)
{
  std::vector<std::pair<float, uint32_t>> all;
  all.reserve(static_cast<size_t>(rows));
  for (int64_t r = 0; r < rows; ++r) {
    const float *row = base + r * dim;
    float acc = 0.0f;
    if (hipvs::Metric::L2 == metric) {
      for (int64_t d = 0; d < dim; ++d) {
        const float diff = query[d] - row[d];
        acc += diff * diff;
      }
    } else if (hipvs::Metric::INNER == metric) {
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
      const float denom = std::sqrt(na) * std::sqrt(nb);
      acc = denom > 0.0f ? 1.0f - acc / denom : 0.0f;
    }
    all.emplace_back(acc, static_cast<uint32_t>(r));
  }
  const size_t kk = std::min<size_t>(static_cast<size_t>(k), all.size());
  std::partial_sort(all.begin(), all.begin() + kk, all.end());
  out.clear();
  for (size_t i = 0; i < kk; ++i) {
    out.push_back(all[i].second);
  }
}

}  // namespace

int main(int argc, char **argv)
{
  Options opt;
  if (!parse_args(argc, argv, opt)) {
    return 1;
  }
  const hipvs::Metric metric = parse_metric(opt.metric);
  const bool normalize = hipvs::Metric::COSINE == metric;

  std::printf("== hipVS HNSW build benchmark ==\n");
  std::printf("rows=%lld dim=%lld m=%d ef_construction=%d metric=%s backend=%s\n",
              static_cast<long long>(opt.rows),
              static_cast<long long>(opt.dim),
              opt.m,
              opt.ef_construction,
              opt.metric.c_str(),
              opt.backend.c_str());

  std::vector<float> base = make_vectors(opt.rows, opt.dim, 20261002u, normalize);
  std::vector<float> queries = make_vectors(opt.queries, opt.dim, 20261003u, normalize);

  std::unique_ptr<hipvs::DistanceBackend> backend;
  if ("hipvs" == opt.backend) {
#if defined(USE_HIPVS)
    backend = hipvs::make_hipvs_backend(0, opt.rows, opt.dim, metric);
    if (nullptr == backend) {
      std::printf("[warn] no AMD device visible, falling back to the CPU backend\n");
    }
#else
    std::printf("[warn] built without hipVS, falling back to the CPU backend\n");
#endif
  }
  if (nullptr == backend) {
    backend = hipvs::make_cpu_backend();
  }
  std::printf("effective backend=%s (gpu=%d)\n", backend->name(), backend->is_gpu() ? 1 : 0);

  hipvs::BuildParams params;
  params.dim = opt.dim;
  params.m = opt.m;
  params.ef_construction = opt.ef_construction;
  params.metric = metric;
  hipvs::HnswBuilder builder(params, backend.get());
  if (builder.build(base.data(), opt.rows) != 0) {
    std::printf("[error] build failed\n");
    return 2;
  }
  const hipvs::BuildStats &stats = builder.stats();
  const double pairs_per_second =
      stats.build_seconds > 0.0 ? static_cast<double>(stats.distance_evaluations) / stats.build_seconds : 0.0;
  std::printf("build_seconds=%.4f distance_evaluations=%lld backend_calls=%lld pairs_per_second=%.1f\n",
              stats.build_seconds,
              static_cast<long long>(stats.distance_evaluations),
              static_cast<long long>(stats.backend_calls),
              pairs_per_second);
  std::printf("max_level=%lld\n", static_cast<long long>(stats.max_level));

  {
    // graph sanity: degree distribution of the bottom layer, isolated nodes are the
    // quickest way to spot a fragmenting build
    const auto &layers = builder.layers();
    if (!layers.empty()) {
      const auto &base_layer = layers.front();
      size_t total_degree = 0;
      size_t isolated = 0;
      size_t max_degree = 0;
      for (const auto &links : base_layer) {
        total_degree += links.size();
        if (links.empty()) {
          ++isolated;
        }
        max_degree = std::max(max_degree, links.size());
      }
      std::printf("layer0_avg_degree=%.2f isolated=%zu max_degree=%zu\n",
                  base_layer.empty() ? 0.0 : static_cast<double>(total_degree) / static_cast<double>(base_layer.size()),
                  isolated,
                  max_degree);
    }
  }

  int hits = 0;
  int total = 0;
  for (int q = 0; q < opt.queries; ++q) {
    const float *query = queries.data() + static_cast<int64_t>(q) * opt.dim;
    std::vector<uint32_t> approx;
    std::vector<uint32_t> exact;
    if (builder.search(query, opt.k, opt.ef_search, approx) != 0) {
      std::printf("[error] search failed\n");
      return 3;
    }
    exact_knn(base.data(), opt.rows, opt.dim, metric, query, opt.k, exact);
    for (const uint32_t id : approx) {
      if (std::find(exact.begin(), exact.end(), id) != exact.end()) {
        ++hits;
      }
    }
    total += opt.k;
  }
  const double recall = total > 0 ? static_cast<double>(hits) / static_cast<double>(total) : 0.0;
  std::printf("recall@%d=%.4f queries=%d ef_search=%d\n", opt.k, recall, opt.queries, opt.ef_search);

  if (!opt.json_path.empty()) {
    FILE *fp = std::fopen(opt.json_path.c_str(), "w");
    if (nullptr != fp) {
      std::fprintf(fp,
                   "{\n"
                   "  \"rows\": %lld,\n"
                   "  \"dim\": %lld,\n"
                   "  \"m\": %d,\n"
                   "  \"ef_construction\": %d,\n"
                   "  \"metric\": \"%s\",\n"
                   "  \"backend\": \"%s\",\n"
                   "  \"gpu\": %d,\n"
                   "  \"build_seconds\": %.6f,\n"
                   "  \"distance_evaluations\": %lld,\n"
                   "  \"backend_calls\": %lld,\n"
                   "  \"pairs_per_second\": %.1f,\n"
                   "  \"max_level\": %lld,\n"
                   "  \"recall_at_k\": %.6f,\n"
                   "  \"k\": %d,\n"
                   "  \"ef_search\": %d,\n"
                   "  \"queries\": %d\n"
                   "}\n",
                   static_cast<long long>(opt.rows),
                   static_cast<long long>(opt.dim),
                   opt.m,
                   opt.ef_construction,
                   opt.metric.c_str(),
                   backend->name(),
                   backend->is_gpu() ? 1 : 0,
                   stats.build_seconds,
                   static_cast<long long>(stats.distance_evaluations),
                   static_cast<long long>(stats.backend_calls),
                   pairs_per_second,
                   static_cast<long long>(stats.max_level),
                   recall,
                   opt.k,
                   opt.ef_search,
                   opt.queries);
      std::fclose(fp);
      std::printf("json=%s\n", opt.json_path.c_str());
    }
  }
  return 0;
}