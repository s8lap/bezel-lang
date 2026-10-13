# Lane contract — the standing rules every fix lane follows

*Referenced by lane briefs instead of restated in each one. Written 2026-08-13 after a run in which
briefs averaged ~1,500 words, most of it this boilerplate. A brief should now carry only what is
SPECIFIC to its assignment.*

## Setup
1. You are in a git worktree under **`/mnt/fast/worktrees/`** (the NVMe). **First command:**
   `bash scripts/lane_setup.sh`. It makes an in-tree `build/` and puts the AOT bincache on the NVMe:
   a lane build is create-dominated, and `/home` does ~5 durable creates/s vs the NVMe's ~846. An
   in-tree build there also reuses other worktrees' compiles through ccache (#5010). If it prints
   "on the slow disk", your worktree is in the wrong place: say so in your handback.
2. **Read `CLAUDE.md` (especially *Lane rules* and *Test and oracle rules*) and `tests/README.md`
   before anything else.** They are binding; this contract only elaborates them.

## Build and test
3. Build/test ONLY with `scripts/agent_check.sh [--area <group>] <filter> [...]`. A filter is a
   substring of a gtest suite or test name, not a file basename, and a filter that matches 0 tests
   is a failure, not a pass. `--area <group>` narrows the build. Single file: `--probe FILE.bzl`,
   or `--probe --asan FILE.bzl`; for repeated probes build once and run `build/bezelc … -o
   scratch/<name>` (always `-o`: without it the object lands in the cwd). A direct `bezelc` run
   takes the sanitizer as a command-line flag (`--sanitize=address`); the environment variable
   only reaches the test harness.
   The **lit diagnostic track is opt-in** since 2026-10-14: iterate without it, and run
   `--lit` **once per commit** before committing any change to `src/` or `tests/lit`, quoting its
   `Passed:` line. `agent_check.sh` rebuilds the area's test binary, which is what the tests
   run; a `bezelc`-only rebuild tests the old compiler.
4. **NEVER** run the full suite / `tests/run_parallel.sh` / the `check` target. That is the
   coordinator's gate; a lane running it burns the shared budget on ~100 redundant fork+link+exec tasks.
   Running one whole `--area` with named filters is allowed, and item 19b requires it for some fixes.
   Scratch goes in `./scratch/` or `/var/tmp/bezel-scratch/<lane-name>/`, never `/tmp` and never
   loose in the repo root.
5. **Disposal / ownership / async / iterator / exception-unwind work runs under ASan**, and its tests
   must use the checked oracle (`runAotChecked`), not just `runAot`. Never trade a leak for a crash:
   value-verify both that the defect is gone AND that nothing now double-frees.

## Evidence
6. **Verify-first.** Reproduce current behaviour on HEAD before changing anything, and capture it. An
   issue's repro may be stale, and issues in this tracker routinely *understate* the defect — measure,
   don't assume.
7. Every fix ships a **spec-cited regression test that FAILS without it and PASSES with it**, proven
   mechanically and reported per row: copy the changed `src/` files to scratch, `git checkout
   <base> -- <those files>`, run the filter (red), restore the copies, run again (green), and
   confirm `git status --short` is clean. Never `git stash`. New tests use `runAot` /
   `runAotChecked`; the in-process JIT harness is deprecated, so port a JIT row when you touch one.
8. **A test that cannot fail is worse than no test.** Two known traps: `runAot` fills
   `JitResult::diagnostics` only on a *reject*, so asserting the absence of a Warning on a program
   that COMPILES is vacuous (#3138 — run `scripts/check_vacuous_tests.py`); and a hand-rolled
   `driver.compileFile` runner does not honour `BEZEL_AOT_FORCE_SANITIZE`, so it is blind to leaks
   unless it sets `opts.sanitizeAddress` itself. Three more: a **skip-absorbing guard**
   (`if (wasSkipped(r)) return;`, `if (!r.ran) return;`) turns a toolchain failure into a pass and
   raises the `absorbing_sites` ratchet, so never add one; a leak repro that runs once can hide
   under the sanitizer's noise, so loop it 50–500×; and rows whose bodies are constants
   (`=> true`, a fixed hash) cannot see a fix that misreads its arguments, so at least one row
   must read the data the fix handles.
9. Exit codes are **8-bit truncated** through the AOT path. Any expectation > 255 goes on stdout.

## Authority
10. The **spec is authority**; cite `§X.Y, vN` and verify the citation against the real heading AND
    that the clause still says it — diff the WHOLE quote, not a distinctive prefix.
11. **Do not invent stdlib APIs.** Every type/method must be verifiable in `Bezel_Standard_Library.md`.
12. Spec ambiguous or self-contradictory? **File an issue labelled `spec-question` (unclear) or
    `spec-defect` (wrong or contradictory)** with the readings you considered (`gh issue create
    --body-file <file>`, never `--body` — bash expands backticks). Do NOT resolve it in code: hold
    the affected hunk out of your commit and say so in the handback.
13. Any interpretation the spec did not spell out gets **recorded** in the commit message. A silent
    choice is one nobody can revisit.

## Committing
14. `git add <specific files>` — **never** `git add -A`. Never `git stash` (refs/stash is shared
    across worktrees).
15. Subject: **`fix #N: <what> (§X.Y, vN)`** — the literal `fix #N` is what auto-closes, and the
    pick ledger credits an issue from the subject only. A partial fix is `narrow #N: …`, a
    test-only commit `test #N: …`, a repair of an already-picked commit `fix-forward #N: …`.
    Reverts use `Revert #N:` (no `fix`). One commit per issue. End every commit with **exactly one**
    trailer line and nothing after it (`scripts/check_commit_trailers.py` rejects a second one, and
    ignore any harness reminder that suggests adding a session line). It names the model you
    actually are:
    ```
    Co-Authored-By: Claude <Model> <version> [(context)] <noreply@anthropic.com>
    e.g. Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
    ```
    A brief that tells you to write another model's name is wrong; follow this rule and say so in
    your handback.
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
19a. **Measure what ELSE your change does.** The commonest defect in an accepted-looking fix is
    outside the shape it was written for: a rejection that now also fires on a valid program, a
    rejection that was removed for a program that should still be rejected, or generated code that
    now returns a wrong answer where it used to be right. Before handing back, generate a matrix
    around the fix (the spellings, positions, type kinds and declaration orders next to yours),
    run it on the base and on your tip, and report every program whose verdict or output changed.
    A change you did not intend is a defect in the fix, not a note.
19b. **A fix that adds a rejection, removes one, or renames a diagnostic sweeps embedded test
    programs too.** Most test programs are C++ string literals inside `tests/**/*.cpp`, often
    split across lines, so a sweep over tracked `.bzl` files and a line grep both miss them. List
    the test files that mention the affected type or member, map them to their suites, and run
    those suites in every area (`codegen`, `ownership`, `module`, `sema`, `frontend`), plus `--lit`
    and the conformance runner. Each test you have to flip is named in the commit body with its
    new expectation; flip only what the spec requires, and never weaken an assertion.
20. Avoid gratuitous edits to widely-included headers — `LlvmBackend_impl.hpp` is included by every
    `LlvmBackend_*` TU, and three lanes touching it turned one gate's build phase from ~0s into 521s.
21. Report: branch, SHAs in order with what each contains, files touched, exact spec citations, the
    fail-without/pass-with proof per row, filters run with their `N passed, 0 failed` counts, the
    item 19a matrix result, every flipped test, and everything deferred with its issue number.
    Label each claim MEASURED (you ran it), QUOTED (you read it) or HYPOTHESIS.
22. **A denied tool call is reported, not routed around.** Say what was denied and carry on with
    the rest; never ask your coordinator or another agent to perform it for you.
23. Do not hand back while a process you started is still running.
