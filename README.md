# bezel-lang

Bezel is a compiled language for services and tools, with ownership-based memory
management and no GC. Instead of a garbage collector, the compiler tracks
ownership of every heap object and guarantees each is disposed exactly once;
borrowing lets you pass references without transferring ownership, with lifetimes
checked at compile time.

The normative spec lives in `docs/`; this repository hosts the compiler that
implements it. Start with `docs/Bezel_Language_Spec.md` for the language and
`CLAUDE.md` for the project's working discipline.

## Status

`bezelc` compiles `.bzl` source to native executables and implements a large,
growing subset of the spec, backed by an extensive spec-derived test suite (all
passing on Fedora) spanning lexer through codegen.

**Works:** ownership tracking (dispose-exactly-once) and borrow checking;
classes, interfaces, generics, records, enums; inheritance and virtual/interface
dispatch (incl. cross-module); exceptions (`try`/`catch`/`finally`);
reference-counted strings, closures, and boxes; standard-library collections
(`Array<T>`, `Dictionary<K,V>`, `Set<T>`, `Stack<T>`, `Queue<T>`,
`LinkedList<T>`, and owned variants), `String`/`StringBuilder`, LINQ,
`Encoding.UTF8`, `Math`/`Convert`, `DateTime`/`TimeSpan`/`Guid`,
`CancellationToken`/`Task`; async/await end-to-end (scheduler, async iterators /
`await for`, `Task` combinators, `await using`); multi-module separate
compilation (`bezelc -c` / `--import`).

**Not yet implemented** (the compiler emits a diagnostic): runtime reflection
(`typeof(T)`, `.GetType()`; §17.10, §8.2); async lambdas (§30.6); cross-module
construction of imported generic types (§14, §2.5); a few `await`-in-guard /
-finally corners (§30, §29). See the issue tracker for the live list.

Developed and tested on **Fedora Linux**; the runtime uses POSIX APIs, so other
platforms are untested (tracked in #446).

## Building

**Requirements:** CMake ≥ 3.20, clang/clang++, and LLVM 21 dev headers and
libraries (the compiler uses the LLVM 21 C++ API extensively; other versions are
untested).

```sh
sudo dnf install cmake clang llvm llvm-devel
cmake -B build -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j$(nproc)
```

Optional, auto-detected accelerators: `ccache` and the `mold` (or `lld`) linker
— CMake prints which it found (`Bezel: using LLVM 21.x.x`, `… using ccache`, …).

## Usage

`.bzl` source; `Main`'s return value becomes the process exit code:

```
module M;
class C
  public static method Main(): int
    return 42;
  end Main;
end C;
```

```sh
./build/bezelc hello.bzl -o hello && ./hello; echo $?   # 42
```

Multi-module separate compilation — compile a library to a native object with
embedded interface metadata (§2.4), then import it (§2.5) for metadata and
linking:

```sh
./build/bezelc -c lib.bzl -o lib.o
./build/bezelc app.bzl --import lib.o -o app     # repeat --import for more deps
```

## Testing

Two tracks: a **gtest behavior suite** (compiles and runs programs through the
real `bezelc`) and a **lit + FileCheck diagnostic track**
(`tests/lit/*.bzl`, matching compiler diagnostics). The diagnostic track needs
`lit` and `FileCheck` (from LLVM) on `PATH`; without them it is skipped cleanly.

```sh
tests/run_parallel.sh 16                    # sharded gtest suite (~2 min)
cmake --build build --target check-lit      # diagnostic track
scripts/gate.sh                             # both + a serial pass; prints a combined total
BEZEL_TEST_FILTER=<substr> build/tests/bezel_tests   # a subset, serially
```

`scripts/gate.sh` is the blessed pre-push gate (`--full` adds a whole-suite ASan
sweep). The serial binary (`build/tests/bezel_tests`) is authoritative — the
sharded runner can over-report under load. See `tests/README.md` for how to
write a test (the `runAot` harness and lit conventions).

## Repository layout

```
src/    lexer, ast, parser, sema, flow, ownership, codegen, runtime, module, driver
tests/  spec-derived gtest tests + lit/ diagnostic track, mirroring src/ phases
scripts/ build/test/gate helpers (gate.sh, dev_heavy.sh, run_parallel.sh, …)
docs/   normative spec, standard library, design rationale, guides
```

## Further reading

- `docs/Bezel_Language_Spec.md` — normative reference (Appendix A grammar,
  Appendix C invariants)
- `docs/Bezel_Standard_Library.md` — every stdlib type and method
- `docs/Bezel_Design_Rationale.md` — the recorded reasoning behind each decision
- `docs/Bezel_Guide_01_Memory_and_Ownership.md`,
  `docs/Bezel_Guide_02_Errors_and_Nullability.md` — example-first guides
- `CLAUDE.md` — working discipline; `tests/README.md` — test-writing guide

## License

Apache 2.0. See `LICENSE`.
