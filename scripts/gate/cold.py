"""cold.py — the cold/warm prediction (Phase 5, #3010 / #2100 / #2918 / #3000).

Python port of gate.sh:597-662. FOUNDATION ONLY this lane — unit-tested, NOT yet
wired into gate.sh (Wave 2 wires it; see the migration plan). gate.sh untouched.

THE PREDICATE (#2100, gate.sh:600-628). A gate run is COLD when ANY of three
independent signals fires; else WARM:

  1. IDENTITY CHANGED — core.a was rebuilt THIS gate (gate.sh:601:
     `[ "$core_post" != "$CORE_MTIME_PRE" ]`). A compiler rebuild invalidates
     every cache entry regardless of count.
  2. NEAR-EMPTY BINCACHE — `cache_n < 2000` (gate.sh:601). A store below this
     size has not been populated enough to serve a full suite; the warm ceiling
     would kill shards mid-suite.
  3. MARKER CHANGED — the identity recorded next to the store does not match the
     identity computed THIS gate (gate.sh:622-625). The store holds entries under
     a STALE key — they are orphans, never hits — so the store is effectively
     cold regardless of the raw count. This is the load-bearing signal: a
     timed-out previous gate leaves a full-looking store under an OLD identity,
     and only the marker catches it (#2100 / gate.sh:602-614).

THE CEILING (#2100 / #2918 / gate.sh:629-662). COLD ⇒ 1800s; WARM ⇒ 900s — for
BOTH oracles. #2918 hoisted the cold logic off the ASan-only branch it lived in;
before that the plain oracle fell through to run_parallel.sh's hardcoded 300s
fallback and was killed at it (#3000 follow-on; f60c1cb: 14 timeout-victim
suites, 5156 of 16482 tests). The warm 900s sits ~2.5x above the observed
whole-suite plain wall (358s) and still fails fast on a genuine hang; the
coverage floor is the real backstop, this only bounds how long a wedged shard is
tolerated.

THE COUNT (#3000, gate.sh:599). `find -L "$GATE_CACHE_ROOT" -type f | wc -l`. The
`-L` is LOAD-BEARING: the default cache root is a SYMLINK on this box
(/var/tmp/bezel-scratch/aot-bincache -> /mnt/fast/aot-bincache), and `find`
without `-L` does not traverse a symlinked START POINT — it returned 0
unconditionally against a store of 102,299 entries, so `cache_n < 2000` always
fired and the gate always predicted COLD (the third category-defect variant: a
correct predicate fed by a source that never held the population). Do not drop
the -L. The count is also oracle-blind: entry filenames are content-addressed,
so the `sa=` bit is not inspectable; per #2918 the per-oracle marker is what
forces the cold verdict.

THE EXCLUSIONS (#3690). The count is of ENTRIES: `! -name '*.sz'` drops the
`<entry>.sz` size sidecars a no-fsync-mode store writes beside every entry
(BEZEL_AOT_STORE_FSYNC=0, #3669 — the mode the gate runs in since #3690), and
`! -name '*.tmp.*'` drops in-flight/orphaned store temps (#3003). Without the
first, cache_n doubles for no-fsync entries and the 2000 threshold means ~1000
real entries; the error direction is toward WARM, whose 900s ceiling is the
TIGHTER one — a shard killed at the ceiling on a store colder than the count
says. Not fail-safe, hence excluded rather than tolerated.

Stdlib only (no pytest). See test_cold.py for the truth-table + find-L pins.
"""

import argparse
import collections
import subprocess
import sys

# The cold/warm ceilings, in seconds, for BOTH oracles. per #2918 /
# gate.sh:629-662 (cold=1800, warm=900).
CEILING_COLD = 1800
CEILING_WARM = 900

# A bincache below this many entries is treated as near-empty (cold).
# per #2100 / gate.sh:601.
COLD_CACHE_THRESHOLD = 2000

# The combined verdict a gate caller consumes: the count observed, the cold/warm
# decision, and the ceiling that decision selects. Exposed so a caller reads one
# value; the pieces are also individually testable (count_cache / is_cold /
# ceiling).
ColdResult = collections.namedtuple("ColdResult", ["cache_n", "cold", "ceiling"])


def is_cold(identity_changed, cache_n, marker_changed):
    """The cold/warm predicate. COLD iff ANY signal fires; else WARM.

    per gate.sh:600-628 (the three OR'd conditions). The arguments are the three
    independent signals:
      - identity_changed: core.a was rebuilt THIS gate (the mtime moved)
      - cache_n: the bincache entry count (near-empty ⇒ cold)
      - marker_changed: the recorded identity != the computed identity
    """
    # per #2100: each signal alone forces COLD; WARM requires all three quiet.
    return bool(
        identity_changed
        or cache_n < COLD_CACHE_THRESHOLD
        or marker_changed
    )


def ceiling(cold):
    """Return the shard-time ceiling (seconds) for a cold/warm verdict.

    per #2918 / gate.sh:653-657: cold ⇒ 1800s, warm ⇒ 900s, for BOTH oracles.
    (The plain warm ceiling was 300s before #2918/#3000 — run_parallel.sh's
    hardcoded fallback that killed a healthy plain gate at f60c1cb.)
    """
    return CEILING_COLD if cold else CEILING_WARM


def count_cache(path):
    """Count the ENTRIES in the AOT bincache at `path`. per #3000 (gate.sh:599)
    and #3690.

    Uses `find -L <path> -type f ! -name '*.sz' ! -name '*.tmp.*'`.

    The `-L` is LOAD-BEARING: the default cache root is a SYMLINK
    (/var/tmp/bezel-scratch/aot-bincache -> /mnt/fast/...), and `find` without
    `-L` does not traverse a symlinked START POINT, returning 0 against a store
    of 102,299 entries.

    The two `! -name` predicates (#3690) keep the count meaning what
    COLD_CACHE_THRESHOLD assumes — cache entries, not files:
      * `<entry>.sz` is the size sidecar a no-fsync-mode store writes beside
        every entry (BEZEL_AOT_STORE_FSYNC=0, #3669; the gate's mode since
        #3690). Counting it doubles cache_n for such entries and biases the
        verdict toward WARM — the TIGHTER (900s) ceiling — on a store that is
        colder than the count says. The sidecar suffix cannot collide with an
        entry name (entries are 64-hex keys, aot_support.hpp cacheSidecarPath).
      * `.tmp.<pid>.<n>`, `<key>.tmp.<pid>.<n>` and `.tmp.sz.<pid>.<n>` are
        in-flight (or orphaned, #3003) store temps; the harness's own GC never
        counts them as entries either.

    Returns 0 on any error (the bash `2>/dev/null` swallow); an
    inaccessible/absent store is cold by signal 2 regardless, so 0 here is
    fail-safe.
    """
    try:
        out = subprocess.run(
            ["find", "-L", str(path), "-type", "f",
             "!", "-name", "*.sz", "!", "-name", "*.tmp.*"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except OSError:
        return 0
    # `wc -l` counts newlines; `find` emits one path per line each with a
    # trailing newline, so splitlines() matches wc -l (and yields 0 for an empty
    # store, where find emits nothing).
    return len(out.stdout.splitlines())


def evaluate(identity_changed, cache_root, marker_changed):
    """One-shot: count the cache, decide cold/warm, pick the ceiling.

    Returns a ColdResult(cache_n, cold, ceiling) — the three values a gate
    caller needs for both the shard-timeout env and the human-facing note
    (gate.sh:658-662). The pieces are exposed individually (count_cache /
    is_cold / ceiling) so each is unit-testable in isolation.
    """
    n = count_cache(cache_root)
    cold = is_cold(identity_changed, n, marker_changed)
    return ColdResult(cache_n=n, cold=cold, ceiling=ceiling(cold))


# ---------------------------------------------------------------------------
# CLI — gate.sh calls `python3 -m gate.cold evaluate ...` (Wave 2). Prints
# shell-sourceable `cache_n`/`cold`/`ceiling` lines that gate.sh evals; these
# are the three values the old bash derived inline (#2100 / #2918 / #3000).
# ---------------------------------------------------------------------------

def _main_evaluate(argv):
    """`evaluate --cache-root P [--identity-changed 0/1] [--marker-changed 0/1]`.

    Prints cache_n / cold / ceiling (shell-sourceable). The two changed-flags
    are 0/1 ints (bool(args.x)) so gate.sh can pass `[ "$x" = ... ]` results
    without quoting. An explicit caller BEZEL_SHARD_TIMEOUT is still resolved
    BY THE CALLER (this returns the ceiling the caller either uses or overrides).
    """
    p = argparse.ArgumentParser(prog="cold.py evaluate")
    p.add_argument("--cache-root", required=True)
    p.add_argument("--identity-changed", type=int, default=0)
    p.add_argument("--marker-changed", type=int, default=0)
    args = p.parse_args(argv)
    r = evaluate(bool(args.identity_changed), args.cache_root,
                 bool(args.marker_changed))
    print("cache_n=%d" % r.cache_n)
    print("cold=%d" % (1 if r.cold else 0))
    print("ceiling=%d" % r.ceiling)
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "evaluate":
        return _main_evaluate(argv[2:])
    sys.stderr.write("usage: cold.py evaluate --cache-root PATH ...\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
