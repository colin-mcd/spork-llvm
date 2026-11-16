#!/usr/bin/env python3
"""Fine-grained microbenchmarks (microbench/: fib, nqueens) across libraries and core counts.

Builds each benchmark against each library with exactly the flags
run-pbbsbench-scaling.py uses (it is imported for its flags, its CPU pinning
and its SMT handling), then runs it under the same conditions:
  - SMT disabled through sysfs for the duration (--smt off, the default), or
    runs restricted to one hardware thread per core (--smt pin);
  - 1-core runs pinned to one CPU of NUMA node 0, memory bound to that node;
  - multi-core runs with memory interleaved across nodes (numactl -i all).
Each run does --warmup + --trials rounds in one process; the warmups are
discarded.  parlaylib-sequential runs only at 1 core.

Prints, per benchmark, the time at each library and core count, its ratio to
parlaylib-sequential at 1 core, and each library's speedup over that baseline;
writes the same to <output>/results.tsv.

Example:
  ./run-microbench.py --library spork-parlaylib --library parlaylib-sequential --cores 1,80
"""

import argparse
import datetime
import importlib.util
import math
import os
import shlex
import statistics
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location("harness", os.path.join(SCRIPT_DIR, "run-pbbsbench-scaling.py"))
harness = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(harness)

BENCH_DIR = os.path.join(SCRIPT_DIR, "microbench")
# Default problem sizes: about 1.5-1.8 s per sequential round on the 80-core
# Xeon 8380 machine.
BENCHMARKS = {"fib": 42, "nqueens": 14}
BASELINE = "parlaylib-sequential"


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--library", action="append", choices=harness.LIBRARIES,
                   help="library to run (repeatable; default: spork-parlaylib and %s)" % BASELINE)
    p.add_argument("--bench", action="append", choices=sorted(BENCHMARKS),
                   help="benchmark to run (repeatable; default: all)")
    p.add_argument("--size", action="append", default=[], metavar="BENCH=N",
                   help="problem size, e.g. fib=40 (default: %s)"
                        % ", ".join("%s=%d" % kv for kv in BENCHMARKS.items()))
    p.add_argument("--cores", default="1,80", help="comma-separated core counts (default: 1,80)")
    p.add_argument("--warmup", type=int, default=3, help="discarded rounds per run (default: 3)")
    p.add_argument("--trials", type=int, default=5, help="timed rounds per run (default: 5)")
    p.add_argument("--smt", choices=["off", "pin", "keep"], default="off",
                   help="as in run-pbbsbench-scaling.py (default: off)")
    p.add_argument("--pass", dest="pass_name", default="SporkUnrollPass",
                   help="Spork unroll plugin directory (default: SporkUnrollPass)")
    p.add_argument("--llvm", choices=sorted(harness.LLVM_TOOLCHAINS), default=harness.DEFAULT_LLVM,
                   help="as in run-pbbsbench-scaling.py: the LLVM for every library but OpenCilk,"
                        " 19 (stock LLVM 19.1.7; default) or repo (clang 24)")
    p.add_argument("--output", help="result directory (default: microbench/results/<timestamp>)")
    return p.parse_args()


def build_flags(lib, plugin):
    """The flags run-pbbsbench-scaling.py builds lib with."""
    cflags, lflags = harness.BASE_CFLAGS, harness.LFLAGS
    if lib in harness.SPORK_LIBRARIES:
        cflags += " -fpass-plugin=%s -DVOLATILE_UNROLL=" % plugin
    if lib in harness.STARTUP_HEADER_LIBRARIES:
        cflags += " -include %s" % harness.STARTUP_HEADER
    if lib in harness.FIBER_LIBRARIES:
        lflags += " -lboost_context"
    if lib in harness.BACKENDS:
        cflags += " " + harness.BACKENDS[lib]["cflags"]
        lflags += " " + harness.BACKENDS[lib]["lflags"]
    return cflags, lflags


def compiler(lib, clangxx):
    if harness.BACKENDS.get(lib, {}).get("cxx") == "opencilk":
        if not harness.OPENCILK_CXX_DEFAULT:
            sys.exit("error: %s needs the OpenCilk compiler (set OPENCILK_CXX)" % lib)
        return harness.OPENCILK_CXX_DEFAULT
    return clangxx


def build(lib, bench, out, clangxx, plugin):
    cflags, lflags = build_flags(lib, plugin)
    include = os.path.join(SCRIPT_DIR, harness.base_library(lib), "include")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cmd = ([compiler(lib, clangxx)] + shlex.split(cflags) + ["-I", include, os.path.join(BENCH_DIR, bench + ".cpp"),
           "-o", out] + shlex.split(lflags))
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=SCRIPT_DIR)
    if r.returncode != 0:
        sys.exit("error: building %s for %s failed:\n$ %s\n%s"
                 % (bench, lib, " ".join(map(shlex.quote, cmd)), r.stderr))


def one_core_cpu(cpus):
    """As run-pbbsbench-scaling.py: the first CPU of NUMA node 0 other than CPU 0."""
    candidates = [c for c in cpus if c != 0 and harness.cpu_node(c) in (0, None)] or cpus
    return candidates[0]


def run(lib, binary, n, cores, rounds, cpus, pin_list):
    if cores == 1:
        cpu = one_core_cpu(cpus)
        prefix = ["numactl", "--physcpubind=%d" % cpu]
        node = harness.cpu_node(cpu)
        if node is not None:
            prefix += ["--membind=%d" % node]
    else:
        prefix = (["taskset", "-c", pin_list] if pin_list else []) + ["numactl", "-i", "all"]
    env = dict(os.environ, PARLAY_NUM_THREADS=str(cores))
    threads_env = harness.BACKENDS.get(lib, {}).get("threads_env")
    if threads_env:
        env[threads_env] = str(cores)
    r = subprocess.run(prefix + [binary, str(n), str(rounds)], capture_output=True, text=True, env=env)
    if r.returncode != 0:
        sys.exit("error: %s failed (exit %d):\n%s" % (binary, r.returncode, r.stderr))
    times = [float(line.split()[2]) for line in r.stdout.splitlines() if line.startswith("round ")]
    if len(times) != rounds:
        sys.exit("error: %s printed %d rounds, expected %d" % (binary, len(times), rounds))
    return times


def main():
    args = parse_args()
    libraries = args.library or ["spork-parlaylib", BASELINE]
    benches = args.bench or sorted(BENCHMARKS)
    sizes = dict(BENCHMARKS)
    for s in args.size:
        name, _, value = s.partition("=")
        if name not in sizes or not value.isdigit():
            sys.exit("error: bad --size %r" % s)
        sizes[name] = int(value)
    cores_list = [int(c) for c in args.cores.split(",")]
    output = args.output or os.path.join(BENCH_DIR, "results", datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    clangxx, plugin_build = harness.LLVM_TOOLCHAINS[args.llvm]
    if not os.path.exists(clangxx):
        sys.exit("error: compiler not found: %s" % clangxx)
    plugin = os.path.join(SCRIPT_DIR, args.pass_name, plugin_build, "SporkUnroll.so")
    if any(lib in harness.SPORK_LIBRARIES for lib in libraries) and not os.path.exists(plugin):
        sys.exit("error: plugin not found: %s" % plugin)

    print("Building into %s with %s" % (output, harness.compiler_version(clangxx)), flush=True)
    for lib in libraries:
        for bench in benches:
            build(lib, bench, os.path.join(output, "bin", lib, bench), clangxx, plugin)

    smt = harness.SmtManager(args.smt)
    results = []   # (bench, n, lib, cores, times)
    try:
        smt.setup()
        cpus = [int(c) for c in smt.pin_list.split(",")] if smt.pin_list else harness.online_cpus()
        if max(cores_list) > len(cpus):
            sys.exit("error: %d cores requested but only %d usable" % (max(cores_list), len(cpus)))
        print("1-core runs pinned to CPU %d" % one_core_cpu(cpus), flush=True)
        rounds = args.warmup + args.trials
        for bench in benches:
            for lib in libraries:
                for cores in cores_list:
                    if not harness.runs_at(lib, cores):
                        continue
                    times = run(lib, os.path.join(output, "bin", lib, bench), sizes[bench], cores, rounds,
                                cpus, smt.pin_list)[args.warmup:]
                    results.append((bench, sizes[bench], lib, cores, times))
                    print("  %-8s %-22s %2d cores  %.4f s (min %.4f)"
                          % (bench, lib, cores, statistics.mean(times), min(times)), flush=True)
    finally:
        smt.restore()
    report(results, output, args)


def report(results, output, args):
    base = {(b, n): statistics.mean(t) for b, n, lib, c, t in results if lib == BASELINE and c == 1}
    lines = ["bench\tn\tlibrary\tcores\tmean_s\tmin_s\tstdev_s\tvs_seq_1core\tspeedup_vs_seq"]
    print("\n%d warmup + %d timed rounds per run; times in seconds; 'vs seq' = time / %s at 1 core;"
          " 'speedup' = %s at 1 core / time" % (args.warmup, args.trials, BASELINE, BASELINE))
    print("%-8s %3s  %-22s %5s  %9s  %9s  %6s  %7s  %8s"
          % ("bench", "n", "library", "cores", "mean", "min", "cv", "vs seq", "speedup"))
    for b, n, lib, c, t in results:
        mean, sd = statistics.mean(t), (statistics.stdev(t) if len(t) > 1 else 0.0)
        seq = base.get((b, n))
        ratio = mean / seq if seq else math.nan
        speedup = seq / mean if seq else math.nan
        print("%-8s %3d  %-22s %5d  %9.4f  %9.4f  %5.1f%%  %7.3f  %8.2f"
              % (b, n, lib, c, mean, min(t), 100 * sd / mean, ratio, speedup))
        lines.append("%s\t%d\t%s\t%d\t%.6f\t%.6f\t%.6f\t%.4f\t%.3f" % (b, n, lib, c, mean, min(t), sd, ratio, speedup))
    os.makedirs(output, exist_ok=True)
    with open(os.path.join(output, "results.tsv"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\nWrote %s" % os.path.join(output, "results.tsv"))


if __name__ == "__main__":
    main()
