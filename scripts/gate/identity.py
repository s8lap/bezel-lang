"""identity.py — the k2 cache identity + the per-oracle marker (Phase 5, #3010).

This is the Python port of the identity/marker logic in gate.sh:597-662 and
:986-990. It is FOUNDATION ONLY this lane: the functions are unit-tested, NOT
yet wired into gate.sh (Wave 2 of the migration wires them — see the plan).
gate.sh is untouched here.

THE k2 IDENTITY (gate.sh:615; tests/support/aot_support.hpp:1108-1127). The AOT
bincache keys each entry on the per-process build identity the COMPILER computes
— SHA-256(compiler ‖ runtime), where `compiler` = libbezel_core.a and `runtime`
= libbezel_runtime.a (aot_support.hpp:1121-1122). The GATE cannot recompute the
in-process identity, but it can recompute the SAME digest from the on-disk
archives: `cat core.a runtime.a | sha256sum` is byte-identical to the pre-#3006
k2 `combined` (compiler ‖ runtime; #3006 folds the toolchain into the in-process
key, but the gate's prediction only needs the archive identity — a toolchain
change rebuilds core.a, so the archive digest already moves). Recording this
digest next to the store, and refreshing it only after a COMPLETED pass, lets the
next gate tell a WARM store (same identity) from a COLD one (identity CHANGED —
the store holds entries under a STALE key).

THE PER-ORACLE MARKER (Plan-A invariant 1.14 / #2918): plain and ASan are
DISJOINT populations in ONE store. A bincache entry's filename is content-
addressed and carries the `sa=` sanitizer bit (aot_support.hpp:1262) as part of
its MODE string, NOT its identity — so the identity digest is oracle-BLIND (the
same core.a ‖ runtime.a for both oracles). The store-wide COUNT is therefore
oracle-blind too, and can only ever err toward warm; the per-oracle IDENTITY
MARKER is the load-bearing signal that actually forces the cold verdict. The
marker filename MUST be per-oracle (`.gate-identity` for plain,
`.gate-identity-asan` for ASan): a single shared marker would let an ASan run
read a plain-populated store's marker and wrongly predict warm.

Stdlib only (no pytest). See test_identity.py for the load-bearing pins:
  - the identity is stable across re-links (same archives ⇒ same hash)
  - it moves when core.a OR runtime.a changes (either half invalidates)
  - the marker filename is a function of oracle (per #2918)
  - the sa= bit parity against aot_support.hpp:1121-1122 / :1262
"""

import hashlib
import pathlib

# The per-oracle marker filenames. per #2918 (Plan-A 1.14): plain and ASan are
# disjoint populations in ONE store, so the marker — the only oracle-
# discriminating signal — MUST be per-oracle. Matches gate.sh:616-620.
MARKER_PLAIN = ".gate-identity"
MARKER_ASAN = ".gate-identity-asan"


def compute_identity(core_path, runtime_path):
    """Return the k2 cache identity: SHA-256(libbezel_core.a ‖ libbezel_runtime.a).

    Faithful port of gate.sh:615 (`cat core.a runtime.a | sha256sum`). The byte
    order is compiler-then-runtime, matching aot_support.hpp:1121-1122 /
    :1216-1221 (compiler = core.a, runtime = libbezel_runtime.a). Streams in
    1 MiB chunks so the ~100 MB archives never load whole. Returns the hex
    digest, or "" if either archive is missing/unreadable — matching the bash
    `2>/dev/null` swallow (gate.sh:622-628 sets gate_id_note "unavailable").

    The digest is oracle-BLIND by construction: the `sa=` sanitizer bit lives in
    the entry's MODE string (aot_support.hpp:1262), NOT the identity. per #2918
    the per-oracle disambiguation is the MARKER FILENAME's job, not this hash's.
    """
    h = hashlib.sha256()
    for archive in (core_path, runtime_path):
        try:
            with open(archive, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
        except OSError:
            # per gate.sh:615 `2>/dev/null`: an unreadable/absent archive ⇒ no
            # identity. The caller treats "" as "unavailable" (cold by the marker
            # path), never as a key — fail CLOSED, never mint a stale identity.
            return ""
    return h.hexdigest()


def marker_filename(oracle):
    """Return the per-oracle identity-marker filename. per #2918 (Plan-A 1.14).

    `oracle` is "plain" or "asan" — the gate's two modes (gate.sh:616-620 reads
    the ASAN flag). Plain and ASan entries share ONE bincache store but are
    DISJOINT populations; the marker — the only oracle-discriminating signal —
    MUST be per-oracle, so an ASan run cannot read a plain-populated store's
    marker and wrongly predict warm. "asan" selects the ASan marker; any other
    value selects the plain marker (the bash default branch).
    """
    if oracle == "asan":
        return MARKER_ASAN
    return MARKER_PLAIN


def read_marker(path):
    """Return the identity recorded in a marker file, or "" if absent/unreadable.

    Faithful port of gate.sh:623 (`cat "$GATE_CACHE_ID_FILE" 2>/dev/null || echo
    none`). A missing/blank marker means "no recorded identity"; the caller
    (cold.is_cold via marker_changed) treats marker=="" != computed as CHANGED,
    i.e. cold — the safe direction for a first run. Returns the stripped digest.
    """
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


def write_marker(path, identity):
    """Record `identity` in the marker file. per gate.sh:986-990.

    The gate calls this only after a COMPLETED pass (gate.sh:979-985:
    `rc==0 && COV_GTESTS>0`) so a timed-out gate cannot mark a half-populated
    store as warm. Best-effort: never raises — a marker-write failure must not
    fail a gate that already passed (the bash trails every redirection with
    `2>/dev/null`). A missing/empty `identity` is a no-op: gate.sh:986 only
    writes when GATE_CACHE_ID is set.
    """
    if not identity:
        return
    p = pathlib.Path(path)
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(identity + "\n")
    except OSError:
        pass  # best-effort — per gate.sh:988 `2>/dev/null`
