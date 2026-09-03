#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
pbbsbench_dir="${script_dir}/pbbsbench"
library_dir="${script_dir}/parlaylib"

if [[ ! -d "${library_dir}/include/parlay" ]]; then
  echo "error: expected Parlay headers at ${library_dir}/include/parlay" >&2
  exit 1
fi

ln -sfn ../parlaylib "${pbbsbench_dir}/parlaylib"
ln -sfn ../parlaylib/include/parlay "${pbbsbench_dir}/parlay"

echo "PBBS now uses parlaylib"
echo "  pbbsbench/parlaylib -> ../parlaylib"
echo "  pbbsbench/parlay    -> ../parlaylib/include/parlay"
