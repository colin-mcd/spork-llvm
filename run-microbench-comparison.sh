#!/usr/bin/env bash
# fib(42) and nqueens(14) on the sequential baseline (1 core) and, at 1 and 80
# cores, on spork-parlaylib and the auto-grain builds of ParlayLib's own
# scheduler, OpenMP and OpenCilk (parlaylib-auto, parlaylib-openmp-auto,
# parlaylib-opencilk-auto), with 10 warmup and 20 timed rounds per run.
#
# Runs run-microbench.py, which does the building, pinning, SMT handling and
# report.  Arguments are passed on after these defaults, so single-valued
# options override them (e.g. --warmup 3 --trials 5, --cores 80, --size fib=40,
# --output DIR), --bench fib runs one benchmark, and --library adds a library.

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec python3 "${script_dir}/run-microbench.py" \
  --library parlaylib-sequential \
  --library spork-parlaylib \
  --library parlaylib-auto \
  --library parlaylib-openmp-auto \
  --library parlaylib-opencilk-auto \
  --size fib=42 --size nqueens=14 \
  --cores 1,80 \
  --warmup 10 --trials 20 \
  "$@"
