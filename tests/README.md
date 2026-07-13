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
file, rename it `*.cpp.disabled`. Diagnostic tests increasingly live as `.bz`
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

### Diagnostic tests → lit + FileCheck (`tests/lit/*.bz`)

Reject/accept **diagnostic** tests live as `.bz` files with `// RUN:` directives
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

- Sharded: `tests/run_parallel.sh 16`. **Serial** (`build/tests/bezel_tests`) is
  the authoritative oracle — the sharded runner can over-report under
  fork-resource contention; a real failure reproduces serially.
- lit track: `cmake --build build --target check-lit`.
- Full gate: `scripts/gate.sh` (`--full` adds the whole-suite ASan sweep, for
  codegen/runtime/disposal/layout changes).
