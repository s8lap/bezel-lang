# CLAUDE.md — Operating instructions for Claude Code sessions in this repository

This file is the first thing every Claude Code session should read. It encodes the conventions and disciplines this project operates under. Following these rules is more important than moving quickly; a fast contribution that drifts from the spec is worse than a slow one that doesn't.

## What this repository is

Bezel is a compiled language for services and tools, with ownership-based memory management and no garbage collector. Compile-time ownership tracking ensures every heap object is disposed exactly once; borrowing lets references be passed without transferring ownership, with lifetimes checked statically.

The design is mostly settled — a normative spec (with rationale, stdlib, and guides alongside; each document's header carries its current revision), multiple verification passes, and dozens of recorded design decisions. A working compiler now exists (`build/bezelc`): an AOT front-end plus LLVM backend that implements a large fraction of the spec, exercised by an extensive automated test suite (gtest behavior tests + a lit/FileCheck diagnostic track). There are no external users yet; the current work is closing the gap to the spec — bug-fixing, soundness hardening, and catching the implementation up to newer spec revisions — with each fix shipping a spec-cited regression test.

## The documents (read these before doing anything)

Twelve documents in `docs/`. Their roles are distinct; consult the right one for the question at hand.

**If you are about to *write* Bezel code, read [`Bezel_For_Code_Generation.md`](docs/Bezel_For_Code_Generation.md) first.** It is a dense writing reference — syntax surface, the ownership rules as obligations, a stdlib name index with exact signatures, the idioms, and a *looks-right-is-rejected* section — at roughly a fifteenth the size of the spec plus stdlib, so it is cheap enough to keep in context alongside the code you are producing. It is **not normative**: where it and the spec disagree, the spec wins and the disagreement is a defect to report.

- **`Bezel_Language_Spec.md`** — the normative reference. Every syntactic and semantic rule. If you need to know what the language *is*, this is the answer. Appendix A is the complete grammar; Appendix C lists compiler-enforced invariants.
- **`Bezel_Standard_Library.md`** — every type, method, and constant the stdlib provides. **Do not invent stdlib APIs.** If you need a type or method that isn't in this document, either (a) it doesn't exist and you need to use the real one, or (b) the stdlib genuinely lacks it and that's worth flagging as an issue.
- **`Bezel_Design_Rationale.md`** — explains *why* each decision was made and what alternatives were considered and declined. When the spec says "X is forbidden," the rationale typically says why and what was tried instead. Read this when you're tempted to wonder "why doesn't Bezel just do Y?" — the answer is usually there.
- **`Bezel_Changelog.md`** — the per-revision narrative history: one entry per revision, oldest first, normative for migration. Pinned history — entries are written at their revision and never retroactively edited; never the place to look up a current rule.
- **`Bezel_For_Code_Generation.md`** — the writing reference described above. Derived, pinned to a revision, and re-validated like the guides; `scripts/check_docs.sh`'s PIN check **fails** if it falls more than three revisions behind, because a stale line there is wrong code rather than stale teaching.
- **`Bezel_Guide_01_Memory_and_Ownership.md`**, **`Bezel_Guide_02_Errors_and_Nullability.md`**, **`Bezel_Guide_03_Modeling_Data.md`**, **`Bezel_Guide_04_Concurrency.md`**, **`Bezel_Guide_05_Collections_and_Iteration.md`**, **`Bezel_Guide_06_Contracts_and_Purity.md`**, and **`Bezel_Guide_07_Style.md`** — short example-first guides showing idiomatic Bezel. Useful for understanding what *good* Bezel code looks like, and for spec-derived test material.

The filenames are stable across revisions (since v0.131; earlier revisions carried the version in the filename, e.g. `Bezel_Language_Spec_0_130.md` — such citations refer to that archived revision in git history). The current revision number lives in each document's header and in the changelog (`Bezel_Changelog.md`). When citing the spec in a PR, test, or issue, cite the section *and* the revision (e.g. "spec §28.2, v0.131") so the reference stays pinned as revisions land.

## The rules of engagement

### 1. The spec is the source of truth

If the implementation disagrees with the spec, the implementation is wrong. The reverse is *also* possible — the spec can have defects — but a spec defect is fixed by *amending the spec*, not by writing code that contradicts it.

When the spec is clear: implement exactly what it says, even if you think a different design would be better.

When the spec is unclear or appears to contradict itself: **stop and open an issue.** Describe the ambiguity, the interpretations you considered, and which you'd recommend. Wait for confirmation before writing implementation code that would commit to one interpretation.

When the spec appears to have an error: same — open an issue, describe the apparent contradiction, propose a fix. Do not "fix" it silently in implementation.

### 2. Do not invent stdlib APIs

A recurring failure mode in earlier drafting was producing code that looks plausible but uses types or methods the stdlib does not define (`MemoryStream`, `File.Open(path)`, methods on abstract `Stream`). This is forbidden. **The fastest way to comply is the stdlib name index in [`Bezel_For_Code_Generation.md`](docs/Bezel_For_Code_Generation.md) §5** — every type and member with its exact signature, plus an explicit list of the APIs a model reaches for that do not exist here (`MemoryStream`, `File.Open`, `List<T>`, `ToList`, `HashSet`, `Tuple`, `string.Format`, `Task.Run`, and more) paired with the real replacement. If a member is not in that index, look it up in `Bezel_Standard_Library.md`; never guess. Every type and method referenced in compiler code, tests, or examples must be verifiable in `Bezel_Standard_Library.md`.

If you need an API that doesn't exist, the right move depends on context: in compiler code, use what does exist; in a test, choose a different test that exercises real APIs; in an example, either pick a different scenario or open an issue proposing the stdlib addition.

### 3. Trace before writing

Before implementing a spec rule, walk through the spec's own examples and confirm you understand what the rule says on each. Before writing a test, identify the spec section it derives from and the specific behavior it verifies. Before producing example code, trace it on every control-flow path against the spec.

This is slower than typing. It also catches the entire class of defects that consumed several revisions of earlier work — implementations of plausible-looking idioms that the spec actually rejects.

### 4. Decline-with-reasons, not silently

When a design question arises that the spec does not answer, **two options are good** and one is bad:

- Good: implement the simplest interpretation, document it in a PR comment, flag in the PR description that this is a choice the spec did not specify.
- Good: stop and open an issue, propose the interpretation, wait for review.
- Bad: implement an interpretation silently, with no record of the choice.

The project's discipline is that decisions get recorded with their reasoning. A choice made silently in implementation is a choice that cannot later be revisited because no one remembers it was made.

## Per-milestone workflow

> **Current mode:** with the compiler substantially built, most day-to-day work is now **bug-fixing and spec-catch-up**, not greenfield milestones — each fix ships a spec-cited regression test that *fails without the fix and passes with it* (see `tests/README.md`). The greenfield milestone flow below still applies when opening a genuinely new feature area.

Implementation milestones (e.g., "lexer for §1," "parser for §17 expression grammar," "type checker for §11 classes") follow the same pattern. Each milestone's first PR is *not* implementation code.

1. **Spec-derived tests first.** The first PR for a milestone contains tests that derive directly from the spec section: every example in the spec that the milestone covers should be a test case, with the spec-stated input and the spec-stated output (parsed AST, compile error, runtime value). The PR description should cite the spec section and list which examples are covered. The tests should fail (because no implementation exists yet); CI failure is expected at this stage.

2. **Review of the tests against the spec.** The tests are the contract. Before any implementation lands, the tests should be reviewed *as a translation of the spec into executable form.* If a test misrepresents the spec, fix the test; if a test reveals an underspecified case, open an issue.

3. **Implementation.** Subsequent PRs implement the feature until the tests pass. Each PR should be small enough to review in one sitting. PRs should not modify the milestone's tests except to fix legitimate test bugs (with explanation).

4. **No "while we're at it" expansions.** A milestone's PRs only touch what the milestone is about. Refactoring opportunities go in separate PRs; bug fixes outside the milestone go in separate issues. This keeps reviews focused and bisects clean.

## Style and conventions

- **Match the existing style** of any code you're modifying. Consistency within a file matters more than personal preference.
- **Naming**: follow the implementation language's standard conventions (snake_case for Rust, camelCase for C++ where the surrounding code uses it, etc.). Do not impose Bezel's naming conventions on the implementation language.
- **Comments**: cite the spec section any non-obvious code derives from, in the form `// per §17.4` or `// per §28.3 Pattern B`. This is how a reader (human or Claude) finds the authority for what the code does.
- **Tests**: each test should name the spec section and example it derives from in its comment or test name. `test_lexer_string_interpolation_spec_1_5_example_3` is good; `test_lexer_misc_5` is not.
- **Commit messages**: imperative mood, short subject, body explains *why* not *what*. Reference issues and spec sections.

## Things that are out of scope for autonomous work

These require explicit human direction; do not undertake them on your own initiative:

- **Changes to the documents in `docs/`** (spec, rationale, stdlib, guides). These are amended through their own dedicated workflow, written down in [`docs/proposals/spec-change-process.md`](docs/proposals/spec-change-process.md) — **read it before amending any normative document.** It defines the change classes (editorial / clarification / ruling / design-change / Ratchet) and the gate each one earns, the artifacts each revision must leave behind, and a one-screen checklist (§9) covering the version-header bump, the kernel and body-locality classifications, the `DECLINED.md` obligation, and the code-generation-reference obligation. Amendments are not part of implementation PRs.
- **Changes to the project's license or governance.**
- **Adoption of new dependencies.** Dependencies are debt; adding one is a project-level decision.
- **Performance optimization that complicates code.** Until the compiler is functionally complete, simplicity beats speed. A correct slow compiler can be made fast; a fast incorrect one cannot be made correct.

## Build toolchain

The Bezel compiler is built with **clang++** linking against **LLVM** as
shipped by Fedora's system packages. The C++ standard is C++20.

This choice is for toolchain consistency: the compiler calls LLVM's C++
API extensively, and LLVM is built and tested primarily with clang
upstream. Using clang to build the compiler avoids cross-toolchain
quirks that surface when g++ compiles LLVM headers at specific version
combinations.

Do not introduce CMake configuration, source files, or CI steps that
assume g++ or that work only with g++. If a piece of code needs to
detect the compiler, use clang-specific feature tests, not version
checks against `__GNUC__`. The build is expected to be configurable
with `cmake -DCMAKE_CXX_COMPILER=clang++ ...` or to set the compiler
explicitly in the root CMakeLists.txt.

### Build & test speed (the build is dominated by the test suite)

(This file states sizes qualitatively on purpose — exact counts like the test
total or TU count drift every run, so they are not hardcoded here. For a live
number run the suite or `find tests -name '*.cpp' | wc -l`; for a spec revision
read the relevant document's header.)

The configure auto-detects accelerators (CMakeLists.txt): **ccache**, the
**mold** linker (falls back to lld), **`-gsplit-dwarf`** (debug info out of the
link), the **shared `libLLVM`** (linked as one `.so`, not static components —
keeps binaries small and links near-instant; the in-process JIT is unaffected),
and a **PCH** of the heavy LLVM/std headers for the test target. Build with
plenty of parallelism — the machine is many-core and each TU peaks well under
the build cage's memory budget: pass `-j16` (or use `cmake --build --preset
default`, which sets jobs); the bundled scripts default to `-j16`. A clean build
is ~2 min and a one-file edit→relink is ~1–2s. ⚠ Those two figures were
measured at ~770 TUs; the tree is now **~1,760** (`python3 -c "import json;
print(len(json.load(open('build/compile_commands.json'))))"` — 1,756 on
2026-08-13, and this number drifts every run, so re-measure rather than trust it),
so treat them as lower bounds. **Do not re-measure by hand** — every gate has
appended one JSON record per run to `/var/tmp/bezel-scratch/gate-timings/`
since 2026-07-14, with per-phase timings. Read it with `scripts/gate_trend.sh`
(which also fails a push when a gate's telemetry contradicts its verdict, #3010).

**The suite is several per-area binaries, not one.** `tests/CMakeLists.txt`
splits the test sources along the existing `tests/<area>/` directories into
`build/tests/bezel_tests_{codegen,ownership,sema,frontend,module,support}`, so a
test-only edit relinks only its own area and a lane can build+run just its area
(`scripts/agent_check.sh --area sema <filter>`). **`bezel_tests` is still the
target name** (`cmake --build build --target bezel_tests` builds them all), so
build commands are unchanged; anything that needs a *path* reads the
CMake-generated manifest `build/tests/test_binaries.txt`
(`<group> <TAB> <path> <TAB> <#sources>`) — never hardcode a binary path, and
never enumerate the areas by hand. Two configure-time FATAL_ERROR invariants
guarantee the split is a true partition (every discovered `tests/**/*.cpp` is in
exactly one binary; a `tests/<newdir>/` no group claims is a hard error), so the
union of the binaries is exactly the whole suite.

**Run the test suite in parallel**, not as a raw binary — a single binary is
serial gtest (several minutes). Use `tests/run_parallel.sh 16` (or `cmake --build
build --target check`): it slices EVERY test binary across cores (each shard a
separate process via `BEZEL_SHARD_*`) and feeds the slices through one queue
whose width is that first argument — so the argument is the *concurrency* cap
(the resource knob the ASan gate lowers to 8), not a per-binary shard count.
Accounting is per binary: each one's `ran` must equal its own registry total and
every one of its shards must have printed a completion summary, so a dropped or
crashed slice in any binary still fails loudly. Tests
drive the *real* `bezelc` AOT path (fork-isolated `Driver::compileFile` + clang
link + native exec) — the strict oracle. The sharded run (`scripts/gate.sh`)
is **authoritative over the retired serial pass** (#1861): every test is
process-isolated (a fork'd compile + a separate native exec), so the old
serial-order heap-corruption class is structurally impossible and the always-on
serial pass was retired. ⚠ That authority is **sharded-vs-serial, NOT
`--full`-vs-plain** — an earlier wording attached it to `--full` and so read as
"the ASan oracle is the authoritative one", which is precisely what #2633
refuted and what the ⚠ block below says. Neither oracle dominates; a batch needs
BOTH. The sharded
runner *can* over-report under fork/link contention — but that shows up as a
whole-suite timeout, not a real failure; to disambiguate, re-run the specific
suspect test(s) in isolation with `BEZEL_TEST_FILTER=<name>` (a single-process
run), NOT a full serial pass. (The binaries silently ignore `--gtest_filter`;
`BEZEL_TEST_FILTER` is the substring selector. `BEZEL_TEST_ENUMERATE=1` lists
what a process *would* run, one `suite.name` per line, without running it — that
is how tooling finds which area binary holds a filter, and how the partition is
proved.) See `tests/README.md` for the
harness (`runAot` / `runAotChecked` / lit+FileCheck) and how to write a test;
`BEZEL_AOT_LINK_TOKENS` caps concurrent compiles when several agents run the
suite at once. ⚠ **The in-process JIT harness is DEPRECATED, not retired.** An
earlier revision of this file said "has been retired" while stating 45 lines
above that "the in-process JIT is unaffected" — both cannot be true, and the
code settles it: **60 `runJit(` + 3 `runJitChecked(` live call sites remain**
(measured 2026-08-08, re-verified 2026-08-13;
`grep -rn 'runJit(' tests/ --include=*.cpp | grep -v '^tests/support/' | wc -l`
— the `--include`/`grep -v` matter: the bare `grep -rn 'runJit(' tests/` an earlier
revision gave here returns **62**, because it also counts the harness's own
definitions under `tests/support/`. A recipe that does not reproduce the number
beside it reads as a violated ratchet; it cost one false alarm already, so if you
find these disagreeing, suspect the recipe before raising the count.)
It is a *lenient* oracle —
it skips the ownership/definite-assignment passes and `verifyModule`, so a test
still on it proves less than its name suggests. **Write new tests on `runAot`
only**, and port a JIT test whenever you touch one. The count above is the
ratchet: it must only ever go down.

**That ratchet is now executed, not just written down** (#3439).
`scripts/check_deprecated_jit_oracle.py` measures all three numbers against
`scripts/deprecated_jit_oracle.baseline` and `gate_full_cycle.sh` runs it —
self-test first, then `--strict` — alongside the seam, vacuous-test and
enumerable-grid ratchets. After **porting** a test, bank it with `--update`
(which refuses to raise a count); `--list` shows every file still on the
lenient oracle. The baseline is **exact, not a ceiling**: a count *below* it
fails as stale headroom, because unbanked headroom is indistinguishable from a
rise somebody already spent. ⚠ The reason this phase exists is worth keeping:
the rule above, with its measured numbers, had been in this file for months and
the count still rose 60→61 on 2026-08-23 and survived five days and two full
gate cycles. **A prose ratchet is not a ratchet** — its three sibling checks
were scripts with self-tests and this one was a paragraph.

One machine-local, not-tracked step is required for ccache to cache the PCH'd
compiles: `~/.config/ccache/ccache.conf` must contain
`sloppiness = pch_defines,time_macros` (without it every PCH build is a cache
miss — slower, never incorrect).

**Concurrency discipline (32 GiB box, parallel dev agents).** Run every full
build and every full suite through `scripts/dev_heavy.sh {build|suite} <cmd>`:
it runs them in a shared resource-capped cgroup slice (`bezel-heavy.slice`,
~18 cores, 24G soft / 28G hard) that bounds TOTAL cpu+mem across all concurrent
heavy ops WITHOUT serializing them — N builds share the budget (each slower)
rather than oversubscribing or OOMing. (An earlier exclusive flock was removed
deliberately: it caused a producer-stall storm; the slice bounds resources
without a lock — do not reintroduce one.) Gate ops get high CPU/IO weight so
they stay fast; producer builds run at background weight on spare capacity; each
op is also memory-caged (12–14G). Small probe compiles/runs use
`dev_heavy.sh probe <cmd>` (own small cage, no slice accounting). Scratch
artifacts (probe binaries, big logs, scratch build trees) go in
`/var/tmp/bezel-scratch` — never `/tmp`, which is tmpfs (RAM): a scratch build
tree parked there permanently consumes gigabytes. Keep ccache `max_size` at
80G (a full 40G cache thrashes under multi-worktree churn).

**Producer/fix lanes: build+test your change with `scripts/agent_check.sh`.** It
is the single, opinionated entry-point that encodes the correct lane protocol so
you cannot get it wrong — it reconfigures (so a *new* test `.cpp` is picked up;
`cmake --build` alone can't see it), builds via `dev_heavy` (slice-capped,
foreground — no background waiter to stall on), removes the stale binary first,
and runs ONLY your targeted filter:
`scripts/agent_check.sh <filter> [filter2 …]` (substring on gtest suite-or-name,
NOT the file basename). You do **not** need to know which area binary holds your
test — it enumerates the binaries (~30 ms each) and runs exactly the ones that
match, so a filter spanning two areas runs in both. `--area <group>` (e.g.
`--area sema`) narrows the LINK to one ~50 MB binary instead of all ~600 MB.
**Since fix #3172 (2026-09-03) `--area` narrows the COMPILE as well as the
link.** The 12-header PCH is owned by a one-TU object library
(`bezel_tests_pch`, generated into the build dir) that all six groups
`REUSE_FROM`, so no group's compile depends on another group's target any more:
`cmake --build build --target bezel_tests_module -- -n` after touching a codegen
TU schedules **0** codegen compile actions (it was ~2,228 when codegen owned the
PCH, measured 2026-08-13). Test TUs are compiled with `-gline-tables-only` (the
compiler library keeps full `-g`), which roughly halved the six binaries
(codegen 366 MB → 212 MB). ⚠ An earlier revision of this paragraph documented
the opposite — that only `--area codegen` was narrow — and it was true until
#3172 landed; if `--area <group>` ever schedules codegen actions again, the
owner target has regressed. Budget one full test rebuild for the first gate
after any change to the test compile flags (the PCH and ccache keys both move). For a single `.bzl`:
`scripts/agent_check.sh --probe
FILE.bzl` (or `--probe --asan FILE.bzl` for memory/disposal fixes, with a hang
timeout); all probe output goes to the scratch dir, never the repo root. It
REFUSES an empty filter and `--gtest_filter` (the binary silently ignores that
flag and runs everything). **A lane must NEVER run the full suite / `run_parallel`
/ `check` — that is the integration GATE's job** (`scripts/gate.sh`, coordinator
only); a lane running the whole suite just burns the shared budget on ~100
redundant fork+link+exec tasks.

**Keep the repo root clean.** Ad-hoc `.bzl` probes, one-off `bezelc -o` outputs,
and investigation scratch do NOT belong loose in the repo root — put them in
`./scratch/` (gitignored) or `/var/tmp/bezel-scratch/`. Cleanup is automated, so
you should rarely sweep by hand: `scripts/gate_and_push.sh` runs
`scripts/gc_scratch.sh` after every successful push, which (a) reaps worktrees
whose commits are already in `origin/main` (branch refs are kept; WIP, locked,
and the current worktree are never touched), (b) drops stale
`/var/tmp/bezel-scratch/build-asan-*` trees, and (c) quarantines >48h-old root
scratch into `/var/tmp/bezel-scratch/root-quarantine/` (moved, not deleted;
self-purged after 14 days). Run `scripts/gc_scratch.sh` (dry-run) any time to
preview, `--apply` to act; `scripts/reap_orphans.sh` clears processes a dead
agent orphaned. ⚠ **Per-worktree build dirs do NOT necessarily live inside the
worktree.** `scripts/lane_setup.sh` symlinks a lane's `build/` out to
`/mnt/fast/lane-builds/<name>` (the NVMe), so `git worktree remove` deletes only
the symlink and **orphans the tree**. That is not theoretical: 68 GB accumulated
across 54 orphans in four days and had to be cleared by hand (#3005), while this
file asserted the invariant that says it cannot happen. `gc_scratch.sh` **now
does** reap them (an earlier revision of this line said it did not — verified
2026-08-13, it reaped three 3.6 GB trees), but it only reaps trees with no
matching worktree, so a wave of N live lanes still holds N trees at ~3.6 GB
each. Watch `df -h /mnt/fast` during a fan-out: five concurrent lanes took it to
**91%**, and a lane that runs out of disk mid-build fails in ways that look like
a compiler error. Re-run `lane_setup.sh` in a worktree whose `build/` symlink
dangles (`scripts/lane_setup.sh` self-repairs dangling build/ symlinks).

⚠ **AOT bincache + workdir must be on a fast disk** (the original `#3010`-class
defect — silently caused a 5.7s/test cold-cache wall time on 2026-08-22 Wave
4 attempt-1, exceeding the 1800s shard ceiling per-test). The AOT harness's
`cacheDir()` and `scratchBase()` defaults (`tests/support/aot_support.hpp:1014`
and `:153`) both resolve to `/var/tmp/bezel-scratch/{aot-bincache,aot-harness}`
— `sda3 btrfs` on the machine this was measured on (~5 durable creates/s,
~65ms fdatasync on a 1 MB file). The fast disk on that box was `/mnt/fast`
NVMe (~846/s, ~11ms fdatasync — a 6× per-op gap, 169× on sustained creates).
Other machines will have other "slow" and "fast" disks; the policy is
**structural, not FS-type-specific**.

**Setup is the operator's responsibility**, not something `gate.sh` probes for.
`gate.sh` deliberately does not measure fsync speed or check filesystem type:
thresholds are machine-dependent (a busy NVMe under contention is ~5ms,
indistinguishable from sda3 btrfs quiet — false positives), and an unfamiliar
slow backend (xfs-on-SATA, USB, network mount) would slip past an FS-name
check. The cold gate's *behavior* — per-test shard phases hitting the 1800s
ceiling with `runs/OK` counts in the 300s — is the load-bearing indicator.

**The fix is `scripts/lane_setup.sh`** (run as the first command in any lane
worktree; idempotent; refuses to clobber a real dir). It symlinks both
`/var/tmp/bezel-scratch/aot-bincache` and `/var/tmp/bezel-scratch/aot-harness`
to `/mnt/fast/{aot-bincache,aot-harness}` when `/mnt/fast` is a mount point.
Override the path with `BEZEL_AOT_FAST_ROOT=/your/fast/disk` if the fast disk
isn't named `/mnt/fast`. To opt out of symlinking entirely (e.g. you have a
single-disk box or want to use a tmpfs), set `BEZEL_AOT_FAST_ROOT=""` or set
`BEZEL_AOT_CACHE_DIR` / `BEZEL_AOT_SCRATCH_DIR` env vars to point at your
chosen locations — both the harness defaults and `lane_setup.sh` respect them.
dangles (it now self-repairs).

**Waiting for a gate (this is where coordinator time actually goes).** Two rules,
both learned expensively:

1. **Never wait on a process pattern.** `until ! pgrep -f '<pat>'` self-matches,
   because the waiter's own command line contains the pattern. Bracketing
   (`[p]at`) is NOT a general fix — it only defeats the self-match when the
   pattern appears solely as grep's argument. Two such loops cost ~3.2 h in the
   2026-08-12 run, one of them spinning for hours over a gate that had passed.
2. **Run the gate DETACHED, and wait on identity.** A gate cycle is ~50 min; a
   harness-tracked background job can be killed at its own timeout, which
   silently leaves the orphaned shards running and writing to the log — it looks
   exactly like a gate still in progress. Launch with
   `setsid nohup scripts/gate_and_push.sh > LOG 2>&1 < /dev/null &`, then wait
   with `scripts/wait_pid.sh "$BEZEL_GATE_PIDFILE"` (identity, via `kill -0` —
   it cannot match itself).

`gate_and_push.sh` publishes `$BEZEL_GATE_PIDFILE` (removed at exit) and
`$BEZEL_GATE_DONEFILE` (the exit status) — but only once it is **committed to
gating** (#3463): a `--help`, a refusal, or the "another gate is running" abort
publishes nothing, because those files belong to whichever gate is actually
running. Before that fix every invocation wrote them, so the #3415 self-test
phase deleted the live gate's pidfile and wrote `1` into its donefile 20s into a
cycle that went on to pass, and `wait_pid.sh` exited ~50 min early. Publication
therefore lands ~50ms in rather than immediately, so `wait_pid.sh` now waits a
bounded moment (`--pidfile-grace`, default 30s) for the pidfile to appear
instead of failing the launch race. ⚠ Prefer the PIDFILE: the donefile is
written from an EXIT trap, so an uncatchable kill produces no marker at all, and
a donefile-only waiter then hangs forever over a finished gate. That is not
hypothetical — the marker was **dead on arrival** from the day it was added
until #3158, because a second `trap … EXIT` later in the same script silently
replaced the handler (bash keeps exactly one). `scripts/gate/tests/test_gate_and_push_trap.py`
pins that a second one has not reappeared, and **since #3415 it is gate-enforced**:
`gate_full_cycle.sh` runs `python3 -m unittest discover -s scripts/gate/tests`
as the `gate-selftest` phase, beside the vacuous-assertion and imported-symbol
ratchets — build-free, ~33s, fail-closed. The same now holds for
`test_telemetry`, `test_canary`, `test_gate_serialization` and
`test_repo_identity`. ⚠ It is a **gate** phase, not a *build* gate: `cmake --build`
still does not run it, so run it by hand while iterating on gate infrastructure.
Two earlier revisions of this passage were wrong in opposite directions — one
claimed it "now fails the build" when nothing invoked the directory at all, the
other still said "MANUAL check" after it was wired; check `gate_full_cycle.sh`
for the phase before trusting either.

The phase ratchets **counts, not just the exit status**, because `unittest` exits
0 for "Ran 0 tests" and for "OK (skipped=590)" alike: it asserts a floor on tests
run and a cap on tests skipped, so a collection failure or a muted directory
fails the cycle instead of passing with a green tick. Two tests in
`test_integration_bash_python` drive the real `gate_and_push.sh` against the real
lockfile and **self-skip with a printed reason** when a gate is running or the
tree is dirty — they would otherwise `unlink` a lockfile a live gate holds
(letting a second gate start on a fresh inode), and the entry point refuses on a
dirty tree by design (exit 65). Inside a gate both hold, so **2 skips is the
expected steady state**. (An earlier revision attributed the lockfile hazard to
`test_gate_serialization`; that file isolates every lock into a
`TemporaryDirectory` and never touches the production path.)

**The tree is FROZEN while a gate runs.** Not just `src/` — a mid-cycle rebuild
makes a mixed-HEAD gate — but `scripts/` too: bash reads a script lazily by byte
offset, so editing one mid-run desynchronises the running shell, and restoring
the original bytes afterwards does NOT save the run. Park edits in
`/var/tmp/bezel-scratch/<run>/parked/` and re-apply after the gate reports.

**Gating a batch: BOTH oracles, always.** Land with `scripts/gate_and_push.sh`
— it runs `scripts/gate_full_cycle.sh` (plain, then `--full`), requires each to
be trustworthy-green, and only then pushes. `--full` forces generated-code
AddressSanitizer on every test program (`BEZEL_AOT_FORCE_SANITIZE=address`), so
the whole suite runs under ASan in that one sharded pass — the former standalone
`asan_sweep.sh` is folded in (#1861), and no serial pass is needed because AOT
process-isolation makes serial-order corruption structurally impossible.

⚠ **`--full` is a DIFFERENT oracle, not a stronger one (#2633) — neither
dominates, so one is never sufficient.** ASan replaces the allocator and changes
layout, so a wild read that FAULTS on the plain build can land in mapped memory
and pass under `--full`; conversely a silent overflow the plain build tolerates
is only caught by ASan. #2633's evidence is one binary, one filter: 3 failures
plain, 0 under ASan, where the bug was a SIGSEGV on the default build. Because
runs had been gating almost exclusively with `--full`, that whole defect class
was "structurally invisible to the gate, however many times it ran".

`scripts/gate_and_push.sh --full` still works and is now a **no-op** (the cycle
always runs ASan). `--quick` is REFUSED for a push. A deliberate one-oracle gate
is `--single-oracle MODE`, which says loudly that it is not a full acceptance.
Running `scripts/gate.sh` directly gates ONE oracle and now prints that its
`GATE PASS` is not an acceptance verdict — use it for diagnosis, not to land.

## When in doubt

The project's overall discipline is: ship the part that fits cleanly now, decline-with-reasons the part that fights the foundations, keep adjacent open questions explicitly scoped. If you're not sure whether a given piece of work fits cleanly: stop, open an issue, describe what you're trying to do and what's giving you pause. The project has reached its current state by catching defects early and recording decisions with reasoning. That discipline continues to apply.
