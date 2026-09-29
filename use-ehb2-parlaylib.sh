#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
pbbsbench_dir="${script_dir}/pbbsbench"
gbbs_dir="${script_dir}/gbbs"
library_dir="${script_dir}/ehb2-parlaylib"

if [[ ! -d "${library_dir}/include/parlay" ]]; then
  echo "error: expected Spork Parlay headers at ${library_dir}/include/parlay" >&2
  exit 1
fi

ln -sfn ../ehb2-parlaylib "${pbbsbench_dir}/parlaylib"
ln -sfn ../ehb2-parlaylib/include/parlay "${pbbsbench_dir}/parlay"

if [[ -d "${gbbs_dir}/benchmarks" ]]; then
  mkdir -p "${gbbs_dir}/external"
  ln -sfn ../../ehb2-parlaylib "${gbbs_dir}/external/parlaylib"
fi

echo "PBBS now uses ehb2-parlaylib"
echo "  pbbsbench/parlaylib -> ../ehb2-parlaylib"
echo "  pbbsbench/parlay    -> ../ehb2-parlaylib/include/parlay"
if [[ -d "${gbbs_dir}/benchmarks" ]]; then
  echo "  gbbs/external/parlaylib -> ../../ehb2-parlaylib"
fi
