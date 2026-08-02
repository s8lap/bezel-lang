# CLAUDE.md — Operating instructions for Claude Code sessions in this repository

This file is the first thing every Claude Code session should read. It encodes the conventions and disciplines this project operates under. Following these rules is more important than moving quickly; a fast contribution that drifts from the spec is worse than a slow one that doesn't.

## What this repository is

Bezel is a compiled language for services and tools, with ownership-based memory management and no garbage collector. Compile-time ownership tracking ensures every heap object is disposed exactly once; borrowing lets references be passed without transferring ownership, with lifetimes checked statically.

The design is mostly settled — a normative spec (with rationale, stdlib, and guides alongside; each document's header carries its current revision), multiple verification passes, and dozens of recorded design decisions. A working compiler now exists (`build/bezelc`): an AOT front-end plus LLVM backend that implements a large fraction of the spec, exercised by an extensive automated test suite (gtest behavior tests + a lit/FileCheck diagnostic track). There are no external users yet; the current work is closing the gap to the spec — bug-fixing, soundness hardening, and catching the implementation up to newer spec revisions — with each fix shipping a spec-cited regression test.

## The documents (read these before doing anything)

Ten documents in `docs/`. Their roles are distinct; consult the right one for the question at hand.

- **`Bezel_Language_Spec.md`** — the normative reference. Every syntactic and semantic rule. If you need to know what the language *is*, this is the answer. Appendix A is the complete grammar; Appendix C lists compiler-enforced invariants.
- **`Bezel_Standard_Library.md`** — every type, method, and constant the stdlib provides. **Do not invent stdlib APIs.** If you need a type or method that isn't in this document, either (a) it doesn't exist and you need to use the real one, or (b) the stdlib genuinely lacks it and that's worth flagging as an issue.
- **`Bezel_Design_Rationale.md`** — explains *why* each decision was made and what alternatives were considered and declined. When the spec says "X is forbidden," the rationale typically says why and what was tried instead. Read this when you're tempted to wonder "why doesn't Bezel just do Y?" — the answer is usually there.
- **`Bezel_Changelog.md`** — the per-revision narrative history: one entry per revision, oldest first, normative for migration. Pinned history — entries are written at their revision and never retroactively edited; never the place to look up a current rule.
- **`Bezel_Guide_01_Memory_and_Ownership.md`**, **`Bezel_Guide_02_Errors_and_Nullability.md`**, **`Bezel_Guide_03_Modeling_Data.md`**, **`Bezel_Guide_04_Concurrency.md`**, **`Bezel_Guide_05_Collections_and_Iteration.md`**, and **`Bezel_Guide_06_Contracts_and_Purity.md`** — short example-first guides showing idiomatic Bezel. Useful for understanding what *good* Bezel code looks like, and for spec-derived test material.

The filenames are stable across revisions (since v0.131; earlier revisions carried the version in the filename, e.g. `Bezel_Language_Spec_0_130.md` — such citations refer to that archived revision in git history). The current revision number lives in each document's header and in the changelog (`Bezel_Changelog.md`). When citing the spec in a PR, test, or issue, cite the section *and* the revision (e.g. "spec §28.2, v0.131") so the reference stays pinned as revisions land.

## The rules of engagement

### 1. The spec is the source of truth

If the implementation disagrees with the spec, the implementation is wrong. The reverse is *also* possible — the spec can have defects — but a spec defect is fixed by *amending the spec*, not by writing code that contradicts it.

When the spec is clear: implement exactly what it says, even if you think a different design would be better.

When the spec is unclear or appears to contradict itself: **stop and open an issue.** Describe the ambiguity, the interpretations you considered, and which you'd recommend. Wait for confirmation before writing implementation code that would commit to one interpretation.

When the spec appears to have an error: same — open an issue, describe the apparent contradiction, propose a fix. Do not "fix" it silently in implementation.

### 2. Do not invent stdlib APIs

A recurring failure mode in earlier drafting was producing code that looks plausible but uses types or methods the stdlib does not define (`MemoryStream`, `File.Open(path)`, methods on abstract `Stream`). This is forbidden. Every type and method referenced in compiler code, tests, or examples must be verifiable in `Bezel_Standard_Library.md`.

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

- **Changes to the documents in `docs/`** (spec, rationale, stdlib, guides). These are amended through their own dedicated workflow (per-revision changelog, full mechanical validation), not as part of implementation PRs.
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
is ~2 min and a one-file edit→relink is ~1–2s.

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
link + native exec) — the strict oracle. The sharded `--full` run (`scripts/gate.sh`)
is **authoritative** (#1861): every test is process-isolated (a fork'd compile +
a separate native exec), so the old serial-order heap-corruption class is
structurally impossible and the always-on serial pass was retired. The sharded
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
suite at once. (The former in-process JIT harness — a *lenient* oracle that
skipped the ownership/definite-assignment passes and `verifyModule` — has been
retired.)

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
match, so a filter spanning two areas runs in both. Add `--area <group>` (e.g.
`--area sema`) to restrict the *build* as well: after a `src/` change that is
the difference between relinking one ~50 MB binary and all ~600 MB of them,
which matters on this I/O-contended box. For a single `.bzl`:
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
agent orphaned. Per-worktree build dirs live *inside* the worktree, so reaping
the worktree reclaims them too.

**Delicate merges (object layout, disposal machinery, runtime).** Gate these
with `scripts/gate_and_push.sh --full` (or `gate.sh --full`): `--full` runs the
whole suite with generated-code AddressSanitizer forced on
(`BEZEL_AOT_FORCE_SANITIZE=address`) — the former standalone whole-suite ASan
sweep is folded into that one sharded pass (#1861). No separate serial pass or
`asan_sweep.sh` is needed: AOT process-isolation makes serial-order corruption
structurally impossible, so the sharded `--full` run is the whole oracle.

## When in doubt

The project's overall discipline is: ship the part that fits cleanly now, decline-with-reasons the part that fights the foundations, keep adjacent open questions explicitly scoped. If you're not sure whether a given piece of work fits cleanly: stop, open an issue, describe what you're trying to do and what's giving you pause. The project has reached its current state by catching defects early and recording decisions with reasoning. That discipline continues to apply.
