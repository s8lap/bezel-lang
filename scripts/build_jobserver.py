#!/usr/bin/env python3
"""Machine-wide build jobserver: one shared GNU-make token pool for every
concurrent lane build on this box.

Why: `dev_heavy.sh` puts every build in one resource-capped cgroup slice, which
bounds TOTAL cpu+mem but not the NUMBER of compiles. N lanes each running
`cmake --build -j16` still start 16*N clang processes that share 18 cores and
22G. Measured during run R20 (2026-09-25): 111-134 concurrent clang, host
MemAvailable down to 9G; an earlier cycle recorded throughput collapse at 68
clang and cage OOM-kills (`gmake: *** Killed`, no `error:` line). An exclusive
flock was tried and removed (producer-stall storm), because it serialises whole
builds: a one-TU relink waited behind a cold build.

A jobserver caps the number of concurrent JOBS instead of builds. Every make
that joins keeps its one implicit slot (so no build ever starves, and a small
incremental build is never queued behind a cold one) and takes extra slots from
a shared FIFO of N tokens. Total concurrent jobs <= N + (number of builds).

GNU make >= 4.4 joins an existing FIFO jobserver through
`MAKEFLAGS="-j --jobserver-auth=fifo:PATH"` (an explicit -jN on the command
line makes it start a private pool instead, which is why `dev_heavy.sh` strips
-j when it routes a build here). Ninja >= 1.13 honours the same variable.

Two properties the FIFO needs that plain `mkfifo` does not give:
  * A pipe's buffered tokens are destroyed when the last descriptor closes, so a
    holder process must keep it open -> `serve` holds it O_RDWR for its lifetime.
  * make returns tokens on a normal exit but NOT when it is SIGKILLed (a lane
    stopped mid-build, a cage OOM-kill). Lost tokens would shrink the pool
    forever -> `serve` audits every --interval seconds: for each process holding
    the FIFO, a make with k running children holds k-1 tokens; if the pool is
    short of N minus that sum on --confirm consecutive audits, it writes the
    difference back. It never tops the pool above N.
  * Jobs of a build that did NOT join (a bare `make -j`, `ninja -j64`) take
    their token through scripts/build_token.sh, the compile/link launcher
    CMakeLists.txt installs. That launcher holds the FIFO open under the comm
    `bzl-tok-held` while it holds one token (`bzl-tok-wait` / `bzl-tok-done`
    while it holds none); the audit counts it, and a pool with only launchers
    in it is busy, not idle. `serve` publishes `features` ("launcher1 <pid>")
    so the launcher can tell this daemon from an older one that would re-mint
    launcher-held tokens as a leak.

Subcommands:
  serve [--tokens N] [--dir D] [--interval S] [--confirm K]
  status [--dir D]            one line: tokens, holders, estimate, daemon pid
  alive  [--dir D]            exit 0 iff a live daemon serves D (dev_heavy uses it)
  rewrite-args -- ARGS...     print ARGS minus -j/--parallel forms, NUL-separated

State lives in D (default /var/tmp/bezel-scratch/jobserver): `fifo`, `pid`,
`tokens`, `features`, and `samples.tsv` (one audit row per interval, for
evaluation).
"""
from __future__ import annotations

import argparse
import array
import errno
import fcntl
import os
import signal
import stat
import sys
import termios
import time

DEFAULT_DIR = os.environ.get("BEZEL_JOBSERVER_DIR", "/var/tmp/bezel-scratch/jobserver")
TOKEN = b"+"
# The launcher protocol this daemon speaks (scripts/build_token.sh checks it).
FEATURES = "launcher1"
BUILD_TOOLS = ("make", "gmake", "ninja", "ninja-build")
LAUNCHER_PREFIX = "bzl-tok-"
LAUNCHER_HOLDING = "bzl-tok-held"
SAMPLES_HEADER = ("epoch\tavailable\texpected_held\tmakes\tclang\t"
                  "mem_avail_mb\trepaired\tlaunchers\n")


def _read(path: str) -> str:
    with open(path) as f:
        return f.read().strip()


def _paths(d: str) -> dict[str, str]:
    return {k: os.path.join(d, k)
            for k in ("fifo", "pid", "tokens", "features", "samples.tsv")}


def available_tokens(fd: int) -> int:
    buf = array.array("i", [0])
    fcntl.ioctl(fd, termios.FIONREAD, buf)
    return buf[0]


def _fifo_holders(fifo: str, self_pid: int) -> list[int]:
    """Pids (other than ours) with an open descriptor on the FIFO."""
    try:
        target = os.stat(fifo)
    except FileNotFoundError:
        return []
    pids = []
    for name in os.listdir("/proc"):
        if not name.isdigit() or int(name) == self_pid:
            continue
        fd_dir = f"/proc/{name}/fd"
        try:
            for fd in os.listdir(fd_dir):
                try:
                    st = os.stat(f"{fd_dir}/{fd}")
                except OSError:
                    continue
                if st.st_ino == target.st_ino and st.st_dev == target.st_dev:
                    pids.append(int(name))
                    break
        except OSError:
            continue
    return pids


def _children_count(pid: int) -> int:
    n = 0
    try:
        for tid in os.listdir(f"/proc/{pid}/task"):
            try:
                with open(f"/proc/{pid}/task/{tid}/children") as f:
                    n += len(f.read().split())
            except OSError:
                pass
    except OSError:
        pass
    return n


def _comm(pid: int) -> str:
    try:
        with open(f"/proc/{pid}/comm") as f:
            return f.read().strip()
    except OSError:
        return ""


def expected_outstanding(fifo: str, self_pid: int) -> tuple[int, int, int]:
    """(tokens the holders should be holding, make/ninja holders, launchers).

    A make running k children holds k-1 tokens (one child uses its implicit
    slot). A build_token.sh launcher holds one token while its comm is
    `bzl-tok-held` and none while it waits or hands it back. Other holders
    (e.g. a `status` probe) hold none."""
    held = 0
    makes = 0
    launchers = 0
    for pid in _fifo_holders(fifo, self_pid):
        comm = _comm(pid)
        # Fedora installs ninja as `ninja-build`; an unrecognised holder's
        # tokens read as a leak and get re-minted (R22: pool unbounded).
        if comm in BUILD_TOOLS:
            makes += 1
            held += max(0, _children_count(pid) - 1)
        elif comm.startswith(LAUNCHER_PREFIX):
            launchers += 1
            if comm == LAUNCHER_HOLDING:
                held += 1
    return held, makes, launchers


def _count_procs(names: tuple[str, ...]) -> int:
    n = 0
    for name in os.listdir("/proc"):
        if name.isdigit() and _comm(int(name)) in names:
            n += 1
    return n


def _mem_available_kb() -> int:
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith("MemAvailable:"):
                return int(line.split()[1])
    return -1


def serve(d: str, tokens: int, interval: float, confirm: int) -> int:
    p = _paths(d)
    os.makedirs(d, exist_ok=True)
    # Refuse a second daemon on the same dir: two holders would each write N.
    if alive(d):
        print(f"build_jobserver: a daemon already serves {d}", file=sys.stderr)
        return 1
    if os.path.exists(p["fifo"]) and not stat.S_ISFIFO(os.stat(p["fifo"]).st_mode):
        print(f"build_jobserver: {p['fifo']} exists and is not a FIFO", file=sys.stderr)
        return 1
    if not os.path.exists(p["fifo"]):
        os.mkfifo(p["fifo"], 0o600)
    fd = os.open(p["fifo"], os.O_RDWR | os.O_NONBLOCK)
    # Drain anything a previous daemon left, then publish exactly N.
    while True:
        try:
            if not os.read(fd, 4096):
                break
        except BlockingIOError:
            break
    os.write(fd, TOKEN * tokens)
    with open(p["tokens"], "w") as f:
        f.write(f"{tokens}\n")
    with open(p["pid"], "w") as f:
        f.write(f"{os.getpid()}\n")
    with open(p["features"], "w") as f:
        f.write(f"{FEATURES} {os.getpid()}\n")

    stop = False

    def _term(_sig, _frm):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGTERM, _term)
    signal.signal(signal.SIGINT, _term)

    # An existing file may carry an older header (no `launchers` column):
    # repeat the header whenever this daemon's columns differ from the last one.
    last_header = ""
    try:
        with open(p["samples.tsv"]) as f:
            for line in f:
                if line.startswith("epoch\t"):
                    last_header = line
    except OSError:
        pass
    log = open(p["samples.tsv"], "a", buffering=1)
    if last_header != SAMPLES_HEADER:
        log.write(SAMPLES_HEADER)
    streak: list[int] = []   # per-audit deficits while the pool looks short
    surplus_streak: list[int] = []  # per-audit surpluses while it looks over-full
    idle_audits = 0
    try:
        while not stop:
            time.sleep(interval)
            # A make that has read a token but not yet forked its child looks
            # like a leak for a moment. Take the MINIMUM deficit over a few
            # quick sub-samples, and repair only the minimum that persisted
            # across `confirm` consecutive audits: a transient never inflates
            # the pool, a real leak (it never comes back) is repaired.
            # A surplus (pool OVER-full) is real only if it persists: take it
            # as the minimum over the same sub-samples (the maximum d_k). The
            # /proc scan is not an atomic snapshot, so a single sample can
            # skew either way; make itself reaps a child before releasing its
            # token, so the zombie window reads as a deficit, not a surplus.
            deficit = None
            dmax = None
            for k in range(5):
                if k:
                    time.sleep(0.05)
                avail = available_tokens(fd)
                held, makes, launchers = expected_outstanding(p["fifo"], os.getpid())
                d_k = tokens - avail - held  # >0: tokens nobody holds
                deficit = d_k if deficit is None else min(deficit, d_k)
                dmax = d_k if dmax is None else max(dmax, d_k)
            repaired = 0
            if makes == 0 and launchers == 0:
                # Idle: the pool must hold exactly N. Restore it both ways
                # (a lost token, or an over-return) once idle is confirmed.
                streak.clear()
                surplus_streak.clear()
                idle_audits += 1
                if idle_audits >= 2 and avail != tokens:
                    if avail < tokens:
                        repaired = tokens - avail
                        os.write(fd, TOKEN * repaired)
                    else:
                        try:
                            repaired = -len(os.read(fd, avail - tokens))
                        except BlockingIOError:
                            pass
            else:
                idle_audits = 0
                if deficit > 0:
                    streak.append(deficit)
                    if len(streak) >= confirm:
                        repaired = max(0, min(min(streak), tokens - available_tokens(fd)))
                        if repaired:
                            os.write(fd, TOKEN * repaired)
                        streak.clear()
                else:
                    streak.clear()
                # Over-full while busy: a daemon restarted under live builds
                # publishes a fresh N while the old holders still return
                # theirs, so the box would run N + (held) jobs until the next
                # idle moment, which a long wave may never reach. Drain the
                # confirmed surplus; never try to read more than is in the pipe.
                if dmax < 0:
                    surplus_streak.append(-dmax)
                    if len(surplus_streak) >= confirm:
                        want = min(min(surplus_streak), available_tokens(fd))
                        if want > 0:
                            try:
                                repaired = -len(os.read(fd, want))
                            except BlockingIOError:
                                pass
                        surplus_streak.clear()
                else:
                    surplus_streak.clear()
            log.write(
                f"{int(time.time())}\t{avail}\t{held}\t{makes}\t"
                f"{_count_procs(('clang', 'clang++', 'cc1plus'))}\t"
                f"{_mem_available_kb() // 1024}\t{repaired}\t{launchers}\n"
            )
    finally:
        # features first: a launcher must never see it without a live pid.
        try:
            if _read(p["features"]).split()[1:] == [str(os.getpid())]:
                os.unlink(p["features"])
        except (OSError, IndexError):
            pass
        try:
            if _read(p["pid"]) == str(os.getpid()):
                os.unlink(p["pid"])
        except OSError:
            pass
        log.close()
    return 0


def alive(d: str) -> bool:
    p = _paths(d)
    try:
        pid = int(_read(p["pid"]))
    except (OSError, ValueError):
        return False
    try:
        os.kill(pid, 0)
    except OSError as e:
        if e.errno != errno.EPERM:
            return False
    try:
        return stat.S_ISFIFO(os.stat(p["fifo"]).st_mode)
    except OSError:
        return False


def status(d: str) -> int:
    p = _paths(d)
    if not alive(d):
        print(f"build_jobserver: no live daemon for {d}")
        return 1
    fd = os.open(p["fifo"], os.O_RDONLY | os.O_NONBLOCK)
    try:
        avail = available_tokens(fd)
    finally:
        os.close(fd)
    held, makes, launchers = expected_outstanding(p["fifo"], os.getpid())
    n = _read(p["tokens"])
    pid = _read(p["pid"])
    # `clang` is measured HERE, synchronously, not read back from the
    # daemon's periodic samples.tsv (which can be up to --interval stale and
    # sits several lines below in the "last audits" table `jobserver_ctl.sh
    # status` tails). Printing it on the SAME line as `held` is the point: an
    # orchestrator heartbeat (or a human) can compare the two at a glance —
    # `held` says how many tokens the pool THINKS are checked out, `clang`
    # says how many compiler processes actually exist system-wide — without
    # having to correlate a TSV column against this line by hand. A large,
    # persistent gap between them (clang far above held + #makes) is the
    # leak signature this field exists to make visible immediately.
    clang = _count_procs(("clang", "clang++", "cc1plus"))
    print(f"build_jobserver: pid={pid} tokens={n} available={avail} "
          f"held~{held} makes={makes} launchers={launchers} clang={clang} "
          f"fifo={p['fifo']}")
    return 0


def rewrite_args(args: list[str]) -> list[str]:
    """Drop every -j/--parallel form (cmake's and make's) so the build joins the
    shared pool instead of creating a private one. A value is consumed only when
    the next argument is purely numeric."""
    out: list[str] = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("-j", "--jobs", "--parallel"):
            if i + 1 < len(args) and args[i + 1].isdigit():
                i += 1
        elif (a.startswith("-j") and a[2:].isdigit()) or \
                a.startswith("--jobs=") or a.startswith("--parallel="):
            pass
        else:
            out.append(a)
        i += 1
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("--tokens", type=int, default=int(os.environ.get("BEZEL_JOBSERVER_TOKENS", "16")))
    s.add_argument("--dir", default=DEFAULT_DIR)
    s.add_argument("--interval", type=float, default=10.0)
    s.add_argument("--confirm", type=int, default=3)
    for name in ("status", "alive"):
        x = sub.add_parser(name)
        x.add_argument("--dir", default=DEFAULT_DIR)
    r = sub.add_parser("rewrite-args")
    r.add_argument("args", nargs=argparse.REMAINDER)
    ns = ap.parse_args(argv)
    if ns.cmd == "serve":
        if ns.tokens < 1:
            ap.error("--tokens must be >= 1")
        return serve(ns.dir, ns.tokens, ns.interval, ns.confirm)
    if ns.cmd == "status":
        return status(ns.dir)
    if ns.cmd == "alive":
        return 0 if alive(ns.dir) else 1
    args = ns.args[1:] if ns.args[:1] == ["--"] else ns.args
    sys.stdout.write("".join(a + "\0" for a in rewrite_args(args)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
