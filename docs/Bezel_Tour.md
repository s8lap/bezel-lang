# A Tour of Bezel

*Validated against v0.338. An introduction for programmers who know C#, Java, Go, Rust, Swift or TypeScript; not a normative document — where it and the [specification](Bezel_Language_Spec.md) disagree, the specification wins.*

Bezel is a compiled language for services and tools. It has no garbage collector. Instead, the compiler tracks who owns every heap object and inserts the code that frees it, at a point it knows statically. You get the predictable memory use of manual management without writing a single `free`, `Dispose` or `defer`.

This tour takes about half an hour. It shows the syntax first, then the ownership model, which is the part of Bezel that is new, then errors, collections, concurrency and contracts. Every example is a complete program or a fragment of one.

---

## 1. Hello, Bezel

```bezel
module Hello;

public class Program
  public static method Main(): int
    Console.WriteLine("Hello, Bezel!");
    return 0;
  end Main;
end Program;
```

```sh
bezelc hello.bzl -o hello && ./hello
```

A few things are visible already:

- Every file starts with `module Name;`. Imports come next (`import System.IO;`); the common modules — `System`, `System.Text`, the collections and the query operators — are imported automatically.
- Blocks close with `end`, optionally followed by the name of what they close: `end Main;`, `end Program;`, or just `end;`. There are no braces.
- Every statement ends with `;`. Members state their visibility.
- `Main` returns the process exit code. It can also take `args: Array<string>`, return nothing, or be `async`.

## 2. The basics

Bezel reads like a member of the Pascal family with the type system of C#. The habits that trip people up most are few, and the compiler catches every one:

| You might write | In Bezel |
|---|---|
| `x = 1;` | `x := 1;` — `=` is equality |
| `a == b`, `a != b` | `a = b`, `a <> b` |
| `&&`, `\|\|`, `!x`, `a % b` | `and`, `or`, `not x`, `a mod b` |
| `else if` | `elif` |
| `cond ? a : b` | `(if cond then a else b)` |
| `x => x * 2` | `lambda(x) => x * 2` |
| `(int)d` | `int(d)` |
| `void M()` | `method M()` — leave the return type out |

```bezel
module Basics;

public class Program
  public static method Classify(n: int): string
    if n < 0 then
      return "negative";
    elif n = 0 then
      return "zero";
    else
      return "positive";
    end;
  end Classify;

  public static method Main()
    var total := 0;                            // the type is inferred: int
    for var i in 1..10 do                      // 1 to 10 inclusive; 0..<10 excludes the end
      total += i;
    end;
    Console.WriteLine($"sum = {total}, {Classify(total)}");

    var numbers: Array<int> := [3, 8, 1, 6, 4];
    var evens := numbers.Where(lambda(n) => n mod 2 = 0).ToArray();
    Console.WriteLine($"{evens.Count} even numbers");

    var label := (if total > 50 then "big" else "small");
    Console.WriteLine(label);
  end Main;
end Program;
```

`var` declares a local; `const` declares one that is never reassigned. Strings interpolate with `$"…{expr}…"` — there is no `String.Format` and no implicit conversion in `+`. A collection literal such as `[3, 8, 1, 6, 4]` takes its type from where it goes, so the local is declared with one.

`case` matches values, ranges and shapes, and works as a statement or an expression:

```bezel
var grade := (case score of
  90..100 => "A",
  80..<90 => "B",
  _       => "C or below");
```

## 3. Types that are values

Classes are reference types. Records, enums and choices are **value types**: they are copied, compared by value, and never need disposing.

```bezel
record Point(X: int, Y: int);

enum Priority
  Low, Medium, High;
end Priority;

public choice Shape
  Circle(radius: double),
  Rectangle(width: double, height: double);
end Shape;
```

A record gets value equality, hashing and `ToString` for free, and `with` makes a modified copy:

```bezel
var p := new Point(1, 2);
var q := p with(X := 5);           // Point(5, 2); p is unchanged
Console.WriteLine(p = q);          // False
```

A **choice** is a sum type: a value is exactly one of its cases, each with its own fields. A `case` over a choice must cover every case — the compiler checks — so adding a case later finds every place that has to handle it:

```bezel
public static method Area(s: Shape): double
  return (case s of
    Circle(var r)       => 3.14159 * r * r,
    Rectangle(var w, var h) => w * h);
end Area;
```

The standard library's `Result<T, E>` is a choice too, and is how a function reports an expected failure without throwing:

```bezel
public static method ParsePort(text: string): Result<int, string>
  if not int.TryParse(text, out var value) then
    return Error($"not a number: {text}");
  end;
  if not (value in 1..65535) then
    return Error($"out of range: {value}");
  end;
  return Ok(value);
end ParsePort;
```

## 4. Ownership in ten minutes

This is the part of Bezel that is new. Read it slowly once; after that it is mostly the compiler's job.

### Owned and borrowed

Every class-typed value is either **owned** or **borrowed** at every point in the program:

- an **owned** value is yours to dispose — exactly once, on every path;
- a **borrowed** value is someone else's; you may use it while its owner keeps it alive, and you may not dispose it.

You almost never dispose anything by hand. An owned value that reaches the end of its scope is disposed there automatically — on the normal path *and* when an exception passes through. The one question to ask of every object is: *am I responsible for this one?*

```bezel
module Pool;

public class Connection(host: string)
  public readonly property Host: string := host;
  public method Send(text: string)
    Console.WriteLine($"{Host} <- {text}");
  end Send;
end Connection;

public class Program
  // A method that creates an object says so: `owned` in the return type.
  public static method Connect(host: string): owned Connection
    return new Connection(host);
  end Connect;

  public static method Main()
    using var db := Connect("db");     // owned; disposed at the end of Main, on every path
    var alias := db;                   // a BORROW of db: nothing is copied or transferred
    alias.Send("hello");
  end Main;                            // db is disposed here; alias needs nothing
end Program;
```

`using var` says "this local owns its value and disposes it at the end of the block". A plain `var` holding an owned value is disposed at the end of the block too, but the compiler warns, because a resource dropped without a word is usually a mistake.

### Transfers are written down

Assigning a class-typed value never transfers it: `var alias := db;` is a borrow. To hand ownership to someone else, you write `move` at the place it happens, and the receiving parameter says `owned`:

```bezel
public static method Retire(owned c: Connection)
  c.Send("goodbye");
end Retire;                            // c is disposed here: Retire owns it

public static method Main()
  var c := Connect("cache");
  Retire(move c);                      // ownership leaves here
  c.Send("still there?");              // ERROR: c was moved out
end Main;
```

The rules are deliberately symmetric. The *declaration* says who owns a slot (`owned` on a parameter, a return type or a field); the *use site* says when a transfer happens (`move`). Where the target already makes the transfer obvious — `return x;` from an `owned` method, `throw x;`, storing into an `owned` field — no `move` is written, and writing one is an error.

### Signatures carry the facts

The compiler reads ownership off the declared types, never off what a method happens to do:

```bezel
public class Registry
  private owned _main: Connection := new Connection("main");

  public method Primary(): Connection => _main;                  // a borrow of the receiver
  public method Fresh(): owned Connection => new Connection("x"); // a new object for the caller
end Registry;
```

A method returning a plain class type lends something the receiver owns; one returning `owned T` hands over a new object. So a call site always knows what it got — and a static method that returns a class type must say `owned`, because there is no receiver to lend from.

### Objects that own objects

A field marked `owned` is disposed with its container, in reverse declaration order, without any code from you. Most classes that hold resources need no destructor at all:

```bezel
public class Customer(name: string)
  public readonly property Name: string := name;
end Customer;

public class Session(owner: Customer)
  private owned _primary: Connection := new Connection("primary");
  private owned _replica: Connection := new Connection("replica");
  private _owner: Customer := owner;      // not owned: a back-reference to someone else's object
end Session;
```

When a `Session` is disposed, `_replica` and then `_primary` are disposed; `_owner` is left alone.

### Borrows cannot outlive their owners

A borrow is valid only while its source lives, and the compiler checks this when you store or return it, not at some later crash:

```bezel
public class Cache
  private _last: Connection? := null;

  public method Remember(c: Connection)
    _last := c;                       // fine: the caller's object outlives this call
  end Remember;

  public method Broken()
    using var temp := new Connection("temp");
    _last := temp;                    // ERROR: temp is disposed at the end of Broken
  end Broken;
end Cache;
```

That is the whole model: one owner, borrows that cannot outlive it, transfers written at the site. The guide [Memory and Ownership](Bezel_Guide_01_Memory_and_Ownership.md) goes further, and the specification's §28 is the full rulebook.

### What you get in exchange

- No collector, no pauses, no reference-counting cycles: memory is freed at a known point.
- No `Dispose`/`finally`/`defer` plumbing, and no resource leak on an exception path.
- Using a moved-out value, disposing twice, letting a borrow outlive its owner and dropping a resource without a word are caught at compile time. Where a stale reference can still arise at run time — a borrow of an element that its owning collection has since removed, for example — the default build fails with a `DisposedAccessException` instead of corrupting memory.

## 5. Collections

Collections come in two families, and the name says which:

| Family | Types | Holds |
|---|---|---|
| Borrowing | `Array<T>`, `Dictionary<K, V>`, `Set<T>`, `Stack<T>`, `Queue<T>`, … | borrows: the elements belong to someone else |
| Owning | `OwningArray<T>`, `OwningDictionary<K, V>`, `OwningSet<T>`, … | the elements themselves: removing one disposes it |

```bezel
public static method BuildPool(): owned OwningArray<Connection>
  var pool := new OwningArray<Connection>();
  pool.Add(new Connection("a"));            // the collection takes ownership
  pool.Add(Connect("b"));
  return pool;
end BuildPool;

public static method Shutdown()
  using var pool := BuildPool();
  for var c in pool do                      // each c is a borrow of a slot
    Console.WriteLine(c.Host);
  end;
  for var c in pool.Drain() do              // each c is taken out: it is yours
    using var live := move c;
    live.Send("shutdown");
  end;                                      // pool is empty now, and still disposed at the end
end Shutdown;
```

Value elements — numbers, strings, records — behave the same in both families. The distinction matters only for objects.

## 6. Errors and nullability

**Null is in the type.** A `string` is never null; a `string?` may be. The compiler narrows a local after a check:

```bezel
public static method Greet(name: string?)
  if name <> null then
    Console.WriteLine($"{name} has {name.Length} letters");
  end;
  var who := name ?? "stranger";
  Console.WriteLine($"Hello, {who}!");
end Greet;
```

`?.` reads through a possibly-null value, `??` supplies a default, and a postfix `!` asserts non-null.

**Exceptions** are for the unexpected. A `catch` has comma-separated arms, matched in order:

```bezel
try
  Process(path);
catch
  var ex: FileNotFoundException => Console.WriteLine($"missing: {ex.Message}"),
  var ex: IOException           => Console.WriteLine($"I/O failed: {ex.Message}")
finally
  Console.WriteLine("done");
end;
```

For failures a caller is expected to handle — bad input, a missing key — the standard library uses a `Try…` method or a `…OrNull` variant, or returns `Result<T, E>`, rather than throwing. The guide [Errors and Nullability](Bezel_Guide_02_Errors_and_Nullability.md) says when to use which.

## 7. Concurrency

`async` methods and `await` work much as in C#:

```bezel
module Notes;

import System.IO;

public class Program
  public static async method Main(): Task<int>
    var text := await File.ReadAllTextAsync("notes.txt");
    Console.WriteLine($"{text.Length} characters");
    return 0;
  end Main;
end Program;
```

Concurrent work is **structured**: a `TaskScope` owns the tasks it spawns; when one fails it cancels the others, and `await scope.Join()` waits for all of them and re-raises the first failure.

```bezel
import System.Threading;

public class Refresher
  private static async method RefreshOne(name: string, token: CancellationToken): Task
    await Task.Delay(TimeSpan.FromMilliseconds(10), token);
    Console.WriteLine($"refreshed {name}");
  end RefreshOne;

  public async method RefreshBoth(primary: string, mirror: string): Task
    await using var scope := new TaskScope();
    scope.Spawn(async lambda() => do await RefreshOne(primary, scope.Token); end);
    scope.Spawn(async lambda() => do await RefreshOne(mirror, scope.Token); end);
    await scope.Join();
  end RefreshBoth;
end Refresher;
```

Code runs on **domains**. A domain runs one piece of work at a time; different domains run in parallel on different threads. Values cross between domains — as spawn arguments, results or channel messages — only if their type is *sendable*, which the compiler checks, so mutable state is never reachable from two domains at once. The guide [Concurrency](Bezel_Guide_04_Concurrency.md) covers domains, channels and cancellation.

## 8. Contracts

A method can state what it needs and promises. Clauses sit between the signature and the body:

```bezel
public class Account
  private _balance: decimal := 0m;

  public method Withdraw(amount: decimal)
    requires(amount > 0m)
    requires(amount <= _balance)
    ensures(_balance = old(_balance) - amount)

    _balance := _balance - amount;
  end Withdraw;
end Account;
```

`requires` is checked in every build; `ensures` and class `invariant`s are checked in the checked-contracts build. A violation raises `ContractViolationException`. Contracts may call only `pure` members — methods that the compiler verifies have no side effects. See [Contracts and Purity](Bezel_Guide_06_Contracts_and_Purity.md).

## 9. Where to go next

- **Guides** — short and example-first: [Memory and Ownership](Bezel_Guide_01_Memory_and_Ownership.md), [Errors and Nullability](Bezel_Guide_02_Errors_and_Nullability.md), [Modeling Data](Bezel_Guide_03_Modeling_Data.md), [Concurrency](Bezel_Guide_04_Concurrency.md), [Collections and Iteration](Bezel_Guide_05_Collections_and_Iteration.md), [Contracts and Purity](Bezel_Guide_06_Contracts_and_Purity.md), [Style](Bezel_Guide_07_Style.md).
- **[Writing reference](Bezel_For_Code_Generation.md)** — the whole syntax, the ownership rules as a checklist, every standard-library name with its exact signature, and a long list of shapes that look right and are rejected. Keep it open while you write.
- **[Standard library](Bezel_Standard_Library.md)** — every type and member. If a name is not there, it does not exist.
- **[Language specification](Bezel_Language_Spec.md)** — the normative reference, with the full grammar (Appendix A) and the compiler-enforced invariants (Appendix C).
- **[Design rationale](Bezel_Design_Rationale.md)** — why each decision was made, and which alternatives were declined.
