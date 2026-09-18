#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
pbbsbench_dir="${script_dir}/pbbsbench"
library_dir="${script_dir}/hbf-parlaylib"

if [[ ! -d "${library_dir}/include/parlay" ]]; then
  echo "error: expected Spork Parlay headers at ${library_dir}/include/parlay" >&2
  exit 1
fi

ln -sfn ../hbf-parlaylib "${pbbsbench_dir}/parlaylib"
ln -sfn ../hbf-parlaylib/include/parlay "${pbbsbench_dir}/parlay"

echo "PBBS now uses hbf-parlaylib"
echo "  pbbsbench/parlaylib -> ../hbf-parlaylib"
echo "  pbbsbench/parlay    -> ../hbf-parlaylib/include/parlay"
