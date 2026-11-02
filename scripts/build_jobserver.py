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

THE ACCOUNTING (r38-i1-jobserver, 2026-11-03).  Two properties the FIFO needs
that plain `mkfifo` does not give:

  * A pipe's buffered tokens are destroyed when the last descriptor closes, so
    a holder process must keep it open -> `serve` holds it O_RDWR for its
    lifetime.
  * make returns tokens on a normal exit but NOT when it is SIGKILLed (a lane
    stopped mid-build, a cage OOM-kill). Lost tokens would shrink the pool
    forever -> `serve` audits every --interval seconds and re-mints the
    difference. It never tops the pool above N.
  * Jobs of a build that did NOT join (a bare `make -j`, `ninja -j64`) take
    their token through scripts/build_token.sh, the compile/link launcher
    CMakeLists.txt installs. That launcher holds the FIFO open under the comm
    `bzl-tok-held` while it holds one token (`bzl-tok-wait` while it waits,
    `bzl-tok-done` while it hands it back); the audit counts it, and a pool
    with only launchers in it is busy, not idle. `serve` publishes `features`
    ("launcher1 <pid>") so the launcher can tell this daemon from an older
    one that would re-mint launcher-held tokens as a leak.

Everything the daemon reports is now an OBSERVED quantity, never a guess:

  available  the tokens physically in the FIFO (FIONREAD). THE ground truth
             for "how many tokens are in the pool".
  held       tokens - available: the tokens checked OUT of the pool. Every
             token is either in the FIFO or in somebody's hand, so the
             difference is exact. THIS USED TO BE A GUESS: the daemon assumed
             a joined make holding k children holds k-1 tokens, which is only
             true while the build runs at its peak concurrency. A build
             running its LAST job holds ONE token with ONE child, which the
             guess read as zero, so the daemon saw a deficit, minted a token
             that was really held (the pool went to N+1) and then drained the
             surplus again. Measured on the production pool: audit rows
             9961-9963 of samples.tsv (available 19 -> 21 -> 21, repaired +1
             then -1) with a compile in flight (clang=1).
  tok_held   launcher processes that hold one token each (comm `bzl-tok-held`).
  tok_wait   launcher processes queued for a token (comm `bzl-tok-wait`).
  builds     joined make/ninja processes holding the FIFO. Each keeps ONE
             implicit job slot, so the box-wide cap is N + builds. A CMake
             build nests several make levels and each level has its own
             implicit slot; a plain `make` build has one.
  build_jobs the running jobs under the joined builds.

The audit re-mints a token only when the deficit is PROVABLE: the pool is
short by more than the launchers hold AND more than any joined build could be
holding (one token per running job, plus one per build for the window in
which a build has read a token and not yet forked its child). Otherwise the
shortfall belongs to a build that is still running, and minting on it is
exactly the bug this rewrite removes. The audit drains a token only when the
FIFO itself holds MORE than the whole pool (available > N), which is a fact
about the FIFO rather than a guess about holders.

Subcommands:
  serve [--tokens N] [--dir D] [--interval S] [--confirm K]
  status [--dir D]            one line: pool state, holder census, daemon pid
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
from typing import NamedTuple

DEFAULT_DIR = os.environ.get("BEZEL_JOBSERVER_DIR", "/var/tmp/bezel-scratch/jobserver")
TOKEN = b"+"
# The launcher protocol this daemon speaks (scripts/build_token.sh checks it).
FEATURES = "launcher1"
BUILD_TOOLS = ("make", "gmake", "ninja", "ninja-build")
LAUNCHER_PREFIX = "bzl-tok-"
LAUNCHER_HOLDING = "bzl-tok-held"
LAUNCHER_WAITING = "bzl-tok-wait"
LAUNCHER_DONE = "bzl-tok-done"
SAMPLES_HEADER = ("epoch\tavailable\theld\ttok_held\ttok_wait\tbuilds\t"
                  "build_jobs\tclang\tmem_avail_mb\trepaired\n")


def _read(path: str) -> str:
    with open(path) as f:
        return f.read().strip()


def _paths(d: str) -> dict[str, str]:
    return {k: os.path.join(d, k)
            for k in ("fifo", "pid", "tokens", "features", "samples.tsv")}


def available_tokens(fd: int) -> int:
    """The tokens physically in the FIFO. THE ground truth for the pool.

    FIONREAD on a FIFO is the number of bytes queued for reading, and one
    token is one byte, so this is the pool's occupancy exactly -- no inference
    about who holds what."""
    buf = array.array("i", [0])
    fcntl.ioctl(fd, termios.FIONREAD, buf)
    return buf[0]


def _fifo_avail(fifo: str) -> int:
    """`available_tokens` for a caller that has no descriptor open."""
    fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
    try:
        return available_tokens(fd)
    finally:
        os.close(fd)


def pool_size(fifo: str) -> int:
    """N: the pool size the daemon published (dir/tokens)."""
    try:
        return int(_read(os.path.join(os.path.dirname(fifo), "tokens")))
    except (OSError, ValueError):
        return 0


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


class Observation(NamedTuple):
    """What the daemon can SEE. Every field is measured, none is inferred."""
    available: int    # tokens physically in the FIFO (FIONREAD)
    held: int        # tokens - available: the tokens checked OUT of the pool
    builds: int      # joined make/ninja processes holding the FIFO
    build_jobs: int  # running jobs under those builds
    launchers: int   # launcher processes (holding + waiting + handing back)
    tok_held: int    # launchers holding one token each
    tok_wait: int    # launchers queued for a token


def observe(fifo: str, self_pid: int, avail: int | None = None,
            tokens: int | None = None) -> Observation:
    """Measure the pool and its holders.

    `avail` and `tokens` can be passed in by a caller that already has the
    FIFO open (the daemon's audit loop) so the two are read as close together
    as possible."""
    if avail is None:
        avail = _fifo_avail(fifo)
    if tokens is None:
        tokens = pool_size(fifo)
    tok_held = tok_wait = builds = build_jobs = launchers = 0
    for pid in _fifo_holders(fifo, self_pid):
        comm = _comm(pid)
        # Fedora installs ninja as `ninja-build`.
        if comm in BUILD_TOOLS:
            builds += 1
            build_jobs += _children_count(pid)
        elif comm.startswith(LAUNCHER_PREFIX):
            launchers += 1
            if comm == LAUNCHER_HOLDING:
                tok_held += 1
            elif comm == LAUNCHER_WAITING:
                tok_wait += 1
            # LAUNCHER_DONE: handing the token back; it holds nothing.
    return Observation(available=avail, held=tokens - avail, builds=builds,
                       build_jobs=build_jobs, launchers=launchers,
                       tok_held=tok_held, tok_wait=tok_wait)


def expected_outstanding(fifo: str, self_pid: int) -> tuple[int, int, int]:
    """(tokens checked out of the pool, joined build tools, launcher processes).

    COMPAT SHIM, kept because the token-launcher tests call it. This used to
    return a GUESS: a joined make with k children was assumed to hold k-1
    tokens. That is only true while the build runs at its peak concurrency; a
    build running its last job holds one token with one child, which the
    guess read as zero -- and the daemon then minted a token that was really
    held. The count is now the OBSERVED one (pool size minus the FIFO
    occupancy), which is exact."""
    obs = observe(fifo, self_pid)
    return obs.held, obs.builds, obs.launchers


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
    streak: list[int] = []       # per-audit deficits of tokens NOBODY holds
    surplus_streak: list[int] = []  # per-audit surpluses of tokens in the FIFO
    try:
        while not stop:
            time.sleep(interval)
            # A holder that has just taken a token flips its comm a few
            # microseconds later (the launcher) and a build that has read a
            # token forks its child a few milliseconds later, so a single
            # sample can read as a deficit that is not one. Take the MINIMUM
            # claimed deficit over a few quick sub-samples, and repair only
            # the minimum that persisted across `confirm` consecutive audits:
            # a transient never inflates the pool, a real leak (it never comes
            # back) is repaired.
            #
            # The claimed deficit is `held - tok_held - (build_jobs + builds)`:
            # the tokens checked out of the pool, minus the ones the launchers
            # demonstrably hold, minus the most the joined builds could be
            # holding (one token per running job, plus one per build for the
            # window in which a build has read a token and not yet forked).
            # What is left is PROVABLY lost: no holder can be holding it.
            unaccounted = None
            surplus = None
            obs = None
            avail = tokens
            for k in range(5):
                if k:
                    time.sleep(0.05)
                avail = available_tokens(fd)
                obs = observe(p["fifo"], os.getpid(), avail=avail, tokens=tokens)
                u = obs.held - obs.tok_held - (obs.build_jobs + obs.builds)
                s = avail - tokens
                unaccounted = u if unaccounted is None else min(unaccounted, u)
                surplus = s if surplus is None else max(surplus, s)
            repaired = 0
            if unaccounted > 0:
                streak.append(unaccounted)
                if len(streak) >= confirm:
                    repaired = max(0, min(min(streak), tokens - available_tokens(fd)))
                    if repaired:
                        os.write(fd, TOKEN * repaired)
                    streak.clear()
            else:
                streak.clear()
            # Over-full while busy: a daemon restarted under live builds
            # publishes a fresh N while the old holders still return theirs,
            # so the FIFO ends up holding more than the whole pool. That is a
            # fact about the FIFO (available > N), not a guess about holders,
            # so it can be drained: the bytes in the FIFO are free tokens.
            if surplus > 0:
                surplus_streak.append(surplus)
                if len(surplus_streak) >= confirm:
                    want = min(min(surplus_streak), available_tokens(fd) - tokens)
                    if want > 0:
                        try:
                            repaired -= len(os.read(fd, want))
                        except BlockingIOError:
                            pass
                    surplus_streak.clear()
            else:
                surplus_streak.clear()
            log.write(
                f"{int(time.time())}\t{avail}\t{obs.held}\t{obs.tok_held}\t"
                f"{obs.tok_wait}\t{obs.builds}\t{obs.build_jobs}\t"
                f"{_count_procs(('clang', 'clang++', 'cc1plus'))}\t"
                f"{_mem_available_kb() // 1024}\t{repaired}\n"
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
    obs = observe(p["fifo"], os.getpid())
    n = _read(p["tokens"])
    pid = _read(p["pid"])
    # The pool's own state comes first and in this order, because
    # `run_status.sh` shows the first 80 characters of this line and the
    # operator reads it there:
    #   available  tokens physically in the FIFO (the pool's occupancy)
    #   held       tokens checked OUT of the pool (pool size - occupancy)
    #   tok_held   launchers holding a token each (a job that has a slot)
    #   tok_wait   launchers queued for a token (a job waiting for a slot)
    #   builds     joined make/ninja holders, each with ONE implicit job slot
    #   jobs       the box-wide cap: N + builds
    # `clang` is measured HERE, synchronously, not read back from the
    # daemon's periodic samples.tsv (which can be up to --interval stale and
    # sits several lines below in the "last audits" table `jobserver_ctl.sh
    # status` tails). Printing it on the SAME line as `held` is the point: an
    # orchestrator heartbeat (or a human) can compare the two at a glance --
    # `held` says how many tokens are checked out, `clang` says how many
    # compiler processes actually exist system-wide -- without having to
    # correlate a TSV column against this line by hand. A large, persistent
    # gap between them (clang far above held + #builds) is the leak signature
    # this field exists to make visible immediately.
    clang = _count_procs(("clang", "clang++", "cc1plus"))
    print(f"build_jobserver: available={obs.available} held={obs.held} "
          f"tok_held={obs.tok_held} tok_wait={obs.tok_wait} "
          f"builds={obs.builds} jobs={int(n) + obs.builds} "
          f"clang={clang} tokens={n} pid={pid} fifo={p['fifo']}")
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
