#!/usr/bin/env python3
"""Core-scaling benchmark of the PBBS parallel programs.

Runs every benchmark that pbbsbench/runall marks as parallel (the boolean in
each test tuple) on the large PBBS inputs, once per library (stock parlaylib,
hb-parlaylib, ehb2-parlaylib with steal requests only and no timer heartbeats,
and stock parlaylib on its alternative scheduler backends: parlaylib-openmp,
parlaylib-tbb and parlaylib-opencilk), across a list of core counts.  Each run performs
``warmup + trials`` in-process rounds; the first ``warmup`` rounds are
discarded and the remaining ``trials`` rounds are averaged.

SMT is disabled through /sys/devices/system/cpu/smt/control for the duration
of the benchmark (and restored afterwards).  See ``--smt`` for alternatives.

Outputs (in --output):
  report.md    Markdown tables: mean +- P90 error per benchmark and core count.
  results.tsv  One row per (library, benchmark, input, cores) with statistics.
  raw.json     Every individual trial time, plus the run configuration.
  logs/        stdout/stderr of every build and benchmark process.
  bin/         The benchmark binaries built for each library.
"""

import argparse
import ast
import atexit
import datetime
import fnmatch
import glob
import json
import math
import os
import shlex
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PBBS_DIR = os.path.join(SCRIPT_DIR, "pbbsbench")
CLANGXX = os.path.join(SCRIPT_DIR, "llvm-project", "build", "bin", "clang++")
STARTUP_HEADER = os.path.join(PBBS_DIR, "common", "spork_benchmark_startup.h")
SMT_CONTROL = "/sys/devices/system/cpu/smt/control"
SMT_ACTIVE = "/sys/devices/system/cpu/smt/active"
DEFAULT_CORES = [1, 2, 4, 6, 8, 10, 20, 40, 80]
# Stock parlaylib on its alternative scheduler backends
# (https://cmuparlay.github.io/parlaylib/installation.html).  Each uses the
# stock headers, selected with use-parlaylib.sh, plus a backend macro and the
# matching compiler/linker flags.  A backend is verified at preflight by
# compiling a probe program; one that cannot be built is skipped when it was
# selected implicitly (--library all) and is an error when named explicitly.
#   parlaylib-openmp   LLVM's libomp: installed under /usr/local if present,
#                      else the build in llvm-project/build-openmp (cmake -S
#                      llvm-project/runtimes -DLLVM_ENABLE_RUNTIMES=openmp;
#                      ninja omp), else GCC's libgomp, whose task scheduling
#                      is far slower
#   parlaylib-tbb      oneTBB (libtbb-dev)
#   parlaylib-opencilk the OpenCilk compiler (a separate LLVM): $OPENCILK_CXX,
#                      --opencilk-cxx, or /opt/opencilk/bin/clang++ (a release
#                      tarball unpacked under /opt and symlinked); thread count
#                      from CILK_NWORKERS
# CilkPlus is not offered: no current compiler supports -fcilkplus.
OPENCILK_CXX_DEFAULT = os.environ.get("OPENCILK_CXX") or \
    next((p for p in ["/opt/opencilk/bin/clang++"] if os.path.exists(p)), None)
LLVM_OMP_DIR = os.path.join(SCRIPT_DIR, "llvm-project", "build-openmp", "openmp", "runtime", "src")
GCC_OMP_INCLUDE = next(iter(sorted(glob.glob("/usr/lib/gcc/*/*/include/omp.h"))), None)
if os.path.exists("/usr/local/include/omp.h") and os.path.exists("/usr/local/lib/libomp.so"):
    # LLVM's libomp installed system-wide (cmake -P build-openmp/cmake_install.cmake).
    OMP_CFLAGS = "-DPARLAY_OPENMP -fopenmp"
    OMP_LFLAGS = "-fopenmp"
elif os.path.exists(os.path.join(LLVM_OMP_DIR, "libomp.so")):
    OMP_CFLAGS = "-DPARLAY_OPENMP -fopenmp -I " + LLVM_OMP_DIR
    OMP_LFLAGS = "-fopenmp -L %s -lomp -Wl,-rpath,%s" % (LLVM_OMP_DIR, LLVM_OMP_DIR)
else:
    # omp.h comes from the GCC install; -idirafter keeps that directory's
    # own stddef.h etc. behind libc++'s headers.
    OMP_CFLAGS = "-DPARLAY_OPENMP -fopenmp=libgomp" + \
                 (" -idirafter " + os.path.dirname(GCC_OMP_INCLUDE) if GCC_OMP_INCLUDE else "")
    OMP_LFLAGS = "-fopenmp=libgomp"
BACKENDS = {
    "parlaylib-openmp": {
        "cflags": OMP_CFLAGS,
        "lflags": OMP_LFLAGS,
        "threads_env": "OMP_NUM_THREADS",
    },
    "parlaylib-tbb": {
        # oneTBB has no thread-count variable; the header narrows the affinity
        # mask and caps the pool from PARLAY_NUM_THREADS.
        "cflags": "-DPARLAY_TBB -include " + os.path.join(PBBS_DIR, "common", "tbb_num_threads.h"),
        "lflags": "-ltbb",
        "threads_env": None,
    },
    "parlaylib-opencilk": {
        "cflags": "-DPARLAY_OPENCILK -fopencilk",
        "lflags": "-fopencilk",
        "threads_env": "CILK_NWORKERS",
        "cxx": "opencilk",   # resolved from --opencilk-cxx / OPENCILK_CXX
    },
}
LIBRARIES = ["parlaylib", "hb-parlaylib", "ehb2-parlaylib"] + list(BACKENDS)


def base_library(lib):
    """The library directory a name refers to (backends share stock parlaylib)."""
    return "parlaylib" if lib in BACKENDS else lib
# Libraries built with the Spork unroll plugin.
SPORK_LIBRARIES = ["hb-parlaylib", "ehb2-parlaylib"]
# Of those, the ones that also need the heartbeat startup header force-included
# (ehb2 sets itself up when its scheduler is constructed).
STARTUP_HEADER_LIBRARIES = ["hb-parlaylib"]

BASE_CFLAGS = "-mcx16 -O3 -g -std=c++20 -DNDEBUG -I . -stdlib=libc++ -w"
LFLAGS = "-pthread -ldl -L/usr/local/lib -ljemalloc -stdlib=libc++"


# --------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------

def parse_cores(text):
    cores = []
    for part in text.replace(",", " ").split():
        if not part.isdigit() or int(part) < 1:
            raise argparse.ArgumentTypeError(
                "core counts must be positive integers: %r" % part)
        cores.append(int(part))
    if not cores:
        raise argparse.ArgumentTypeError("at least one core count is required")
    return sorted(set(cores))


def positive_int(text):
    if not text.isdigit() or int(text) < 1:
        raise argparse.ArgumentTypeError("expected a positive integer: %r" % text)
    return int(text)


def nonneg_int(text):
    if not text.isdigit():
        raise argparse.ArgumentTypeError("expected a non-negative integer: %r" % text)
    return int(text)


def parse_args():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--warmup", type=nonneg_int, default=5,
                   help="discarded warmup rounds per run (default: 5)")
    p.add_argument("--trials", type=positive_int, default=15,
                   help="timed rounds per run that are averaged (default: 15)")
    p.add_argument("--cores", type=parse_cores,
                   default=DEFAULT_CORES,
                   help="comma-separated core counts (default: %s)"
                        % ",".join(map(str, DEFAULT_CORES)))
    p.add_argument("--library", action="append", choices=["all", "both"] + LIBRARIES,
                   help="which Parlay implementation(s) to benchmark; repeatable"
                        " (default: all of %s; a backend whose toolchain is missing"
                        " is skipped unless named explicitly)" % ", ".join(LIBRARIES))
    p.add_argument("--opencilk-cxx", default=OPENCILK_CXX_DEFAULT,
                   metavar="PATH", help="OpenCilk clang++ for parlaylib-opencilk"
                        " (default: $OPENCILK_CXX, else /opt/opencilk/bin/clang++ if present)")
    p.add_argument("--only", action="append", default=[], metavar="GLOB",
                   help="only run benchmarks matching this glob, e.g. 'comparisonSort/*'"
                        " (repeatable)")
    p.add_argument("--primary-only", action="store_true",
                   help="skip the benchmarks runall marks as extended (third tuple field 1)")
    p.add_argument("--max-inputs", type=positive_int, metavar="N",
                   help="only run the first N inputs listed in each family's testInputs"
                        " (default: all)")
    p.add_argument("--pass", dest="pass_name", default="fablepass",
                   help="Spork unroll plugin directory: fablepass (default) or gempass")
    p.add_argument("--output", metavar="DIR",
                   help="result directory (default: pbbsbench-scaling/<timestamp>)")
    p.add_argument("--no-build", action="store_true",
                   help="reuse binaries already present in <output>/bin (requires --output)")
    p.add_argument("--reuse-bin", nargs="?", const="latest", metavar="DIR",
                   help="skip the build and run the binaries of a previous run directory"
                        " (its bin/ subdirectory); with no DIR, the newest run under"
                        " pbbsbench-scaling/ that has binaries. Results go to a new --output.")
    p.add_argument("--build-only", action="store_true",
                   help="build the benchmark binaries into <output>/bin and exit")
    p.add_argument("--build-jobs", type=positive_int, default=os.cpu_count() or 1,
                   help="concurrent benchmark builds (default: all %d CPUs)" % (os.cpu_count() or 1))
    p.add_argument("--check", action="store_true",
                   help="run the PBBS correctness checker once per benchmark and library"
                        " (at the largest core count)")
    p.add_argument("--smt", choices=["off", "pin", "keep"], default="off",
                   help="off: disable SMT via sysfs while benchmarking (default, needs root"
                        " or sudo); pin: keep SMT on but restrict benchmarks to one hardware"
                        " thread per core; keep: do nothing about SMT")
    p.add_argument("--no-numa", action="store_true",
                   help="do not wrap multi-core runs in 'numactl -i all'")
    p.add_argument("--timeout", type=nonneg_int, default=7200,
                   help="seconds allowed per benchmark process, 0 for unlimited (default: 7200)")
    p.add_argument("--dry-run", action="store_true",
                   help="print the plan (benchmarks, inputs, cores) and exit")
    args = p.parse_args()
    if args.no_build and not args.output:
        p.error("--no-build requires --output")
    if args.reuse_bin and args.no_build:
        p.error("--reuse-bin and --no-build are mutually exclusive")
    return args


def latest_run_with_binaries(exclude, needed):
    """Newest pbbsbench-scaling/<stamp> directory (other than `exclude`) whose bin/
    holds every path in `needed` (relative to bin/).  Directories with only some of
    them, e.g. a build still in progress or one built with --only, are skipped."""
    root = os.path.join(SCRIPT_DIR, "pbbsbench-scaling")
    candidates = sorted((d for d in glob.glob(os.path.join(root, "*"))
                         if os.path.isdir(os.path.join(d, "bin")) and
                         os.path.realpath(d) != os.path.realpath(exclude)),
                        key=os.path.getmtime, reverse=True)
    for d in candidates:
        if all(os.path.exists(os.path.join(d, "bin", rel)) for rel in needed):
            return d
    sys.exit("error: --reuse-bin: no run under %s has binaries for every selected"
             " benchmark and library; pass a directory explicitly" % root)


# --------------------------------------------------------------------------
# Benchmark discovery (from pbbsbench/runall and each family's testInputs)
# --------------------------------------------------------------------------

def literal_assignments(path):
    """Return {name: value} for every top-level literal assignment in a Python file."""
    with open(path) as f:
        tree = ast.parse(f.read(), path)
    values = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 \
                and isinstance(node.targets[0], ast.Name):
            try:
                values[node.targets[0].id] = ast.literal_eval(node.value)
            except ValueError:
                pass
    return values


def parallel_benchmarks_from_runall():
    tests = literal_assignments(os.path.join(PBBS_DIR, "runall")).get("tests")
    if not tests:
        sys.exit("error: could not find the 'tests' list in pbbsbench/runall")
    return [(name, int(ext) != 0) for name, is_parallel, ext in tests if is_parallel]


class Benchmark:
    def __init__(self, name, extended):
        self.name = name                      # e.g. comparisonSort/sampleSort
        self.family = name.split("/")[0]
        self.extended = extended
        self.dir = os.path.join(PBBS_DIR, "benchmarks", name)
        bench_dir = os.path.join(PBBS_DIR, "benchmarks", self.family, "bench")
        spec = literal_assignments(os.path.join(bench_dir, "testInputs"))
        for key in ("bnchmrk", "checkProgram", "dataDir", "tests"):
            if key not in spec:
                sys.exit("error: %s/bench/testInputs does not define %s" % (self.family, key))
        self.binary_name = spec["bnchmrk"]
        self.check_program = spec["checkProgram"]
        self.data_dir = os.path.realpath(os.path.join(self.dir, spec["dataDir"]))
        self.inputs = []
        for weight, files, run_opts, check_opts in spec["tests"]:
            if isinstance(files, str):
                files = [files]
            self.inputs.append({
                "files": list(files),
                "label": "+".join(files) + (" " + run_opts if run_opts else ""),
                "run_opts": run_opts,
                "check_opts": check_opts,
            })

    @property
    def log_stem(self):
        return self.name.replace("/", "__")


# --------------------------------------------------------------------------
# CPU topology and SMT control
# --------------------------------------------------------------------------

def read_sysfs(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def write_sysfs(path, value):
    """Write to a sysfs file, escalating through sudo if needed. Returns True on success."""
    try:
        with open(path, "w") as f:
            f.write(value)
        return True
    except PermissionError:
        pass
    except OSError as e:
        print("warning: writing %s failed: %s" % (path, e), file=sys.stderr)
        return False
    if not shutil.which("sudo"):
        return False
    cmd = ["sudo", "-n", "tee", path]
    r = subprocess.run(cmd, input=value, text=True, capture_output=True)
    if r.returncode == 0:
        return True
    if sys.stdin.isatty():
        print("Root is required to write %s (SMT control); asking sudo." % path)
        r = subprocess.run(["sudo", "tee", path], input=value, text=True,
                           stdout=subprocess.DEVNULL)
        return r.returncode == 0
    return False


def online_cpus():
    return sorted(os.sched_getaffinity(0))


def one_thread_per_core(cpus):
    """Pick the lowest-numbered sibling of each physical core from `cpus`."""
    chosen = set()
    seen_cores = set()
    for cpu in cpus:
        sib = read_sysfs("/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list" % cpu)
        if sib is None:
            chosen.add(cpu)
            continue
        siblings = []
        for part in sib.split(","):
            if "-" in part:
                a, b = part.split("-")
                siblings.extend(range(int(a), int(b) + 1))
            else:
                siblings.append(int(part))
        key = tuple(sorted(siblings))
        if key in seen_cores:
            continue
        seen_cores.add(key)
        chosen.add(cpu)
    return sorted(chosen)


class SmtManager:
    """Disables SMT for the benchmark and restores the previous setting on exit."""

    def __init__(self, mode):
        self.mode = mode
        self.previous = None
        self.changed = False
        self.pin_list = None

    def setup(self):
        control = read_sysfs(SMT_CONTROL)
        active = read_sysfs(SMT_ACTIVE)
        if self.mode == "keep":
            if active == "1":
                print("warning: SMT is active and --smt keep was given", file=sys.stderr)
            return
        if self.mode == "pin":
            cpus = one_thread_per_core(online_cpus())
            self.pin_list = ",".join(map(str, cpus))
            print("SMT pinning: benchmarks restricted to one hardware thread per core (%d CPUs)"
                  % len(cpus))
            return
        # mode == off
        if control is None:
            if active == "0" or active is None:
                print("SMT control not available in sysfs; assuming no SMT.")
                return
            sys.exit("error: cannot control SMT (no %s). Use --smt pin or --smt keep."
                     % SMT_CONTROL)
        if control in ("notsupported", "notimplemented", "forceoff", "off"):
            print("SMT is already off (%s)." % control)
            return
        self.previous = control
        print("Disabling SMT via %s (was '%s')." % (SMT_CONTROL, control))
        if not write_sysfs(SMT_CONTROL, "off"):
            sys.exit("error: could not disable SMT (need root or passwordless sudo).\n"
                     "       Re-run as root, or use --smt pin to restrict runs to one hardware\n"
                     "       thread per core without changing the system setting.")
        self.changed = True
        time.sleep(0.5)
        if read_sysfs(SMT_ACTIVE) not in ("0", None):
            self.restore()
            sys.exit("error: SMT still reported active after writing 'off'")
        print("SMT disabled; %d CPUs online." % len(online_cpus()))

    def restore(self):
        if self.changed and self.previous is not None:
            print("Restoring SMT setting '%s'." % self.previous)
            if write_sysfs(SMT_CONTROL, self.previous):
                self.changed = False
            else:
                print("warning: failed to restore SMT; run: echo %s | sudo tee %s"
                      % (self.previous, SMT_CONTROL), file=sys.stderr)


# --------------------------------------------------------------------------
# Library selection (symlinks inside pbbsbench)
# --------------------------------------------------------------------------

def current_library():
    target = os.readlink(os.path.join(PBBS_DIR, "parlaylib")) if \
        os.path.islink(os.path.join(PBBS_DIR, "parlaylib")) else ""
    base = os.path.basename(target.rstrip("/"))
    return base if base in LIBRARIES else None


def select_library(lib):
    subprocess.run([os.path.join(SCRIPT_DIR, "use-%s.sh" % base_library(lib))],
                   check=True, stdout=subprocess.DEVNULL)


# --------------------------------------------------------------------------
# Statistics
# --------------------------------------------------------------------------

def percentile(values, q):
    s = sorted(values)
    n = len(s)
    if n == 1:
        return s[0]
    pos = (n - 1) * q
    lo = int(math.floor(pos))
    hi = min(lo + 1, n - 1)
    return s[lo] + (s[hi] - s[lo]) * (pos - lo)


def summarize(times):
    """Mean and P90 error: the 90th percentile of |t - mean| over the trials."""
    mean = statistics.fmean(times)
    return {
        "n": len(times),
        "mean": mean,
        "p90_err": percentile([abs(t - mean) for t in times], 0.90),
        "min": min(times),
        "median": statistics.median(times),
        "stdev": statistics.stdev(times) if len(times) > 1 else 0.0,
    }


def geomean(values):
    return math.exp(sum(math.log(v) for v in values) / len(values))


# --------------------------------------------------------------------------
# Runner
# --------------------------------------------------------------------------

class Runner:
    def __init__(self, args):
        self.args = args
        chosen = args.library or ["all"]
        self.libraries = (list(LIBRARIES) if {"all", "both"} & set(chosen)
                          else [l for l in LIBRARIES if l in chosen])
        self.explicit_libraries = {l for l in chosen if l in LIBRARIES}
        self.plugin = os.path.join(SCRIPT_DIR, args.pass_name, "build", "SporkUnroll.so")
        now = datetime.datetime.now()
        stamp = now.strftime("%Y%m%d-%H%M%S")
        self.started = now.isoformat(timespec="seconds")
        self.output = os.path.abspath(args.output or
                                      os.path.join(SCRIPT_DIR, "pbbsbench-scaling", stamp))
        self.bin_dir = os.path.join(self.output, "bin")
        self.log_dir = os.path.join(self.output, "logs")
        self.initial_library = current_library()
        self.smt = SmtManager(args.smt)
        self.results = []           # per (library, benchmark, input, cores)
        self.failures = []
        self.cores = list(args.cores)
        self.finalized = False
        self.benchmarks = self.discover()
        if args.reuse_bin:
            if args.reuse_bin == "latest":
                needed = [os.path.join(lib, b.log_stem, b.binary_name)
                          for lib in self.libraries for b in self.benchmarks]
                src = latest_run_with_binaries(self.output, needed)
            else:
                src = os.path.abspath(args.reuse_bin)
                if not os.path.isdir(os.path.join(src, "bin")):
                    sys.exit("error: --reuse-bin: %s has no bin/ directory" % src)
            self.bin_dir = os.path.join(src, "bin")

    # ---- discovery -------------------------------------------------------
    def discover(self):
        selected = []
        for name, extended in parallel_benchmarks_from_runall():
            if self.args.primary_only and extended:
                continue
            if self.args.only and not any(fnmatch.fnmatch(name, g) for g in self.args.only):
                continue
            bench = Benchmark(name, extended)
            if self.args.max_inputs:
                bench.inputs = bench.inputs[:self.args.max_inputs]
            selected.append(bench)
        if not selected:
            sys.exit("error: no benchmarks selected")
        return selected

    # ---- preflight -------------------------------------------------------
    def compiler_for(self, lib):
        if BACKENDS.get(lib, {}).get("cxx") == "opencilk":
            return self.args.opencilk_cxx
        return CLANGXX

    def probe_backend(self, lib):
        """Try to build and run a tiny program on this backend; return None or an error string."""
        cxx = self.compiler_for(lib)
        if not cxx:
            return "no OpenCilk compiler given (--opencilk-cxx or OPENCILK_CXX)"
        if not os.path.exists(cxx):
            return "compiler not found: %s" % cxx
        b = BACKENDS[lib]
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "probe.cpp")
            exe = os.path.join(d, "probe")
            with open(src, "w") as f:
                f.write("#include <parlay/parallel.h>\n#include <atomic>\n#include <cstdio>\n"
                        "int main() { std::atomic<long> s{0};"
                        " parlay::parallel_for(0, 100000, [&](size_t i) { s += i; });"
                        " std::printf(\"%ld %zu\\n\", s.load(), parlay::num_workers()); }\n")
            cmd = [cxx] + shlex.split(BASE_CFLAGS + " " + b["cflags"]) + \
                  ["-I", os.path.join(SCRIPT_DIR, "parlaylib", "include"), src, "-o", exe] + \
                  shlex.split(LFLAGS + " " + b["lflags"])
            r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if r.returncode != 0:
                return "probe failed to build:\n" + r.stdout.strip()
            r = subprocess.run([exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if r.returncode != 0 or not r.stdout.startswith("4999950000 "):
                return "probe failed to run: " + r.stdout.strip()
        return None

    def preflight(self):
        for path in [CLANGXX, os.path.join(SCRIPT_DIR, "use-parlaylib.sh")] + \
                [os.path.join(SCRIPT_DIR, "use-%s.sh" % base_library(lib)) for lib in self.libraries]:
            if not os.path.exists(path):
                sys.exit("error: required file not found: %s" % path)
        for lib in [l for l in self.libraries if l in BACKENDS]:
            err = self.probe_backend(lib)
            if err is None:
                continue
            if lib in self.explicit_libraries:
                sys.exit("error: %s is not usable: %s" % (lib, err))
            print("warning: skipping %s: %s" % (lib, err.splitlines()[0]), file=sys.stderr)
            self.libraries.remove(lib)
        if any(lib in SPORK_LIBRARIES for lib in self.libraries):
            if not os.path.exists(self.plugin):
                sys.exit("error: required file not found: %s" % self.plugin)
        if any(lib in STARTUP_HEADER_LIBRARIES for lib in self.libraries):
            if not os.path.exists(STARTUP_HEADER):
                sys.exit("error: required file not found: %s" % STARTUP_HEADER)
        if not self.args.no_numa and not shutil.which("numactl"):
            print("warning: numactl not found; running without memory interleaving",
                  file=sys.stderr)
            self.args.no_numa = True
        if self.args.smt == "pin" and not shutil.which("taskset"):
            sys.exit("error: --smt pin requires taskset")

    def apply_core_limit(self):
        available = len(online_cpus())
        if self.smt.pin_list:
            available = len(self.smt.pin_list.split(","))
        usable = [c for c in self.cores if c <= available]
        skipped = [c for c in self.cores if c > available]
        if skipped:
            print("warning: only %d cores available without SMT; skipping core counts %s"
                  % (available, ", ".join(map(str, skipped))), file=sys.stderr)
        if not usable:
            sys.exit("error: no requested core count fits on this machine")
        self.cores = usable

    # ---- build -----------------------------------------------------------
    def binary_path(self, lib, bench):
        return os.path.join(self.bin_dir, lib, bench.log_stem, bench.binary_name)

    def build_one(self, lib, bench, family_locks):
        cflags, lflags = BASE_CFLAGS, LFLAGS
        if lib in SPORK_LIBRARIES:
            cflags += " -fpass-plugin=%s" % self.plugin
        if lib in STARTUP_HEADER_LIBRARIES:
            cflags += " -include %s" % STARTUP_HEADER
        if lib in BACKENDS:
            cflags += " " + BACKENDS[lib]["cflags"]
            lflags += " " + BACKENDS[lib]["lflags"]
        log = os.path.join(self.log_dir, lib, bench.log_stem + ".build.log")
        cmd = ["make", "-B", "-s", "-C", bench.dir, "CC=" + self.compiler_for(lib),
               "CFLAGS=" + cflags, "LFLAGS=" + lflags]
        print("[%s] building %s" % (lib, bench.name), flush=True)
        # Variants of one family share ../bench (checker, Time.C); serialize them.
        with family_locks[bench.family]:
            with open(log, "w") as f:
                r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
            src = os.path.join(bench.dir, bench.binary_name)
            if r.returncode != 0 or not os.path.exists(src):
                self.failures.append((lib, bench.name, "BUILD_FAILED", "see " + log))
                print("[%s] BUILD FAILED %s (see %s)" % (lib, bench.name, log), flush=True)
                return
            dst = self.binary_path(lib, bench)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)

    def build_all(self):
        family_locks = {b.family: threading.Lock() for b in self.benchmarks}
        for lib in self.libraries:
            os.makedirs(os.path.join(self.log_dir, lib), exist_ok=True)
            print("=== Building with %s (%d jobs) ===" % (lib, self.args.build_jobs))
            select_library(lib)
            with ThreadPoolExecutor(max_workers=self.args.build_jobs) as pool:
                futures = [pool.submit(self.build_one, lib, b, family_locks)
                           for b in self.benchmarks]
                for f in futures:
                    f.result()

    def verify_binaries(self):
        for lib in self.libraries:
            for b in self.benchmarks:
                if not os.path.exists(self.binary_path(lib, b)):
                    self.failures.append((lib, b.name, "MISSING_BINARY",
                                          self.binary_path(lib, b)))

    # ---- data ------------------------------------------------------------
    @staticmethod
    def input_present(data_dir, name):
        # Some inputs are a family of files (e.g. covtype.data -> covtype.data.train/.test/.labels)
        # and the make target itself does not exist as a file.
        # A lone <name>.bz2 is the compressed source copied in by the data
        # Makefile, not a generated input.
        path = os.path.join(data_dir, name)
        return os.path.exists(path) or \
            any(not f.endswith(".bz2") for f in glob.glob(glob.escape(path) + ".*"))

    def generate_data(self):
        """Generate every needed input once; benchmarks whose inputs fail are skipped."""
        print("=== Generating input data ===")
        # Generators are built against whatever pbbsbench/parlay points at;
        # use stock parlaylib so plain g++ can compile them.
        select_library("parlaylib")
        wanted = {}
        for b in self.benchmarks:
            for inp in b.inputs:
                wanted.setdefault(b.data_dir, set()).update(inp["files"])
        os.makedirs(os.path.join(self.log_dir, "data"), exist_ok=True)
        self.bad_inputs = set()
        for data_dir, files in sorted(wanted.items()):
            log = os.path.join(self.log_dir, "data", os.path.basename(os.path.dirname(data_dir)) + ".log")
            for name in sorted(files):
                if self.input_present(data_dir, name):
                    continue
                print("generating %s/%s" % (os.path.relpath(data_dir, PBBS_DIR), name), flush=True)
                with open(log, "a") as f:
                    f.write("$ make -s %s   (in %s)\n" % (name, data_dir))
                    f.flush()
                    r = subprocess.run(["make", "-s", name], cwd=data_dir,
                                       stdout=f, stderr=subprocess.STDOUT)
                if r.returncode != 0 or not self.input_present(data_dir, name):
                    print("  FAILED to generate %s (see %s)" % (name, log), flush=True)
                    self.bad_inputs.add((data_dir, name))
        for b in self.benchmarks:
            bad = sorted({f for inp in b.inputs for f in inp["files"]
                          if (b.data_dir, f) in self.bad_inputs})
            if bad:
                b.inputs = [inp for inp in b.inputs
                            if not any((b.data_dir, f) in self.bad_inputs for f in inp["files"])]
                for lib in self.libraries:
                    self.failures.append((lib, b.name, "NO_INPUT",
                                          "could not generate: " + " ".join(bad)))

    # ---- run -------------------------------------------------------------
    def run_command_prefix(self, cores):
        prefix = []
        if self.smt.pin_list:
            prefix += ["taskset", "-c", self.smt.pin_list]
        if cores > 1 and not self.args.no_numa:
            prefix += ["numactl", "-i", "all"]
        return prefix

    def run_one(self, lib, bench, inp, cores, do_check):
        rounds = self.args.warmup + self.args.trials
        binary = self.binary_path(lib, bench)
        inputs = [os.path.join(bench.data_dir, f) for f in inp["files"]]
        out_file = None
        cmd = self.run_command_prefix(cores) + [binary]
        if inp["run_opts"]:
            cmd += shlex.split(inp["run_opts"])
        cmd += ["-r", str(rounds)]
        if do_check:
            out_file = os.path.join(self.output, "check_%s_%s.out" % (lib, bench.log_stem))
            cmd += ["-o", out_file]
        cmd += inputs
        env = dict(os.environ, PARLAY_NUM_THREADS=str(cores))
        threads_env = BACKENDS.get(lib, {}).get("threads_env")
        if threads_env:
            env[threads_env] = str(cores)
        log = os.path.join(self.log_dir, lib, "%s__p%d__%s.log"
                           % (bench.log_stem, cores, inp["label"].replace(" ", "_").replace("/", "_")))
        key = "%s %s [%s] p=%d" % (lib, bench.name, inp["label"], cores)
        timeout = self.args.timeout or None
        print("    running %-37s (%d rounds; tail -f %s)"
              % (inp["label"], rounds, log), flush=True)
        # stdout goes straight to the log file (not through a pipe) so that
        # `tail -f` on the log shows each round as PBBS prints it.
        with open(log, "w") as f:
            f.write("$ PARLAY_NUM_THREADS=%d %s%s\n"
                    % (cores, "%s=%d " % (threads_env, cores) if threads_env else "",
                       " ".join(shlex.quote(c) for c in cmd)))
            f.flush()
            try:
                r = subprocess.run(cmd, cwd=bench.dir, env=env, stdout=f,
                                   stderr=subprocess.STDOUT, timeout=timeout)
            except subprocess.TimeoutExpired:
                f.write("TIMEOUT after %s s\n" % timeout)
                self.failures.append((lib, bench.name, "RUN_TIMEOUT", key))
                print("  TIMEOUT %s" % key, flush=True)
                return None
        with open(log, errors="replace") as f:
            output = f.read()
        if r.returncode != 0:
            self.failures.append((lib, bench.name, "RUN_FAILED", "%s exit %d; see %s"
                                  % (key, r.returncode, log)))
            print("  FAILED %s (exit %d, see %s)" % (key, r.returncode, log), flush=True)
            return None
        times = []
        for line in output.splitlines():
            if line.startswith("Parlay time:"):
                try:
                    times.append(float(line.rsplit(":", 1)[1]))
                except ValueError:
                    pass
        if len(times) < rounds:
            self.failures.append((lib, bench.name, "BAD_OUTPUT",
                                  "%s produced %d of %d timings; see %s"
                                  % (key, len(times), rounds, log)))
            print("  BAD OUTPUT %s (%d/%d timings)" % (key, len(times), rounds), flush=True)
            return None
        if do_check and out_file:
            self.run_check(lib, bench, inp, inputs, out_file)
        return times[self.args.warmup:self.args.warmup + self.args.trials]

    def run_check(self, lib, bench, inp, inputs, out_file):
        cmd = [bench.check_program] + shlex.split(inp["check_opts"]) + inputs + [out_file]
        r = subprocess.run(cmd, cwd=bench.dir, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        problems = [l for l in r.stdout.splitlines() if l and not l.startswith(":")]
        if r.returncode != 0 or problems:
            self.failures.append((lib, bench.name, "CHECK_FAILED",
                                  "%s: %s" % (inp["label"], " | ".join(problems)[:300])))
            print("  CHECK FAILED %s %s [%s]" % (lib, bench.name, inp["label"]), flush=True)
        try:
            os.remove(out_file)
        except OSError:
            pass

    def run_all(self):
        built = {(lib, b.name) for lib in self.libraries for b in self.benchmarks
                 if os.path.exists(self.binary_path(lib, b))}
        total = len(built) * len(self.cores)
        done = 0
        print("=== Running: %d warmup + %d trials per input, cores %s ==="
              % (self.args.warmup, self.args.trials, self.cores))
        for bench in self.benchmarks:
            for cores in self.cores:
                for lib in self.libraries:
                    if (lib, bench.name) not in built:
                        continue
                    done += 1
                    print("[%d/%d] %s %s on %d core(s)" % (done, total, lib, bench.name, cores),
                          flush=True)
                    do_check = self.args.check and cores == self.cores[-1]
                    for inp in bench.inputs:
                        times = self.run_one(lib, bench, inp, cores, do_check)
                        if times is None:
                            continue
                        stats = summarize(times)
                        self.results.append({
                            "library": lib, "benchmark": bench.name,
                            "input": inp["label"], "cores": cores,
                            "times": times, **stats,
                        })
                        print("    %-45s %.4f ± %.4f s" % (inp["label"], stats["mean"],
                                                           stats["p90_err"]), flush=True)
                    self.save_raw()

    # ---- output ----------------------------------------------------------
    def config(self):
        return {
            "warmup": self.args.warmup, "trials": self.args.trials,
            "cores": self.cores, "libraries": self.libraries,
            "smt_mode": self.args.smt, "numactl": not self.args.no_numa,
            "pass": self.args.pass_name, "check": self.args.check,
            "max_inputs": self.args.max_inputs,
            "bin_dir": self.bin_dir,
            "benchmarks": [b.name for b in self.benchmarks],
            "hostname": os.uname().nodename,
            "started": self.started,
        }

    def save_raw(self):
        tmp = os.path.join(self.output, "raw.json.tmp")
        with open(tmp, "w") as f:
            json.dump({"config": self.config(), "results": self.results,
                       "failures": self.failures}, f, indent=1)
        os.replace(tmp, os.path.join(self.output, "raw.json"))

    def per_benchmark(self):
        """Aggregate inputs per trial index by geomean; returns {(lib, bench, cores): stats}."""
        groups = {}
        for r in self.results:
            groups.setdefault((r["library"], r["benchmark"], r["cores"]), []).append(r["times"])
        agg = {}
        for key, series in groups.items():
            n = min(len(s) for s in series)
            samples = [geomean([s[i] for s in series]) for i in range(n)]
            agg[key] = summarize(samples)
        return agg

    @staticmethod
    def cell(stats):
        return "%.3f ± %.3f" % (stats["mean"], stats["p90_err"]) if stats else "—"

    def table(self, rows, cell_fn, header0):
        lines = ["| %s | %s |" % (header0, " | ".join("%d" % c for c in self.cores)),
                 "|---|" + "---:|" * len(self.cores)]
        for label, key in rows:
            lines.append("| %s | %s |" % (label, " | ".join(cell_fn(key, c) for c in self.cores)))
        return "\n".join(lines)

    def write_report(self):
        agg = self.per_benchmark()
        by_input = {(r["library"], r["benchmark"], r["input"], r["cores"]): r
                    for r in self.results}
        bench_names = [b.name for b in self.benchmarks]
        out = []
        out.append("# PBBS core-scaling benchmark")
        out.append("")
        out.append("- Host: `%s`" % os.uname().nodename)
        out.append("- Libraries: %s" % ", ".join("`%s`" % l for l in self.libraries))
        out.append("- Cores: %s" % ", ".join(map(str, self.cores)))
        out.append("- Rounds per run: %d warmup (discarded) + %d timed trials"
                   % (self.args.warmup, self.args.trials))
        out.append("- SMT: `%s`%s" % (self.args.smt,
                   " (pinned to CPUs %s)" % self.smt.pin_list if self.smt.pin_list else ""))
        out.append("- Memory interleaving: %s" % ("`numactl -i all` for >1 core"
                                                   if not self.args.no_numa else "off"))
        out.append("- Spork plugin: `%s`" % self.args.pass_name)
        out.append("- Cell format: mean seconds ± P90 error, where the P90 error is the 90th"
                   " percentile of |trial − mean| over the timed trials.")
        out.append("- Per-benchmark rows combine a benchmark's inputs by taking, for each"
                   " trial index, the geometric mean across inputs; the statistics are over"
                   " those combined samples.")
        out.append("")
        for lib in self.libraries:
            out.append("## %s — mean ± P90 error (s)" % lib)
            out.append("")
            out.append(self.table([(n, n) for n in bench_names],
                                  lambda key, c, lib=lib: self.cell(agg.get((lib, key, c))),
                                  "Benchmark"))
            out.append("")
        a = self.libraries[0]
        for b in self.libraries[1:]:
            out.append("## Ratio of means: %s / %s" % (b, a))
            out.append("")

            def ratio(key, c, a=a, b=b):
                x, y = agg.get((a, key, c)), agg.get((b, key, c))
                return "%.3f" % (y["mean"] / x["mean"]) if x and y else "—"
            rows = [(n, n) for n in bench_names]
            out.append(self.table(rows, ratio, "Benchmark"))
            # Geometric mean of the per-benchmark ratios, per core count.
            gm_row = []
            for c in self.cores:
                rs = [agg[(b, n, c)]["mean"] / agg[(a, n, c)]["mean"] for n in bench_names
                      if (a, n, c) in agg and (b, n, c) in agg]
                gm_row.append("%.3f (%d)" % (geomean(rs), len(rs)) if rs else "—")
            out.append("")
            out.append("Geomean of ratios (benchmarks counted): " +
                       ", ".join("%d cores: %s" % (c, g) for c, g in zip(self.cores, gm_row)))
            out.append("")
        out.append("## Per-input results")
        out.append("")
        for lib in self.libraries:
            out.append("### %s" % lib)
            out.append("")
            rows = [("%s `%s`" % (bch.name, inp["label"]), (bch.name, inp["label"]))
                    for bch in self.benchmarks for inp in bch.inputs]
            out.append(self.table(rows, lambda key, c, lib=lib: self.cell(
                by_input.get((lib, key[0], key[1], c))), "Benchmark / input"))
            out.append("")
        if self.failures:
            out.append("## Failures")
            out.append("")
            out.append("| Library | Benchmark | Status | Note |")
            out.append("|---|---|---|---|")
            for lib, name, status, note in self.failures:
                out.append("| %s | %s | %s | %s |" % (lib, name, status, note.replace("|", "\\|")))
            out.append("")
        with open(os.path.join(self.output, "report.md"), "w") as f:
            f.write("\n".join(out))

        with open(os.path.join(self.output, "results.tsv"), "w") as f:
            f.write("library\tbenchmark\tinput\tcores\tn\tmean\tp90_err\tmin\tmedian\tstdev\n")
            for r in self.results:
                f.write("%s\t%s\t%s\t%d\t%d\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\n" % (
                    r["library"], r["benchmark"], r["input"], r["cores"], r["n"],
                    r["mean"], r["p90_err"], r["min"], r["median"], r["stdev"]))
            for lib, name, cores in sorted(agg):
                s = agg[(lib, name, cores)]
                f.write("%s\t%s\t%s\t%d\t%d\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\n" % (
                    lib, name, "<geomean of inputs>", cores, s["n"],
                    s["mean"], s["p90_err"], s["min"], s["median"], s["stdev"]))

        # Console summary
        print()
        for lib in self.libraries:
            print("%s — mean ± P90 error (seconds)" % lib)
            print(self.table([(n, n) for n in bench_names],
                             lambda key, c, lib=lib: self.cell(agg.get((lib, key, c))),
                             "Benchmark"))
            print()

    def finalize(self):
        if self.finalized:
            return
        self.finalized = True
        try:
            if self.results or self.failures:
                self.save_raw()
                self.write_report()
                print("Report:  %s" % os.path.join(self.output, "report.md"))
                print("Results: %s" % os.path.join(self.output, "results.tsv"))
        finally:
            self.smt.restore()
            if self.initial_library and current_library() != self.initial_library:
                select_library(self.initial_library)

    # ---- main ------------------------------------------------------------
    def dry_run(self):
        print("Output: %s" % self.output)
        if self.args.no_build or self.args.reuse_bin:
            print("Binaries: %s (not rebuilding)" % self.bin_dir)
        print("Libraries: %s" % " ".join(self.libraries))
        print("Cores: %s" % " ".join(map(str, self.cores)))
        print("Rounds: %d warmup + %d trials" % (self.args.warmup, self.args.trials))
        print("SMT mode: %s" % self.args.smt)
        for b in self.benchmarks:
            print("%s%s (binary %s, data %s)" % (b.name, " [ext]" if b.extended else "",
                                                b.binary_name, os.path.relpath(b.data_dir, PBBS_DIR)))
            for inp in b.inputs:
                print("    %s" % inp["label"])

    def main(self):
        if self.args.dry_run:
            self.dry_run()
            return 0
        self.preflight()
        for lib in self.libraries:
            os.makedirs(os.path.join(self.log_dir, lib), exist_ok=True)
        atexit.register(self.finalize)
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(sig, lambda s, f: sys.exit(128 + s))
        print("Output directory: %s" % self.output)
        if self.args.no_build or self.args.reuse_bin:
            print("Binaries: %s (not rebuilding)" % self.bin_dir)
            self.verify_binaries()
        else:
            self.build_all()
        if self.args.build_only:
            return 1 if self.failures else 0
        self.generate_data()
        self.smt.setup()
        try:
            self.apply_core_limit()
            self.run_all()
        finally:
            self.finalize()
        return 1 if self.failures else 0


if __name__ == "__main__":
    sys.exit(Runner(parse_args()).main())
