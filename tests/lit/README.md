# Bezel lit + FileCheck diagnostic test track

This directory holds an **additive, complementary** test track for the Bezel
compiler: [LLVM `lit`](https://llvm.org/docs/CommandGuide/lit.html) drives the
real `bezelc` binary over `.bz` source files, and LLVM's `FileCheck` matches the
compiler's diagnostics against `// CHECK:` directives embedded in each file.
This is the idiomatic way an LLVM-based compiler tests its front-end
diagnostics and compile-time behavior.

It **complements — does not replace** — the two existing test suites:

- the in-process **gtest / JIT suite** (`tests/**/*.cpp`, run via
  `tests/run_parallel.sh` or `cmake --build build --target check`), and
- the coarse **conformance corpus** (`tests/conformance/`, `manifest.tsv` maps
  each case to a `compiles` / `fails` / `program` expectation).

## Why this exists (three concrete problems it fixes)

1. **Sharding-pollution flakes.** The in-process gtest binary runs many tests in
   one process (sharded by `run_parallel.sh`); cross-test global-state leakage
   between tests in the same shard has produced flakes. lit runs **each test as
   its own `bezelc` process**, so cross-test leakage cannot happen here.

2. **Wrong-harness → reject invisible.** The gtest suite has several front-end
   harnesses that run *different subsets* of the pipeline (a sema-only path, an
   ownership path, a full-JIT path). A test written against the wrong one can
   silently miss a diagnostic: a real §22.3 reject emitted by the **ownership
   pass** was invisible to the sema-only harness and cost a gate cycle
   (issue #1682). lit invokes the **real `bezelc`, which always runs the full
   pipeline** (lex → parse → sema → ownership → codegen), so a diagnostic from
   any pass is always in the observed output. See
   `ref_out_owned_local_1682.bz` — the flagship example.

3. **Build cost.** These tests are plain text files: no per-test C++
   compilation, no relink. Adding a diagnostic test costs nothing at build time.

lit + FileCheck also does something neither existing suite can: **fine-grained
diagnostic-content matching** — the exact rule wording, the spec section cited,
and the precise `file:line:col` location (see `const_div_by_zero_1688.bz`, which
pins the error to an exact source line with `[[@LINE-1]]`). The conformance
manifest's single expectation column (`compiles` / `fails`) cannot express any
of that.

## When to use which suite

| Use lit + FileCheck for…                     | Use the in-process gtest / JIT suite for…         |
| -------------------------------------------- | ------------------------------------------------- |
| Compile-time **reject** diagnostics          | **Runtime** behavior / program output             |
| Exact diagnostic **text / section / column** | **Disposal** ordering, double-free, refcounting   |
| Clean-compile **accept** checks              | Anything needing the **JIT** or cross-test heap   |
| "the reject must come from the right pass"   | accumulation in one process                       |

The in-process JIT suite stays **load-bearing**: a disposal double-free once
surfaced *only* there, because it needed cross-test heap accumulation within a
single process (not reproducible under per-process ASan). Do **not** migrate
runtime/disposal tests here.

## Layout

- `lit.cfg.py` — machine-independent lit config (suite name, `.bz` suffix,
  `%bezelc` / `%FileCheck` / `FileCheck` substitutions, `ShTest` format).
- `lit.site.cfg.py.in` — CMake template; `cmake` configures it into
  `<build>/tests/lit/lit.site.cfg.py` with the concrete `bezelc` and
  `FileCheck` paths, then loads `lit.cfg.py`.
- `*.bz` — the tests. Each begins with `// RUN:` lines and carries `// CHECK:`
  directives. Each is ported from a named gtest and cites its spec section.
- `requirements-dev.txt` — pins `lit` (dev-time only; see below).

## Running

Preferred (configures the site config from CMake, then runs lit):

```
cmake --build build --target check-lit
```

Directly with lit (after a CMake configure has generated the site config):

```
lit -sv build/tests/lit
```

Or ad hoc against the source tree (uses `lit.cfg.py`'s discovered-tool
fallback — `bezelc` and `FileCheck` must be on `PATH`):

```
lit -sv tests/lit
```

`-s` = succinct, `-v` = show output of failing tests. `lit` exits non-zero if
any test fails.

## Dependencies

- **`FileCheck`** — provided by the system LLVM this project already links
  (Fedora: `/usr/bin/FileCheck`, with a `/usr/bin/FileCheck-21` symlink). Not a
  new dependency.
- **`lit`** — a tiny pure-Python test driver. **Dev-time only**: it is *not* a
  runtime or build dependency of `bezelc`; a plain `cmake --build` never needs
  it, and nothing that ships depends on it. Install per-user:

  ```
  pip3 install --user -r tests/lit/requirements-dev.txt
  ```

  If `lit` is not installed, the `check-lit` CMake target is simply skipped with
  a status message — it never breaks the build.

## The `%not` wrapper on reject tests (important)

Every **reject** test's RUN line wraps the compile in `%not`:

```
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
```

`%not` is LLVM's `not` utility (it inverts a command's exit status). It is
**required**, not decoration: a reject test's whole point is that `bezelc`
exits non-zero, but lit's shell **drops the output of a non-zero-exit producer
stage in a pipeline** (verified on lit 18.1.8 — FileCheck then sees empty input
and the test spuriously fails). Wrapping the compile in `not` makes the failing
stage exit 0, so its diagnostics flow to FileCheck. This is exactly why LLVM's
own front-end tests are written `not %clang … | FileCheck`. **Accept** tests
(which must exit 0) use a bare `%bezelc` with no `%not`.

## Adding a test

1. For a **reject**, write a `.bz` file whose first line is
   `// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s` (note the `%not`). For an
   **accept**, use `// RUN: %bezelc -c -o %t.o %s` — a zero exit *is* the pass.
2. Add `// CHECK:` directives matching the expected diagnostic. Use
   `{{[0-9]+}}` for the variable column, `[[@LINE-1]]` to pin a line relative to
   the CHECK, and `CHECK-SAME:` to match more of the same diagnostic line.
   `bezelc` emits diagnostics as `file:line:col: message` with **no "error:"
   severity word**, so anchor on the `:line:col:` shape and the rule text.
3. Prefer porting from (or pairing with) an existing gtest, and cite the spec
   section (`§NN.N`, with the revision) in a comment — same discipline as the
   gtest suite.
4. **Prove the CHECK bites**: temporarily corrupt one `// CHECK:` line and
   confirm the test fails, then restore. A CHECK that matches nothing real is
   worse than no test.
