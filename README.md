# bezel-lang

Bezel is a systems language designed to feel high-level. It has no garbage
collector; instead, the compiler tracks ownership of every heap object and
ensures that each one is disposed exactly once.

The language is paper-designed: the normative spec lives in `docs/`, and this
repository hosts the compiler that implements it. Start with `CLAUDE.md` for the
project's working discipline and `docs/Bezel_Language_Spec_0_126.md` for the
language itself.

## Status

Scaffold only. The phase modules are stubs; no compilation is implemented yet.
`bezelc` reads a file and runs an empty pipeline. Each phase is filled in by its
own milestone (spec-derived tests first — see `CLAUDE.md`).

## Building

The compiler is C++20, built with CMake (≥ 3.20) using **clang++**, the project
toolchain (see `CLAUDE.md`, "Build toolchain"). No external dependencies yet.

On Fedora, the toolchain is `sudo dnf install cmake clang llvm`.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`CMakeLists.txt` defaults `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` to clang, so a
plain `cmake -B build` uses it. To build with a different compiler, override on
the command line: `cmake -S . -B build -DCMAKE_CXX_COMPILER=...`.

This produces the `bezelc` driver:

```sh
./build/bezelc --version
./build/bezelc path/to/source.bz
```

## Layout

The compiler is one directory per compilation phase under `src/`, aligned to the
spec. Phase boundaries are directories and namespaces, not separate packages.

| Directory          | Phase                          | Spec sections                          |
| ------------------ | ------------------------------ | -------------------------------------- |
| `src/lexer/`       | Lexing                         | §1, §18, §34, Appendix B               |
| `src/ast/`         | AST node definitions           | Appendix A, §9–§27                      |
| `src/parser/`      | Parsing                        | Appendix A, §17, §19–§21               |
| `src/sema/`        | Type checking                  | §3–§14                                  |
| `src/ownership/`   | Ownership & borrow checking    | §28, Appendix C                         |
| `src/codegen/`     | Code generation                | §32, §33 (target/backend unspecified)  |
| `src/driver/`      | Pipeline orchestration         | §2, §33                                 |
| `src/support/`     | Shared infrastructure (source locations, diagnostics) — not a phase | — |
| `src/main.cpp`     | `bezelc` CLI entry point        | §33                                     |

Tests mirror this layout under `tests/`; see `tests/README.md`.

## Open decisions recorded in the scaffold

- **Codegen target/backend is unspecified.** The spec defines the source
  language but not a compilation target (native/LLVM, C, bytecode VM, …).
  `src/codegen/CodeGen.hpp` carries this note; the choice is a project-level
  decision to be settled by an issue before codegen grows a body.
- **No test framework adopted.** Tests use a minimal in-repo harness
  (`tests/test_harness.hpp`) rather than a third-party dependency. Adopting one
  is a deferred project-level decision.

## Conventions

Standard C++ conventions, applied consistently since there is no prior code to
match:

- Types: `PascalCase`. Functions, methods, locals: `camelCase`. Member
  variables: trailing underscore (`foo_`). Namespaces: lowercase (`bezel`,
  `bezel::ast`, `bezel::test`).
- Headers use `#pragma once`. Includes are quoted and rooted at `src/`
  (e.g. `#include "lexer/Lexer.hpp"`).
- Any non-obvious code cites the spec section it derives from, e.g. `// per §28`.
- Source files are listed explicitly in CMake (no globbing) so that adding a
  file is always a reviewable diff.
