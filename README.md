# Bezel

**A compiled language for services and tools, with ownership-based memory management and no garbage collector.**

Manual memory management is easy to get wrong, and garbage collection and automatic reference counting cost time and memory at run time. Bezel takes a third route. The compiler knows, for every object, where it is created, who owns it, and the exact point at which it is released, and it inserts the release itself, on the normal path and when an exception passes through. The effect is close to C++'s RAII, except that the compiler checks the rules instead of leaving them to discipline. There is no collector and no hidden reference count on any class you write (only the opt-in `Shared<T>` counts), and the guarantees hold in multi-threaded programs too.

## Status: experimental, expect things to break

**The compiler is not reliable yet: expect arbitrary things to break at any moment while you write Bezel code.** The language design is close to settled and written down in full, and the compiler is still being brought up to it. Known bugs are tracked in the [issue tracker](https://github.com/s8lap/bezel-lang/issues). The language may still change, and there are no stability guarantees.

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

## How memory management works

The rules are in [chapter 28 of the specification](https://s8lap.github.io/bezel-lang/spec/29-28-resource-management.html#sec-resource-management). They are long because they are precise, but most of their weight falls on the compiler, which tracks ownership through every statement. The programmer meets three ownership keywords, plus `using var` for a local disposed at the end of its block:

- **`owned`** marks a slot that holds ownership: a parameter that takes it, a return type that hands it to the caller, or a field that holds it until it is replaced or its object is disposed.
- **`move`** transfers ownership out of a named variable at a use site, for example into an `owned` parameter. Where the destination itself announces the transfer, such as a `return` or a store into an `owned` field, no marker is written. Either way, every transfer is visible in the source.
- **`scoped`** lets a method keep something it was given, typically a callback, beyond the call. See below.

A local initialized with a new object (`new`, or a call whose return type is `owned`) owns it. Every other reference is a **borrow**: a reference passed around without transferring ownership. The compiler checks that no borrow outlives what it borrows, so a dangling reference is a compile-time error.

**What `scoped` is for.** A lambda is only safe while everything it captured is still alive. By default, a method that receives a lambda may call it but may not keep it: storing it in a field, in a collection or in a returned closure is a compile error, because the method's signature says nothing about how long the captured values live. Declaring the parameter `scoped` says *"I will keep this for as long as the object that stores it lives"*, and the compiler then checks the caller's side:

```bezel
class Timer
  private scoped _onTick: (() -> ())?;              // a scoped field: it can be invoked, replaced or cleared, never read out
  public method OnTick(scoped handler: () -> ())
    _onTick := handler;                              // allowed: stored in the object that received it
  end OnTick;
  public method Tick()
    _onTick?();                                      // call the handler if one is set
  end Tick;
end Timer;

using var conn := new Connection("db");              // declared before the timer
using var timer := new Timer();
timer.OnTick(lambda() => conn.Send("ping"));         // the lambda borrows conn; timer stores the lambda
timer.Tick();
// while timer lives, conn cannot be moved, reassigned or disposed
```

The call is accepted because `conn` is declared before `timer`, so it is disposed after `timer` at the end of the block. If the two `using var` lines are swapped, the call is rejected and the message names `conn`. If `timer` is a plain `var` with no fixed end, only lambdas that capture nothing are accepted. The same mechanism lets an object keep a borrowed object rather than a lambda (`new Statement(scoped account)`), and the standard library uses it for event subscriptions and for `TaskScope.Spawn` ([§28.14](https://s8lap.github.io/bezel-lang/spec/29-28-resource-management.html#sec-scoped-callback-parameters); [Guide 1](https://s8lap.github.io/bezel-lang/guide-01/) has more).

Only an object's owner may dispose it ([§28.6](https://s8lap.github.io/bezel-lang/spec/29-28-resource-management.html#sec-who-may-dispose)). Calling `Dispose()` runs the object's cleanup, its destructor, and marks it disposed. It does not free the memory. The memory is always released by code the compiler inserts at the owner's release point: the end of a scope, the overwrite of a field, the disposal of the object that holds it. Using an object after it has been disposed raises `DisposedAccessException` in the default build instead of reading freed memory.

**Threads.** The guarantees hold in multi-threaded programs, through **execution domains** ([§30.13](https://s8lap.github.io/bezel-lang/spec/31-30-asynchronous-programming.html#sec-runtime-model), [§30.14](https://s8lap.github.io/bezel-lang/spec/31-30-asynchronous-programming.html#sec-multiple-domains)). Code inside a domain runs one piece at a time, so it never races with itself, and domains run in parallel on separate threads but share no mutable state. A value crosses from one domain to another only by being moved, so the sender statically loses access to it, or as an immutable value such as a string or a copy. Only *sendable* types may cross at all, and the compiler checks every crossing. That rules out data races by construction, with one documented limit: an alias made before the move and kept out of the compiler's sight, such as a borrow stored in another object's field or held by a pending task, is the programmer's responsibility.

**Where reference counting is used.** No class you write is ever reference-counted. The specification lets the runtime manage four kinds of value by whatever mechanism it chooses: strings, `Type` tokens, boxed values and function values ([§28.9](https://s8lap.github.io/bezel-lang/spec/29-28-resource-management.html#sec-runtime-managed-types)). It asks for counting only where a program opts in, with `Shared<T>` and `Weak<T>` ([§28.13](https://s8lap.github.io/bezel-lang/spec/29-28-resource-management.html#sec-shared-ownership)). The current compiler and runtime count references in these places:

- strings, boxed values and closures (`Type` tokens are interned and never freed);
- the cell holding a variable that a lambda captures, but only when the compiler cannot prove where the lambda's environment dies, for example when a capturing lambda is passed to another method. The last holder frees the cell;
- `Shared<T>` and `Weak<T>` handles;
- runtime internals that never appear as user objects: tasks, cancellation token sources, and the shared cores of `Semaphore` and the atomics.

## Why Bezel

- **Memory safety without a collector, across threads too.** Every object has exactly one owner and is disposed exactly once, and execution domains keep those guarantees in multi-threaded code. Using a moved-out value and keeping a borrow past its owner are compile-time errors; a resource is never dropped without being disposed; where a stale reference can still arise at run time, the default build fails with an exception instead of corrupting memory.
- **No null surprises.** A class type is non-nullable unless it is written with `?`: a `Connection` always holds an object, a `Connection?` may hold `null` ([§7](https://s8lap.github.io/bezel-lang/spec/08-7-nullability.html#sec-nullability-2)). The compiler enforces the difference at every assignment, argument and return, and calling a member on a `Connection?` is a compile error until a check such as `if c <> null then` has narrowed it. A non-nullable field must be initialized where it is declared or by every constructor ([§7.9](https://s8lap.github.io/bezel-lang/spec/08-7-nullability.html#sec-definite-assignment-for-non-nullable-fields)). Absence is handled explicitly with `?.`, `??` and `??=`, and `!` asserts presence, raising `NullReferenceException` if the value is null.
- **Ownership you can read.** The declaration says who owns a slot (`owned` on a parameter, a return type or a field); the use site, or the `owned` slot it stores into, says when ownership moves (`move`). An assignment never transfers anything silently.
- **Familiar everywhere else.** Classes, interfaces, generics, records, enums, sum types (`choice`) with exhaustive matching, `async`/`await` with structured concurrency, LINQ-style queries, exceptions, and contracts (`requires`/`ensures`) — in a syntax that reads like the Pascal family with the type system of C#.
- **A specification first.** The language is defined by a normative specification, and every rule is pinned by a test that states the specification's verdict, not the compiler's.

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

- **The design** is written down in full: the [specification](https://s8lap.github.io/bezel-lang/spec/), the [standard library](https://s8lap.github.io/bezel-lang/stdlib/) and the [rationale](https://s8lap.github.io/bezel-lang/rationale/), with one [changelog](https://s8lap.github.io/bezel-lang/changelog/) entry per revision.
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

## Acknowledgements

Bezel was built with [Claude](https://www.anthropic.com/claude), Anthropic's AI model, which wrote most of the specification, compiler and tests.

## License

Apache 2.0. See [`LICENSE`](LICENSE).
