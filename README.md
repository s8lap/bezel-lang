# Bezel

**A compiled language for services and tools, with ownership-based memory management and no garbage collector.**

Memory management in general-purpose languages is still a trade-off. Manual management is fast but easy to get wrong: a missed free leaks, an early one corrupts memory. Garbage collection and automatic reference counting remove those errors, but they cost time and memory at run time, and a collector adds pauses. Bezel takes a third route. The compiler knows, for every object, where it is created, who owns it, and the exact point at which it is released. It inserts the release itself, on the normal path and when an exception passes through. The effect is close to C++'s RAII, except that the compiler checks the rules instead of leaving them to discipline. There is no collector, no reference count on any class you write, and no pause.

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
>
> Most of the language already works. The remaining bugs are mostly memory errors where two features meet, and each one is tracked in the issue tracker. A few current examples:
> - an owned collection passed as an argument leaks when a later argument throws ([#6670](https://github.com/s8lap/bezel-lang/issues/6670));
> - a lambda that captures a string and is passed to a constructor leaks its capture cell ([#6628](https://github.com/s8lap/bezel-lang/issues/6628));
> - a plain `var` local holding an `Array<string?>` is never reclaimed, so its storage and every element leak ([#6516](https://github.com/s8lap/bezel-lang/issues/6516));
> - a user-defined operator whose parameter is a standard collection interface crashes when its body uses the operand ([#6655](https://github.com/s8lap/bezel-lang/issues/6655)).

## How memory management works

The rules are in [chapter 28 of the specification](docs/Bezel_Language_Spec.md#sec-resource-management). They are long because they are precise, but most of their weight falls on the compiler, which tracks ownership through every statement. The programmer meets three keywords:

- **`owned`** marks a slot that holds ownership: a parameter that takes it, a return type that hands it to the caller, or a field that keeps it for the life of its object.
- **`move`** transfers ownership at a use site. An existing object never changes hands without it, so every transfer is visible in the source.
- **`scoped`** marks a callback parameter that the callee may store rather than only call. The stored callback lives no longer than the object that stores it, so a lambda passed there may still capture borrowed locals: the compiler checks that everything it captures outlives that object ([§28.14](docs/Bezel_Language_Spec.md#sec-scoped-callback-parameters)).

Everything else is a **borrow**: a reference passed around without transferring ownership. The compiler checks that no borrow outlives what it borrows, so a dangling reference is a compile-time error.

Only an object's owner may dispose it ([§28.6](docs/Bezel_Language_Spec.md#sec-who-may-dispose)). Calling `Dispose()` runs the object's cleanup, its destructor, and marks it disposed. It does not free the memory. The memory is always released by code the compiler inserts at the owner's release point: the end of a scope, the overwrite of a field, the disposal of the object that holds it. Using an object after it has been disposed raises `DisposedAccessException` in the default build instead of reading freed memory.

Reference counting appears only where the runtime owns a value's lifetime: strings, boxed values and function values such as closures ([§28.9](docs/Bezel_Language_Spec.md#sec-runtime-managed-types)). The specification leaves the mechanism to the runtime, and the current one counts references. Shared ownership is never implicit. A program that wants it says so with `Shared<T>` and `Weak<T>` ([§28.13](docs/Bezel_Language_Spec.md#sec-shared-ownership)).

## A longer example

Bezel's syntax comes from the Pascal family: blocks close with `end`, assignment is `:=` and equality is `=`. Its type system is close to C#'s, and its standard library borrows many .NET names, but the resemblance is on the surface: the library is its own design, built around ownership. The program below has a sum type (`choice`), `case` and `if` used as expressions, a function value, and lambdas passed to query operators:

```bezel
module Shapes;

public choice Shape                                  // a sum type: each value is exactly one case
  Circle(radius: double),
  Rectangle(width: double, height: double),
  Dot;                                               // a case with no payload
end Shape;

public class Program
  // `case` as an expression; the compiler checks that every case is covered
  public static method Area(s: Shape): double => (case s of
    Circle(var r)           => Math.Pi * r * r,
    Rectangle(var w, var h) => w * h,
    Dot                     => 0.0);

  public static method Main()
    var shapes: Array<Shape> := [Shape.Circle(1.0), Shape.Rectangle(2.0, 3.0),
                                 Shape.Dot, Shape.Rectangle(4.0, 4.0)];

    // A function value; the lambda's parameter type comes from the declared type
    var describe: (Shape) -> string := lambda(s) => (case s of
      Circle(var r)           => $"circle of radius {r}",
      Rectangle(var w, var h) => (if w = h then $"square of side {w}" else $"rectangle {w} by {h}"),
      Dot                     => "dot");

    for var s in shapes do
      Console.WriteLine($"{describe(s)}: area {Area(s):F2}");
    end;

    var threshold := 5.0;                            // captured by the first lambda below
    var large := shapes.Where(lambda(s) => Area(s) > threshold).Count();
    var total := shapes.Select(lambda(s) => Area(s)).Sum();
    Console.WriteLine($"{large} of {shapes.Count} shapes exceed {threshold}; total area {total:F2}");
  end Main;
end Program;
```

Adding a case to `Shape` makes both `case` expressions fail to compile until they handle it. [A Tour of Bezel](docs/Bezel_Tour.md) walks through the rest of the language.

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
