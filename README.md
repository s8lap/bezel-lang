# bezel-lang

Bezel is a systems language designed to feel high-level. It has no garbage
collector; instead, the compiler tracks ownership of every heap object and
ensures that each one is disposed exactly once. Borrowing lets you pass
references without transferring ownership, with lifetimes checked at compile
time rather than enforced by a runtime.

The language is paper-designed: the normative spec lives in `docs/`, and this
repository hosts the compiler that implements it. Start with `CLAUDE.md` for the
project's working discipline and `docs/Bezel_Language_Spec_0_129.md` for the
language itself.

## Status

The project is a compiler-under-construction for a language with no users yet.
The compiler AOT-compiles `.bzl` source files to native executables and
implements a large and growing subset of the spec. The test suite contains
4,274 spec-derived test cases (all passing on Fedora), spanning the full
compiler pipeline from lexer through codegen.

**What works:**

- Ownership tracking with dispose-exactly-once semantics; borrow checking
- Classes, interfaces, generics, records, enums
- Inheritance, virtual dispatch, interface dispatch (including cross-module)
- Exceptions: `try` / `catch` / `finally`
- Reference-counted runtime-managed types: strings, closures, boxes
- Standard library: `Array<T>`, `Dictionary<K,V>`, `Set<T>`, `Stack<T>`,
  `Queue<T>`, `LinkedList<T>` (and owned variants); `String`, `StringBuilder`;
  LINQ; `Encoding.Utf8`; `Math`, `Convert`; `DateTime`, `TimeSpan`, `Guid`;
  `CancellationToken`, `Task`
- Async/await end-to-end: cross-thread scheduler, async iterators / `await for`,
  `Task` combinators, `await using`
- Multi-module separate compilation (`bezelc -c` / `--import`)

**Not yet implemented (compiler emits a diagnostic):**

- `typeof(T)` and `.GetType()` — runtime reflection (§17.10, §8.2); deferred
  pending `System.Reflection`
- Async lambdas (§30.6) — the state-machine lowering for lambda-specific async
  frames is not yet implemented; sync and async methods work
- Cross-module instantiation of generic types (§14, §2.5) — importing a generic
  class or record and constructing it cross-module; generic methods work
- `await` inside a catch guard (`catch ... when`) or inside a try/catch/finally
  body with an async-iterator (§30 / §29)
- Open-ended range patterns in `case` (§19)

**Platform:** developed and tested on Fedora Linux. The runtime uses POSIX
threads, `clock_gettime`, `pthread`, and `/dev/urandom`, so Windows is not yet
supported. See [Cross-platform notes](#cross-platform-notes) below.

## Building

**Prerequisites:** CMake ≥ 3.20, clang/clang++, LLVM 21 dev headers and
libraries. The compiler calls the LLVM 21 C++ API extensively; older LLVM
versions are untested and will likely need source-level changes.

---

### Fedora (verified)

```sh
sudo dnf install cmake clang llvm llvm-devel
```

`llvm-devel` provides `llvm-config` and `LLVMConfig.cmake`, which CMake uses to
locate the LLVM installation. Optionally install `ccache` and `lld` for faster
incremental builds — the build system detects and uses them automatically:

```sh
sudo dnf install ccache lld   # optional; auto-detected
```

Then configure and build:

```sh
cmake -B build -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j$(nproc)
```

CMake prints `Bezel: using LLVM 21.x.x` and, if installed, `Bezel: using
ccache` and `Bezel: using lld linker`.

---

### Cross-platform notes

The build has only been verified on Fedora Linux. The notes below are
best-effort guidance for other platforms; they are **untested**. Verification
and any needed fixes are tracked in issue #446.

#### Debian / Ubuntu — UNTESTED

```sh
sudo apt install cmake clang llvm-dev
cmake -B build -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j$(nproc)
```

Ubuntu 24.04 LTS ships LLVM 18; you may need to install LLVM 21 from
[apt.llvm.org](https://apt.llvm.org/) and point CMake at it with
`-DLLVM_DIR=/path/to/llvm21/lib/cmake/llvm`.

#### Arch Linux — UNTESTED

```sh
sudo pacman -S cmake clang llvm
cmake -B build -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j$(nproc)
```

Arch typically ships a recent LLVM.

#### macOS — UNTESTED

Apple's system clang does not include LLVM development headers; install
the Homebrew `llvm` formula which provides them:

```sh
brew install llvm cmake
cmake -B build \
    -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++ \
    -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm
cmake --build build -j$(nproc)
```

#### Windows — NOT SUPPORTED

The runtime uses POSIX threads (`<pthread.h>`), `clock_gettime`, and
`/dev/urandom`. Cross-platform runtime portability work is tracked in issue
#446.

## Usage

### AOT compilation

Source files use the `.bzl` extension. A minimal program — `Main` returns an
integer that becomes the process exit code:

```
module M;
class C
  public static method Main(): int
    return 42;
  end Main;
end C;
```

```sh
./build/bezelc hello.bzl -o hello && ./hello; echo $?
# 42
```

### Multi-module separate compilation

Compile a library to a native object with embedded interface metadata (§2.4):

```sh
./build/bezelc -c lib.bzl -o lib.o
```

Compile an application that imports the library (§2.5); `--import` supplies the
dependency object for both metadata resolution and linking:

```sh
./build/bezelc app.bzl --import lib.o -o app
```

Multiple `--import` flags are accepted for multiple dependencies.

## Testing

Run the full test suite:

```sh
./build/tests/bezel_tests
```

Or use the helper script, which removes any stale binary before rebuilding (the
stale-binary problem is documented in `scripts/run-tests.sh`):

```sh
scripts/run-tests.sh
```

Run a subset by name substring:

```sh
BEZEL_TEST_FILTER=<substring> ./build/tests/bezel_tests
```

For example, `BEZEL_TEST_FILTER=codegen` runs only codegen tests,
`BEZEL_TEST_FILTER=ownership` runs only ownership tests.

## Repository layout

```
src/
  lexer/      Lexer (§1, §18, §34, Appendix B)
  ast/        AST node definitions (Appendix A, §9–§27)
  parser/     Parser (Appendix A, §17, §19–§21)
  sema/       Type checking (§3–§14)
  flow/       Control-flow graph
  ownership/  Ownership & borrow checking (§28, Appendix C)
  codegen/    LLVM IR code generation (decision #147)
  runtime/    Bezel standard library native layer (Array, Dict, Task, …)
  module/     Module metadata codec + cross-module loader (§2)
  driver/     Pipeline orchestration (§2, §33)
  main.cpp    bezelc CLI entry point
tests/        Spec-derived tests, mirroring src/ phase layout
scripts/      Build and test helpers (run-tests.sh)
docs/         Normative spec, standard library, design rationale, guides
```

## Further reading

- `docs/Bezel_Language_Spec_0_129.md` — the normative reference; Appendix A is
  the complete grammar, Appendix C lists compiler-enforced invariants
- `docs/Bezel_Standard_Library_0_129.md` — every stdlib type and method
- `docs/Bezel_Design_Rationale_0_129.md` — the recorded reasoning behind each
  language decision
- `docs/Bezel_Guide_01_Memory_and_Ownership.md` and
  `docs/Bezel_Guide_02_Errors_and_Nullability.md` — example-first guides to
  idiomatic Bezel
- `CLAUDE.md` — working discipline for contributors and AI sessions

## License

Apache 2.0. See `LICENSE`.
