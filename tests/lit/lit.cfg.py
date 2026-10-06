# lit configuration for the Bezel compile-behavior / diagnostic test track.
#
# This is the ADDITIVE, COMPLEMENTARY test harness described in
# tests/lit/README.md.  It drives the REAL `bezelc` binary over `.bzl` source
# files and matches its diagnostics with LLVM's FileCheck — the idiomatic way
# an LLVM-based compiler tests its front-end diagnostics.
#
# It does NOT replace the in-process gtest/JIT suite (tests/ *.cpp, run via
# run_parallel.sh) — that stays load-bearing for runtime / disposal / JIT
# behavior.  See tests/lit/README.md for WHEN to use which.
#
# Machine-independent knobs live here; the machine-specific tool paths
# (bezelc, FileCheck) are injected by the CMake-generated lit.site.cfg.py,
# which loads this file last.

import os

import lit.formats
import lit.util

# ── Suite identity ───────────────────────────────────────────────────────────
config.name = "Bezel-lit"

# Each test is a shell script embedded in `// RUN:` lines of a `.bzl` file;
# ShTest runs those RUN lines and reports PASS/FAIL per file.  Because lit runs
# each test as its OWN process, cross-test global-state leakage (the shard-
# pollution flake class the in-process suite is exposed to) cannot happen here.
config.test_format = lit.formats.ShTest(execute_external=False)

# Only `.bzl` files are tests.  (README, requirements, *.py configs are ignored.)
config.suffixes = [".bzl"]

# ── Roots ────────────────────────────────────────────────────────────────────
# Source root: the directory holding this config (and the .bzl tests).
config.test_source_root = os.path.dirname(__file__)
# Exec root: where lit writes per-test scratch (`%t`).  The site config sets
# this to the CMake build tree; fall back to the source tree for a hand-run
# `lit tests/lit` with no generated site config.
if not getattr(config, "test_exec_root", None):
    config.test_exec_root = config.test_source_root

# ── Substitutions ────────────────────────────────────────────────────────────
# `config.bezelc` and `config.filecheck` are set by lit.site.cfg.py (from the
# CMake-configured absolute paths).  When someone runs `lit tests/lit` WITHOUT
# a generated site config, fall back to discovered defaults so the suite is
# still runnable ad hoc.
bezelc = getattr(config, "bezelc", None) or lit.util.which("bezelc") or "bezelc"
filecheck = (
    getattr(config, "filecheck", None) or lit.util.which("FileCheck") or "FileCheck"
)
# `not` (the LLVM utility that inverts a command's exit status) is REQUIRED by
# every reject test — see the block comment below.  It ships with LLVM
# alongside FileCheck.
not_tool = getattr(config, "not_tool", None) or lit.util.which("not") or "not"

# `%bezelc`     → the compiler driver under test.
# `%not`        → LLVM's `not` (exit-status inverter).
# `%FileCheck`  → the FileCheck binary (explicit, unambiguous form).
# `FileCheck`   → bare form, so the canonical `... | FileCheck %s` RUN line
#                 (as written throughout LLVM) works verbatim.  These patterns
#                 do not overlap, so substitution order is immaterial.
#
# WHY `%not` on every reject test — a lit-specific hazard we hit and pinned:
#   A reject test's whole point is that `bezelc` EXITS NON-ZERO.  But in a
#   pipeline `bezelc … 2>&1 | FileCheck`, lit's shell DROPS the output of a
#   non-zero-exit producer stage (verified on lit 18.1.8: FileCheck then sees
#   empty input and the test spuriously fails).  Wrapping the compile in `not`
#   makes the failing stage exit 0, so its diagnostics flow to FileCheck — this
#   is exactly why LLVM's own front-end tests are written `not %clang … |
#   FileCheck`.  Accept tests, which exit 0, need no `not`.
config.substitutions.append(("%bezelc", bezelc))
config.substitutions.append(("%not", not_tool))
config.substitutions.append(("%FileCheck", filecheck))
config.substitutions.append(("FileCheck", filecheck))

# WHY a per-test timeout (#5431).  lit's default for this is 0, which means
# UNLIMITED (lit/LitConfig.py:37 sets `maxIndividualTestTime=0`, and
# TestRunner.py:1075 passes it straight through as the `executeShCmd` timeout).
# So a single wedged `bezelc` — an infinite loop, or a process blocked on a
# dead pipe — stalls the whole lit track for the entire gate cycle with no
# bound and no diagnostic.  That is the gate's worst failure mode: a cycle that
# never returns rather than a cycle that fails.
#
# lit only *reports* a timeout when the value is > 0
# (TestRunner.py:1152: `if maxIndividualTestTime > 0 and
# result.timeoutReached`), and TimeoutHelper kills the child's process group,
# so the hang becomes a clean `Test.TIMEOUT` and the cycle proceeds.
#
# 300 s, deliberately generous.  Every lit row here is compile-only
# (`bezelc -c` feeding FileCheck — no link, no execute, no ASan runtime), and
# the whole 2439-row track completes in seconds of wall time across 20
# workers, so 300 s is orders of magnitude above any legitimate row.  Erring
# high is deliberate: the failure we are buying insurance against is an
# infinite hang, which ANY finite bound catches, whereas a bound set too low
# would fail the gate on a merely slow row.  A false positive here costs a
# gate cycle; a false negative costs the whole cycle.
config.maxIndividualTestTime = 300

# A minimal, hermetic environment is fine: `bezelc -c` neither reads config
# files nor needs a populated PATH.  Keep HOME/TMPDIR so tmp-file creation
# behaves, and put the LLVM tools directory (FileCheck/`not`) on PATH so a bare
# `FileCheck`/`not` in a RUN line also resolves.
for var in ("HOME", "TMPDIR", "TEMP", "TMP"):
    if var in os.environ:
        config.environment[var] = os.environ[var]
_tools_dir = os.path.dirname(filecheck) if os.path.dirname(filecheck) else "/usr/bin"
config.environment["PATH"] = os.pathsep.join(
    [_tools_dir, os.environ.get("PATH", "/usr/bin")]
)
