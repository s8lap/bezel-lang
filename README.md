# Bezel

[![Latest pre-release](https://img.shields.io/github/v/release/s8lap/bezel-lang?include_prereleases&label=pre-release)](https://github.com/s8lap/bezel-lang/releases) [![Documentation](https://img.shields.io/badge/docs-s8lap.github.io%2Fbezel--lang-blue)](https://s8lap.github.io/bezel-lang/) [![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-green)](LICENSE)

**A compiled language for services and tools, with ownership-based memory management and no garbage collector.**

Bezel is built for **predictable performance and few surprises**. What a program costs should follow from what its source says, and what it does should not depend on rules the reader cannot see.

Memory is where that matters most. Manual memory management is easy to get wrong, and garbage collection and automatic reference counting cost time and memory at run time. Bezel takes a third route: the compiler knows, for every object, who owns it and the exact point at which it is released, and inserts the release itself, on the normal path and when an exception passes through. The effect is close to C++'s RAII, except that the compiler checks the rules instead of leaving them to discipline. There is no collector, no pause and no hidden reference count on the classes you write, and the guarantees hold in multi-threaded programs too.

The same aim runs through the rest of the language. Types are non-nullable unless written with `?`, integer arithmetic is checked for overflow, arithmetic on a possibly missing value is a compile error, and nothing converts implicitly between your own types.

## Status: experimental, expect things to break

**The compiler is not reliable yet: expect arbitrary things to break at any moment while you write Bezel code.** The language design is close to settled and written down in full, and the compiler is still being brought up to it. Known bugs are tracked in the [issue tracker](https://github.com/s8lap/bezel-lang/issues). The language may still change, and there are no stability guarantees. The compiler runs on Linux x86-64 only for now. If something breaks, please [report it](#reporting-a-bug).

## A first look

Bezel's syntax comes from the Pascal family: blocks close with `end`, assignment is `:=` and equality is `=`. Its type system is close to C#'s, and its standard library borrows many .NET names, but the resemblance is on the surface: the library is its own design, built around ownership.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/readme-example-dark.svg">
  <img alt="A Bezel program: a shop with Product and Order records, an Outcome choice with Charged, UnknownSku and OutOfStock cases, an Inventory class that owns a dictionary of products, and a Main method that places orders and prints each outcome." src="docs/images/readme-example-light.svg">
</picture>

<details>
<summary>The same program as text</summary>

<!-- readme-example: editors/vscode/tools/readme-example.js renders this block as docs/images/readme-example-{light,dark}.svg -->
```bezel
module Shop;

public record Product(Sku: string, Price: decimal, Stock: int);
public record Order(Sku: string, Qty: int);

public choice Outcome
    Charged(amount: decimal),
    UnknownSku(sku: string),
    OutOfStock(left: int);
end Outcome;

public class Inventory(products: Array<Product>)
    private owned _bySku: Dictionary<string, Product> :=
        products.ToDictionary(lambda(p) => p.Sku);
    public method Place(order: Order): Outcome
        var p := _bySku.GetOrNull(order.Sku);
        if p = null then
            return UnknownSku(order.Sku);
        elif p.Stock < order.Qty then
            return OutOfStock(p.Stock);
        end;
        _bySku[p.Sku] := p with(Stock := p.Stock - order.Qty);  // an updated copy
        return Charged(p.Price * order.Qty);
    end Place;

    // A fresh array, so the return type says `owned`: the caller now holds it.
    public method LowStock(below: int): owned Array<string> =>
        _bySku.Where(lambda(e) => e.Value.Stock < below).Select(lambda(e) => e.Key)
            .ToArray();
end Inventory;

public class Program
    public static method Main()
        using var shop := new Inventory([
            new Product("pen", 1.25m, 40),
            new Product("pad", 3.50m, 6),
            new Product("mug", 8.00m, 3)
        ]);
        var orders: Array<Order> :=
            [new Order("pen", 10), new Order("ink", 1), new Order("pad", 7)];
        for var order in orders do
            var verdict := (case shop.Place(order) of
                Charged(var amount) => $"charged {amount:F2}",
                UnknownSku(var sku) => $"refused, no product '{sku}'",
                OutOfStock(var left) => $"refused, only {left} left");
            Console.WriteLine($"{order.Qty} x {order.Sku}: {verdict}");
        end;
        Console.WriteLine($"running low: {String.Join(", ", shop.LowStock(10))}");
    end Main;
end Program;
```

</details>

It prints `10 x pen: charged 12.50`, `1 x ink: refused, no product 'ink'`, `7 x pad: refused, only 6 left` and `running low: pad, mug`. Adding a case to `Outcome` makes the `case` expression in `Main` fail to compile until it handles the new case. [A Tour of Bezel](https://s8lap.github.io/bezel-lang/tour/) walks through the rest of the language.

**Try it:** a prebuilt compiler for Linux x86-64 is on the [releases page](https://github.com/s8lap/bezel-lang/releases). It is a single static download with its own linker, so nothing else needs to be installed ([Getting started](#getting-started)).

## Why Bezel

- **Memory safety without a collector.** Every object has one owner and is disposed exactly once. Using a moved-out value or keeping a borrow past its owner is a compile-time error; where a stale reference can still arise at run time, the default build throws instead of corrupting memory.
- **Safe across threads.** Execution domains share no mutable state, so the guarantees hold in multi-threaded code and data races are ruled out by construction.
- **Non-null by default.** A `Connection` always holds an object; only a `Connection?` may be `null`, and calling a member on it is a compile error until a check rules `null` out ([§7](https://s8lap.github.io/bezel-lang/spec/08-7-nullability.html#sec-nullability-2)). Missing values are handled in the open with `?.`, `??` and `!`.
- **Predictable costs and behaviour.** Objects are released at points you can find in the source. Integer overflow throws instead of wrapping ([§6.1](https://s8lap.github.io/bezel-lang/spec/07-6-arithmetic-and-overflow.html#sec-integer-arithmetic)), arithmetic on a nullable value is a compile error ([§7.11](https://s8lap.github.io/bezel-lang/spec/08-7-nullability.html#sec-lifted-operators)), and user types cannot declare conversion operators ([§25.4](https://s8lap.github.io/bezel-lang/spec/26-25-operator-overloading.html#sec-no-user-defined-conversion-operators)).
- **Compared with Rust.** The ownership model is close to Rust's, but there are no lifetime annotations and no single-writer rule: freedom from data races comes from domains, not from restricting aliases. The price is that a small class of mistakes Rust rejects at compile time shows up here as a deterministic run-time exception ([rationale §1](https://s8lap.github.io/bezel-lang/rationale/02-1-the-big-picture.html#sec-the-big-picture)).
- **Familiar everywhere else.** Classes, interfaces, generics, records, enums, sum types with exhaustive matching, `async`/`await` with structured concurrency, LINQ-style queries, exceptions and contracts.
- **A specification first.** Every rule is pinned by a test that states the specification's verdict, not the compiler's.

## How memory management works

The rules are in [chapter 28 of the specification](https://s8lap.github.io/bezel-lang/spec/29-28-resource-management.html#sec-resource-management). Most of their weight falls on the compiler; the programmer meets three keywords, plus `using var` for a local disposed at the end of its block:

- **`owned`** marks a slot that holds ownership: a parameter that takes it, a return type that hands it to the caller, or a field.
- **`move`** transfers ownership out of a named variable. Where the destination itself announces the transfer, such as a `return` or a store into an `owned` field, no marker is needed, so every transfer is visible in the source.
- **`scoped`** lets a method keep a callback (or a borrowed object) beyond the call, for as long as the object that stores it lives. A plain parameter may only be used during the call; with `scoped`, the compiler checks at the call site that everything the callback captures outlives the object keeping it ([§28.14](https://s8lap.github.io/bezel-lang/spec/29-28-resource-management.html#sec-scoped-callback-parameters); [Guide 1](https://s8lap.github.io/bezel-lang/guide-01/) has a worked example).

A local initialized with a new object owns it. Every other reference is a **borrow**, and the compiler checks that no borrow outlives what it borrows. Only the owner may dispose an object ([§28.6](https://s8lap.github.io/bezel-lang/spec/29-28-resource-management.html#sec-who-may-dispose)): `Dispose()` runs its cleanup and marks it disposed, and the memory is released by code the compiler inserts at the owner's release point, such as the end of a scope. Using an object after it has been disposed throws `DisposedAccessException` in the default build instead of reading freed memory.

**Threads.** Code runs in **execution domains** ([§30.13](https://s8lap.github.io/bezel-lang/spec/31-30-asynchronous-programming.html#sec-runtime-model)). A domain runs one piece of code at a time; domains run in parallel but share no mutable state, and a value passes between them only by being moved or as an immutable copy ([§30.14](https://s8lap.github.io/bezel-lang/spec/31-30-asynchronous-programming.html#sec-multiple-domains)). That keeps the memory guarantees in multi-threaded programs and rules out data races, with one documented limit: an alias the compiler cannot see, made before the move, is the programmer's responsibility.

**Reference counting.** No class you write is ever reference-counted. The runtime manages strings, boxed values and closures itself, and the current one counts their references; it also counts the cells of variables captured by a lambda whose lifetime the compiler cannot bound statically. Shared ownership is opt-in, with `Shared<T>` and `Weak<T>` ([§28.9](https://s8lap.github.io/bezel-lang/spec/29-28-resource-management.html#sec-runtime-managed-types), [§28.13](https://s8lap.github.io/bezel-lang/spec/29-28-resource-management.html#sec-shared-ownership)).

## Getting started

**Prebuilt compiler.** Download the archive for the latest build from the [releases page](https://github.com/s8lap/bezel-lang/releases) (Linux x86-64; builds are pre-releases for now). `bin/bezelc` is fully static and links programs with the toolchain bundled beside it, and the programs it produces are fully static too, so they run on any x86-64 Linux. The archive's own `README.md` covers the options, such as `--link=system` for AddressSanitizer builds.

```sh
tar xzf bezelc-<version>-linux-x86_64.tar.gz
export PATH="$PWD/bezelc-<version>-linux-x86_64/bin:$PATH"
```

**From source.** Bezel builds on Linux with clang and LLVM 21. It is developed and tested on Fedora; other platforms are untested ([#446](https://github.com/s8lap/bezel-lang/issues/446)).

```sh
sudo dnf install cmake clang llvm llvm-devel      # LLVM 21
cmake -B build -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j$(nproc)                    # the compiler is build/bezelc
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
bezelc hello.bzl -o hello && ./hello
```

`Main`'s return value becomes the exit code. Libraries compile separately and are imported by their object file: `bezelc -c lib.bzl -o lib.o`, then `bezelc app.bzl --import lib.o -o app`.

## Learn the language

| Start with | What it is |
|---|---|
| [A Tour of Bezel](https://s8lap.github.io/bezel-lang/tour/) | the language in half an hour: syntax, ownership, errors, collections, concurrency, contracts |
| [The guides](https://s8lap.github.io/bezel-lang/guide-01/) | short, example-first: memory and ownership, errors and nullability, modelling data, concurrency, collections, contracts, style |
| [Writing reference](https://s8lap.github.io/bezel-lang/writing/) | the whole syntax, the ownership rules as a checklist, every library name with its signature, and the shapes that look right but are rejected |
| [Standard library](https://s8lap.github.io/bezel-lang/stdlib/) | every type and member; if a name is not there, it does not exist |
| [Language specification](https://s8lap.github.io/bezel-lang/spec/) | the normative reference, with the full grammar and the compiler-enforced invariants |
| [Design rationale](https://s8lap.github.io/bezel-lang/rationale/) | why each decision was made, and what was declined |
| [Design proposals](docs/proposals/README.md) | how the language was argued into shape: about 190 proposals, accepted, declined and pending, each with the alternatives it weighed |

Apart from the proposals, these are published together as a searchable site, one page per chapter, at **[s8lap.github.io/bezel-lang](https://s8lap.github.io/bezel-lang/)**. The site is rebuilt from `docs/` by `scripts/build_book.py` whenever the documents change on main.

**Editor support:** [`editors/vscode/`](editors/vscode/) is a VS Code extension with syntax highlighting, editing rules and snippets. Its grammar is generated from the specification.

## How far along is it

- **The compiler**, `bezelc`, is an ahead-of-time front end and LLVM back end. It compiles the language's core end to end — ownership and borrow checking, classes and generics, records, enums and choices, exceptions, collections and queries, `async`/`await`, multi-module builds — and much of the standard library.
- **The gap** is measured. A rule corpus of one hand-written case per specification rule ([`tests/conformance/rules/`](tests/conformance/rules/)) records where the compiler still differs: of the 3,085 cases with a recorded result, 2,873 match the specification. Every difference has a tracking issue.

## Reporting a bug

Bug reports are very welcome. Please [open an issue](https://github.com/s8lap/bezel-lang/issues/new/choose) with:

- the compiler version (`bezelc --version`, or the release you downloaded);
- the smallest `.bzl` program that shows the problem;
- what the compiler or the program did, and what you expected.

Questions about what the specification means are welcome too. A crash, a leak or a memory error in a compiled program is always a bug.

## Contributing and testing

The project works specification first: if the compiler and the specification disagree, the compiler is wrong, and every fix ships a regression test that cites the section it enforces. [`CONTRIBUTING.md`](CONTRIBUTING.md) explains how to build, test and send a change, [`tests/README.md`](tests/README.md) how to write a test, and [`docs/proposals/spec-change-process.md`](docs/proposals/spec-change-process.md) how the language itself changes. [`CLAUDE.md`](CLAUDE.md) holds the stricter working rules the project's AI agents follow.

```sh
tests/run_parallel.sh 16                    # the behaviour suite, sharded
cmake --build build --target check-lit      # the diagnostic suite (needs lit and FileCheck)
scripts/check_docs.sh                       # the documentation checks
```

## Acknowledgements

Bezel is built with [Claude](https://www.anthropic.com/claude), Anthropic's AI model, which writes most of the specification, compiler and tests.

## License

Apache 2.0. See [`LICENSE`](LICENSE).
