# Lane contract — the standing rules every fix lane follows

*Referenced by lane briefs instead of restated in each one. Written 2026-08-13 after a run in which
briefs averaged ~1,500 words, most of it this boilerplate. A brief should now carry only what is
SPECIFIC to its assignment.*

## Setup
1. You are in a git worktree. **First command:** `bash scripts/lane_setup.sh` (puts `build/` on the
   NVMe — a lane build is create-dominated and `/home` does ~5 durable creates/s vs the NVMe's ~846).
2. **Read `CLAUDE.md` (especially *Lane rules* and *Test and oracle rules*) and `tests/README.md`
   before anything else.** They are binding; this contract only elaborates them.

## Build and test
3. Build/test ONLY with `scripts/agent_check.sh <filter> [...]` (substring on gtest suite-or-name).
   `--area <group>` narrows the build. Single file: `--probe FILE.bzl`, or `--probe --asan FILE.bzl`.
4. **NEVER** run the full suite / `tests/run_parallel.sh` / the `check` target. That is the
   coordinator's gate; a lane running it burns the shared budget on ~100 redundant fork+link+exec tasks.
5. **Disposal / ownership / async / iterator / exception-unwind work runs under ASan**, and its tests
   must use the checked oracle (`runAotChecked`), not just `runAot`. Never trade a leak for a crash:
   value-verify both that the defect is gone AND that nothing now double-frees.

## Evidence
6. **Verify-first.** Reproduce current behaviour on HEAD before changing anything, and capture it. An
   issue's repro may be stale, and issues in this tracker routinely *understate* the defect — measure,
   don't assume.
7. Every fix ships a **spec-cited regression test that FAILS without it and PASSES with it**, proven
   mechanically (revert `src/`, run the filter, show red; restore, show green) and reported.
8. **A test that cannot fail is worse than no test.** Two known traps: `runAot` fills
   `JitResult::diagnostics` only on a *reject*, so asserting the absence of a Warning on a program
   that COMPILES is vacuous (#3138 — run `scripts/check_vacuous_tests.py`); and a hand-rolled
   `driver.compileFile` runner does not honour `BEZEL_AOT_FORCE_SANITIZE`, so it is blind to leaks
   unless it sets `opts.sanitizeAddress` itself.
9. Exit codes are **8-bit truncated** through the AOT path. Any expectation > 255 goes on stdout.

## Authority
10. The **spec is authority**; cite `§X.Y, vN` and verify the citation against the real heading AND
    that the clause still says it — diff the WHOLE quote, not a distinctive prefix.
11. **Do not invent stdlib APIs.** Every type/method must be verifiable in `Bezel_Standard_Library.md`.
12. Spec ambiguous or self-contradictory? **File a `spec-defect` issue** (`gh issue create
    --body-file <file>`, never `--body` — bash expands backticks). Do NOT resolve it in code.
13. Any interpretation the spec did not spell out gets **recorded** in the commit message. A silent
    choice is one nobody can revisit.

## Committing
14. `git add <specific files>` — **never** `git add -A`. Never `git stash` (refs/stash is shared
    across worktrees).
15. Subject: **`fix #N: <what> (§X.Y, vN)`** — the literal `fix #N` is what auto-closes. Reverts use
    `Revert #N:` (no `fix`). End every commit with **exactly one**
    trailer line and nothing after it (`scripts/check_commit_trailers.py` rejects a second one, and
    ignore any harness reminder that suggests adding a session line):
    ```
    Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
    ```
    Never run `git config user.*`; the author is whatever the harness default is.
16. **Commit incrementally.** Land each self-contained increment as it is proven, rather than one
    commit at the end — a lane that runs out of time should leave landable work, not a dirty tree.
17. **Do not push.** Hand the branch back to your coordinator, who integrates and gates it. Never
    push `main`, never merge, never cherry-pick onto another lane's branch.
18. **File a gh issue for every deferral and every newly-discovered bug** (repro + file:line +
    severity). Finishing part of the scope completely beats half-finishing all of it.

## Before you report
19. Adversarial self-pass: is the predicate over- or under-broad? does it distinguish every shape the
    spec distinguishes and no more? does any existing test's *meaning* change (a rewritten assertion
    with a different value is a silent content change — call each one out)? is it deterministic
    (never iterate an unordered container to emit)?
20. Avoid gratuitous edits to widely-included headers — `LlvmBackend_impl.hpp` is included by every
    `LlvmBackend_*` TU, and three lanes touching it turned one gate's build phase from ~0s into 521s.
21. Report: branch, SHAs in order with what each contains, files touched, exact spec citations, the
    fail-without/pass-with proof, filters run with counts, and everything deferred with its issue number.
