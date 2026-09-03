#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
pbbs_dir="${script_dir}/pbbsbench"
clangxx="${script_dir}/llvm-project/build/bin/clang++"
pass_name=fablepass
plugin=""
only_pattern=""
startup_header="${pbbs_dir}/common/spork_benchmark_startup.h"

cpu_list=12-19
workers=8
build_jobs=8
rounds=1
input_driver=testInputs_small
run_timeout=900
build_timeout=900
check_outputs=0
dry_run=0
library_selection=both
timestamp="$(date +%Y%m%d-%H%M%S)"
output_dir="${script_dir}/pbbsbench-results/${timestamp}"

usage() {
  cat <<'EOF'
Usage: ./run-pbbsbench-comparison.sh [options]

Build and run every target in PBBS's ALL_BENCHMARKS list with both stock
parlaylib and hb-parlaylib. Builds, benchmark processes, correctness
checkers, and PBBS data generation are restricted to logical CPUs 12-19.

Options:
  --parlaylib-only       Build and run only stock parlaylib targets.
  --hb-parlaylib-only Build and run only hb-parlaylib targets.
  --full                 Use testInputs instead of testInputs_small.
  --input-driver NAME    Use the driver script NAME in each benchmark
                         directory (overrides --full).
  --rounds N             PBBS timing rounds per input (default: 1).
  --workers N            Worker count, from 1 through 8 (default: 8).
  --build-jobs N         Concurrent builds, from 1 through 8 (default: 8).
  --check                Enable output generation and correctness checkers.
  --no-check             Disable correctness checking (the default).
  --output DIR           Report/log directory.
  --run-timeout SEC      Timeout per target run (default: 900).
  --build-timeout SEC    Timeout per target build (default: 900).
  --pass NAME            Spork unroll plugin to use: fablepass (default) or
                         gempass (uses NAME/build/SporkUnroll.so).
  --only PATTERN         Only build/run benchmarks matching this shell glob
                         (e.g. 'suffixArray/*'). May be repeated.
  --dry-run              Print the discovered targets without building.
  -h, --help             Show this help.

Outputs:
  results.tsv            Machine-readable aggregate results.
  report.md              Markdown summary and failure report.
  logs/<library>/...     Complete build and run logs for each target.
EOF
}

require_positive_integer() {
  local option="$1"
  local value="$2"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "error: ${option} requires a positive integer" >&2
    exit 2
  fi
}

while (($#)); do
  case "$1" in
    --parlaylib-only)
      if [[ "${library_selection}" == hb-parlaylib ]]; then
        echo "error: --parlaylib-only and --hb-parlaylib-only are mutually exclusive" >&2
        exit 2
      fi
      library_selection=parlaylib
      shift
      ;;
    --hb-parlaylib-only)
      if [[ "${library_selection}" == parlaylib ]]; then
        echo "error: --parlaylib-only and --hb-parlaylib-only are mutually exclusive" >&2
        exit 2
      fi
      library_selection=hb-parlaylib
      shift
      ;;
    --full)
      input_driver=testInputs
      shift
      ;;
    --pass)
      if (($# < 2)); then
        echo "error: missing value for $1" >&2
        exit 2
      fi
      pass_name="$2"
      shift 2
      ;;
    --only)
      if (($# < 2)); then
        echo "error: missing value for $1" >&2
        exit 2
      fi
      only_pattern="${only_pattern:+${only_pattern}|}$2"
      shift 2
      ;;
    --input-driver)
      if (($# < 2)); then
        echo "error: missing value for $1" >&2
        exit 2
      fi
      input_driver="$2"
      shift 2
      ;;
    --rounds|--workers|--build-jobs|--output|--run-timeout|--build-timeout)
      if (($# < 2)); then
        echo "error: missing value for $1" >&2
        exit 2
      fi
      option="$1"
      value="$2"
      case "${option}" in
        --rounds)
          require_positive_integer "${option}" "${value}"
          rounds="${value}"
          ;;
        --workers)
          require_positive_integer "${option}" "${value}"
          workers="${value}"
          ;;
        --build-jobs)
          require_positive_integer "${option}" "${value}"
          build_jobs="${value}"
          ;;
        --output)
          output_dir="$(realpath -m -- "${value}")"
          ;;
        --run-timeout)
          require_positive_integer "${option}" "${value}"
          run_timeout="${value}"
          ;;
        --build-timeout)
          require_positive_integer "${option}" "${value}"
          build_timeout="${value}"
          ;;
      esac
      shift 2
      ;;
    --no-check)
      check_outputs=0
      shift
      ;;
    --check)
      check_outputs=1
      shift
      ;;
    --dry-run)
      dry_run=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if ((workers > 8)); then
  echo "error: --workers cannot exceed the eight allowed CPUs (12-19)" >&2
  exit 2
fi
if ((build_jobs > 8)); then
  echo "error: --build-jobs cannot exceed the eight allowed CPUs (12-19)" >&2
  exit 2
fi

plugin="${script_dir}/${pass_name}/build/SporkUnroll.so"

for required in "${clangxx}" "${script_dir}/use-parlaylib.sh" \
                "${script_dir}/use-hb-parlaylib.sh"; do
  if [[ ! -e "${required}" ]]; then
    echo "error: required file not found: ${required}" >&2
    exit 1
  fi
done

if [[ "${library_selection}" != parlaylib ]]; then
  for required in "${plugin}" "${startup_header}"; do
    if [[ ! -e "${required}" ]]; then
      echo "error: required file not found: ${required}" >&2
      exit 1
    fi
  done
fi

if [[ "${library_selection}" == both ]]; then
  libraries=(parlaylib hb-parlaylib)
else
  libraries=("${library_selection}")
fi

if ! command -v taskset >/dev/null; then
  echo "error: taskset is required" >&2
  exit 1
fi
if ! command -v flock >/dev/null; then
  echo "error: flock is required for safe parallel PBBS builds" >&2
  exit 1
fi
if ! taskset -c "${cpu_list}" true; then
  echo "error: CPUs ${cpu_list} are not available to this process" >&2
  exit 1
fi

benchmark_text="$(make -s --no-print-directory -C "${pbbs_dir}" \
  --eval='print-all-benchmarks: ; @echo $(ALL_BENCHMARKS)' \
  print-all-benchmarks)"
read -r -a all_benchmarks <<<"${benchmark_text}"

benchmarks=()
for benchmark in "${all_benchmarks[@]}"; do
  if [[ -n "${only_pattern}" ]]; then
    matched=0
    IFS='|' read -r -a only_globs <<<"${only_pattern}"
    for glob in "${only_globs[@]}"; do
      # shellcheck disable=SC2053
      if [[ "${benchmark}" == ${glob} ]]; then
        matched=1
        break
      fi
    done
    ((matched)) || continue
  fi
  benchmarks+=("${benchmark}")
done

if ((${#benchmarks[@]} == 0)); then
  echo "error: no PBBS benchmarks selected" >&2
  exit 1
fi

if ((dry_run)); then
  printf 'CPU affinity: %s\nWorkers: %s\nInput driver: %s\n' \
    "${cpu_list}" "${workers}" "${input_driver}"
  printf 'Libraries: %s\n' "${libraries[*]}"
  printf 'Spork plugin: %s\n' "${plugin}"
  printf 'Parallel build jobs: %s\n' "${build_jobs}"
  printf '%s\n' "${benchmarks[@]}"
  exit 0
fi

if [[ -e "${output_dir}/results.tsv" || -e "${output_dir}/report.md" ]]; then
  echo "error: output directory already contains a report: ${output_dir}" >&2
  exit 1
fi

for library in "${libraries[@]}"; do
  mkdir -p "${output_dir}/logs/${library}"
done
results_file="${output_dir}/results.tsv"
report_file="${output_dir}/report.md"
printf 'library\tbenchmark\tstatus\tgeomean_min_seconds\tgeomean_seconds\tnote\n' >"${results_file}"

initial_parlaylib_target="$(readlink "${pbbs_dir}/parlaylib" 2>/dev/null || true)"
finalized=0

restore_initial_library() {
  case "${initial_parlaylib_target}" in
    ../parlaylib)
      "${script_dir}/use-parlaylib.sh" >/dev/null
      ;;
    ../hb-parlaylib)
      "${script_dir}/use-hb-parlaylib.sh" >/dev/null
      ;;
  esac
}

write_report() {
  if ((finalized)); then
    return
  fi
  finalized=1

  {
    echo '# PBBS Parlay comparison'
    echo
    echo "- CPUs: \`${cpu_list}\` (enforced with \`taskset\`)"
    echo "- Workers: \`${workers}\`"
    echo "- Parallel build jobs: \`${build_jobs}\`"
    echo "- Inputs: \`${input_driver}\`"
    echo "- Rounds per input: \`${rounds}\`"
    echo "- Libraries: \`${libraries[*]}\`"
    echo "- Spork plugin: \`${pass_name}\`"
    if ((check_outputs)); then
      echo '- Correctness checking: enabled'
    else
      echo '- Correctness checking: disabled'
    fi
    echo
    echo '| Library | Benchmark | Status | Geomean min (s) | Geomean (s) | Note |'
    echo '|---|---|---:|---:|---:|---|'
    awk -F '\t' 'NR > 1 {
      min = ($4 == "" ? "—" : $4)
      mean = ($5 == "" ? "—" : $5)
      note = ($6 == "" ? "" : $6)
      gsub(/\|/, "\\|", note)
      printf "| %s | %s | %s | %s | %s | %s |\n", $1, $2, $3, min, mean, note
    }' "${results_file}"
    echo
    echo 'Complete compiler and benchmark output is available in `logs/`.'
  } >"${report_file}"

  restore_initial_library
  echo "Results: ${results_file}"
  echo "Report:  ${report_file}"
}

trap write_report EXIT
trap 'exit 130' INT TERM

record_result() {
  local library="$1"
  local benchmark="$2"
  local status="$3"
  local geomean_min="$4"
  local geomean="$5"
  local note="$6"
  note="${note//$'\t'/ }"
  note="${note//$'\n'/ }"
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
    "${library}" "${benchmark}" "${status}" \
    "${geomean_min}" "${geomean}" "${note}" >>"${results_file}"
}

select_library() {
  local library="$1"

  if [[ "${library}" == parlaylib ]]; then
    "${script_dir}/use-parlaylib.sh"
  else
    "${script_dir}/use-hb-parlaylib.sh"
  fi
}

build_one() {
  local library="$1"
  local benchmark="$2"
  local status_dir="$3"
  local cflags="$4"
  local lflags="$5"
  local benchmark_dir="${pbbs_dir}/benchmarks/${benchmark}"
  local log_stem="${benchmark//\//__}"
  local build_log="${output_dir}/logs/${library}/${log_stem}.build.log"
  local status_file="${status_dir}/${log_stem}.status"
  local problem_family="${benchmark%%/*}"
  local family_lock="${output_dir}/build-locks/${library}/${problem_family}.lock"
  local benchmark_cflags="${cflags}"
  local build_status

  # suffixArray's shared timing/checker sources explicitly initialize Spork.
  # Other PBBS executables receive the same one-time startup via this header.
  if [[ "${library}" == hb-parlaylib && "${benchmark}" != suffixArray/* ]]; then
    benchmark_cflags+=" -include ${startup_header}"
  fi

  echo "[${library}] building ${benchmark}"
  # Variants of one PBBS problem share checker binaries in ../bench. Serialize
  # within a problem family while building unrelated families concurrently.
  if flock "${family_lock}" timeout "${build_timeout}s" \
      taskset -c "${cpu_list}" make -B -s -C "${benchmark_dir}" \
      CC="${clangxx}" CFLAGS="${benchmark_cflags}" LFLAGS="${lflags}" \
      >"${build_log}" 2>&1; then
    printf 'PASS\t\n' >"${status_file}"
  else
    build_status=$?
    if ((build_status == 124)); then
      printf 'BUILD_TIMEOUT\tSee logs/%s/%s.build.log\n' \
        "${library}" "${log_stem}" >"${status_file}"
    else
      printf 'BUILD_FAILED\tExit %s; see logs/%s/%s.build.log\n' \
        "${build_status}" "${library}" "${log_stem}" >"${status_file}"
    fi
  fi
}

build_library() {
  local library="$1"
  local cflags
  local lflags='-pthread -ldl -L/usr/local/lib -ljemalloc -stdlib=libc++'
  local status_dir="${output_dir}/build-status/${library}"
  local benchmark
  local status
  local note
  local active_builds=0

  select_library "${library}"
  if [[ "${library}" == parlaylib ]]; then
    cflags='-mcx16 -O3 -g -std=c++20 -DNDEBUG -I . -stdlib=libc++ -w'
  else
    cflags="-mcx16 -O3 -g -std=c++20 -DNDEBUG -I . -stdlib=libc++ -w -fpass-plugin=${plugin}"
  fi

  mkdir -p "${status_dir}" "${output_dir}/build-locks/${library}"
  echo "[${library}] parallel build phase (${build_jobs} jobs)"

  for benchmark in "${benchmarks[@]}"; do
    if ((active_builds >= build_jobs)); then
      wait -n || true
      ((active_builds -= 1))
    fi
    build_one "${library}" "${benchmark}" "${status_dir}" \
      "${cflags}" "${lflags}" &
    ((active_builds += 1))
  done
  wait || true

  # Record build failures only after every build has stopped, ensuring no
  # benchmark starts while another target is still compiling.
  for benchmark in "${benchmarks[@]}"; do
    log_stem="${benchmark//\//__}"
    if [[ ! -f "${status_dir}/${log_stem}.status" ]]; then
      record_result "${library}" "${benchmark}" BUILD_INTERRUPTED '' '' \
        "No build status was produced"
      continue
    fi
    IFS=$'\t' read -r status note <"${status_dir}/${log_stem}.status"
    if [[ "${status}" != PASS ]]; then
      record_result "${library}" "${benchmark}" "${status}" '' '' "${note}"
    fi
  done
}

run_library() {
  local library="$1"
  local status_dir="${output_dir}/build-status/${library}"
  local benchmark
  local status
  local ignored_note

  select_library "${library}"
  echo "[${library}] sequential benchmark phase"

  for benchmark in "${benchmarks[@]}"; do
    local benchmark_dir="${pbbs_dir}/benchmarks/${benchmark}"
    local log_stem="${benchmark//\//__}"
    local run_log="${output_dir}/logs/${library}/${log_stem}.run.log"

    if [[ ! -f "${status_dir}/${log_stem}.status" ]]; then
      continue
    fi
    IFS=$'\t' read -r status ignored_note <"${status_dir}/${log_stem}.status"
    [[ "${status}" == PASS ]] || continue

    if [[ ! -x "${benchmark_dir}/${input_driver}" ]]; then
      record_result "${library}" "${benchmark}" NO_INPUT_DRIVER '' '' \
        "Missing ${input_driver}"
      continue
    fi

    run_args=(-p "${workers}" -r "${rounds}" -k)
    if ((!check_outputs)); then
      run_args+=(-x)
    fi

    echo "[${library}] running ${benchmark} on CPUs ${cpu_list}"
    if (cd "${benchmark_dir}" && \
        timeout "${run_timeout}s" taskset -c "${cpu_list}" \
          env PARLAY_NUM_THREADS="${workers}" \
          "./${input_driver}" "${run_args[@]}" >"${run_log}" 2>&1 </dev/null); then
      run_status=0
    else
      run_status=$?
    fi

    if ((run_status == 124)); then
      record_result "${library}" "${benchmark}" RUN_TIMEOUT '' '' \
        "See logs/${library}/${log_stem}.run.log"
      continue
    fi
    if ((run_status != 0)); then
      record_result "${library}" "${benchmark}" RUN_FAILED '' '' \
        "Exit ${run_status}; see logs/${library}/${log_stem}.run.log"
      continue
    fi
    if grep -q 'TEST TERMINATED ABNORMALLY' "${run_log}"; then
      record_result "${library}" "${benchmark}" RUN_FAILED '' '' \
        "PBBS reported abnormal termination; see logs/${library}/${log_stem}.run.log"
      continue
    fi

    summary="$(grep -E " : ${workers} : geomean of mins = " "${run_log}" | tail -n 1)"
    if [[ -z "${summary}" ]]; then
      record_result "${library}" "${benchmark}" NO_SUMMARY '' '' \
        "No PBBS aggregate line; see logs/${library}/${log_stem}.run.log"
      continue
    fi

    geomean_min="$(sed -E 's/.*geomean of mins = ([^,]+),.*/\1/' <<<"${summary}")"
    geomean="$(sed -E 's/.*geomean of geomeans = ([^ ]+).*/\1/' <<<"${summary}")"
    record_result "${library}" "${benchmark}" PASS \
      "${geomean_min}" "${geomean}" ''
  done
}

echo "Discovered ${#benchmarks[@]} PBBS benchmark targets"
echo "Results will be written to ${output_dir}"
for library in "${libraries[@]}"; do
  build_library "${library}"
  run_library "${library}"
done
write_report
