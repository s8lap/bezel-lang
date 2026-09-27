"""Property tests for gate serialization (sibling detection + lockfile).

Context (2026-08-10, issue #2323 follow-up): Two parallel gate_and_push.sh
runs deadlock because each gate's zero-builders preflight (gate.sh:162-168)
sees the OTHER gate's cmake --build (or its child shard process) as 'builders
active' and refuses to start. Lane A's fix introduces:

  1. gate.serialize — a Python helper that
     (a) detects "sibling" gates — processes whose ancestor chain includes
         a gate.sh / gate_full_cycle.sh / gate_and_push.sh — so the preflight
         can SKIP them rather than abort on them.
     (b) acquires a flock-based gate lockfile (default
         /var/tmp/bezel-scratch/gate.lock) at the top of every entry point,
         released automatically on process exit. Two gates cannot run the
         heavy phases concurrently even if the preflight were bypassed.

These four tests pin both behaviors so a regression to the pre-fix deadlock
class is caught immediately. Stdlib only (unittest, subprocess, os, tempfile,
pathlib) — no pytest, no new deps. Every subprocess has a hard timeout; the
real gate is ~30 minutes so the tests target the helper directly, not the
entry points.

Pin layout:
  LockfileAcquisitionTest.test_second_acquire_cleanly_refused (Test 1)
  SiblingDetectionTest.test_child_of_gate_named_parent_is_sibling (Test 2)
  SiblingDetectionTest.test_orphan_builder_is_not_sibling (Test 3)
  LockfileReleaseTest.test_lockfile_released_on_subprocess_exit (Test 4)

Fails LOUD on current main (gate.serialize does not exist — ImportError on
import) and PASSES once Lane A's gate/serialize.py lands.
"""

import os
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest

# Make `gate` importable when discovery runs this file directly: this test
# lives at scripts/gate/tests/, so two levels up is scripts/ where the `gate`
# package sits. Mirrors the sys.path dance in every other test_*.py here.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import serialize  # noqa: E402  — fails LOUD pre-fix (ImportError)


# Process comm (argv[0]) names the preflight treats as "this is a gate". The
# bash preflight at gate.sh:162 sources this list from the helper; tests use
# `serialize.GATE_COMM_NAMES` directly (the helper's source of truth) so
# they cannot drift from production.


# ---------------------------------------------------------------------------
# Subprocess + process-tree helpers.
# ---------------------------------------------------------------------------

# Sibling detection keys on /proc/<pid>/cmdline (Critical #2 fix; comm
# is the executable basename — "bash" for every script invoked under
# `#!/usr/bin/env bash`, regardless of which script — so comm-based
# detection silently never matches in production). To fake a "gate.sh"
# or "cmake" process for the helper, we spawn `/bin/bash` with `argv[0]`
# overridden to the gate name via subprocess.Popen's `executable=`
# parameter. The kernel then runs /bin/bash with argv[0]=NAME and
# argv=["NAME","-c","…"], so /proc/<pid>/cmdline contains NAME as
# argv[0] (joined: "NAME -c …") — which is exactly what the helper's
# substring match against GATE_COMM_NAMES looks for.
#
# `comm` stays "bash" — that's the point: it proves the OLD comm-based
# detection would have failed on this exact subprocess. The fact that
# is_gate_sibling now returns True for these fixtures is the regression
# pin.
_BASH_PATH = "/bin/bash"


def _spawn_named(name, hold_s=30):
    """Spawn a `/bin/bash` subprocess with `argv[0] = name` (via
    subprocess.Popen's `executable=` parameter) and have it sleep for
    `hold_s` seconds. Returns the Popen; caller MUST reap it.

    The trick: bash is invoked with `argv[0]=name` and a `-c` body that
    uses bash's `exec` builtin to replace the bash with `sleep`, keeping
    argv[0]=name. After spawn:
      * comm is "sleep" (the executable's basename), NOT "bash".
      * /proc/<pid>/cmdline is "name hold_s" (joined) — so the helper's
        `any(name in cmdline …)` substring match lands.

    The comm='bash' we wanted (to prove the OLD detection would have
    failed) is moot here — the fixture becomes sleep with cmdline
    containing `name`, which is the load-bearing shape. The comm-vs-
    cmdline regression pin is asserted in the IntegrationTest cases
    where comm='bash' (the bash-with-while-loop shape) and cmdline
    contains a gate name.

    Without the inner `exec -a`, bash would optimize `-c "sleep N"` by
    exec'ing directly into sleep and argv[0] would be lost (the
    resulting process is just `sleep N`, no gate name). The `exec -a
    NAME sleep N` preserves argv[0] across the optimization.

    #4489: the return is gated on cmdline actually reflecting `name`, not
    just on `_spawn_detached`'s exec-started barrier -- see the call to
    `_wait_for_cmdline` below for why that barrier alone is insufficient.
    """
    # #3415: detached, so the fixture does not inherit this runner's ancestry.
    # See _spawn_detached for why a plain Popen makes the negative assertions
    # depend on whatever launched the suite.
    proc = _spawn_detached(["sleep", str(hold_s)], argv0=name)
    # #4489: `_spawn_detached`'s fd barrier only proves `execvp` has STARTED
    # -- it fires at the kernel's `do_close_on_exec()`, which runs inside
    # `begin_new_exec()` BEFORE `setup_arg_pages()` installs the new argv
    # that /proc/<pid>/cmdline actually reads from (see `get_mm_cmdline()` /
    # `mm->arg_start`..`arg_end`). Between those two points the target's
    # cmdline reads back EMPTY even though the process is alive and mid-exec
    # into the right program. A caller that samples cmdline immediately
    # after `_spawn_named` returns can land in that window; under load the
    # window widens, which is exactly what made `test_orphan_builder_is_not_sibling`
    # and `test_self_is_not_sibling_when_invoked_as_gate_script` contention-
    # flaky (#4489) despite each already re-checking cmdline itself -- their
    # re-check used `_wait_for_cmdline`, which used to give up on that very
    # first empty read instead of retrying (see its docstring). Making the
    # postcondition explicit HERE, as part of what "spawned" means, is the
    # actual fix: a fixture that can return before its own precondition
    # holds is the defect, not any one caller that forgot to re-poll.
    settled = _wait_for_cmdline(proc.pid, name, timeout_s=5.0)
    if settled is None or name not in settled:
        _kill_proc(proc.pid)
        raise RuntimeError(
            "_spawn_named({!r}): cmdline never settled to contain the "
            "requested argv[0] within 5s; last read: {!r}".format(
                name, _cmdline_of(proc.pid)))
    return proc


class _DetachedProc(object):
    """Popen-shaped handle for a process we deliberately do NOT parent.

    Exposes the subset the fixtures use — `.pid`, `.wait()`, `.poll()` — but
    reaps by polling `/proc/<pid>` rather than `waitpid()`, because a detached
    process is not our child and `waitpid()` would raise ECHILD.

    The other cleanup helpers in this module need no such adaptation:
    `_kill_proc` is `os.kill` + a `/proc` poll, and `_children_of` /
    `_kill_proc_tree` enumerate `/proc` by `PPid`. None of them calls
    `waitpid`, so all three work unchanged on a non-child. (An earlier
    analysis of #3415 suspected `_children_of` of reaping the detached
    fixtures — it does not: it is asked for the children OF a fixture, never
    for the children of the fixture's new parent, so a reparented `PPid` of 1
    or `systemd --user` never widens its result. The real cause was the
    spawn race documented in `_spawn_detached`.)
    """

    def __init__(self, pid):
        self.pid = pid

    def poll(self):
        return None if os.path.isdir("/proc/{}".format(self.pid)) else 0

    def wait(self, timeout=5):
        deadline = time.time() + (timeout if timeout is not None else 5)
        while time.time() < deadline:
            if self.poll() is not None:
                return 0
            time.sleep(0.01)
        return None


def _spawn_detached(argv, argv0=None):
    r"""Spawn `argv` REPARENTED AWAY from this test runner, and return a
    `_DetachedProc`. Raises `RuntimeError` if the fixture cannot be
    established — a broken fixture must report as a broken fixture, not as a
    failure of the predicate under test.

    WHY THIS EXISTS (#3415). `is_gate_sibling` walks a pid's ANCESTOR chain
    substring-matching `GATE_COMM_NAMES` (`gate.sh`, `gate_and_push.sh`,
    `gate_full_cycle.sh`, `run_parallel.sh`, `dev_heavy.sh`). Every negative
    test below asserts `is_gate_sibling(fixture) is False`, which silently
    presumes the fixture has no gate-named ancestor. A fixture spawned with a
    plain `subprocess.Popen` is OUR child, so it inherits the test runner's
    entire ancestry — and the assertion is then decided by whatever launched
    the suite, not by the predicate.

    That is not hypothetical, and it is why this suite could not simply be
    wired into the gate: run it as a gate phase and the runner's ancestors are
    literally `gate_and_push.sh` -> `gate_full_cycle.sh`, so all four negative
    tests fail 100% of the time, by construction.

    The fix is to give the fixture a genuinely clean ancestry rather than to
    weaken the predicate. `is_gate_sibling` is correct in production, where its
    only call site passes pids from `pgrep -x cmake|cc1plus|ld|mold|lld|ninja|
    ninja-build|make|clang(\+\+)?|bezel_tests*` and so never sees a gate-named
    process. A detached
    fixture is also the *faithful* model of what these tests describe — a
    "foreign builder" that is not part of this gate — which a child of the
    test runner can never be.

    MECHANISM. Double fork: the intermediate child forks the real fixture and
    exits immediately, so the kernel reparents the fixture to `init` or
    `systemd --user` (neither carries a gate name). `setsid` /
    `start_new_session=True` is NOT sufficient: it changes the session, not
    `PPid`, and the predicate walks `PPid`.

    SYNCHRONISATION — this is the part that has to be exact. The caller needs
    the fixture to have FINISHED `exec`ing before it reads `/proc`, because
    `_wait_for_cmdline` gives up on its FIRST unreadable poll
    (`if last is None: return None`) and a process is momentarily empty-cmdline
    mid-`exec`. Polling for "cmdline is non-empty" does NOT establish this: the
    pre-`exec` process is a shell whose own cmdline is both non-empty AND
    contains the requested name, so a poll-based barrier returns while the
    fixture is still the launcher. That produced exactly two symptoms —
    `_comm_of` reading 'bash' instead of 'sleep', and `_cmdline_of` reading
    None inside the `exec` window — for a measured 10 failures in 12 runs of
    `SiblingDetectionTest`.

    So the barrier is an fd, not a poll: `ready_w` is close-on-exec (every fd
    from `os.pipe()` is, in Python 3), so it survives `fork` and is closed by
    the kernel at the instant `execvp` succeeds. The parent's blocking read
    therefore returns EOF exactly when the fixture has become the target
    program. On `exec` FAILURE the child writes one byte first, so the parent
    can tell "exec'd" (EOF) from "could not exec" (a byte) with no ambiguity
    and no timing assumption.

    `argv` is passed to `execvp` as a real argument vector, NOT joined into a
    shell string: joining loses the argument boundaries, so
    `["/bin/bash", "-c", "sleep 30"]` would become `bash -c sleep 30`, i.e.
    `bash -c "sleep"` with `$0="30"` — which exits instantly with
    "sleep: missing operand" and made the baseline fixture fail every run.
    """
    real_argv = [argv0 if argv0 is not None else argv[0]] + list(argv[1:])
    ready_r, ready_w = os.pipe()
    pid_r, pid_w = os.pipe()

    intermediate = os.fork()
    if intermediate == 0:  # pragma: no cover — child process
        try:
            os.close(ready_r)
            os.close(pid_r)
            grandchild = os.fork()
            if grandchild == 0:
                os.close(pid_w)
                try:
                    devnull = os.open(os.devnull, os.O_RDWR)
                    os.dup2(devnull, 0)
                    os.dup2(devnull, 1)
                    os.dup2(devnull, 2)
                    if devnull > 2:
                        os.close(devnull)
                    os.execvp(argv[0], real_argv)
                except BaseException:
                    pass
                # Only reached if execvp failed. Tell the parent so it can
                # raise instead of handing back a pid that never became the
                # program under test.
                try:
                    os.write(ready_w, b"E")
                except BaseException:
                    pass
                os._exit(127)
            os.write(pid_w, str(grandchild).encode())
        except BaseException:
            pass
        os._exit(0)

    # --- parent ---
    os.close(ready_w)
    os.close(pid_w)
    try:
        # Reaping the intermediate is what orphans the fixture: the kernel
        # reparents it the moment the intermediate exits, which has already
        # happened by the time waitpid() returns.
        os.waitpid(intermediate, 0)
        raw = b""
        while True:
            chunk = os.read(pid_r, 32)
            if not chunk:
                break
            raw += chunk
        if not raw.strip():
            raise RuntimeError(
                "_spawn_detached({!r}): intermediate never reported a pid".format(
                    argv))
        pid = int(raw.strip())
        # Blocks until execvp succeeds (EOF) or fails (one byte). No timeout
        # race: both outcomes close or write the fd. select() only bounds a
        # pathological hang so a broken fixture cannot wedge the suite.
        readable, _, _ = select.select([ready_r], [], [], 20.0)
        if not readable:
            raise RuntimeError(
                "_spawn_detached({!r}): exec barrier never signalled".format(argv))
        failed = os.read(ready_r, 1)
    finally:
        os.close(pid_r)
        os.close(ready_r)
    if failed:
        raise RuntimeError(
            "_spawn_detached({!r}): execvp failed in the child".format(argv))

    # Establish the postcondition the callers actually depend on, so an
    # environment that cannot provide it fails HERE (as a fixture error naming
    # the cause) rather than as a confusing assertion about the predicate.
    ppid = _ppid_of(pid)
    if ppid is not None and ppid == os.getpid():
        raise RuntimeError(
            "_spawn_detached({!r}): fixture {} was not reparented (PPid is "
            "still this test runner) — its ancestry would be inherited from "
            "whatever launched the suite, which is exactly what #3415 is "
            "about".format(argv, pid))
    return _DetachedProc(pid)


def _comm_of(pid):
    """Return /proc/<pid>/comm (the kernel-truncated process name). Returns
    None if the process has exited (the /proc entry is gone).

    Retained as a sanity-check helper for non-faked subprocesses (e.g.
    `sleep` has comm='sleep'). The fixtures produced by `_spawn_named`
    have comm='bash' regardless of the requested name; use
    `_wait_for_cmdline` for those."""
    try:
        with open("/proc/{}/comm".format(pid), "r") as f:
            return f.read().strip()
    except (IOError, OSError):
        return None


def _cmdline_of(pid):
    """Return /proc/<pid>/cmdline as a space-joined string (NUL-separated
    argv replaced with spaces and stripped), or None if the process has
    exited. Mirrors `serialize._cmdline_of` so the test and helper see the
    same representation."""
    try:
        with open("/proc/{}/cmdline".format(pid), "rb") as f:
            data = f.read()
    except (IOError, OSError):
        return None
    if not data:
        return None
    return data.replace(b"\x00", b" ").decode("utf-8", errors="replace").strip()


def _wait_for_cmdline(pid, expected, timeout_s=5.0):
    """Poll /proc/<pid>/cmdline until it contains `expected` (or timeout).
    Returns the final cmdline (None if the process genuinely exited).
    Substring match — matches the helper's `any(name in cmdline …)` shape.

    #4489: a `None` read from `_cmdline_of` is AMBIGUOUS -- it means either
    (a) the process has exited (/proc/<pid> is gone), or (b) the process is
    alive but between `do_close_on_exec()` and `setup_arg_pages()` inside
    its own `exec`, where /proc/<pid>/cmdline reads back empty even though
    the process is mid-transition into the right program (see
    `_spawn_detached`'s docstring, and `_spawn_named`'s call to this
    function, for why that window exists and survives the exec-started fd
    barrier). An earlier version of this function collapsed that ambiguity
    the wrong way -- `if last is None: return None` treated (b) as (a) and
    gave up on the very first transient empty read instead of retrying.
    Under load that window widens, and giving up on it was the actual
    mechanism behind the #4489 contention-flakiness (confirmed by
    replaying that exact sequence — a `None` read followed by a settled
    one — through this function and observing it return `None` after a
    single call). So only a genuinely-exited process (no `/proc/<pid>`
    directory) ends the poll early; a merely-empty cmdline keeps polling
    until `expected` appears or the timeout elapses.

    Used to verify the fixture setup: the `_spawn_named(name)` subprocess
    is reported as ready when cmdline contains `name`."""
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        last = _cmdline_of(pid)
        if last is not None and expected in last:
            return last
        if not os.path.isdir("/proc/{}".format(pid)):
            # Genuine exit -- no point continuing to poll for a pid that
            # will never produce another /proc/<pid>/cmdline read.
            return None
        time.sleep(0.05)
    return last


def _wait_for_comm(pid, expected, timeout_s=5.0):
    """Poll /proc/<pid>/comm until it equals `expected` (or timeout).
    Returns the final value (None if the process exited). Used for
    non-faked subprocesses (e.g. `sleep` has comm='sleep') where the
    test setup expects a real comm."""
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        last = _comm_of(pid)
        if last == expected:
            return last
        if last is None:
            return None
        time.sleep(0.05)
    return last


def _ppid_of(pid):
    """Return the parent PID of <pid> from /proc/<pid>/status. None on exit."""
    try:
        with open("/proc/{}/status".format(pid), "r") as f:
            for line in f:
                if line.startswith("PPid:"):
                    return int(line.split()[1])
    except (IOError, OSError):
        return None
    return None


def _children_of(parent_pid):
    """Walk /proc and return PIDs whose PPid == parent_pid. Used to locate
    the children of a fixture process without depending on its internal state.
    Empty list if no children exist or parent has exited."""
    out = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        pid = int(entry)
        if pid == parent_pid:
            continue
        if _ppid_of(pid) == parent_pid:
            out.append(pid)
    return out


def _kill_proc(pid):
    """SIGTERM then SIGKILL a PID, swallowing ESRCH (already gone). Best-effort."""
    try:
        os.kill(pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        return
    deadline = time.time() + 2.0
    while time.time() < deadline:
        if _comm_of(pid) is None:
            return
        time.sleep(0.05)
    try:
        os.kill(pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def _kill_proc_tree(root_pid):
    """Walk /proc descendants of root_pid and SIGTERM them. Best-effort."""
    for pid in _children_of(root_pid):
        _kill_proc(pid)
    _kill_proc(root_pid)


def _setup_gate_sibling_chain(hold_s=30):
    """Spawn a 'gate.sh'-comm process that itself spawns a 'cmake'-comm child.

    Returns the (gate_popen, cmake_pid) tuple. The cmake process's
    cmdline starts with 'cmake' (argv[0]='cmake' via os.execvp) and its
    PPid is gate_popen.pid; the gate_popen's cmdline starts with
    'gate.sh' (argv[0]='gate.sh' via os.execvp). Both have comm='bash'
    because the kernel sets comm from the executable basename, NOT from
    argv[0] — that's exactly the production shape (every bash script has
    comm='bash'), and the load-bearing reason sibling detection keys on
    cmdline rather than comm. The child's PPid is the gate's pid, so
    is_gate_sibling(cmake_pid) must return True.

    Construction: a single Python subprocess (the "gate") execs itself
    into a bash with argv[0]='gate.sh', then forks. The child execs
    itself into a bash with argv[0]='cmake' and sleeps. The gate parent
    sleeps. The child's PPid is the gate's pid, so is_gate_sibling(cmake_pid)
    must walk up and find a 'gate.sh' substring in the parent's cmdline.

    The child pid is communicated back to the test via a temp file (the gate
    writes "<child_pid>\\n" to it after the fork returns).
    """
    pidfile = tempfile.NamedTemporaryFile(
        mode="w", suffix=".pid", delete=False)
    pidfile.close()
    pidfile_path = pidfile.name
    # os.execvp replaces the current process image with /bin/bash, so the
    # PID is preserved across the exec and the test can use the parent
    # python3's PID as the gate's PID. The argv[0] ("gate.sh" / "cmake")
    # is what /proc/<pid>/cmdline reads back.
    #
    # The bash body uses `exec -a NAME sleep N` to preserve argv[0]=NAME
    # across bash's `-c` optimization (without it, bash exec's directly
    # into sleep and argv[0] becomes "sleep", losing the gate name).
    sleep_s = str(hold_s)
    script = (
        "import os, time\n"
        "pid = os.fork()\n"
        "if pid == 0:\n"
        "    # Child: become a bash with argv[0]='cmake'.\n"
        "    os.execvp('/bin/bash', ['cmake', '-c', 'exec -a cmake sleep " + sleep_s + "'])\n"
        "else:\n"
        "    # Parent: write the child's PID, then become a bash with\n"
        "    # argv[0]='gate.sh'.\n"
        "    with open(" + repr(pidfile_path) + ", 'w') as f:\n"
        "        f.write(str(pid) + '\\n')\n"
        "    os.execvp('/bin/bash', ['gate.sh', '-c', 'exec -a gate.sh sleep " + sleep_s + "'])\n"
    )
    gate_proc = subprocess.Popen(
        [sys.executable, "-c", script],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Wait for the gate to write the child pid.
    deadline = time.time() + 5.0
    cmake_pid = None
    while time.time() < deadline:
        try:
            with open(pidfile_path) as f:
                content = f.read().strip()
            if content:
                cmake_pid = int(content)
                break
        except (IOError, ValueError):
            pass
        time.sleep(0.05)
    try:
        os.unlink(pidfile_path)
    except OSError:
        pass
    if cmake_pid is None:
        try:
            gate_proc.kill()
            gate_proc.wait(timeout=5)
        except Exception:
            pass
        raise RuntimeError(
            "gate sibling chain setup timed out (no child pid written)")
    # Attach the cmake_pid to the gate_proc for cleanup symmetry.
    gate_proc._cmake_pid = cmake_pid  # type: ignore[attr-defined]
    return gate_proc, cmake_pid


# ---------------------------------------------------------------------------
# Lockfile helpers (run in a fresh subprocess so flock semantics match).
# ---------------------------------------------------------------------------

_LOCKFILE_HELPER_PY = """\
import os, sys, time
sys.path.insert(0, __GATE_PATH__)
from gate import serialize

action = sys.argv[1]
lockfile = sys.argv[2]
extra = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0

if action == "acquire":
    lock = serialize.acquire_gate_lock(lockfile)
    if lock is None:
        print("REFUSED")
        sys.exit(0)
    print("ACQUIRED")
    time.sleep(extra)
elif action == "acquire-timeout":
    lock = serialize.acquire_gate_lock(lockfile, timeout_s=extra)
    print("ACQUIRED" if lock is not None else "REFUSED")
elif action == "release":
    lock = serialize.acquire_gate_lock(lockfile)
    if lock is None:
        print("REFUSED")
        sys.exit(2)
    serialize.release_gate_lock(lock)
    print("RELEASED")
"""


def _lockfile_helper(action, lockfile, extra=0.0, timeout=20):
    """Spawn a subprocess that performs one lockfile action and prints a
    known marker. Returns (rc, stdout_text, stderr_text)."""
    gate_path = os.path.join(os.path.dirname(__file__), "..", "..")
    script_body = _LOCKFILE_HELPER_PY.replace("__GATE_PATH__", repr(gate_path))
    with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False) as f:
        f.write(script_body)
        helper_path = f.name
    try:
        result = subprocess.run(
            [sys.executable, helper_path, action, lockfile, str(extra)],
            capture_output=True, timeout=timeout,
        )
        return (result.returncode, result.stdout.decode().strip(),
                result.stderr.decode())
    finally:
        try:
            os.unlink(helper_path)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Test 1 — lockfile prevents concurrent gates.
# ---------------------------------------------------------------------------

class LockfileAcquisitionTest(unittest.TestCase):
    """test_lockfile_prevents_concurrent_gates.

    Two processes both try to acquire the gate lockfile. The first MUST win;
    the second MUST cleanly refuse (NOT crash, NOT silently acquire the same
    lock, NOT block forever). flock semantics guarantee auto-release on exit,
    so the holder dying releases the lock — the second never gets stuck.

    On current main this test FAILS with ImportError on `from gate import
    serialize`. After Lane A's fix lands, this test passes.
    """

    def test_second_acquire_cleanly_refused(self):
        """Holder grabs the lock; challenger times out after 0.5s and gets
        REFUSED. No corruption, no crash, no forever-block."""
        with tempfile.TemporaryDirectory() as d:
            lockfile = os.path.join(d, "gate.lock")
            holder = subprocess.Popen(
                [sys.executable, "-c",
                 "import sys, time; "
                 "sys.path.insert(0, '{gate}'); "
                 "from gate import serialize; "
                 "l = serialize.acquire_gate_lock('{lf}'); "
                 "assert l is not None, 'holder must acquire'; "
                 "time.sleep(8)".format(
                     gate=os.path.join(os.path.dirname(__file__), "..", ".."),
                     lf=lockfile)]
            )
            try:
                # Give the holder time to acquire (lockfile to appear).
                deadline = time.time() + 5.0
                while time.time() < deadline:
                    if os.path.exists(lockfile):
                        break
                    time.sleep(0.05)
                # Now a fresh subprocess tries with timeout_s=0.5.
                rc, out, err = _lockfile_helper(
                    "acquire-timeout", lockfile, extra=0.5, timeout=10)
                self.assertEqual(rc, 0,
                                 "challenger crashed (rc={}): {}".format(
                                     rc, err))
                self.assertEqual(out, "REFUSED",
                                 "second acquire must signal clean refusal, "
                                 "got: {!r}, stderr={!r}".format(out, err))
                # Holder still owns the lock — verify by acquiring once more
                # in a third subprocess (also times out).
                rc2, out2, _ = _lockfile_helper(
                    "acquire-timeout", lockfile, extra=0.3, timeout=10)
                self.assertEqual(out2, "REFUSED",
                                 "lock must remain held while holder is alive")
            finally:
                holder.kill()
                holder.wait(timeout=5)

    def test_first_acquire_returns_lock(self):
        """Smoke: a fresh acquire in a subprocess returns ACQUIRED, and after
        release a second acquire in the SAME process works again."""
        with tempfile.TemporaryDirectory() as d:
            lockfile = os.path.join(d, "gate.lock")
            rc, out, err = _lockfile_helper("acquire", lockfile, extra=0.5,
                                            timeout=10)
            self.assertEqual(rc, 0,
                             "first acquire crashed: {}".format(err))
            self.assertEqual(out, "ACQUIRED",
                             "first acquire must report ACQUIRED, got: "
                             "{!r}, stderr={!r}".format(out, err))
            self.assertTrue(os.path.exists(lockfile),
                            "lockfile path must exist after acquire")
            # Release + re-acquire in same process.
            rc2, out2, err2 = _lockfile_helper("release", lockfile,
                                               timeout=10)
            self.assertEqual(rc2, 0,
                             "release+re-acquire crashed: {}".format(err2))
            self.assertEqual(out2, "RELEASED",
                             "release+re-acquire must report RELEASED, "
                             "got: {!r}".format(out2))

    def test_acquire_is_nonblocking_when_no_timeout(self):
        """When timeout_s=0 (the default), a second acquire is INSTANT refusal
        rather than indefinite block — callers can't deadlock waiting for a
        gate that will never free."""
        with tempfile.TemporaryDirectory() as d:
            lockfile = os.path.join(d, "gate.lock")
            holder = subprocess.Popen(
                [sys.executable, "-c",
                 "import sys, time; "
                 "sys.path.insert(0, '{gate}'); "
                 "from gate import serialize; "
                 "l = serialize.acquire_gate_lock('{lf}'); "
                 "time.sleep(8)".format(
                     gate=os.path.join(os.path.dirname(__file__), "..", ".."),
                     lf=lockfile)]
            )
            try:
                deadline = time.time() + 5.0
                while time.time() < deadline:
                    if os.path.exists(lockfile):
                        break
                    time.sleep(0.05)
                t0 = time.time()
                rc, out, err = _lockfile_helper(
                    "acquire-timeout", lockfile, extra=0.0, timeout=10)
                elapsed = time.time() - t0
                self.assertEqual(out, "REFUSED")
                self.assertLess(elapsed, 2.0,
                                "default-zero-timeout acquire must NOT block: "
                                "took {:.2f}s".format(elapsed))
            finally:
                holder.kill()
                holder.wait(timeout=5)


# ---------------------------------------------------------------------------
# Test 2 & Test 3 — sibling detection vs orphan detection.
# ---------------------------------------------------------------------------

class SiblingDetectionTest(unittest.TestCase):
    """test_preflight_recognizes_gate_sibling_as_sibling / test_preflight_still_aborts_on_orphan_builder.

    The bash preflight at gate.sh:162-168 calls `serialize.is_gate_sibling`
    on every matched builder PID. If ANY builder is NOT a sibling, the gate
    aborts (the original behavior — this is the regression-prevention cell).
    If ALL builders ARE siblings (or there are none), the preflight passes.
    """

    def test_child_of_gate_named_parent_is_sibling(self):
        """Test 2: a bash with argv[0]='cmake' whose PPid is a bash with
        argv[0]='gate.sh' IS a sibling — the helper walks up, sees the
        parent's cmdline contain 'gate.sh', and returns True.

        Note: comm is 'bash' for BOTH processes (the kernel sets comm
        from the executable basename, NOT from argv[0]). The OLD
        comm-based detection returned False for this fixture — that's
        the regression pin. The NEW cmdline-based detection returns
        True."""
        gate_proc, cmake_pid = _setup_gate_sibling_chain(hold_s=20)
        try:
            self.assertIn(
                "cmake", _wait_for_cmdline(cmake_pid, "cmake", timeout_s=5.0) or "",
                "fake cmake cmdline did not settle: got {!r}".format(
                    _cmdline_of(cmake_pid)))
            self.assertIn(
                "gate.sh", _wait_for_cmdline(gate_proc.pid, "gate.sh",
                                             timeout_s=5.0) or "",
                "fake gate.sh cmdline did not settle: got {!r}".format(
                    _cmdline_of(gate_proc.pid)))
            self.assertEqual(_ppid_of(cmake_pid), gate_proc.pid,
                             "cmake child must be reparented to gate.sh "
                             "parent for the chain to read sensibly")
            # The pin: is_gate_sibling must return True for the cmake child.
            self.assertTrue(
                serialize.is_gate_sibling(cmake_pid),
                "is_gate_sibling({}) returned False; the cmake child's "
                "ancestor chain includes a gate.sh-named process "
                "(cmdline={!r}, ppid={}, parent_cmdline={!r})".format(
                    cmake_pid, _cmdline_of(cmake_pid),
                    _ppid_of(cmake_pid), _cmdline_of(gate_proc.pid)))
        finally:
            _kill_proc_tree(gate_proc.pid)
            gate_proc.wait(timeout=5)

    def test_orphan_builder_is_not_sibling(self):
        """Test 3: a bash with argv[0]='cmake' whose ancestor chain does
        NOT include any gate-named process MUST report
        is_gate_sibling=False.

        The fixture is a `_spawn_named('cmake')` bash (cmdline
        'cmake -c sleep N', comm='bash'). It has NO gate-named ancestor
        in its chain, so the helper must walk all the way up to PID 1
        without finding a match. Pinned: the OLD comm-based detection
        also returned False here (comm='bash' was never a gate name
        either), so this test alone does not pin the FIX — but it pins
        that the new cmdline-based detection does not OVER-match."""
        cmake = _spawn_named("cmake", hold_s=20)
        try:
            actual_cmdline = _wait_for_cmdline(cmake.pid, "cmake",
                                              timeout_s=5.0)
            self.assertIsNotNone(
                actual_cmdline,
                "fake cmake cmdline did not settle: got {!r}".format(
                    _cmdline_of(cmake.pid)))
            self.assertIn(
                "cmake", actual_cmdline,
                "fake cmake cmdline did not contain 'cmake': got {!r}".format(
                    actual_cmdline))
            # Walk the chain manually and confirm NO gate-named ancestor.
            # The substring-match shape (any(name in cmdline …) for the
            # same GATE_COMM_NAMES list the helper uses) is the right
            # match here, because that's what the helper itself does.
            ancestor = cmake.pid
            chain = []
            for _ in range(20):  # depth cap — long chains exist
                ancestor = _ppid_of(ancestor)
                if ancestor is None or ancestor <= 1:
                    break
                cmdline = _cmdline_of(ancestor)
                if cmdline is None:
                    break
                chain.append(cmdline)
                for name in serialize.GATE_COMM_NAMES:
                    if name in cmdline:
                        self.fail(
                            "test setup contaminated: orphan cmake's "
                            "chain includes a gate-named ancestor ({!r} "
                            "matches {!r}); chain: {!r}".format(
                                name, cmdline, chain))
            # The pin: is_gate_sibling must return False for an orphan cmake.
            self.assertFalse(
                serialize.is_gate_sibling(cmake.pid),
                "is_gate_sibling({}) returned True for an ORPHAN cmake; "
                "chain walked: {!r}".format(cmake.pid, chain))
        finally:
            _kill_proc(cmake.pid)
            cmake.wait(timeout=5)

    def test_self_is_not_sibling_when_invoked_as_gate_script(self):
        """Edge: a bash with argv[0]='gate.sh' ITSELF (no ancestor chain
        beyond it) reports is_gate_sibling=False — only ANCESTORS
        qualify. Otherwise the bash preflight would always think the
        running gate was a sibling of itself and the preflight would
        never refuse a real gate.

        Note: argv[0]='gate.sh' is in the bash's cmdline, so the helper
        WOULD match it on a substring search — but `is_gate_sibling`
        only walks ANCESTORS, not the pid itself, so it walks up to
        PID 1 (the test process's parent chain, which has no gate name)
        and returns False."""
        gate_self = _spawn_named("gate.sh", hold_s=10)
        try:
            self.assertIn(
                "gate.sh",
                _wait_for_cmdline(gate_self.pid, "gate.sh", timeout_s=5.0)
                or "",
                "fake gate.sh cmdline did not settle: got {!r}".format(
                    _cmdline_of(gate_self.pid)))
            self.assertFalse(
                serialize.is_gate_sibling(gate_self.pid),
                "is_gate_sibling must return False for the gate process "
                "itself (only ANCESTORS qualify); got True for {}".format(
                    gate_self.pid))
        finally:
            _kill_proc(gate_self.pid)
            gate_self.wait(timeout=5)

    def test_random_unrelated_process_is_not_sibling(self):
        """Regression guard: a normal subprocess (comm='sleep') with no
        gate-named ancestors reports is_gate_sibling=False."""
        unrelated = _spawn_detached(["sleep", "10"])  # #3415: detached
        try:
            self.assertEqual(_comm_of(unrelated.pid), "sleep")
            self.assertFalse(
                serialize.is_gate_sibling(unrelated.pid),
                "is_gate_sibling returned True for an unrelated 'sleep' "
                "process whose ancestor chain has no gate-named process")
        finally:
            _kill_proc(unrelated.pid)
            unrelated.wait(timeout=5)

    def test_deep_chain_recurses_to_root(self):
        """A cmake 4 ancestors deep with a gate.sh 3 ancestors deep reports
        is_gate_sibling=True. Pins the recursion depth.

        Construction: a single Python subprocess forks 3-deep, with the
        outermost (P0) and innermost (P3) exec'ing into a bash whose
        argv[0] is the test role name. The intermediate forks (P1, P2)
        exec into a bash with a benign argv[0] of 'mid' (not a gate
        name), so they do NOT short-circuit the chain. P0's argv[0] is
        'gate.sh' — the only gate-named ancestor — and the helper must
        walk 3 levels up to find it.

        Args[0] (argv[0]) is the load-bearing signal: the kernel sets
        comm from the executable basename (so every fork ends up with
        comm='bash' regardless of argv[0]), and is_gate_sibling keys on
        cmdline (substring match against the joined argv).
        """
        pidfile = tempfile.NamedTemporaryFile(
            mode="w", suffix=".pid", delete=False)
        pidfile.close()
        pidfile_path = pidfile.name
        hold_s = 20
        sleep_s = str(hold_s)
        # Three fork levels. Each fork: parent waits for child to exit, then
        # exits itself (but with the exec trick the parent execs FIRST into
        # a sleeping bash so the test can read the chain before the chain
        # collapses). The innermost execs into 'cmake'.
        script = (
            "import os, sys, time\n"
            # Level 0 (P0): fork, parent execs into 'gate.sh', child continues.
            "pid = os.fork()\n"
            "if pid != 0:\n"
            "    os.execvp('/bin/bash', ['gate.sh', '-c', 'exec -a gate.sh sleep " + sleep_s + "'])\n"
            # Level 1 (P1): fork, parent execs into 'mid', child continues.
            "pid = os.fork()\n"
            "if pid != 0:\n"
            "    os.execvp('/bin/bash', ['mid', '-c', 'exec -a mid sleep " + sleep_s + "'])\n"
            # Level 2 (P2): fork, parent execs into 'mid', child continues.
            "pid = os.fork()\n"
            "if pid != 0:\n"
            "    os.execvp('/bin/bash', ['mid', '-c', 'exec -a mid sleep " + sleep_s + "'])\n"
            # Level 3 (P3): exec into 'cmake', write pidfile, sleep.
            "with open(" + repr(pidfile_path) + ", 'w') as f:\n"
            "    f.write(str(os.getpid()) + '\\n')\n"
            "os.execvp('/bin/bash', ['cmake', '-c', 'exec -a cmake sleep " + sleep_s + "'])\n"
        )
        gate_proc = subprocess.Popen(
            [sys.executable, "-c", script],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.time() + 5.0
            cmake_pid = None
            while time.time() < deadline:
                try:
                    with open(pidfile_path) as f:
                        content = f.read().strip()
                    if content:
                        cmake_pid = int(content)
                        break
                except (IOError, ValueError):
                    pass
                time.sleep(0.05)
            self.assertIsNotNone(
                cmake_pid,
                "deep-chain cmake never appeared under gate.sh ancestor")
            self.assertIn("cmake", _cmdline_of(cmake_pid) or "",
                          "deep-chain cmake cmdline did not settle: "
                          "got {!r}".format(_cmdline_of(cmake_pid)))
            self.assertTrue(
                serialize.is_gate_sibling(cmake_pid),
                "is_gate_sibling missed a cmake 4 ancestors deep under "
                "a gate.sh ancestor (pid={}, cmdline={!r})".format(
                    cmake_pid, _cmdline_of(cmake_pid)))
        finally:
            try:
                os.unlink(pidfile_path)
            except OSError:
                pass
            _kill_proc_tree(gate_proc.pid)
            gate_proc.wait(timeout=5)


# ---------------------------------------------------------------------------
# Test 4 — lockfile released on subprocess exit.
# ---------------------------------------------------------------------------

class LockfileReleaseTest(unittest.TestCase):
    """test_lockfile_released_on_exit.

    flock semantics: when the holding process exits (cleanly OR via signal),
    the kernel releases the flock. A subsequent acquire in a fresh
    subprocess must succeed IMMEDIATELY (not block, not timeout, not fail).
    Without this, a crashed gate leaves a stale lock that no future gate can
    ever acquire.

    This test exits the holder via SIGKILL — the strongest form of "exit"
    (no cleanup handlers run) — and asserts the lock is still released.
    """

    def test_lockfile_released_on_subprocess_exit(self):
        """Holder acquires + sleeps; challenger times out at 0.5s. Kill
        holder with SIGKILL. Fresh challenger acquires successfully (no
        timeout) — proving the lock was released by the kernel."""
        with tempfile.TemporaryDirectory() as d:
            lockfile = os.path.join(d, "gate.lock")
            holder = subprocess.Popen(
                [sys.executable, "-c",
                 "import sys, time; "
                 "sys.path.insert(0, '{gate}'); "
                 "from gate import serialize; "
                 "l = serialize.acquire_gate_lock('{lf}'); "
                 "assert l is not None, 'holder must acquire'; "
                 "time.sleep(30)".format(
                     gate=os.path.join(os.path.dirname(__file__), "..", ".."),
                     lf=lockfile)]
            )
            try:
                # Wait for lockfile to exist.
                deadline = time.time() + 5.0
                while time.time() < deadline:
                    if os.path.exists(lockfile):
                        break
                    time.sleep(0.05)
                # Sanity: a timed-out challenger gets REFUSED while held.
                rc, out, _ = _lockfile_helper(
                    "acquire-timeout", lockfile, extra=0.3, timeout=10)
                self.assertEqual(out, "REFUSED",
                                 "pre-kill challenger must see the lock held")
                # Kill the holder hard.
                holder.kill()
                holder.wait(timeout=5)
            finally:
                if holder.poll() is None:
                    holder.kill()
                    holder.wait(timeout=5)
            # Post-kill: a fresh challenger with timeout_s=1.0 must acquire.
            t0 = time.time()
            rc, out, err = _lockfile_helper(
                "acquire-timeout", lockfile, extra=1.0, timeout=10)
            elapsed = time.time() - t0
            self.assertEqual(rc, 0,
                             "post-kill acquire crashed: {}".format(err))
            self.assertEqual(
                out, "ACQUIRED",
                "lockfile MUST be released on holder exit (flock semantics); "
                "got {!r}, stderr={!r}, elapsed={:.2f}s".format(
                    out, err, elapsed))
            self.assertLess(
                elapsed, 1.5,
                "post-release acquire must be fast (kernel release is "
                "instant); took {:.2f}s".format(elapsed))

    def test_lockfile_released_on_clean_exit(self):
        """The same property via CLEAN exit (return / sys.exit) — the
        production exit path through gate_finish's EXIT trap."""
        with tempfile.TemporaryDirectory() as d:
            lockfile = os.path.join(d, "gate.lock")
            proc = subprocess.Popen(
                [sys.executable, "-c",
                 "import sys; "
                 "sys.path.insert(0, '{gate}'); "
                 "from gate import serialize; "
                 "l = serialize.acquire_gate_lock('{lf}'); "
                 "assert l is not None; "
                 "sys.exit(0)".format(
                     gate=os.path.join(os.path.dirname(__file__), "..", ".."),
                     lf=lockfile)]
            )
            proc.wait(timeout=10)
            self.assertEqual(proc.returncode, 0)
            t0 = time.time()
            rc, out, err = _lockfile_helper(
                "acquire-timeout", lockfile, extra=1.0, timeout=10)
            elapsed = time.time() - t0
            self.assertEqual(out, "ACQUIRED",
                             "lockfile must be released on clean exit: "
                             "got {!r}, stderr={!r}".format(out, err))
            self.assertLess(elapsed, 1.5,
                            "post-clean-exit acquire took {:.2f}s".format(
                                elapsed))

    def test_lockfile_state_visible_across_subprocesses(self):
        """Cross-process visibility: while one subprocess holds the lock,
        a SECOND subprocess sees it as held (times out). After the first
        exits, the second's lock state is released WITHOUT requiring any
        explicit cleanup. Together with test_lockfile_released_on_*, this
        pins the flock semantic both directions."""
        with tempfile.TemporaryDirectory() as d:
            lockfile = os.path.join(d, "gate.lock")
            holder = subprocess.Popen(
                [sys.executable, "-c",
                 "import sys, time; "
                 "sys.path.insert(0, '{gate}'); "
                 "from gate import serialize; "
                 "l = serialize.acquire_gate_lock('{lf}'); "
                 "time.sleep(6)".format(
                     gate=os.path.join(os.path.dirname(__file__), "..", ".."),
                     lf=lockfile)]
            )
            try:
                deadline = time.time() + 5.0
                while time.time() < deadline:
                    if os.path.exists(lockfile):
                        break
                    time.sleep(0.05)
                rc, out, _ = _lockfile_helper(
                    "acquire-timeout", lockfile, extra=0.3, timeout=10)
                self.assertEqual(out, "REFUSED",
                                 "lock state must be visible across processes")
            finally:
                holder.kill()
                holder.wait(timeout=5)
            rc, out, err = _lockfile_helper(
                "acquire-timeout", lockfile, extra=1.0, timeout=10)
            self.assertEqual(out, "ACQUIRED",
                             "after holder exit, lock must be available: "
                             "got {!r}, stderr={!r}".format(out, err))


# ---------------------------------------------------------------------------
# Test 5 (Critical #2 regression pin) — comm-vs-cmdline shapes that prove
# the production bug is fixed. These tests are the load-bearing reason
# this lane exists: the OLD comm-based sibling detection silently returned
# False for the production shape (a bash running scripts/gate_and_push.sh
# has comm='bash', which is not in GATE_COMM_NAMES), so the "builders
# active" preflight always fired against another gate's cmake child.
# The NEW cmdline-based detection returns True for the same shape.
# ---------------------------------------------------------------------------

class IntegrationTest(unittest.TestCase):
    """Critical #2 regression pin: prove cmdline-based sibling detection
    catches what the OLD comm-based detection silently missed.

    Every test in this class targets the production shape: a bash process
    whose argv[0] is the script name (or, equivalently, whose cmdline
    contains a path-component like `scripts/gate_and_push.sh`). comm is
    'bash' for all of them — that's the kernel's behavior, NOT a test
    artifact. The OLD helper would have returned False for every test in
    this class. The NEW helper returns True for the positives and False
    for the negative."""

    def test_integration_gate_path_in_cmdline_is_sibling(self):
        """The production shape: a bash running scripts/gate_and_push.sh
        spawns a child (cmake); the child is a sibling because the bash's
        cmdline contains 'gate_and_push.sh' as a path-component.

        This is THE regression pin for Critical #2. The OLD comm-based
        detection would have read comm='bash' for the parent bash AND
        for the cmake child, and returned False for is_gate_sibling(cmake_pid).
        The NEW cmdline-based detection reads the parent's cmdline
        (which contains the script path), finds 'gate_and_push.sh',
        and returns True.

        Spawns a fake scripts/gate_and_push.sh in a temp dir (so we
        don't run the real gate, which takes ~30 minutes and acquires
        the production lockfile as a side effect). The fixture's
        comm='bash' is the kernel behavior — NOT a test artifact —
        and the load-bearing reason the OLD detection silently failed
        in production."""
        with tempfile.TemporaryDirectory() as d:
            # Step 1: write a python3 helper that forks a cmake child.
            # We use python3 (not bash) for the fork so the parent→child
            # PPid chain is preserved (bash `&` reparents to init).
            py_helper = os.path.join(d, "fork_cmake.py")
            pidfile = os.path.join(d, "child.pid")
            with open(py_helper, "w") as f:
                f.write(
                    "import os, time\n"
                    "pid = os.fork()\n"
                    "if pid == 0:\n"
                    "    os.execvp('/bin/bash', ['cmake', '-c', 'sleep 30'])\n"
                    "else:\n"
                    "    open('{0}', 'w').write(str(pid) + '\\n')\n"
                    "    time.sleep(30)\n".format(pidfile))
            os.chmod(py_helper, 0o644)
            # Step 2: write the fake gate_and_push.sh.
            fake_gate = os.path.join(d, "gate_and_push.sh")
            with open(fake_gate, "w") as f:
                f.write("#!/usr/bin/env bash\n")
                f.write("python3 '{0}'\n".format(py_helper))
            os.chmod(fake_gate, 0o755)
            # Step 3: spawn the gate. cmdline will be:
            #   "/bin/bash /tmp/.../gate_and_push.sh"
            proc = subprocess.Popen(
                [_BASH_PATH, fake_gate],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                # Wait for the child pid to be written.
                deadline = time.time() + 5.0
                cmake_pid = None
                while time.time() < deadline:
                    try:
                        with open(pidfile) as f_:
                            content = f_.read().strip()
                        if content:
                            cmake_pid = int(content)
                            break
                    except (IOError, ValueError):
                        pass
                    time.sleep(0.05)
                self.assertIsNotNone(
                    cmake_pid,
                    "test setup: cmake child pid was not written; "
                    "fake gate_and_push.sh fork never reported its pid")
                # Wait for the parent bash's cmdline to settle.
                deadline = time.time() + 5.0
                parent_cmdline = None
                while time.time() < deadline:
                    parent_cmdline = _cmdline_of(proc.pid)
                    if parent_cmdline and "gate_and_push.sh" in parent_cmdline:
                        break
                    if parent_cmdline is None:
                        self.fail(
                            "fake gate bash exited before cmdline could "
                            "be read")
                    time.sleep(0.05)
                self.assertIsNotNone(parent_cmdline)
                self.assertIn(
                    "gate_and_push.sh", parent_cmdline,
                    "test setup: fake gate_and_push.sh path must appear "
                    "in the bash's cmdline; got {!r}".format(parent_cmdline))
                # The OLD comm-based detection's comm for this process:
                self.assertEqual(
                    _comm_of(proc.pid), "bash",
                    "test setup: the gate bash's comm must be 'bash' "
                    "(the executable basename), NOT 'gate_and_push.sh' "
                    "— that's the kernel behavior that broke the OLD "
                    "comm-based detection; got {!r}".format(
                        _comm_of(proc.pid)))
                # The regression pin: the OLD comm-based detection would
                # have walked up from cmake_pid, read comm='bash' for
                # the parent (and for python3, and for the cmake child),
                # and returned False. The NEW cmdline-based detection
                # reads the parent's cmdline, finds 'gate_and_push.sh',
                # and returns True.
                self.assertTrue(
                    serialize.is_gate_sibling(cmake_pid),
                    "is_gate_sibling({}) returned False for a cmake "
                    "child of a bash running scripts/gate_and_push.sh; "
                    "the OLD comm-based detection silently failed on "
                    "this exact production shape (comm='bash', not in "
                    "GATE_COMM_NAMES). parent_cmdline={!r}".format(
                        cmake_pid, parent_cmdline))
            finally:
                _kill_proc_tree(proc.pid)
                proc.wait(timeout=10)

    def test_integration_argv0_override_is_sibling(self):
        """The exec -a mechanism: a bash with argv[0] overridden to a
        gate name at exec time (via os.execvp("bash", ["gate_full_cycle.sh",
        …])) that itself forks a cmake child. The cmake child is a
        sibling because the parent's cmdline starts with
        'gate_full_cycle.sh'.

        Production-equivalent: this is exactly what a `#!`-interpreted
        script does at exec time — the kernel re-execs the interpreter
        with argv[0] set to the script path. So `bash gate_full_cycle.sh`
        and `execvp("bash", ["gate_full_cycle.sh", …])` produce
        identical /proc/<pid>/cmdline content for the purposes of the
        substring match.

        Uses `gate_full_cycle.sh` (a different gate name) to demonstrate
        that the helper matches any name in GATE_COMM_NAMES, not just
        gate.sh / gate_and_push.sh.

        Note: comm='bash' for the fixture — the OLD comm-based
        detection would have returned False; the NEW cmdline-based
        detection returns True."""
        pidfile = tempfile.NamedTemporaryFile(
            mode="w", suffix=".pid", delete=False)
        pidfile.close()
        pidfile_path = pidfile.name
        hold_s = 20
        sleep_s = str(hold_s)
        # The "gate" process: spawn a python3 that execs itself into
        # bash with argv[0]='gate_full_cycle.sh', then forks a cmake
        # child (also exec'd to bash with argv[0]='cmake'). The child
        # writes its PID to the pidfile.
        script = (
            "import os, time\n"
            "pid = os.fork()\n"
            "if pid == 0:\n"
            "    # Child: become a bash with argv[0]='cmake' that stays\n"
            "    # alive (the inner sleep is a child, so bash itself\n"
            "    # remains the comm='bash' fixture we want to pin).\n"
            "    os.execvp('/bin/bash', ['cmake', '-c', 'trap \\\"\\\" TERM; while true; do sleep 1; done'])\n"
            "else:\n"
            "    # Parent: write the child's PID, then become a bash\n"
            "    # with argv[0]='gate_full_cycle.sh' that stays alive.\n"
            "    with open(" + repr(pidfile_path) + ", 'w') as f:\n"
            "        f.write(str(pid) + '\\n')\n"
            "    os.execvp('/bin/bash', ['gate_full_cycle.sh', '-c', 'trap \\\"\\\" TERM; while true; do sleep 1; done'])\n"
        )
        gate_proc = subprocess.Popen(
            [sys.executable, "-c", script],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.time() + 5.0
            cmake_pid = None
            while time.time() < deadline:
                try:
                    with open(pidfile_path) as f:
                        content = f.read().strip()
                    if content:
                        cmake_pid = int(content)
                        break
                except (IOError, ValueError):
                    pass
                time.sleep(0.05)
            self.assertIsNotNone(
                cmake_pid,
                "test setup: cmake child pid was not written")
            # Wait for the parent bash's cmdline to settle (the exec
            # happens AFTER the pidfile write, so there's a small race).
            deadline = time.time() + 5.0
            parent_cmdline = None
            while time.time() < deadline:
                parent_cmdline = _cmdline_of(gate_proc.pid)
                if parent_cmdline and "gate_full_cycle.sh" in parent_cmdline:
                    break
                if parent_cmdline is None:
                    self.fail(
                        "gate bash exited before cmdline could be read")
                time.sleep(0.05)
            self.assertIsNotNone(parent_cmdline)
            self.assertIn(
                "gate_full_cycle.sh", parent_cmdline,
                "test setup: argv[0] override did not produce a "
                "cmdline containing 'gate_full_cycle.sh'; got {!r}".format(
                    parent_cmdline))
            self.assertEqual(
                _comm_of(gate_proc.pid), "bash",
                "test setup: argv[0]-override bash must have comm='bash' "
                "(the executable basename), not 'gate_full_cycle.sh'; "
                "got {!r}".format(_comm_of(gate_proc.pid)))
            # The OLD comm-based detection would have returned False
            # (comm='bash' for parent and child). The NEW cmdline-based
            # detection must return True.
            self.assertTrue(
                serialize.is_gate_sibling(cmake_pid),
                "is_gate_sibling({}) returned False for a cmake child "
                "of a bash with argv[0]='gate_full_cycle.sh'; the OLD "
                "comm-based detection silently failed on this shape "
                "(comm='bash', not in GATE_COMM_NAMES). "
                "parent_cmdline={!r}".format(cmake_pid, parent_cmdline))
        finally:
            try:
                os.unlink(pidfile_path)
            except OSError:
                pass
            _kill_proc_tree(gate_proc.pid)
            gate_proc.wait(timeout=10)

    def test_integration_baseline_bash_not_sibling(self):
        """A bare `bash -c 'sleep 30'` is NOT a sibling. cmdline
        contains 'bash -c sleep 30' — no gate-name substring, no
        script path. is_gate_sibling must return False.

        This is the negative pin: the cmdline-based detection must NOT
        over-match on every bash invocation. (Production: a developer
        who runs `bash -c 'make'` from a terminal does not have a
        running gate as a sibling.)"""
        # #3415: detached, so this baseline does not inherit the runner's
        # ancestry. The cmdline must stay `bash -c sleep 30` (no gate-name
        # substring) — that is what the test pins — so argv0 stays "bash".
        proc = _spawn_detached([_BASH_PATH, "-c", "sleep 30"], argv0="bash")
        try:
            deadline = time.time() + 5.0
            cmdline = None
            while time.time() < deadline:
                cmdline = _cmdline_of(proc.pid)
                if cmdline is not None:
                    break
                time.sleep(0.05)
            self.assertIsNotNone(
                cmdline,
                "baseline bash exited before cmdline could be read")
            # Sanity: confirm the cmdline genuinely lacks a gate name.
            # If this assertion fails, the test setup is contaminated
            # and the negative pin below proves nothing.
            for name in serialize.GATE_COMM_NAMES:
                self.assertNotIn(
                    name, cmdline,
                    "test setup contaminated: baseline bash cmdline "
                    "should not contain {!r}; got {!r}".format(
                        name, cmdline))
            self.assertFalse(
                serialize.is_gate_sibling(proc.pid),
                "is_gate_sibling({}) returned True for a bare bash "
                "invocation with no gate-named cmdline token; "
                "cmdline={!r}".format(proc.pid, cmdline))
        finally:
            _kill_proc(proc.pid)
            proc.wait(timeout=10)


# ---------------------------------------------------------------------------
# Adversarial self-pass coverage notes.
# ---------------------------------------------------------------------------
#
# AXIS GRID (per the lane protocol §5 — weight the grid toward what the
# change could BREAK, not what it fixes):
#
#   lockfile path:        tests use a fresh tempfile dir per test; the
#                         helper's GATE_LOCK_PATH default is not exercised
#                         here (Lane A's bash preflight handles the default
#                         path; this lane pins the helper's contract on a
#                         caller-supplied path, which is the testable unit).
#   spawn timing:         _children_of polls /proc up to 5s to handle slow
#                         spawns; every test reaps its subprocess with a
#                         hard SIGKILL after a 2s grace.
#   cleanup on crash:     every test is wrapped in try/finally that kills
#                         the subprocess tree; a test failure must NOT
#                         leak a process holding the lockfile (which would
#                         poison subsequent test runs).
#   no-op mode:           no gate is actually invoked; the helper is the
#                         unit under test. The real gate is ~30 minutes and
#                         cannot fit a test budget.
#   cmdline shapes:       integration tests cover (a) script-path-in-cmdline,
#                         (b) argv[0]-override, (c) bare bash (no gate
#                         name). Together they pin both the positive
#                         detection (comm-based missed) AND the negative
#                         bound (no over-match on every bash).
#   comm-vs-cmdline:      every fixture's comm is 'bash' regardless of the
#                         test role. The OLD comm-based detection would
#                         have returned False for every positive test.
#                         The NEW cmdline-based detection returns True.
#                         This is the regression pin for Critical #2.
#
# DETERMINISM: every test uses an isolated tempfile dir, distinct lockfile
# path, and a fresh subprocess. No shared global state; no order-dependence.
#
# PRE-EMPT (3 strongest attacks):
#   1. "What if the test process is killed mid-test?" — every helper
#      subprocess is either SIGKILL'd in finally OR is short-lived (clean
#      exit). flock auto-releases on either path, so a SIGKILL'd test
#      leaves no leaked lockfile. We never block indefinitely; every
#      timeout is bounded.
#   2. "What if a previous test's lockfile is leaked?" — every test uses
#      tempfile.TemporaryDirectory() which is cleaned up on test exit. If
#      a SIGKILL'd helper left a flock, it died with the process (kernel
#      releases flock on process exit regardless of cause). The /tmp-based
#      scratch root is gone before the next test starts.
#   3. "What if /proc/<pid>/cmdline is not yet populated when we read
#      it?" — kernel populates cmdline synchronously at exec; the file
#      is ready by the time subprocess.Popen returns. We poll with a
#      small sleep margin (50ms × 100 = 5s deadline) so a slow spawn
#      does not flake. If cmdline is None on the first read, the
#      process has already exited — fail LOUD rather than retry.
#
# WHAT THIS TEST DOES NOT COVER (explicit limits):
#   - The bash preflight at gate.sh:162-168 wiring is tested by integration
#     at full-gate time. The helper's contract is what this lane pins; if
#     the helper is correct, the preflight (which only calls it) is
#     correct.
#   - The default GATE_LOCK_PATH (/var/tmp/bezel-scratch/gate.lock) is the
#     bash side's responsibility. The helper accepts an arbitrary path
#     (testable); the default is a single-line constant in Lane A's bash.
#   - Concurrent acquire from 3+ processes — the 2-process pin is the
#     load-bearing case (the deadlock that motivated the fix). 3+
#     concurrent acquires reduce to 2-process by induction (the first
#     holds, all others refuse).
#   - The cmdline string is read as bytes and decoded with errors="replace".
#     A pathological argv containing invalid UTF-8 would have its bytes
#     preserved as replacement characters; the substring match would still
#     land against any ASCII gate name (all GATE_COMM_NAMES are ASCII).
#     This is intentional — we do not want a non-UTF-8 byte to crash the
#     helper.


if __name__ == "__main__":
    unittest.main()
