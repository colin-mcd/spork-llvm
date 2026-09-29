#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
pbbsbench_dir="${script_dir}/pbbsbench"
gbbs_dir="${script_dir}/gbbs"
library_dir="${script_dir}/parlaylib"

if [[ ! -d "${library_dir}/include/parlay" ]]; then
  echo "error: expected Parlay headers at ${library_dir}/include/parlay" >&2
  exit 1
fi

ln -sfn ../parlaylib "${pbbsbench_dir}/parlaylib"
ln -sfn ../parlaylib/include/parlay "${pbbsbench_dir}/parlay"

if [[ -d "${gbbs_dir}/benchmarks" ]]; then
  mkdir -p "${gbbs_dir}/external"
  ln -sfn ../../parlaylib "${gbbs_dir}/external/parlaylib"
fi

echo "PBBS now uses parlaylib"
echo "  pbbsbench/parlaylib -> ../parlaylib"
echo "  pbbsbench/parlay    -> ../parlaylib/include/parlay"
if [[ -d "${gbbs_dir}/benchmarks" ]]; then
  echo "  gbbs/external/parlaylib -> ../../parlaylib"
fi
