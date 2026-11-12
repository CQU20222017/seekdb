#!/usr/bin/env bash
# Copyright (c) 2025 OceanBase
# Licensed under the Apache License, Version 2.0 (see ../../LICENSE).
#
# Thin wrapper kept for runners that look for a script next to the benchmark
# driver instead of at tools/benchmark/hipvs_bench.sh.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
exec bash "$ROOT/tools/benchmark/hipvs_bench.sh" "$@"