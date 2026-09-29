#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
pbbsbench_dir="${script_dir}/pbbsbench"
gbbs_dir="${script_dir}/gbbs"
library_dir="${script_dir}/hbf-parlaylib"

if [[ ! -d "${library_dir}/include/parlay" ]]; then
  echo "error: expected Spork Parlay headers at ${library_dir}/include/parlay" >&2
  exit 1
fi

ln -sfn ../hbf-parlaylib "${pbbsbench_dir}/parlaylib"
ln -sfn ../hbf-parlaylib/include/parlay "${pbbsbench_dir}/parlay"

if [[ -d "${gbbs_dir}/benchmarks" ]]; then
  mkdir -p "${gbbs_dir}/external"
  ln -sfn ../../hbf-parlaylib "${gbbs_dir}/external/parlaylib"
fi

echo "PBBS now uses hbf-parlaylib"
echo "  pbbsbench/parlaylib -> ../hbf-parlaylib"
echo "  pbbsbench/parlay    -> ../hbf-parlaylib/include/parlay"
if [[ -d "${gbbs_dir}/benchmarks" ]]; then
  echo "  gbbs/external/parlaylib -> ../../hbf-parlaylib"
fi
