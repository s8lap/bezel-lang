# Bezel

**A compiled language for services and tools, with ownership-based memory management and no garbage collector.**

The compiler tracks who owns every heap object and frees it at a point it knows statically: on the normal path and when an exception passes through, with no collector, no pauses and no `Dispose`/`finally`/`defer` plumbing. Borrowing lets code pass references around without transferring ownership, and the compiler checks that no borrow outlives what it borrows.

```bezel
module Example;

public class Connection(host: string)
  public readonly property Host: string := host;
  public method Send(text: string)
    Console.WriteLine($"{Host} <- {text}");
  end Send;
end Connection;

public class Program
  public static method Connect(host: string): owned Connection   // `owned`: the caller gets a new object
    return new Connection(host);
  end Connect;

  public static method Retire(owned c: Connection)                // takes ownership...
    c.Send("goodbye");
  end Retire;                                                     // ...and disposes it here

  public static method Main()
    using var db := Connect("db");          // owned by this block, disposed at its end
    var view := db;                         // a borrow: nothing copied, nothing transferred
    view.Send("hello");

    var cache := Connect("cache");
    Retire(move cache);                     // a transfer is always written down
  end Main;
end Program;
```

> **Status: experimental.** The language design is close to settled and is written down in full; the compiler implements most of it and is still being brought up to the specification. Expect rough edges, and expect the language to change. There are no stability guarantees yet.

## Why Bezel

- **Memory safety without a collector.** Every object has exactly one owner and is disposed exactly once. Using a moved-out value, disposing twice, keeping a borrow past its owner and dropping a resource silently are compile-time errors; where a stale reference can still arise at run time, the default build fails with an exception instead of corrupting memory.
- **Ownership you can read.** The declaration says who owns a slot (`owned` on a parameter, a return type or a field); the use site says when ownership moves (`move`). An assignment never transfers anything silently.
- **Familiar everywhere else.** Classes, interfaces, generics, records, enums, sum types (`choice`) with exhaustive matching, `async`/`await` with structured concurrency, LINQ-style queries, exceptions, and contracts (`requires`/`ensures`) — in a syntax that reads like the Pascal family with the type system of C#.
- **A specification first.** The language is defined by a normative specification, and every rule is pinned by a test that states the specification's verdict, not the compiler's.

## Getting started

Bezel builds on Linux with clang and LLVM 21. It is developed and tested on Fedora; other platforms are untested ([#446](https://github.com/s8lap/bezel-lang/issues/446)).

```sh
sudo dnf install cmake clang llvm llvm-devel      # LLVM 21
cmake -B build -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j$(nproc)
```

Write `hello.bzl`:

```bezel
module Hello;

public class Program
  public static method Main(): int
    Console.WriteLine("Hello, Bezel!");
    return 0;
  end Main;
end Program;
```

and run it:

```sh
./build/bezelc hello.bzl -o hello && ./hello
```

`Main`'s return value becomes the exit code. Libraries compile separately and are imported by their object file: `bezelc -c lib.bzl -o lib.o`, then `bezelc app.bzl --import lib.o -o app`.

## Learn the language

| Start with | What it is |
|---|---|
| [A Tour of Bezel](docs/Bezel_Tour.md) | the language in half an hour: syntax, ownership, errors, collections, concurrency, contracts |
| [The guides](docs/Bezel_Guide_01_Memory_and_Ownership.md) | short, example-first: memory and ownership, errors and nullability, modelling data, concurrency, collections, contracts, style |
| [Writing reference](docs/Bezel_For_Code_Generation.md) | the whole syntax, the ownership rules as a checklist, every library name with its signature, and the shapes that look right but are rejected |
| [Standard library](docs/Bezel_Standard_Library.md) | every type and member; if a name is not there, it does not exist |
| [Language specification](docs/Bezel_Language_Spec.md) | the normative reference, with the full grammar and the compiler-enforced invariants |
| [Design rationale](docs/Bezel_Design_Rationale.md) | why each decision was made, and what was declined |

`scripts/build_book.py` builds the same documents into a searchable site, one page per chapter; it is published to GitHub Pages once Pages is enabled for the repository.

**Editor support:** [`editors/vscode/`](editors/vscode/) is a VS Code extension with syntax highlighting, editing rules and snippets. Its grammar is generated from the specification.

## How far along is it

- **The design** is written down in full: the [specification](docs/Bezel_Language_Spec.md), the [standard library](docs/Bezel_Standard_Library.md) and the [rationale](docs/Bezel_Design_Rationale.md), with one [changelog](docs/Bezel_Changelog.md) entry per revision.
- **The compiler**, `bezelc`, is an ahead-of-time front end and LLVM back end. It compiles the language's core end to end — ownership and borrow checking, classes and generics, records, enums and choices, exceptions, collections and queries, `async`/`await`, multi-module builds — and much of the standard library.
- **The gap** is measured. A rule corpus of one hand-written case per specification rule ([`tests/conformance/rules/`](tests/conformance/rules/)) records where the compiler still differs: of the 2,671 cases with a recorded result, 2,462 match the specification. Every difference has a tracking issue.

## Repository layout

```text
src/      the compiler: lexer, parser, semantic analysis, ownership, codegen, runtime, modules, driver
tests/    behaviour tests (gtest), diagnostic tests (lit + FileCheck), the conformance corpora
docs/     specification, standard library, rationale, changelog, tour and guides
editors/  editor support (VS Code)
scripts/  build, test and documentation tooling
```

## Contributing and testing

The project works specification first: if the compiler and the specification disagree, the compiler is wrong, and every fix ships a regression test that cites the section it enforces. [`CLAUDE.md`](CLAUDE.md) is the project's working discipline, [`tests/README.md`](tests/README.md) explains how to write a test, and [`docs/proposals/spec-change-process.md`](docs/proposals/spec-change-process.md) describes how the language itself changes.

```sh
tests/run_parallel.sh 16                    # the behaviour suite, sharded (~2 min)
cmake --build build --target check-lit      # the diagnostic suite (needs lit and FileCheck)
scripts/gate.sh                             # both, with a combined total
scripts/check_docs.sh                       # the documentation checks
```

## License

Apache 2.0. See [`LICENSE`](LICENSE).
