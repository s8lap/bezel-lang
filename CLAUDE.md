# CLAUDE.md — Operating instructions for Claude Code sessions in this repository

This file is the first thing every Claude Code session should read. It encodes the conventions and disciplines this project operates under. Following these rules is more important than moving quickly; a fast contribution that drifts from the spec is worse than a slow one that doesn't.

## What this repository is

Bezel is a systems language with explicit ownership and no garbage collector. Compile-time ownership tracking ensures every heap object is disposed exactly once; borrowing lets references be passed without transferring ownership, with lifetimes checked statically.

The language is paper-designed: there are no users yet, and the compiler does not yet exist. The design itself is mostly settled — 17,000 lines of normative spec, multiple verification passes, 70+ recorded design decisions — and the work this repository hosts is implementing that design.

## The documents (read these before doing anything)

Five documents in `docs/`. Their roles are distinct; consult the right one for the question at hand.

- **`Bezel_Language_Spec.md`** — the normative reference. Every syntactic and semantic rule. If you need to know what the language *is*, this is the answer. Appendix A is the complete grammar; Appendix C lists compiler-enforced invariants.
- **`Bezel_Standard_Library.md`** — every type, method, and constant the stdlib provides. **Do not invent stdlib APIs.** If you need a type or method that isn't in this document, either (a) it doesn't exist and you need to use the real one, or (b) the stdlib genuinely lacks it and that's worth flagging as an issue.
- **`Bezel_Design_Rationale.md`** — explains *why* each decision was made and what alternatives were considered and declined. When the spec says "X is forbidden," the rationale typically says why and what was tried instead. Read this when you're tempted to wonder "why doesn't Bezel just do Y?" — the answer is usually there.
- **`Bezel_Guide_01_Memory_and_Ownership.md`** and **`Bezel_Guide_02_Errors_and_Nullability.md`** — short example-first guides showing idiomatic Bezel. Useful for understanding what *good* Bezel code looks like, and for spec-derived test material.

The filenames are stable across revisions (since v0.131; earlier revisions carried the version in the filename, e.g. `Bezel_Language_Spec_0_130.md` — such citations refer to that archived revision in git history). The current revision number lives in each document's header and in the rationale's per-revision changelog. When citing the spec in a PR, test, or issue, cite the section *and* the revision (e.g. "spec §28.2, v0.131") so the reference stays pinned as revisions land.

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

### Build & test speed (the build is dominated by the ~770-TU `bezel_tests`)

The configure auto-detects accelerators (CMakeLists.txt): **ccache**, the
**mold** linker (falls back to lld), **`-gsplit-dwarf`** (debug info out of the
link), the **shared `libLLVM`** (linked as one `.so`, not static components —
keeps binaries small and links near-instant; the in-process JIT is unaffected),
and a **PCH** of the heavy LLVM/std headers for the test target. Build with
plenty of parallelism — the machine is many-core and each TU peaks well under
the build cage's memory budget: pass `-j16` (or use `cmake --build --preset
default`, which sets jobs); the bundled scripts default to `-j16`. A clean build
is ~2 min and a one-file edit→relink is ~1–2s.

**Run the test suite in parallel**, not as the raw binary — the single binary is
serial gtest (~4–5 min). Use `tests/run_parallel.sh` (or `cmake --build build
--target check`): it shards `bezel_tests` across cores (each shard a separate
JIT process via `BEZEL_SHARD_*`), running the whole suite in ~40s. The full
*in-process* JIT suite is still the load-bearing correctness check (a disposal
double-free once surfaced only there, not in per-process ASan).

One machine-local, not-tracked step is required for ccache to cache the PCH'd
compiles: `~/.config/ccache/ccache.conf` must contain
`sloppiness = pch_defines,time_macros` (without it every PCH build is a cache
miss — slower, never incorrect).

**Concurrency discipline (32 GiB box, parallel dev agents).** Run every full
build and every full suite through `scripts/dev_heavy.sh {build|suite} <cmd>`:
it serializes heavy ops across agents via a global lock (one -j16 build at a
time beats two thrashing ones), cages memory (12G/8G), and runs at background
CPU/IO weight so the interactive session stays responsive. Small probe
compiles/runs use `dev_heavy.sh probe <cmd>` (cage only, no lock). Scratch
artifacts (probe binaries, big logs, scratch build trees) go in
`/var/tmp/bezel-scratch` — never `/tmp`, which is tmpfs (RAM): a scratch build
tree parked there permanently consumes gigabytes. Keep ccache `max_size` at
80G (a full 40G cache thrashes under multi-worktree churn).

**Keep the repo root clean.** Ad-hoc `.bz` probes, one-off `bezelc -o` outputs,
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

**Extra gates for delicate merges.** Merges touching object layout, disposal
machinery, or the runtime additionally run (a) one full **serial** suite pass
(`build/tests/bezel_tests` directly — twice this project, sharded-green runs
masked heap corruption the serial order exposed) and (b)
`scripts/asan_sweep.sh` — a whole-suite ASan shard sweep (caught the v0.175
exception-layout corruption and #1390).

## When in doubt

The project's overall discipline is: ship the part that fits cleanly now, decline-with-reasons the part that fights the foundations, keep adjacent open questions explicitly scoped. If you're not sure whether a given piece of work fits cleanly: stop, open an issue, describe what you're trying to do and what's giving you pause. The project has reached its current state by catching defects early and recording decisions with reasoning. That discipline continues to apply.
