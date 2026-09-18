#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
pbbsbench_dir="${script_dir}/pbbsbench"
library_dir="${script_dir}/ehbf-parlaylib"

if [[ ! -d "${library_dir}/include/parlay" ]]; then
  echo "error: expected Spork Parlay headers at ${library_dir}/include/parlay" >&2
  exit 1
fi

ln -sfn ../ehbf-parlaylib "${pbbsbench_dir}/parlaylib"
ln -sfn ../ehbf-parlaylib/include/parlay "${pbbsbench_dir}/parlay"

echo "PBBS now uses ehbf-parlaylib"
echo "  pbbsbench/parlaylib -> ../ehbf-parlaylib"
echo "  pbbsbench/parlay    -> ../ehbf-parlaylib/include/parlay"
