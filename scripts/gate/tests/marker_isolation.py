"""Redirect the gate's terminal markers away from the production paths (#3463).

NOT a test module — the name deliberately does not match unittest's `test*.py`
discovery pattern, so this file is imported, never collected.

WHY IT EXISTS. `scripts/gate_and_push.sh` publishes two files that are the
gate's completion signal:

    $BEZEL_GATE_PIDFILE   its pid, so `scripts/wait_pid.sh` can wait on IDENTITY
    $BEZEL_GATE_DONEFILE  its exit status, written once from the EXIT trap

Both default to fixed paths under /var/tmp/bezel-scratch, i.e. GLOBAL state
shared with whatever gate happens to be running. Several tests in this directory
drive the REAL script as a subprocess; #3415 then wired this directory into
every gate cycle as the `gate-selftest` phase. The result was that a gate ran
self-tests that overwrote its own markers:

    test_repo_identity           -> pidfile DELETED, donefile "1" (gate FAILED)
    test_integration_bash_python -> pidfile DELETED, donefile "0" (gate PASSED)

against a live, healthy gate that went on to pass both oracles. `wait_pid.sh`
exited 3 about fifty minutes early.

`gate_and_push.sh` was fixed so that only an invocation that will actually gate
publishes markers, which covers `--help` and every refusal path. It CANNOT cover
a test that runs the script in its real gating shape (test_repo_identity drives
it with a stub cycle and no arguments) — that invocation is a gate, as far as the
script can tell, and publishing is the correct thing for it to do. Isolation is
the other half, and it has to live here.

The precedent is already in this directory: test_gate_serialization and
test_repo_identity both keep their probes off the production LOCKFILE. The
markers are the same kind of shared document and had simply not been considered.

Usage:

    with isolated_markers(os.environ) as env:
        subprocess.run([GATE_AND_PUSH, "--help"], env=env, ...)

`scripts/gate/tests/test_gate_markers.py` pins that every module here which
spawns the real script leaves a sentinel marker pair byte-identical, so
forgetting this helper fails the gate-selftest phase rather than a later gate.
"""

import contextlib
import os
import tempfile

__all__ = ["isolated_markers", "marker_env"]


def marker_env(base_env, directory):
    """A copy of `base_env` whose gate markers point inside `directory`.

    Takes the environment explicitly rather than reading os.environ, because the
    callers here build their own (GIT_ENV pins the git identity), and silently
    dropping those additions would be a second defect of the same shape.
    """
    env = dict(base_env)
    env["BEZEL_GATE_PIDFILE"] = os.path.join(directory, "gate.pid")
    env["BEZEL_GATE_DONEFILE"] = os.path.join(directory, "gate.done")
    return env


@contextlib.contextmanager
def isolated_markers(base_env):
    """Context manager yielding an env with the markers in a fresh temp dir."""
    with tempfile.TemporaryDirectory(prefix="gate-markers-") as d:
        yield marker_env(base_env, d)
