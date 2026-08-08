"""io.py — atomic JSON writes + JSON-lines reads for the gate tooling (#3010).

Every gate artifact that another step reads back (a status file, a timing
record, a manifest) must survive a crash mid-write: a half-written target is
the #3000/#3003 shape — a reader sees plausible-looking garbage and proceeds.
`atomic_json_write` is the single helper the later phases compose on; its crash
invariant (a simulated mid-write crash leaves NO partial target) is pinned by
test_io.py.
"""

import json
import os
import pathlib
import tempfile


def atomic_json_write(path, obj):
    """Serialize `obj` to JSON and install it at `path` atomically.

    Writes to a temp file in the SAME directory as the target (so the rename is
    on one filesystem), fsyncs the temp, then `os.replace`s it onto the target.
    `os.replace` is atomic on POSIX: the target is either the old bytes or the
    new bytes, never a partial overwrite. A crash at any earlier point leaves
    the temp orphaned (cleaned up in the `except`) and the target untouched.

    Per #3009/#3010: the `fsync` before the rename is the durability step the
    AOT cache temp was missing (a torn temp could install under a crash).
    Returns None.
    """
    p = pathlib.Path(path)
    parent = p.parent if str(p.parent) else pathlib.Path(".")
    parent.mkdir(parents=True, exist_ok=True)
    data = json.dumps(obj)
    fd, tmp = tempfile.mkstemp(
        dir=str(parent), prefix=p.name + ".", suffix=".tmp"
    )
    try:
        with os.fdopen(fd, "w") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, str(p))
    except BaseException:
        # Best-effort cleanup of the orphaned temp; never shadow the real error.
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def read_jsonl(path):
    """Read a JSON-lines file into a list of parsed records.

    Blank lines are skipped. Unparseable lines are skipped SILENTLY: this is the
    general-purpose helper, and a malformed line in an arbitrary manifest is not
    necessarily fatal. Callers whose contract treats a malformed line as fatal
    — notably the gate_trend newest-record refusal (Plan-A 1.17) — have their
    own reader (gate.trend.read_trend_log) so this one stays a clean primitive.
    """
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except Exception:
                continue
    return out
