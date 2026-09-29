#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
pbbsbench_dir="${script_dir}/pbbsbench"
gbbs_dir="${script_dir}/gbbs"
library_dir="${script_dir}/pf-parlaylib"

if [[ ! -d "${library_dir}/include/parlay" ]]; then
  echo "error: expected Spork Parlay headers at ${library_dir}/include/parlay" >&2
  exit 1
fi

ln -sfn ../pf-parlaylib "${pbbsbench_dir}/parlaylib"
ln -sfn ../pf-parlaylib/include/parlay "${pbbsbench_dir}/parlay"

if [[ -d "${gbbs_dir}/benchmarks" ]]; then
  mkdir -p "${gbbs_dir}/external"
  ln -sfn ../../pf-parlaylib "${gbbs_dir}/external/parlaylib"
fi

echo "PBBS now uses pf-parlaylib"
echo "  pbbsbench/parlaylib -> ../pf-parlaylib"
echo "  pbbsbench/parlay    -> ../pf-parlaylib/include/parlay"
if [[ -d "${gbbs_dir}/benchmarks" ]]; then
  echo "  gbbs/external/parlaylib -> ../../pf-parlaylib"
fi
