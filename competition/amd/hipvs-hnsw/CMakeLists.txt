# Copyright (c) 2025 OceanBase
# Licensed under the Apache License, Version 2.0 (see ../LICENSE).
#
# Standalone build for the hipVS accelerated HNSW builder.
#
#   cmake -S amd/hipvs-hnsw -B build/hipvs -DHIPVS_ENABLED=OFF   # CPU only (default)
#   cmake -S amd/hipvs-hnsw -B build/hipvs -DHIPVS_ENABLED=ON \
#         -DCMAKE_HIP_ARCHITECTURES=gfx1100                      # with hipVS
#
# The target is intentionally standalone: it never becomes part of the seekdb server
# build, so enabling it cannot regress the engine. docs/DESIGN.md describes how the
# same backend plugs into the in-tree build path.

cmake_minimum_required(VERSION 3.20)
project(hipvs_hnsw LANGUAGES CXX)

option(HIPVS_ENABLED "Build the hipVS (HIP) distance backend" OFF)
option(HIPVS_BUILD_BENCH "Build the benchmark driver" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(HIPVS_CORE_SOURCES
    src/distance_cpu.cpp
    src/hnsw_builder.cpp)

add_library(hipvs_hnsw_core STATIC ${HIPVS_CORE_SOURCES})
target_include_directories(hipvs_hnsw_core PUBLIC src)

if(HIPVS_ENABLED)
  enable_language(HIP)
  add_library(hipvs_distance_hip STATIC src/distance_hipvs.hip)
  target_include_directories(hipvs_distance_hip PUBLIC src)
  target_compile_definitions(hipvs_distance_hip PUBLIC USE_HIPVS=1)
  target_link_libraries(hipvs_hnsw_core PUBLIC hipvs_distance_hip)
  # hipVS ships as a ROCm library; keep it optional so a plain ROCm install (HIP only)
  # still builds with the reference kernels in distance_hipvs.hip.
  find_library(HIPVS_LIBRARY hipvs PATHS /opt/rocm/lib /opt/rocm/lib64)
  if(HIPVS_LIBRARY)
    target_link_libraries(hipvs_distance_hip PUBLIC ${HIPVS_LIBRARY})
  else()
    message(WARNING "libhipvs not found, building with the reference HIP kernels only")
  endif()
endif()

if(HIPVS_BUILD_BENCH)
  add_executable(hipvs_bench src/bench_main.cpp)
  target_link_libraries(hipvs_bench PRIVATE hipvs_hnsw_core)
  if(HIPVS_ENABLED)
    target_compile_definitions(hipvs_bench PRIVATE USE_HIPVS=1)
  endif()
endif()