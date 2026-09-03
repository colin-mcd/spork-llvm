#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
pbbsbench_dir="${script_dir}/pbbsbench"
library_dir="${script_dir}/hb-parlaylib"

if [[ ! -d "${library_dir}/include/parlay" ]]; then
  echo "error: expected Spork Parlay headers at ${library_dir}/include/parlay" >&2
  exit 1
fi

ln -sfn ../hb-parlaylib "${pbbsbench_dir}/parlaylib"
ln -sfn ../hb-parlaylib/include/parlay "${pbbsbench_dir}/parlay"

echo "PBBS now uses hb-parlaylib"
echo "  pbbsbench/parlaylib -> ../hb-parlaylib"
echo "  pbbsbench/parlay    -> ../hb-parlaylib/include/parlay"
