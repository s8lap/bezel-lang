#!/usr/bin/env python3
"""check_lit_sources_not_ignored.py - assert the repo .gitignore can neither
swallow a NEW lit test source nor expose lit's real in-tree build artifacts.

WHY (issue #6523). The rule that stood here was

    tests/lit/*[!.]

It read as "a name containing no dot" and is not. git matches a gitignore glob
against the path's BASENAME and lets `*` cross `/`, so the `[!.]` class tested
only the LAST CHARACTER. `row.bzl` ends in `l`, so every newly created lit
source matched - and because git does not descend into an excluded directory,
the match reached EVERY depth, so a row written into `tests/lit/sema/` was
swallowed too. All 2513 tracked `tests/lit/**.bzl` files were force-added past
the rule, which is why the tracked-row count never revealed the hole.

On git 2.55 this is NOT silent: `git add tests/lit/<row>.bzl` prints "the
following paths are ignored" and exits 1 (for a subdir it names the
DIRECTORY, e.g. tests/lit/sema, as ignored). The hazard is that the row is
simply absent from the commit while `git status` never mentions it, and that
one non-zero path inside a multi-path `git add` is easy to lose.

BUT THE RULE WAS NOT USELESS, and deleting it wholesale was a real regression
caught in review. Measured on this tree with a real bezelc on PATH and the
repo's own lit.cfg.py (lit 18.1.8), a hand-run `lit tests/lit` (2513 tests)
leaves 2323 files under tests/lit/:

    1127  EXTENSIONLESS objects named for the row stem, one per row whose RUN
          line compiles without -o, sitting NEXT TO THAT ROW'S SOURCE. These
          were suppressed ONLY by `tests/lit/*[!.]`, and dropping that rule
          without replacement exposed every one of them.
    1196  dotted files: `.lit_test_times.txt` plus %t scratch under **/Output/.

The 1127 are real: `%bezelc -c %s` (2463 such RUN lines) leaves bezelc's
outputPath empty, and bezelc fills it with defaultOutput(inputPath)
(src/main.cpp:254), which strips the directory AND the final extension
(src/main.cpp:96-107) - an extensionless file named for the row stem,
relative to the process CWD. `-c` does not avoid this: line 254 runs before
the compile branch at 259-260.

So BOTH halves are pinned here, because either alone is regressible - and a
ratchet that certifies only the half you happened to fix is worse than none:

  * every lit source suffix must be addable at EVERY depth under tests/lit;
  * every artifact shape actually produced must stay ignored.

WHAT COUNTS AS A LIT SOURCE. The suffix is read from `tests/lit/lit.cfg.py`
(`config.suffixes`) rather than hardcoded, and BOTH the source probes and the
artifact probes are derived from it, so a future suffix change cannot leave
this checker testing a shape nothing produces.

An artifact shape that ever slips through surfaces as an untracked file under
`tests/`, where scripts/untracked_guard.sh FAILs the gate LOUDLY. That is the
failure mode we want: this class of bug must not fail silently again.

Usage:  check_lit_sources_not_ignored.py [--self-test] [--strict]
Exit 0 = every property holds.  Exit 1 = a violation (with --strict, a gate
FAIL).  --self-test reconstructs TWO .gitignore states that each break one
half and requires this detector to FAIL on both, so a green here is not a
matcher that matched nothing.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIT_CFG = os.path.join(REPO, "tests", "lit", "lit.cfg.py")
GITIGNORE = os.path.join(REPO, ".gitignore")

# Two reconstructed .gitignore states, each of which breaks exactly one half.
# The first is r30-c1's rule (the #6523 defect). The second drops the
# suppression entirely, which is what a careless "just delete the broken rule"
# does - and which review caught leaving 1127 artifacts exposed.
PRE_FIX_RULES = ["tests/lit/*[!.]"]
NO_SUPPRESSION_RULES: list = []

# Depths a lane actually writes a row at. `tests/lit/sema/` holds the
# conformance rows; the ignored-parent-directory trap meant a top-level-only
# fix would still have swallowed every one of them.
PROBE_DIRS = [
    "tests/lit",
    "tests/lit/sema",
    "tests/lit/ownership",
    "tests/lit/sema/deeper/still",
]

PROBE_STEM = "zz_probe_row_6523"


def lit_source_suffixes(path=LIT_CFG):
    """The suffixes lit treats as tests, read from lit.cfg.py."""
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        raise SystemExit(f"check_lit_sources_not_ignored: cannot read {path}: {e}")
    m = re.search(r"config\.suffixes\s*=\s*\[([^\]]*)\]", text)
    if not m:
        raise SystemExit(
            "check_lit_sources_not_ignored: no `config.suffixes = [...]` in "
            f"{path}; the checker cannot tell a lit source from anything else."
        )
    sufs = re.findall(r"""["']([^"']+)["']""", m.group(1))
    if not sufs:
        raise SystemExit(f"check_lit_sources_not_ignored: config.suffixes is empty in {path}")
    return sufs


def source_probes(suffixes):
    """Paths a lane might create that MUST be addable."""
    return [f"{d}/{PROBE_STEM}{s}" for d in PROBE_DIRS for s in suffixes]


def artifact_probes(suffixes):
    """The artifact shapes a hand-run `lit tests/lit` actually produces.

    Derived from the real suffixes so `foo.bzl` -> also probe `foo` (the
    defaultOutput row-stem object), `foo.bzl.tmp`, `foo.bzl.tmp.exec`,
    `foo.bzl.tmp.lib.o` and the Output/ copies of the same.
    """
    out = ["tests/lit/.lit_test_times.txt"]          # lit's test-times file
    for d in PROBE_DIRS:
        for s in suffixes:
            # 1. THE 1127: defaultOutput() strips the extension, so the object
            #    is named for the row stem with NO extension at all.
            out.append(f"{d}/{PROBE_STEM}")
            # 2. %t scratch (dotted), beside the source and under Output/.
            out.append(f"{d}/{PROBE_STEM}{s}.tmp")
            out.append(f"{d}/{PROBE_STEM}{s}.tmp.exec")
            out.append(f"{d}/{PROBE_STEM}{s}.tmp.lib.o")
            out.append(f"{d}/Output/{PROBE_STEM}{s}.tmp")
            out.append(f"{d}/Output/{PROBE_STEM}{s}.tmp.exec")
    return out


def ignored(repo, path):
    """True if `path` is ignored. git check-ignore does not need the file to exist."""
    r = subprocess.run(
        ["git", "check-ignore", "-q", "--", path],
        cwd=repo, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    return r.returncode == 0


def violations(repo, suffixes=None):
    """Every (kind, path, detail) that breaks one of the two properties."""
    suffixes = suffixes if suffixes is not None else lit_source_suffixes()
    out = []
    for p in source_probes(suffixes):
        if ignored(repo, p):
            out.append(("SOURCE-SWALLOWED", p,
                        "a new lit source would be ignored: `git add` reports it "
                        "ignored and the row is absent from the commit"))
    for p in artifact_probes(suffixes):
        if not ignored(repo, p):
            out.append(("ARTIFACT-EXPOSED", p,
                        "a lit build artifact is not ignored: it appears in "
                        "`git status --porcelain` and FAILs scripts/untracked_guard.sh"))
    return out


def _seed_repo(td, rules):
    subprocess.run(["git", "init", "-q", td], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(os.path.join(td, ".gitignore"), "w", encoding="utf-8") as f:
        f.write("".join(r + "\n" for r in rules))
    return td


def self_test():
    """The checker must FAIL on each reconstructed state, for the right reason."""
    suffixes = lit_source_suffixes()
    cases = [
        ("pre-fix rule (r30-c1)", PRE_FIX_RULES, "SOURCE-SWALLOWED"),
        ("no suppression at all", NO_SUPPRESSION_RULES, "ARTIFACT-EXPOSED"),
    ]
    rc = 0
    with tempfile.TemporaryDirectory(prefix="lit-ignore-selftest-") as root:
        for i, (label, rules, want) in enumerate(cases):
            td = os.path.join(root, str(i))
            os.makedirs(td, exist_ok=True)
            _seed_repo(td, rules)
            found = violations(td, suffixes)
            hits = [v for v in found if v[0] == want]
            if not hits:
                print(f"check_lit_sources_not_ignored: SELF-TEST FAILED - the "
                      f"{label!r} state produced no {want} finding, so this checker "
                      "does not reproduce the defect it exists to prevent.",
                      file=sys.stderr)
                rc = 1
            else:
                print(f"self-test: {label} -> {len(hits)} {want} "
                      f"(e.g. {hits[0][1]}) - reproduced.")
    return rc


def main(argv):
    strict = "--strict" in argv
    if "--self-test" in argv:
        rc = self_test()
        if rc:
            return rc

    if not os.path.exists(GITIGNORE):
        print(f"check_lit_sources_not_ignored: no {GITIGNORE}", file=sys.stderr)
        return 1

    suffixes = lit_source_suffixes()
    found = violations(REPO, suffixes)
    if found:
        print(f"check_lit_sources_not_ignored: {len(found)} violation(s)")
        for kind, path, why in found:
            print(f"  {kind}  {path}\n      {why}")
        if strict:
            print("check_lit_sources_not_ignored: FAIL (.gitignore must suppress lit's "
                  "real artifacts WITHOUT swallowing a new lit source - see #6523)")
        return 1

    print(f"check_lit_sources_not_ignored: PASS - {len(source_probes(suffixes))} lit "
          f"source path(s) addable, {len(artifact_probes(suffixes))} artifact "
          f"shape(s) suppressed.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))