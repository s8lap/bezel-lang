# Tests

Tests follow the per-milestone workflow described in `CLAUDE.md`:

1. A milestone's **first** PR adds spec-derived tests — every example from the
   spec section the milestone covers, with the spec-stated input and expected
   output. These tests *fail* until the implementation lands; that CI failure is
   expected.
2. The tests are reviewed as a translation of the spec into executable form.
3. Implementation PRs follow until the tests pass, without modifying the tests
   except to fix legitimate test bugs.

Each test names the spec section and example it derives from, e.g.
`BEZEL_TEST(lexer_string_interpolation, spec_1_5_example_3)`.

## Layout

The directories here mirror the compiler phases under `src/` and are where each
phase's spec-derived tests go:

- `lexer/` — §1, §18, §34, Appendix B
- `parser/` — Appendix A, §17, §19–§21
- `sema/` — §3–§14
- `ownership/` — §28, Appendix C

Every `*.cpp` under these directories joins the suite automatically
(`file(GLOB … CONFIGURE_DEPENDS)` in `tests/CMakeLists.txt`); to **exclude** a
file, rename it `*.cpp.disabled`. Diagnostic tests increasingly live as `.bzl`
files under `lit/` instead (see "Diagnostic tests" below).

## Harness

`test_harness.hpp` is a minimal, zero-dependency C++ harness (register with
`BEZEL_TEST`, assert with `BEZEL_EXPECT_TRUE` / `BEZEL_EXPECT_FALSE` /
`BEZEL_EXPECT_EQ`). `sanity_test.cpp` only verifies the harness itself.

### Why AOT, not JIT

Tests compile Bezel source and check the result. The compiler is **AOT-only**
(`bezelc -c` / `bezelc -o`); there is no `run`/`jit` mode. An older harness
(`runJit`) JIT-compiled and ran a program *in-process* but **skipped the
ownership pass, the definite-assignment pass, and LLVM `verifyModule`** — a
*lenient* oracle that silently ran ill-formed programs and masked codegen bugs.

The AOT harness (`runAot` and friends, in `support/aot_support.hpp`) drives the
**real** `bezelc` front-end (`Driver::compileFile`, fork-isolated) and runs the
linked executable as a subprocess — the **strict, correct oracle**. If a test
fails under AOT but "worked" under JIT, the test program is almost always
*ill-formed* (an ownership/DA violation the JIT skipped): **fix the test to be
valid Bezel, don't work around the AOT check.**

### Which helper? (all return `JitResult`: `.ran`, `.exitCode`, `.stdoutText`, `.diagnostics`)

- **`runAot(src)`** — normal behavior test whose result fits an 8-bit exit code
  (**0..255**). Assert `r.exitCode` (via a local `run()` wrapper). Reject tests
  can also use `runAot` and assert `r.diagnostics.hasErrors()` / `.message.find`
  — a compile reject's stderr is captured as a diagnostic, so these work as under
  JIT.
- **`runAotOut(src)`** — the result is **outside 0..255** or **negative** (e.g.
  `IndexOf`-not-found = -1, `2024`, `60007`). A process exit status is masked to
  `WEXITSTATUS` (0..255), so move the value to **stdout**: make `Main` do
  `Console.WriteLine(<expr>); return 0;` and assert the **bare value** string
  (e.g. `== "60007"`). `runAotOut` strips the trailing line-ending
  (`Console.WriteLine` appends the platform-dependent `Console.NewLine`), so the
  expectation is platform-independent — do **not** write `"60007\n"`. If the
  observable is `char`/enum-typed, wrap it `int(<expr>)`.
- **`runAotChecked(src)`** — ownership / disposal / **leak** tests. Same as
  `runAot` but the generated code is ASan/LSan-instrumented, so a leak /
  double-free / use-after-free flips `r.ran` to false. (Note: this is *memory*
  checking — distinct from checked-disposal *mode*, `bezelc --checked-disposal`
  §28.2, which inserts `DisposedAccessException` guards.)

### Diagnostic tests → lit + FileCheck (`tests/lit/*.bzl`)

Reject/accept **diagnostic** tests live as `.bzl` files with `// RUN:` directives
(config in `lit/lit.cfg.py`: `%bezelc`, `%not`, `%s`, `%t`, `FileCheck`).

Reject:
```
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
// CHECK: cannot observe 'this' ... (§22.7)
module M; ...ill-formed program...
```
`%not` is required (lit drops a non-zero-exit producer's output in a pipe).
Accept: `// RUN: %bezelc -c -o %t.o %s`. Diagnostics have **no error codes** —
match the **message text** + the `(§NN.N)` citation. lit runs the **full**
`bezelc -c` pipeline, so a test meant to exercise only one pass may hit an
unrelated downstream gap — those stay as gtest. Bulk gtest→lit conversion is
scripted by `scripts/extract_diag_to_lit.py`.

### Running

- **A producer/fix lane building+testing its OWN change → `scripts/agent_check.sh
  <filter>`.** One command: reconfigure (picks up a new test `.cpp`) + build via
  `dev_heavy` (slice-capped, foreground) + run ONLY that targeted filter
  (substring on gtest suite-or-name — *not* the file basename). `--probe FILE.bzl`
  (or `--probe --asan FILE.bzl`) for a single program; output goes to the scratch
  dir. It refuses an empty filter and `--gtest_filter` (the binary ignores that
  flag). A lane must NOT run the whole suite — see the last two bullets, those are
  the coordinator/gate's job.
- **The suite is several per-area binaries**, `build/tests/bezel_tests_<group>`
  (`codegen`, `ownership`, `sema`, `frontend`, `module`, `support`), one per
  `tests/<area>/` group — so a test-only edit relinks only its own area.
  `bezel_tests` is still the *target* that builds them all, and
  `build/tests/test_binaries.txt` (generated: `<group> <TAB> <path> <TAB>
  <#sources>`) is the list every runner reads. Never hardcode a binary path.
  `scripts/agent_check.sh --area <group> <filter>` builds+runs just one.
- Targeted, by hand: `BEZEL_TEST_FILTER=<substr> build/tests/bezel_tests_sema`
  (the binaries ignore `--gtest_filter`). `BEZEL_TEST_ENUMERATE=1` prints what a
  process *would* run (`suite.name` per line) and exits without running — use it
  to find which area holds a test, or to verify a shard partition.
  ⚠ **This form does NOT rebuild anything.** After a `src/` change,
  `scripts/agent_check.sh --area <group> <filter>` legitimately relinks only
  *that* group's binary (that's the point — ~50 MB instead of ~600 MB), so
  every OTHER group's binary on disk is left exactly as it was, possibly still
  linked against an older `libbezel_core.a` (`agent_check.sh` now warns about
  this by name — see #2426). Running one of those stale binaries directly
  reports results computed against pre-edit object code with no indication
  anything is wrong. Rebuild first (`agent_check.sh` with no `--area`, or
  `cmake --build build --target bezel_tests_<group>`) before trusting a
  by-hand run of a binary you didn't just build yourself.
- Exact-list subset (#2100): `BEZEL_TEST_LIST=<file>` — one `suite.name` per
  line, exact match, composable with the filter. Each binary's summary then
  carries an explicit `N selected` field and `run_parallel.sh` switches its
  drop-detection to *selected == ran*, **per binary** (a binary holding none of
  the listed tests legitimately selects 0); "the list selected nothing at all"
  and the phantom-name warning are computed globally by the runner, which is the
  only place that can see the whole list.
  Lists are produced by `scripts/select_tests.py` (diff → affected tests;
  `--variant lane` for a tight lane subset, `--variant gate` for the canary;
  `--check` self-tests its parsing invariants). Lane convenience:
  `scripts/agent_check.sh --select [base]`.
- Sharded (whole suite): `tests/run_parallel.sh 16` — the argument is the
  *concurrency* cap (how many test processes run at once), not a per-binary
  shard count: every binary is sliced that many ways and the slices go through
  one bounded queue. The sharded pass is the authoritative oracle (#1861); it
  can over-report under fork-resource contention, so a suspect is re-checked in
  isolation (`BEZEL_TEST_FILTER=<name> build/tests/bezel_tests_<group>`), not by
  a serial whole-suite pass.
- lit track: `cmake --build build --target check-lit`.
- Full gate: `scripts/gate.sh` (`--full` adds the whole-suite ASan sweep, for
  codegen/runtime/disposal/layout changes). The gate runs a diff-selected
  **canary** subset before the authoritative full pass — fail-fast only, never
  acceptance (#2100; knobs `GATE_CANARY=0`, `GATE_CANARY_BASE`,
  `GATE_CANARY_MAX_PCT`).

### The AOT build-product cache, and the one way to poison it

Every `runAot` compile+link is content-addressed into a **shared, cross-worktree**
store (`/var/tmp/bezel-scratch/aot-bincache`, symlinked to the fast disk by
`scripts/lane_setup.sh`). Every lane and the gate write it at once. The key is
`SHA-256(libbezel_core.a) ‖ SHA-256(libbezel_runtime.a) ‖ toolchain ‖ mode ‖
source` — it keys on the compiler **archive**, not on the test binary, which is
exactly what keeps the store warm across a test-only relink.

That has one consequence worth knowing by heart (#2877): **a binary whose
compiler differs from the archives it names must not write to the shared
store.** The routine way to produce one is the cheap FAIL-WITHOUT trick —
recompile the one changed `src/` file at its parent revision and put the object
*ahead* of the archive on the link line, so `ld` takes it and never extracts the
member it shadows. That binary is a different compiler under an identical key.
Measured in #2877: it stored pre-fix executables under the post-fix archives'
keys, and the next run of the real binary was served them — five reject rows
"ran" and returned the pre-fix answer, while the same binary with the cache off
was 12/12 green.

Two mechanisms now stand between you and that:

- **Automatic.** The archive identity is trusted only for a binary the build
  system *declared* (`build/tests/test_binaries.txt`); anything else keys on its
  own whole-binary hash and so isolates itself. A hand-link that lands outside
  the manifest is covered without you doing anything.
- **Manual, for what the guard cannot see** — an `LD_PRELOAD`, a patched
  `libLLVM`, a doctored archive, or a run of the experimental
  `BEZEL_OWNERSHIP_LAMBDA_DATAFLOW` / `BEZEL_VERIFY_IR` flags: set
  **`BEZEL_AOT_CACHE_SALT=<anything>`**. It folds into every key, so the run's
  entries are isolated in one word — no cold private `BEZEL_AOT_CACHE_DIR`, no
  losing the cache to `BEZEL_AOT_CACHE=0`. (Those two flags salt themselves
  automatically; the knob is for everything else.)

The other half of the same rule is about the **key**: every `CodegenOptions`
bit that changes the emitted object or the link line must appear in
`modeString()` *and* `codegenModeString()` in `tests/support/aot_support.hpp`.
One did not — `checkedContracts` (#4386, §22.16) — so `runAot(src)` (which
defaults it ON) and `runAot(src, CodegenOptions{})` (the release default) keyed
the same source identically and each served the other its binary. A
`static_assert` on `sizeof(CodegenOptions)` in `aot_cache_key_test.cpp` now
stops the build if an option is added without being keyed — though only once
the `support` area is compiled, so `--target bezelc` and
`agent_check.sh --area codegen` will not surface it; the gate will.

And the diagnosis, unchanged: **a test that fails in a way your diff cannot
reach is a cache question before it is a compiler question.** Re-run it with
`BEZEL_AOT_CACHE=0` before believing it. A hit is silent, so a poisoned entry
hides a regression exactly as easily as it invents one.

### Behaviour differential — the oracle for a change that ships no test

A pure refactor (a deduplication, a unification, a rename) is supposed to change
nothing observable, so it can ship no regression test that FAILS WITHOUT and
PASSES WITH — the discipline the rest of this file is built around has nothing
to bite on. A green suite then only says *the behaviours the suite already pins
did not move*; it says nothing about the ~2,800 programs in `tests/conformance/`
and `tests/lit/` that no gtest asserts on individually.

`scripts/behaviour_differential.sh` is the oracle for exactly that case. It
compiles **and runs** both corpora under a BASELINE `bezelc` and a CANDIDATE
`bezelc` and compares what a user of the language can observe — compile exit
status, compiler diagnostics, run exit status, program stdout, program stderr —
reporting each difference under a named class and exiting non-zero on any of
them.

```
scripts/behaviour_differential.sh --self-test         # ~20s, no compiler needed
scripts/behaviour_differential.sh \
    --baseline  /path/to/main-build/bezelc \
    --candidate ./build/bezelc
```

Why not `cmp` the object files: `bezelc` is not byte-reproducible against
*itself*. Generated symbol names embed raw AST pointers (#3548), async/iterator
spill order is pointer-hash dependent (#3504), and a one-`bool` change to
`sizeof(sema::LocalSymbol)` reorders spill groups — identical instruction
multiset, different bytes. Object `SAME` is conclusive; object `DIFF` is not.
Running the program is immune to all three.

Three things about it are worth knowing before you trust a green:

- **It is fail-closed.** It refuses to report clean unless it measured that it
  did work: the corpus must clear a size floor, every program must yield an
  observation from both compilers, at least one must compile cleanly under both,
  and `executed` must EQUAL `runnable under both` — every program that compiled
  in executable mode under both compilers must actually have been run.
- **It prints how much each observable channel carried.** Measured at 907ce0f:
  of 467 programs that ran under both, only **24 printed anything** and only 5
  wrote to stderr. A bare "467 executed, 0 stdout mismatches" overstates its
  evidence by ~19x, so the report names the witness count per channel and marks
  the thin ones. Read that table before reading the verdict. (The corpus gap
  behind those two numbers is filed as issue 3607.)
- **Every mismatch is re-observed on both sides before it is reported**, and
  a side that disagrees with itself is filed under `nondeterministic` rather
  than reported. A raw one-shot diff between two runs is not conclusive.

The baseline binary must keep its own build tree: `bezelc` links the
`libbezel_runtime.a` that tree built, so a preserved binary is not a preserved
experiment. Build the baseline in a separate checkout and leave it there.
