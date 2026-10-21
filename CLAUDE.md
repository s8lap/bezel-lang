# CLAUDE.md — Operating instructions for Claude Code sessions in this repository

This file is the first thing every Claude Code session should read. It encodes the conventions and disciplines this project operates under. Following these rules is more important than moving quickly; a fast contribution that drifts from the spec is worse than a slow one that doesn't.

This file holds the **rules**. The evidence behind the operational rules (the incidents that paid for them) and the full coordinator procedures live in [`docs/ops/coordinator-runbook.md`](docs/ops/coordinator-runbook.md). Lanes also follow [`docs/ops/lane-contract.md`](docs/ops/lane-contract.md), reviewers [`docs/ops/review-contract.md`](docs/ops/review-contract.md), and a peer coordinator follows [`docs/ops/peer-contract.md`](docs/ops/peer-contract.md) (index: [`docs/ops/README.md`](docs/ops/README.md)). If any of them disagrees with this file, this file wins.

## What this repository is

Bezel is a compiled language for services and tools, with ownership-based memory management and no garbage collector. Compile-time ownership tracking ensures every heap object is disposed exactly once; borrowing lets references be passed without transferring ownership, with lifetimes checked statically.

The design is mostly settled: a normative spec (with rationale, stdlib and guides alongside; each document's header carries its current revision), multiple verification passes, and dozens of recorded design decisions. A working compiler exists (`build/bezelc`): an AOT front-end plus LLVM backend that implements a large fraction of the spec, exercised by an extensive automated test suite (gtest behavior tests + a lit/FileCheck diagnostic track). There are no external users yet. The current work is closing the gap to the spec (bug-fixing, soundness hardening, catching up to newer spec revisions), and each fix ships a spec-cited regression test.

## The documents (read these before doing anything)

Twelve documents in `docs/`. Their roles are distinct; consult the right one for the question at hand.

**If you are about to *write* Bezel code, read [`Bezel_For_Code_Generation.md`](docs/Bezel_For_Code_Generation.md) first.** It is a dense writing reference (syntax surface, ownership rules as obligations, a stdlib name index with exact signatures, idioms, and a *looks-right-is-rejected* section) at roughly a fifteenth the size of the spec plus stdlib. It is **not normative**: where it and the spec disagree, the spec wins and the disagreement is a defect to report.

- **`Bezel_Language_Spec.md`**: the normative reference. Appendix A is the complete grammar; Appendix C lists compiler-enforced invariants.
- **`Bezel_Standard_Library.md`**: every type, method and constant the stdlib provides. **Do not invent stdlib APIs.**
- **`Bezel_Design_Rationale.md`**: *why* each decision was made and what alternatives were declined.
- **`Bezel_Changelog.md`**: per-revision history, oldest first, normative for migration. Pinned: never edited retroactively, never the place to look up a current rule.
- **`Bezel_For_Code_Generation.md`**: the writing reference above. `scripts/check_docs.sh`'s PIN check fails if it falls more than three revisions behind.
- **`Bezel_Guide_01` … `07`** (Memory and Ownership, Errors and Nullability, Modeling Data, Concurrency, Collections and Iteration, Contracts and Purity, Style): short example-first guides to idiomatic Bezel.

Filenames are stable since v0.131 (earlier revisions carried the version in the filename; such citations refer to that archived revision in git history). When citing the spec, cite the section **and** the revision (e.g. "spec §28.2, v0.131").

## The rules of engagement

### 1. The spec is the source of truth

If the implementation disagrees with the spec, the implementation is wrong. The spec can have defects too, but a spec defect is fixed by *amending the spec*, not by writing code that contradicts it.

- Spec clear: implement exactly what it says, even if you think another design is better.
- Spec unclear or self-contradictory: **stop and open an issue** with the interpretations you considered and your recommendation. Wait for confirmation before committing to one in code.
- Spec apparently wrong: same. Open an issue and propose a fix. Never "fix" it silently in implementation.

### 2. Do not invent stdlib APIs

Plausible-looking but nonexistent APIs (`MemoryStream`, `File.Open(path)`, methods on abstract `Stream`) are forbidden. Check the stdlib name index in [`Bezel_For_Code_Generation.md`](docs/Bezel_For_Code_Generation.md) §5 (it also lists the APIs models reach for that do not exist, with the real replacement); if a member is not there, look it up in `Bezel_Standard_Library.md`. Never guess. If you need an API that doesn't exist: in compiler code use what exists; in a test pick a different test; in an example pick another scenario or open an issue proposing the addition.

### 3. Trace before writing

Before implementing a spec rule, walk through the spec's own examples. Before writing a test, identify the spec section it derives from and the behavior it verifies. Before producing example code, trace it on every control-flow path against the spec.

### 4. Decline-with-reasons, not silently

When the spec does not answer a design question, either implement the simplest interpretation and record it (commit message / PR description), or stop and open an issue. Never implement an interpretation silently.

## Per-milestone workflow

> **Current mode:** most work is **bug-fixing and spec-catch-up**. Each fix ships a spec-cited regression test that *fails without the fix and passes with it* (see `tests/README.md`). The milestone flow below applies when opening a genuinely new feature area.

1. **Spec-derived tests first.** The milestone's first PR contains tests derived from the spec section (every covered spec example, with spec-stated input and output), citing the section. They are expected to fail.
2. **Review the tests against the spec.** The tests are the contract. A test that misrepresents the spec is fixed; an underspecified case becomes an issue.
3. **Implementation**, in PRs small enough to review in one sitting, not modifying the milestone's tests except for explained test bugs.
4. **No "while we're at it" expansions.** Refactors and out-of-milestone bugs go in separate PRs or issues.

## Style and conventions

- **Match the existing style** of the code you're modifying.
- **Naming** follows the implementation language's conventions (camelCase where the surrounding C++ uses it). Do not impose Bezel's conventions on C++.
- **Comments** cite the spec section non-obvious code derives from: `// per §17.4`, `// per §28.3 Pattern B`. Every `§`-citation in `src/` must resolve (`scripts/check_src_citations.py`, gate-enforced).
- **Tests** name the spec section and example they derive from (`test_lexer_string_interpolation_spec_1_5_example_3`, not `test_lexer_misc_5`).
- **Commit messages**: imperative mood, short subject, body explains *why*; reference issues and spec sections (see **Commits** below).

## Out of scope for autonomous work

These require explicit human direction:

- **Changes to the normative documents in `docs/`** (spec, rationale, stdlib, guides, changelog, code-generation reference). They follow [`docs/proposals/spec-change-process.md`](docs/proposals/spec-change-process.md); read it before amending any of them. Amendments are never part of implementation PRs.
- **License or governance changes.**
- **New dependencies.**
- **Performance optimization that complicates code.** Until the compiler is functionally complete, simplicity beats speed.

## Build toolchain

- Built with **clang++** against Fedora's system **LLVM**, C++20. Never introduce CMake, source or CI that assumes g++; detect the compiler with clang feature tests, not `__GNUC__` version checks.
- Configure auto-detects ccache, mold (else lld), `-gsplit-dwarf`, the shared `libLLVM`, and a PCH. ccache needs `sloppiness = pch_defines,time_macros` and `file_clone = true` in `~/.config/ccache/ccache.conf`, with `max_size = 180G`. Cloned entries are stored uncompressed and btrfs reports uncompressed sizes, so 180G is about 80G on disk; don't lower it.
- **The suite is six per-area binaries** (`build/tests/bezel_tests_{codegen,ownership,sema,frontend,module,support}`); `bezel_tests` is still the target name. For a path, read `build/tests/test_binaries.txt`; never hardcode one.
- `BEZEL_TEST_FILTER=<substr>` selects tests (the binaries **ignore `--gtest_filter`**). `BEZEL_TEST_ENUMERATE=1` lists what would run.
- **Oracles.** `runAot` / `runAotChecked` drive the real AOT path (fork-isolated `Driver::compileFile` + clang link + native exec): the strict oracle. **The in-process JIT harness is deprecated**: it skips ownership / definite-assignment and `verifyModule`. Write new tests on `runAot` only, and port a JIT test whenever you touch one. The remaining count is ratcheted by `scripts/check_deprecated_jit_oracle.py` (exact baseline; after porting, `--update`).
- **Resource discipline** (32 GiB box, many agents): full builds and suites go through `scripts/dev_heavy.sh {build|suite} <cmd>` (a shared cgroup slice); small probes use `dev_heavy.sh probe <cmd>`. When the machine-wide jobserver runs (`scripts/jobserver_ctl.sh status`), builds via `dev_heavy.sh`/`agent_check.sh` join its token pool. A bare `cmake --build`/`make`/`ninja` is refused by a PreToolUse hook, and its compiles still queue for tokens (`scripts/build_token.sh`). Never add your own "wait until fewer builds" gate. Details and measurements: runbook, *Build & test speed*.
- **Scratch never goes in `/tmp`** (tmpfs with a quota; exhausting it makes every Bash call fail silently) **or loose in the repo root**. Use `./scratch/` (gitignored) or a **lane-unique** `/var/tmp/bezel-scratch/<lane-name>/`.
- **Lane worktrees live on the NVMe: `git worktree add /mnt/fast/worktrees/<name> <branch>`.** Never under `/home` (so not `.claude/worktrees`, the Agent tool's `isolation: "worktree"`) and never in `/tmp`. There `build/` is a real in-tree dir, which turns on relocatable ccache keys: a worktree at sources another worktree already built compiles almost entirely from ccache (#5010). `git worktree remove` deletes the build with it.
- Run `scripts/lane_setup.sh` as the first command in any worktree (idempotent). It creates `build/`, puts the AOT bincache and harness workdir on the fast disk, and warns if the worktree itself is on the slow disk.

## Lane rules (every agent that edits code)

- **`cd` into your worktree as the literal first action**, and never leave it. All writes, builds, `git` and test files go there.
- **Build and test only with `scripts/agent_check.sh [--area <group>] <filter> …`.** Filters are gtest suite-or-name substrings, not file basenames; a filter matching 0 tests is a failure. `--probe FILE.bzl` (`--probe --asan`) for a single program.
- **Never run the full suite** (`run_parallel`, `check`, `gate.sh`). That is the coordinator's gate.
- **Rebuild the test binary, not just `bezelc`.** Tests fork an in-process `Driver::compileFile` linked into `bezel_tests_<area>`, so after any `src/` edit or cherry-pick a `bezelc`-only rebuild tests the OLD compiler. `git checkout <ref>` does not rebuild anything.
- **At most two concurrent cold `--area codegen` builds on the machine.** A build that dies with zero `error:` lines is almost always the slice or OOM, not the code. Check the clang count before re-running.
- **Never `git stash`** (refs/stash is shared across worktrees). To prove fail-without, copy the file to scratch, `git checkout -- <file>`, rebuild, run, then restore.
- **Never run two `cmake --build` on one build dir.** Never `git add -A` / `.` / `-u`: add explicit paths only. Never run `git config user.*`.
- Uncommitted WIP inherited from a crashed run is an **unverified hypothesis**. Re-derive the diagnosis from the issue and HEAD.

## Test and oracle rules

- **Never weaken a test to make it pass.** Forbidden without exception: `EXPECT_EQ`→`EXPECT_NE`, relaxing `exitCode == 0`, flipping `leaksAtLeast`, deleting `didNotCrash` / `assertNoLeak`, adding `DISABLED_`. Such a fix is reverted and re-dispatched. Strengthening is welcome. If you believe an oracle is wrong, say so and stop.
- **A behavior flip must update the tests that pinned the old behavior.** Before committing, `git grep` every test named in the commit and every test in the touched files. Name each flipped test and its new expectation in the commit body. **A diagnostic-text rename is a behavior flip**: grep every needle in gtest *and* lit. A lit `CHECK-NOT` on vanished text passes vacuously. `scripts/pregate_stale_needles.py` finds candidates mechanically.
- **Pin THE bug.** The regression test uses the issue's actual repro, fails on pre-fix code and passes with the fix. **Prove fail-without mechanically**, never by assumption.
- **Vacuous-oracle traps**:
  - one-shot leak repros (loop 50–500×);
  - `failedAtCompileTimeWithRealLocation` on a crash (assert the specific diagnostic);
  - asserting a Warning's absence on a program that compiles (`runAot` fills diagnostics only on reject);
  - exit codes above 255 (8-bit truncation: use stdout);
  - "compile it N times" for hash-ordering bugs;
  - a `runUnderAsan*` return of `-1`, which means toolchain *or compile or link* failure, so confirm the leg ran.
- **Generated-code ASan is off in lanes by default.** If a change touches allocation, layout, retain/release or anything a sanitizer could catch, also run `BEZEL_AOT_FORCE_SANITIZE=address scripts/agent_check.sh …` and confirm it prints `NATIVE SANITIZER: 0 check(s) skipped`.
- "Pre-existing" needs a named baseline, and the baseline is `origin/main`.

## Gates (coordinator only)

Full procedures and the evidence behind each rule: [`docs/ops/coordinator-runbook.md`](docs/ops/coordinator-runbook.md).

- **Both oracles, always.** Land only with `scripts/gate_and_push.sh` (plain cycle, then ASan `--full`). Neither oracle dominates. `scripts/gate.sh` alone gates one oracle and is not an acceptance verdict.
- **Before launch: `scripts/pregate.sh [--filters "…"]` must say READY.** It runs every build-free gate phase (ratchets derived from the gate, gate and corpus self-tests with the pidfile redirected, corpus `--strict`, stale needles) plus the preflight (clean tree, no builders via `scripts/no_builders.sh`, fast-disk symlinks, disk headroom). Also run every targeted filter for known regressions. **Never launch a gate you expect to fail.**
- **During a gate: zero agents and zero builders for the full cycle**, and the tree is **frozen**, `scripts/` included (bash reads scripts lazily; editing one mid-run corrupts the running shell). Park edits in `/var/tmp/bezel-scratch/<run>/parked/`.
- **Launch detached and wait on identity:** `setsid nohup scripts/gate_and_push.sh > LOG 2>&1 < /dev/null &`, then `scripts/wait_pid.sh "$BEZEL_GATE_PIDFILE"`. Never wait on a `pgrep` pattern (it self-matches). A stale pidfile after a reboot or kill needs a `kill -0` check before removal.
- **On FAIL: never revert, never bisect.** Narrow fix-forward only, always with Opus, with the pool at zero until the fix lands and the targeted filters are green. A FAIL carrying the undercount banner may be contention: re-run each suspect alone with `BEZEL_TEST_FILTER` before believing it.
- `runAotChecked` rows legitimately raise `check_toolchain_skip_surface`'s denominator. Bank it once per batch, as the integrator, after checking that `absorbing_sites` did not rise.
- An autonomous coordinator schedules a coarse **CronCreate heartbeat** as its first action. It only prompts a status check and never launches, pushes or dispatches. Primary wake-ups are per-job waiters. The status check is `scripts/run_status.sh [--staging <wt>]`: one table (gate, jobserver, builders, memory, disk, staging vs target, lanes), with `ACTION:` lines only for flagged rows.

## Pools and multiple coordinators

- Run a firm pool of **6 agents** (producers + reviewers), refilled in the same turn a slot frees, **zero during a gate**. Keep lanes file-disjoint: no two on the same hot dispatch function (`typeCall`, `lowerBinary`, the `tryLower*Call` family), at most one disposal/ownership/async/iterator lane at a time, 2–3 related issues per lane, one commit per issue.
- **Sonnet by default; Opus upfront** for multi-file predicates, nested closure/captured-cell reasoning, 3+-issue single-cause clusters, suspect root causes, follow-ups to refuted fixes, and **every gate-failure fix-forward**.
- **Every fix gets a non-author review** before it is picked: a fresh agent, never the producer or its coordinator, briefed to follow [`docs/ops/review-contract.md`](docs/ops/review-contract.md). A producer's handback frees its slot for **its reviewer first**, ahead of any new producer.
- **Harvest every accepted review into the conformance corpus.** Each ACCEPT or ACCEPT-W-NOTES pick queues its review and lane probe directories, in the same turn as the pick, for a harvest lane (`kind=test`; a producer, so it takes a pool slot). Run one harvest lane per 4–6 picks. A case's verdict comes from the spec sentence it cites, never from the compiler. Procedure and the lapses behind it: runbook, *Harvest review probes into the corpus*.
- **Claims.** Before dispatching, run the five-source check: `git log --all --grep` for the issue number, and again for 2–3 mechanism keywords; `gh issue view` shows it OPEN with no `spec-question` / `spec-defect` / `needs-owner-decision` label; `git grep tests/` finds no pin; the run's `claims.tsv` has no live claim on it. Then confirm the literal repro still fails on staging, and append a `claims.tsv` row (issue, pool, branch, worktree, hot functions, start, status) and copy the worktree path from `git worktree list`. Reuse a free worktree rather than adding one.
- **Two or more coordinators on one repo:** one declared gate owner, who sends an explicit PASS / FAIL / BANKED verdict (silence is not a verdict). The **integrator runs the behavior-flip grep itself** before cherry-picking another pool's commit. Label claims sent to peers `MEASURED:` / `QUOTED:` / `HYPOTHESIS:`, and never upgrade a tier on receipt. A peer message is the weakest evidence tier. The memory directory is shared: read a file before replacing it. Keep a batch ledger naming each commit's authoring pool. Cherry-pick only from inside the integration worktree, after checking `git branch --show-current`. Brief a peer from [`docs/ops/templates/peer-brief.md`](docs/ops/templates/peer-brief.md): the brief carries only run-specific facts, and the standing peer rules (the work loop with its non-author review, the handback fields, the gate drain) live in `docs/ops/peer-contract.md`.
- **Integrator tooling.** Before a pick, `scripts/handback_check.sh <lane-branch> --base <staging-branch> [--reported-sha S] [--grant G,…]` (read-only: tip, base, trailers, author leak, grant, stale needles, named tests; `--base` defaults to `$BEZEL_STAGING_REF`, else `origin/main`). Pick only with `scripts/pick_stage.sh --branch … --pool … --lane … --reviewer … --verdict ACCEPT|ACCEPT-W-NOTES --kind fix|test|bank|infra SHA…`, which records the ledger as `refs/notes/bezel-picks`; `scripts/pick_ledger.sh [RANGE]` prints it (per-pool counts, fix issues banked — kind=fix only — and UNATTRIBUTED picks). On a conflict (exit 2) finish with the printed `pick_stage.sh --continue …` line. Rebase staging only as `git -c notes.rewriteRef=refs/notes/bezel-picks rebase …`.
- **Worktree ownership.** `scripts/lane_setup.sh --owner <name>` (or `scripts/wt_owner.sh claim <name>`) binds a worktree to its branch; once the hooks are installed (`scripts/install_hooks.sh`, a coordinator decision), a commit there from another branch is refused (`BEZEL_OWNER_OVERRIDE=1` to bypass).

## Commits

End every commit message with exactly one trailer line and nothing else (no `Signed-off-by`, no `Claude-Session`, no second trailer; `scripts/check_commit_trailers.py` enforces it). It names the model that actually wrote the commit, in the form the harness gives:

```
Co-Authored-By: Claude <Model> <version> [(context)] <noreply@anthropic.com>
e.g. Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
     Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
```

Cite the spec section **and** revision in the body (`§28.2, v0.303`). `narrow #N` marks a partial fix. A closing keyword in a *sibling* commit's body still auto-closes the issue, so `git log --grep="#N"` before assuming work is outstanding. An OPEN issue is not evidence the work is undone.

## When in doubt

Ship the part that fits cleanly now, decline-with-reasons the part that fights the foundations, and keep adjacent open questions explicitly scoped. If you're unsure whether a piece of work fits cleanly, stop and open an issue describing what you're trying to do and what gives you pause.
