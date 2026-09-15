#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
pbbsbench_dir="${script_dir}/pbbsbench"
library_dir="${script_dir}/ehb2-parlaylib"

if [[ ! -d "${library_dir}/include/parlay" ]]; then
  echo "error: expected Spork Parlay headers at ${library_dir}/include/parlay" >&2
  exit 1
fi

ln -sfn ../ehb2-parlaylib "${pbbsbench_dir}/parlaylib"
ln -sfn ../ehb2-parlaylib/include/parlay "${pbbsbench_dir}/parlay"

echo "PBBS now uses ehb2-parlaylib"
echo "  pbbsbench/parlaylib -> ../ehb2-parlaylib"
echo "  pbbsbench/parlay    -> ../ehb2-parlaylib/include/parlay"
