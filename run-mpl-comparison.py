#!/usr/bin/env python3
"""MPL's -ng benchmarks (heartbeat scheduling) vs spork-parlaylib, on parallel-ml-bench.

Compares MaPLe (MPL) with automatic parallelism management, the heartbeat
scheduler of Westrick et al., "Automatic Parallelism Management" (POPL'24),
running the -ng ("no grain") benchmarks of parallel-ml-bench's hb branch, with
spork-parlaylib running the C++ versions of the same benchmarks.  Only the
benchmarks whose C++ version runs the same algorithm as the -ng one are
included, so the timed sections differ in how par/par_do and
parfor/parallel_for are implemented (and in the languages and their runtimes):

  benchmark   MPL (mpl/bench)  C++ (cpp/)      timed section
  nqueens     nqueens-ng       nqueens.cpp     N-queens solutions: a par-split sum over the
                                               columns of each row, list of placed queens
  wc          wc-ng            wc.cpp          one reduce over (line break, word start) pairs
  linefit     linefit-ng       linefit.cpp     two reduces over an array of (x, y) points
  sparse-mxv  sparse-mxv-ng    sparse-mxv.cpp  map over separately allocated 100-entry rows,
                                               a reduce within each row
  msort       msort-ng         msort.cpp       mergesort: ping-pong buffers, halves in
                                               parallel, merges split by binary search
  primes      primes-ng        primes.cpp      recursive sieve: nested loops mark multiples,
                                               then a blocked filter of the flags
  bignum-add  bignum-add-ng    bignum-add.cpp  delayed digit sums, delayed carry scan, zip,
                                               materialize

spork's parlay::reduce is a heartbeat parallel loop with a reduction, as
SeqBasisNG.reduce is a par-split reduction.  The block sizes of the blocked
algorithms are constants in spork-parlaylib and runtime flags of the -ng
library (Grains.sml); --mpl-grains matched (the default) sets MPL's to
spork-parlaylib's: msort -mergesort-grain 47 -merge-grain 1999 (spork sorts
fewer than MERGE_SORT_BASE = 48 elements by insertion and merges fewer than
_merge_base = 2000 serially), primes -block-grain 1024 (pack's _block_size),
bignum-add -block-grain 2000 (the delayed sequences' block_size).
--mpl-grains default keeps the hb defaults (parfor 4, block 100, merge 100,
mergesort 10).  The other four have no blocked steps.

Setup, in mpl-artifact/ (see its README.md):
  mpl/                MPL v0.5.3, built from source (build/bin/mpl).  v0.5's
                      default fork-join is the heartbeat greedyWorkAmortizedFork
                      that the hb branch's local-hb config selects.
  parallel-ml-bench/  the hb branch, with mpllib v0.4.0 in mpl/lib (what smlpkg
                      sync installs) and inputs/words256.txt generated.  The C++
                      edits are left uncommitted there (git status, git diff).
MPL benchmarks are compiled as the hb branch's scripts/compile.py compiles
them with config/local-hb.json.  The C++ ones are compiled with the repo clang
and the PBBS harness's flags (heartbeat libraries: Spork plugin and startup
header; generic x86-64 unless --cpp-native) and linked with jemalloc.

Two sequential baselines run at 1 core only:
  mlton  the same -ng MPL sources built with MLton (apt) as the hb branch's
         config/mlton.json builds them: the C codegen (MPL's only backend),
         with a ForkJoin whose par calls both sides in sequence.  The -ng
         library still splits its loops recursively, with par a plain call.
  seq    the same C++ sources on cap-parlaylib with -DPARLAY_SEQUENTIAL (the
         PBBS harness's parlaylib-sequential): parallel_for is a plain loop
         and par_do calls its lambdas in sequence.
The report divides each parallel system's times by its language's baseline:
mpl's by mlton's, the C++ libraries' by seq's.

Each run is one process that repeats the timed section warmup + trials times
(-repeat, -warmup 0); the first warmup times are discarded.  --processes
repeats that per system, interleaving systems.  Multi-core runs use the first
P cores (one hardware thread each) with memory interleaved across NUMA nodes;
1-core runs are pinned to one CPU with memory bound to its node.  MPL pins its
workers to those CPUs (set-affinity); ParlayLib gets PARLAY_NUM_THREADS.  SMT
is switched off for the runs and restored afterwards (--smt keep leaves it).
Each system keeps its own heartbeat settings (MPL's runtime defaults are
500 us and 30 tokens; the C++ libraries' are compiled in) unless
--mpl-heartbeat-us or --mpl-heartbeat-tokens is given.

Outputs (in --output):
  report.md    Mean, min and max per benchmark, system and core count; ratios to
               the first parallel system; speedups; overheads and speedups
               against the sequential baselines; geometric means; output checks.
  results.tsv  One row per timed trial.
  config.json  Arguments, commands, heartbeat and CPU settings, revisions.
  logs/        Build and run output.   bin/  The binaries.
"""

import argparse
import concurrent.futures
import datetime
import fnmatch
import json
import math
import os
import re
import shlex
import signal
import statistics
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ARTIFACT = os.path.join(SCRIPT_DIR, "mpl-artifact")
SMT_CONTROL = "/sys/devices/system/cpu/smt/control"
CLANG = os.path.join(SCRIPT_DIR, "llvm-project", "build", "bin", "clang++")
SPORK_PLUGIN = os.path.join(SCRIPT_DIR, "SporkUnrollPass", "build", "SporkUnroll.so")
STARTUP_HEADER = os.path.join(SCRIPT_DIR, "pbbsbench", "common", "spork_benchmark_startup.h")
# Benchmark harnesses that must not run at the same time as this one.
HARNESSES = r"[r]un-(pbbsbench-scaling|microbench|hbc-comparison|mpl-comparison)\.py"

# C++ libraries: name -> (directory under SCRIPT_DIR, heartbeat-scheduled, extra
# flags).  The heartbeat libraries are built with the Spork plugin and the
# startup header.  stock is cap-parlaylib (ParlayLib with hbf's capture fixes) on
# its own scheduler, the PBBS harness's parlaylib; seq is the PBBS harness's
# parlaylib-sequential.
CPP_LIBS = {"spork": ("spork-parlaylib", True, ""),
            "stock": ("cap-parlaylib", False, ""), "seq": ("cap-parlaylib", False, "-DPARLAY_SEQUENTIAL")}
# Systems that build the MPL sources; the others build the C++ ones.
ML_SYSTEMS = ("mpl", "mlton")
# The sequential baselines, which run at 1 core only, and each parallel system's baseline.
SEQUENTIAL = ("mlton", "seq")
BASELINE = {"mpl": "mlton", "spork": "seq", "stock": "seq"}
CPP_CFLAGS = "-mcx16 -O3 -std=c++20 -DNDEBUG -stdlib=libc++ -w"
CPP_LFLAGS = "-pthread -ldl -L/usr/local/lib -ljemalloc -stdlib=libc++"

# What the hb branch's scripts/compile.py passes for config/local-hb.json, plus
# mpl/Makefile's DEFAULT_FLAGS.  v0.5 ignores PICK_FJ: its default fork-join is
# the greedy-work-amortized one that PICK_FJ selected in the hb compiler.
MPL_FLAGS = ("-mlb-path-var 'COMPAT mpl-hb' -default-type int64 -default-type word64 "
             "-disable-pass splitTypes1 -disable-pass splitTypes2 -detect-entanglement true "
             "-mlb-path-var 'PICK_FJ greedy-work-amortized' -mlb-path-var 'GRAINS small'")
# config/mlton.json: the C codegen and the sequential ForkJoin of compat/mlton.
MLTON_FLAGS = ("-mlb-path-var 'COMPAT mlton' -default-type int64 -default-type word64 "
               "-codegen c -mlb-path-var 'GRAINS small'")
# MPL's runtime defaults (runtime/gc/init.c), for the record.
MPL_HEARTBEAT_DEFAULTS = {"heartbeat-us": 500, "heartbeat-tokens": 30}


class Bench:
    def __init__(self, name, mpl, cpp, mpl_args, cpp_args, full, test, grains=""):
        self.name, self.mpl, self.cpp = name, mpl, cpp
        self.mpl_args, self.cpp_args = mpl_args, cpp_args   # {n}: the size
        self.sizes = {"full": full, "test": test}
        self.grains = grains   # MPL grain flags that match spork-parlaylib's constants


# Full sizes are the hb branch's exp-hb.json ones, except sparse-mxv's: exp-hb
# ran the CSR variant, so this is the 200M of exp.json and cpp-exp.json.
BENCHMARKS = [
    Bench("nqueens", "nqueens-ng", "nqueens", "-N {n}", "-N {n} -variant ng", "13", "10"),
    Bench("wc", "wc-ng", "wc", "-infile {n}", "-infile {n}",
          "{inputs}/words256.txt", "{inputs}/words64.txt"),
    Bench("linefit", "linefit-ng", "linefit", "-n {n} --check", "-n {n}", "100000000", "1000000"),
    Bench("sparse-mxv", "sparse-mxv-ng", "sparse-mxv", "-n {n} --check", "-n {n}", "200000000", "1000000"),
    Bench("msort", "msort-ng", "msort", "-N {n}", "-n {n}", "20000000", "1000000",
          "-mergesort-grain 47 -merge-grain 1999"),
    Bench("primes", "primes-ng", "primes", "-N {n}", "-n {n}", "100000000", "1000000", "-block-grain 1024"),
    Bench("bignum-add", "bignum-add-ng", "bignum-add", "-n {n} --check", "-n {n}", "100000000", "1000000",
          "-block-grain 2000"),
]

# Outputs that the MPL and C++ runs of a benchmark must agree on: label, MPL
# regex, C++ regex.  A value is the integers in the first group (at most 7).
CROSS_CHECKS = {
    "nqueens": [("solutions", r"^result (\d+)$", r"^result (\d+)$")],
    "wc": [("lines", r"^lines (\d+)$", r"^lines (\d+)$"), ("words", r"^words (\d+)$", r"^words (\d+)$")],
    "msort": [("first elements", r"^result \[([-\d, ]+)", r"^([-\d ]+) \.\.\.$")],
    "primes": [("primes", r"^number of primes (\d+)$", r"^number of primes (\d+)$")],
}
# Checks a program makes itself: side ("mpl" or "cpp"), regex, expected first group.
SELF_CHECKS = {
    "linefit": [("mpl", r"^correct\? (\S+)", "yes"), ("cpp", r"^correct (\S+)", "1")],
    "sparse-mxv": [("mpl", r"^correct\? (\S+)", "yes"), ("cpp", r"^result (\S+)", "100")],
    "bignum-add": [("mpl", r"^correct\? (\S+)", "yes")],
}


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--systems", default="mpl,spork,mlton,seq",
                   help="comma-separated, the first parallel one is the reference for ratios; any of "
                        "mpl, mlton, %s; mlton and seq are sequential and run at 1 core only "
                        "(default: mpl,spork,mlton,seq)" % ", ".join(CPP_LIBS))
    p.add_argument("--workers", default="1,80",
                   help="comma-separated core counts (default 1,80)")
    p.add_argument("--warmup", type=int, default=3, help="discarded repetitions per process (default 3)")
    p.add_argument("--trials", type=int, default=10, help="timed repetitions per process (default 10)")
    p.add_argument("--processes", type=int, default=1,
                   help="processes per benchmark, system and core count, systems interleaved (default 1)")
    p.add_argument("--only", action="append", default=[], metavar="GLOB",
                   help="only benchmarks matching this glob (repeatable)")
    p.add_argument("--size", choices=["full", "test"], default="full",
                   help="full: the hb branch's experiment sizes; test: small inputs for a quick check")
    p.add_argument("--mpl-grains", choices=["matched", "default"], default="matched",
                   help="matched: MPL block sizes set to spork-parlaylib's (default); "
                        "default: the hb branch's -ng defaults")
    p.add_argument("--mpl-heartbeat-us", type=int, default=None,
                   help="MPL heartbeat interval (default: MPL's, %d us)" % MPL_HEARTBEAT_DEFAULTS["heartbeat-us"])
    p.add_argument("--mpl-heartbeat-tokens", type=int, default=None,
                   help="MPL tokens per heartbeat (default: MPL's, %d)" % MPL_HEARTBEAT_DEFAULTS["heartbeat-tokens"])
    p.add_argument("--cpp-native", action="store_true",
                   help="build the C++ benchmarks for -march=native (default generic x86-64)")
    p.add_argument("--cpp-flags", default="", help="extra C++ compiler flags, e.g. -DSPORK_NO_CLOCK")
    p.add_argument("--one-core-cpu", type=int, default=None,
                   help="CPU for 1-core runs (default: the first core on NUMA node 0 other than CPU 0)")
    p.add_argument("--smt", choices=["off", "keep"], default="off",
                   help="off: disable SMT for the runs and restore it afterwards (needs sudo); "
                        "keep: leave SMT as it is (runs still use one hardware thread per core)")
    p.add_argument("--no-numa", action="store_true", help="do not interleave memory with numactl")
    p.add_argument("--timeout", type=int, default=3600, help="seconds per run process (default 3600)")
    p.add_argument("--build-jobs", type=int, default=8, help="parallel compiles (default 8)")
    p.add_argument("--artifact", default=DEFAULT_ARTIFACT, help="the mpl-artifact directory")
    p.add_argument("--output", default=None, help="output directory (default mpl-comparison/<timestamp>)")
    p.add_argument("--reuse-bin", metavar="DIR", default=None,
                   help="skip building; take binaries from DIR/bin of an earlier run")
    p.add_argument("--allow-concurrent", action="store_true",
                   help="run even if another benchmark harness is running")
    p.add_argument("--dry-run", action="store_true", help="print the plan and commands, and exit")
    return p.parse_args()


def sh(cmd, log=None, cwd=None, env=None, timeout=None):
    """Run a shell command; append its output to log.  Returns the exit code."""
    with open(log, "a") if log else open(os.devnull, "w") as f:
        f.write("$ %s\n" % cmd); f.flush()
        try:
            return subprocess.run(cmd, shell=True, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT,
                                  timeout=timeout, executable="/bin/bash").returncode
        except subprocess.TimeoutExpired:
            f.write("TIMEOUT after %s s\n" % timeout)
            return 124


def capture(cmd, cwd=None):
    try:
        return subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True,
                              executable="/bin/bash").stdout.strip()
    except OSError:
        return ""


def parse_cpu_list(text):
    cpus = []
    for part in text.strip().split(","):
        if part:
            lo, _, hi = part.partition("-")
            cpus.extend(range(int(lo), int(hi or lo) + 1))
    return cpus


def format_cpu_list(cpus):
    """[0, 1, 2, 5] -> "0-2,5" (the syntax numactl and taskset take)."""
    runs = []
    for c in cpus:
        if runs and c == runs[-1][1] + 1:
            runs[-1][1] = c
        else:
            runs.append([c, c])
    return ",".join(str(lo) if lo == hi else "%d-%d" % (lo, hi) for lo, hi in runs)


def core_cpus():
    """Online CPUs, one per core: the lowest-numbered online thread of each."""
    online = parse_cpu_list(open("/sys/devices/system/cpu/online").read())
    online_set = set(online)
    cores = []
    for c in online:
        path = "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list" % c
        siblings = parse_cpu_list(open(path).read()) if os.path.exists(path) else [c]
        if c == min(s for s in siblings + [c] if s in online_set):
            cores.append(c)
    return cores


def cpu_node(cpu):
    for name in os.listdir("/sys/devices/system/cpu/cpu%d" % cpu):
        if name.startswith("node") and name[4:].isdigit():
            return int(name[4:])
    return None


def ancestor_pids():
    """This process and its ancestors."""
    pids, pid = set(), os.getpid()
    while pid > 1:
        pids.add(str(pid))
        try:
            with open("/proc/%d/stat" % pid) as f:
                pid = int(f.read().rsplit(")", 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            break
    return pids


def other_harnesses():
    """Benchmark harness processes other than this run.  This process's ancestors
    and process group (the pipeline it runs in, whose shells carry the whole
    command line) can name the script too, so they do not count."""
    mine, group = ancestor_pids(), os.getpgid(0)
    others = []
    for line in capture("pgrep -af '%s'" % HARNESSES).splitlines():
        pid = line.split()[0]
        try:
            if pid not in mine and os.getpgid(int(pid)) != group:
                others.append(line)
        except OSError:
            pass   # exited meanwhile
    return others


def geomean(xs):
    return math.exp(sum(map(math.log, xs)) / len(xs))


def git_revision(path, untracked=False):
    rev = capture("git -C %s rev-parse --short HEAD" % shlex.quote(path))
    dirty = capture("git -C %s status --porcelain%s" % (shlex.quote(path), "" if untracked else
                                                        " --untracked-files=no"))
    return rev + ("+uncommitted" if dirty else "") if rev else "unknown"


class Smt:
    """Switches SMT off for the runs and always puts it back."""

    def __init__(self, mode):
        self.mode, self.saved = mode, None

    def disable(self):
        if self.mode != "off":
            return
        if not os.path.exists(SMT_CONTROL):
            sys.exit("error: %s not found; use --smt keep" % SMT_CONTROL)
        self.saved = open(SMT_CONTROL).read().strip()
        if self.saved in ("on", "off"):
            self._write("off")

    def restore(self):
        if self.saved in ("on", "off") and open(SMT_CONTROL).read().strip() != self.saved:
            self._write(self.saved)
            print("SMT restored to %s" % self.saved)
        self.saved = None

    @staticmethod
    def _write(value):
        if subprocess.run("echo %s | sudo -n tee %s > /dev/null" % (value, SMT_CONTROL), shell=True).returncode:
            sys.exit("error: could not write %s (needs passwordless sudo); use --smt keep" % SMT_CONTROL)


class Comparison:
    def __init__(self, args):
        self.args = args
        self.systems = [s.strip() for s in args.systems.split(",") if s.strip()]
        bad = [s for s in self.systems if s not in ML_SYSTEMS and s not in CPP_LIBS]
        if bad or len(set(self.systems)) != len(self.systems) or not self.systems:
            sys.exit("error: --systems: unknown or repeated: %s" % args.systems)
        try:
            self.workers = sorted({int(w) for w in args.workers.split(",") if w.strip()})
        except ValueError:
            sys.exit("error: --workers: expected comma-separated integers: %s" % args.workers)
        if not self.workers or self.workers[0] < 1:
            sys.exit("error: --workers: expected positive core counts: %s" % args.workers)
        self.benches = [b for b in BENCHMARKS
                        if not args.only or any(fnmatch.fnmatch(b.name, g) for g in args.only)]
        if not self.benches:
            sys.exit("error: no benchmarks selected")
        self.artifact = os.path.abspath(args.artifact)
        self.pml = os.path.join(self.artifact, "parallel-ml-bench")
        self.mplc = os.path.join(self.artifact, "mpl", "build", "bin", "mpl")
        self.inputs = os.path.join(self.pml, "inputs")
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.out = os.path.abspath(args.output or os.path.join(SCRIPT_DIR, "mpl-comparison", stamp))
        self.bin = os.path.join(self.out, "bin")
        self.logs = os.path.join(self.out, "logs")
        self.smt = Smt(args.smt)
        self.cores = core_cpus()   # numbering is the same with SMT on or off
        self.one_core_cpu = None
        self.times = {}      # (workers, benchmark, system) -> [seconds]
        self.checks = {}     # (benchmark, check) -> {system: set of values}
        self.commands = {}   # "workers benchmark system" -> run command
        self.failures = []

    # ---- commands ----------------------------------------------------------
    def size(self, b):
        return b.sizes[self.args.size].format(inputs=self.inputs)

    def program_args(self, b, system):
        if system in ML_SYSTEMS:
            args = b.mpl_args.format(n=shlex.quote(self.size(b)))
            return args + (" " + b.grains if b.grains and self.args.mpl_grains == "matched" else "")
        return b.cpp_args.format(n=shlex.quote(self.size(b)))

    def binary(self, b, system):
        return os.path.join(self.bin, "%s-%s" % (b.name, system))

    @staticmethod
    def runs_at(system, workers):
        """Whether a system runs at this core count (the sequential baselines only at 1)."""
        return workers == 1 or system not in SEQUENTIAL

    def build_command(self, b, system):
        """The compile command for a benchmark and system, and where to run it."""
        if system in ML_SYSTEMS:
            compiler, flags = (shlex.quote(self.mplc), MPL_FLAGS) if system == "mpl" else ("mlton", MLTON_FLAGS)
            return ("%s %s -output %s bench/%s/%s.mlb" % (
                compiler, flags, shlex.quote(self.binary(b, system)), b.mpl, b.mpl),
                os.path.join(self.pml, "mpl"))
        lib_dir, heartbeat, extra = CPP_LIBS[system]
        flags = CPP_CFLAGS + (" " + extra if extra else "")
        if heartbeat:
            flags += " -fpass-plugin=%s -DVOLATILE_UNROLL= -include %s" % (SPORK_PLUGIN, STARTUP_HEADER)
        flags += " -I%s" % os.path.join(SCRIPT_DIR, lib_dir, "include")
        if self.args.cpp_native:
            flags += " -march=native"
        if self.args.cpp_flags:
            flags += " " + self.args.cpp_flags
        src = os.path.join(self.pml, "cpp", b.cpp + ".cpp")
        return ("%s %s %s -o %s %s" % (CLANG, flags, src, shlex.quote(self.binary(b, system)), CPP_LFLAGS),
                os.path.join(self.pml, "cpp"))

    def placement(self, workers):
        """The numactl/taskset prefix for a run on this many cores, and its CPUs."""
        if workers == 1:
            cpu = self.one_core_cpu
            node = cpu_node(cpu)
            return ("numactl --physcpubind=%d%s" % (cpu, "" if node is None else " --membind=%d" % node),
                    [cpu])
        cpus = self.cores[:workers]
        cpu_list = format_cpu_list(cpus)
        if self.args.no_numa:
            return "taskset -c %s" % cpu_list, cpus
        return "numactl --physcpubind=%s --interleave=all" % cpu_list, cpus

    def mpl_runtime(self, workers, cpus):
        """MPL's runtime arguments: processors, pinning to cpus, heartbeat overrides."""
        rts = ["procs", str(workers)]
        stride = cpus[1] - cpus[0] if len(cpus) > 1 else 1
        # set-affinity places worker i on CPU base + i * stride.
        if stride > 0 and all(c == cpus[0] + i * stride for i, c in enumerate(cpus)):
            rts += ["set-affinity", "affinity-base", str(cpus[0]), "affinity-stride", str(stride)]
        if self.args.mpl_heartbeat_us is not None:
            rts += ["heartbeat-us", str(self.args.mpl_heartbeat_us)]
        if self.args.mpl_heartbeat_tokens is not None:
            rts += ["heartbeat-tokens", str(self.args.mpl_heartbeat_tokens)]
        return "@mpl %s --" % " ".join(rts)

    def run_command(self, workers, b, system):
        prefix, cpus = self.placement(workers)
        parts = [prefix, shlex.quote(self.binary(b, system))]
        if system == "mpl":
            parts.append(self.mpl_runtime(workers, cpus))
        parts += [self.program_args(b, system),
                  "-repeat %d -warmup 0" % (self.args.warmup + self.args.trials)]
        return " ".join(parts)

    # ---- checks ------------------------------------------------------------
    def preflight(self):
        others = other_harnesses()
        if others and not self.args.allow_concurrent:
            sys.exit("error: another benchmark harness is running (--allow-concurrent to run anyway):\n  "
                     + "\n  ".join(others))
        need = []
        if not self.args.reuse_bin:
            if "mpl" in self.systems:
                need.append((self.mplc, "MPL not built (make in mpl-artifact/mpl)"))
            if "mlton" in self.systems and not capture("command -v mlton"):
                sys.exit("error: mlton not found (sudo apt-get install mlton libgmp-dev)")
            if any(s in ML_SYSTEMS for s in self.systems):
                need.append((os.path.join(self.pml, "mpl", "lib", "github.com", "mpllang", "mpllib",
                                          "sources.mpl.mlb"), "mpllib v0.4.0 missing from mpl/lib"))
            if any(s in CPP_LIBS for s in self.systems):
                need.append((CLANG, "repo clang not built"))
            if any(CPP_LIBS[s][1] for s in self.systems if s in CPP_LIBS):
                need += [(SPORK_PLUGIN, "Spork plugin not built"), (STARTUP_HEADER, "startup header missing")]
            need += [(os.path.join(SCRIPT_DIR, CPP_LIBS[s][0], "include", "parlay"), "library missing")
                     for s in self.systems if s in CPP_LIBS]
        need += [(self.size(b), "input missing (in inputs/: cat words64.txt words64.txt words64.txt "
                                "words64.txt > words256.txt)")
                 for b in self.benches if "{inputs}" in b.sizes[self.args.size]]
        for path, msg in need:
            if not os.path.exists(path):
                sys.exit("error: %s: %s" % (msg, path))
        if self.workers[-1] > len(self.cores):
            sys.exit("error: --workers %d exceeds the %d cores" % (self.workers[-1], len(self.cores)))
        if 1 not in self.workers and any(s in SEQUENTIAL for s in self.systems):
            print("note: %s run at 1 core only, and --workers has no 1"
                  % ", ".join(s for s in self.systems if s in SEQUENTIAL))
        if not self.args.no_numa and not capture("command -v numactl"):
            sys.exit("error: numactl not found (or pass --no-numa)")
        if 1 in self.workers:
            self.choose_one_core_cpu()

    def choose_one_core_cpu(self):
        if self.args.one_core_cpu is not None:
            if self.args.one_core_cpu not in self.cores:
                sys.exit("error: --one-core-cpu %d is not a usable CPU (one per core: %s)"
                         % (self.args.one_core_cpu, ",".join(map(str, self.cores))))
            self.one_core_cpu = self.args.one_core_cpu
        else:
            # CPU 0 takes most housekeeping interrupts; prefer another core on node 0.
            candidates = [c for c in self.cores if c != 0 and cpu_node(c) in (0, None)] or self.cores
            self.one_core_cpu = candidates[0]

    def record_checks(self, b, system, out):
        side = "mpl" if system in ML_SYSTEMS else "cpp"
        for label, mpl_re, cpp_re in CROSS_CHECKS.get(b.name, []):
            found = re.findall(mpl_re if side == "mpl" else cpp_re, out, re.M)
            value = " ".join(re.findall(r"-?\d+", found[-1])[:7]) if found else "missing"
            self.checks.setdefault((b.name, label), {}).setdefault(system, set()).add(value)
        for check_side, regex, expected in SELF_CHECKS.get(b.name, []):
            if check_side == side:
                found = re.findall(regex, out, re.M)
                value = found[-1] if found else "missing"
                self.checks.setdefault((b.name, "self-check"), {}).setdefault(system, set()).add(
                    "ok" if value == expected else "FAILED (%s)" % value)

    def check_rows(self):
        """(benchmark, check, {system: values}, status) for the report."""
        rows = []
        for (name, label), per_system in self.checks.items():
            values = [v for vs in per_system.values() for v in vs]
            if label == "self-check":
                status = "ok" if all(v == "ok" for v in values) else "FAILED"
            else:
                status = "agree" if len(set(values)) == 1 and "missing" not in values else "DISAGREE"
            rows.append((name, label, per_system, status))
        return rows

    # ---- build -------------------------------------------------------------
    def build(self):
        os.makedirs(self.bin, exist_ok=True)

        def one(b, system):
            cmd, cwd = self.build_command(b, system)
            return sh(cmd, os.path.join(self.logs, "build-%s-%s.log" % (b.name, system)), cwd=cwd)

        jobs = [(b, s) for b in self.benches for s in self.systems]
        with concurrent.futures.ThreadPoolExecutor(max(1, self.args.build_jobs)) as pool:
            futures = {pool.submit(one, b, s): (b, s) for b, s in jobs}
            for future in concurrent.futures.as_completed(futures):
                b, s = futures[future]
                rc = future.result()
                print("  build %-11s %-6s %s" % (b.name, s, "ok" if rc == 0 else
                                                 "FAILED (see logs/build-%s-%s.log)" % (b.name, s)), flush=True)
                if rc:
                    self.failures.append((b.name, s, "-", "build failed"))

    # ---- run ---------------------------------------------------------------
    def run(self):
        for workers in self.workers:
            print("== %d core%s: %s" % (workers, "" if workers == 1 else "s", self.placement(workers)[0]),
                  flush=True)
            env = dict(os.environ, PARLAY_NUM_THREADS=str(workers))
            for b in self.benches:
                for proc in range(self.args.processes):
                    for system in self.systems:
                        if not self.runs_at(system, workers) or not os.path.exists(self.binary(b, system)):
                            continue
                        cmd = self.run_command(workers, b, system)
                        self.commands["%d %s %s" % (workers, b.name, system)] = cmd
                        log = os.path.join(self.logs, "run-%s-%s-p%d-%d.log" % (b.name, system, workers, proc))
                        rc = sh(cmd, log, cwd=self.out, env=env, timeout=self.args.timeout)
                        out = open(log).read()
                        times = [float(t) for t in re.findall(r"^time (\d+(?:\.\d+)?)s$", out, re.M)]
                        trials = times[self.args.warmup:]
                        if rc == 0:
                            self.record_checks(b, system, out)
                        if rc or len(trials) < self.args.trials:
                            self.failures.append((b.name, system, workers, "rc=%d, %d times" % (rc, len(times))))
                            print("  %-11s %-6s FAILED rc=%d (%d times; see %s)" % (
                                b.name, system, rc, len(times), os.path.relpath(log, self.out)), flush=True)
                            continue
                        self.times.setdefault((workers, b.name, system), []).extend(trials)
                        print("  %-11s %-6s mean %.4f s  (%s)" % (
                            b.name, system, statistics.mean(trials), " ".join("%.4f" % t for t in trials)),
                            flush=True)

    # ---- report ------------------------------------------------------------
    def mean(self, workers, name, system):
        ts = self.times.get((workers, name, system))
        return statistics.mean(ts) if ts else None

    @staticmethod
    def ratio(a, b):
        return a / b if a is not None and b else None

    def baseline_section(self, parallel):
        """Report lines comparing each parallel system with its sequential baseline."""
        baselines = [s for s in SEQUENTIAL if s in self.systems]
        if 1 not in self.workers or not baselines:
            return []
        paired = [s for s in parallel if BASELINE.get(s) in baselines]
        # (header, value for a benchmark, format, whether the geomean row shows it)
        cols = [("%s TS" % s, lambda n, s=s: self.mean(1, n, s), "%.4f", False) for s in baselines]
        if len(baselines) == 2:
            cols.append(("mlton/seq", lambda n: self.ratio(self.mean(1, n, "mlton"), self.mean(1, n, "seq")),
                         "%.2f", True))
        cols += [("%s T1/TS" % s, lambda n, s=s: self.ratio(self.mean(1, n, s), self.mean(1, n, BASELINE[s])),
                  "%.2f", True) for s in paired]
        cols += [("%s TS/T%d" % (s, w), lambda n, s=s, w=w: self.ratio(self.mean(1, n, BASELINE[s]),
                                                                      self.mean(w, n, s)), "%.1f", True)
                 for w in self.workers if w > 1 for s in paired]
        lines = ["", "## Sequential baselines", "",
                 "TS: mean time of the sequential baseline (mlton for mpl, seq for the C++ libraries).  "
                 "T1/TS: a parallel system's overhead on 1 core.  TS/TP: its speedup over the baseline "
                 "on P cores.", "",
                 "| benchmark | " + " | ".join(c[0] for c in cols) + " |", "|" + "---|" * (1 + len(cols))]
        values = {c[0]: [] for c in cols}
        for b in self.benches:
            cells = []
            for head, get, fmt, _ in cols:
                v = get(b.name)
                if v is not None:
                    values[head].append(v)
                cells.append(fmt % v if v is not None else "—")
            lines.append("| %s | %s |" % (b.name, " | ".join(cells)))
        lines.append("| geomean | %s |" % " | ".join(
            fmt % geomean(values[head]) if in_mean and values[head] else "" for head, _, fmt, in_mean in cols))
        return lines

    def heartbeats(self):
        """Each system's heartbeat settings."""
        mpl = dict(MPL_HEARTBEAT_DEFAULTS)
        if self.args.mpl_heartbeat_us is not None:
            mpl["heartbeat-us"] = self.args.mpl_heartbeat_us
        if self.args.mpl_heartbeat_tokens is not None:
            mpl["heartbeat-tokens"] = self.args.mpl_heartbeat_tokens
        result = {}
        for s in self.systems:
            if s == "mpl":
                result[s] = "%d us, %d tokens" % (mpl["heartbeat-us"], mpl["heartbeat-tokens"])
            elif s in CPP_LIBS and CPP_LIBS[s][1]:
                header = os.path.join(SCRIPT_DIR, CPP_LIBS[s][0], "include", "parlay", "internal",
                                      "spork_scheduler.h")
                get = lambda name: capture("grep -oP '%s = \\K[0-9]+' %s | head -1" % (name, shlex.quote(header)))
                interval, factor, tokens = (get("HEARTBEAT_INTERVAL_US"), get("HEARTBEAT_COARSENING_FACTOR"),
                                            get("TOKENS_PER_HEARTBEAT"))
                if not tokens and factor:
                    tokens = str((1 << int(factor)) - 1)
                result[s] = "%s us, %s tokens" % (interval or "?", tokens or "?")
        return result

    def revisions(self):
        revs = {s: git_revision(os.path.join(SCRIPT_DIR, CPP_LIBS[s][0])) for s in self.systems if s in CPP_LIBS}
        if "mpl" in self.systems:
            revs["mpl"] = capture("%s 2>&1 | head -1" % shlex.quote(self.mplc)) or "unknown"
        if "mlton" in self.systems:
            revs["mlton"] = capture("mlton 2>&1 | head -1") or "unknown"
        if any(s in ML_SYSTEMS for s in self.systems):
            revs["mpllib"] = capture("git -C %s describe --tags" % shlex.quote(os.path.join(self.artifact, "mpllib")))
        revs["parallel-ml-bench"] = git_revision(self.pml, untracked=True)
        return revs

    def write_outputs(self):
        with open(os.path.join(self.out, "results.tsv"), "w") as f:
            f.write("workers\tbenchmark\tsystem\ttrial\tseconds\n")
            for (workers, name, system), ts in self.times.items():
                for i, t in enumerate(ts):
                    f.write("%d\t%s\t%s\t%d\t%.6f\n" % (workers, name, system, i, t))
        heartbeats, revisions = self.heartbeats(), self.revisions()
        config = {
            "date": datetime.datetime.now().isoformat(timespec="seconds"),
            "args": vars(self.args), "systems": self.systems, "workers": self.workers,
            "benchmarks": [b.name for b in self.benches],
            "build_commands": {"%s %s" % (b.name, s): self.build_command(b, s)[0]
                               for b in self.benches for s in self.systems},
            "run_commands": self.commands,
            "heartbeats": heartbeats, "revisions": revisions,
            "cpp_edits": capture("git -C %s status --porcelain" % shlex.quote(self.pml)).splitlines(),
            "one_core_cpu": self.one_core_cpu, "cores": len(self.cores),
            "compilers": {"clang": capture("%s --version | head -1" % CLANG),
                          "mpl": revisions.get("mpl", "")},
            "cpu": capture("lscpu | grep 'Model name' | sed 's/.*: *//'"),
            "checks": [{"benchmark": n, "check": l, "values": {s: sorted(v) for s, v in vals.items()},
                        "status": st} for n, l, vals, st in self.check_rows()],
            "failures": self.failures,
        }
        with open(os.path.join(self.out, "config.json"), "w") as f:
            json.dump(config, f, indent=2, default=str)

        parallel = [s for s in self.systems if s not in SEQUENTIAL]
        ref = (parallel or self.systems)[0]
        grains = ("matched to spork-parlaylib (%s)" % "; ".join("%s %s" % (b.name, b.grains)
                                                                 for b in self.benches if b.grains)
                  if self.args.mpl_grains == "matched" else "the hb branch's -ng defaults")
        placement = []
        if 1 in self.workers:
            placement.append("1-core runs on CPU %d, memory on its node" % self.one_core_cpu)
        if self.workers[-1] > 1:
            placement.append("multi-core runs on the first P cores, memory %s"
                             % ("not interleaved" if self.args.no_numa else "interleaved"))
        lines = ["# MPL vs spork comparison", "",
                 "- Systems: %s (ratios relative to `%s`)" % (", ".join(
                     "`%s`%s" % (s, " (sequential, 1 core)" if s in SEQUENTIAL else "") for s in self.systems), ref),
                 "- Cores: %s; SMT %s; %s" % (", ".join(map(str, self.workers)), self.args.smt,
                                              "; ".join(placement)),
                 "- Repetitions: %d warmup (discarded) + %d timed, %d process(es) per system" % (
                     self.args.warmup, self.args.trials, self.args.processes),
                 "- Sizes: `%s`; MPL grains: %s" % (self.args.size, grains),
                 "- Heartbeats: %s" % "; ".join("%s %s" % kv for kv in heartbeats.items()),
                 "- C++: %s%s" % ("-march=native" if self.args.cpp_native else "generic x86-64",
                                  ", flags %s" % self.args.cpp_flags if self.args.cpp_flags else ""),
                 "- Revisions: %s" % ", ".join("%s %s" % kv for kv in revisions.items())]
        for workers in self.workers:
            systems = [s for s in self.systems if self.runs_at(s, workers)]
            others = [s for s in systems if s != ref]
            lines += ["", "## %d core%s" % (workers, "" if workers == 1 else "s"), "",
                      "Mean seconds per repetition [min-max]; ratio = mean / `%s` mean." % ref, "",
                      "| benchmark | " + " | ".join(systems) + " | " +
                      " | ".join("%s/%s" % (s, ref) for s in others) + " |",
                      "|" + "---|" * (1 + len(systems) + len(others))]
            ratios = {s: [] for s in others}
            for b in self.benches:
                cells = []
                for s in systems:
                    ts = self.times.get((workers, b.name, s))
                    cells.append("%.4f [%.4f-%.4f]" % (statistics.mean(ts), min(ts), max(ts)) if ts else "—")
                rcells = []
                for s in others:
                    r = self.ratio(self.mean(workers, b.name, s), self.mean(workers, b.name, ref))
                    if r is not None:
                        ratios[s].append(r)
                    rcells.append("%.3f" % r if r is not None else "—")
                lines.append("| %s | %s |" % (b.name, " | ".join(cells + rcells)))
            lines += ["", "Geometric mean of ratios: " + (", ".join(
                "%s/%s %.3f (%d benchmarks)" % (s, ref, geomean(rs), len(rs))
                for s, rs in ratios.items() if rs) or "—")]
        if 1 in self.workers and len(self.workers) > 1 and parallel:
            lines += ["", "## Speedups", "", "Mean 1-core time / mean P-core time.", "",
                      "| benchmark | " + " | ".join("%s P=%d" % (s, w) for w in self.workers[1:]
                                                   for s in parallel) + " |",
                      "|" + "---|" * (1 + len(parallel) * (len(self.workers) - 1))]
            for b in self.benches:
                cells = []
                for w in self.workers[1:]:
                    for s in parallel:
                        r = self.ratio(self.mean(1, b.name, s), self.mean(w, b.name, s))
                        cells.append("%.1f" % r if r is not None else "—")
                lines.append("| %s | %s |" % (b.name, " | ".join(cells)))
        lines += self.baseline_section(parallel)
        rows = self.check_rows()
        if rows:
            lines += ["", "## Output checks", "",
                      "| benchmark | check | " + " | ".join(self.systems) + " | status |",
                      "|" + "---|" * (3 + len(self.systems))]
            for name, label, per_system, status in rows:
                lines.append("| %s | %s | %s | %s |" % (name, label, " | ".join(
                    ", ".join(sorted(per_system.get(s, []))) or "—" for s in self.systems), status))
        if self.failures:
            lines += ["", "## Failures", ""] + ["- %s %s (cores %s): %s" % f for f in self.failures]
        report = "\n".join(lines) + "\n"
        with open(os.path.join(self.out, "report.md"), "w") as f:
            f.write(report)
        print("\n" + report)

    # ---- driver ------------------------------------------------------------
    def main(self):
        self.preflight()
        print("Benchmarks: %s" % ", ".join(b.name for b in self.benches))
        print("Systems:    %s   cores %s   %d warmup + %d trials x %d process(es)   SMT %s" % (
            ", ".join(self.systems), ",".join(map(str, self.workers)), self.args.warmup, self.args.trials,
            self.args.processes, self.args.smt))
        print("Sizes:      %s   MPL grains %s" % (self.args.size, self.args.mpl_grains))
        if self.one_core_cpu is not None:
            print("1 core:     CPU %d (NUMA node %s)" % (self.one_core_cpu, cpu_node(self.one_core_cpu)))
        print("Output:     %s" % self.out)
        if self.args.dry_run:
            if not self.args.reuse_bin:
                print("\nBuild:")
                for b in self.benches:
                    for s in self.systems:
                        cmd, cwd = self.build_command(b, s)
                        print("  (cd %s) %s" % (cwd, cmd))
            print("\nRuns (C++ with PARLAY_NUM_THREADS=P):")
            for w in self.workers:
                for b in self.benches:
                    for s in self.systems:
                        if self.runs_at(s, w):
                            print("  " + self.run_command(w, b, s))
            return
        os.makedirs(self.logs, exist_ok=True)
        if self.args.reuse_bin:
            src = os.path.join(os.path.abspath(self.args.reuse_bin), "bin")
            if not os.path.isdir(src):
                sys.exit("error: --reuse-bin: %s has no bin/" % src)
            self.bin = src
        else:
            print("== build")
            self.build()
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(sig, lambda s, f: sys.exit(128 + s))
        try:
            self.smt.disable()
            print("== run (SMT %s, %d CPUs online)" % (open(SMT_CONTROL).read().strip()
                                                       if os.path.exists(SMT_CONTROL) else "?", os.cpu_count()))
            self.run()
        finally:
            self.smt.restore()
            self.write_outputs()


if __name__ == "__main__":
    Comparison(parse_args()).main()
