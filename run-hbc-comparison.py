#!/usr/bin/env python3
"""HBC vs spork-parlaylib on the loop benchmarks of the HBC artifact.

Compares the heartbeat compiler of Su et al., "Compiling Loop-Based Nested
Parallelism for Irregular Workloads" (ASPLOS'24, artifact
https://zenodo.org/records/10551774, unpacked under hbc-artifact/), with
spork-parlaylib, on the artifact's hbc_irregular and hbc_regular benchmark
sets.

Every library runs the benchmark's own source file, on the artifact's cached
inputs and with its timing loop (utility::run / taskparts::benchmark), so only
the implementation differs:
  hbc      the artifact's HBC build, exactly as scripts/evaluation/scaling.sh
           makes it (make hbc ... ACC=true CHUNKSIZE=1; for kmeans the manual
           heartbeat version, make hbm, as the artifact does).  Its Makefiles
           compile the sources to bitcode without -march=native and pass it
           only to the final link, which leaves every function the bitcode
           already holds compiled for generic x86-64; --hbc-native adds
           -march=native to the bitcode step as well (HBC's transformation
           then fails on cg and the spmv benchmarks)
  spork    the USE_PARLAY implementation built against spork-parlaylib (every
           loop HBC parallelizes is a parallel loop there, inner loops included)
  cilk-auto
           the same against cap-parlaylib on OpenCilk with every grain left to
           OpenCilk (-DPARLAY_IGNORE_GRANULARITY), as the PBBS harness's
           parlaylib-opencilk-auto
  stock, cilk, stock14
           the same USE_PARLAY implementation against cap-parlaylib (ParlayLib
           with hbf's capture fixes, the PBBS harness's parlaylib) on its own
           scheduler, on OpenCilk, and on its own scheduler built with clang 14
           (HBC's compiler)
and two sequential baselines, which run on one core only:
  baseline the artifact's serial code (make baseline, -DUSE_BASELINE), built
           like hbc (clang 14, the same targets): hbc's baseline
  seq      the USE_PARLAY implementation on cap-parlaylib with
           -DPARLAY_SEQUENTIAL (no scheduler; repo clang, spork's target): the
           ParlayLib libraries' baseline
All builds use the artifact's code generation flags (-O3 -march=native, no
vectorization, no FP contraction) and its jemalloc, except that spork and seq
target generic x86-64 by default (--spork-native builds both for
-march=native like the others).  --generic compiles every build for generic
x86-64 instead, HBC's final link included.  --spork-vectorize lets the Spork
plugin vectorize spork's parallel loops (it vectorizes a probe of each loop
and inlines it in the loop's place); everything else stays unvectorized.

Each run is one process that repeats the benchmark's timed body
``warmup + trials`` times with no time-based warmup; the first ``warmup``
exectimes are discarded.  ``--processes`` repeats that per library,
interleaving libraries.  ``--workers`` takes a list (e.g. 1,80).  Runs on
P > 1 workers are pinned to CPUs 0..P-1 with memory interleaved across NUMA
nodes (as the artifact's scripts do); 1-worker runs, the baselines' included,
run on one CPU with memory on its node (--one-core-cpu; by default the first
CPU on node 0 other than CPU 0, as the artifact runs its baselines on CPU 2).
SMT is switched off for the runs and restored afterwards (``--smt keep`` to
leave it).

Outputs (in --output):
  report.md    Per core count: mean, min and max per benchmark and library,
               ratio to the first parallel library, geometric means.  With
               1 in --workers and the baselines: each library's overhead on
               one core over its baseline (T1/TS) and speedup over it (TS/TP).
  results.tsv  One row per timed trial.
  config.json  Arguments, heartbeat and CPU settings, compiler versions, git
               revision of each library.
  logs/        Build and run output.   bin/  The binaries.

Prerequisites: the artifact set up as in hbc-artifact (LLVM 14 under
/usr/lib/llvm-14, HBC built, env.sh written), and the USE_PARLAY
implementations and scripts/parlay/build.sh in the artifact.
"""

import argparse
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
DEFAULT_ARTIFACT = os.path.join(SCRIPT_DIR, "hbc-artifact", "hbc-asplos24-artifact")
LLVM14 = "/usr/lib/llvm-14"
SMT_CONTROL = "/sys/devices/system/cpu/smt/control"

# name, directory (relative to the artifact), HBC make target and variables,
# extra macros for the USE_PARLAY build.  The spmv-* benchmarks share one
# directory, so builds run one at a time.
BENCHMARKS = [
    ("cg", "benchmarks/hbc_irregular/cg", "hbc", "", "-I{artifact}/externals/matrix-market/include"),
    ("mandelbrot", "benchmarks/tpal/mandelbrot", "hbc", "", ""),
    ("mandelbulb", "benchmarks/hbc_irregular/mandelbulb", "hbc", "", ""),
    ("spmv-arrowhead", "benchmarks/tpal/spmv-random", "hbc", "INPUT_CLASS=ARROWHEAD", "-DSPMV_ARROWHEAD"),
    ("spmv-powerlaw", "benchmarks/tpal/spmv-random", "hbc", "INPUT_CLASS=POWERLAW", "-DSPMV_POWERLAW"),
    ("floyd-warshall", "benchmarks/tpal/floyd-warshall", "hbc", "", ""),
    ("kmeans", "benchmarks/tpal/kmeans", "hbm", "", ""),
    ("plus-reduce-array", "benchmarks/tpal/plus-reduce-array", "hbc", "", ""),
    ("spmv-random", "benchmarks/tpal/spmv-random", "hbc", "INPUT_CLASS=RANDOM", "-DSPMV_RANDOM"),
    ("srad", "benchmarks/tpal/srad", "hbc", "", ""),
]
PARLAY_LIBS = {"spork": "spork", "stock": "stock", "cilk": "cilk", "cilk-auto": "cilk-auto",
               "stock14": "stock14", "seq": "seq"}
LIBRARY_DIRS = {"spork": "spork-parlaylib", "stock": "cap-parlaylib", "cilk": "cap-parlaylib",
                "cilk-auto": "cap-parlaylib", "stock14": "cap-parlaylib", "seq": "cap-parlaylib"}
# The sequential baselines (run on one core only) and each parallel library's baseline.
SEQUENTIAL = ("baseline", "seq")
BASELINE = {"hbc": "baseline", "spork": "seq", "stock": "seq", "cilk": "seq", "cilk-auto": "seq",
            "stock14": "seq"}
# Libraries whose heartbeat settings are compiled in, read from their headers for the record.
HEARTBEAT_LIBS = ("spork",)
# The target of --generic.  The last -march on a command line wins, so it is
# appended to the ParlayLib builds' flags; HBC's final link takes OPTS, which
# every benchmark Makefile sets to -O3 -march=native -fno-vectorize
# -fno-slp-vectorize, so --generic gives make that OPTS with this target.
GENERIC_MARCH = "-march=x86-64 -mtune=generic"
HBC_GENERIC_OPTS = "OPTS=-O3 %s -fno-vectorize -fno-slp-vectorize" % GENERIC_MARCH
# ParlayLib libraries built for generic x86-64 even without --generic (seq
# follows spork, whose baseline it is).
DEFAULT_GENERIC_LIBS = ("spork", "seq")


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--libraries", default="hbc,spork",
                   help="comma-separated, the first parallel one is the reference for ratios; any of "
                        "hbc, spork, stock, cilk, cilk-auto, stock14 and the sequential baselines "
                        "baseline, seq "
                        "(default: hbc,spork)")
    p.add_argument("--workers", default="80",
                   help="comma-separated worker counts, e.g. 1,80 (default 80); the baselines run at 1 only")
    p.add_argument("--one-core-cpu", type=int, default=None, metavar="CPU",
                   help="CPU for 1-worker runs (default: the first CPU on node 0 other than CPU 0)")
    p.add_argument("--warmup", type=int, default=3, help="discarded repetitions per process (default 3)")
    p.add_argument("--trials", type=int, default=5, help="timed repetitions per process (default 5)")
    p.add_argument("--processes", type=int, default=1,
                   help="processes per benchmark and library, libraries interleaved (default 1)")
    p.add_argument("--only", action="append", default=[], metavar="GLOB",
                   help="only benchmarks matching this glob (repeatable), e.g. 'spmv-*'")
    p.add_argument("--input-size", default="benchmarking",
                   choices=["benchmarking", "tpal", "testing"], help="artifact input size")
    p.add_argument("--heartbeat-us", type=int, default=None,
                   help="HBC heartbeat interval (default: HEARTBEAT_USECS from the artifact's env.sh); "
                        "spork's interval is compiled in (HEARTBEAT_INTERVAL_US)")
    target = p.add_mutually_exclusive_group()
    target.add_argument("--hbc-native", action="store_true",
                        help="compile HBC's bitcode with -march=native too, like the native ParlayLib builds "
                             "(by default the artifact's kernels are generic x86-64)")
    target.add_argument("--generic", action="store_true",
                        help="compile everything for generic x86-64: the ParlayLib builds and HBC's final "
                             "link get %s (HBC's bitcode already is generic)" % GENERIC_MARCH)
    p.add_argument("--spork-native", action="store_true",
                   help="build spork and its baseline seq for -march=native like the other ParlayLib "
                        "libraries (by default they target generic x86-64)")
    p.add_argument("--spork-vectorize", action="store_true",
                   help="let the Spork plugin vectorize spork's parallel loops (it vectorizes a probe "
                        "of each loop and inlines it); by default nothing is vectorized, as in the artifact")
    p.add_argument("--smt", choices=["off", "keep"], default="off",
                   help="off: disable SMT for the runs and restore it afterwards (needs sudo); "
                        "keep: leave SMT as it is")
    p.add_argument("--no-numa", action="store_true", help="do not interleave memory with numactl")
    p.add_argument("--timeout", type=int, default=5400, help="seconds per run process (default 5400)")
    p.add_argument("--artifact", default=DEFAULT_ARTIFACT, help="HBC artifact root")
    p.add_argument("--output", default=None,
                   help="output directory (default hbc-comparison/<timestamp>)")
    p.add_argument("--reuse-bin", metavar="DIR", default=None,
                   help="skip building; take binaries from DIR/bin of an earlier run")
    p.add_argument("--dry-run", action="store_true", help="print the plan and exit")
    args = p.parse_args()
    if args.generic and args.spork_native:
        p.error("argument --spork-native: not allowed with argument --generic")
    try:
        args.workers = sorted({int(w) for w in args.workers.split(",") if w.strip()})
    except ValueError:
        p.error("argument --workers: expected comma-separated integers")
    if not args.workers or args.workers[0] < 1:
        p.error("argument --workers: worker counts must be positive")
    return args


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


def core_cpus():
    """Online CPUs, one per core: the lowest-numbered online thread of each (they stay online with SMT off)."""
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


def geomean(xs):
    return math.exp(sum(map(math.log, xs)) / len(xs))


def read_env_sh(artifact):
    vals = {}
    for line in open(os.path.join(artifact, "env.sh")):
        m = re.match(r"\s*export\s+(\w+)=(\S+?)\s*;?\s*$", line)
        if m:
            vals[m.group(1)] = m.group(2)
    return vals


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
        self.artifact = os.path.abspath(args.artifact)
        self.libs = [l.strip() for l in args.libraries.split(",") if l.strip()]
        bad = [l for l in self.libs if l not in ("hbc", "baseline") and l not in PARLAY_LIBS]
        if bad:
            sys.exit("error: unknown libraries: %s" % ", ".join(bad))
        self.parallel = [l for l in self.libs if l not in SEQUENTIAL]
        self.ref = (self.parallel or self.libs)[0]
        self.workers = args.workers
        self.one_core_cpu = None
        self.benches = [b for b in BENCHMARKS
                        if not args.only or any(fnmatch.fnmatch(b[0], g) for g in args.only)]
        if not self.benches:
            sys.exit("error: no benchmarks selected")
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.out = os.path.abspath(args.output or os.path.join(SCRIPT_DIR, "hbc-comparison", stamp))
        self.bin = os.path.join(self.out, "bin")
        self.logs = os.path.join(self.out, "logs")
        self.env_sh = read_env_sh(self.artifact) if os.path.exists(os.path.join(self.artifact, "env.sh")) else {}
        self.cpu_khz = self.env_sh.get("CPU_FREQUENCY_KHZ", "")
        self.heartbeat_us = args.heartbeat_us or int(self.env_sh.get("HEARTBEAT_USECS", "100"))
        self.smt = Smt(args.smt)
        self.times = {}   # (workers, benchmark, library) -> [trial times]
        self.failures = []

    # ---- checks ------------------------------------------------------------
    def preflight(self):
        need = [(os.path.join(self.artifact, "env.sh"), "artifact not set up (no env.sh)"),
                (os.path.join(self.artifact, "scripts", "parlay", "build.sh"), "scripts/parlay/build.sh missing")]
        if any(l in self.libs for l in ("hbc", "baseline")) and not self.args.reuse_bin:
            need += [(os.path.join(LLVM14, "bin", "clang++"), "LLVM 14 not installed under %s" % LLVM14),
                     (os.path.join(self.artifact, "heartbeatcompiler", "noelle", "install", "bin", "noelle-norm"),
                      "NOELLE not built (heartbeatcompiler/noelle/install)")]
        if "hbc" in self.libs and not self.args.reuse_bin:
            need += [(os.path.join(self.artifact, "heartbeatcompiler", "build", "lib", "Heartbeat.so"),
                      "HBC not built (heartbeatcompiler/build/lib/Heartbeat.so)")]
        for path, msg in need:
            if not os.path.exists(path):
                sys.exit("error: %s: %s" % (msg, path))
        if not self.cpu_khz:
            sys.exit("error: CPU_FREQUENCY_KHZ missing from env.sh")
        if self.workers[-1] > os.cpu_count():
            sys.exit("error: --workers %d exceeds the %d online CPUs" % (self.workers[-1], os.cpu_count()))
        if 1 not in self.workers and any(l in SEQUENTIAL for l in self.libs):
            print("note: %s run at 1 worker only, and --workers has no 1"
                  % ", ".join(l for l in self.libs if l in SEQUENTIAL))
        if not self.args.no_numa and not capture("command -v numactl"):
            sys.exit("error: numactl not found (or pass --no-numa)")
        if 1 in self.workers:
            self.choose_one_core_cpu()

    def choose_one_core_cpu(self):
        cores = core_cpus()
        if self.args.one_core_cpu is not None:
            if self.args.one_core_cpu not in cores:
                sys.exit("error: --one-core-cpu %d is not a usable CPU (one per core: %s)"
                         % (self.args.one_core_cpu, ",".join(map(str, cores))))
            self.one_core_cpu = self.args.one_core_cpu
        else:
            # CPU 0 takes most housekeeping interrupts; prefer another core on node 0.
            candidates = [c for c in cores if c != 0 and cpu_node(c) in (0, None)] or cores
            self.one_core_cpu = candidates[0]

    # ---- build -------------------------------------------------------------
    def hbc_env(self):
        env = dict(os.environ)
        env["PATH"] = os.path.join(LLVM14, "bin") + ":" + env.get("PATH", "")
        env["LD_LIBRARY_PATH"] = os.path.join(LLVM14, "lib") + ":" + env.get("LD_LIBRARY_PATH", "")
        return env

    def build(self):
        os.makedirs(self.bin, exist_ok=True)
        env = self.hbc_env()
        # The Makefiles append their bitcode flags to FLAGS (FLAGS += ...), so
        # a FLAGS from the environment is prepended to them.
        hbc_env = dict(env, FLAGS="-march=native") if self.args.hbc_native else env
        hbc_vars = shlex.quote(HBC_GENERIC_OPTS) if self.args.generic else ""

        def make(name, bdir, lib, target, binary, make_vars):
            """Build lib with one of the artifact's make targets, as its scripts do."""
            log = os.path.join(self.logs, "build-%s-%s.log" % (name, lib))
            cmd = ("source %s/env.sh && make clean > /dev/null 2>&1 && "
                   "make %s INPUT_SIZE=%s %s %s && cp %s %s/%s-%s; rc=$?; "
                   "make clean > /dev/null 2>&1; exit $rc"
                   % (shlex.quote(self.artifact), target, self.args.input_size, make_vars, hbc_vars,
                      binary, shlex.quote(self.bin), name, lib))
            self.report_build(name, lib, sh(cmd, log, cwd=bdir, env=hbc_env))

        for name, rel, target, make_vars, macros in self.benches:
            bdir = os.path.join(self.artifact, rel)
            if "hbc" in self.libs:
                make(name, bdir, "hbc", target, "heartbeat_manual" if target == "hbm" else "heartbeat_compiler",
                     "ACC=true CHUNKSIZE=1 " + make_vars)
            if "baseline" in self.libs:
                make(name, bdir, "baseline", "baseline", "baseline", make_vars)
            for lib in self.libs:
                if lib in ("hbc", "baseline"):
                    continue
                log = os.path.join(self.logs, "build-%s-%s.log" % (name, lib))
                cmd = "%s %s %s %s/%s-%s -DINPUT_%s %s %s" % (
                    os.path.join(self.artifact, "scripts", "parlay", "build.sh"), shlex.quote(bdir),
                    PARLAY_LIBS[lib], shlex.quote(self.bin), name, lib, self.args.input_size.upper(),
                    macros.format(artifact=self.artifact), GENERIC_MARCH if self.generic(lib) else "")
                lib_env = dict(env, SPORK_VECTORIZE="1") if self.vectorized(lib) else env
                self.report_build(name, lib, sh(cmd, log, cwd=self.artifact, env=lib_env))

    def report_build(self, name, lib, rc):
        status = "ok" if rc == 0 else "FAILED (see logs/build-%s-%s.log)" % (name, lib)
        print("  build %-18s %-8s %s" % (name, lib, status), flush=True)
        if rc:
            self.failures.append((name, lib, None, "build failed"))

    # ---- run ---------------------------------------------------------------
    def run_env(self, lib, workers):
        env = self.hbc_env()
        reps = str(self.args.warmup + self.args.trials)
        w = str(workers)
        if lib in ("hbc", "baseline"):
            # The artifact's baseline also runs on taskparts' timing loop (run_baseline in its Makefiles).
            env.update(TASKPARTS_NUM_WORKERS=w, TASKPARTS_CPU_BASE_FREQUENCY_KHZ=self.cpu_khz,
                       TASKPARTS_KAPPA_USEC=str(self.heartbeat_us), TASKPARTS_BENCHMARK_WARMUP_SECS="0",
                       TASKPARTS_BENCHMARK_NUM_REPEAT=reps)
        else:
            env.update(PARLAY_NUM_THREADS=w, CILK_NWORKERS=w, UTILITY_BENCHMARK_WARMUP_SECS="0",
                       UTILITY_BENCHMARK_NUM_REPEAT=reps)
        return env

    @staticmethod
    def runs_at(lib, workers):
        """Whether a library runs at this worker count (the sequential baselines only at 1)."""
        return workers == 1 or lib not in SEQUENTIAL

    def placement(self, workers):
        """The numactl/taskset prefix for a run on this many workers."""
        if workers == 1:
            cpu = self.one_core_cpu
            if self.args.no_numa:
                return "taskset -c %d" % cpu
            node = cpu_node(cpu)
            return "numactl --physcpubind=%d%s" % (cpu, "" if node is None else " --membind=%d" % node)
        if self.args.no_numa:
            return "taskset -c 0-%d" % (workers - 1)
        return "numactl --physcpubind=0-%d --interleave=all" % (workers - 1)

    def run(self):
        for workers in self.workers:
            prefix = self.placement(workers)
            print("== %d worker%s: %s" % (workers, "" if workers == 1 else "s", prefix), flush=True)
            for name, rel, *_ in self.benches:
                bdir = os.path.join(self.artifact, rel)
                for proc in range(self.args.processes):
                    for lib in self.libs:
                        binary = os.path.join(self.bin, "%s-%s" % (name, lib))
                        if not self.runs_at(lib, workers) or not os.path.exists(binary):
                            continue
                        log = os.path.join(self.logs, "run-%s-%s-p%d-%d.log" % (name, lib, workers, proc))
                        rc = sh("%s %s" % (prefix, shlex.quote(binary)), log, cwd=bdir,
                                env=self.run_env(lib, workers), timeout=self.args.timeout)
                        exec_times = [float(x) for x in re.findall(r'"exectime":\s*([0-9.]+)', open(log).read())]
                        trials = exec_times[self.args.warmup:]
                        if rc or len(trials) < self.args.trials:
                            self.failures.append((name, lib, workers, "rc=%d, %d exectimes" % (rc, len(exec_times))))
                            print("  run   %-18s %-8s FAILED rc=%d (%d exectimes)" % (
                                name, lib, rc, len(exec_times)), flush=True)
                            continue
                        self.times.setdefault((workers, name, lib), []).extend(trials)
                        print("  run   %-18s %-8s mean %.4f s  (%s)" % (
                            name, lib, statistics.mean(trials), " ".join("%.3f" % t for t in trials)), flush=True)

    # ---- report ------------------------------------------------------------
    def generic(self, lib):
        """Whether ParlayLib library lib is built for generic x86-64 (else -march=native)."""
        return self.args.generic or (lib in DEFAULT_GENERIC_LIBS and not self.args.spork_native)

    def vectorized(self, lib):
        """Whether library lib is built with the Spork plugin's vectorization on."""
        return lib == "spork" and self.args.spork_vectorize

    def targets(self):
        """What each build was compiled for."""
        if self.args.generic:
            parts = ["everything generic x86-64 (%s)" % GENERIC_MARCH]
        else:
            parts = []
            made = [lib for lib in self.libs if lib in ("hbc", "baseline")]
            if made:
                parts.append("%s -march=native, bitcode included" % " and ".join(made) if self.args.hbc_native
                             else "%s bitcode generic x86-64, final link -march=native (artifact default)"
                             % " and ".join(made))
            parts += ["%s %s" % (lib, "generic x86-64" if self.generic(lib) else "-march=native")
                      for lib in self.libs if lib not in ("hbc", "baseline")]
        vec = [lib for lib in self.libs if self.vectorized(lib)]
        parts.append("vectorized: %s (Spork plugin)" % ", ".join(vec) if vec else "nothing vectorized")
        return "; ".join(parts)

    def library_revision(self, lib):
        if lib == "hbc":
            path = os.path.join(self.artifact, "heartbeatcompiler")
        elif lib == "baseline":
            path = self.artifact   # the serial code is in the artifact's benchmark sources
        else:
            path = os.path.join(SCRIPT_DIR, LIBRARY_DIRS[lib])
        rev = capture("git -C %s rev-parse --short HEAD" % shlex.quote(path))
        dirty = capture("git -C %s status --porcelain --untracked-files=no" % shlex.quote(path))
        return rev + ("+uncommitted" if dirty else "") if rev else "unknown"

    @staticmethod
    def heartbeat_settings(lib):
        """The heartbeat interval and coarsening factor compiled into lib ("" if it has none)."""
        header = os.path.join(SCRIPT_DIR, LIBRARY_DIRS[lib], "include", "parlay", "internal", "spork_scheduler.h")
        get = lambda name: capture("grep -oP '%s = \\K[0-9]+' %s" % (name, shlex.quote(header)))
        return {"interval_us": get("HEARTBEAT_INTERVAL_US"), "coarsening_factor": get("HEARTBEAT_COARSENING_FACTOR")}

    def mean(self, workers, name, lib):
        ts = self.times.get((workers, name, lib))
        return statistics.mean(ts) if ts else None

    @staticmethod
    def ratio(a, b):
        """a / b, or None without both (a time of 0 is below the timer's 1 ms resolution)."""
        return a / b if a and b else None

    def baseline_section(self):
        """Report lines comparing each parallel library with its sequential baseline."""
        baselines = [l for l in SEQUENTIAL if l in self.libs]
        if 1 not in self.workers or not baselines:
            return []
        paired = [l for l in self.parallel if BASELINE.get(l) in baselines]
        # (header, value for a benchmark, format, whether the geomean row shows it)
        cols = [("%s TS" % l, lambda n, l=l: self.mean(1, n, l), "%.4f", False) for l in baselines]
        if len(baselines) == 2:
            cols.append(("baseline/seq", lambda n: self.ratio(self.mean(1, n, "baseline"), self.mean(1, n, "seq")),
                         "%.3f", True))
        cols += [("%s T1/TS" % l, lambda n, l=l: self.ratio(self.mean(1, n, l), self.mean(1, n, BASELINE[l])),
                  "%.2f", True) for l in paired]
        cols += [("%s TS/T%d" % (l, w), lambda n, l=l, w=w: self.ratio(self.mean(1, n, BASELINE[l]),
                                                                      self.mean(w, n, l)), "%.1f", True)
                 for w in self.workers if w > 1 for l in paired]
        lines = ["", "## Sequential baselines", "",
                 "TS: mean time of the sequential baseline (baseline for hbc, seq for the ParlayLib libraries).  "
                 "T1/TS: a parallel library's overhead on 1 worker.  TS/TP: its speedup over its baseline "
                 "on P workers.", "",
                 "| benchmark | " + " | ".join(c[0] for c in cols) + " |", "|" + "---|" * (1 + len(cols))]
        values = {c[0]: [] for c in cols}
        for name, *_ in self.benches:
            cells = []
            for head, get, fmt, _ in cols:
                v = get(name)
                if v is not None:
                    values[head].append(v)
                cells.append(fmt % v if v is not None else "—")
            lines.append("| %s | %s |" % (name, " | ".join(cells)))
        lines.append("| geomean | %s |" % " | ".join(
            fmt % geomean(values[head]) if in_mean and values[head] else "" for head, _, fmt, in_mean in cols))
        return lines

    def write_outputs(self):
        with open(os.path.join(self.out, "results.tsv"), "w") as f:
            f.write("workers\tbenchmark\tlibrary\ttrial\tseconds\n")
            for (workers, name, lib), ts in self.times.items():
                for i, t in enumerate(ts):
                    f.write("%d\t%s\t%s\t%d\t%.6f\n" % (workers, name, lib, i, t))
        config = {
            "date": datetime.datetime.now().isoformat(timespec="seconds"),
            "args": vars(self.args), "libraries": self.libs, "workers": self.workers,
            "one_core_cpu": self.one_core_cpu,
            "benchmarks": [b[0] for b in self.benches],
            "hbc_heartbeat_us": self.heartbeat_us, "cpu_frequency_khz": self.cpu_khz,
            "heartbeats": {lib: self.heartbeat_settings(lib) for lib in self.libs if lib in HEARTBEAT_LIBS},
            "revisions": {lib: self.library_revision(lib) for lib in self.libs},
            "compilers": {
                "clang14": capture("%s/bin/clang++ --version | head -1" % LLVM14),
                "repo_clang": capture("%s/llvm-project/build/bin/clang++ --version | head -1" % SCRIPT_DIR),
                "opencilk": capture("/opt/opencilk/bin/clang++ --version 2>/dev/null | head -1")},
            "cpu": capture("lscpu | grep 'Model name' | sed 's/.*: *//'"),
            "failures": self.failures,
        }
        with open(os.path.join(self.out, "config.json"), "w") as f:
            json.dump(config, f, indent=2)

        ref = self.ref
        heartbeats = ["HBC heartbeat %d us" % self.heartbeat_us] + [
            "%s heartbeat %s us%s" % (lib, s["interval_us"] or "?",
                                      ", coarsening factor %s" % s["coarsening_factor"] if s["coarsening_factor"] else "")
            for lib, s in config["heartbeats"].items()]
        placement = []
        if 1 in self.workers:
            placement.append("1-worker runs on CPU %d, memory %s" % (
                self.one_core_cpu, "not bound" if self.args.no_numa else "on its node"))
        if self.workers[-1] > 1:
            placement.append("P-worker runs on CPUs 0..P-1, memory %s" % (
                "not interleaved" if self.args.no_numa else "interleaved"))
        lines = ["# HBC comparison", "",
                 "- Libraries: %s (ratios relative to `%s`)" % (", ".join(
                     "`%s`%s" % (l, " (sequential, 1 worker)" if l in SEQUENTIAL else "") for l in self.libs), ref),
                 "- Workers: %s; SMT %s; %s" % (", ".join(map(str, self.workers)), self.args.smt,
                                                "; ".join(placement)),
                 "- Repetitions: %d warmup (discarded) + %d timed, %d process(es) per library" % (
                     self.args.warmup, self.args.trials, self.args.processes),
                 "- %s; input size `%s`" % ("; ".join(heartbeats), self.args.input_size),
                 "- Targets: %s" % self.targets(),
                 "- Revisions: %s" % ", ".join("%s %s" % kv for kv in config["revisions"].items())]
        for workers in self.workers:
            libs = [l for l in self.libs if self.runs_at(l, workers)]
            if not libs:
                continue
            others = [l for l in libs if l != ref]
            lines += ["", "## %d worker%s" % (workers, "" if workers == 1 else "s"), "",
                      "Mean seconds per repetition [min-max]; ratio = mean / `%s` mean." % ref, "",
                      "| benchmark | " + " | ".join(libs) + " | " +
                      " | ".join("%s/%s" % (l, ref) for l in others) + " |",
                      "|" + "---|" * (1 + len(libs) + len(others))]
            ratios = {l: [] for l in others}
            for name, *_ in self.benches:
                cells = []
                for l in libs:
                    ts = self.times.get((workers, name, l))
                    cells.append("%.4f [%.4f-%.4f]" % (statistics.mean(ts), min(ts), max(ts)) if ts else "—")
                rcells = []
                for l in others:
                    r = self.ratio(self.mean(workers, name, l), self.mean(workers, name, ref))
                    if r is not None:
                        ratios[l].append(r)
                    rcells.append("%.3f" % r if r is not None else "—")
                lines.append("| %s | %s |" % (name, " | ".join(cells + rcells)))
            lines += ["", "Geometric mean of ratios: " + (", ".join(
                "%s/%s %.3f (%d benchmarks)" % (l, ref, geomean(rs), len(rs))
                for l, rs in ratios.items() if rs) or "—")]
        if 1 in self.workers and len(self.workers) > 1 and self.parallel:
            lines += ["", "## Speedups", "", "Mean 1-worker time / mean P-worker time.", "",
                      "| benchmark | " + " | ".join("%s P=%d" % (l, w) for w in self.workers[1:]
                                                   for l in self.parallel) + " |",
                      "|" + "---|" * (1 + len(self.parallel) * (len(self.workers) - 1))]
            for name, *_ in self.benches:
                cells = []
                for w in self.workers[1:]:
                    for l in self.parallel:
                        r = self.ratio(self.mean(1, name, l), self.mean(w, name, l))
                        cells.append("%.1f" % r if r is not None else "—")
                lines.append("| %s | %s |" % (name, " | ".join(cells)))
        lines += self.baseline_section()
        if self.failures:
            lines += ["", "## Failures", ""] + [
                "- %s %s%s: %s" % (n, l, "" if w is None else " (%d workers)" % w, msg)
                for n, l, w, msg in self.failures]
        report = "\n".join(lines) + "\n"
        with open(os.path.join(self.out, "report.md"), "w") as f:
            f.write(report)
        print("\n" + report)

    # ---- driver ------------------------------------------------------------
    def main(self):
        self.preflight()
        print("Benchmarks: %s" % ", ".join(b[0] for b in self.benches))
        print("Libraries:  %s   workers %s   %d warmup + %d trials x %d process(es)   SMT %s" % (
            ", ".join(self.libs), ",".join(map(str, self.workers)), self.args.warmup, self.args.trials,
            self.args.processes, self.args.smt))
        if self.one_core_cpu is not None:
            print("1 worker:   CPU %d (NUMA node %s)" % (self.one_core_cpu, cpu_node(self.one_core_cpu)))
        print("HBC heartbeat %d us, CPU_FREQUENCY_KHZ %s" % (self.heartbeat_us, self.cpu_khz))
        print("Targets:    %s" % self.targets())
        print("Output:     %s" % self.out)
        if self.args.dry_run:
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
            print("== run (SMT %s, %d CPUs online)" % (open(SMT_CONTROL).read().strip(), os.cpu_count()))
            self.run()
        finally:
            self.smt.restore()
            self.write_outputs()


if __name__ == "__main__":
    Comparison(parse_args()).main()
