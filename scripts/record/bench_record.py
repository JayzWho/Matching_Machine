#!/usr/bin/env python3
"""Record a benchmark study: repeated launches of the benchmark binaries in
measurement mode, with telemetry showing what else used the measurement cores.

Vocabulary
    repetition  one Google Benchmark repetition inside a process; the first
                repetition of each benchmark in a launch is a warm-up, kept in
                the raw data and in samples.csv but left out of the statistics
    launch      one execution of one benchmark binary
    round       one launch of each binary, in an order that rotates per round
    session     one `setup` ... `restore` span of measurement mode
    condition   shared   other processes may run on the measurement cores
                cleared  every task of the invoking user is restricted to the
                         other online cores for the duration of the round

Usage
    scripts/record/bench_record.py [--label L] [--sessions S] [--rounds N]
                                   [--repetitions R] [--conditions shared,cleared]
    scripts/record/bench_record.py --smoke [--unpinned]      # short wiring check, output discarded
    scripts/record/bench_record.py --restore-affinity FILE   # after a killed run; FILE is the
                                                             # affinity_baseline.json it left behind

A study needs the root-owned copy of bench_env.sh and its password-free sudo
rule (scripts/env/install_bench_env.sh): this script enters and leaves measurement
mode itself, with exactly `sudo -n /usr/local/sbin/mm-bench-env setup` and
`... restore`. --unpinned skips both and marks the result INVALID.

Output: docs/evidence/<cpu>/<date>_<commit>_<label>/
    samples.csv    one row per repetition; `warmup` is 1 for a warm-up
    launches.csv   one row per launch: validity checks and interference telemetry
    intervals.csv  one row per benchmark per launch: wall time, CPU time, run-queue wait
    latency.csv    the percentiles printed by the two latency-report benchmarks,
                   with `warmup` as in samples.csv
    env.json       design, binaries, environment before and after each session
    raw.tar.gz     every raw file: Google Benchmark JSON, console output, setup output

scripts/report/bench_report.py turns that directory into tables.
"""
import argparse
import csv
import hashlib
import json
import os
import re
import select
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / "release"
INSTALLED = "/usr/local/sbin/mm-bench-env"
STATE_FILE = Path("/run/matching-machine-bench/state")
SYS_CPU = Path("/sys/devices/system/cpu")
# "solo" binaries are single-threaded and get the best-ranked measurement core;
# "pair" binaries create a second thread and get both.
BINARIES = (("bench_order_book", "solo"), ("bench_spsc_ring_buffer", "pair"),
            ("bench_matching_engine", "pair"), ("bench_baseline_mutex", "pair"))
BUILD_INPUTS = ("CMakeLists.txt", "src", "include", "benchmarks", "tests")
AGGREGATE = re.compile(r"_(mean|median|stddev|cv)$")
# The first repetition of every benchmark in a launch pays one-time costs of
# the process: in the variance-sources study, the pipeline throughput at
# 10,000 orders was 41% slower in it. It is recorded and marked, and the
# report scripts leave it out.
WARMUP_REPETITIONS = 1
IRQ_KINDS = ("LOC", "RES", "CAL", "TLB", "NMI", "PMI", "TRM")
SOFTIRQ_KINDS = ("TIMER", "SCHED", "RCU", "HRTIMER", "NET_RX", "NET_TX", "BLOCK", "TASKLET")


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def die(msg):
    sys.exit(f"bench_record.py: {msg}")


def run(*cmd):
    return subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)


def cpu_list(text):
    cpus = set()
    for part in text.strip().split(","):
        if part:
            lo, _, hi = part.partition("-")
            cpus.update(range(int(lo), int(hi or lo) + 1))
    return cpus


def online_cpus():
    return cpu_list((SYS_CPU / "online").read_text())


# ── telemetry ────────────────────────────────────────────────────────────────

def read_schedstat():
    """Per CPU: ns that tasks ran, ns that tasks waited to run, timeslices.
    These three come from sched_info and are kept without kernel.sched_schedstats."""
    out = {}
    with open("/proc/schedstat") as fh:
        for line in fh:
            if line.startswith("cpu"):
                f = line.split()
                out[int(f[0][3:])] = (int(f[7]), int(f[8]), int(f[9]))
    return out


def read_cpu_table(path, cpus):
    """/proc/interrupts or /proc/softirqs as {label: count summed over cpus}.
    Columns are mapped from the header because offline CPUs have no column."""
    table = {}
    with open(path) as fh:
        cols = [int(c[3:]) for c in fh.readline().split()]
        want = [i + 1 for i, c in enumerate(cols) if c in cpus]
        for line in fh:
            f = line.split()
            table[f[0].rstrip(":")] = sum(int(f[i]) for i in want if i < len(f) and f[i].isdigit())
    return table


def read_throttle():
    """(sum of per-core throttle events, package throttle events). A core's
    counter is reported by both SMT siblings and the package counter by every
    CPU, so each is counted once."""
    core, seen, package = 0, set(), None
    for d in sorted(SYS_CPU.glob("cpu[0-9]*")):
        try:
            core_id = (d / "topology" / "core_id").read_text().strip()
            c = int((d / "thermal_throttle" / "core_throttle_count").read_text())
            p = int((d / "thermal_throttle" / "package_throttle_count").read_text())
        except (OSError, ValueError):
            continue
        if core_id not in seen:
            seen.add(core_id)
            core += c
        if package is None:
            package = p
    return core, package or 0


def package_temp():
    for zone in Path("/sys/class/thermal").glob("thermal_zone*"):
        try:
            if (zone / "type").read_text().strip() == "x86_pkg_temp":
                return int((zone / "temp").read_text()) / 1000.0
        except (OSError, ValueError):
            pass
    return float("nan")


def core_mhz(cpus):
    """Current frequency of each busy core. The kernel derives the value from
    the last scheduler tick; a core that is idle has no recent tick and is
    reported at the lowest frequency (400 MHz here), which says nothing about
    the speed it runs at, so readings below 1000 MHz are left out. Real
    slow-downs are caught by the throttle counters."""
    out = []
    for c in cpus:
        try:
            mhz = int((SYS_CPU / f"cpu{c}" / "cpufreq" / "scaling_cur_freq").read_text()) / 1000.0
        except (OSError, ValueError):
            continue
        if mhz >= 1000.0:
            out.append(mhz)
    return out


def suspended_seconds():
    """CLOCK_BOOTTIME counts time spent suspended and CLOCK_MONOTONIC does not."""
    return time.clock_gettime(time.CLOCK_BOOTTIME) - time.clock_gettime(time.CLOCK_MONOTONIC)


def snapshot(measure):
    sched = read_schedstat()
    return {
        "throttle": read_throttle(),
        "temp": package_temp(),
        "irq": read_cpu_table("/proc/interrupts", measure),
        "softirq": read_cpu_table("/proc/softirqs", measure),
        "ran_ns": {c: sched[c][0] for c in measure},
        "waited_ns": {c: sched[c][1] for c in measure},
        "suspended": suspended_seconds(),
        "load1": float(open("/proc/loadavg").read().split()[0]),
    }


# ── the user's other tasks ───────────────────────────────────────────────────
# cgroup cpusets would be the usual way to keep other processes off the
# measurement cores, but the cpuset controller is not delegated to the user and
# measurement mode does not set one up. What can be done without privilege is
# to change the affinity of the invoking user's own tasks, which is what the
# `cleared` condition does. Tasks of other users (root daemons, kernel threads)
# stay where they are; launches.csv records the CPU time they took on the
# measurement cores as `foreign_ms`.

def user_tasks():
    uid = os.getuid()
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            if os.stat(f"/proc/{pid}").st_uid != uid:
                continue
            tids = os.listdir(f"/proc/{pid}/task")
        except OSError:
            continue
        for tid in tids:
            yield int(pid), int(tid)


def task_start(pid, tid):
    try:
        with open(f"/proc/{pid}/task/{tid}/stat") as fh:
            return fh.read().rsplit(")", 1)[1].split()[19]
    except (OSError, IndexError):
        return None


def affinity_baseline():
    """{tid: [start time, allowed CPUs]} for every task of this user. Taken
    before `setup`, while every CPU is online, so it is what `restore` returns to."""
    base = {}
    for pid, tid in user_tasks():
        try:
            base[str(tid)] = [task_start(pid, tid), sorted(os.sched_getaffinity(tid))]
        except OSError:
            pass
    return base


def apply_affinity(baseline, exclude=frozenset(), failures=None):
    """Give every task of this user its baseline affinity minus `exclude`.
    A task born since the baseline takes its thread-group leader's, else every
    CPU. Returns (tasks seen, tasks changed, tasks that could not be changed);
    `failures`, when given, collects the (pid, tid) of the last kind."""
    online, me = online_cpus(), os.getpid()
    everything = set(range(os.cpu_count() or 1))
    seen = changed = failed = 0
    for pid, tid in user_tasks():
        if pid == me:
            continue
        seen += 1
        want = everything
        for key, owner in ((str(tid), tid), (str(pid), pid)):
            entry = baseline.get(key)
            if entry and entry[0] == task_start(pid, owner):
                want = set(entry[1])
                break
        want = (want & online) - exclude or online - exclude
        try:
            if os.sched_getaffinity(tid) != want:
                os.sched_setaffinity(tid, want)
                changed += 1
        except ProcessLookupError:
            seen -= 1
        except OSError:
            failed += 1
            if failures is not None:
                failures.append((pid, tid))
    return seen, changed, failed


# ── one launch ───────────────────────────────────────────────────────────────

def kill_launch_session(session):
    """Stop descendants that changed process group but kept the launch session."""
    if session <= 1 or session == os.getsid(0):
        raise ValueError("refusing to terminate the recorder's session")
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        try:
            if os.getsid(pid) != session:
                continue
            # Pin the process identity before checking membership again, so
            # PID reuse cannot redirect the signal to an unrelated process.
            descriptor = os.pidfd_open(pid)
            try:
                if os.getsid(pid) == session:
                    signal.pidfd_send_signal(descriptor, signal.SIGKILL)
            finally:
                os.close(descriptor)
        except ProcessLookupError:
            pass
    # Also cover the original group if a task appeared during the scan.
    try:
        os.killpg(session, signal.SIGKILL)
    except ProcessLookupError:
        pass


def launch(binary, cores, measure, args, json_path, *, command_prefix=(), env=None, sample_interval=1.0,
           timeout=None):
    """Run one binary on `cores`; return output, telemetry marks and a record.

    Prefixes may create process groups, but must not start a new session.
    A launch still running after `timeout` seconds is killed with TimeoutError.
    """
    if not sample_interval > 0:
        raise ValueError("sample_interval must be positive")
    if timeout is not None and not timeout > 0:
        raise ValueError("timeout must be positive")
    cmd = ["taskset", "-c", ",".join(map(str, sorted(cores))), *command_prefix, str(BUILD / binary),
           "--benchmark_out_format=json", f"--benchmark_out={json_path}", *args]
    before = snapshot(measure)
    started = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    t0 = time.monotonic_ns()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=env, start_new_session=True)
    clock = ((~proc.pid) << 3) | 2          # the child's process CPU-time clock
    fd, out, marks, mhz, cpu_ns = proc.stdout.fileno(), bytearray(), [], [], 0
    try:
        while True:
            ready, _, _ = select.select([fd], [], [], sample_interval)
            if timeout is not None and time.monotonic_ns() - t0 > timeout * 1e9:
                raise TimeoutError(f"launch still running after {timeout} s")
            if not ready:
                mhz += core_mhz(cores)      # sample while the launch is silent, i.e. measuring
                continue
            chunk = os.read(fd, 1 << 16)
            if not chunk:
                break
            out += chunk
            try:
                cpu_ns = time.clock_gettime_ns(clock)
            except OSError:
                pass
            sched = read_schedstat()
            marks.append((len(out), time.monotonic_ns(), cpu_ns, sum(sched[c][1] for c in cores)))
        _, status, usage = os.wait4(proc.pid, 0)
    except BaseException as exc:
        # GDB can put its inferior in a different process group. The session
        # still identifies all processes belonging to this launch.
        kill_launch_session(proc.pid)
        proc.wait()
        # Preserve available output without waiting for EOF from an unsupported
        # prefix that escaped the session and retained the pipe.
        os.set_blocking(fd, False)
        while True:
            try:
                chunk = os.read(fd, 1 << 16)
            except BlockingIOError:
                break
            if not chunk:
                break
            out += chunk
        exc.benchmark_output = bytes(out)
        raise
    finally:
        proc.stdout.close()
    wall = (time.monotonic_ns() - t0) / 1e9
    after = snapshot(measure)
    proc.returncode = os.waitstatus_to_exitcode(status)

    own = usage.ru_utime + usage.ru_stime
    ran = {c: (after["ran_ns"][c] - before["ran_ns"][c]) / 1e9 for c in measure}
    irq = {k: after["irq"].get(k, 0) - before["irq"].get(k, 0) for k in after["irq"]}
    soft = {k: after["softirq"].get(k, 0) - before["softirq"].get(k, 0) for k in after["softirq"]}
    record = {
        "started": started, "exit_status": proc.returncode,
        "wall_s": round(wall, 4), "cpu_s": round(own, 4), "cpu_per_wall": round(own / wall, 4),
        "voluntary_switches": usage.ru_nvcsw, "involuntary_switches": usage.ru_nivcsw,
        "minor_faults": usage.ru_minflt, "major_faults": usage.ru_majflt, "max_rss_kb": usage.ru_maxrss,
        "throttle_core": after["throttle"][0] - before["throttle"][0],
        "throttle_package": after["throttle"][1] - before["throttle"][1],
        "suspended_s": round(after["suspended"] - before["suspended"], 3) or 0.0,
        "temp_start_c": before["temp"], "temp_end_c": after["temp"],
        "mhz_min": min(mhz, default=""), "mhz_max": max(mhz, default=""), "mhz_samples": len(mhz),
        "device_irqs": sum(v for k, v in irq.items() if k.isdigit()),
        **{f"irq_{k}": irq.get(k, 0) for k in IRQ_KINDS},
        **{f"softirq_{k}": soft.get(k, 0) for k in SOFTIRQ_KINDS},
        **{f"ran_s_cpu{c}": round(ran[c], 4) for c in sorted(measure)},
        # Time tasks spent runnable but waiting on the cores this launch was
        # allowed to use, and CPU time taken there by anything else.
        "waited_ms": round(sum(after["waited_ns"][c] - before["waited_ns"][c] for c in cores) / 1e6, 3),
        "foreign_ms": round((sum(ran[c] for c in cores) - own) * 1e3, 3),
        "load1": before["load1"],
    }
    start_mark = (0, t0, 0, sum(before["waited_ns"][c] for c in cores))
    return bytes(out), [start_mark] + marks, record


def intervals(out, marks):
    """Per benchmark, in console order: (name, wall s, CPU s, run-queue wait ms).
    Google Benchmark prints and flushes a benchmark's lines when all of its
    repetitions are done, so a benchmark spans from the previous flush to its own."""
    last, order, offset = {}, [], 0
    for line in out.split(b"\n"):
        offset += len(line) + 1
        if line.startswith(b"BM_"):
            name = AGGREGATE.sub("", line.split()[0].decode())
            if name not in last:
                order.append(name)
            last[name] = offset
    rows, prev = [], marks[0]
    for name in order:
        mark = next((m for m in marks if m[0] >= min(last[name], marks[-1][0])), marks[-1])
        rows.append((name, (mark[1] - prev[1]) / 1e9, (mark[2] - prev[2]) / 1e9, (mark[3] - prev[3]) / 1e6))
        prev = mark
    return rows


def samples(json_path):
    """Per repetition: (benchmark, repetition, real ns, CPU ns, iterations)."""
    ns = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}
    with open(json_path) as fh:
        data = json.load(fh)
    return [(b["run_name"], b["repetition_index"], b["real_time"] * ns[b["time_unit"]],
             b["cpu_time"] * ns[b["time_unit"]], b["iterations"])
            for b in data["benchmarks"] if b["run_type"] == "iteration"]


def warmup(repetition):
    """1 for a warm-up repetition, else 0."""
    return int(repetition < WARMUP_REPETITIONS)


def latency_reports(out):
    """Per printed latency report: {label: TSC ticks}. The source prints the
    unit as "cycles"; RDTSC returns ticks of the timestamp counter."""
    reports = []
    for block in re.split(r"=== Latency Report \(\d+ samples\) ===\n", out.decode(errors="replace"))[1:]:
        found = re.findall(r"^(\w+)\s*:\s*(\d+) cycles", block.split("\n\n")[0], re.M)
        if found:
            reports.append({k: int(v) for k, v in found})
    return reports


# ── a study ──────────────────────────────────────────────────────────────────

class Study:
    def __init__(self, opts):
        self.opts = opts
        self.rows = {"samples": [], "launches": [], "intervals": [], "latency": []}
        self.env = {"sessions": []}
        self.baseline = None        # affinity baseline of the session in progress
        self.entered = False        # this process entered measurement mode
        self.round_index = 0

    def prepare(self):
        o = self.opts
        for name, _ in BINARIES:
            if not os.access(BUILD / name, os.X_OK):
                die(f"missing {BUILD / name}; build Release first")
        if STATE_FILE.exists():
            die("measurement mode is already active; run `sudo -n /usr/local/sbin/mm-bench-env restore` first")
        if not o.unpinned and not os.access(INSTALLED, os.X_OK):
            die(f"{INSTALLED} is not installed; see scripts/env/install_bench_env.sh, or pass --unpinned")
        if run("cmake", "--build", str(BUILD), f"-j{os.cpu_count()}").returncode != 0:
            die("Release build failed")
        sha = run("git", "rev-parse", "--short", "HEAD").stdout.strip()
        dirty = bool(run("git", "status", "--porcelain", "--", *BUILD_INPUTS).stdout.strip())
        model = next(l.split(":", 1)[1] for l in open("/proc/cpuinfo") if l.startswith("model name"))
        tag = re.sub(r"\((R|TM)\)| CPU.*| @.*", "", model.strip())
        tag = re.sub(r"\s+", "-", tag.strip())
        stamp = time.strftime("%Y-%m-%d_%H-%M-%S")
        name = f"{stamp}_{sha}{'-dirty' if dirty else ''}_{o.label}"
        self.out = Path(tempfile.mkdtemp(prefix="bench_record_")) / name if o.smoke \
            else ROOT / "docs" / "evidence" / tag / name
        self.raw = self.out / "raw"
        self.raw.mkdir(parents=True)
        self.recovery = self.out / "affinity_baseline.json"
        self.env.update({
            "label": o.label, "commit": sha, "build_inputs_dirty": dirty,
            "measurement_mode": not o.unpinned, "smoke": o.smoke,
            "design": {"sessions": o.sessions, "rounds_per_session": o.rounds, "repetitions": o.repetitions,
                       "warmup_repetitions": WARMUP_REPETITIONS,
                       "conditions": o.conditions, "extra_benchmark_args": o.benchmark_args},
            "binaries": {n: hashlib.sha256((BUILD / n).read_bytes()).hexdigest() for n, _ in BINARIES},
            "binaries_built": {n: time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime((BUILD / n).stat().st_mtime))
                               for n, _ in BINARIES},
        })
        log(f"study -> {self.out}")

    def capture_env(self, name):
        path = self.raw / name
        run("scripts/env/capture_env.sh", str(path), str(BUILD))
        return json.loads(path.read_text())

    def session(self, s):
        o = self.opts
        self.baseline = affinity_baseline()
        self.recovery.write_text(json.dumps(self.baseline))     # for --restore-affinity after a kill
        if not o.unpinned:
            done = run("sudo", "-n", INSTALLED, "setup")
            (self.raw / f"s{s}_setup.txt").write_text(done.stdout + done.stderr)
            if done.returncode != 0:
                die(f"setup failed:\n{done.stdout}{done.stderr}")
            self.entered = True
        listed = run("scripts/env/bench_env.sh", "cores", "--list")
        cores = [int(c) for c in listed.stdout.strip().split(",") if c]
        if listed.returncode != 0 or len(cores) != 2:
            die(f"could not determine the measurement cores: {listed.stdout}{listed.stderr}")
        measure, online = set(cores), online_cpus()
        if not measure <= online or not online - measure:
            die(f"measurement cores {cores} need to be online with at least one other core (online: {sorted(online)})")
        os.sched_setaffinity(0, online - measure)      # this process and everything it starts, except launches
        env = {"session": s, "cores": cores, "housekeeping": sorted(online - measure),
               "before": self.capture_env(f"s{s}_env_before.json")}
        log(f"session {s}: cores {cores}, housekeeping {sorted(online - measure)}; warm-up")
        subprocess.run(["taskset", "-c", str(cores[0]), str(BUILD / "bench_order_book"),
                        "--benchmark_filter=BM_AddOrder_NoMatch", f"--benchmark_min_time={o.warmup}s"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        for r in range(1, o.rounds + 1):
            # The first condition alternates from session to session so that
            # neither condition always runs first.
            condition = o.conditions[(r - 1 + s - 1) % len(o.conditions)]
            order = BINARIES[self.round_index % len(BINARIES):] + BINARIES[:self.round_index % len(BINARIES)]
            self.round_index += 1
            try:
                for position, (binary, kind) in enumerate(order, 1):
                    moved = apply_affinity(self.baseline, measure)[1] if condition == "cleared" else 0
                    self.one(s, r, condition, position, binary, cores[:1] if kind == "solo" else cores, measure, moved)
            finally:
                if condition == "cleared":
                    apply_affinity(self.baseline)
            log(f"session {s} round {r}/{o.rounds} ({condition}) done")

        env["after"] = self.capture_env(f"s{s}_env_after.json")
        self.leave(env)
        self.env["sessions"].append(env)

    def one(self, s, r, condition, position, binary, cores, measure, moved):
        o = self.opts
        stem = f"s{s}_r{r:02d}_{condition}_{binary}"
        args = [f"--benchmark_repetitions={o.repetitions}", *o.benchmark_args]
        out, marks, record = launch(binary, cores, measure, args, self.raw / f"{stem}.json")
        (self.raw / f"{stem}.log").write_bytes(out)
        if record["exit_status"] != 0:
            die(f"{binary} exited with {record['exit_status']}; see {self.raw / (stem + '.log')}")
        key = {"session": s, "round": r, "condition": condition, "binary": binary}
        self.rows["launches"].append({**key, "position": position, "cores": " ".join(map(str, cores)),
                                      "user_tasks_moved": moved, **record})
        for name, rep, real, cpu, iters in samples(self.raw / f"{stem}.json"):
            self.rows["samples"].append({**key, "benchmark": name, "repetition": rep, "warmup": warmup(rep),
                                         "real_time_ns": repr(real), "cpu_time_ns": repr(cpu), "iterations": iters})
        for name, wall, cpu, waited in intervals(out, marks):
            self.rows["intervals"].append({**key, "benchmark": name, "wall_s": round(wall, 4), "cpu_s": round(cpu, 4),
                                           "cpu_per_wall": round(cpu / wall, 4) if wall > 0 else "",
                                           "waited_ms": round(waited, 3)})
        # Each binary has one latency-report benchmark of one iteration, so
        # report i is printed by repetition i.
        for i, report in enumerate(latency_reports(out)):
            for label, ticks in report.items():
                self.rows["latency"].append({**key, "report": i, "warmup": warmup(i), "percentile": label,
                                             "tsc_ticks": ticks})

    def leave(self, env=None):
        """Leave measurement mode, then return every task to its baseline affinity.
        The order matters: an affinity set while the SMT siblings are offline
        does not grow back when they return."""
        if self.entered:
            done = run("sudo", "-n", INSTALLED, "restore")
            self.entered = False
            if done.returncode != 0:
                log(f"RESTORE FAILED — run `sudo -n {INSTALLED} restore` by hand:\n{done.stdout}{done.stderr}")
        if self.baseline is not None:
            os.sched_setaffinity(0, online_cpus())
            apply_affinity(self.baseline)
            seen, wrong, failed = apply_affinity(self.baseline)     # second pass counts what the first left
            self.baseline = None
            self.recovery.unlink(missing_ok=True)
            if env is not None:
                env["affinity_restored"] = {"tasks": seen, "not_at_baseline": wrong, "not_changeable": failed}
            if wrong or failed:
                log(f"affinity: {wrong} tasks not at baseline, {failed} not changeable")

    def write(self):
        for name, rows in self.rows.items():
            if rows:
                with open(self.out / f"{name}.csv", "w", newline="") as fh:
                    writer = csv.DictWriter(fh, fieldnames=list(rows[0]), lineterminator="\n")
                    writer.writeheader()
                    writer.writerows(rows)
        (self.out / "env.json").write_text(json.dumps(self.env, indent=1) + "\n")
        with tarfile.open(self.out / "raw.tar.gz", "w:gz") as tar:
            for path in sorted(self.raw.iterdir()):
                tar.add(path, arcname=f"raw/{path.name}")
        for path in self.raw.iterdir():
            path.unlink()
        self.raw.rmdir()

    def summary(self):
        launches = self.rows["launches"]
        log(f"{len(launches)} launches, {len(self.rows['samples'])} repetitions")
        for condition in self.opts.conditions:
            mine = [l for l in launches if l["condition"] == condition]
            if mine:
                foreign = sorted(l["foreign_ms"] / l["wall_s"] for l in mine)
                log(f"  {condition:8s} throttle +{sum(l['throttle_core'] + l['throttle_package'] for l in mine)}"
                    f"  device IRQs +{sum(l['device_irqs'] for l in mine)}"
                    f"  foreign CPU ms per s: median {foreign[len(foreign) // 2]:.2f}, max {foreign[-1]:.2f}")
        log(f"written to {self.out}")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--label", default="control")
    p.add_argument("--sessions", type=int, default=4)
    p.add_argument("--rounds", type=int, default=8, help="rounds per session")
    p.add_argument("--repetitions", type=int, default=6, help="per benchmark and launch, the first a warm-up")
    p.add_argument("--conditions", default="shared,cleared")
    p.add_argument("--warmup", type=int, default=5, help="seconds of discarded warm-up per session")
    p.add_argument("--unpinned", action="store_true", help="do not enter measurement mode (result INVALID)")
    p.add_argument("--smoke", action="store_true", help="one short session written to a temporary directory")
    p.add_argument("--restore-affinity", metavar="FILE", help="apply a saved affinity baseline and exit")
    o = p.parse_args()

    if o.restore_affinity:
        seen, changed, failed = apply_affinity(json.loads(Path(o.restore_affinity).read_text()))
        print(f"{seen} tasks, {changed} changed, {failed} not changeable")
        return
    if o.repetitions <= WARMUP_REPETITIONS:
        die(f"--repetitions must exceed the {WARMUP_REPETITIONS} warm-up repetition")
    o.conditions = o.conditions.split(",")
    if not set(o.conditions) <= {"shared", "cleared"}:
        die("--conditions takes shared and cleared")
    o.benchmark_args = []
    if o.smoke:
        o.sessions, o.rounds, o.repetitions, o.warmup = 1, len(o.conditions), 2, 1
        o.benchmark_args = ["--benchmark_min_time=0.01s"]
        o.label = "smoke"

    def stop(signum, _frame):
        sys.exit(128 + signum)
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop)

    study = Study(o)
    study.prepare()
    try:
        for s in range(1, o.sessions + 1):
            study.session(s)
    finally:
        study.leave()
    study.write()
    study.summary()


if __name__ == "__main__":
    main()
