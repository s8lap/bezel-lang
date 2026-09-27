"""Gate serialization helper (gate-infra preflight-lockfile lane, post-run).

Two responsibilities — both pinned by `scripts/gate/tests/test_gate_serialization.py`:

  1. `acquire_gate_lock(path, timeout_s=0) -> Lock | None` /
     `release_gate_lock(lock)`
     — exclusive flock on a single-path lockfile. Non-blocking by default
     (`timeout_s=0`, matching Lane A's `flock -n`). Released automatically by
     the kernel on process exit (including SIGKILL — see
     `test_lockfile_released_on_subprocess_exit`). The held lock means
     "another gate is currently driving this checkout"; the bash entry
     points at `scripts/gate_and_push.sh` and `scripts/gate.sh` delegate to
     this helper so two concurrent gate invocations cannot run their heavy
     phases simultaneously (the 2026-08-09 deadlock class).

  2. `is_gate_sibling(pid) -> bool`
     — walks /proc/<pid>/status' `PPid:` chain up to `_SIBLING_DEPTH_CAP=32`
     levels; returns True iff any ANCESTOR's cmdline (joined NUL-separated
     argv from /proc/<pid>/cmdline) contains any name in `GATE_COMM_NAMES`.
     The pid itself is NOT checked (only descendants qualify) — this matches
     the `test_self_is_not_sibling_when_invoked_as_gate_script` contract and
     has no production impact: `pgrep -x cmake|cc1plus|...` (the only caller
     in gate.sh's preflight) never matches a gate-named comm in the first
     place. The bash equivalent at `scripts/gate.sh:173-188` walked the pid
     itself too, but that branch is unreachable in production (cmake has
     comm "cmake", not "gate.sh"), so the Python tightening is regression-
     free for the gate's actual call site.

CRITICAL #2 fix (post-run review): the original implementation read
/proc/<pid>/comm and matched it against GATE_COMM_NAMES. That is silently
wrong in production — the kernel sets comm from the EXECUTABLE'S BASENAME
(e.g. "bash" for /usr/bin/bash), NOT from argv[0]. Every script invoked
under `#!/usr/bin/env bash` therefore has comm="bash" regardless of which
script is running: the gate driver's comm is "bash", not "gate.sh".
Sibling detection at the gate layer always returned False, so the
"builders active" preflight abort always fired against another gate's
cmake child and the helper never recognized the lock-holding gate. The
fix reads /proc/<pid>/cmdline (which carries the full argv, including
the script path like /home/stepan/.../gate_and_push.sh) and matches the
joined cmdline string against GATE_COMM_NAMES. The bash gate entry points
themselves — invoked as `#!/usr/bin/env bash …/gate_and_push.sh …` — have
cmdline = "…/gate_and_push.sh …", so the substring match lands. Pinned
by `tests/test_gate_serialization.py`'s `test_integration_baseline_bash_not_sibling`,
`test_integration_argv0_override_is_sibling`, and
`test_integration_gate_path_in_cmdline_is_sibling`.

The CLI surface (`python3 -m gate.serialize <cmd>`) replaces Lane A's bash
implementations so the entry points at gate_and_push.sh and gate.sh delegate
here. Exit codes mirror `flock -n` (0=acquired, 1=refused) and the
is-sibling predicate (0=yes, 1=no) so the bash `if ! …` shape carries over
verbatim. Stdlib only — `fcntl`, `os`, `sys`, `time`.
"""

import fcntl
import os
import signal
import sys
import time


# Names the preflight treats as "this is a gate". Substring-matched
# against /proc/<pid>/cmdline (see `is_gate_sibling` and `_cmdline_of`
# below) — drift between this tuple and the bash `_is_gate_ancestor`
# case-list at scripts/gate.sh:179 would silently re-introduce the
# deadlock class.
#
# Note: these are PATH-LIKE substrings (some include `tests/` or omit
# it), not kernel comms. /proc/<pid>/cmdline is NUL-separated argv, so
# for a gate entry point like `/usr/bin/env bash scripts/gate_and_push.sh`,
# the joined string contains `… scripts/gate_and_push.sh …` and
# `gate_and_push.sh` matches.
GATE_COMM_NAMES = (
    "gate_and_push.sh",
    "gate_full_cycle.sh",
    "gate.sh",
    "tests/run_parallel.sh",
    "run_parallel.sh",
    "dev_heavy.sh",
)

# Default gate lockfile path. Matches `GATE_LOCK` in Lane A's bash at
# scripts/gate_and_push.sh:54; lives under /var/tmp/bezel-scratch (the
# project scratch dir, NOT /tmp which is tmpfs/RAM per CLAUDE.md).
DEFAULT_GATE_LOCK_PATH = "/var/tmp/bezel-scratch/gate.lock"

# Mirrors the bash `_is_gate_ancestor` depth cap at scripts/gate.sh:175.
# Real init is PID 1, so any chain longer than ~10 hops is a fork bomb or
# a CI runner gone wrong — 32 is the bash-compatibility value.
_SIBLING_DEPTH_CAP = 32

# Polling interval for `acquire_gate_lock` with timeout_s > 0. Coarse
# enough that a held lock wastes ~this much latency before re-trying,
# fine enough that the 0.5s challenger in `test_second_acquire_cleanly_refused`
# does not block long past its deadline.
_ACQUIRE_RETRY_SLEEP_S = 0.02


def gate_lock_path() -> str:
    """Default lockfile path; matches the bash `GATE_LOCK` at
    `scripts/gate_and_push.sh:54`. Callers may pass an arbitrary path to
    `acquire_gate_lock` (the lockfile is just a name for the kernel flock
    state — paths are interchangeable as long as both sides agree)."""
    return DEFAULT_GATE_LOCK_PATH


class Lock:
    """Opaque flock-based lock handle returned by `acquire_gate_lock`.

    Usable as a context manager (auto-release on exit) or paired explicitly
    with `release_gate_lock`. The handle is per-FD across the holder's
    lifetime; the kernel releases the flock automatically when the FD is
    closed (i.e. on process exit, including SIGKILL — see
    `test_lockfile_released_on_subprocess_exit`)."""

    __slots__ = ("_fd", "_path")

    def __init__(self, fd: int, path: str) -> None:
        self._fd = fd
        self._path = path

    @property
    def path(self) -> str:
        return self._path

    def __enter__(self) -> "Lock":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> bool:
        self.close()
        return False

    def close(self) -> None:
        """Release the flock and close the FD. Idempotent — safe to call
        from `__exit__` after an explicit `release_gate_lock` (or vice
        versa). Swallows OSError because close-after-unlock is the
        well-formed path."""
        fd = self._fd
        if fd is None:
            return
        self._fd = None
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass
        try:
            os.close(fd)
        except OSError:
            pass


def _open_lockfile(path: str) -> int:
    """Open (and create) the lockfile path; best-effort mkdir on the parent.
    Returns the raw FD (caller owns the lifetime). The fd must be closed
    via `Lock.close()` / `release_gate_lock` for the flock to release.
    """
    parent = os.path.dirname(path)
    if parent:
        # makedirs(exist_ok=True) is the right shape — best-effort, and
        # we don't want a pre-existing /var/tmp/bezel-scratch to fail.
        # Race-tolerant: another caller may have created it just now.
        try:
            os.makedirs(parent, exist_ok=True)
        except OSError:
            # Race: another process created the dir in parallel. If it
            # is still missing afterwards, the open() below will surface
            # the real error to the caller.
            pass
    return os.open(path, os.O_RDWR | os.O_CREAT, 0o644)


def acquire_gate_lock(path: str, timeout_s: float = 0):
    """Acquire an exclusive flock on `path`. Returns a `Lock` handle on
    success or `None` on refusal.

      * `timeout_s == 0` (default): non-blocking — try once with
        `LOCK_EX | LOCK_NB`, return `None` immediately if held. This is
        the shape Lane A's bash uses (`flock -n`) and the shape the bash
        entry points rely on (the gate cannot wait around for another
        gate; it must abort cleanly with the "another gate is running"
        message).

      * `timeout_s > 0`: block for at most `timeout_s` seconds, polling at
        `_ACQUIRE_RETRY_SLEEP_S` intervals; return `None` if the lock is
        still held at the deadline. Bounded blocking is what the test
        `test_second_acquire_cleanly_refused` exercises (challenger calls
        with `timeout_s=0.5`, must report REFUSED within the deadline).

    A `None` return never blocks the caller; a returned `Lock` is held
    until `release_gate_lock` is called, the context manager exits, OR
    the holder process exits (kernel auto-release)."""
    try:
        fd = _open_lockfile(path)
    except OSError:
        return None

    if timeout_s <= 0:
        # Single try — non-blocking. Mirrors `flock -n`.
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except (OSError, BlockingIOError):
            try:
                os.close(fd)
            except OSError:
                pass
            return None
        return Lock(fd, path)

    # Bounded blocking: poll LOCK_EX | LOCK_NB until it succeeds or the
    # deadline elapses. NB: `time.monotonic()` so a wall-clock jump
    # (NTP, DST) cannot extend the wait.
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return Lock(fd, path)
        except (OSError, BlockingIOError):
            now = time.monotonic()
            if now >= deadline:
                try:
                    os.close(fd)
                except OSError:
                    pass
                return None
            sleep_s = min(_ACQUIRE_RETRY_SLEEP_S, max(0.0, deadline - now))
            time.sleep(sleep_s)


def release_gate_lock(lock) -> None:
    """Release a `Lock` returned by `acquire_gate_lock`. Safe to call
    multiple times (subsequent calls are no-ops) and safe to call on
    `None` (a defensive no-op). Idempotent because the lock state is
    already cleared once, and the FD close + flock(LOCK_UN) paths each
    handle a second invocation cleanly."""
    if lock is None:
        return
    lock.close()


def _cmdline_of(pid: int):
    """Read /proc/<pid>/cmdline. Returns the joined (space-separated)
    command-line, or `None` on read failure. Strips NULs and trims.

    The cmdline contains the full argv NUL-separated, including the
    script path like `/home/stepan/.../gate_and_push.sh`. Unlike
    /proc/<pid>/comm (the kernel-truncated EXECUTABLE basename — set
    by the kernel from the binary's basename, NOT from argv[0], which
    is why every script invoked under `#!/usr/bin/env bash` has
    comm="bash" regardless of which script it runs), cmdline carries
    argv[0] verbatim. That makes cmdline the load-bearing source for
    "is this a gate driver" in production. The matched substrings are
    in `GATE_COMM_NAMES`."""
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            data = f.read()
    except (IOError, OSError):
        return None
    if not data:
        return None
    # cmdline is NUL-separated; replace NULs with spaces so substring
    # matches land against a path-component. decode with errors="replace"
    # so a non-UTF8 byte (rare, but legal in argv on Linux) does not
    # raise — the substring match is byte-as-string and a single
    # replacement character is harmless.
    return data.replace(b"\x00", b" ").decode("utf-8", errors="replace").strip()


def _comm_of(pid: int):
    """Read /proc/<pid>/comm. Returns the stripped comm string or `None`
    if the proc entry is missing (process exited) or unreadable (EPERM).

    Retained for tests that need to sanity-check fixture comm values
    (e.g. `sleep` has comm='sleep'); NOT used by `is_gate_sibling` —
    sibling detection keys on cmdline because comm is the executable
    basename and every bash script has comm='bash'."""
    try:
        with open(f"/proc/{pid}/comm", "r") as f:
            return f.read().strip()
    except (IOError, OSError):
        return None


def _ppid_of(pid: int):
    """Read /proc/<pid>/status's `PPid:` line. Returns the integer PPid or
    `None` on any parse/IO error. Defensive: the field can be missing or
    the file can be partially-written if the pid is exiting."""
    try:
        with open(f"/proc/{pid}/status", "r") as f:
            for line in f:
                if line.startswith("PPid:"):
                    return int(line.split()[1])
    except (IOError, OSError, ValueError, IndexError):
        return None
    return None


def is_gate_sibling(pid: int) -> bool:
    r"""Return True iff `pid`'s ANCESTOR chain (up to `_SIBLING_DEPTH_CAP=32`
    levels) includes any process whose cmdline contains any name in
    `GATE_COMM_NAMES`.

    The pid itself is NOT inspected — only its parents/grand-parents/etc.
    That matches the `test_self_is_not_sibling_when_invoked_as_gate_script`
    contract and is structurally safe in production: the bash call site at
    `scripts/gate.sh:194-201` only invokes the predicate on pids returned
    by `pgrep -x cmake|cc1plus|ld|mold|lld|ninja|ninja-build|make|clang(\+\+)?|bezel_tests(_[a-z]+)?`,
    so the predicate never sees a gate-named comm in the first place.

    Match shape: substring match against the joined argv (NUL-separated
    args joined by spaces). This catches the production shape — a bash
    entry point invoked as `/usr/bin/env bash scripts/gate_and_push.sh …`
    has cmdline containing the substring `scripts/gate_and_push.sh` —
    and the test shape (`exec -a gate.sh bash -c 'sleep N'` has cmdline
    containing `gate.sh` as the first token).

    Returns False on:
      * any /proc read failure (transient — the next call after the
        process settles will work);
      * depth cap exhaustion (real-world chains are <=10; 32 is the
        bash-compatibility value, exceeded only by a fork bomb or a CI
        runner gone wrong);
      * a self-cycle (defensive against /proc corruption: a PPid equal to
        the pid itself would loop forever otherwise)."""
    cur = pid
    for _ in range(_SIBLING_DEPTH_CAP):
        ppid = _ppid_of(cur)
        if ppid is None or ppid <= 1:
            # Reached init (PID 1) or the chain ended (process exited).
            return False
        if ppid == cur:
            # /proc corruption / kernel bug — refuse to loop forever.
            return False
        cmdline = _cmdline_of(ppid)
        if cmdline is not None and any(name in cmdline for name in GATE_COMM_NAMES):
            return True
        cur = ppid
    return False


# ---------------------------------------------------------------------------
# CLI — the bash preflight at scripts/gate.sh and the bash lockfile at
# scripts/gate_and_push.sh invoke this helper. Exit codes mirror the bash
# primitives they replace so the bash `if ! …` shape carries over verbatim:
#
#   acquire      exit 0 (acquired) / exit 1 (refused/timeout)
#                stdout: "ACQUIRED" / "REFUSED"
#   is-sibling   exit 0 (yes) / exit 1 (no)
#                stdout: "yes" / "no"
#   lock-path    exit 0, stdout: default path
#   release      exit 0, stdout: "RELEASED"  (symmetry; bash does not invoke)
#
# The bash entry points translate the helper's non-zero exit into the
# documented "another gate is running, exiting cleanly" message + exit 0
# (the gate's contract is that a refused lock is a clean abort, NOT a
# gate failure — see scripts/gate_and_push.sh:62-79).
# ---------------------------------------------------------------------------

_CLI_EPILOG = """\
Examples:
  python3 -m gate.serialize acquire --path /var/tmp/bezel-scratch/gate.lock
  python3 -m gate.serialize acquire --path /tmp/g.lock --timeout 5
  python3 -m gate.serialize is-sibling --pid 12345
  python3 -m gate.serialize lock-path
"""


def _build_arg_parser():
    import argparse
    parser = argparse.ArgumentParser(
        prog="gate.serialize",
        description=(
            "Gate serialization helper: flock-based gate lockfile + /proc "
            "ancestor-walk for sibling detection."),
        epilog=_CLI_EPILOG,
    )
    sub = parser.add_subparsers(dest="cmd", required=True, metavar="CMD")

    acq = sub.add_parser(
        "acquire",
        help="Try to acquire an exclusive flock on PATH (exit 0=ok, 1=held).")
    acq.add_argument(
        "--path", default=DEFAULT_GATE_LOCK_PATH,
        help="Lockfile path (default: %(default)s)")
    acq.add_argument(
        "--timeout", type=float, default=0,
        help="Seconds to wait before giving up (default: 0 = non-blocking).")
    acq.add_argument(
        "--hold", action="store_true",
        help=("Stay alive holding the lock until killed (SIGTERM/SIGINT). "
              "For bash coproc use: the lock is held by this helper until "
              "bash kills it on exit, and the kernel auto-releases on the "
              "kill regardless of cleanup."))

    rel = sub.add_parser(
        "release",
        help="No-op (symmetry for `acquire`; the kernel releases on exit).")
    rel.add_argument(
        "--path", default=DEFAULT_GATE_LOCK_PATH,
        help="Lockfile path (default: %(default)s, ignored)")

    sib = sub.add_parser(
        "is-sibling",
        help="Is PID a sibling of a running gate? exit 0=yes, 1=no.")
    sib.add_argument("--pid", type=int, required=True, help="PID to inspect")

    sub.add_parser(
        "lock-path",
        help="Print the default gate lockfile path and exit.")

    return parser


def _main(argv=None) -> int:
    parser = _build_arg_parser()
    args = parser.parse_args(argv)

    if args.cmd == "acquire":
        lock = acquire_gate_lock(args.path, timeout_s=args.timeout)
        if lock is None:
            print("REFUSED")
            return 1  # exit 1 mirrors `flock -n` so bash `if !` works
        print("ACQUIRED")
        sys.stdout.flush()
        if not args.hold:
            return 0
        # --hold: stay alive so bash can keep the lock for its lifetime.
        # Kernel auto-releases the flock on process exit (incl. SIGKILL —
        # the production exit path), so we don't strictly need a signal
        # handler. We install one anyway so SIGTERM/SIGINT release the lock
        # promptly rather than waiting for the kernel's grace period; a
        # clean release is faster than a process kill and shows up better
        # in /proc/locks diagnostics.
        def _release_on_signal(signum, frame):
            release_gate_lock(lock)
            sys.exit(0)
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            signal.signal(sig, _release_on_signal)
        # Block until signalled. signal.pause() returns when a caught
        # signal is delivered; the handler then exits the process.
        while True:
            signal.pause()

    if args.cmd == "release":
        # Documented no-op: the kernel releases the flock on process exit,
        # which is the production exit path. This subcommand is here for
        # symmetry / testability only.
        print("RELEASED")
        return 0

    if args.cmd == "is-sibling":
        result = is_gate_sibling(args.pid)
        sys.stdout.write("yes\n" if result else "no\n")
        return 0 if result else 1

    if args.cmd == "lock-path":
        print(gate_lock_path())
        return 0

    # argparse's `required=True` on subparsers means we never reach here,
    # but be defensive in case someone calls _main directly with junk.
    parser.error(f"unknown command: {args.cmd!r}")
    return 2  # unreachable


if __name__ == "__main__":
    sys.exit(_main())
