# Bezel Standard Library Specification

**Companion to the Bezel Language Specification, v0.193 — July 2026**

This document specifies the standard library types referenced by the language specification. The language spec assumes these types exist and have certain shapes; this document defines those shapes.

The library is organized into modules. Some are auto-imported in every file; others require explicit `import`. See [§2.3](Bezel_Language_Spec.md#sec-always-available-names) of the language spec for the auto-import list.

<a id="sec-contents"></a>
## Contents

1. [Conventions](#1-conventions)
2. [`System` (auto-imported)](#2-system-auto-imported)
3. [`System.Text` (auto-imported)](#3-systemtext-auto-imported)
4. [`System.Collections.Generic`](#4-systemcollectionsgeneric)
5. [`System.Collections.Linq`](#5-systemcollectionslinq)
6. [`System.Threading`](#6-systemthreading)
7. [`System.IO`](#7-systemio)
8. [`System.Diagnostics`](#8-systemdiagnostics)
9. [Primitive type members](#9-primitive-type-members)
10. [FFI-boundary types](#10-ffi-boundary-types)
11. [Modules deferred to future revisions](#11-modules-deferred-to-future-revisions)

---

<a id="sec-conventions"></a>
## 1. Conventions

Throughout this document:

- Method signatures use Bezel syntax. Modifier order follows [§11.5](Bezel_Language_Spec.md#sec-modifier-order) of the language spec.
- A method without a `:` clause returns `()`.
- All listed members are `public` unless otherwise noted.
- Every class implicitly implements `IDisposable`. A class whose disposal does meaningful work (releases a native resource, removes a registration, fires pending callbacks) is shown with a `destructor;` declaration in its body, followed by a one-line comment describing the disposal effect; the destructor body itself is provided by the runtime or implementation. This `destructor;` form is a **documentation shorthand** used in this document only; user code requires a destructor body per language spec [§28.4](Bezel_Language_Spec.md#sec-the-destructor) (the form `destructor /* statements */ end;`). A class whose `Dispose` is a no-op (memory-only state, no registry, no native handle) shows no destructor — readers should infer the no-op behavior from absence.
- Implementations may add internal helper methods and private members. Anything documented here is part of the public contract.
- Methods returning freshly-allocated class instances annotate the return type with `owned` to communicate the caller's disposal obligation (see [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) and [§28.8](Bezel_Language_Spec.md#sec-ownership-transfer) of the language spec). Methods returning `string` or other runtime-managed types ([§28.9](Bezel_Language_Spec.md#sec-runtime-managed-types)) do not use `owned`, since those types are exempt from manual disposal.
- Generic constraints are noted where relevant; absence of a constraint clause means "any T."
- **The Try convention (v0.171, #1336; shape (c) codified v0.184).** Exactly three Try shapes are sanctioned. **(a) `bool` + nullable out** — `TryX(..., out item: T?): bool` (with `owned` on the out where the payload transfers) — for keyed/positional extraction: the bool distinguishes the outcome from a legitimately-null payload, and, for a **generic or class-typed** payload, the out parameter is *nullable in every form*, because a class payload has no `default` to assign on the false path (spec §17.11) and a generic payload may be instantiated by a class; §22.3 requires every out assigned on every return path (v0.184 wording; throw paths carry no obligation), and the false path assigns `null`. **(b) nullable return** — `TryX(...): owned T?` — when null alone is the signal (e.g. `Mutex.TryAcquire`, the v0.77 ruling: simpler call sites). **(c) `bool` + non-nullable value-typed out** — `TryX(..., out result: T): bool` with `default(T)` assigned on the false path (spec §22.3) — for APIs whose payload type is a **concrete value type**: the §9 primitive `TryParse` family, `Convert.TryTo*` (§2.3), the `DateTime`/`DateOnly`/`TimeOnly`/`Guid` `TryParse` members (§2.5/§2.6), and the §4.14/§4.15 `TryRead*` accessors. The `when true` postcondition does not apply — the out is never null, so `default(T)` (a real value distinguished only by the `bool`) is what the false path assigns. The pre-v0.171 non-nullable `out owned item: T` shape was unimplementable under §22.3 (the rationale's v0.89 entry states the rule; the v0.96 collection rewrite reintroduced the illegal shape without a decision) and is removed everywhere. **Every shape-(a) signature in this document carries the `when true` nullability postcondition** on its out parameter (language spec §22.4, v0.183) — `TryX(..., out item: T? when true): bool` — so in a region where the call is known `true`, the recipient is narrowed non-null and no `!` is needed. The clause is elided from the per-type listings below for brevity but is normative for all of them (`TryGetValue`, `TryPop`/`TryPeek`/`TryDequeue`, `TryPopFirst`/`TryPopLast`, `Channel<T>.TryReceive`).
- **Negative magnitudes fault (v0.183, #1396).** Every constructor parameter that states a magnitude — `capacity`, `size`, `length` — raises `ArgumentOutOfRangeException` when passed a negative value: `StringBuilder(capacity)`, `Array<T>(capacity)`, `Array<T>(size, fill)`, `OwningArray<T>(capacity)`, the `Dictionary`/`OwningDictionary`/`Set`/`OwningSet` capacity forms, and `OwningBuffer<T>(length)` with its `init` overloads. Zero is valid (an empty container or zero-length buffer). A negative magnitude is a programming error, not a request — the defined-fault stance of `DateTime`'s component validation (§2.5), chosen over a silent empty fallback. (`Channel<T>(capacity)` is stricter under its own v0.174 contract — `capacity >= 1`, `ArgumentException` otherwise — and is unchanged.)
- Compiler-intrinsic types (primitive wrappers, `Range<T>`) may declare `const` members whose values are provided by the compiler rather than by a source-level initializer expression. In the declarations below, a `const` field shown without `:= value` (e.g. `public static const MaxValue: double;`) is such a compiler-provided constant. User-defined code may not omit `const` initializers (see language spec [§22.12](Bezel_Language_Spec.md#sec-static-members)).
- **Elided interface-required members.** When a class declares it implements an interface, the members required by that interface are part of the class's public contract whether or not the declaration shows them explicitly. Some declarations show every interface-required member; others elide the redundant ones with a marker comment such as `// plus standard ICollection<T> operations` or `// ICollection<T> operations plus:`. Both forms are equivalent — the elided members are still part of the public contract, with signatures determined by the interface declaration plus the implementation's type substitution (per the [§11.2](Bezel_Language_Spec.md#sec-interface-declaration) exact-match rule). For interfaces whose substitution produces a member with the same name but a different parameter type than another method on the class — `Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>` is the leading example, where ICollection's `Add(item: T)` substitutes to `Add(item: DictionaryEntry<K, V>)` while Dictionary also offers `Add(key: K, value: V)` — both forms are public members of the class and both are reachable.

This document is normative for the names, signatures, and observable behavior listed; it is not normative for performance characteristics, internal data structures, or the choice of algorithm. Implementations are free to optimize.

---

<a id="sec-system-auto-imported"></a>

**Naming conventions for acronyms and widths (recorded v0.165, #1359, generalizing the v0.148 ruling).** Acronyms in Bezel-coined names are PascalCase (`IeeeEquals`); names inherited verbatim from .NET keep the .NET spelling (`IEEERemainder`, `Encoding.UTF8`, `ToInt64`) — so `Math.IEEERemainder` and `Double.IeeeEquals` are both *correct* under one rule, not an inconsistency. Where a bit-width is the point of the name, the width-bearing .NET type words are used (`Int32`, `Int64` — e.g. `Random.NextInt64`, `BitConverter`'s family). The owning enumeration interfaces carry the `I-` prefix since the v0.168 family rename (#1372).

## 2. `System` (auto-imported)

<a id="sec-console"></a>
### 2.1 `Console`

A static class providing process-level standard input and output.

```bezel
public static class Console
  // Output
  public static method Write(value: string);
  public static method Write(value: Object);
  public static method WriteLine();
  public static method WriteLine(value: string);
  public static method WriteLine(value: Object);
  // v0.165 (#1355): per-type Write/WriteLine overloads for bool and the numeric
  // primitives (same shape as StringBuilder.Append §3.1) — behavior =
  // Write(value.ToString()); no Object boxing on the path. TextWriter (§7.4) mirrors them.

  // Input
  public static method ReadLine(): string?;     // null at EOF
  public static method ReadKey(): char;

  // Streams
  public static readonly property In: TextReader;
  public static readonly property Out: TextWriter;
  public static readonly property Error: TextWriter;

  // Convenience
  public static method Clear();
end Console;
```

The `Object` overloads invoke `ToString()`. For interpolated strings, the compiler resolves the `string` overload directly.

<a id="sec-math"></a>
### 2.2 `Math`

A static class of mathematical functions and constants. All trigonometric functions take and return `double` (radians). Constants are `double`.

```bezel
public static class Math
  public static const Pi: double := 3.14159265358979323846;
  public static const E: double := 2.71828182845904523536;
  public static const Tau: double := 6.28318530717958647692;

  public static method Abs(x: int): int;
  public static method Abs(x: long): long;
  public static method Abs(x: double): double;
  public static method Abs(x: decimal): decimal;

  public static method Min<T>(a: T, b: T): T where T: IComparable<T>;
  public static method Max<T>(a: T, b: T): T where T: IComparable<T>;
  public static method Clamp<T>(value: T, low: T, high: T): T where T: IComparable<T>;

  public static method Sqrt(x: double): double;
  public static method Pow(x: double, y: double): double;
  public static method Exp(x: double): double;
  public static method Log(x: double): double;       // natural log
  public static method Log(x: double, base_: double): double;
  public static method Log10(x: double): double;
  public static method Log2(x: double): double;

  public static method Sin(x: double): double;
  public static method Cos(x: double): double;
  public static method Tan(x: double): double;
  public static method Asin(x: double): double;
  public static method Acos(x: double): double;
  public static method Atan(x: double): double;
  public static method Atan2(y: double, x: double): double;
  public static method Sinh(x: double): double;
  public static method Cosh(x: double): double;
  public static method Tanh(x: double): double;

  public static method Floor(x: double): double;
  public static method Ceiling(x: double): double;
  public static method Round(x: double): double;     // banker's rounding
  public static method Round(x: double, digits: int): double;
  public static method Truncate(x: double): double;

  public static method Sign(x: int): int;            // -1, 0, or 1
  public static method Sign(x: long): int;
  public static method Sign(x: double): int;

  public static method Fmod(x: double, y: double): double;
  public static method IEEERemainder(x: double, y: double): double;
end Math;
```

`Fmod(x, y)` returns `x - n*y` where `n` is the integer truncated toward zero such that the result has the same sign as `x` and magnitude less than `|y|`. Matches C's `fmod`. Returns `NaN` if `y = 0` or if either operand is `NaN`; returns `x` if `x` is finite and `y` is `±Infinity`.

`IEEERemainder(x, y)` returns `x - n*y` where `n` is the integer nearest the exact value of `x/y`, with half-cases rounded to the even integer. Matches IEEE 754 `remainder`. The result may have the opposite sign from `x` (unlike `Fmod`). Returns `NaN` for the same edge cases as `Fmod`.

Use `Fmod` when interoperating with C-style code or when the intuition "remainder has the same sign as the dividend" matches the problem domain. Use `IEEERemainder` for numerical algorithms that require the IEEE 754 form (argument reduction, periodic-function evaluation). The `mod` operator (language spec [§6.1](Bezel_Language_Spec.md#sec-integer-arithmetic)) is integer-only; these methods are the floating-point counterparts.

<a id="sec-convert"></a>
### 2.3 `Convert`

Conversions between primitive types that produce informative exceptions on failure.

```bezel
public static class Convert
  public static method ToInt32(value: string): int;
  public static method ToInt64(value: string): long;
  public static method ToDouble(value: string): double;
  public static method ToDecimal(value: string): decimal;
  public static method ToBoolean(value: string): bool;

  public static method TryToInt32(value: string, out result: int): bool;
  public static method TryToInt64(value: string, out result: long): bool;
  public static method TryToDouble(value: string, out result: double): bool;
  public static method TryToDecimal(value: string, out result: decimal): bool;
  public static method TryToBoolean(value: string, out result: bool): bool;

  public static method ToString(value: int): string;
  public static method ToString(value: long): string;                          // v0.165: elision expanded
  public static method ToString(value: double): string;
  public static method ToString(value: decimal): string;
  public static method ToString(value: bool): string;
  public static method ToString(value: int, toBase: int): string;              // bases 2, 8, 10, 16
  public static method ToString(value: long, toBase: int): string;
end Convert;
```

`Try` forms return `false` instead of throwing on parse failure; the `out` parameter receives the default value.

<a id="sec-environment"></a>
### 2.4 `Environment`

Process- and OS-level information.

```bezel
public static class Environment
  public static readonly property NewLine: string;
  public static readonly property MachineName: string;
  public static readonly property UserName: string;
  public static readonly property OSVersion: string;
  public static readonly property ProcessorCount: int;
  public static readonly property CommandLineArgs: Array<string>;

  public static method GetEnvironmentVariable(name: string): string?;
  public static method SetEnvironmentVariable(name: string, value: string?);
  public static method GetEnvironmentVariables(): owned Dictionary<string, string>;

  public static method Exit(exitCode: int);
end Environment;
```

`GetEnvironmentVariables()` returns a freshly-allocated `owned Dictionary<string, string>` (per the §1 convention for fresh-instance returns). Although `Environment` itself is auto-imported via `System` ([§2.3](Bezel_Language_Spec.md#sec-always-available-names) of the language spec), the `Dictionary` type lives in `System.Collections.Generic` and is not auto-imported; callers using the return value typically `import System.Collections.Generic;` to access dictionary members.

<a id="sec-datetime-timespan-dateonly-timeonly"></a>
### 2.5 `DateTime`, `TimeSpan`, `DateOnly`, `TimeOnly`

Value types for representing points in time and durations. All are records.

```bezel
public record DateTime: IComparable<DateTime>, IEquatable<DateTime>
  // v0.165 (#1357): ordering + construction. CompareTo = tick order (Kind not
  // normalized, matching .NET); the <,>,<=,>= operator group agrees with CompareTo.
  public constructor(year: int, month: int, day: int);
  public constructor(year: int, month: int, day: int, hour: int, minute: int, second: int,
                     millisecond: int := 0, kind: DateTimeKind := DateTimeKind.Unspecified);
                              // ArgumentOutOfRangeException on invalid components
  public method CompareTo(other: DateTime): int;
  public readonly property Ticks: long;
  public static method FromTicks(ticks: long, kind: DateTimeKind := DateTimeKind.Unspecified): DateTime;
  public method ToUnixTimeSeconds(): long;
  public static method FromUnixTimeSeconds(seconds: long): DateTime;
  public readonly property Year: int;
  public readonly property Month: int;        // 1..12
  public readonly property Day: int;          // 1..31
  public readonly property Hour: int;         // 0..23
  public readonly property Minute: int;       // 0..59
  public readonly property Second: int;       // 0..59
  public readonly property Millisecond: int;
  public readonly property DayOfWeek: DayOfWeek;
  public readonly property DayOfYear: int;
  public readonly property Kind: DateTimeKind;

  // Now, UtcNow, Today are computed at each access (returning the current
  // date/time); MinValue, MaxValue are constants. All are "readonly" in the
  // property sense — no setter — but only the latter two are constants.
  public static readonly property Now: DateTime;
  public static readonly property UtcNow: DateTime;
  public static readonly property Today: DateTime;
  public static readonly property MinValue: DateTime;
  public static readonly property MaxValue: DateTime;

  public method AddYears(years: int): DateTime;
  public method AddMonths(months: int): DateTime;
  public method AddDays(days: double): DateTime;
  public method AddHours(hours: double): DateTime;
  public method AddMinutes(minutes: double): DateTime;
  public method AddSeconds(seconds: double): DateTime;
  public method AddMilliseconds(ms: double): DateTime;
  public method Add(span: TimeSpan): DateTime;

  public method Subtract(other: DateTime): TimeSpan;
  public method Subtract(span: TimeSpan): DateTime;

  public method ToUtc(): DateTime;
  public method ToLocal(): DateTime;
  public method ToString(): string;                // ISO 8601: "yyyy-MM-ddTHH:mm:ss"
  public method ToString(format: string): string;

  public static method Parse(s: string): DateTime;
  public static method TryParse(s: string, out result: DateTime): bool;

  public static method operator -(a: DateTime, b: DateTime): TimeSpan;
  public static method operator -(d: DateTime, t: TimeSpan): DateTime;
  public static method operator +(d: DateTime, t: TimeSpan): DateTime;
  public static method operator +(t: TimeSpan, d: DateTime): DateTime;
end DateTime;

public enum DayOfWeek: byte
  Sunday, Monday, Tuesday, Wednesday, Thursday, Friday, Saturday;
end DayOfWeek;

public enum DateTimeKind: byte
  Unspecified, Utc, Local;
end DateTimeKind;

public record TimeSpan: IComparable<TimeSpan>, IEquatable<TimeSpan>
  // v0.165 (#1357): CompareTo = tick order; operator group agrees.
  public method CompareTo(other: TimeSpan): int;
  public readonly property Ticks: long;
  public static method FromTicks(ticks: long): TimeSpan;
  public readonly property Days: int;
  public readonly property Hours: int;             // -23..23
  public readonly property Minutes: int;           // -59..59
  public readonly property Seconds: int;           // -59..59
  public readonly property Milliseconds: int;
  public readonly property TotalDays: double;
  public readonly property TotalHours: double;
  public readonly property TotalMinutes: double;
  public readonly property TotalSeconds: double;
  public readonly property TotalMilliseconds: double;

  public static method FromDays(days: double): TimeSpan;
  public static method FromHours(hours: double): TimeSpan;
  public static method FromMinutes(minutes: double): TimeSpan;
  public static method FromSeconds(seconds: double): TimeSpan;
  public static method FromMilliseconds(ms: double): TimeSpan;

  public static method operator +(a: TimeSpan, b: TimeSpan): TimeSpan;
  public static method operator -(a: TimeSpan, b: TimeSpan): TimeSpan;
  public static method operator *(a: TimeSpan, factor: double): TimeSpan;
end TimeSpan;
```

`DateOnly` and `TimeOnly` (v0.165, #1357 — explicit surfaces replacing the previous one-liner):

```bezel
public record DateOnly: IComparable<DateOnly>, IEquatable<DateOnly>
  public constructor(year: int, month: int, day: int);   // ArgumentOutOfRangeException on invalid
  public readonly property Year: int;
  public readonly property Month: int;
  public readonly property Day: int;
  public method CompareTo(other: DateOnly): int;          // chronological; operator group agrees
  public method ToDateTime(time: TimeOnly): DateTime;     // Kind = Unspecified
  public static method Parse(s: string): DateOnly;        // FormatException on invalid
  public static method TryParse(s: string, out result: DateOnly): bool;
  public method ToString(): string;                       // "yyyy-MM-dd"
end DateOnly;

public record TimeOnly: IComparable<TimeOnly>, IEquatable<TimeOnly>
  public constructor(hour: int, minute: int, second: int := 0, millisecond: int := 0);
  public readonly property Hour: int;
  public readonly property Minute: int;
  public readonly property Second: int;
  public readonly property Millisecond: int;
  public method CompareTo(other: TimeOnly): int;
  public static method Parse(s: string): TimeOnly;
  public static method TryParse(s: string, out result: TimeOnly): bool;
  public method ToString(): string;                       // "HH:mm:ss"
end TimeOnly;
```

`DateTime.Date: DateOnly` and `DateTime.TimeOfDay: TimeOnly` accessors decompose a `DateTime`.

The `ArgumentOutOfRangeException` these constructors raise is declared here (v0.182 — previously referenced without a declaration anywhere), deriving from `ArgumentException` so that an `ArgumentException` catch arm covers the whole argument-validation family (language spec [§31.2.1](Bezel_Language_Spec.md#sec-arm-patterns)); it lives in the auto-imported `System` alongside its producers:

```bezel
public class ArgumentOutOfRangeException: ArgumentException
  public constructor(message: string);
  public constructor(message: string, paramName: string);
end ArgumentOutOfRangeException;
```

The no-argument `ToString()` renders a `DateTime` in the round-trippable ISO 8601 form `yyyy-MM-ddTHH:mm:ss` (e.g. `2024-03-15T00:00:00`); it is the value also used when a `DateTime` is interpolated into a string with no format specifier. `ToString(format)` selects an explicit format.

<a id="sec-guid"></a>
### 2.6 `Guid`

A 128-bit globally unique identifier.

```bezel
public record Guid: IComparable<Guid>, IEquatable<Guid>
  // v0.165 (#1357): CompareTo = lexicographic over the 16 canonical big-endian
  // bytes (stated explicitly — .NET's field-group order is a known trap);
  // operator group agrees.
  public method CompareTo(other: Guid): int;
  public static method FromByteArray(bytes: Array<byte>): Guid;   // borrows; requires Length = 16, ArgumentException otherwise
  public static readonly property Empty: Guid;

  public static method NewGuid(): Guid;
  public static method Parse(s: string): Guid;
  public static method TryParse(s: string, out result: Guid): bool;

  public method ToString(): string;          // canonical form
  public method ToString(format: string): string;   // "N", "D", "B", "P"
  public method ToByteArray(): owned Array<byte>;
end Guid;
```

<a id="sec-random"></a>
### 2.7 `Random`

Pseudorandom number generation. Seeded; a default-constructed `Random` uses a system-time-based seed.

`Random` is a class with a no-op `Dispose` (its only state is its internal seed; there is nothing to release). Using `using var` on a `Random` is permitted but is purely cosmetic.

```bezel
public class Random
  public constructor();
  public constructor(seed: long);

  public method Next(): int;                    // 0..int.MaxValue
  public method Next(maxValue: int): int;       // 0..<maxValue
  public method Next(minValue: int, maxValue: int): int;
  public method NextInt64(): long;
  public method NextDouble(): double;           // [0.0, 1.0)
  public method NextBytes(buffer: Array<byte>);
  public method NextBoolean(): bool;
end Random;
```

For cryptographic randomness, see `System.Security.Cryptography` (a future module).

<a id="sec-icomparablet"></a>
### 2.8 `IComparable<T>`

```bezel
public interface IComparable<T>
  method CompareTo(other: T): int;
end IComparable;
```

`a.CompareTo(b)` returns a negative number if `a < b`, zero if equal, a positive number if `a > b`.

<a id="sec-iequatablet"></a>
### 2.9 `IEquatable<T>`

```bezel
public interface IEquatable<T>
  method Equals(other: T): bool;
end IEquatable;
```

A typed alternative to `Object.Equals(Object?)`. Records implement this implicitly; classes may implement it for type-safe equality without boxing.

<a id="sec-idisposable-and-iasyncdisposable"></a>
### 2.10 `IDisposable` and `IAsyncDisposable`

```bezel
public interface IDisposable
  method Dispose();
end IDisposable;

public interface IAsyncDisposable
  method DisposeAsync(): Task;
end IAsyncDisposable;
```

Every class implicitly implements `IDisposable` (language spec [§28.2](Bezel_Language_Spec.md#sec-the-idisposable-contract)); user code does not implement it directly. The interface is declared here so that generic constraints (`where T: IDisposable`) and explicit interface variables (`var d: IDisposable := someInstance;`) refer to a named declaration in the standard library rather than a language built-in.

`IAsyncDisposable` is implemented explicitly by classes that need asynchronous cleanup (language spec [§28.12](Bezel_Language_Spec.md#sec-asynchronous-disposal)). Implementations declare a method matching `DisposeAsync(): Task` and add `async` to the implementation; `async` is an implementation-side modifier and never appears on the interface declaration. A class that implements `IAsyncDisposable` should also declare a `destructor` for the synchronous fallback path.

The constraint `where T: IDisposable` admits every class (every class implicitly implements `IDisposable`), no records (records cannot implement `IDisposable` per language spec [§9.7](Bezel_Language_Spec.md#sec-records-and-disposal)), and the interfaces that extend `IDisposable` directly or transitively — including `IAsyncDisposable`, `IEnumerator<T>`, `IAsyncEnumerator<T>`, and user-defined interfaces that list `IDisposable` in their extension clause. See language spec [§14.2](Bezel_Language_Spec.md#sec-constraints) for the general conformance rule.

Both interfaces are auto-imported.

<a id="sec-subscription"></a>
### 2.11 `Subscription`

```bezel
public class Subscription
  // No public constructor; obtained from a registration call.
  destructor;       // removes the registration
end Subscription;
```

`Subscription` is returned from `IEvent<T>.Subscribe` ([§6.5](#sec-events-ievent-eventsource)), from `IAsyncEvent<T>.Subscribe` ([§6.5](#sec-events-ievent-eventsource)), from `CancellationToken.Register` ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)), and from analogous register-and-unregister APIs. Methods returning a `Subscription` annotate the return type as `owned Subscription` so that the caller's leak-prevention obligation is explicit.

`Dispose()` is idempotent (per the language-wide rule, language spec [§28.2](Bezel_Language_Spec.md#sec-the-idisposable-contract)): the first call removes the registration; subsequent calls are no-ops. This makes it safe to dispose a subscription whether or not the underlying source has already removed it for other reasons (the source was disposed, the event was unhooked elsewhere, the cancellation already fired).

`Subscription` lives in `System` (not `System.Threading`) and is auto-imported. It was a v0.76 move: in v0.74 and v0.75 the type lived in `System.Threading`, but every register-and-unregister API across the standard library returned it, including `CancellationToken.Register` whose declaring class is itself auto-imported. Putting `Subscription` next to those APIs and auto-imported by default — rather than requiring an explicit `import System.Threading;` — matches the type's actual usage. See language spec [§2.3](Bezel_Language_Spec.md#sec-always-available-names) for the auto-import list.

<a id="sec-bitconverter"></a>
### 2.12 `BitConverter`

Conversions between floating-point values and their IEEE 754 bit patterns. Used by the byte-buffer accessors of [§4.14](#sec-owned-buffers-and-spans) to read and write float values from byte spans, and available directly when user code needs the same operation.

```bezel
public static class BitConverter
  // float (32-bit) ↔ int bit pattern
  public static method FloatToInt32Bits(value: float): int;
  public static method Int32BitsToFloat(value: int): float;

  // double (64-bit) ↔ long bit pattern
  public static method DoubleToInt64Bits(value: double): long;
  public static method Int64BitsToDouble(value: long): double;

  // Same-width bit reinterpretation (compiler intrinsic). The single type
  // argument names the TARGET type; the source type is inferred from `value`.
  // The source and target must be primitive numeric types of equal bit width.
  public static method Reinterpret<TTo>(value: TFrom): TTo;
end BitConverter;
```

`FloatToInt32Bits` and `DoubleToInt64Bits` return the raw IEEE 754 bit pattern of the floating-point value as a signed integer of matching width. The integer return types are `int` (32 bits) and `long` (64 bits) — Bezel's standard width-named primitives, equivalent to the boxed record types `Int32` and `Int64` ([§9.1](#sec-integer-types)) that appear in the method names. `Int32BitsToFloat` and `Int64BitsToDouble` perform the reverse — interpreting the integer's bit pattern as a float.

These are pure bit-reinterpretation operations: the bit patterns are preserved exactly. For `NaN` values, the specific bit pattern (signaling vs quiet, payload bits) is preserved, which is what bit-level tools (serialization, hashing, exact comparison) need.

The method names use the sized record-type names (`Int32`, `Int64`) consistently with the byte-buffer accessors of [§4.14](#sec-owned-buffers-and-spans). The `Float`/`Double` portion uses the primitive keyword names (Bezel has no separate `Single` record type; `float` is the only 32-bit floating-point type). No unsigned *named* forms (e.g. `FloatToUInt32Bits`) are provided — the bit-pattern integer is treated as opaque by callers most of the time, and the signed form interoperates with the standard bitwise operators identically; when a same-width signedness view is genuinely needed, the generic `Reinterpret<TTo>` below supplies it.

`Reinterpret<TTo>(value)` (v0.145) is the general same-width bit-reinterpretation operation. The single type argument names the *target* type; the source type is inferred from the argument, so the call site writes only the target — `BitConverter.Reinterpret<int>(someUint)`. The source and target must be primitive numeric types of equal bit width (the 8-, 16-, 32-, and 64-bit groups — e.g. `int`, `uint`, and `float` are mutually reinterpretable at 32 bits; `long`, `ulong`, and `double` at 64 bits); a target of a different width is a compile error. `Reinterpret` copies the value's bits unchanged — it never raises and never alters the bit pattern. The four named `…Bits…` methods above are the float/double special cases of `Reinterpret`, retained for readability and continuity. `Reinterpret` is the bit-level counterpart to the *checked* same-width signed↔unsigned cast of language spec [§5.2](Bezel_Language_Spec.md#sec-explicit-conversions): the cast performs a *value* conversion that raises `OverflowException` if the value does not fit, while `Reinterpret` performs a *bit* reinterpretation that always succeeds. Convert a value with the cast; view the same bits under a different type with `Reinterpret`.

<a id="sec-the-type-class"></a>
### 2.13 The `Type` class

`Type` is the runtime type-identity token. A `Type` value is obtained from the `typeof(T)` operator (language spec [§17.10](Bezel_Language_Spec.md#sec-the-typeof-operator)) for a statically named type, or from `value.GetType()` (language spec [§8.2](Bezel_Language_Spec.md#sec-gettype)) for the runtime type of a value. The runtime guarantees exactly one `Type` instance per distinct type, so reference equality and `=` coincide; `Type` is a valid `Dictionary` key.

```bezel
public sealed class Type
  public readonly property Name: string;            // unqualified: "BankAccount"
  public readonly property FullName: string;        // qualified: "BankingApp.BankAccount"

  // Kind predicates. Exactly one of IsClass, IsInterface, IsRecord, IsEnum,
  // IsPrimitive, IsInlineArray, IsFunctionType is true for any Type.
  public readonly property IsClass: bool;
  public readonly property IsInterface: bool;
  public readonly property IsRecord: bool;
  public readonly property IsEnum: bool;
  public readonly property IsPrimitive: bool;       // int, bool, char, float, ...
  public readonly property IsInlineArray: bool;     // [N]T, [N owned]T?, ...
  public readonly property IsFunctionType: bool;    // (A, B) -> R
  public readonly property IsValueType: bool;       // = IsPrimitive or IsEnum
                                                    //   or IsRecord or IsInlineArray

  // Lattice queries.
  public readonly property BaseType: Type?;         // direct superclass; null for
                                                    //   Object, interfaces, value
                                                    //   types, function types
  public method Implements(iface: Type): bool;      // true if this type conforms to
                                                    //   the interface `iface`
  public method IsAssignableFrom(other: Type): bool;// true if a value of type
                                                    //   `other` is assignable to a
                                                    //   location of this type

  // Enum metadata. Valid only when IsEnum; on any other Type these raise
  // InvalidOperationException.
  public readonly property EnumNames: Array<string>;  // member names, declaration
                                                      //   order
  public readonly property EnumValues: Array<long>;   // member values, same order
                                                      //   (widened to long)
end Type;
```

**Kind predicates.** For any `Type`, exactly one of `IsClass`, `IsInterface`, `IsRecord`, `IsEnum`, `IsPrimitive`, `IsInlineArray`, `IsFunctionType` holds. These map the type kinds of language spec [§3.1](Bezel_Language_Spec.md#sec-type-kinds) onto a runtime predicate surface, with two deliberate flattenings: `string` and the heap array types (`Array<T>`, `OwningArray<T>`) report `IsClass` (they are reference types — language spec [§3.1](Bezel_Language_Spec.md#sec-type-kinds) lists `string` and heap arrays as reference types reaching `Object` directly, and treating them as classes here is consistent with how the language and standard library describe them elsewhere); the unit type `()` reports `IsValueType` with none of the seven primary predicates true (it is a degenerate value type and the rare `typeof(())` is not expected in practice, but the surface does not claim a primary kind for it). `IsValueType` is a derived convenience equal to `IsPrimitive or IsEnum or IsRecord or IsInlineArray` (and additionally true for the unit type); it does not add a primary category. The "exactly one of seven" guarantee therefore holds for every type a program can name with `typeof` or obtain from `GetType` except the unit type, which no realistic program reifies.

**`BaseType`.** Returns the direct superclass of a class type. It is `null` for `Object` (the root has no base), for interface types (interfaces have no base *class*; conformance is queried with `Implements`), for all value types, and for function types. Walking the chain to the root is `t.BaseType`, then `that!.BaseType`, terminating at the `Object` `Type` whose `BaseType` is `null`. For most subtype questions `IsAssignableFrom` is the better tool; `BaseType` is the lower-level primitive, useful mainly for diagnostics that print an inheritance chain.

**`Implements` and `IsAssignableFrom`.** `t.Implements(i)` requires `i.IsInterface` (otherwise it raises `InvalidOperationException`) and returns whether the type `t` conforms to interface `i`. `a.IsAssignableFrom(b)` is the reflective form of the `is` operator (language spec [§12.7](Bezel_Language_Spec.md#sec-type-tests-is-is-not-as)): it returns whether a value whose runtime type is `b` may be assigned to a location of static type `a` — true when `a = b`, when `a` is a base class of `b`, or when `a` is an interface that `b` implements. The two are related: `a.IsAssignableFrom(b)` with `a.IsInterface` is equivalent to `b.Implements(a)`. Both operate on `Type` tokens with no value in hand, which is what distinguishes them from `is`/`as`.

**Enum metadata.** When `IsEnum`, `EnumNames` and `EnumValues` are parallel arrays (same length, same order — declaration order of the enum members) giving each member's source name and its integer value widened to `long`. They are the building blocks for enum-to-string formatting, string-to-enum parsing, and enumerating all members for UI or validation. On any non-enum `Type`, accessing either property raises `InvalidOperationException` (the property is meaningless, and a silent empty array would mask a programming error). Example:

```bezel
public method DescribeEnum(t: Type)
  if not t.IsEnum then return; end;
  var names := t.EnumNames;
  var values := t.EnumValues;
  for var i in 0..<names.Length do
    Console.WriteLine($"{names[i]} = {values[i]}");
  end;
end DescribeEnum;
```

**What `Type` deliberately does not provide.** There is no field, property, or method enumeration; no by-name member access; no attribute reading; no dynamic instantiation. `Type` is type identity plus lattice metadata, not member-access reflection. The reasoning is in rationale [§5.18](Bezel_Design_Rationale.md#sec-type-identity-not-reflection): a reflectively-accessed `owned` member would have no static call site to carry its ownership contract, and member reflection is the largest possible breach of the "no hidden magic" principle the language is built on. Programs that would reach for member reflection in other languages use compile-time generics, explicit registration instead of reflective discovery, and source-defined serialization in Bezel.

<a id="sec-serialization-vocabulary"></a>
### 2.14 Serialization vocabulary

Bezel's serialization model separates two concerns that runtime-reflection-based serializers conflate: *what a type's shape is* (the type's concern, statically known) and *how a format encodes primitives and structure* (the format's concern). This separation is the architectural keystone — it is what lets serialization work without reflection, and it is the one part that cannot be retrofitted later. v0.114 establishes the interface vocabulary; the writing direction is usable immediately by hand, and the design is forward-compatible with the compiler-synthesized conformance planned for a later revision (rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection)).

Four interfaces, all auto-imported with the rest of `System`. Two are *capabilities a type declares about itself* (the `-able` family, like `IDisposable`/`IEquatable<T>`); two are *agents that act on values* (the agent-noun family, like `IEnumerator<T>`/`IEqualityComparer<T>`).

```bezel
// Agent: writes primitive and structural tokens in a concrete format.
// Implemented by JsonSerializer, BinarySerializer, etc.
public interface ISerializer
  method WriteInt(value: long);
  method WriteUInt(value: ulong);
  method WriteFloat(value: double);
  method WriteDecimal(value: decimal);
  method WriteBool(value: bool);
  method WriteString(value: string);
  method WriteChar(value: char);
  method WriteNull();
  method BeginObject(fieldCount: int);
  method WriteFieldName(name: string);
  method EndObject();
  method BeginArray(elementCount: int);
  method EndArray();
end ISerializer;

// Agent: reads the same tokens back. Mirror of ISerializer.
public interface IDeserializer
  method ReadInt(): long;
  method ReadUInt(): ulong;
  method ReadFloat(): double;
  method ReadDecimal(): decimal;
  method ReadBool(): bool;
  method ReadString(): string;
  method ReadChar(): char;
  method TryReadNull(): bool;            // true and consumes if next is null
  method BeginObject(): int;             // returns field count if the format
                                         //   provides one, else -1
  method ReadFieldName(): string;
  method SkipValue();                    // consume and discard the current
                                         //   value (including nested object/
                                         //   array structure); used by
                                         //   synthesized Deserialize for
                                         //   unknown fields. A strict
                                         //   deserializer raises on the
                                         //   unknown field before this is
                                         //   reached; a tolerant one skips.
  method EndObject();
  method BeginArray(): int;              // element count, or -1 if unknown
  method HasMoreElements(): bool;        // for count = -1 streaming formats
  method EndArray();
end IDeserializer;

// Capability: this type can write itself to any ISerializer.
public interface ISerializable
  method Serialize(s: ISerializer);
end ISerializable;

// Capability: this type can be reconstructed from any IDeserializer.
// The reconstruction entry point is a static factory; see the note below
// on why this is recognized by name rather than declared as an interface
// method in this version.
public interface IDeserializable
end IDeserializable;
```

**`ISerializer` / `IDeserializer` are the format abstraction.** A concrete format implements them once; the standard library provides `JsonSerializer`/`JsonDeserializer` and a compact `BinarySerializer`/`BinaryDeserializer` pair (their concrete definitions are in [§2.15](#sec-concrete-serialization-formats); design rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection)). The interfaces are format-agnostic by construction: they speak only of primitives (`WriteInt`, `WriteString`, …) and structure (`BeginObject`/`EndObject`, `BeginArray`/`EndArray`). They name no format. A type that serializes itself against `ISerializer` is automatically serializable to every format, present and future, without change — exactly the property reflection-based serializers achieve at the cost of runtime type inspection, achieved here with none.

**`ISerializable` is usable today, by hand.** `Serialize` is an ordinary instance method against an interface; nothing about it depends on a deferred feature. A type that wants to be serializable in this version writes the method explicitly:

```bezel
public record Point(X: int, Y: int): ISerializable
  public method Serialize(s: ISerializer)
    s.BeginObject(2);
    s.WriteFieldName("X"); s.WriteInt(X);
    s.WriteFieldName("Y"); s.WriteInt(Y);
    s.EndObject();
  end Serialize;
end Point;
```

(A positional record may add a body to supply method implementations; the positional parameters remain the auto-generated readonly properties of [§9.1](Bezel_Language_Spec.md#sec-positional-records).) Composition works through the same interface: a containing type's `Serialize` calls each serializable component's `Serialize`, passing the same `ISerializer` through. A serializable collection writes `BeginArray`, each element's `Serialize`, `EndArray`.

**Why `IDeserializable` has no method in this version.** Serialization is an *instance* operation (an existing value writes itself); deserialization is a *static* one (a format reader produces a new value — there is no instance yet). A static entry point could not be expressed as an interface method until static interface members existed (added in v0.176 — language spec [§11.8](Bezel_Language_Spec.md#sec-static-interface-members)); this by-name protocol predates them. Rather than block deserialization on that keystone, v0.114 recognizes the deserialization entry point **by name**, exactly as the user-defined collection-literal protocol recognizes `FromCollectionLiteral` ([§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol) of the language spec): a type that declares `: IDeserializable` and provides a static method of the shape

```bezel
public static method Deserialize(d: IDeserializer): T;          // value-type
                                                                //   target (record,
                                                                //   enum): no `owned`
public static method Deserialize(d: IDeserializer): owned T;     // class target:
                                                                //   `owned` return
public static method Deserialize(d: IDeserializer): owned Shared<C>;  // co-owned entry:
                                                                //   the class is itself
                                                                //   a shared node
// T = the declaring type; C = the declaring shared-node class; both written concretely.
```

is deserializable. The recognized factory's return type is the declaring type itself, written concretely. The `owned` modifier appears exactly when the declaring type is a class (a class instance is an owned heap value transferred to the caller — language spec [§28.8](Bezel_Language_Spec.md#sec-ownership-transfer)); for a value-type target (record or enum, the synthesis-eligible common case) the return is a plain by-value `T` with no `owned` modifier, because `owned` on a non-class return is semantically rejected (language spec [§22.1](Bezel_Language_Spec.md#sec-method-declarations) — the modifier applies only to class-typed returns). A class that is itself a *shared node* in its own serialized graph uses the `owned Shared<C>` form — it participates by identity and round-trips as a co-owned handle, and `Shared<C>` is a class so the `owned` modifier applies; the choice is locally decidable — a class that merely *holds* `Shared<T>`/`Weak<T>` fields returns `owned C`, a class that *is* the shared node returns `owned Shared<C>` (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis), rationale [§5.22](Bezel_Design_Rationale.md#sec-type-driven-serialization)). Bezel has no `Self` type (that feature is separately deferred), so the return type is spelled with the concrete type name. `IDeserializable` is a *marker* whose presence says "this type provides the recognized `Deserialize` factory," in the same way `: IEquatable<T>` opts a record into synthesized equality without the user writing the comparison. The hand-written form today, for the record (value-type) case:

```bezel
public record Point(X: int, Y: int): ISerializable, IDeserializable
  public method Serialize(s: ISerializer)
    s.BeginObject(2);
    s.WriteFieldName("X"); s.WriteInt(X);
    s.WriteFieldName("Y"); s.WriteInt(Y);
    s.EndObject();
  end Serialize;

  public static method Deserialize(d: IDeserializer): Point
    d.BeginObject();
    var x := 0;  var y := 0;
    // field-name dispatch omitted for brevity; a real implementation
    // loops over ReadFieldName() and fills the locals
    d.EndObject();
    return new Point(x, y);
  end Deserialize;
end Point;
```

When static interface members are added, `Deserialize` can be promoted to an actual `IDeserializable` interface method with the identical name and signature, without breaking any code — the name and shape were chosen now to be promotable, the same forward-compatibility discipline applied to `FromCollectionLiteral`.

**Synthesis (v0.116) and what remains.** As of v0.116, a record or enum that declares `: ISerializable` and/or `: IDeserializable` and satisfies the synthesis-eligibility rule has the bodies **generated by the compiler** — the hand-written forms above are no longer necessary for the common case. The rule, the recursive component check, and the generated-body semantics are defined in language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis); the design mirrors the existing auto-equality synthesis ([§9.2](Bezel_Language_Spec.md#sec-body-records)). The hand-written `Point` above becomes simply:

```bezel
public record Point(X: int, Y: int): ISerializable, IDeserializable;
// compiler synthesizes Serialize(s: ISerializer) and the recognized
// static Deserialize(d: IDeserializer): Point — no body needed
```

Hand-writing either member suppresses synthesis of that direction (exactly as a manual `Equals` suppresses auto-equality), so the explicit forms shown earlier remain valid and take precedence when present. As of v0.117, a `serialized` clause customizes the synthesized field names without disabling synthesis:

```bezel
public record User(
    Id: int,
    FirstName: string        serialized "first_name",
    InternalToken: string    serialized skip,
    LastSeen: DateTime?      serialized "last_seen" optional
): ISerializable, IDeserializable;
// synthesized form codes {"Id": …, "first_name": …, "last_seen": …};
// InternalToken is excluded; last_seen may be absent on read
```

The clause has a closed effect vocabulary — `serialized "<name>"` (rename), `serialized skip` (exclude), `serialized optional` (omit-if-null on write, default-if-missing on read), with rename and tolerate combinable as `serialized "<name>" optional` and `skip` mutually exclusive with both — defined normatively in language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis). It is a compiler-recognized declaration modifier, deliberately *not* an attribute system and deliberately not a separate exhaustive key list (rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection) explains the design and why it improves on the Swift `CodingKeys` model).

A **class** can also be synthesized: it declares `: ISerializable`/`: IDeserializable` and the compiler reads its serialized shape from its field types — no header clause. A class whose fields are all `owned` (or value-typed) serializes as a tree:

```bezel
public class Order(id: int, owned customer: Customer,
                   owned lines: OwningArray<LineItem>): ISerializable, IDeserializable
  public owned Customer: Customer := customer;        // owned: an owning edge, serialized inline
  public owned Lines: OwningArray<LineItem> := lines;
  public readonly property Id: int => id;
  // compiler synthesizes Serialize / Deserialize; each owned child recurses
  // through its own synthesized members. The primary constructor binds every
  // serialized component (id, customer, lines), matched case-insensitively to
  // the fields — spec §9.8 constructor selection.
end Order;
```

A class whose serialized graph *aliases* — the same object reachable by more than one serialized path — expresses the shared edge through its field types: a plain borrowing field is a back-edge into the owning tree, and a `Shared<T>` field is a co-owned identity participant. No header clause:

```bezel
public class Invoice(number: string, owned seller: Company, owned buyer: Company,
                     accountManager: Employee): ISerializable, IDeserializable
  public owned Seller: Company := seller;             // owned: in the owning spanning tree
  public owned Buyer: Company := buyer;               // owned
  public AccountManager: Employee := accountManager;  // borrow: a back-edge, e.g. an
                                                      //   Employee owned by Seller
  public readonly property Number: string => number;
end Invoice;
```

The synthesized `Deserialize` for a class returns `owned Invoice` (a class instance is an owned heap value transferred to the caller). It reconstructs the owning spanning tree (the `owned` edges) in owner-before-borrower order; each plain borrowing back-edge (here `AccountManager`) is supplied **as a constructor argument**, resolved against an identity table that already holds the target because the owning tree admits such an order. Because every reference field bound through the constructor is set before any constructor body runs, no constructed object ever has an unbound non-nullable reference — definite assignment (language spec [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields)) holds with no nullable relaxation. A `Shared<T>` participant is reconstructed once at its first occurrence and `Clone()`d at each alias; a `Weak<T>` back-edge is `Downgrade()`d from its reconstructed `Shared<T>`. Identity is preserved, never duplicated — Bezel refuses the silent-duplication failure mode of reference-unaware serializers. The one read-side check the field types cannot decide is that a `Weak<T>`'s target is strongly reached somewhere in the graph: a `Weak` reference to a never-strongly-reached target raises `SerializationException`.

A base class whose components may hold derived instances declares an explicit `polymorphic` registry as a class-body member:

```bezel
public abstract class Shape: ISerializable, IDeserializable
  polymorphic ("circle": Circle, "square": Square);
end Shape;

public class Circle(radius: double): Shape, ISerializable, IDeserializable
  public readonly property Radius: double => radius;
end Circle;
```

The registry is a closed, explicitly written map from a wire discriminator to a permitted concrete type — **no reflective subtype discovery** (the design principle of rationale [§5.18](Bezel_Design_Rationale.md#sec-type-identity-not-reflection); this is what prevents a serialized discriminator from becoming an arbitrary-type construction primitive, the defect behind reflective-`$type` remote-code-execution vulnerabilities elsewhere). The synthesized serializer writes the concrete type's discriminator as a leading `$type` field; the synthesized deserializer reads it **first**, looks it up in the registry, and delegates to that type's synthesized `Deserialize`. An unregistered discriminator (read) or an unregistered run-time type (write) raises `SerializationException` — the closed set enforced on both directions. Polymorphic dispatch composes uniformly with the type-driven model: each registered type reconstructs under whatever shape its own field types describe.

A class whose serialized graph may contain **cycles** uses `Shared<T>` for the shared spine and a nullable `Weak<T>?` for each cycle-closing back-edge — both ordinary owned handles, so the cycle is constructed in ownership-valid Bezel and round-trips through the ownership checker:

```bezel
public class Node(label: string): ISerializable, IDeserializable
  public owned Next: Shared<Node>?;  // owned handle to the shared spine (identity
                                     //   participant); nullable to terminate the chain
  public owned Back: Weak<Node>?;    // back-edge: a weak observer that may close a cycle
  public readonly property Label: string => label;
end Node;
```

A cycle is built by constructing the `Shared<Node>` spine, then wiring each `Weak<Node>?` back-edge with `Downgrade()` once its target exists — ordinary mutation of a nullable field, so definite assignment (language spec [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields)) is satisfied by the null initializer and no two-phase construction is required. The synthesized `Deserialize` reconstructs the spine and binds each `Weak` `$ref` by `Downgrade()`ing the already-reconstructed `Shared<Node>` its identity tag resolves to. A `Weak<T>` reference whose target is never strongly reached in the graph raises `SerializationException` (the design is rationale [§5.22](Bezel_Design_Rationale.md#sec-type-driven-serialization)).

This is the type-driven serialization model: a class declares `: ISerializable`/`: IDeserializable` and its serialized shape — tree, DAG, or cyclic — follows from its field types (`owned` → owning edge, `Shared<T>` → identity participant, `Weak<T>`/borrow → back-reference), with polymorphic edges resolved through an explicit class-body `polymorphic` registry. The concrete format implementations are provided as of v0.115 — see [§2.15](#sec-concrete-serialization-formats). The migration to type-driven shape (retiring the earlier `serialized tree`/`serialized graph`/`serialized cyclic` keyword clauses in favour of `Shared<T>`/`Weak<T>` ser/des) is recorded in rationale [§5.22](Bezel_Design_Rationale.md#sec-type-driven-serialization).

<a id="sec-concrete-serialization-formats"></a>
### 2.15 Concrete serialization formats

The standard library provides two concrete implementations of the [§2.14](#sec-serialization-vocabulary) agent interfaces: a JSON pair (`JsonSerializer`/`JsonDeserializer`) and a compact binary pair (`BinarySerializer`/`BinaryDeserializer`). Both are ordinary classes implementing `ISerializer`/`IDeserializer`; they introduce no language feature. They live in `System.Serialization` and are *not* auto-imported (unlike the vocabulary interfaces) — a program that performs serialization names the format explicitly with `import System.Serialization`.

Both serializers write to, and both deserializers read from, an `OwningBuffer<byte>`-backed stream. The serializer accumulates bytes and exposes the result; the deserializer is constructed over a `ReadOnlySpan<byte>` of the encoded form.

```bezel
public class JsonSerializer: ISerializer, IDisposable
  public constructor();
  public constructor(options: JsonOptions);
  // ISerializer surface (§2.14) implemented for JSON text.
  public method ToBytes(): owned OwningBuffer<byte>;   // UTF-8 encoded result
  public method ToString(): string;                   // convenience: UTF-8 → string
end JsonSerializer;

public class JsonDeserializer: IDeserializer, IDisposable
  public constructor(input: ReadOnlySpan<byte>);
  public constructor(input: ReadOnlyArraySpan<byte>);    // v0.165 (#1358): Array-world twin;
                                                          //   borrowed identically for the deserializer's lifetime      // UTF-8 JSON
  public constructor(input: string);                  // convenience
  public constructor(input: ReadOnlySpan<byte>, options: JsonOptions);
  // IDeserializer surface (§2.14) implemented for JSON text.
end JsonDeserializer;

public record JsonOptions(
  Indented: bool,                 // pretty-print (serializer); ignored on read
  IndentWidth: int,               // spaces per level when Indented; default 2
  AllowTrailingCommas: bool,      // deserializer leniency; default false
  IgnoreUnknownFields: bool,      // deserializer: skip input fields that match
                                  //   no declared component instead of raising;
                                  //   default false (strict)
  MaxDepth: int                   // nesting guard; default 64
);

public class BinarySerializer: ISerializer, IDisposable
  public constructor();
  // ISerializer surface (§2.14) implemented for the compact binary format.
  public method ToBytes(): owned OwningBuffer<byte>;
end BinarySerializer;

public class BinaryDeserializer: IDeserializer, IDisposable
  public constructor(input: ReadOnlySpan<byte>);
  public constructor(input: ReadOnlyArraySpan<byte>);    // v0.165 (#1358): Array-world twin;
                                                          //   borrowed identically for the deserializer's lifetime                     // strict
  public constructor(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool);
  // IDeserializer surface (§2.14) implemented for the compact binary format.
end BinaryDeserializer;
```

**JSON mapping.** The `ISerializer` token stream maps to JSON as follows. `WriteInt`/`WriteUInt` produce JSON numbers; `WriteFloat` produces a JSON number, with non-finite values (`NaN`, `±Infinity`) raising `SerializationException` (JSON has no representation for them — a binary format is the correct choice when non-finite floats must round-trip). `WriteDecimal` produces a JSON number written without precision loss. `WriteBool` produces `true`/`false`; `WriteString` produces a quoted, escaped JSON string; `WriteChar` produces a one-character JSON string. `WriteNull` produces `null`. `BeginObject`/`WriteFieldName`/`EndObject` produce a JSON object; the `fieldCount` argument to `BeginObject` is advisory (JSON objects are not length-prefixed) and is ignored by `JsonSerializer`. `BeginArray`/`EndArray` produce a JSON array; the `elementCount` argument is likewise advisory. **Identity tokens (specified in v0.139):** the graph-identity operations of the synthesis (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis) — first-occurrence tags for `Shared<T>` identity participants and `$ref` back-references for `Weak<T>` and borrowing fields) map to JSON as follows: an object carrying an identity tag is written with a reserved first field `"$id": n` (a positive JSON number, unique per stream); a back-reference is written as the one-field object `{"$ref": n}` in place of the value. On the read side these forms are recognized positionally (an object whose first field is `"$id"` registers the tag; an object whose only field is `"$ref"` resolves it). Field names beginning with `$` are reserved for this mapping: a `serialized "<name>"` rename whose name begins with `$` is a compile error at the conformance declaration. The binary format's tag encoding is internal to it (§9.8's semantics; this paragraph is the JSON wire form only). On the read side, `JsonDeserializer.BeginObject()` and `BeginArray()` return `-1` (JSON does not prefix counts), so consumers use `ReadFieldName()` driven by the object's textual extent and `HasMoreElements()` for arrays. `TryReadNull()` consumes and returns `true` iff the next value is `null`.

**Binary mapping.** The compact binary format is length-prefixed and self-describing enough to be read back without the schema, but it is *not* a stable interchange format across Bezel versions — it is intended for same-version persistence and IPC, where compactness and speed matter and the producer and consumer share a build. Integers use a variable-length encoding; `BeginObject`/`BeginArray` write the count, so `BinaryDeserializer.BeginObject()` and `BeginArray()` return the actual element count and `HasMoreElements()` is answered from it. Non-finite floats round-trip exactly (the format stores IEEE 754 bit patterns via `BitConverter`, [§2.12](#sec-bitconverter)). Programs needing a documented, version-stable wire format use JSON or an explicit hand-written format; the binary format trades interchange stability for size and speed.

**Ownership and lifetime.** All four classes are `IDisposable`; the serializers own an internal growable byte buffer, the deserializers own parser state. The idiom is scope-bound:

```bezel
import System.Serialization;

public method SavePoint(p: Point): owned OwningBuffer<byte>
  using var s := new JsonSerializer();
  p.Serialize(s);                       // p implements ISerializable (§2.14)
  return s.ToBytes();                   // ownership of the buffer transfers out
end SavePoint;

public method LoadPoint(bytes: ReadOnlySpan<byte>): Point
  using var d := new JsonDeserializer(bytes);
  return Point.Deserialize(d);          // recognized static factory (§2.14)
end LoadPoint;
```

`ToBytes()` returns an `owned OwningBuffer<byte>` ([§4.14](#sec-owned-buffers-and-spans)) — a fresh buffer whose ownership transfers to the caller; the serializer may be disposed immediately after. `JsonSerializer.ToString()` is a convenience that UTF-8-decodes the same bytes into a `string`. A deserializer constructed over a `ReadOnlySpan<byte>` borrows that span for the deserializer's lifetime; the backing bytes must outlive the `using var d` scope, which the example satisfies because the span parameter outlives the method body.

**Errors.** Malformed input, type mismatches (a `ReadInt` when the next token is a string), depth-limit violations, and trailing-data conditions raise `SerializationException` (declared in `System.Serialization`). `SerializationException` carries a byte offset (for binary) or line/column (for JSON) to locate the fault. The deserializer interfaces' `Try*`-shaped members (`TryReadNull`) do not throw; every other read member throws on mismatch rather than returning a sentinel, because a deserialization mismatch is a structural error in the input, not an expected control-flow branch.

**Unknown-field policy.** A field present in the input that matches no declared component of the target type is, by default, a `SerializationException` — the strict default established when synthesis shipped (v0.116), because silently dropping unrecognized input masks producer/consumer schema drift and field-name typos. This behavior is a property of the deserialization *operation*, not of the target type: the same type may legitimately be read strictly when consuming a trusted internal message and leniently when consuming a third-party producer that adds fields freely, a decision that belongs to the caller performing the read, not to the type's serialization contract (rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection)). It is therefore configured on the deserializer: `JsonOptions.IgnoreUnknownFields` (default `false`) for JSON, the `ignoreUnknownFields` constructor parameter for `BinaryDeserializer`. When set, the deserializer skips an unrecognized field's value (via `IDeserializer.SkipValue`, [§2.14](#sec-serialization-vocabulary), which consumes the value including any nested object/array structure) and continues rather than raising. The synthesized `Deserialize` (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis)) is identical regardless of policy — it always delegates an unrecognized field to the deserializer; strictness lives entirely in the deserializer's configuration. This policy governs only *unknown* fields; missing required fields, duplicates, and type mismatches remain unconditional errors regardless of the setting, and a *missing* declared field is tolerated only by the per-member `serialized optional` clause (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis)) — the two leniencies are orthogonal and compose.

`JsonSerializer`, `JsonDeserializer`, `BinarySerializer`, `BinaryDeserializer`, `JsonOptions`, and `SerializationException` are declared in `System.Serialization` and require an explicit `import System.Serialization`. The vocabulary interfaces they implement (`ISerializer`/`IDeserializer`/`ISerializable`/`IDeserializable`, [§2.14](#sec-serialization-vocabulary)) remain auto-imported, so a type's `: ISerializable` conformance never requires the import — only the code that picks a concrete format does.

<a id="sec-shared-and-weak"></a>
### 2.16 `Shared<T>` and `Weak<T>`

Scoped shared ownership (language spec [§28.13](Bezel_Language_Spec.md#sec-shared-ownership)): N handles jointly own one underlying value; the last handle's disposal disposes it. Both types are compiler-known; the shared count and last-one-disposes behavior are not expressible in user Bezel. Both are auto-imported with `System`. `Shared<T>` is sendable iff `T` is in the `Shared`-payload synchronized set of language spec [§28.13](Bezel_Language_Spec.md#sec-shared-ownership) (v0.184 wording for the v0.140 rule; the count is maintained with synchronized operations); `Weak<T>` and `Shared<T>` of any other `T` remain domain-local ([§30.14](Bezel_Language_Spec.md#sec-sendable-types)).

```bezel
public class Shared<T>: IDisposable where T: class      // proper class types only;
                                                         //   runtime-managed T rejected at instantiation
  public constructor(owned value: T);                    // consumes the value; count = 1

  public method Clone(): owned Shared<T>;                // count + 1; a new independent handle
  public method Get(): T;                                // borrow of the underlying value;
                                                         //   source: this handle (valid while it lives)
  public method Downgrade(): owned Weak<T>;              // mint a non-owning observer

  // Dispose(): count - 1; the last disposal disposes the underlying value.
end Shared;

public class Weak<T>: IDisposable where T: class
  public method TryUpgrade(out owned s: Shared<T>?): bool;   // true + a new strong handle
                                                              //   if the value is still alive
  // Does not keep the underlying value alive. Dispose() releases the observer.
end Weak;
```

```bezel
using var cacheEntry := new Shared<ParsedConfig>(Parse(text));
_index.Add("primary", cacheEntry.Clone());   // the index owns its own handle
                                             //   (_index: OwningDictionary<string, Shared<ParsedConfig>>)
Apply(cacheEntry.Get());                     // borrow for the call
```

Usage notes:

- Handles are ordinary owned class instances; `using var`, `owned` fields, `move`, owning collections, and every other language-spec [§28](Bezel_Language_Spec.md#sec-resource-management) rule apply unchanged. Handle duplication is never implicit — only `Clone()` increments the count.
- `Get()`'s borrow is valid while *that handle* lives; disposing the handle while borrows from it are live is the ordinary borrow-lifetime error, caught by the same rules as any other source.
- Ownership cycles through `Shared<T>` keep the count above zero and leak; break them with `Weak<T>` (the standard parent-strong/child-weak shape). The compiler does not detect such cycles.
- `TryUpgrade` follows the Try pattern: `if w.TryUpgrade(out using var s) then ... end;` — on `true` the caller owns a fresh handle, discharged by the `out using var`.

<a id="sec-built-in-exceptions-cross-reference"></a>
### 2.17 Built-in exception types (cross-reference)

The built-in exception hierarchy — `Exception` and the types available without `import` — is normative in the language spec ([§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)); this document does not repeat it. Exceptions whose producers live in this document require the producer's `import` and are declared alongside it (e.g. `KeyNotFoundException` in [§4.8](#sec-dictionaryk-v), the IO exceptions in [§7.5](#sec-io-exception-types)).

One built-in carries surface beyond `Exception`'s and is recorded here for completeness:

```bezel
public class TypeInitializationException: Exception
  public readonly property InnerException: Exception?;   // the original initializer exception (null for the spec §22.13 cycle backstop)
end TypeInitializationException;
```

Raised on access to a static member of a type whose static initialization failed; `InnerException` carries the original exception — `null` for the instance the runtime synthesizes at the static-init cycle backstop, where no original exists (language spec [§22.13](Bezel_Language_Spec.md#sec-static-initialization-order), [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)).

---

<a id="sec-system-text-auto-imported"></a>
<a id="sec-iformattable"></a>
### 2.18 `IFormattable` and format specifiers (v0.172, #812/#1339)

```bezel
public interface IFormattable
  method ToString(format: string): string;
end IFormattable;
```

Auto-imported. String interpolation dispatches a hole's format spec here (language spec §18.5): `$"{money:C2}"` calls `money.ToString("C2")` when `money`'s type implements `IFormattable`. The primitive and temporal types are **intrinsically formattable** (they behave as conforming implementations without declaring the interface); a format spec on any other non-conforming operand is a compile error.

**The specifier vocabulary is invariant-culture only** — Bezel has no culture machinery, and formatted output is identical on every platform. An unrecognized specifier raises `FormatException` (strict-fault, as everywhere).

| Kind | Specifiers | Notes |
|---|---|---|
| Integers (`int`, `long`, `byte`, …) | `D`/`Dn` decimal (n = min digits, zero-padded) · `X`/`x` hex (upper/lower, `Xn` pads) · `B` binary | no grouping separators (no culture) |
| Floating (`double`, `float`) | `F`/`Fn` fixed-point (n decimals, default 2) · `E`/`En` scientific · `G`/`Gn` general · `P`/`Pn` percent (×100, `%` suffix) · `R` round-trip | `R` and the **no-format `ToString()`** both produce the shortest string that parses back to the identical value (round-trip guarantee, v0.172) |
| `decimal` | `F`/`Fn`, `E`/`En`, `G`, `P`/`Pn` | exact decimal semantics |
| `DateTime`, `DateOnly`, `TimeOnly` | pattern strings of `yyyy` `MM` `dd` `HH` `mm` `ss` `fff` plus literal separators | e.g. `yyyy-MM-dd HH:mm:ss`; no-format `ToString()` is ISO 8601 |
| `TimeSpan` | `c` (constant: `[-][d.]HH:mm:ss[.fffffff]`) | no-format = `c` |
| `Guid` | `D` (hyphenated, lowercase), `N` (no hyphens) | no-format = `D` |
| `bool`, `char`, `string` | *(none)* | a format spec on these is a compile error |

`C` (currency) deliberately does not exist — currency formatting is culture by definition; format money explicitly (`$"{amount:F2} EUR"`).

<a id="sec-static-abstractions"></a>
### 2.19 `INumber<T>`, `ISteppable<T>`, `IParsable<T>` (v0.176, #807)

The static-interface abstractions (language spec [§11.8](Bezel_Language_Spec.md#sec-static-interface-members)). The primitive numeric tower and `char` conform via compiler-provided conformances; user types conform by declaring and supplying the statics.

```bezel
public interface INumber<T> where T: INumber<T>
  static method Zero(): T;
  static method One(): T;
  static operator +(a: T, b: T): T;
  static operator -(a: T, b: T): T;
  static operator *(a: T, b: T): T;
  static operator /(a: T, b: T): T;      // raises per the operand type's division rules
  static operator <(a: T, b: T): bool;
end INumber;

public interface ISteppable<T> where T: ISteppable<T>
  static method Step(value: T, by: long): T;   // advance in T's space
  static operator <(a: T, b: T): bool;
end ISteppable;

public interface IParsable<T> where T: IParsable<T>
  static method Parse(s: string): T;                       // FormatException on bad input
  static method TryParse(s: string, out value: T?): bool;  // the §1 Try convention
end IParsable;
```

Conformances: `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` conform to all three; `char` to `ISteppable<char>` and `IParsable<char>`. `string` conforms to none (concatenation is not addition; parsing a string to a string is meaningless). `Parse` is witnessed by the existing per-type members unchanged. For `TryParse`, the per-type members keep their non-nullable value-typed outs (the shape-(c) convention, [§1](#sec-conventions)); the **compiler-provided conformance bridges the out-nullability difference**, so `T.TryParse(s, out v)` **through the constraint** has the declared `out value: T?` shape while a **direct** call keeps `out result: T` (language spec [§11.8](Bezel_Language_Spec.md#sec-static-interface-members)).

## 3. `System.Text` (auto-imported)

<a id="sec-stringbuilder"></a>
### 3.1 `StringBuilder`

Mutable string-building. Use this when concatenating in a loop.

```bezel
public class StringBuilder
  public constructor();
  public constructor(capacity: int);
  public constructor(initial: string);

  public readonly property Length: int;
  public readonly property Capacity: int;

  public method Append(value: string): StringBuilder;
  public method Append(value: char): StringBuilder;
  public method Append(value: Object): StringBuilder;
  // v0.165 (#1355): per-type overloads for bool and the numeric primitives —
  // observable behavior = Append(value.ToString()); no Object boxing on the path.
  public method Append(value: bool): StringBuilder;
  public method Append(value: int): StringBuilder;      // + byte/sbyte/short/ushort/uint/long/ulong/float/double/decimal, same shape
  public method AppendLine(): StringBuilder;
  public method AppendLine(value: string): StringBuilder;
  public method AppendFormat(format: string, args: Array<Object>): StringBuilder;

  public method Insert(index: int, value: string): StringBuilder;
  public method Remove(startIndex: int, length: int): StringBuilder;
  public method Replace(oldValue: string, newValue: string): StringBuilder;
  public method Clear(): StringBuilder;

  public method ToString(): string;
end StringBuilder;
```

Methods return `this` to allow chaining.

<a id="sec-encoding"></a>
### 3.2 `Encoding`

```bezel
public abstract class Encoding
  public abstract method GetBytes(s: string): owned Array<byte>;
  public abstract method GetString(bytes: Array<byte>): string;
  public abstract method GetString(bytes: Array<byte>, index: int, count: int): string;
                              // window over Count; IndexOutOfRangeException out of range (v0.165)
  public abstract method GetByteCount(s: string): int;                       // v0.165 (#1355)
  public abstract method GetBytes(s: string, destination: Span<byte>): int;  // v0.165: returns bytes written;
                                                                             //   IndexOutOfRangeException if too short
  public abstract readonly property Name: string;

  public static readonly property UTF8: Encoding;
  public static readonly property UTF16: Encoding;
  public static readonly property UTF32: Encoding;
  public static readonly property ASCII: Encoding;
end Encoding;
```

The static properties (`UTF8`, `UTF16`, `UTF32`, `ASCII`) return singleton instances of implementation-provided concrete subclasses. The concrete subclass types are not part of the public API; callers access them through the `Encoding` base type. The singleton instances are program-lifetime objects with no resources to release; their `Dispose()` is a no-op. They are not marked `static owned` — they are exempt from the ownership-discharge requirement by the same reasoning as non-`owned` static class-typed fields ([§22.13](Bezel_Language_Spec.md#sec-static-initialization-order) of the language spec).

`Encoding` is `abstract` to permit implementation-provided concrete subclasses. User code may subclass `Encoding` for custom encodings; user subclasses are subject to normal class disposal rules (they may have destructors and owned fields).

**Semantics of the standard encodings (specified in v0.136).** All four standard encodings follow one error model: malformed or unrepresentable input is a **defined fault**, never a silent substitution. `GetString` raises `FormatException` when the input bytes are not a well-formed sequence in the encoding; `GetBytes` raises `FormatException` when the string contains a code point the encoding cannot represent (possible only for `ASCII`). No replacement characters are produced. (The lenient decode-with-replacement mode was considered and declined — [`Bezel_Changelog.md`](Bezel_Changelog.md), v0.136.)

- **`UTF8`** (`Name` = `"utf-8"`): bytes are the UTF-8 form of the string's code points. `GetBytes` never emits a byte-order mark; `GetString` does not treat a leading `EF BB BF` specially (it decodes to U+FEFF). Ill-formed sequences — overlong forms, unpaired-surrogate encodings, truncated tails — raise `FormatException`.
- **`UTF16`** (`Name` = `"utf-16le"`): little-endian, no BOM emitted. `GetString` requires an even byte count, consumes one leading little-endian BOM (`FF FE`) if present, and raises `FormatException` on an odd count, a big-endian BOM (`FE FF`), or an unpaired surrogate. Big-endian UTF-16 is not provided; a program that needs it byte-swaps explicitly.
- **`UTF32`** (`Name` = `"utf-32le"`): little-endian, no BOM emitted. `GetString` requires a byte count divisible by four, consumes one leading little-endian BOM if present, and raises `FormatException` on any unit that is not a Unicode scalar value (greater than `U+10FFFF` or in the surrogate range).
- **`ASCII`** (`Name` = `"us-ascii"`): strict seven-bit. `GetBytes` raises `FormatException` on any code point above `U+007F`; `GetString` raises `FormatException` on any byte above `0x7F`.

The `index`/`count` overload of `GetString` applies the same rules to the selected byte range; a BOM is consumed only at the start of the range.

<a id="sec-readonlystringspan"></a>
### 3.3 `ReadOnlyStringSpan`

A read-only view over a contiguous range of code points in a `string`. The natural companion to `string.AsSpan()` for zero-allocation substring operations — tokenization, parsing, prefix/suffix checks — without materializing intermediate string instances.

```bezel
public record ReadOnlyStringSpan(Source: string, Start: int, Length: int): IReadOnlySpan<char>

  public property [i: int]: char
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Source[Start + i];
    end;
  end;

  public readonly property IsEmpty: bool => Length = 0;

  // Range construction
  public method Slice(start: int, length: int): ReadOnlyStringSpan =>
    new ReadOnlyStringSpan(Source, this.Start + start, length);

  // Search by char
  public method IndexOf(value: char): int;
  public method Contains(value: char): bool;

  // Search by substring (span and string forms)
  public method IndexOf(value: ReadOnlyStringSpan): int;
  public method IndexOf(value: string): int;
  public method Contains(value: ReadOnlyStringSpan): bool;
  public method Contains(value: string): bool;

  // Prefix / suffix (span and string forms)
  public method StartsWith(prefix: ReadOnlyStringSpan): bool;
  public method StartsWith(prefix: string): bool;
  public method EndsWith(suffix: ReadOnlyStringSpan): bool;
  public method EndsWith(suffix: string): bool;

  // Equality (span and string forms)
  public method SequenceEqual(other: ReadOnlyStringSpan): bool;
  public method SequenceEqual(other: string): bool;
  public method SequenceEqual(other: IReadOnlySpan<char>): bool; // v0.108 — interface form

  // Bulk copy (v0.109)
  public method CopyTo(destination: Span<char>);    // copy chars to destination;
                                                     //   lengths must match

  // Materialization
  public method ToString(): string;
end ReadOnlyStringSpan;
```

A `ReadOnlyStringSpan` is a borrow-bearing record ([§9.7](Bezel_Language_Spec.md#sec-records-and-disposal) of the language spec) viewing into a contiguous range of a `string`. The `Source` field carries a borrow of a string; per the borrow-bearing-record rule, the span's lifetime is bounded by the source's lifetime. Strings are reference-typed and runtime-managed ([§4.6](Bezel_Language_Spec.md#sec-string)), so the lifetime story simplifies: as long as anything reachable from the program holds the string, the span remains valid.

`ReadOnlyStringSpan` is purely user-level Bezel — no compiler intrinsic. The record machinery, borrow lifetime tracking, and indexer dispatch are all standard language features.

**Why a separate type from `ReadOnlySpan<char>`.** `ReadOnlySpan<char>` (stdlib [§4.14](#sec-owned-buffers-and-spans)) views into an `OwningBuffer<char>` — a contiguous char buffer with each slot occupying `sizeof(char)` bytes. A `string`'s internal storage is adaptive (1, 2, or 4 bytes per code point per language spec [§4.6](Bezel_Language_Spec.md#sec-string)), so a string-backed view cannot share storage with a buffer-backed view. Two source types would force `ReadOnlySpan<T>`'s `Buffer` field to be polymorphic over source type, which would either change the existing record's shape (breaking v0.99-v0.103) or require a general source-polymorphism mechanism that v0.104 does not introduce. The separate `ReadOnlyStringSpan` type leaves `ReadOnlySpan<char>` unchanged and makes the source visible at API boundaries ("does this method want a buffer-backed view or a string-backed view?").

**Indexer access.** `span[i]` returns the code point at position `Start + i` in the source string, by delegating to the string's indexer (which dispatches on the string's internal storage width). The per-access overhead is small — one range check, one width-dispatched read — and the call site never sees the dispatch. For parsing work where the per-element cost is dominated by compare/branch logic, the overhead is negligible.

**Searches.** `IndexOf(value: char)` walks the span's range, returning the first index at which the code point matches `value`, or `-1` if none. `Contains(value: char)` returns `IndexOf(value) >= 0`.

`IndexOf(value: ReadOnlyStringSpan)` searches for the first occurrence of `value` as a contiguous substring. The algorithm is left to the implementation — naïve scan, Knuth-Morris-Pratt, Boyer-Moore are all admissible. The return is the start index of the match in the receiver, or `-1` if not found. `Contains(value: ReadOnlyStringSpan)` returns `IndexOf(value) >= 0`.

For convenience when comparing against string literals, `IndexOf(value: string)` and `Contains(value: string)` are also provided. They are equivalent to calling the `ReadOnlyStringSpan` overload after `value.AsSpan()` — `span.Contains("Content-Type:")` parses naturally without the user writing the explicit `.AsSpan()`. No allocation happens for the conversion; it is record construction only.

**Prefix and suffix.** `StartsWith(prefix)` returns `true` if the prefix matches the leading portion of the span; `EndsWith(suffix)` is the trailing analog. Both compare code-point-by-code-point — no Unicode normalization, no case folding. For locale-aware or Unicode-equivalent comparison, users go through the dedicated string-comparison APIs ([§9.6](#sec-string)). Each method has both a `ReadOnlyStringSpan` and a `string` overload, mirroring the search methods above.

**Equality.** `SequenceEqual(other)` returns `true` if the two spans have the same length and matching code points at every position. The comparison is pairwise; no normalization or case folding. A `SequenceEqual(other: string)` overload is provided for convenience.

**Materialization.** `ToString()` returns a `string` containing the span's code points. If the span covers the entire source string (`Start = 0` and `Length = Source.Length`), implementations may return the source string directly (no copy); otherwise materialization is required (a fresh string is allocated). This is the way to "escape" a span into a plain string when the caller needs to retain the chars beyond the source's lifetime.

**Iteration.** `for var c in span do ... end` is admitted. The desugaring is a direct indexed loop over the span's range, parallel to the buffer-backed span rule of language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for). The iteration variable's type is `char` and the loop body sees a copy of each code point.

**Construction.** Callers construct `ReadOnlyStringSpan` via the `AsSpan()` family on `string` ([§9.6](#sec-string)) rather than via the constructor directly. The direct constructor is admitted but is rarely needed — `s.AsSpan()` and its overloads cover the common cases.

<a id="sec-readonlyasciibytespan"></a>
### 3.4 `ReadOnlyAsciiByteSpan` (retired v0.188)

Retired. Earlier revisions defined `ReadOnlyAsciiByteSpan` here — a read-only byte view over the contiguous code points of an ASCII string, constructed via `string.AsAsciiByteSpan()` / `TryAsAsciiByteSpan(out)` ([§9.6](#sec-string)) — added in v0.106 as a zero-allocation alternative to `Encoding.UTF8.GetBytes` for FFI and I/O paths. The type and its construction surface are retired (v0.188, proposal 0038): the FFI half of its motivation never applied — a borrow-carrying record has no C-compatible form (language spec [§32.5](Bezel_Language_Spec.md#sec-ffi-compatible-types) admits only the §32.13 C-compatible records, which spans are not), and the boundary carriers for string bytes are `CString`/`RawBytes` ([§10](#sec-ffi-boundary-types)) — while the allocation-avoidance half is served for **all** strings, not only ASCII ones, by the v0.165 span-destination encoding: `Encoding.GetByteCount(s)` plus `GetBytes(s, destination: Span<byte>)` into a caller-reused `OwningBuffer<byte>` ([§3.2](#sec-encoding)), zero per-call allocation. For one-shot byte access, `Encoding.UTF8.GetBytes(s)` remains the always-correct allocating form; byte-span-consuming APIs are fed with the array's or buffer's own span (`bytes.AsReadOnlySpan()`, `buf.AsReadOnlySpan()`). The section number is retained so later-section citations stay stable; the decision record is the v0.188 changelog entry ([`Bezel_Changelog.md`](Bezel_Changelog.md)).

---

<a id="sec-system-collections-generic"></a>
## 4. `System.Collections.Generic`

The collection types. None are auto-imported except the iteration-interface family (the read-only and mutating interfaces), which is auto-imported per language spec [§2.3](Bezel_Language_Spec.md#sec-always-available-names).

The language separates **borrowing** collections (store references the caller continues to own; element disposal is the caller's responsibility) from **owning** collections (own their elements; element disposal is the collection's responsibility). Each kind has its own concrete class and its own interfaces; the two kinds share read-only iteration interfaces (`IEnumerable<T>`, `IReadOnlyCollection<T>`, `IReadOnlyList<T>`) because reading produces borrows in either case.

The naming convention (v0.168, #1372): borrowing collection types use their plain names (`Array<T>`, `LinkedList<T>`, etc.); owning collection types are prefixed `Owning…` (`OwningArray<T>`, `OwningLinkedList<T>`, etc.) — the prefix names the **element** axis ("a collection that *owns its elements*"), lexically distinct from the `owned` qualifier, which names the **container** axis ("a value *you* own"). `owned OwningArray<Dog>` therefore reads exactly as intended: an owning array that you own; a bare `OwningArray<Dog>` is a borrow of someone else's owning array. Interface members of the owning family carry the standard `I` prefix (`IOwningList`, `IOwningCollection`, `IOwningEnumerable`, `IOwningEnumerator`, and the async pair). The owning types constrain `where T: class` (and, for `OwningDictionary<K, V>`, `where V: class`) because ownership only makes sense for class-typed elements.

<a id="sec-ireadonlycollectiont-ireadonlylistt-and-ilistt"></a>
<a id="sec-borrowing-collection-interfaces"></a>
### 4.1 Borrowing collection interfaces

```bezel
public interface IReadOnlyCollection<out T>: IEnumerable<T>
  readonly property Count: int;
end IReadOnlyCollection;

public interface IReadOnlyList<out T>: IReadOnlyCollection<T>
  readonly property [i: int]: T;
end IReadOnlyList;

public interface ICollection<T>: IReadOnlyCollection<T>
  readonly property IsReadOnly: bool;
  method Add(item: T);
  method Clear();
  method Contains(item: T): bool;
  method Remove(item: T): bool;
  method DisposeElements();
end ICollection;

public interface IList<T>: IReadOnlyList<T>, ICollection<T>
  property [i: int]: T;
  method Insert(index: int, item: T);
end IList;
```

The four interfaces form the borrowing collection family. `IReadOnlyCollection<T>` and `IReadOnlyList<T>` are covariant in T (`out T`); `ICollection<T>` and `IList<T>` are invariant because they admit mutation through the type parameter.

`IsReadOnly` is `false` for the standard mutable collections (`Array<T>`, `LinkedList<T>`, `Set<T>`, `Stack<T>`, `Queue<T>`, `Dictionary<K, V>`) and `true` for read-only views over them. Calling mutating methods on a read-only collection raises `InvalidOperationException`.

`DisposeElements()` disposes every class-typed element and then clears the collection. For value-only types it is equivalent to `Clear()`. This is a destructive operation provided as a convenience for borrowing collections whose elements happen to be class instances that the caller wants to dispose en masse; it does *not* indicate that the collection owns its elements.

<a id="sec-owning-collection-interfaces"></a>
### 4.2 Owning collection interfaces

```bezel
public interface IOwningCollection<T>: IReadOnlyCollection<T>, IDisposable
  where T: class
  readonly property IsReadOnly: bool;
  method Add(owned item: T);
  method Clear();                            // disposes every element
  method Contains(item: T): bool;            // borrow-taking query
  method Remove(item: T): bool;              // disposes the removed element
end IOwningCollection;

public interface IOwningList<T>: IReadOnlyList<T>, IOwningCollection<T>
  where T: class
  property [i: int]: T
    get;
    set(owned);                              // takes owned T; disposes previous value
  end;
  method Insert(index: int, owned item: T);
end IOwningList;
```

The two owning-collection interfaces. They extend the borrowing read-only interfaces (`IReadOnlyCollection<T>`, `IReadOnlyList<T>`) for unified read-only access, and they additionally extend `IDisposable` because owning collections are themselves owned objects (their `Dispose()` cascades through their elements).

The mutating methods take `owned` parameters where appropriate: `Add` and `Insert` take ownership of the new element; the indexer setter takes ownership and disposes the previous slot value. Read-side methods (`Contains`, the indexer getter, iteration through the read-only-collection ancestor) produce borrows.

The `where T: class` constraint expresses that owning collections are meaningful only for elements carrying a disposal obligation, and the language spec ([§14.4](Bezel_Language_Spec.md#sec-owned-as-a-generic-argument)) states the enforced rule: element types are **class and interface types** (nullable-underlying admitted). **Value-typed elements are rejected at the instantiation site** (v0.165, resolving a three-way documentation contradiction in the class-only direction the v0.102 decision records — this paragraph briefly claimed otherwise): a value element has no disposal obligation, so `OwningArray<int>` says nothing `Array<int>` doesn't; use `Array<T>` or `OwningBuffer<T>` for value elements. Runtime-managed reference types (`string`, function values) are likewise rejected. Code that wants a borrowing collection uses the unprefixed name (`Array<…>`).

<a id="sec-borrowing-and-owning-collections-relate-via-iiterator-interfaces"></a>
<a id="sec-how-the-borrowing-and-owning-hierarchies-relate"></a>
### 4.3 How the borrowing and owning hierarchies relate

The two hierarchies share `IEnumerable<T>`, `IReadOnlyCollection<T>`, and `IReadOnlyList<T>` as common ancestors. A function that wants to read elements without taking ownership accepts one of these interfaces; both borrowing and owning collections of T conform.

```bezel
public method ReportEach<T>(items: IReadOnlyList<T>) where T: class
  for var c in items do Console.WriteLine(c.Name); end;
end ReportEach;

// Accepts both:
ReportEach(myArray);          // Array<Connection> → IReadOnlyList<Connection> ✓
ReportEach(myPool);            // OwningArray<Connection> → IReadOnlyList<Connection> ✓
```

The two hierarchies do *not* share mutating interfaces. `IList<T>` (borrowing) and `IOwningList<T>` (owning) are unrelated types with no subtyping; code that abstracts over "any mutable list" must pick one. This is intentional — the semantic of `Add` differs between the two (borrow-taking vs. owned-taking), so there is no useful single abstraction over both.

For draining iteration that transfers ownership, the owning collections expose `Drain()` returning `IOwningEnumerable<T>` (declared in [§4.13](#sec-owning-enumeration-interfaces)).

Every collection's element semantics — what `Add`, `Remove`, `Clear`, the indexer, iteration, and `Dispose` do with the elements — derive from the element slot policy of language spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy): the collection's family and the element type's category determine the slot kind, and the slot kind determines every element operation. The per-type sections below present that table for each concrete type; they introduce no element semantics of their own. The storage primitive embodying the owning column is `OwningBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)).

<a id="sec-arrayt"></a>
### 4.4 `Array<T>`

Borrowing resizable array. Stores references that the caller continues to own (or value-typed elements directly).

```bezel
public class Array<T>: IList<T>
  public constructor();
  public constructor(capacity: int);                     // empty (Count = 0), storage reserved;
                                                          //   capacity < 0: ArgumentOutOfRangeException (v0.183, #1396)
  public constructor(size: int, fill: T);                // Count = size, every slot = fill (v0.165, #1375);
                                                          //   size < 0: ArgumentOutOfRangeException (v0.183, #1396)

  public readonly property Count: int;
  public readonly property Capacity: int;
  public property [i: int]: T;

  public method Add(item: T);
  public method AddRange(items: IEnumerable<T>);
  public method Insert(index: int, item: T);
  public method RemoveAt(index: int);
  public method Remove(item: T): bool;
  public method Clear();
  public method Contains(item: T): bool;
  public method IndexOf(item: T): int;          // -1 if not found

  public method Sort(comparer: (T, T) -> int);  // in place
  public method Reverse();                       // in place

  public method Sorted(comparer: (T, T) -> int): owned Array<T>;  // fresh sorted copy
  public method Reversed(): owned Array<T>;                       // fresh reversed copy

  public method ToArray(): owned Array<T>;      // fresh heap-array copy
  public method DisposeElements();              // disposes each class-typed element

  // Span views (v0.107)
  public method AsSpan(): ArraySpan<T>;                          // full array, writable
  public method AsSpan(start: int): ArraySpan<T>;                // from offset to end
  public method AsSpan(start: int, length: int): ArraySpan<T>;
  public method AsSpan(r: Range<int>): ArraySpan<T>;             // parallels Range slicing

  public method AsReadOnlySpan(): ReadOnlyArraySpan<T>;
  public method AsReadOnlySpan(start: int): ReadOnlyArraySpan<T>;
  public method AsReadOnlySpan(start: int, length: int): ReadOnlyArraySpan<T>;
  public method AsReadOnlySpan(r: Range<int>): ReadOnlyArraySpan<T>;

  // Range slicing (language spec §15.6): `arr[r]` — arr[a..b], arr[..b], arr[a..], arr[..] —
  // is the compiler-recognized equivalent of this method (v0.182):
  public method Slice(r: Range<int>): owned Array<T>;   // fresh array; the range's values, or copies of the element borrows
end Array;

public extension Array<T> where T: IComparable<T>
  public method Sort();                    // in place
  public method Sorted(): owned Array<T>;  // fresh sorted copy
end;
```

A collection literal `[1, 2, 3]` constructs an `Array<T>` when the target type is `Array<T>`; it constructs an inline array `[N]T` when the target is an inline array. See language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals) for the contextual-typing rules.

`Array<T>` stores borrows: when the element type is a class, the elements are references whose ownership remains with whoever passed them in. The array's `Dispose()` is a no-op — `Array<T>` is trivial-dispose (language spec [§28.9](Bezel_Language_Spec.md#sec-runtime-managed-types), v0.184): the backing storage is reclaimed with the instance — and the elements are not disposed. To dispose owned class instances when clearing the collection, either call `DisposeElements()` (which disposes each class-typed element then clears) or use `OwningArray<T>` ([§4.5](#sec-ownedarrayt)) instead.

`Reverse`/`Sort` versus `Reversed`/`Sorted` follow a uniform naming convention: the imperative-verb form (`Reverse`, `Sort`) mutates the receiver in place and returns nothing; the past-participle form (`Reversed`, `Sorted`) leaves the receiver unchanged and returns a fresh `owned Array<T>` with the elements in the new order. The result's ownership mirrors `ToArray(): owned Array<T>` — it is a borrowing array (a fresh backing buffer holding the same element borrows), so the caller is responsible for disposing the array itself, not its elements. `Sorted` comes in two forms, mirroring `Sort`: the comparer-taking method on `Array<T>`, and the parameterless `Sorted()` in the `where T: IComparable<T>` extension that sorts by the natural order. `OwningArray<T>` deliberately has **no** value copy-forms — neither `Reversed`/`Sorted` nor `ToArray` — because its elements are uniquely owned: a copy would either double-own each element (two arrays claiming the same owned instances) or drain the source (leaving it empty). To reorder an `OwningArray<T>`, use the in-place `Reverse()`/`Sort()`; to move its elements into a new owning array, drain it (`Drain()`), which transfers ownership and empties the original.

**Sorting contract.** (1) **Stability.** `Array<T>`/`OwningArray<T>` `Sort`/`Sorted` (both the comparer and comparerless forms) are **not guaranteed stable** — equal elements may be reordered. `OrderBy`, `OrderByDescending`, `ThenBy`, and `ThenByDescending` ([§5](#sec-system-collections-linq)) **are stable** — equal keys retain source order (`ThenBy`'s compositional semantics require it). (2) **Comparer contract.** A comparer (or `CompareTo`) must induce a **total order**; under an inconsistent comparer the result is an **unspecified permutation of the original elements** — memory-safe, no element lost or duplicated. (3) **A comparer that throws mid-sort** propagates the exception and leaves the collection holding **exactly the original elements in unspecified order** (none lost, none duplicated). For `OwningArray<T>` this is the Appendix C disposed-exactly-once guarantee: every owned slot remains present and singly-held on the throw edge. (4) **Re-entrancy.** Structurally mutating a collection from inside its own sort comparer raises `InvalidOperationException` (the loud-failure discipline of language spec [§29.12](Bezel_Language_Spec.md#sec-concurrent-modification), extended from enumerator invalidation to sort callbacks).

`AsSpan()` and its overloads return an `ArraySpan<T>` (writable view) over a contiguous range of the array; `AsReadOnlySpan()` returns a `ReadOnlyArraySpan<T>`. These views are non-allocating record constructions and admit `Slice`, search, equality, and bulk-mutation methods. See [§4.15](#sec-arrayspant-readonlyarrayspant) for the full description; note that the mutation surface differs from `Span<T>`'s — it is uniform `Fill`/`CopyFrom`/`Reverse` for all T, without the class/value split that `Span<T>` requires, because `Array<T>` slots store borrows rather than owned values. `AsSpan` returns the writable form by default (matching `Array<T>`'s mutable nature); callers wanting read-only access either call `AsReadOnlySpan` directly or call `.AsReadOnly()` on the writable form. The four overload signatures of each form accept range parameters in the same style as v0.104's `string.AsSpan()`: no-arg (full used range, `0..<Count`), `(start)` (from offset to end), `(start, length)`, or `(r: Range<int>)`. Bounds-check semantics follow the standard Bezel convention: `IndexOutOfRangeException` if `start` or `length` is out of range against the array's `Count`.

<a id="sec-ownedarrayt"></a>
### 4.5 `OwningArray<T>`

Owning resizable array. Owns its elements; disposal cascades through them.

```bezel
public class OwningArray<T>: IOwningList<T> where T: class
  public constructor();
  public constructor(capacity: int);

  public readonly property Count: int;
  public readonly property Capacity: int;

  public property [i: int]: T
    get;                                     // borrow of slot
    set(owned);                              // takes owned T; disposes previous value
  end;

  public method Add(owned item: T);
  public method AddRange(owned items: OwningArray<T>);   // drains items into self
  public method Insert(index: int, owned item: T);
  public method RemoveAt(index: int);                    // disposes the removed element
  public method Remove(item: T): bool;                   // borrow-taking query;
                                                          //   disposes the matched element
  public method Clear();                                  // disposes every element
  public method Contains(item: T): bool;
  public method IndexOf(item: T): int;

  public method Sort(comparer: (T, T) -> int);          // comparer takes borrows
  public method Reverse();

  // Drain methods — transfer ownership of elements out of the collection
  public method PopAt(index: int): owned T;             // removes and returns owned
  public method PopFirst(): owned T;                     // = PopAt(0); raises if empty
  public method PopLast(): owned T;                      // = PopAt(Count - 1); raises if empty
  public method TryPopFirst(out owned item: T?): bool;   // v0.171 (#1336): the false path assigns null (§22.3)
  public method TryPopLast(out owned item: T?): bool;
  public method Drain(): IOwningEnumerable<T>;             // iteration drains the source
end OwningArray;

public extension OwningArray<T> where T: class, IComparable<T>
  public method Sort();
end;
```

`OwningArray<T>` owns each element. `Add`, `Insert`, and the indexer setter take ownership; `RemoveAt`, `Remove`, `Clear`, and the array's own `Dispose()` dispose the affected elements. `Sort`'s comparer parameter takes borrows of T (the comparer doesn't consume; it inspects).

The indexer getter produces a borrow of the slot's owned element; the borrow is valid until the slot is mutated (by indexer set, RemoveAt, Clear, or any operation that disposes the slot's contents). To take an element out, use `PopAt(i)` (returns owned T and removes the slot) or `move arr[i]` (returns owned T and leaves the slot in moved-from state; the caller must reassign before the next exit point — see language spec [§28.8.1](Bezel_Language_Spec.md#sec-the-move-expression)).

`Drain()` returns an `IOwningEnumerable<T>` ([§4.13](#sec-owning-enumeration-interfaces)) whose iteration transfers ownership of each element to the consumer. A `for var x in arr.Drain() do ... end` loop has owned iteration variables and consumes the array; the spec §28.3 discharge rules apply per iteration (an undischarged element auto-drops at the body's end with the must-use warning).

There is no `ToArray()` on `OwningArray<T>` that produces a non-owning copy: copying the array would manufacture duplicate ownership of each element. To produce a fresh `OwningArray<T>` with the same elements, the source array must be drained — the original ceases to own its elements.

`OwningArray<T>` does not declare `DisposeElements()` — disposing the array automatically disposes every element. The method would be redundant.

A collection literal `[c1, c2, c3]` with target type `OwningArray<Connection>` constructs an owning array and transfers ownership of each element in:

```bezel
public method Build(): owned OwningArray<Connection>
  return [
    Connect("a"),                          // each Connect returns owned Connection
    Connect("b"),
    Connect("c")
  ];                                       // collection literal targets OwningArray<Connection>
end Build;
```

<a id="sec-ownedarrayt-reference-implementation"></a>
<a id="sec-reference-implementation"></a>
#### Reference implementation

`OwningArray<T>` composes over `OwningBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)) in pure Bezel; the standard library provides this implementation directly, and user code is welcome to inspect or extend it.

```bezel
public class OwningArray<T>: IOwningList<T> where T: class
  private owned _storage: OwningBuffer<T>;
  private _count: int := 0;

  public constructor()
    this(8);
  end constructor;

  public constructor(capacity: int)
    if capacity < 0 then
      throw new ArgumentOutOfRangeException("capacity");
    end;
    _storage := new OwningBuffer<T>(capacity if capacity > 0 else 1);
  end constructor;

  public readonly property Count: int => _count;
  public readonly property Capacity: int => _storage.Length;

  public property [i: int]: T
    get => do
      BoundsCheck(i);
      return _storage[i]!;
    end;
    set(owned) => do
      BoundsCheck(i);
      _storage[i] := value;                       // owned-setter disposes previous
    end;
  end;

  public method Add(owned item: T)
    if _count = _storage.Length then
      GrowTo(_storage.Length * 2);
    end;
    _storage[_count] := item;
    _count := _count + 1;
  end Add;

  public method PopAt(index: int): owned T
    BoundsCheck(index);
    var taken := move _storage[index];             // taken: T? (owned)
    _storage[index] := null;                        // restore slot
    for var j in index..<(_count - 1) do
      var shifted := move _storage[j + 1];          // shifted: T? (owned)
      _storage[j + 1] := null;                       // restore shifted-from slot
      _storage[j] := shifted;                        // setter on moved-from slot;
                                                      //   no previous to dispose
    end;
    _count := _count - 1;
    return taken!;                                   // i < _count guaranteed non-null
  end PopAt;

  public method RemoveAt(index: int)
    using var _ := PopAt(index);                  // dispose at scope end
  end RemoveAt;

  public method Clear()
    for var i in 0..<_count do
      var item := move _storage[i];                // item: T? (owned)
      _storage[i] := null;                          // restore moved-from slot
      item!.Dispose();                              // slot was non-null
                                                     //   (i < _count guarantees it)
    end;
    _count := 0;
  end Clear;

  // ... and so on for Insert, Remove, Contains, IndexOf, Sort, Reverse,
  //     PopFirst, PopLast, TryPopFirst, TryPopLast, Drain, AddRange.

  private method BoundsCheck(i: int)
    if i < 0 or i >= _count then
      throw new IndexOutOfRangeException();
    end;
  end BoundsCheck;

  private method GrowTo(newCapacity: int)
    var fresh := new OwningBuffer<T>(newCapacity);
    for var i in 0..<_count do
      var moved := move _storage[i];                 // moved: T? (owned)
      _storage[i] := null;                            // restore moved-from slot
      fresh[i] := moved;                              // setter on moved-from slot of fresh
    end;
    _storage.Dispose();                              // dispose old buffer
                                                      //   (now all-null, no slots leak)
    _storage := fresh;
  end GrowTo;

  // No destructor declared; the compiler generates the default Dispose
  // which disposes _storage's contents (every non-null slot's owned T)
  // and the buffer's storage block, in that order.
end OwningArray;
```

The implementation is straightforward Bezel: a class with an owned `OwningBuffer<T>` field, a count, and the standard ownership-transferring operations. The `move`-and-null pattern from [§28.8.1](Bezel_Language_Spec.md#sec-the-move-expression) of the language spec discharges every slot before reassignment; the compiler verifies the field-restoration rule statically.

`GrowTo` is the resize operation. It allocates a fresh buffer, moves every live element from the old buffer to the new one (nulling the old slot after each move to satisfy the restoration rule), then disposes the old buffer. The old buffer's destructor sees only null slots and does no further work.

`Drain()`, `PopFirst()`, `PopLast()`, and the rest follow the same pattern: take from a slot via `move`, restore the slot with `null`, adjust `_count`. None of the operations require compiler-intrinsic knowledge of `OwningArray<T>` itself — the intrinsic surface is confined to `OwningBuffer<T>`.

The implementation example above omits some methods for brevity; the production standard library version provides the full surface listed in the class declaration above, all built over the same `OwningBuffer<T>` primitive.

<a id="sec-linkedlistt"></a>
### 4.6 `LinkedList<T>`

Borrowing doubly-linked list. Use when frequent insertion/removal in the middle is required. (Bezel deliberately does not name this type `List<T>` — programmers from C#, Java, Kotlin, and Python should reach for `Array<T>` for a dynamic array; `LinkedList<T>` is the explicit choice for a linked structure.)

```bezel
public class LinkedList<T>: ICollection<T>
  public constructor();
  public constructor(source: IEnumerable<T>);

  public readonly property Count: int;
  public readonly property FirstNode: LinkedListNode<T>?;
  public readonly property LastNode: LinkedListNode<T>?;
  public readonly property First: T?;
  public readonly property Last: T?;

  public method AddFirst(item: T): LinkedListNode<T>;
  public method AddLast(item: T): LinkedListNode<T>;
  public method AddBefore(node: LinkedListNode<T>, item: T): LinkedListNode<T>;
  public method AddAfter(node: LinkedListNode<T>, item: T): LinkedListNode<T>;
  public method RemoveFirst();
  public method RemoveLast();
  public method RemoveNode(node: LinkedListNode<T>);
  public method Find(item: T): LinkedListNode<T>?;
  // Plus the standard ICollection<T> operations: Add, Clear, Contains, Remove,
  // IsReadOnly, DisposeElements.
end LinkedList;

public class LinkedListNode<T>
  public readonly property Value: T;
  public readonly property Next: LinkedListNode<T>?;
  public readonly property Previous: LinkedListNode<T>?;
end LinkedListNode;
```

`LinkedListNode<T>` instances are owned by their containing `LinkedList<T>`. Methods returning `LinkedListNode<T>` return a borrow whose lifetime is the list's lifetime.

A `LinkedList<T>` may be constructed with a sequence literal (language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)); textual order is head-to-tail: `using var l: LinkedList<int> := [10, 20, 30];` produces a list whose `First` is `10` and `Last` is `30`. The owning form `OwningLinkedList<T>` accepts the same literal with element expressions producing owned values.

`List<T>` is not defined in this version. The name is reserved.

<a id="sec-ownedlinkedlistt"></a>
### 4.7 `OwningLinkedList<T>`

Owning doubly-linked list.

```bezel
public class OwningLinkedList<T>: IOwningCollection<T> where T: class
  public constructor();

  public readonly property Count: int;
  public readonly property FirstNode: LinkedListNode<T>?;       // borrow if non-null
  public readonly property LastNode: LinkedListNode<T>?;
  public readonly property First: T?;                           // borrow
  public readonly property Last: T?;

  public method AddFirst(owned item: T): LinkedListNode<T>;
  public method AddLast(owned item: T): LinkedListNode<T>;
  public method AddBefore(node: LinkedListNode<T>, owned item: T): LinkedListNode<T>;
  public method AddAfter(node: LinkedListNode<T>, owned item: T): LinkedListNode<T>;
  public method RemoveFirst();                                  // disposes
  public method RemoveLast();                                   // disposes
  public method RemoveNode(node: LinkedListNode<T>);            // disposes
  public method Clear();                                         // disposes every element
  public method Contains(item: T): bool;                         // borrow-taking
  public method Find(item: T): LinkedListNode<T>?;               // borrow-taking
  public method Remove(item: T): bool;                           // disposes the matched

  // Drain methods
  public method PopFirst(): owned T;                            // raises if empty
  public method PopLast(): owned T;
  public method TryPopFirst(out owned item: T?): bool;   // v0.171 (#1336)
  public method TryPopLast(out owned item: T?): bool;
  public method Drain(): IOwningEnumerable<T>;
end OwningLinkedList;
```

`LinkedListNode<T>` (declared in [§4.6](#sec-linkedlistt)) is reused. On `OwningLinkedList<T>`, the node's `Value` property returns a borrow of the node's element; the node still owns the element until the node is removed. To take an element out, use `RemoveNode` (which disposes), `PopFirst` / `PopLast` (transfer ownership to caller), or `Drain()`.

The `move` operator does not extend to linked-list nodes in this version — removing a value from a specific node would leave the node in an inconsistent state, and the surrounding-node-pointer integrity makes the move-out-and-restore pattern impractical. Future revisions may add a node-level move form.

<a id="sec-dictionaryk-v"></a>
### 4.8 `Dictionary<K, V>`

Borrowing hash-based map. `K` must be **equality-bearing** (spec §3.6/§8.3) — enforced at the instantiation site per §3.6 rule 2: a `Dictionary<K, V>` (or `OwningDictionary<K, V>`) whose `K` is a record that is neither auto-equality-bearing nor declares user equality is rejected with a diagnostic naming the property. (Added in v0.158, replacing the previous unenforceable prose requirement.)

**Stability while stored.** While a value is stored as a **key** of a `Dictionary`/`OwningDictionary` or a `SortedDictionary`, or an **element** of a `Set`/`OwningSet` or `SortedSet`, the program must not mutate any state consulted by its `Equals`/`GetHashCode` / the collection's `IEqualityComparer` (hash forms) or `CompareTo` (ordered forms). Violation leaves the collection's observable behavior **unspecified** — entries may become unfindable by lookup/`Remove` while still visible to iteration and `Count` — but **memory-safe**: no slot is lost or double-held, and an owning collection still disposes every owned element exactly once at collection disposal/`Clear` (the Appendix C guarantee is unconditional). A class-typed key/element is held by **borrow** (the [§28.11](Bezel_Language_Spec.md#sec-borrowing-collections) borrowing column), so its owner may still mutate it while it is stored — this rule is the caller's obligation, not a compile-time guarantee.

```bezel
public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<K>);
  public constructor(capacity: int, comparer: IEqualityComparer<K>);

  public readonly property Count: int;
  public readonly property Keys: IReadOnlyCollection<K>;      // v0.165 (#1354): read-only views, matching §4.9
  public readonly property Values: IReadOnlyCollection<V>;

  public property [key: K]: V;

  public method Add(key: K, value: V);
  public method ContainsKey(key: K): bool;
  public method Contains(key: K): bool;            // alias for ContainsKey;
                                                   //   enables `key in dict`
  public method Remove(key: K): bool;
  public method TryGetValue(key: K, out value: V?): bool;   // v0.171 (#1336): false path assigns null; value-typed V reads as V? at the call site
  public method GetOrAdd(key: K, factory: (K) -> V): V;

  public method Clear();
  public method DisposeElements();                  // disposes class-typed
                                                    //   keys and values
end Dictionary;

public record DictionaryEntry<K, V>(Key: K, Value: V);

public interface IEqualityComparer<T>
  method Equals(a: T, b: T): bool;
  method GetHashCode(value: T): int;
end IEqualityComparer;
```

Indexer `dict[key]` raises `KeyNotFoundException` if the key is not present. Use `TryGetValue` for safe lookup. The exception is declared here (v0.182 — previously referenced without a declaration), in `System.Collections.Generic` per the producer-locality rule (language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)); it derives directly from `Exception` — deliberately not from `InvalidOperationException`, so that a state-fault catch arm does not silently swallow missing-key lookups:

```bezel
public class KeyNotFoundException: Exception
  public constructor();
  public constructor(message: string);
end KeyNotFoundException;
```

The write surface is the indexer and `Add`: `dict[key] := value` inserts the key or updates an existing one (add-or-update), while `Add(key, value)` inserts and raises `ArgumentException` if the key is already present. There is no separate `Set` method — neither here nor on any other collection (`OwningDictionary` in [§4.9](#sec-owneddictionaryk-v) writes through its owned-taking indexer-set `dict[key] := value`, and the array/list/stack/queue types write through their own indexer or `Add`/`Push`/`Enqueue`). A call to a `Set` method on any collection is rejected as an unknown member.

Iteration over a `Dictionary<K, V>` — `for var entry in dict do ... end` — yields `DictionaryEntry<K, V>` values (the element type of the `ICollection<DictionaryEntry<K, V>>` conformance), one per live entry in a single pass, in insertion order. `entry.Key` and `entry.Value` read the entry; for class-typed values, `entry.Value` is a *borrow* of the stored value — the dictionary retains ownership (the borrowing column of language spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy)). Removing a key and re-adding it places it at the end of the iteration order. `Clear()` resets the order. `Contains(key)` is an alias for `ContainsKey(key)`, enabling the `in` operator (language spec [§17.5](Bezel_Language_Spec.md#sec-the-in-operator)) to test key membership: `key in dict` is equivalent to `dict.ContainsKey(key)`.

**Construction via collection literal.** A `Dictionary<K, V>` may be constructed with the dictionary-literal form `[k1: v1, k2: v2, ...]` (language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)):

```bezel
using var ages: Dictionary<string, int> := [
  "alice": 30,
  "bob":   25
];
```

The key expressions have type `K`, the value expressions type `V`. Literal element order is the insertion order, which is observable because iteration is insertion-ordered. Duplicate keys that are all compile-time constants are a compile error; otherwise the last occurrence wins. The empty dictionary is `[]` with a `Dictionary<K, V>`-typed target.

<a id="sec-owneddictionaryk-v"></a>
### 4.9 `OwningDictionary<K, V>`

Owning hash-based map. Owns its values; keys are value-like (typically `string`, `int`, or a record with value equality). The typical owning-map form.

```bezel
public class OwningDictionary<K, V>: IDisposable where V: class
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<K>);
  public constructor(capacity: int, comparer: IEqualityComparer<K>);

  public readonly property Count: int;
  public readonly property Keys: IReadOnlyCollection<K>;       // borrowing view
  public readonly property Values: IReadOnlyCollection<V>;     // borrowing view

  public property [key: K]: V
    get;                                       // borrow of stored value
    set(owned);                                // owned-taking; disposes previous
  end;

  public method Add(key: K, owned value: V);
  public method ContainsKey(key: K): bool;
  public method Contains(key: K): bool;
  public method Remove(key: K): bool;          // disposes the removed value
  public method TryGetValue(key: K, out value: V?): bool;      // out is borrow; v0.171 (#1336)
  public method GetOrAdd(key: K, factory: (K) -> owned V): V;  // returns borrow

  // Drain
  public method PopValue(key: K): owned V;     // raises if not present
  public method TryPopValue(key: K, out owned value: V?): bool;   // v0.171 (#1336)
  public method Drain(): IOwningEnumerable<DrainedEntry<K, V>>;
                                               // iteration drains the dictionary;
                                               //   each yielded entry owns its Value

  public method Clear();                        // disposes every value
end OwningDictionary;

public class DrainedEntry<K, V>: IDisposable where V: class
  public constructor(key: K, owned value: V);  // ownership of value transfers in
  public readonly property Key: K;             // borrow / value copy of the key
  public owned Value: V;                        // owned — DrainedEntry is a class,
                                                //   so it may own its Value field
end DrainedEntry;
```

`OwningDictionary<K, V>` requires `V: class` (the value type is owned). Keys can be any type with the standard equality contract.

The interface conformance is `IDisposable` only — not `IOwningCollection<DictionaryEntry<K, V>>`. The `DictionaryEntry<K, V>` record (used by the borrowing `Dictionary<K, V>` in [§4.8](#sec-dictionaryk-v)) is a record, and records cannot have an owned `Value` field per language spec [§28.5](Bezel_Language_Spec.md#sec-owned-fields); a borrowing entry view over an owning dictionary would hand out a borrow of a value the dictionary still owns, which is the upcast-iteration behavior, not an ownership-transferring drain. For the ownership-transferring case, `Drain()` (below) yields `DrainedEntry<K, V>` — a *class*, which per the same [§28.5](Bezel_Language_Spec.md#sec-owned-fields) rule *may* own its `Value`. Default `for var entry in ownedDict do` iteration is not provided **on `OwningDictionary<K, V>`** — its common intent is ownership transfer, and a borrowing look-don't-take entry view over an owning dictionary is a separate decision deliberately not taken here; use either the key-iteration pattern (borrowing) or `Drain()` (ownership-transferring). (The borrowing `Dictionary<K, V>` *does* provide single-pass entry iteration — [§4.8](#sec-dictionaryk-v).)

```bezel
// Borrowing key-iteration, one value taken at a time:
public method DrainAndProcess(dict: OwningDictionary<string, Connection>)
  using var keys := new Array<string>(dict.Count);
  for var key in dict.Keys do
    keys.Add(key);
  end;
  for var key in keys do
    using var conn := dict.PopValue(key);     // takes ownership
    Process(conn);
  end;
  // dict is now empty
end DrainAndProcess;

// Single-call drain — each entry owns its value:
public method DrainAndProcess2(dict: OwningDictionary<string, Connection>)
  for var entry in dict.Drain() do
    using var e := owned entry;                // entry is owned; adopt to discharge (v0.180 marker)
    Process(e.Key, e.Value);                   // e.Value is the owned Connection
  end;
  // dict is now empty; each entry (and its Value) disposed at loop-body end
end DrainAndProcess2;
```

`Drain()` returns `IOwningEnumerable<DrainedEntry<K, V>>` ([§4.13](#sec-owning-enumeration-interfaces)). Iterating it transfers ownership of each `DrainedEntry<K, V>` to the consumer; the entry is a class that owns its `Value`, so adopting the iteration variable with `using var e := owned entry;` (the v0.180 transfer marker, language spec [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)) — or otherwise discharging it — disposes both the entry shell and, via the standard owned-field cascade (language spec [§28.5](Bezel_Language_Spec.md#sec-owned-fields)), the contained value. Partial iteration drains only the entries yielded; the rest remain in the dictionary. This is the single-call drain; no language feature beyond the existing owning-class and `IOwningEnumerable<T>` machinery is required — `DrainedEntry<K, V>` owns its `Value` exactly as any class owns an `owned` field. The `Key` is carried by value (or as a borrow for class-typed K, following the standard field rules).

`GetOrAdd(key, factory)`'s factory produces an `owned V`; the dictionary stores the value (taking ownership) and the call returns a borrow of the now-stored value.

There is no `DisposeElements()` on `OwningDictionary<K, V>` — the dictionary's `Dispose()` cascade handles every value disposal.

The `Keys` and `Values` properties return read-only borrowing views; their type is `IReadOnlyCollection<K>` and `IReadOnlyCollection<V>` (the unmodified types, since reading produces borrows). Code that needs to mutate the dictionary calls `Add`, `Remove`, etc. directly.

**Construction via collection literal.** An `OwningDictionary<K, V>` may be constructed with the dictionary-literal form `[k1: v1, ...]`; each value expression must produce an `owned V`, and ownership transfers into the dictionary:

```bezel
using var pool: OwningDictionary<string, Connection> := [
  "primary": Connect("p"),
  "backup":  Connect("b")
];
```

If a literal contains duplicate keys that are not all compile-time constants, the last value wins and each displaced earlier value is disposed at construction (consistent with the runtime last-write-wins rule). The empty owned dictionary is `[]` with an `OwningDictionary<K, V>`-typed target.

`OwningDictionary` does **not** support owned keys (`OwningDictionary<owned K, V>` and similar forms are not provided). Realistic key types are value-like (string, int, records, etc.); cases for owned class-typed keys are rare to nonexistent in practice, and the v0.96-era four-way Add declaration that admitted them was contortion for a use case that didn't motivate the complexity.

<a id="sec-owneddictionaryk-v-reference-implementation"></a>
#### Reference implementation

`OwningDictionary<K, V>` composes over `OwningBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)) in pure Bezel. The implementation is more involved than `OwningArray<T>`'s because mutation through a chain of borrowed pointers is incompatible with Bezel's borrow-lifetime tracker (see "Note on chain mutation" below); the implementation works in terms of *drain-and-rebuild* on each bucket.

The structure: each bucket of an `OwningBuffer<Entry<K, V>>` holds an owned `Entry<K, V>` (or null), where each entry has an `owned Next: Entry<K, V>?` field forming a chain of synonym-hash entries. The `Value` field is declared nullable (`owned Value: V?`) so the `move`-and-null restoration pattern can be used during take operations; in committed state every entry's `Value` is non-null.

```bezel
// Internal entry type.
private class Entry<K, V> where V: class
  public Key: K;
  public owned Value: V?;          // nullable for move-and-null restoration;
                                    //   non-null in committed dictionary state
  public owned Next: Entry<K, V>?;  // chain link
  public constructor(key: K, owned value: V, owned next: Entry<K, V>?)
    Key := key; Value := value; Next := next;
  end constructor;
end Entry;

public class OwningDictionary<K, V>: IDisposable where V: class
  private owned _buckets: OwningBuffer<Entry<K, V>>;
  private _count: int := 0;
  private _comparer: IEqualityComparer<K>;

  // ... constructors, BucketIndex, Count, Keys, Values, etc.

  public method Add(key: K, owned value: V)
    var idx := BucketIndex(key);
    // Drain the bucket's chain so we can walk it as owned local state
    // (Bezel's borrow tracker rejects walks-with-mutation; see note below).
    // Reverse the chain as we walk; if we don't find the key, the new entry
    // is prepended to the reversed chain — chain reversal is unobservable
    // because OwningDictionary doesn't promise insertion order.
    var chain := move _buckets[idx];
    _buckets[idx] := null;
    var rebuilt: Entry<K, V>? := null;
    var foundDup := false;
    while chain <> null do
      var rest := move chain.Next;
      if not foundDup and _comparer.Equals(chain.Key, key) then
        foundDup := true;              // test the key BEFORE the node is transferred
      end;
      chain.Next := rebuilt;           // restore (with the rebuilt-chain head)
      rebuilt := owned chain;          // ownership transfers in (v0.180 marker)
      chain := owned rest;             // (v0.180 marker)
    end;
    if foundDup then
      _buckets[idx] := rebuilt;       // restore bucket with unchanged chain
      throw new ArgumentException("duplicate key");
      // The owned `value` parameter is auto-disposed on throw per the
      // [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) exception-path discharge rule.
    end;
    _buckets[idx] := new Entry<K, V>(key, value, rebuilt);
    _count := _count + 1;
    MaybeGrow();
  end Add;

  public method PopValue(key: K): owned V
    var idx := BucketIndex(key);
    var chain := move _buckets[idx];
    _buckets[idx] := null;
    var rebuilt: Entry<K, V>? := null;
    var taken: V? := null;
    while chain <> null do
      var rest := move chain.Next;
      if taken = null and _comparer.Equals(chain.Key, key) then
        // Target: take its Value, dispose the entry node.
        taken := move chain.Value;
        chain.Value := null;             // restore (with null)
        chain.Next := null;              // restore (with null)
        chain.Dispose();                  // value and next are both null;
                                          //   destructor's auto-dispose passes are no-ops
      else
        chain.Next := rebuilt;            // restore (with rebuilt-chain head)
        rebuilt := owned chain;          // (v0.180 marker)
      end;
      chain := owned rest;               // (v0.180 marker)
    end;
    _buckets[idx] := rebuilt;             // restore bucket
    if taken = null then
      throw new KeyNotFoundException();
    end;
    _count := _count - 1;
    return taken!;
  end PopValue;

  // The rest of the implementation (TryGetValue with read-only chain walk,
  // ContainsKey, indexer get/set, GetOrAdd, Clear, MaybeGrow which calls Grow
  // when the load factor is exceeded, Grow which allocates a new larger
  // _buckets and rehashes every entry, Keys, Values, etc.) follows the same
  // patterns as Add and PopValue: every bucket mutation is a drain-and-rebuild.

  // No destructor declared; the compiler generates the default Dispose,
  // which cascades through _buckets → each Entry → each Entry's owned
  // Next and Value, recursively disposing the whole map.
end OwningDictionary;
```

The structural insight: **every bucket mutation is expressed as taking the chain head with `move`, walking the owned chain as local state, and storing a rebuilt chain back into the bucket**. No method mutates a chain through a borrow into shared storage. Every `move buf[i]` and `move entry.X` is followed by an explicit restoration (`buf[i] := newValue`, `entry.X := null` or `entry.X := newOwner`). The compiler verifies the field-restoration rule statically.

**Note on chain mutation.** A naïve implementation might walk the chain with a borrow `probe: Entry<K, V>?` and surgically modify links (`prev.Next := probe.Next`, etc.). Bezel's borrow-lifetime tracker rejects this pattern: `probe`'s borrow source is rooted in `_buckets[idx]`, and mutating any link along the chain — or mutating the slot itself — invalidates the borrow chain, after which subsequent reads of `probe.X` are borrow-after-mutation errors. The drain-and-rebuild pattern sidesteps the issue by walking the chain as owned local state. The cost is `O(chain length)` work per mutation rather than `O(1)`. In practice, chain lengths are bounded by the dictionary's load factor (typically ≤ 8), so the constant factor is small; for use cases where this cost is unacceptable, an open-addressing implementation can be substituted without changing the public API.

**Note on iteration order.** The drain-and-rebuild pattern reverses chain order on every bucket mutation. Iteration over `OwningDictionary<K, V>` therefore yields entries in an order that depends on insertion-and-mutation history rather than purely on insertion order. The standard library makes no insertion-order guarantee for `OwningDictionary` (in contrast to `Dictionary<K, V>` at [§4.8](#sec-dictionaryk-v), which does); programs that need ordered iteration use a different collection.

**Why no compiler intrinsic for `OwningDictionary`.** None of the operations above require compiler-intrinsic knowledge of `OwningDictionary<K, V>`. Every `move` goes through the field-restoration rule that the compiler enforces on `move buf[i]` and on `move entry.X`. The intrinsic surface of the owning-collection family is once again confined to `OwningBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)).

<a id="sec-sett"></a>
<a id="sec-sett-and-ownedsett"></a>
### 4.10 `Set<T>` and `OwningSet<T>`

Borrowing and owning hash-based sets. `T` must be **equality-bearing** (spec §3.6/§8.3), enforced at the instantiation site per §3.6 rule 2 — same rule as `Dictionary<K, V>` keys (§4.8). (Added in v0.158.)

**Iteration order.** Iteration over a `Set<T>` yields elements in **insertion order** — removing and re-adding an element places it at the end — the same guarantee as `Dictionary<K, V>` ([§4.8](#sec-dictionaryk-v)). `OwningSet<T>` makes **no** iteration-order guarantee, for [§4.9](#sec-owneddictionaryk-v)'s drain-and-rebuild reason; programs needing ordered iteration use `SortedSet<T>` or an `Array`.

```bezel
public class Set<T>: ICollection<T>
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<T>);

  // ICollection<T> operations (including Add(item: T) returning ()) plus:
  public method TryAdd(item: T): bool;                 // returns whether added
                                                       //   (false if duplicate)
  public method UnionWith(other: IEnumerable<T>);
  public method IntersectWith(other: IEnumerable<T>);
  public method ExceptWith(other: IEnumerable<T>);
  public method IsSubsetOf(other: IEnumerable<T>): bool;
  public method IsSupersetOf(other: IEnumerable<T>): bool;
end Set;

public class OwningSet<T>: IOwningCollection<T> where T: class
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<T>);

  // IOwningCollection<T> operations plus:
  public method TryAdd(owned item: T): bool;          // returns whether added
                                                       //   (false if duplicate;
                                                       //   passed item is disposed)
  // Drain
  public method Take(query: T): owned T;               // borrow-taking query;
                                                       //   returns owned matched
  public method TryTake(query: T, out owned removed: T?): bool;
  public method Drain(): IOwningEnumerable<T>;

  public method UnionWith(owned other: OwningSet<T>);   // drains other into self;
                                                       //   duplicates disposed
  public method IntersectWith(other: IEnumerable<T>);  // disposes elements not in other
  public method ExceptWith(other: IEnumerable<T>);     // disposes elements in other
  public method IsSubsetOf(other: IEnumerable<T>): bool;
  public method IsSupersetOf(other: IEnumerable<T>): bool;
end OwningSet;
```

`OwningSet<T>` inherits `Add(owned item: T)` from `IOwningCollection<T>` — this form returns `()` and silently disposes the item on duplicate. For callers that want to know whether the add succeeded, `TryAdd(owned item: T): bool` returns the insertion result; on `false` (duplicate), the passed-in item is disposed. The separate name avoids the return-type conformance issue ([§11.2](Bezel_Language_Spec.md#sec-interface-declaration) of the language spec requires exact return-type match on overrides; the borrowing `Set<T>` similarly inherits `Add(item: T)` returning `()` and provides `TryAdd(item: T): bool` for the bool-returning case).

`OwningSet<T>.Add` is the case where the duplicate-rejection rule has an ownership consequence. When the caller passes `owned item` and the set already contains an equal element, `Add` returns `()` and *disposes the passed-in item* — the caller relinquished ownership at the call site, and the set has no use for the duplicate. Callers who need to react to the duplicate path should call `TryAdd` instead, or pre-check with `Contains`:

```bezel
if not pool.Contains(candidate) then
  pool.Add(owned candidate);   // ownership transfers (v0.169 call-site marker)
end;
// On the duplicate path, candidate retains ownership.
```

`Take(query: T)` finds the equal element in the set, removes it, and returns ownership. The query and the stored element are equal by the set's `Equals` rule but may be distinct instances. `TryTake` is the no-throw variant.

A `Set<T>` may be constructed with a sequence literal (language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)); duplicates (per `T`'s equality contract) are silently collapsed, the first textual occurrence retained: `using var s: Set<int> := [1, 2, 2, 3];` yields `{1, 2, 3}`. For `OwningSet<T>`, each later duplicate's owned element is disposed at construction. The empty set is `[]` with a `Set<T>`-typed (or `OwningSet<T>`-typed) target.

<a id="sec-stackt-and-queuet"></a>
<a id="sec-stackt-ownedstackt-queuet-and-ownedqueuet"></a>
### 4.11 `Stack<T>`, `OwningStack<T>`, `Queue<T>`, and `OwningQueue<T>`

LIFO and FIFO collections.

```bezel
public class Stack<T>: IReadOnlyCollection<T>   // v0.165 (#1354): the v0.94 owned-twin fix applied here too; Contains stays via the explicit member
  public constructor();
  public method Push(item: T);
  public method Pop(): T;             // raises if empty
  public method Peek(): T;
  public method TryPop(out item: T?): bool;    // v0.171 (#1336)
  public method TryPeek(out item: T?): bool;
end Stack;

public class OwningStack<T>: IOwningCollection<T> where T: class
  public constructor();
  public method Push(owned item: T);
  public method Pop(): owned T;                   // raises if empty; transfers out
  public method Peek(): T;                         // borrow of top element
  public method TryPop(out owned item: T?): bool;   // v0.171 (#1336)
  public method TryPeek(out item: T?): bool;       // out is borrow
  public method Clear();                           // disposes every element
  public method Drain(): IOwningEnumerable<T>;
end OwningStack;

public class Queue<T>: IReadOnlyCollection<T>   // v0.165 (#1354)
  public constructor();
  public method Enqueue(item: T);
  public method Dequeue(): T;
  public method Peek(): T;
  public method Contains(item: T): bool;        // explicit (v0.165)
  public method TryDequeue(out item: T?): bool;   // v0.171 (#1336)
  public method TryPeek(out item: T?): bool;
end Queue;

public class OwningQueue<T>: IOwningCollection<T> where T: class
  public constructor();
  public method Enqueue(owned item: T);
  public method Dequeue(): owned T;
  public method Peek(): T;                         // borrow of front element
  public method TryDequeue(out owned item: T?): bool;   // v0.171 (#1336)
  public method TryPeek(out item: T?): bool;       // out is borrow
  public method Clear();                           // disposes every element
  public method Drain(): IOwningEnumerable<T>;
end OwningQueue;
```

For class T on the borrowing forms, `Pop`, `Peek`, and `Dequeue` produce borrows whose source is the same as the source of whatever was pushed or enqueued; the stack and queue are transit points, not sources. Storing the popped value in a long-lived field of another object is a borrow-lifetime violation per language spec [§28.6](Bezel_Language_Spec.md#sec-leak-prevention).

On the owning forms, `Pop` and `Dequeue` return ownership; the collection no longer holds the element. This resolves the borrow-source story cleanly: when the stack owns its elements, the popped element transfers ownership to the caller with the standard discharge obligation.

**Construction via collection literal.** All four types accept a sequence literal (language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)). The ordering convention matters and differs between stack and queue:

- `Queue<T>` / `OwningQueue<T>`: the first literal element is at the front (dequeued first). `using var q: Queue<int> := [1, 2, 3];` dequeues `1`, `2`, `3` in that order.
- `Stack<T>` / `OwningStack<T>`: the literal lists elements bottom-to-top — the first element is pushed first (ends up at the bottom), the last element is on top (popped first). `using var s: Stack<int> := [1, 2, 3];` pops `3`, `2`, `1` in that order.

The bottom-to-top reading for stacks was chosen so that the literal's left-to-right order matches the push order; a reader can trace `[1, 2, 3]` as "push 1, push 2, push 3." For owning forms, element expressions must produce owned values. The empty collection is `[]` with the corresponding typed target.

<a id="sec-borrowing-enumeration-interfaces"></a>
### 4.12 Borrowing enumeration interfaces

```bezel
public interface IEnumerable<out T>
  method GetEnumerator(): owned IEnumerator<T>;
end IEnumerable;

public interface IEnumerator<out T>: IDisposable
  readonly property Current: T;         // non-destructive: borrow of current value
  method MoveNext(): bool;
end IEnumerator;
```

The standard iteration interfaces. `IEnumerable<T>` is covariant in T. `IEnumerator<T>.Current` is non-destructive — it returns a borrow of the most-recently-yielded value, which the enumerator continues to own (if it has ownership; if T is a borrowing element type, Current returns a re-borrow).

These interfaces are auto-imported per language spec [§2.3](Bezel_Language_Spec.md#sec-always-available-names) because they participate in the for-loop desugaring.

`IAsyncEnumerable<T>` and `IAsyncEnumerator<T>` are the async variants; they participate in `await for` similarly:

```bezel
public interface IAsyncEnumerable<out T>
  method GetAsyncEnumerator(token: CancellationToken := CancellationToken.None): owned IAsyncEnumerator<T>;
end IAsyncEnumerable;

public interface IAsyncEnumerator<out T>: IAsyncDisposable
  readonly property Current: T;
  method MoveNextAsync(): Task<bool>;
end IAsyncEnumerator;
```

<a id="sec-owning-enumeration-interfaces"></a>
### 4.13 Owning enumeration interfaces

```bezel
public interface IOwningEnumerable<T>: IDisposable where T: class
  method GetOwningEnumerator(): owned IOwningEnumerator<T>;
end IOwningEnumerable;

public interface IOwningEnumerator<T>: IDisposable where T: class
  method TakeCurrent(): owned T;        // transfers ownership (v0.161; was destructive property Current)
  method MoveNext(): bool;
end IOwningEnumerator;
```

The owning iteration interfaces. `IOwningEnumerator<T>.TakeCurrent()` (v0.161 — replaces the destructive `Current` property) transfers ownership of the most-recently-yielded value to the caller after a successful `MoveNext`; a second take without an intervening advance, or a take before the first advance/after exhaustion, raises `InvalidOperationException` (spec §29.1). The for-loop desugaring at language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) calls `TakeCurrent()` exactly once per iteration.

A `for var x in src do ... end` where `src` is an `IOwningEnumerable<T>` produces owned iteration variables. The loop body must discharge each `x` on every normal-exit edge (fall-through, `break`, `continue`, `return`); the standard owned-local discharge rules ([§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)) apply. On exception edges the iteration variable is auto-disposed by the implicit `try`/`finally` rule of [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) "Exception-path discharge."

These interfaces are auto-imported alongside the borrowing iteration family.

The async variants:

```bezel
public interface IOwningAsyncEnumerable<T>: IAsyncDisposable where T: class
  method GetOwningAsyncEnumerator(token: CancellationToken := CancellationToken.None): owned IOwningAsyncEnumerator<T>;
end IOwningAsyncEnumerable;

public interface IOwningAsyncEnumerator<T>: IAsyncDisposable where T: class
  method TakeCurrent(): owned T;        // transfers ownership (v0.161)
  method MoveNextAsync(): Task<bool>;
end IOwningAsyncEnumerator;
```

`IOwningAsyncEnumerable<T>` is the return type for async iterator methods that yield owned values (asynchronously-produced owned resources, async drain operations on owning collections). The `TakeCurrent()` rule applies to `IOwningAsyncEnumerator<T>` the same way (relative to `MoveNextAsync`); `await for var x in src do ... end` produces owned iteration variables; the spec §28.3 discharge rules (auto-drop with must-use warning) apply in the body.

A type may implement both `IEnumerable<T>` and `IOwningEnumerable<T>`. `OwningArray<T>` does so: default iteration via `IEnumerable<T>` yields borrows of T (the standard read-only-iteration semantics); drain iteration via `IOwningEnumerable<T>` yields owned T values that the consumer must discharge. The for-loop desugaring picks based on the static type of the source expression: `for var x in arr do` desugars via `IEnumerable<T>` (borrowing); `for var x in arr.Drain() do` desugars via `IOwningEnumerable<T>` (owning).


<a id="sec-owned-buffers-and-spans"></a>
<a id="sec-ownedbuffert-spant-readonlyspant"></a>
### 4.14 `OwningBuffer<T>`, `Span<T>`, `ReadOnlySpan<T>`

A runtime-sized storage primitive and two non-owning view types built over it. Together they are the foundation over which the resizable owning collections (`OwningArray<T>`, `OwningDictionary<K, V>`, etc.) compose, and they give user code a safe way to express "a fixed-but-runtime-sized buffer of slots" without any `unsafe` mechanism.

The slot semantics dispatch on T's category:

- **Class-typed T** (classes, interfaces): slots hold `T?` (nullable references) and the buffer owns each non-null slot's referent. Setter takes `owned T?` and disposes the previous value before storing the new one. `move buf[i]` is admitted; the field-restoration rule applies. The buffer's destructor cascades through slots, disposing every non-null one. This is the v0.99 design and is unchanged in v0.102.
- **Value-typed T** (primitives, records without owned fields, enums, non-move-only inline arrays): slots hold `T` directly (no nullability wrapper). Setter takes `T` and simply overwrites the slot. `move buf[i]` is *not* admitted (values have no ownership to transfer; reading is sufficient). The buffer's destructor releases its backing storage but performs no per-slot disposal.

The class-typed semantics were the v0.99–v0.101 design; v0.102 generalizes by adding the value-typed case. The two categories share the same `Length`, `Slice`, indexer-access, and other category-agnostic surface; they differ in the slot type, the setter modifier, and the admissibility of `move`.

**What is not admitted.** Runtime-managed reference types (`string`, function types) are not admitted as T — they have no disposal obligation, so the "owning" semantics add nothing, and they are stored more naturally in borrowing collections (`Array<string>`, `Array<(int) -> int>`). Move-only value types (currently only the owning inline arrays `[N owned]T` and `[N owned]T?`, [§15.3](Bezel_Language_Spec.md#sec-move-only-value-types) of the language spec) are also not admitted in v0.102 — the slot semantics for buffers of move-only types require additional design work and are deferred (see [§18](Bezel_Design_Rationale.md#sec-things-deferred) in the rationale).

<a id="sec-ownedbuffert"></a>
#### `OwningBuffer<T>`

The declaration's surface depends on T's category. For class-typed T:

```bezel
public class OwningBuffer<T>: IEnumerable<T?>            // class T only
  public constructor(length: int);                       // all slots null
  public constructor(length: int, init: (int) -> owned T);

  public readonly property Length: int;

  public property [i: int]: T?
    get;                                                  // borrow of slot
    set(owned);                                           // takes owned T?; disposes previous
  end;

  // move buf[i] admitted (returns owned T?, leaves slot moved-from;
  //   the field-restoration rule of language spec §28.8.1 applies)

  // Span views (v0.109)
  public method AsSpan(): Span<T>;                          // full buffer, writable
  public method AsSpan(start: int): Span<T>;                // from offset to end
  public method AsSpan(start: int, length: int): Span<T>;
  public method AsSpan(r: Range<int>): Span<T>;             // parallels Range slicing

  public method AsReadOnlySpan(): ReadOnlySpan<T>;
  public method AsReadOnlySpan(start: int): ReadOnlySpan<T>;
  public method AsReadOnlySpan(start: int, length: int): ReadOnlySpan<T>;
  public method AsReadOnlySpan(r: Range<int>): ReadOnlySpan<T>;
end OwningBuffer;
```

For value-typed T:

```bezel
public class OwningBuffer<T>                              // no IEnumerable conformance
                                                          //   (value-typed; see "Iteration" below)
  public constructor(length: int);                       // all slots default(T)
                                                          //   requires T to have a default value
  public constructor(length: int, init: (int) -> T);     // init returns T (no `owned`)

  public readonly property Length: int;

  public property [i: int]: T
    get;                                                  // borrow of slot (value copy
                                                          //   for primitives at the use site)
    set;                                                  // overwrites slot
  end;

  // move buf[i] NOT admitted (values have no ownership; use plain read)

  // Foreign-memory access (v0.186; language spec §32.12) — admitted only for
  //   FFI-pointee scalar and C-compatible-record T; see prose. The result type
  //   lives in System.Interop (import required at the use site).
  public method DangerousPointer(): Ptr<T>;

  // Span views (v0.109)
  public method AsSpan(): Span<T>;                          // full buffer, writable
  public method AsSpan(start: int): Span<T>;                // from offset to end
  public method AsSpan(start: int, length: int): Span<T>;
  public method AsSpan(r: Range<int>): Span<T>;             // parallels Range slicing

  public method AsReadOnlySpan(): ReadOnlySpan<T>;
  public method AsReadOnlySpan(start: int): ReadOnlySpan<T>;
  public method AsReadOnlySpan(start: int, length: int): ReadOnlySpan<T>;
  public method AsReadOnlySpan(r: Range<int>): ReadOnlySpan<T>;
end OwningBuffer;
```

The two presentations are different specializations of the same generic class. The compiler resolves T's category at instantiation and emits the appropriate slot semantics. Users see the form corresponding to the T they instantiate.

**Class-typed T (unchanged from v0.99).** `OwningBuffer<T>` is a class with fixed-at-construction storage of N slots, each holding `T?` (a nullable reference). Once allocated, `Length` is immutable. Non-null slots are owned by the buffer; the buffer's destructor disposes every non-null slot in implementation-defined order. The single-argument constructor produces a buffer with all slots initialized to `null`. The two-argument constructor calls `init(i)` for each `i` in `0..<length`, taking ownership of each returned value. The indexer's setter takes ownership of the assigned value and disposes whatever the slot previously held. `move buf[i]` is admitted and produces an owned `T?` value, leaving the slot in moved-from state; the field-restoration rule of [§28.8.1](Bezel_Language_Spec.md#sec-the-move-expression) applies. The natural restoration for a nullable slot is `buf[i] := null`.

**Value-typed T (new in v0.102).** `OwningBuffer<T>` is a class with fixed-at-construction storage of N slots, each holding `T` directly (no nullability wrapper). Once allocated, `Length` is immutable. The single-argument constructor requires T to have a default value (every primitive does; records with default-able fields do; enums with a zero-equivalent member do; user-defined types without defaults do not). It initializes every slot to `default(T)`. For types without a default, only the two-argument constructor with an explicit init function is admitted. The two-argument constructor calls `init(i)` for each `i` in `0..<length`, copying the returned value into the slot. The indexer's setter overwrites the slot's previous value with the new one — no disposal is performed (values are not owned). `move buf[i]` is not admitted; values are read with the plain indexer and copied at the use site.

The "Owned" in the name refers to the buffer-class's ownership of *its allocated storage*, not to slot-level ownership. For class-typed T, the buffer also owns its slot contents (the ownership semantics that motivated the name). For value-typed T, the buffer owns only its allocation; the slot contents are just data. In both cases the buffer is `IDisposable`: its destructor releases the allocation. The name is retained for continuity with the v0.99 design and for the class-typed case where slot-ownership remains the dominant concern.

**Span views (v0.109).** `AsSpan()` and its overloads return a `Span<T>` (writable view) over a contiguous range of the buffer; `AsReadOnlySpan()` returns a `ReadOnlySpan<T>`. These views are non-allocating record constructions and admit the standard span surface (`Slice`, indexer, search, equality, bulk mutation per T's category). The methods are pure ergonomics — `buf.AsSpan(0, 100)` reads more naturally than `new Span<T>(buf, 0, 100)` and matches the established pattern from v0.104's `string.AsSpan()` and v0.107's `Array<T>.AsSpan()`. `AsSpan` returns the writable form by default (matching the buffer's mutability); callers wanting read-only access either call `AsReadOnlySpan` directly or call `.AsReadOnly()` on the writable form. The four overload signatures of each form accept range parameters: no-arg (full buffer, `0..<Length`), `(start)` (from offset to end), `(start, length)`, or `(r: Range<int>)`. Bounds-check semantics follow the standard Bezel convention: `IndexOutOfRangeException` if `start` or `length` is out of range against the buffer's `Length`.

**`DangerousPointer()` (v0.186).** Returns the address of slot 0 as a `Ptr<T>` (language spec [§32.12](Bezel_Language_Spec.md#sec-foreign-memory)), valid until the buffer is disposed. Declared on the value-typed presentation only, and admitted only where `T` is an FFI-pointee scalar — integer types, `float`, `double`, `bool`, `IntPtr`, `UIntPtr` — or a C-compatible record (language spec [§32.13](Bezel_Language_Spec.md#sec-c-compatible-records)); every other `T` — classes (the class-typed presentation never satisfies the gate, so the member does not appear there), `string`, and `Ptr<U>` elements (whose slot address would reconstruct the declined `Ptr<Ptr<U>>`) — is rejected at the call's instantiation with a diagnostic naming the set. Pointer arrays are `OwningBuffer<IntPtr>` — exactly the argv shape. The `Dangerous` prefix marks the one accessor family whose result aliases mutable Bezel-owned slots; native retention past the synchronous call is the language spec [§32.10](Bezel_Language_Spec.md#sec-boundary-contract) obligation, and the recorded mitigation is the wrapper class that owns buffer and registration together.

**Iteration.** For class-typed T, `OwningBuffer<T>` implements `IEnumerable<T?>`; iteration yields borrows of `T?` (slot contents, possibly null). The `for var x in buf do ... end` form works both via the enumerator protocol and via the direct-indexed-loop optimization that [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) permits for concretely-typed sources. For value-typed T, `OwningBuffer<T>` does *not* implement `IEnumerable<T>` (the type-parameter-category dispatch would require conditional interface implementation, which Bezel does not currently provide as a general mechanism). `for var x in buf do ... end` over a value-typed OwningBuffer desugars to a direct indexed loop per the [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) extension; the iteration variable has type T and is a copy of the slot. Users who need IEnumerable<T> conformance on a value-typed buffer can wrap or convert.

**Why this is a compiler-known primitive.** The `OwningBuffer<T>` class needs intrinsic-level support primarily for one operation: **allocation of N slots where N is a runtime constructor parameter**. There is no user-level Bezel syntax for "a class field holding a runtime-determined-size array of slots"; inline arrays `[N owned]T?` and `[N]T` require compile-time N. The remaining surface (the indexer's interaction with the `move` expression form for class T, the destructor's per-slot disposal pass for class T, the iteration support) reuses mechanisms that `OwningArray<T>` and similar collections already exercise, just operating on the runtime-sized backing. The intrinsic boundary of the owning-collection family shrinks to this one class.

**Disposal.** For class-typed T, the buffer's destructor disposes every non-null slot. Slot-disposal order within a single buffer is implementation-defined. For value-typed T, the destructor releases the backing storage but performs no per-slot work. Both cases satisfy the `IDisposable` contract; both are non-trivial-dispose ([§28.9](Bezel_Language_Spec.md#sec-runtime-managed-types) of the language spec) in the sense that the user must discharge the buffer (typically via `using var` or by storing it as an `owned` field of a longer-lived class).

**Borrow-bearing slots.** When T is a class type whose instances themselves carry borrows from external sources (a class with a borrow-bearing record field, an interface backed by such a class, or any class whose fields include reachable borrows), the buffer participates in the same borrow-lifetime tracking as any other class-typed storage: the buffer may not outlive any source from which a slot borrows. The compiler tracks slot-borrow sources element-wise where possible and falls back to whole-buffer tracking when slot-borrow sources cannot be statically separated. For value-typed T whose values may themselves carry borrows (records with borrow-bearing fields), the same rule applies to the values stored in slots.

<a id="sec-spant"></a>
#### `Span<T>`

The declaration's surface, like `OwningBuffer<T>`'s, depends on T's category.

For class-typed T:

```bezel
public record Span<T>(Buffer: OwningBuffer<T>, Start: int, Length: int): IReadOnlySpan<T?>
  // class T

  // Indexer (bounds-checked against span Length as of v0.105).
  public property [i: int]: T?
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Buffer[Start + i];
    end;
    set(owned) => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      Buffer[Start + i] := value;
    end;
  end;

  // Range query (v0.101).
  public readonly property IsEmpty: bool => Length = 0;

  // Search (v0.101).
  public method IndexOf(value: T?): int;          // -1 if not found
  public method Contains(value: T?): bool;        // IndexOf(value) >= 0

  // Equality (v0.101).
  public method SequenceEqual(other: Span<T>): bool;
  public method SequenceEqual(other: IReadOnlySpan<T?>): bool; // v0.108 — interface form

  // Range construction (v0.99 / v0.100).
  public method Slice(start: int, length: int): Span<T> =>
    new Span<T>(Buffer, this.Start + start, length);

  public method AsReadOnly(): ReadOnlySpan<T> =>
    new ReadOnlySpan<T>(Buffer, Start, Length);

  // Bulk mutation (v0.101).
  public method Clear();                          // set every slot to null
                                                   //   (disposes previous contents)
  public method MoveFrom(source: Span<T>);        // move slot-by-slot from source;
                                                   //   source slots become null;
                                                   //   destination's previous
                                                   //   contents disposed;
                                                   //   lengths must match
  public method MoveTo(destination: Span<T>);     // v0.109 — symmetric with MoveFrom;
                                                   //   move slot-by-slot to destination;
                                                   //   source slots become null;
                                                   //   destination's previous
                                                   //   contents disposed;
                                                   //   lengths must match
  public method Reverse();                         // reverse slot ordering in place
end Span;
```

For value-typed T:

```bezel
public record Span<T>(Buffer: OwningBuffer<T>, Start: int, Length: int): IReadOnlySpan<T>
  // value T (primitive, record, enum, non-move-only inline array)

  // Indexer (bounds-checked against span Length as of v0.105).
  public property [i: int]: T
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Buffer[Start + i];
    end;
    set => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      Buffer[Start + i] := value;
    end;
  end;

  // Range query.
  public readonly property IsEmpty: bool => Length = 0;

  // Search.
  public method IndexOf(value: T): int;
  public method Contains(value: T): bool;

  // Equality.
  public method SequenceEqual(other: Span<T>): bool;
  public method SequenceEqual(other: IReadOnlySpan<T>): bool; // v0.108 — interface form

  // Range construction.
  public method Slice(start: int, length: int): Span<T> =>
    new Span<T>(Buffer, this.Start + start, length);

  public method AsReadOnly(): ReadOnlySpan<T> =>
    new ReadOnlySpan<T>(Buffer, Start, Length);

  // Bulk mutation (value-typed forms).
  public method Fill(value: T);                   // copy `value` into every slot
  public method CopyFrom(source: ReadOnlySpan<T>); // copy slot-by-slot from source
                                                    //   lengths must match
  public method CopyTo(destination: Span<T>);      // v0.109 — copy slot-by-slot to destination
                                                    //   lengths must match
  public method Reverse();
end Span;
```

A `Span<T>` is a borrow-bearing record ([§9.7](Bezel_Language_Spec.md#sec-records-and-disposal) of the language spec) viewing into a contiguous range of an `OwningBuffer<T>`. The `Buffer` field carries a borrow of an `OwningBuffer<T>`; per the borrow-bearing-record rule, the span's lifetime is bounded by the borrow's source, so a span cannot outlive the buffer it views.

`Span<T>` is purely user-level Bezel — no compiler intrinsic. The record machinery, borrow lifetime tracking, and indexer dispatch are all standard language features.

**Read access.** For class T: `span[i]` returns a borrow of the slot's value (which is `T?` because the underlying buffer's slots are nullable). For value T: `span[i]` returns a borrow of the slot (`T`, non-nullable). The borrow's source is the underlying `OwningBuffer<T>` (transitively reached through the span's `Buffer` field). For class T with potentially-null slots, programs that view into a buffer known to have non-null contents at every slot can use `span[i]!` for the null-forgiving access, raising `NullReferenceException` if the slot is null.

The indexer bounds-checks against the span's `Length` (not just the underlying buffer's length) as of v0.105. If `i < 0` or `i >= Length`, `IndexOutOfRangeException` is thrown. The check fires even when `Start + i` would be in the underlying buffer's range — the point of the span's range restriction is to enforce that callers cannot read outside the slice the span advertises. Earlier revisions (v0.99-v0.104) silently delegated to the buffer's indexer, which only checked against the buffer's length; this was a latent correctness bug, fixed in v0.105.

**Write access.** For class T: `span[i] := value` takes ownership of the assigned value (per the `set(owned)` setter) and stores it in the underlying buffer slot. The underlying buffer's owned-setter disposes whatever the slot previously held; the new value replaces it. `move span[i]` is also admitted: it takes ownership of the slot's current contents (an owned `T?`) and leaves the slot moved-from; the field-restoration rule of [§28.8.1](Bezel_Language_Spec.md#sec-the-move-expression) applies, and the natural restoration is `span[i] := null`. The setter and `move` form bounds-check against the span's `Length` (same rule as the getter, applied as of v0.105).

For value T: `span[i] := value` simply overwrites the slot with the new value. No ownership transfer happens; no previous value is disposed. `move span[i]` is *not* admitted — values are read with the plain indexer and copied at the use site. Writing through a value-typed span is mechanically the same as writing through a value-typed buffer directly.

A span carries the full read/write capability of the underlying buffer for its viewed range; what distinguishes it from holding the whole buffer is *range restriction*, not access mode. Programs that want a read-only view use `ReadOnlySpan<T>` instead.

`Slice(start, length)` produces a sub-span that views into the same buffer. The slice's lifetime is the same as the original span's — both are borrows of the same source. The slice has the same access mode as its parent (a `Span<T>` slices to a `Span<T>`; a `ReadOnlySpan<T>` slices to a `ReadOnlySpan<T>`).

`AsReadOnly()` returns a `ReadOnlySpan<T>` viewing the same buffer range. Bezel has no implicit record-to-record conversions, so passing a `Span<T>` where a `ReadOnlySpan<T>` is expected requires the explicit call. This is friction the C# precedent does not have (C# admits an implicit conversion from `Span<T>` to `ReadOnlySpan<T>`); Bezel's choice trades that convenience for the broader principle that record-type identity is nominal and conversions between records are always explicit. The `AsReadOnly()` call is cheap (record construction, no allocation) and makes the API-boundary intent visible at the call site.

**Iteration.** `for var x in span do ... end` is admitted for any T. The desugaring is a direct indexed loop over the span's range, parallel to the inline-array rule of [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) of the language spec: spans do not implement `IEnumerable<T?>` (the enumerator that `IEnumerable<T>.GetEnumerator()` returns is a heap object that would hold the span's borrow across its own, unbounded lifetime, which the borrow tracker cannot bound — a *different* situation from the **call-bounded** box of a span into `IReadOnlySpan<T?>` ([§4.16](#sec-ireadonlyspan)), which the language spec [§8.6](Bezel_Language_Spec.md#sec-boxing-of-borrow-bearing-records) permits because it cannot escape the borrow's source scope), so the standard enumerator path does not apply. The iteration variable's type follows the slot type: `T?` for class T, `T` for value T. For class T, the loop body sees a borrow of each slot's value; for value T, it sees a borrow of the slot (which, for primitive T, is effectively the value). To consume slot contents inside the loop body for class T, use `move span[i]` followed by an explicit assignment to restore the slot.

**Search and equality.** `IndexOf` walks the span's range and returns the first index whose slot's value equals the target. The comparison uses `Equals` (records, classes, primitives), null-safe for class T. `Contains` is `IndexOf(value) >= 0`. `SequenceEqual(other: Span<T>)` compares two spans element-by-element; lengths differ → false; otherwise pairwise `Equals`. There is no overload for `SequenceEqual(other: ReadOnlySpan<T>)` — callers convert one side via `.AsReadOnly()` if they need to compare across the two span types.

**Bulk mutation — class T.** `Clear()` assigns `null` to every slot in the span's range. Each assignment goes through the underlying buffer's owned-setter, which disposes the slot's previous contents. After `Clear()`, every slot in the range is null; slots outside the span's range are unaffected.

`MoveFrom(source: Span<T>)` requires `source.Length` to equal `this.Length` (else `ArgumentException` is raised). It transfers ownership slot-by-slot: for each `i`, `move source[i]` takes the source slot's contents, then `this[i] := value` stores those contents in the destination (disposing the destination's previous contents). After `MoveFrom`, every slot in the source span is null; every slot in the destination span holds what the source previously held.

`MoveTo(destination: Span<T>)` is the symmetric form, added in v0.109. It is equivalent to `destination.MoveFrom(this)` and provided for the ergonomic case where the caller holds the source and writes left-to-right: `source.MoveTo(dest)` reads naturally where `dest.MoveFrom(source)` would require introducing the destination first. Same length requirement, same `ArgumentException` on mismatch, same per-slot move semantics.

**Overlap behavior.** When the source and destination spans view overlapping ranges of the same underlying buffer (which is admitted by the borrow tracker — multiple spans into the same buffer are normal), the bulk-copy and bulk-move operations process slots in implementation-defined order. For non-overlapping ranges (the typical case) the order is invisible to programs. For overlapping ranges, programs that depend on a particular processing order are wrong; if the source and destination overlap, callers should either copy via an intermediate buffer or use slot-by-slot loops to control the order explicitly. This applies to `CopyFrom`, `CopyTo`, `MoveFrom`, and `MoveTo` on all span types.

`Reverse()` reverses the slot ordering within the span's range, in place. Implementation pattern: pairwise swap via `move` and re-assign, working from both ends toward the middle.

**Bulk mutation — value T.** `Fill(value: T)` copies `value` into every slot. Because values are not owned references, the operation is a plain copy — no contradictions with ownership semantics arise.

`CopyFrom(source: ReadOnlySpan<T>)` requires `source.Length` to equal `this.Length` (else `ArgumentException` is raised). It copies each value from the source span to the destination span, slot by slot. No move semantics involved (values are copied, not transferred).

`CopyTo(destination: Span<T>)` is the symmetric form, added in v0.109. Equivalent to `destination.CopyFrom(this)` and provided for the same ergonomic reason as `MoveTo`. Same length requirement and same per-slot copy semantics. The `ReadOnlySpan<T>` (value T) form also gets `CopyTo(destination: Span<T>)` — the source side is read-only but the destination side is writable; this is the common "I have a read-only view, copy it into a writable buffer" pattern.

`Reverse()` reverses the slot ordering within the span's range, in place. Implementation pattern: pairwise swap via plain reads and writes (no `move` for value-typed spans).

**Why the mutation methods differ by category.** For class T, the slots own their contents; bulk-assigning a single value would either share a class reference into N slots (forbidden by Bezel's ownership model — one owner per reference) or require synthesizing N independent class instances (Bezel does not provide implicit cloning). The only bulk-transfer that works is move-from-another-span (one ownership root per slot, transferring intact). For value T, none of these constraints apply — values are copies by default, so `Fill(value)` (broadcast) and `CopyFrom(source)` (per-slot copy) are both well-defined. The category-dependent mutation API is a direct consequence of the ownership-vs-value-semantics distinction.

<a id="sec-readonlyspant"></a>
#### `ReadOnlySpan<T>`

The declaration's surface mirrors `Span<T>`'s but exposes only the read-side operations.

For class-typed T:

```bezel
public record ReadOnlySpan<T>(Buffer: OwningBuffer<T>, Start: int, Length: int): IReadOnlySpan<T?>
  // class T

  public property [i: int]: T?
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Buffer[Start + i];
    end;
  end;

  public readonly property IsEmpty: bool => Length = 0;

  public method IndexOf(value: T?): int;
  public method Contains(value: T?): bool;

  public method SequenceEqual(other: ReadOnlySpan<T>): bool;
  public method SequenceEqual(other: IReadOnlySpan<T?>): bool; // v0.108 — interface form

  public method Slice(start: int, length: int): ReadOnlySpan<T> =>
    new ReadOnlySpan<T>(Buffer, this.Start + start, length);
end ReadOnlySpan;
```

For value-typed T:

```bezel
public record ReadOnlySpan<T>(Buffer: OwningBuffer<T>, Start: int, Length: int): IReadOnlySpan<T>
  // value T

  public property [i: int]: T
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Buffer[Start + i];
    end;
  end;

  public readonly property IsEmpty: bool => Length = 0;

  public method IndexOf(value: T): int;
  public method Contains(value: T): bool;

  public method SequenceEqual(other: ReadOnlySpan<T>): bool;
  public method SequenceEqual(other: IReadOnlySpan<T>): bool; // v0.108 — interface form

  public method Slice(start: int, length: int): ReadOnlySpan<T> =>
    new ReadOnlySpan<T>(Buffer, this.Start + start, length);

  public method CopyTo(destination: Span<T>);     // v0.109 — copy slot-by-slot to destination;
                                                   //   lengths must match
end ReadOnlySpan;
```

A read-only counterpart to `Span<T>`. Mechanically, `ReadOnlySpan<T>` is the same shape as `Span<T>` but exposes only the indexer getter — no setter, no `move`-from-slot (class T), no bulk mutation. The type distinction is real and present-day, not a name-squat: an API accepting `ReadOnlySpan<T>` cannot mutate the underlying buffer through the span, even if it happens to be backed by the same buffer that another span (`Span<T>`-typed) is writing to. Callers can construct a `ReadOnlySpan<T>` from any source that produces a `Span<T>` view — including from a span obtained via owner-internal access — and pass it to code that should not gain write capability. Code that needs read-only access takes `ReadOnlySpan<T>`; code that needs write access takes `Span<T>`. The C# precedent is the same split; the names line up for readers familiar with that ecosystem.

`Slice` returns `ReadOnlySpan<T>` (read-only slice of a read-only span). There is no widening operation from `ReadOnlySpan<T>` to `Span<T>` — once read-only, always read-only through that handle. To regain write access, the code path must be reached through a span constructed against the underlying buffer directly.

**Iteration.** `for var x in roSpan do ... end` is admitted, with the same direct-indexed-loop desugaring as `Span<T>`. See the iteration paragraph under `Span<T>` above.

**Search and equality.** `IndexOf`, `Contains`, and `SequenceEqual` mirror `Span<T>`'s read-only methods. `SequenceEqual(other: ReadOnlySpan<T>)` compares two read-only spans element-by-element. To compare against a `Span<T>`, the caller converts the writable span via `.AsReadOnly()`.

**Why not stack-allocated like C#'s `Span<T>`?** C#'s `Span<T>` is a `ref struct` — stack-only, cannot escape to the heap. This gives it the strongest possible lifetime guarantees but constrains where it can appear (no `Span<T>` fields in classes, no `Span<T>` in async state machines, no `Span<T>` returned across await points). Bezel's `Span<T>` is an ordinary record subject to borrow-bearing-record rules; it can be stored anywhere the borrow's lifetime allows, including as a class field when the field's enclosing class outlives the borrow source, or, for a span *local* whose source is co-resident in the same async frame (an owned local, `using var`, or `owned` parameter of that method), across `await` points in that method. A span may **not** be an async method's parameter or result — a caller-side source cannot be shown to outlive the frame (spec §30.1, v0.179, #1438). Programs whose buffer lifetimes cannot be statically proven sufficient are rejected by the borrow tracker. The Bezel choice trades the unconditional stack-only safety of C# for a much smaller language footprint — no new "ref struct" type kind is introduced. The lifetime tracker handles the common cases (parser scratch buffers, byte-buffer reads, slice-and-walk patterns, async pipelines whose buffer outlives the awaits) without a special type kind.

**Construction patterns.** A buffer's owner produces spans by handing out `new Span<T>(buf, start, length)` or `new ReadOnlySpan<T>(buf, start, length)` directly, or by exposing helper methods:

```bezel
public class FrameBuffer
  private owned _pixels: OwningBuffer<Pixel>;
  private readonly _width: int;

  // Width and Height as needed; Pixel is assumed to be a class type.

  public method ScanLineReadOnly(y: int): ReadOnlySpan<Pixel> =>
    new ReadOnlySpan<Pixel>(_pixels, y * _width, _width);

  public method ScanLine(y: int): Span<Pixel> =>
    new Span<Pixel>(_pixels, y * _width, _width);
end FrameBuffer;
```

`ScanLine` returns a writable span; callers can do `frame.ScanLine(10)[20] := newPixel`. `ScanLineReadOnly` returns a read-only span; the same call site would fail to compile because `ReadOnlySpan<Pixel>` has no indexer setter.

In either case, the returned span borrows the `FrameBuffer` instance (transitively, through the `_pixels` field). Callers cannot retain the span past the `FrameBuffer`'s lifetime — the borrow tracker rejects code that would.

<a id="sec-byte-buffer-accessors"></a>
#### Byte-buffer accessors

When T is `byte`, the spans are commonly used for binary I/O — file formats, network protocols, image headers, serialization. The endianness-aware accessors below read and write multi-byte numeric types from a `Span<byte>` or `ReadOnlySpan<byte>` without bit-shuffling at the call site.

```bezel
extension ReadOnlySpan<byte>
  // 16-bit integers
  public method ReadInt16LittleEndian(): short;
  public method ReadInt16BigEndian(): short;
  public method ReadUInt16LittleEndian(): ushort;
  public method ReadUInt16BigEndian(): ushort;

  // 32-bit integers
  public method ReadInt32LittleEndian(): int;
  public method ReadInt32BigEndian(): int;
  public method ReadUInt32LittleEndian(): uint;
  public method ReadUInt32BigEndian(): uint;

  // 64-bit integers
  public method ReadInt64LittleEndian(): long;
  public method ReadInt64BigEndian(): long;
  public method ReadUInt64LittleEndian(): ulong;
  public method ReadUInt64BigEndian(): ulong;

  // IEEE 754 floating point
  public method ReadFloatLittleEndian(): float;
  public method ReadFloatBigEndian(): float;
  public method ReadDoubleLittleEndian(): double;
  public method ReadDoubleBigEndian(): double;

  // Try variants — return false instead of throwing on short input
  public method TryReadInt16LittleEndian(out value: short): bool;
  public method TryReadInt16BigEndian(out value: short): bool;
  public method TryReadUInt16LittleEndian(out value: ushort): bool;
  public method TryReadUInt16BigEndian(out value: ushort): bool;
  public method TryReadInt32LittleEndian(out value: int): bool;
  public method TryReadInt32BigEndian(out value: int): bool;
  public method TryReadUInt32LittleEndian(out value: uint): bool;
  public method TryReadUInt32BigEndian(out value: uint): bool;
  public method TryReadInt64LittleEndian(out value: long): bool;
  public method TryReadInt64BigEndian(out value: long): bool;
  public method TryReadUInt64LittleEndian(out value: ulong): bool;
  public method TryReadUInt64BigEndian(out value: ulong): bool;
  public method TryReadFloatLittleEndian(out value: float): bool;
  public method TryReadFloatBigEndian(out value: float): bool;
  public method TryReadDoubleLittleEndian(out value: double): bool;
  public method TryReadDoubleBigEndian(out value: double): bool;
end;

extension Span<byte>
  // Reads and TryReads (delegate to AsReadOnly)
  public method ReadInt16LittleEndian(): short =>
    this.AsReadOnly().ReadInt16LittleEndian();
  public method ReadInt16BigEndian(): short =>
    this.AsReadOnly().ReadInt16BigEndian();
  // ... (full set of 16 Read methods and 16 TryRead methods, each
  //      delegating identically to the corresponding AsReadOnly method;
  //      omitted here for brevity)

  // Writes — Span<byte> only
  public method WriteInt16LittleEndian(value: short);
  public method WriteInt16BigEndian(value: short);
  public method WriteUInt16LittleEndian(value: ushort);
  public method WriteUInt16BigEndian(value: ushort);
  public method WriteInt32LittleEndian(value: int);
  public method WriteInt32BigEndian(value: int);
  public method WriteUInt32LittleEndian(value: uint);
  public method WriteUInt32BigEndian(value: uint);
  public method WriteInt64LittleEndian(value: long);
  public method WriteInt64BigEndian(value: long);
  public method WriteUInt64LittleEndian(value: ulong);
  public method WriteUInt64BigEndian(value: ulong);
  public method WriteFloatLittleEndian(value: float);
  public method WriteFloatBigEndian(value: float);
  public method WriteDoubleLittleEndian(value: double);
  public method WriteDoubleBigEndian(value: double);

  // Try variants — return false instead of throwing on short output buffer
  public method TryWriteInt16LittleEndian(value: short): bool;
  public method TryWriteInt16BigEndian(value: short): bool;
  // ... (full set of 16 TryWrite methods; omitted for brevity)
end;
```

**Endianness in the method name.** Every method names the endianness it interprets. There is no platform-default version and no enum parameter — both alternatives invite the class of bugs where someone forgets that "ints over the network are big-endian" or where the same source produces different values on different platforms. The C# `BinaryPrimitives` family settled on the same convention after a decade of practical use; the names line up for readers familiar with that ecosystem.

**No offset parameter.** Every method operates from the start of the span. To read at an offset, slice first: `header.Slice(8).ReadInt32LittleEndian()`. `Slice` is cheap (record construction, no allocation), and the composition is uniform — the same indexing model applies everywhere. The alternative (offset as a parameter) would split the API into two access patterns and add one parameter per method, both costs the slice-then-read form avoids.

**Bounds-check semantics.** Read methods throw `IndexOutOfRangeException` if the span is shorter than the type being read (e.g. `ReadInt32LittleEndian` on a 3-byte span). Write methods throw the same exception if the span is too short to hold the value being written.

`TryRead*` variants return `false` instead of throwing and assign the `out` parameter to `default(T)` on the false path (per the language spec [§22.3](Bezel_Language_Spec.md#sec-parameter-modifiers) requirement that every `out` parameter be assigned on every return path — v0.184 wording; throw paths carry no obligation). They suit incremental parsers that accept short inputs as end-of-stream rather than as errors.

`TryWrite*` variants return `false` instead of throwing on a too-short destination span. On the false path the span is **not modified** — the write is atomic (either every byte of the value is written, or none is). This matters for code that uses a single shared buffer for multiple writes and falls back to a different output path on overflow; the unmodified state is the invariant the fallback path can rely on.

**Float bit handling.** `ReadFloatLittleEndian` reads four bytes into an int value, then invokes `BitConverter.Int32BitsToFloat` ([§2.12](#sec-bitconverter)) to reinterpret the bits as a `float`. The double-precision form uses `BitConverter.Int64BitsToDouble`. Both preserve NaN bit patterns exactly. Users with explicit bit-pattern needs (custom encodings, byte-twiddling outside the standard formats) can call `BitConverter` directly.

**Example implementation (one Read, one Write).** The remaining methods follow the same pattern.

```bezel
extension ReadOnlySpan<byte>
  public method ReadInt32LittleEndian(): int
    if this.Length < 4 then
      throw new IndexOutOfRangeException();
    end;
    return int(this[0]) |
           (int(this[1]) << 8) |
           (int(this[2]) << 16) |
           (int(this[3]) << 24);
  end ReadInt32LittleEndian;
end;

extension Span<byte>
  public method WriteInt32BigEndian(value: int)
    if this.Length < 4 then
      throw new IndexOutOfRangeException();
    end;
    this[0] := byte((value >> 24) & 0xFF);
    this[1] := byte((value >> 16) & 0xFF);
    this[2] := byte((value >> 8) & 0xFF);
    this[3] := byte(value & 0xFF);
  end WriteInt32BigEndian;
end;
```

The unsigned variants follow by casting through the matching unsigned type at the boundary; the 16/64-bit variants follow by changing the byte count and shift widths; float variants compose Read/Write with the matching `BitConverter` call. The implementation pattern is mechanical and the standard library provides the methods so user code does not repeat them.

<a id="sec-arrayspant-readonlyarrayspant"></a>
### 4.15 `ArraySpan<T>`, `ReadOnlyArraySpan<T>`

Records carrying borrows of an `Array<T>` ([§4.4](#sec-arrayt)) plus a `(Start, Length)` range. The natural companion to `Array<T>.AsSpan()` and `Array<T>.AsReadOnlySpan()` for passing a sub-range of an array to a method without copying.

`ArraySpan<T>` is the writable form; `ReadOnlyArraySpan<T>` is the read-only form. The split parallels `Span<T>` / `ReadOnlySpan<T>` ([§4.14](#sec-owned-buffers-and-spans)) but with `Array<T>` as the source instead of `OwningBuffer<T>`. The two span families coexist; they are not interchangeable (a method that takes `Span<T>` does not accept `ArraySpan<T>` and vice versa) because the underlying storage models differ — `OwningBuffer<T>` owns its slots for class T, while `Array<T>` borrows them.

**Byte-buffer accessors (v0.165, #1358):** the §4.14 endianness-aware extension family is mirrored onto this family — all `Read*{Little,Big}Endian`/`TryRead*` forms on `ReadOnlyArraySpan<byte>` and `ArraySpan<byte>`, and the `Write*`/`TryWrite*` forms on `ArraySpan<byte>`, with identical semantics (no offset parameters; slice first). This closes the file→binary-parse path: `File.ReadAllBytes(p).AsReadOnlySpan().ReadUInt32LittleEndian()` is direct and zero-copy.

<a id="sec-arrayspant"></a>
#### `ArraySpan<T>`

```bezel
public record ArraySpan<T>(Source: Array<T>, Start: int, Length: int): IReadOnlySpan<T>

  public property [i: int]: T
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Source[Start + i];
    end;
    set => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      Source[Start + i] := value;
    end;
  end;

  public readonly property IsEmpty: bool => Length = 0;

  // Range construction
  public method Slice(start: int, length: int): ArraySpan<T> =>
    new ArraySpan<T>(Source, this.Start + start, length);

  public method AsReadOnly(): ReadOnlyArraySpan<T> =>
    new ReadOnlyArraySpan<T>(Source, Start, Length);

  // Search
  public method IndexOf(value: T): int;
  public method Contains(value: T): bool;

  // Equality
  public method SequenceEqual(other: ArraySpan<T>): bool;
  public method SequenceEqual(other: ReadOnlyArraySpan<T>): bool;
  public method SequenceEqual(other: IReadOnlySpan<T>): bool; // v0.108 — interface form

  // Bulk mutation
  public method Fill(value: T);                              // copy `value` into every slot
  public method CopyFrom(source: ReadOnlyArraySpan<T>);      // copy slot-by-slot; lengths must match
  public method CopyTo(destination: ArraySpan<T>);           // v0.109 — copy slot-by-slot to destination;
                                                              //   lengths must match
  public method Reverse();                                    // reverse slot ordering in place
end ArraySpan;
```

An `ArraySpan<T>` is a borrow-bearing record ([§9.7](Bezel_Language_Spec.md#sec-records-and-disposal) of the language spec) viewing into a contiguous range of an `Array<T>`. The `Source` field carries a borrow of the array; per the borrow-bearing-record rule, the span's lifetime is bounded by the source's lifetime.

`ArraySpan<T>` is purely user-level Bezel — no compiler intrinsic. The record machinery, borrow lifetime tracking, and indexer dispatch are all standard language features.

**Read access.** `span[i]` returns the value at position `Start + i` in the source array. The return type follows the array's element type — `T` is whatever the array was instantiated with (a class type for `Array<MyClass>`, a nullable class type for `Array<MyClass?>`, a value type for `Array<int>`). Unlike `Span<T>` over `OwningBuffer<T>`, there is no `T?` wrapping; the source array does not store nullable wrappers for class T (the array stores borrows directly, and a non-nullable `Array<MyClass>` has non-null borrow slots).

**Write access.** `span[i] := value` stores `value` into the source array's slot at `Start + i`. For class T (whether nullable or not), the assigned value is a borrow — no ownership transfer; the previous slot contents are not disposed (they were borrows, not owned values). For value T, the value is copied into the slot. The setter takes the value with the default parameter convention (no `owned` modifier); this differs from `Span<T>`'s class-T setter, which takes `owned T?` because the underlying `OwningBuffer<T>` owns its slots.

`move arrSpan[i]` is **not admitted** for any T. Borrowed slots have no ownership to transfer; the plain indexer-get is the only read form. This differs from class-typed `Span<T>` (which admits `move` because `OwningBuffer<T>` slots are owned).

**Bounds-check semantics.** The indexer bounds-checks against the span's `Length` (not just the underlying array's `Count`); `IndexOutOfRangeException` is thrown if `i < 0` or `i >= Length`. The same rule applies to the setter. This matches the v0.105 bounds-check rule for `Span<T>`/`ReadOnlySpan<T>` and the v0.104/v0.106 rules for the string-backed spans.

**Iteration.** `for var x in arrSpan do ... end` is admitted. The desugaring is a direct indexed loop over the span's range, parallel to the OwningBuffer-backed span rule of language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for). The iteration variable's type is T and the loop body sees a borrow of each slot (the borrow's source is the underlying array, reached through the span's `Source` field).

**Search and equality.** `IndexOf` walks the span's range, returning the first index whose slot equals `value`, or `-1` if none. `Contains` returns `IndexOf(value) >= 0`. The comparison uses `Equals` per the standard convention. `SequenceEqual(other: ArraySpan<T>)` compares two writable array spans pairwise; the `ReadOnlyArraySpan<T>` overload supports comparison across the access-mode boundary without requiring an `AsReadOnly()` call at the comparison site.

**Bulk mutation.** Unlike `Span<T>` (whose mutation API splits by T's category — `Clear`/`MoveFrom`/`MoveTo` for class T owning slots, `Fill`/`CopyFrom`/`CopyTo` for value T), `ArraySpan<T>` provides a single uniform mutation surface for all T: `Fill(value)` assigns `value` to every slot; `CopyFrom(source)` copies slot-by-slot from a source span (lengths must match, else `ArgumentException`); `CopyTo(destination)` copies slot-by-slot to a destination span (lengths must match); `Reverse()` reverses the slot ordering in place. The uniform API is admissible because `Array<T>` slots do not own their contents — there is no ownership transfer to worry about, no dispose-previous semantics, no need for the category-dependent split. For class T, the assigned-into slot stores a borrow (`Fill(myClass)` makes every slot borrow `myClass`); for value T, the slot stores a copy. Both fit naturally under `Fill`. `CopyTo` was added in v0.109 alongside the broader v0.109 ergonomics pass.

**No in-place modification of value slots.** Bezel has no ref returns ([§22.14](Bezel_Language_Spec.md#sec-no-params-no-ref-returns)), so `arrSpan[i].Field := x` (modifying a value-type slot in place through the span) is not available. The pattern is `var v := arrSpan[i]; v.Field := x; arrSpan[i] := v;` — read-modify-write through assignment. This limitation applies equally to `Array<T>` itself; the span inherits it.

**Construction.** Callers construct `ArraySpan<T>` via the `AsSpan()` family on `Array<T>` ([§4.4](#sec-arrayt)) rather than via the constructor directly. The direct constructor is admitted but is rarely needed.

**Caveat: array mutation during span lifetime.** Unlike `OwningBuffer<T>` (which has a fixed `Length`), `Array<T>`'s `Count` can change during the array's lifetime via `Add`, `Insert`, `Remove`, `RemoveAt`, and `Clear`. A span's `Length` is fixed at construction; subsequent mutation of the underlying array does not update the span. If the array shrinks below `Start + Length` after the span was constructed, accesses through the span (which pass the span's bounds check but then index into the array beyond its new `Count`) will throw `IndexOutOfRangeException` from the array's own bounds check. If the array grows or its contents shift via `Insert` or `RemoveAt`, the span's `(Start, Length)` continues to point at the same logical positions in the array — which may now hold different elements than the span originally viewed. This is a real footgun and a structural difference from the `OwningBuffer<T>`-backed spans (whose underlying buffer cannot resize). The intended usage pattern is short-lived spans: construct the span, pass it to a method that reads or writes through it, return — without resizing the source array in between. Programs that need a stable view across mutations should copy the relevant range into a fresh `OwningBuffer<T>` or `Array<T>` instead.

<a id="sec-readonlyarrayspant"></a>
#### `ReadOnlyArraySpan<T>`

```bezel
public record ReadOnlyArraySpan<T>(Source: Array<T>, Start: int, Length: int): IReadOnlySpan<T>

  public property [i: int]: T
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Source[Start + i];
    end;
  end;

  public readonly property IsEmpty: bool => Length = 0;

  public method Slice(start: int, length: int): ReadOnlyArraySpan<T> =>
    new ReadOnlyArraySpan<T>(Source, this.Start + start, length);

  public method IndexOf(value: T): int;
  public method Contains(value: T): bool;

  public method SequenceEqual(other: ReadOnlyArraySpan<T>): bool;
  public method SequenceEqual(other: ArraySpan<T>): bool;
  public method SequenceEqual(other: IReadOnlySpan<T>): bool; // v0.108 — interface form

  public method CopyTo(destination: ArraySpan<T>);            // v0.109 — copy slot-by-slot to destination;
                                                               //   lengths must match
end ReadOnlyArraySpan;
```

A read-only counterpart to `ArraySpan<T>`. Mechanically the same shape but exposes only the indexer getter — no setter, no bulk mutation. The type distinction is real and present-day, not a name-squat: an API accepting `ReadOnlyArraySpan<T>` cannot mutate the underlying array through the span, even if it happens to be backed by the same array that another span (`ArraySpan<T>`-typed) is writing to. Callers construct a `ReadOnlyArraySpan<T>` from the source array directly (via `Array<T>.AsReadOnlySpan()`) or by calling `.AsReadOnly()` on an `ArraySpan<T>`.

`Slice` returns `ReadOnlyArraySpan<T>` (read-only slice of a read-only span). There is no widening operation from `ReadOnlyArraySpan<T>` to `ArraySpan<T>` — once read-only through that handle, write access requires reaching the underlying array through a different path.

The borrow-of-`Array<T>` lifetime story is the same as `ArraySpan<T>`'s.

<a id="sec-ireadonlyspan"></a>
<a id="sec-ireadonlyspantelement"></a>
### 4.16 `IReadOnlySpan<TElement>`

An interface unifying the read-only access surface of every span type in the standard library. Implemented by all five concrete span types — `Span<T>`, `ReadOnlySpan<T>`, `ArraySpan<T>`, `ReadOnlyArraySpan<T>`, `ReadOnlyStringSpan` — with the element type parameterized to match each concrete type's natural indexer return type.

```bezel
public interface IReadOnlySpan<TElement>
  readonly property Length: int;
  readonly property IsEmpty: bool;
  property [i: int]: TElement
    get;
  end;

  method IndexOf(value: TElement): int;
  method Contains(value: TElement): bool;
  method SequenceEqual(other: IReadOnlySpan<TElement>): bool;
end IReadOnlySpan;
```

**Element-type parameterization.** Each concrete span type implements `IReadOnlySpan<TElement>` with `TElement` set to whatever its indexer actually returns:

| Concrete type | Implements |
|---|---|
| `Span<T>` for class T | `IReadOnlySpan<T?>` (OwningBuffer slots are nullable) |
| `Span<T>` for value T | `IReadOnlySpan<T>` |
| `ReadOnlySpan<T>` for class T | `IReadOnlySpan<T?>` |
| `ReadOnlySpan<T>` for value T | `IReadOnlySpan<T>` |
| `ArraySpan<T>` | `IReadOnlySpan<T>` (Array slots are non-nullable T or nullable T?, whichever T was instantiated as) |
| `ReadOnlyArraySpan<T>` | `IReadOnlySpan<T>` |
| `ReadOnlyStringSpan` | `IReadOnlySpan<char>` |

A method that wants to accept any read-only span of `MyClass` typically takes `IReadOnlySpan<MyClass?>` (which matches both OwningBuffer-backed nullable-slot spans and Array-backed `MyClass?` spans). Code holding a non-nullable Array span (`ArraySpan<MyClass>`) cannot pass it to that parameter directly — it implements `IReadOnlySpan<MyClass>`, not `IReadOnlySpan<MyClass?>` — because the interface's TElement parameter is invariant. Authors who want full breadth across nullability provide two overloads (one for `IReadOnlySpan<T>`, one for `IReadOnlySpan<T?>`); authors who want the nullable form should use it consistently and accept that callers may need an explicit conversion at the boundary.

**Boxing and lifetime.** Spans are borrow-bearing records ([§9.7](Bezel_Language_Spec.md#sec-records-and-disposal) of the language spec). A span passed as an `IReadOnlySpan<TElement>` parameter is boxed per the standard rule for borrow-bearing records (language spec [§8.6](Bezel_Language_Spec.md#sec-boxing-of-borrow-bearing-records)): the boxed copy holds the same borrows as the original, the boxed value's lifetime is bounded by the source of every borrow it carries, and the compiler enforces that the boxed value does not escape the source scope. The interface parameter therefore admits any span whose source outlives the call frame — which is the common case for synchronous calls — without further annotation.

A method declaring `IReadOnlySpan<TElement>` cannot store the parameter in a field that outlives the call, return it from a method whose return-lifetime rules do not establish source lifetime, or capture it in a closure that escapes. These are the standard borrow-bearing-record constraints; nothing new is added by the interface.

**Performance cost.** Indexer access through an `IReadOnlySpan<TElement>` parameter is a virtual dispatch — vtable lookup, indirect call, no inlining at the call site. For tight loops over many elements, this is meaningfully slower than the direct-dispatch path available when the parameter is typed concretely. For code that accesses spans in inner loops, the recommended pattern is to take the concrete span type (possibly via overloads) rather than the interface. The interface is the right choice when breadth across span sources matters more than peak per-element throughput.

```bezel
// Fast inner loop — direct dispatch
public method Sum(s: ReadOnlySpan<int>): int do ... end;

// Convenient and broad — virtual dispatch
public method Sum(s: IReadOnlySpan<int>): int do ... end;
```

Authors of performance-sensitive APIs typically provide the concrete-type signatures (one per common span family). Authors of utility code that runs outside hot paths typically use the interface for the smaller surface.

**Slice and the interface.** The interface deliberately omits `Slice`. Each concrete span type has its own `Slice` returning the same concrete type — `ReadOnlySpan<T>.Slice` returns `ReadOnlySpan<T>`, `ReadOnlyArraySpan<T>.Slice` returns `ReadOnlyArraySpan<T>`, and so on. Putting `Slice` on the interface would require the return type to be `IReadOnlySpan<TElement>`, which would propagate the interface-typed handle (and its virtual-dispatch cost) through the slicing chain. Callers who hold an `IReadOnlySpan<TElement>` and want to slice can do so by reading the relevant elements through the interface in a loop, or by recovering the concrete type via a pattern match (`if s is ReadOnlySpan<T> then ... else if s is ReadOnlyArraySpan<T> then ...`) and slicing on the concrete handle. In practice, slicing through the interface is rare enough that the explicit pattern is acceptable.

**SequenceEqual and the interface.** `SequenceEqual` *is* on the interface, with parameter type `IReadOnlySpan<TElement>`. This admits cross-source equality through the interface — `bufSpan.SequenceEqual(arrSpan)` where one is a `ReadOnlySpan<byte>` (buffer-backed) and the other is a `ReadOnlyArraySpan<byte>` (array-backed) compiles when both are passed via `IReadOnlySpan<byte>`. The cost is two virtual-dispatched indexer reads per element; the convenience is real for comparison code that doesn't care which source type the operands came from.

**What about writable spans.** v0.108 does not introduce an `IMutableSpan` (or similar) writable interface. The writable side has substantive semantic differences across span families — `Span<T>`'s class-T setter takes `owned T?` and disposes previous slot contents; `ArraySpan<T>`'s setter takes a borrow with no disposal; value-T setters are plain copies. Unifying these under a single writable interface would either pick one model and exclude the others, or fork the contract by source kind, which defeats the unification. The read-only side is genuinely uniform: every span supports indexer-get returning the slot's element, and that's enough for one interface. The writable side may be revisited if a partial unification (e.g. covering only the non-owned-slot writable spans) proves valuable.

<a id="sec-sorted-dictionary"></a>
### 4.17 `SortedDictionary<K, V>` (added in v0.136)

A comparison-ordered associative collection: iteration visits entries in ascending key order. The borrowing/non-owning counterpart of `Dictionary<K, V>` ([§4.8](#sec-dictionaryk-v)) with an ordering guarantee.

```bezel
public class SortedDictionary<K, V>: IReadOnlyCollection<DictionaryEntry<K, V>>, IDisposable where K: IComparable<K>   // conformance added v0.165 (#1354): makes for-in/LINQ legal per spec §29
  public constructor();

  public readonly property Count: int;

  public property [key: K]: V;                  // get: KeyNotFoundException if absent
                                                // set: insert or replace
  public method ContainsKey(key: K): bool;
  public method TryGetValue(key: K, out value: V?): bool;   // v0.171 (#1336): nullable out, matching §4.8's revised uniform Try shape
  public method Remove(key: K): bool;           // false if absent
  public method Clear();

  public readonly property FirstKey: K;         // least key; InvalidOperationException if empty
  public readonly property LastKey: K;          // greatest key; InvalidOperationException if empty
end SortedDictionary;
```

Ordering is defined by `K`'s `IComparable<K>.CompareTo` — there is no separate comparer parameter; a type that needs a different order wraps its key in a record with the desired `CompareTo` (the single-order rule keeps equality, ordering, and hashing in one place, on the type). Two keys are the same key exactly when `CompareTo` returns `0`. Point operations (`[k]`, `ContainsKey`, `TryGetValue`, `Remove`) are **O(log n)**; iteration is **O(n)** in ascending key order and is the single-pass borrowing form of [§4.8](#sec-dictionaryk-v) (`for var e in sd` yields entries with `e.Key`/`e.Value` borrows, one live pass at a time, structural modification during iteration raises `InvalidOperationException`).

Value semantics match `Dictionary<K, V>`: values are copied in and out for value types and `string`, borrowed for class types. There is no owning `OwningSortedDictionary` in this revision — owning sorted collections ride the self-hosted-collections work (changelog, v0.136).

<a id="sec-sorted-set"></a>
### 4.18 `SortedSet<T>` (added in v0.136)

A comparison-ordered set: membership by `CompareTo`-equality, iteration in ascending order.

```bezel
public class SortedSet<T>: IReadOnlyCollection<T>, IDisposable where T: IComparable<T>   // conformance added v0.165 (#1354)
  public constructor();

  public readonly property Count: int;

  public method Add(item: T);                   // v0.165 (#1354): unit-returning, matching §4.10's convention
  public method TryAdd(item: T): bool;          // false if an equal item is present
  public method Contains(item: T): bool;
  public method Remove(item: T): bool;          // false if absent
  public method Clear();

  public readonly property First: T;            // least; InvalidOperationException if empty
  public readonly property Last: T;             // greatest; InvalidOperationException if empty
end SortedSet;
```

The same single-order rule, complexity guarantees (point ops O(log n), in-order iteration O(n)), iteration form, and value semantics as [§4.17](#sec-sorted-dictionary) apply. As with `Set<T>` ([§4.10](#sec-sett-and-ownedsett)), the element type supplies the identity; for `SortedSet<T>` that identity is `CompareTo`-equality, and an `Add` of an equal element returns `false` without modifying the set.

<a id="sec-frozen-array"></a>
### 4.19 `FrozenArray<T>` (v0.186)

A **deeply immutable, fixed-size array** whose contents can never change after construction — the standard library's
zero-copy shared-read primitive (language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types),
[§28.13](Bezel_Language_Spec.md#sec-shared-ownership)). `FrozenArray<T>` is sendable, and it is a member of the
[§28.13](Bezel_Language_Spec.md#sec-shared-ownership) `Shared`-payload synchronized set — the set's first member
admitted by **deep immutability** rather than synchronization machinery (v0.186 ruling) — so
`Shared<FrozenArray<T>>` is sendable: clone one handle per worker domain, move the clones, and N domains read one
buffer with zero bytes copied. A `static owned readonly` `FrozenArray<T>` is a sound all-domains singleton and draws
no [§22.12](Bezel_Language_Spec.md#sec-static-members) rule-4 warning (the v0.186 suppression list); mutable statics
of it remain rejected (rule 2 — it is deliberately not in the form-(i) set).

```bezel
/// Compiler-intrinsic; T restricted to sendable value types and `string`.
public class FrozenArray<T>: IEnumerable<T>
  // Construction consumes or copies its source — after either, no writer can exist.
  public static method From(owned buffer: OwningBuffer<T>): owned FrozenArray<T>;
      // VALUE-TYPED T ONLY: O(1) — steals the storage; the buffer is consumed
      //   (owned call-site marker; the proposal-0006 owned-vs-copy construction shape).
      //   No string-element form can exist: OwningBuffer<T> does not admit string (§4.14).
  public static method From(items: Array<T>): owned FrozenArray<T>;
      // Copies; the only construction form for T = string.

  public readonly property Length: int;

  public property [i: int]: T
    get;                       // bounds-checked; IndexOutOfRangeException outside 0..<Length
  end;

  public method ToArray(): owned Array<T>;   // fresh mutable copy back out

  destructor;                  // releases the backing storage
end FrozenArray;
```

`T` is restricted by the compiler to **sendable value types and `string`** (a compiler-intrinsic restriction in the
`Atomic<T>` style, checked at instantiation per language spec [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)):
*"FrozenArray<T> requires a sendable value type or string; T is not eligible. For class elements, frozen ownership
trees are a designed future extension."*

**Why no writer can exist — checked rules plus one named obligation (v0.186).** `FrozenArray<T>` has no mutating
member, and both `From` forms sever their source: the owned overload consumes the buffer, the borrowing overload
copies. The severance is delivered by named rules, not by assumption: the consume-site freeze (language spec
[§28.3](Bezel_Language_Spec.md#sec-consume-site-freeze)) rejects `From(owned buf)` while any local borrow of `buf`
survives — **including a live span local over it**, which §28.3 states counts as such a borrow — and
[§9.7](Bezel_Language_Spec.md#sec-records-and-disposal)'s escape bullets keep spans out of fields, statics, escaping
closures, and async frames, so no span over the buffer outlives the freezing body invisibly. **The residual is the
domain clause** (language spec [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows), extended in
v0.186 to exactly this construction): a borrow of the source held in a *field, a collection slot, or a pending async
frame* is invisible to the freeze, and not consuming a so-borrowed source into `From` is the same recorded caller
obligation as not transferring it to another domain — a violation is a cross-domain data race no conformance mode
detects. Programs that honor that recorded obligation have, after construction, no writer: there is nothing to freeze
*later* and no window in which a mutable alias soundly survives — the `string` precedent (language spec
[§30.14](Bezel_Language_Spec.md#sec-sendable-types)), generalized to bulk data under the same conditional form §30.14
itself uses for its transfer guarantee.

**Cost accounting, stated plainly.** The O(1) freeze is **value-typed-only**; string elements always pay one copy at
construction (the sharing after construction is zero-copy for both). `From(items: Array<T>)` is O(n) for both
categories.

**Sharing idiom** (requires `Shared<T>`, [§2.16](#sec-shared-and-weak); the handle count is maintained with
synchronized operations, and the last handle — from whichever domain — disposes the frozen storage):

```bezel
var raw := LoadSamples();                                        // owned OwningBuffer<double>
using var input := new Shared<FrozenArray<double>>(
    FrozenArray<double>.From(owned raw));                        // O(1) freeze
using var t1 := Domain.Spawn(Program.ScanLow,  input.Clone());   // Shared<FrozenArray<double>>: sendable
using var t2 := Domain.Spawn(Program.ScanHigh, input.Clone());
var answer := await t1 + await t2;                               // zero bytes copied
```

**Iteration** yields element copies (value semantics; `string` elements as shared references). The
[§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) direct-indexed-loop optimization applies (concretely-typed
source). Because class-typed `T` is not admitted, the `IEnumerable<T>` conformance is unconditional — the
category-conditional-conformance problem that keeps value-typed `OwningBuffer<T>` out of `IEnumerable` ([§4.14](#sec-owned-buffers-and-spans))
does not arise.

**Disposal.** Non-trivial dispose: the destructor releases the backing storage (the [§4.14](#sec-owned-buffers-and-spans)
`OwningBuffer` reasoning). A standalone owned `FrozenArray<T>` is discharged like any owned class instance; under
`Shared<FrozenArray<T>>`, the last handle disposes it. *(Implementation note: like `string`, the frozen storage's
reclamation bookkeeping must tolerate cross-domain sharing — language spec §30.14's string clause, applied here.)*

**Deferred, each recorded:** frozen **class trees** (freeze = seal a transfer-closed ownership tree; real work in the
disposal story); **frozen span/view forms** (today's `ReadOnlySpan<T>` is a record over `OwningBuffer<T>`
specifically; a frozen view family should be designed once, together with the fork-join extension's needs — rationale
[§12.9](Bezel_Design_Rationale.md#sec-data-parallelism)); `FrozenDictionary`/`FrozenSet` (same pattern, evidence
first).

---

<a id="sec-system-collections-linq"></a>
## 5. `System.Collections.Linq`

LINQ-style extension methods on `IEnumerable<T>`. All are lazy unless documented otherwise. A lazy query produced by these operators is **single-consumption** — it may be enumerated at most once (a re-enumeration is a compile error, or `InvalidOperationException` where only dynamically reachable); call `ToArray()` to materialize an owned, re-enumerable `Array<T>`. See language spec [§29.11](Bezel_Language_Spec.md#sec-multiple-enumeration).

A bound lazy query is single-consumption with the normative consumption-point list of spec §29.11 (enumeration/terminal, composing a further operator, passing as `IEnumerable<T>`); inside a callee a plain `IEnumerable<T>` parameter is statically untracked and guarded at runtime.

**Callback retention (the callback-taking lazy operators store their callbacks).** A predicate/selector/key/combiner passed to a **lazy** operator that takes one — `Where`, `Select`, `SelectMany`, `TakeWhile`, `SkipWhile`, `OrderBy`, `OrderByDescending`, `ThenBy`, `ThenByDescending`, `GroupBy`, `Zip` — is **retained by the returned query value until enumeration**, not just for the duration of the call. These are **stdlib-certified storing signatures** (language spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters), [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)): capture admission for a capturing lambda is checked against the **query value** as the deferred-activation frame, and the retention ends at the query's [§29.11](Bezel_Language_Spec.md#sec-multiple-enumeration) single-consumption point. The callback-free lazy operators (`Take`, `Skip`, `Reverse`, `Distinct`, `OfType`, `Concat`, `Union`, `Intersect`, `Except`) hold no callback and carry no such obligation. The **eager** operators (`First(pred)`, `Last(pred)`, `Single(pred)`, `Count(pred)`, `Any(pred)`, `All(pred)`, `Sum`, `Min`, `Max`, `Average`, `Aggregate`, `ToArray`, `ToDictionary`, `ToSet`) are **call-bounded** as their unmarked signatures state — they invoke the callback during the call and retain nothing.

```bezel
public extension IEnumerable<T>
  // Filtering
  public method Where(predicate: (T) -> bool): IEnumerable<T>;
  public method OfType<U>(): IEnumerable<U>;

  // Projection
  public method Select<U>(selector: (T) -> U): IEnumerable<U>;
  public method SelectMany<U>(selector: (T) -> IEnumerable<U>): IEnumerable<U>;

  // Quantifiers
  public method Any(): bool;
  public method Any(predicate: (T) -> bool): bool;
  public method All(predicate: (T) -> bool): bool;
  public method Count(): int;
  public method Count(predicate: (T) -> bool): int;
  public method Contains(value: T): bool;

  // Element access
  public method First(): T;                                          // throws if empty
  public method First(predicate: (T) -> bool): T;
  public method FirstOrNull(): T?;
  public method FirstOrNull(predicate: (T) -> bool): T?;
  public method Last(): T;
  public method Last(predicate: (T) -> bool): T;
  public method LastOrNull(): T?;
  public method LastOrNull(predicate: (T) -> bool): T?;
  public method Single(): T;                                         // throws if not exactly one
  public method Single(predicate: (T) -> bool): T;                   // throws unless exactly one matches
  public method SingleOrNull(): T?;
  public method SingleOrNull(predicate: (T) -> bool): T?;            // null if none; throws if more than one
  public method ElementAt(index: int): T;

  // Aggregation
  public method Sum(selector: (T) -> int): int;
  public method Sum(selector: (T) -> long): long;
  public method Sum(selector: (T) -> uint): uint;
  public method Sum(selector: (T) -> ulong): ulong;
  public method Sum(selector: (T) -> float): float;
  public method Sum(selector: (T) -> double): double;
  public method Sum(selector: (T) -> decimal): decimal;
  public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
  public method Max<U>(selector: (T) -> U): U where U: IComparable<U>;
  public method Average(selector: (T) -> int): double;
  public method Average(selector: (T) -> long): double;
  public method Average(selector: (T) -> double): double;
  public method Average(selector: (T) -> decimal): decimal;
  public method Aggregate<TAccum>(seed: TAccum, fold: (TAccum, T) -> TAccum): TAccum;

  // Ordering
  public method OrderBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T> where TKey: IComparable<TKey>;
  public method OrderByDescending<TKey>(key: (T) -> TKey): IOrderedEnumerable<T> where TKey: IComparable<TKey>;
  public method Reverse(): IEnumerable<T>;

  // Set-like
  public method Distinct(): IEnumerable<T>;
  public method Union(other: IEnumerable<T>): IEnumerable<T>;
  public method Intersect(other: IEnumerable<T>): IEnumerable<T>;
  public method Except(other: IEnumerable<T>): IEnumerable<T>;

  // Slicing
  public method Take(count: int): IEnumerable<T>;
  public method TakeWhile(predicate: (T) -> bool): IEnumerable<T>;
  public method Skip(count: int): IEnumerable<T>;
  public method SkipWhile(predicate: (T) -> bool): IEnumerable<T>;

  // Combination
  public method Concat(other: IEnumerable<T>): IEnumerable<T>;
  public method Zip<U, R>(other: IEnumerable<U>, combine: (T, U) -> R): IEnumerable<R>;

  // Grouping
  public method GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>>;

  // Materialization (eager)
  public method ToArray(): owned Array<T>;
  public method ToDictionary<TKey>(key: (T) -> TKey): owned Dictionary<TKey, T>;
  public method ToDictionary<TKey, TValue>(key: (T) -> TKey, value: (T) -> TValue): owned Dictionary<TKey, TValue>;
  public method ToSet(): owned Set<T>;
end;

public interface IGrouping<TKey, T>: IEnumerable<T>
  readonly property Key: TKey;
end IGrouping;

public interface IOrderedEnumerable<T>: IEnumerable<T>
end IOrderedEnumerable;

public extension IOrderedEnumerable<T>
  public method ThenBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T> where TKey: IComparable<TKey>;
  public method ThenByDescending<TKey>(key: (T) -> TKey): IOrderedEnumerable<T> where TKey: IComparable<TKey>;
end;
```

`First`/`Last`/`Single`/`ElementAt` throw `InvalidOperationException` (or `IndexOutOfRangeException` for `ElementAt`) on missing-element; the `*OrNull` variants return `null` instead. (When the element type is itself nullable, `?` is idempotent under substitution — spec §7.10, v0.159 — so an `*OrNull` result cannot distinguish "empty" from "a present `null`"; use `Count` or the `Try` shape when the distinction matters.)

The predicate overloads of `First`, `Last`, and `Single` (and their `*OrNull` forms) restrict to the elements satisfying the predicate: `First(pred)` and `Last(pred)` yield the first and last matching element, and `Single(pred)` the sole matching element. Each is equivalent to the corresponding no-argument method applied after `Where(pred)` — `xs.First(pred)` is `xs.Where(pred).First()` — and inherits its missing-element behaviour: `First(pred)`/`Last(pred)`/`Single(pred)` throw `InvalidOperationException` when no element matches (and `Single(pred)` also when more than one matches), while `FirstOrNull(pred)`/`LastOrNull(pred)`/`SingleOrNull(pred)` return `null` when none matches (`SingleOrNull(pred)` still throws when more than one matches).

`ToArray()` returns a fresh `Array<T>` containing the materialized sequence. The return is `owned` — the caller is responsible for disposing it. There is no `ToList()` because Bezel does not have a dynamic-array type named `List<T>` (`Array<T>` is the resizable form; `LinkedList<T>` is the linked form; use `new LinkedList<T>(source)` if a linked list is wanted).

`Sum` and `Average` are provided as per-type overloads rather than as a single generic constrained on a numeric interface. The reason is historical: a generic numeric constraint requires static interface members (so `Sum`'s body could reference `T.Zero` and `T.operator +`), and these overloads predate the `INumber<T>` abstraction ([§2.19](#sec-static-abstractions)) that now provides them. The overload set covers the standard numeric primitives. Aggregating user-defined numeric types is done via `Aggregate(seed, fold)`.

<a id="sec-no-selector-aggregation-overloads"></a>
### 5.1 No-selector aggregation overloads

`Min` and `Max` without a selector are generic over comparable types:

```bezel
public extension IEnumerable<T> where T: IComparable<T>
  public method Min(): T;              // throws if empty
  public method Max(): T;              // throws if empty
  public method MinOrNull(): T?;       // null if empty
  public method MaxOrNull(): T?;       // null if empty
end;
```

`Sum` and `Average` without a selector are provided as per-type extensions on the standard numeric types:

```bezel
public extension IEnumerable<int>
  public method Sum(): int;
  public method Average(): double;
end;

public extension IEnumerable<long>
  public method Sum(): long;
  public method Average(): double;
end;

public extension IEnumerable<double>
  public method Sum(): double;
  public method Average(): double;
end;

public extension IEnumerable<decimal>
  public method Sum(): decimal;
  public method Average(): decimal;
end;
```

Analogous extensions exist for `uint`, `ulong`, and `float`. The per-type pattern follows the selector-based overloads and exists for the same reason: no generic numeric constraint in this version.

---

<a id="sec-system-threading"></a>
## 6. `System.Threading`

<a id="sec-cancellationtoken-and-cancellationtokensource"></a>
### 6.1 `CancellationToken` and `CancellationTokenSource`

See [§30.8](Bezel_Language_Spec.md#sec-cancellation) of the language spec.

```bezel
public record CancellationToken
  public static readonly property None: CancellationToken;
  public readonly property IsCancellationRequested: bool;
  public method ThrowIfCancellationRequested();
  public method Register(scoped callback: () -> ()): owned Subscription;
end CancellationToken;

public class CancellationTokenSource
  public constructor();

  public static method WithCancelAfter(delay: TimeSpan): owned CancellationTokenSource;

  // v0.173 (#810): a source cancelled when ANY parent token cancels (or by its
  // own Cancel/CancelAfter). Two-token form for the everyday case (caller +
  // deadline); array form for N parents.
  public static method CreateLinked(a: CancellationToken, b: CancellationToken): owned CancellationTokenSource;
  public static method CreateLinkedAll(parents: Array<CancellationToken>): owned CancellationTokenSource;

  public readonly property Token: CancellationToken;
  public readonly property IsCancellationRequested: bool;

  public method Cancel();
  public method CancelAfter(delay: TimeSpan);

  destructor;       // signals cancellation to any tokens still observing,
                           // unregisters from linked parents (v0.173), then
                           // releases the registered-callback list
end CancellationTokenSource;
```

**Linked sources (v0.173, #810).** `CreateLinked`/`CreateLinkedAll` complete the composition story: *"cancel if the caller cancels OR my local timeout fires"* binds **both** sources so the timeout source outlives the linked source:

```bezel
using var timeoutSrc := CancellationTokenSource.WithCancelAfter(timeout);
using var cts := CancellationTokenSource.CreateLinked(callerToken, timeoutSrc.Token);
```

— no hand-wired `Register` plumbing. A `WithCancelAfter` result **must be bound**: left as an unbound temporary, its disposal at the end of the statement cancels the source (cancel-on-dispose, below) and severs the just-created link, so `cts` would be born cancelled. Reverse-order disposal (`cts` before `timeoutSrc`) satisfies the source-outlives-token rule. Semantics: the linked source enters the cancelled state when **any** parent token cancels or its own `Cancel()`/`CancelAfter()` fires, whichever happens first; a parent already cancelled at creation cancels the linked source before `CreateLinked` returns (the same race-free synchronous policy as `Register` on a cancelled token, below). Disposing the linked source **unregisters from its parents first** — no callback can fire into a disposed source. Lifetime follows the existing source-outlives-token rule ([§28.13](Bezel_Language_Spec.md#sec-shared-ownership) posture, spec §30.8): the parent *sources* must outlive the linked source, exactly as they must outlive any other holder of their tokens. Linking is **intra-domain** — cross-domain cancellation remains a message on a channel (rationale §12.6).

**Timeout-as-a-scope needs no new type.** The RFC's `CancellationScope` sketch is already expressible: the source's destructor *cancels on dispose*, so `using var cts := CancellationTokenSource.WithCancelAfter(delay);` **is** a cancellation scope — auto-cancelled at scope exit or on timeout, whichever first. Recorded as the idiom rather than a wrapper type (see the v0.173 rationale entry).

`Register` invokes the callback when cancellation is requested; the returned `Subscription` removes the registration when disposed. The `callback` parameter is `scoped` (spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters)): the registration stores the callback bounded by the returned `Subscription` (the owned-return bounding form — `using var reg := token.Register(...)`), and disposal severs the registration, which is the quiescence guarantee. (Erratum within v0.177, #1431: the marker was implemented with the #1420 enforcement but this signature had been left unmarked; the event `Subscribe` signatures in [§6.5](#sec-events-ievent-eventsource) carried it since v0.175.)

If the token is **already in the cancelled state** at the moment `Register` is called, the callback is invoked synchronously on the calling thread before `Register` returns, and the returned `Subscription` is one whose `Dispose()` is a no-op (the registration was never installed because there was nothing to install — cancellation has already happened). Code receiving the `Subscription` should not assume the callback has not yet run.

This matches C#'s `CancellationToken.Register` behavior. The synchronous-fire policy is the only race-free choice: any policy that schedules the callback for later, or that ignores the registration on an already-cancelled token, would create a window in which the caller could observe an inconsistent state ("I registered a callback but it never fired, even though the token is cancelled").

`WithCancelAfter(delay)` is a static factory that constructs a `CancellationTokenSource` and immediately schedules it to cancel after `delay`. It is the recommended form for "create a source that auto-cancels after a fixed time," replacing the side-effect-bearing constructor that earlier versions exposed: constructors that schedule callbacks were inconsistent with the rest of the language's preference for constructors whose only job is initialization. To set or extend the delay on an existing source, use `CancelAfter(delay)`.

`CancellationTokenSource.IsCancellationRequested` and `source.Token.IsCancellationRequested` always return the same value: the source's property is a convenience for callers that hold the source directly, equivalent to `source.Token.IsCancellationRequested`. The duplication on the API surface matches C#'s analogous shape and exists for ergonomics — code that already has the source on hand shouldn't have to traverse to the token to ask the question.

<a id="sec-timeoutexception"></a>
#### `TimeoutException`

```bezel
public class TimeoutException: Exception
  public constructor();
  public constructor(message: string);
  public constructor(message: string, inner: Exception);   // borrows inner; snapshot copied into CauseChain (spec §31.4, v0.162) — no reference retained
end TimeoutException;
```

Raised by operations that exceed a deadline. Distinct from `OperationCanceledException`, which signals cancellation by an explicit `CancellationToken`; `TimeoutException` is for time-budget exhaustion where no token is involved. Library methods accepting a `TimeSpan` timeout typically raise this when the deadline elapses without the operation completing. Declared in `System.Threading`; available once that module is imported (it is not in the auto-imported set).

<a id="sec-mutex-and-lock"></a>
### 6.2 `Mutex` and `Lock`

```bezel
public class Mutex
  public constructor();
  public method Acquire(): owned Lock;
  public method TryAcquire(timeout: TimeSpan): owned Lock?;
  public async method AcquireAsync(token: CancellationToken := CancellationToken.None): OwningTask<Lock>;
  public async method TryAcquireAsync(timeout: TimeSpan, token: CancellationToken := CancellationToken.None): OwningTask<Lock?>;
end Mutex;

public class Lock
  // No public constructor; obtained from Mutex.Acquire / TryAcquire.
  destructor;       // releases the held lock
end Lock;
```

`Acquire` blocks until the mutex is available, then returns an `owned Lock` representing the held lock. The non-null return makes the result idiomatic with `using var`:

```bezel
using var guard := mutex.Acquire();
// critical section; guard disposed at scope end -> mutex released.
```

`TryAcquire(timeout)` returns `owned Lock?` — an `owned Lock` on success or `null` on timeout. The caller binds the result with `using var` and narrows by null-test:

```bezel
using var guard := mutex.TryAcquire(TimeSpan.FromSeconds(5));
if guard <> null then
  // critical section; guard non-null in this branch (narrowing)
end;
// At scope end: guard disposed if non-null, releasing the lock; null is a no-op.
```

**Sendability (specified in v0.138).** `Mutex` is *sendable* (language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)) and internally synchronized — like `Channel<T>` ([§6.9](#sec-channel)), it may be reached by multiple domains simultaneously; that is its purpose. `Lock` is **not** sendable: a held guard is bound to the acquiring domain and is released there (moving a held lock between domains would decouple acquisition from release and break the `using var` discipline that makes the guard idiom sound).

**Visibility (v0.184; language spec [§30.13](Bezel_Language_Spec.md#sec-cross-thread-visibility)).** Releasing a `Lock` — disposing it — *publishes* the holder's writes: the release happens-before the next acquisition of the same `Mutex`, whichever domain or thread (including an FFI foreign thread) acquires next. `Semaphore`/`SemaphoreToken` ([§6.3](#sec-semaphore)) carry the same edge.

`AcquireAsync` and `TryAcquireAsync` are the async counterparts. `AcquireAsync` returns a `OwningTask<Lock>` (Pattern B, single-consumer); `TryAcquireAsync` returns `OwningTask<Lock?>` — the same nullable shape inside a Pattern B task. Consumers write:

```bezel
using var guard := await mutex.AcquireAsync();
// critical section; guard disposed at scope end.

using var maybe := await mutex.TryAcquireAsync(TimeSpan.FromSeconds(5));
if maybe <> null then
  // critical section
end;
```

The nullable-result shape replaces the `out owned guard` form used in earlier versions: the `out` parameter contract ([§22.3](Bezel_Language_Spec.md#sec-parameter-modifiers) of the language spec) requires the parameter to be assigned on every return path (v0.184 wording), but the failure path of `TryAcquire` has no meaningful value to assign. Returning `owned Lock?` resolves the contradiction — the `null` value is itself the failure signal — and produces simpler call sites.

**Blocking discipline (v0.179; spec §30.12).** `Acquire()` and `TryAcquire(timeout)` block the calling thread. They are rejected at compile time inside `async` methods and `async` lambdas (use `AcquireAsync`/`TryAcquireAsync`); where a non-`async` helper reached from a domain continuation would actually park in them, the runtime raises `InvalidOperationException` instead. They remain fully legal on the startup thread and on FFI foreign threads (outside any domain), and when the mutex is immediately available.

<a id="sec-semaphore"></a>
### 6.3 `Semaphore`

```bezel
public class Semaphore
  public constructor(initialCount: int);
  public constructor(initialCount: int, maxCount: int);

  public method Acquire(): owned SemaphoreToken;
  public method TryAcquire(timeout: TimeSpan): owned SemaphoreToken?;
  public async method AcquireAsync(token: CancellationToken := CancellationToken.None): OwningTask<SemaphoreToken>;
  public async method TryAcquireAsync(timeout: TimeSpan, token: CancellationToken := CancellationToken.None): OwningTask<SemaphoreToken?>;
end Semaphore;

public class SemaphoreToken
  destructor;       // releases the held permit
end SemaphoreToken;
```

`Acquire()` and `TryAcquire(timeout)` follow the same blocking discipline as `Mutex`'s (v0.179; spec §30.12): compile error inside `async` bodies, runtime `InvalidOperationException` where a domain continuation would park in them, legal outside domains.

**Sendability (language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)).** `Semaphore` is **sendable** and internally synchronized — like `Mutex` ([§6.2](#sec-mutex-and-lock)) it may be reached by multiple domains simultaneously; that is its purpose (`Shared<Semaphore>` is the cross-domain sharing idiom, language spec §28.13). `SemaphoreToken` is **not** sendable: a held permit is bound to the acquiring domain and released there, like `Lock` ([§6.2](#sec-mutex-and-lock)) — moving a held permit between domains would decouple acquisition from release.

`AcquireAsync` returns `OwningTask<SemaphoreToken>`; awaiters consume the task under Pattern B (language spec [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)) — the await must appear in an owned-consuming context. The natural call site:

```bezel
using var token := await sem.AcquireAsync();
DoCriticalWork();
// token disposed at scope end -> semaphore released.
```

<a id="sec-atomict-and-atomicintegert"></a>
### 6.4 `Atomic<T>` and `AtomicInteger<T>`

Atomic operations on primitive types. The split into two classes — one for the operations that make sense on any cell-sized primitive, one for the integer-arithmetic extensions — replaces the single `Atomic<T>` type used through v0.64. The split makes the intrinsic restriction visible in the type system: `Increment`, `Decrement`, and `Add` are members of `AtomicInteger<T>` and unavailable on `Atomic<T>`, so a typo like `var b := new Atomic<bool>(false); b.Increment();` is a name-resolution error rather than an opaque intrinsic-restriction error.

<a id="sec-atomict-load-store-exchange-compare-and-swap"></a>
#### `Atomic<T>`: load, store, exchange, compare-and-swap

`T` is restricted by the compiler to the primitive types `bool`, `char`, `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `IntPtr`, and `UIntPtr`. This restriction is stricter than the `where T: value` constraint expressed in the declaration and is enforced as a compiler-intrinsic rule. Instantiating `Atomic<T>` with a record, enum, floating-point type, or other non-primitive value type is a compile error: *"Atomic<T> requires a cell-sized primitive type; T is not eligible. For floating-point atomics, use Atomic<int> with a bit-cast; for record-typed atomics, use a Mutex."*

```bezel
/// Compiler-intrinsic; T restricted to cell-sized primitive types.
public class Atomic<T> where T: value
  public constructor(initial: T);

  public property Value: T
    get;       // hardware-atomic read
    set;       // hardware-atomic write
  end Value;

  public method Exchange(value: T): T;
  public method CompareExchange(expected: T, replacement: T): T;
end Atomic;
```

`Value`'s read and write are atomic with respect to other `Value` reads, `Value` writes, and the `Exchange`/`CompareExchange` methods on the same instance. All §6.4 atomic operations are **sequentially consistent** (specified in v0.140): they participate in a single global order; no weaker ordering is offered. (Relaxed/acquire-release variants were considered and declined — see the changelog, v0.140; the surface stays minimal until profiles demand otherwise.) `Atomic<T>` and `AtomicInteger<T>` are **sendable** (language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)) and internally synchronized, and participate in the `Shared<T>` cross-domain rule of language-spec §28.13 (v0.140). The accessors are compiler-intrinsic; they are not auto-property storage.

`Exchange(value)` atomically replaces `Value` with `value` and returns the previous value. `CompareExchange(expected, replacement)` atomically replaces `Value` with `replacement` if and only if the current value equals `expected`, and returns the value observed (the original value, equal to `expected` on success, or the value that prevented the swap on failure).

<a id="sec-atomicintegert-integer-arithmetic"></a>
#### `AtomicInteger<T>`: integer arithmetic

`T` is restricted by the compiler to the primitive integer types: `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`. `bool`, `char`, `IntPtr`, and `UIntPtr` are not eligible — for those, use `Atomic<T>` and `CompareExchange` directly.

```bezel
/// Compiler-intrinsic; T restricted to primitive integer types.
public class AtomicInteger<T> where T: value
  public constructor(initial: T);

  public property Value: T
    get;
    set;
  end Value;

  public method Exchange(value: T): T;
  public method CompareExchange(expected: T, replacement: T): T;

  public method Increment(): T;        // returns post-increment value
  public method Decrement(): T;        // returns post-decrement value
  public method Add(delta: T): T;      // returns post-add value
end AtomicInteger;
```

`AtomicInteger<T>` is *not* a subtype of `Atomic<T>` — it is a separate class with the same load/store/CAS surface plus the arithmetic methods. The duplication is deliberate: a generic method that wants "any primitive atomic" should accept the relevant interface (no atomic-arithmetic interface currently exists) or be overloaded for both classes; promoting `AtomicInteger<T>` to a subtype would imply substitutability that the type system cannot otherwise express.

`Increment`, `Decrement`, and `Add` perform the operation atomically and return the post-operation value. Overflow follows the standard integer-arithmetic rule ([§6.1](Bezel_Language_Spec.md#sec-integer-arithmetic) of the language spec): an operation whose mathematical result is not representable in `T` raises `OverflowException`.

<a id="sec-events-ievent-eventsource"></a>
### 6.5 Events: `IEvent`, `EventSource`, `ISharedEvent`, `SharedEventSource`

An **event** is a named publication point on a class: the class controls when the event fires; external code subscribes but cannot fire the event or inspect the subscriber list. As of v0.187 events are not a member kind (the former `event` declaration is retired — language spec [§24](Bezel_Language_Spec.md#sec-events), rationale §13.6); an event is the following **pattern** over the types of this section, and this section is the normative home of event semantics. The generic parameter `T` is the event's **payload type** — the type of value passed to handlers when the event fires. The payload type may be any type. For a synchronous event the handler type is `(T) -> ()`; for an asynchronous event, `(T) -> Task`.

**The event pattern.** One `owned readonly` field holding the source, and — for a public event — one expression-bodied property exposing the subscribe-only interface face:

```bezel
public record ClickedEvent(Sender: Button, X: int, Y: int);

public class Button
  private owned readonly _clicked: EventSource<ClickedEvent> := new();
  public property Clicked: IEvent<ClickedEvent> => _clicked;

  private method OnPress(x: int, y: int)
    _clicked.Raise(new ClickedEvent(this, x, y));
  end OnPress;
end Button;

// Subscription sites:
using var sub := button.Clicked.Subscribe((e) => UpdateStatus(e.X, e.Y));
```

Every piece is an existing mechanism: the field is an ordinary `owned` field (initialized during construction, disposed with the object — spec §28.5), `readonly` per spec §22.8, initialized with the spec §17.7 `new()` shorthand; the property is a spec §23.2 expression-bodied read-only property whose body is a receiver-sourced borrow (spec §28.6). A **`private` event needs no property** — the field alone is the event (one line). A **`protected` event** declares the field `protected`; derived classes raise through it. **Async events** use `AsyncEventSource<T>` / `IAsyncEvent<T>` identically. **No-payload events** use `EventSource<()>`; both `EventSource<()>` and `SharedEventSource<()>` provide a convenience `Raise()` overload that takes no arguments, equivalent to `Raise(())`. `readonly` on the field is the pattern's safety feature, not decoration: an accidental reassignment would otherwise silently dispose the source and sever every live subscription (spec §28.7 drop-on-overwrite); the producer-side `_clicked += h` spelling remains available on the `readonly` field because the subscription forms neither read nor assign the operand (spec §17.2). An observable **interface contract** is a get-only interface property of the interface face — `readonly property PropertyChanged: IEvent<PropertyChangedEvent>;` — which the pattern's property satisfies directly.

```bezel
public interface IEvent<out T>
  method Subscribe(scoped handler: (T) -> ()): owned Subscription;
end IEvent;

public interface IAsyncEvent<out T>
  method Subscribe(scoped handler: (T) -> Task): owned Subscription;
end IAsyncEvent;

public class EventSource<T>: IEvent<T>
  public constructor();
  public method Subscribe(scoped handler: (T) -> ()): owned Subscription;
  public method Raise(payload: T);

  public method Add(scoped handler: (T) -> ());         // scoped (v0.180): Add stores — spec §28.14
  public method Remove(handler: (T) -> ());              // call-bounded: Remove only compares
  destructor;    // severs the subscriber list before reclamation (spec §28.14 quiescence)
end EventSource;

public class AsyncEventSource<T>: IAsyncEvent<T>
  public constructor();
  public method Subscribe(scoped handler: (T) -> Task): owned Subscription;
  public async method RaiseAsync(payload: T): Task;

  public method Add(scoped handler: (T) -> Task);        // scoped (v0.180): Add stores — spec §28.14
  public method Remove(handler: (T) -> Task);             // call-bounded: Remove only compares
  destructor;    // severs the subscriber list before reclamation (spec §28.14 quiescence)
end AsyncEventSource;
```

The constructors are public as of v0.187 (previously "no public constructor; obtained via event declaration"): the pattern constructs its own sources, and standalone construction threatens nothing — a third party constructing a *fresh* `EventSource<T>` gains capability over its own event only.

**Payload records.** Event payloads should be records; there is no required base class. The sender, if any, is a field of the payload record, typed precisely — the pattern example's `ClickedEvent` above follows the same convention:

```bezel
record ValueChangedEvent(Sender: Slider, OldValue: decimal, NewValue: decimal);
record DataReceivedEvent(Sender: Connection, Payload: Array<byte>, Timestamp: DateTime);
record ProcessResult(Success: bool, Message: string);
```

A record provides named fields, destructuring in handlers, and a single point of documentation for the event's data contract. (A record can hold the *payload* but not the *source*: an `EventSource<T>` field is an `owned` field, which records cannot carry — spec §9.7.)

**Subscribing.** `Subscribe` — on the interface, so available everywhere — registers a handler and returns an owned `Subscription` ([§2.11](#sec-subscription)); disposing the subscription removes the handler. The producer-side forms `source += handler` / `source -= handler` (a fixed, language-defined desugaring to `Add`/`Remove` — normative statement in spec §17.2) are available wherever the source itself is in scope, i.e. to the declaring class; `-=` removes the first handler matching by function-value equality (spec §27.7.1) and is a no-op when no handler matches. `Add` is a `scoped` (storing) position: the caller-side capture admission is spec §28.14's (for the pattern's `_clicked += OnClicked`, bounding object `this` via form 4 — the receiver chain roots at `this` through an `owned` field). Storing a `Subscription` in an **`owned` field of `this`** resolves spec §28.14's bounding object to `this` (form 3), so a handler capturing `this` (or state reached through it) is admitted *regardless of how the publisher was reached* — a parameter, a field, an interface face. Disposing `this` disposes the field, severing the registration before `this` is reclaimed (spec §28.12); the recorded residual is that the subscription must not be moved out past `this` (spec §28.14, the registration-lifetime residual). This is the standard observer pattern:

```bezel
public class TickObserver
  private owned _sub: Subscription;
  private _count: int := 0;

  public constructor(ticks: IEvent<TickEvent>)
    _sub := ticks.Subscribe((e) => do OnTick(e); end);   // captures `this` — admitted:
  end constructor;                                        //   bounding object is `this` (§28.14 form 3)

  private method OnTick(e: TickEvent)
    _count := _count + 1;
  end OnTick;
end TickObserver;
```

Captured constructor parameters other than `this` are not admitted — a constructor parameter is a borrow of a caller local and does not provably outlive the constructed object. A raise cannot reach the handler before the constructor completes unless the constructor itself — directly or transitively — raises the subscribed event: instance sources are domain-serial (below) and no other continuation runs during construction. A constructor that does so observes `this` partially initialized — the construction-time-observation residual of spec §22.7 (v0.184), with the runtime bound recorded there. For handlers that must not capture `this`, `using var sub := …` remains the scope-bound alternative (spec §28.14 form 2).

**Raising.** `Raise` and `RaiseAsync` invoke handlers in subscription order, over a **snapshot of the subscriber list taken when the raise begins** (v0.163): a handler that subscribes or unsubscribes during the raise affects the *next* raise, never the in-flight pass — snapshot rather than fault, because self-unsubscribing one-shot listeners are a legitimate idiom (spec §29.12's loud-mutation discipline is the contrasting response to mutation mid-traversal; the correction of the former spec §24.5 "same posture" comparison is recorded in rationale §13.6). With zero subscribers, both methods are no-ops — no null check, no `?.`, no `Invoke` indirection. Handlers execute **on the raising domain's thread** (v0.180 — normative): for an instance source this is invisible (all access is domain-serial); for a shared source (below) a handler subscribed from one domain runs on whichever domain raises — which is why shared-source handlers must be capture-free. The synchronous `Raise` returns `()` and runs each handler to completion before invoking the next. `RaiseAsync` returns a `Task` that completes when every handler's task completes; **each handler's task is awaited before the next is invoked.**

**Handler exceptions — first error wins (rationale §13.5).** If a synchronous handler throws, subsequent handlers are not invoked and the exception propagates out of `Raise` to the raiser. If an asynchronous handler's task faults, subsequent handlers are not invoked and the exception propagates from the awaited `RaiseAsync`. If an asynchronous handler's task is **cancelled**, the behavior is the same as a fault: subsequent handlers are not invoked and `OperationCanceledException` propagates from the awaited `RaiseAsync`. To run all handlers regardless of failures and aggregate exceptions, the raiser must implement that policy explicitly — `Raise`/`RaiseAsync` do not aggregate. Handlers that must run independently catch their own exceptions (fragment context: `_dataReceived` is the connection's own source field, so the async lambda's bare `ProcessPayloadAsync`/`Log` calls capture `this` and are admitted by spec §28.14's subscription admission):

```bezel
_dataReceived += async (e) => do
  try
    await ProcessPayloadAsync(e.Payload);
  catch
    var ex: Exception => Log($"Handler failed: {ex.Message}")
  end;
end;
```

**Thread safety, per type.** An `EventSource<T>`/`AsyncEventSource<T>` subscriber list is **not synchronized**, and within a single execution domain (spec §30.13) it does not need to be: continuations never run concurrently, so subscribe, unsubscribe, and raise — from sync or async code — are serialized by construction (v0.163 reasoning, recorded). When handlers are async, the subscriber list may still change between a raise's suspension points; that is interleaving, not a data race. External synchronization (`Mutex`, [§6.2](#sec-mutex-and-lock)) is required only where event operations are reachable from foreign threads through the FFI (spec §32). A `SharedEventSource<T>` (below) is the internally-synchronized exception. Quiescence is normative for every source type of this section: the source's destructor severs the subscriber list before reclamation (spec §28.14), so a publisher's disposal is a deterministic unsubscribe-all.

**The type fence.** `Add`, `Remove`, and `Raise` are `public` rather than `internal` because the producer-side restriction is carried by **types**: the producer surface of a given event is fenced by the **field's** visibility. An `EventSource<T>` reference for someone else's event is reachable only if their class exposes it — or if a holder of the interface face downcasts it (`face as EventSource<T>`), which is the recorded, not-defended-against breach: a type-driven fence is exactly as strong as the type system's static view, and downcast is that view's built-in escape. Code that smuggles a source out of its class — through a public member or through the FFI — gains `Raise`, as a deliberate breach of the pattern's contract that the language does not attempt to defend against. The conventional design is to never expose the `EventSource<T>` itself; the public surface is always the interface face. Third-party construction of a *fresh* source (the public constructor) grants no capability over anyone else's events.

**Variance.** `IEvent<T>` and `IAsyncEvent<T>` are covariant in T (`out T`): `IEvent<Dog>` is a subtype of `IEvent<Animal>`, the same direction as `IEnumerable<out T>` — sound because T appears once, in the parameter position of `(T) -> ()` (contravariant), itself a method parameter (contravariant), composing to covariant overall (spec §12.9). The source classes are invariant — `Raise(payload: T)` places T in input position. The conventional public surface is the interface, so user-facing variance is expected through `IEvent<out T>`/`IAsyncEvent<out T>` (and `ISharedEvent<out T>`); an exposed source is simply invariant, as its signature dictates. Covariant exposure is directly declarable by the pattern: an `EventSource<Dog>` field behind a `property Sightings: IEvent<Animal>` face.

**Shared events (cross-domain publication points).** Static state is process-global, and a publication point reachable from every domain cannot be an unsynchronized `EventSource<T>` (spec §22.12). The shared pair carries the guarantees in its declared positions:

```bezel
public interface ISharedEvent<out T>
  method Subscribe(handler: (T) -> ()): owned Subscription;
      // capture-free admission: the handler must be a non-capturing lambda or a
      // static method reference — a stdlib-stated position in spec §28.14's
      // capture-free register (the same register Domain.Spawn's entries live in),
      // NOT a `scoped` storage-bounded position.
end ISharedEvent;

public sealed class SharedEventSource<T>: ISharedEvent<T>
  public constructor();
  public method Subscribe(handler: (T) -> ()): owned Subscription;   // capture-free (as above)
  public method Raise(payload: T);       // synchronized snapshot-raise; handlers run on the raising domain
  public method Add(handler: (T) -> ());                             // capture-free (as above)
  public method Remove(handler: (T) -> ());                          // call-bounded: compares only
  destructor;                            // synchronized quiescence: severs the subscriber list under the
                                         // same lock discipline as subscribe/raise
end SharedEventSource;
```

- **Synchronization.** The subscriber list is internally synchronized: subscribe, raise, and `Subscription.Dispose` may arrive from any domain. A raise takes the v0.163 snapshot; handlers execute on the raising domain; first-error-wins applies unchanged. The destructor severs the list under the same lock as subscribe/raise — a concurrent subscribe either lands before the severance and is severed, or observes the disposed source; program-exit disposal of the recommended `static owned readonly` field runs per spec §22.13 on the shutdown path with no other Bezel code running, which makes exit-time severance race-free.
- **Capture-freedom at every subscribe position.** Every subscribe position of the pair (`Subscribe` on the interface and the class, `Add`, and the `+=` desugaring, spec §17.2) admits only **capture-free** handlers — a non-capturing lambda or a static method reference — as a stdlib-stated entry in spec §28.14's capture-free register (`Domain.Spawn`'s admission), keyed on the **resolved callee**: the check applies wherever a call resolves to a subscribe position declared on the pair, a constrained generic receiver included. Two companion rules make the admission alias-proof: `SharedEventSource<T>` deliberately does **not** implement `IEvent<T>` (an upcast would launder the requirement through `IEvent<T>.Subscribe`'s capture-admitting `scoped` signature), and forming a method reference over a capture-free-register position is a compile error (spec §28.14, v0.187 — the formed plain function type would erase the admission). The requirement is therefore enforced on every route through which a subscription can be written. A payload never needs to be sendable: under capture-freedom plus raising-domain execution, each payload is constructed and consumed within one domain.
- **The severed `IEvent<T>` composition, and its bridge.** A shared source does not compose with `IEvent<T>`-consuming APIs (the `TickObserver` shape above). The bridge for that use case is a domain-local relay: subscribe a capture-free forwarder that routes payloads through a process-global `ChannelSender<T>` static (spec §22.12 rule 2 form (i)); the consuming domain owns the `Channel<T>`, receives, and re-raises on a local instance `EventSource<T>`, whose `IEvent<T>` face composes with anything. Payloads crossing the bridge must be sendable (spec §30.14) — inherent to any cross-domain delivery.
- **User implementations of `ISharedEvent<T>`** are admissible but gain no retention right: a user-authored `Subscribe` body is verified under spec §28.14's ordinary call-bounded regime, so it can neither store its handler parameter nor forward it into a register position. **User classes cannot be cross-domain publication points in v1** (recorded; the lift path is §28.14's deferred unbounded tier, #1427). `SharedEventSource<T>` is `sealed`, so no derived class inherits the trusted position.
- **Statics.** `SharedEventSource<T>` is in spec §22.12 rule 2's form-(i) synchronized set: the recommended shape `private static owned readonly _exiting: SharedEventSource<()> := new();` is admitted warning-free, with program-exit disposal (spec §22.13). The recommended public face is a static expression-bodied property of the interface face, as in the idiom below — a bare read of a `static` `owned` field returned as a borrow of process-lived static storage (spec §28.3/§28.6, v0.187). A *mutable* static of the type is admissible under rule 2's store semantics; note that overwrite-never-disposes means a displaced source leaks with its subscriber list intact until process exit — prefer `readonly`. The type is **not sendable** (spec §30.14): static reachability, not transfer, is its distribution mechanism — and it is deliberately not in spec §28.13's `Shared`-payload set (`Shared<SharedEventSource<T>>` would add a second distribution mechanism).
- **The shared async twin is deferred.** `ISharedAsyncEvent<T>`/`SharedAsyncEventSource<T>` (a synchronized `RaiseAsync` awaiting each handler's task on the raising domain) ships when demand appears; until then a cross-domain async publication point is not spellable, and the channel bridge above is the alternative.

The static idiom, complete:

```bezel
public static class App
  private static owned readonly _exiting: SharedEventSource<()> := new();
  public static property Exiting: ISharedEvent<()> => _exiting;

  public static method NotifyExiting()
    _exiting.Raise();
  end NotifyExiting;
end App;

// consumer, any domain — handler must be capture-free (static method ref / non-capturing lambda):
using var sub := App.Exiting.Subscribe(OnExit);
```

All six event types live in `System.Threading` and are always available without import (language spec [§2.3](Bezel_Language_Spec.md#sec-always-available-names), v0.187 — the `CancellationToken` precedent). `Subscription` (the return type of `Subscribe`) is declared in [§2.11](#sec-subscription) and lives in the auto-imported `System` module. v0.76 moved it from `System.Threading` so that all register-and-unregister APIs returning a `Subscription` work without requiring an explicit threading-module import.

<a id="sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet"></a>
### 6.6 `TaskCompletionSource<T>` and `OwningTaskCompletionSource<T>`

See [§30.11](Bezel_Language_Spec.md#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet) of the language spec.

```bezel
// For non-class T (value types, string, runtime-managed reference types).
// Instantiating with a class T is a compile error pointing to OwningTaskCompletionSource<T>.
public class TaskCompletionSource<T>
  public constructor();
  public readonly property Task: Task<T>;

  public method SetResult(result: T);
  public method SetException(owned exception: Exception);   // v0.179: the task owns its fault (spec §30.9)
  public method SetCancelled();

  public method TrySetResult(result: T): bool;
  public method TrySetException(owned exception: Exception): bool;
  public method TrySetCancelled(): bool;
  destructor;    // disposes the produced task, faulting it first if the source
                 //   is disposed before completion (spec §30.11, v0.184)
end TaskCompletionSource;

// For class T. Result is transferred via `owned` parameter; produced task is Pattern B.
public class OwningTaskCompletionSource<T> where T: class
  public constructor();
  public readonly property Task: OwningTask<T>;

  public method SetResult(owned result: T);
  public method SetException(owned exception: Exception);   // v0.179: the task owns its fault (spec §30.9)
  public method SetCancelled();

  public method TrySetResult(owned result: T): bool;
  public method TrySetException(owned exception: Exception): bool;
  public method TrySetCancelled(): bool;
  destructor;    // disposes the produced task, faulting it first if the source
                 //   is disposed before completion (spec §30.11, v0.184)
end OwningTaskCompletionSource;

// Non-generic form for void-completion tasks
public class TaskCompletionSource
  public constructor();
  public readonly property Task: Task;
  public method SetResult();
  public method SetException(owned exception: Exception);
  public method SetCancelled();
  public method TrySetResult(): bool;
  public method TrySetException(owned exception: Exception): bool;
  public method TrySetCancelled(): bool;
  destructor;    // disposes the produced task, faulting it first if the source
                 //   is disposed before completion (spec §30.11, v0.184)
end TaskCompletionSource;
```

**Fault-path ownership (v0.179).** `SetException`/`TrySetException` (all forms) take the exception **`owned`**: the produced task becomes the single owner of its fault exception (spec §30.9 — awaiters borrow it on re-raise; it is disposed exactly once, at task disposal; a `TrySetException` returning `false` disposes the passed exception). A caller that holds only a caught borrow (spec §31.1) constructs a fresh cause-chained exception instead: `tcs.SetException(new Exception(ex.Message, ex));` — the `(message, cause)` constructor snapshots the chain (spec §31.4).

<a id="sec-task-static-helpers"></a>
### 6.7 `Task` static helpers

See [§30.10](Bezel_Language_Spec.md#sec-standard-library-task-helpers) of the language spec for the full list. They live on `Task` and `Task<T>`.

**Task-family returns are caller-owned productions (v0.190).** Every method in this library whose declared return type is `Task`, `Task<T>`, or `OwningTask<T>` — the async acquisitions (`AcquireAsync`/`TryAcquireAsync`, §6.2–§6.3), `Locked<T>`/`RwLocked<T>`'s `LockAsync`/`ReadAsync`/`WriteAsync` families (§6.11), `Channel<T>.Receive`/`SendAsync` (§6.9), stream and file I/O (`ReadAsync`/`WriteAsync`/`ReadAllAsync`, §7.1–§7.3), `RaiseAsync` (§6.5), `TaskScope.Join` (§6.10), `IAsyncDisposable.DisposeAsync` and `MoveNextAsync` (§2.10, §4.12–§4.13), and the static helpers below — mints a **fresh task owned by the caller** at every call site (language spec [§28.3](Bezel_Language_Spec.md#sec-producers), v0.190). The signatures carry no `owned` marker: the `Task`-family return type is itself the transfer announcement. Where a signature does spell it out — `Domain.Spawn(...): owned Task` (§6.8), the `Parallel` combinators (§6.12) — the marker states the same contract explicitly and is retained for emphasis at the domain boundary; it is not a semantic difference from the unmarked async forms. Stored tasks are exposed as **properties** (`TaskCompletionSource<T>.Task`, `OwningTaskCompletionSource<T>.Task`, §6.6), whose reads are borrows of the source's storage (spec [§30.11](Bezel_Language_Spec.md#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet)).

```bezel
public class Task
  // For non-class T (value types, string, runtime-managed reference types).
  // For class T, use FromOwnedResult instead.
  public static method FromResult<T>(value: T): Task<T>;

  // For class T. Produces a Pattern B task; awaiters consume under `using var x := await ...`.
  public static method FromOwnedResult<T>(owned value: T): OwningTask<T> where T: class;

  // For any T: a faulted task never produces its result, so the Pattern A vs
  // Pattern B distinction does not apply. `FromException<MyClass>(ex)` is correctly
  // typed as Task<MyClass> even when MyClass is a class type — there is no value
  // to transfer ownership of, only an exception to re-raise on await.
  public static method FromException<T>(owned exception: Exception): Task<T>;   // v0.179: the task owns its fault (spec §30.9)
  public static method FromCancelled<T>(): Task<T>;

  public static method Delay(milliseconds: int): Task;
  public static method Delay(milliseconds: int, token: CancellationToken): Task;
  public static method Delay(duration: TimeSpan): Task;
  public static method Delay(duration: TimeSpan, token: CancellationToken): Task;

  public static method WhenAll(tasks: Array<Task>): Task;

  // For non-class T (value types, string, runtime-managed reference types).
  public static method WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>;

  // For class T. Aggregates OwningTask<T> results into an owned array of owned elements.
  // The caller transfers ownership of the input array (and its tasks) to WhenAllOwned;
  // the returned task produces an owned OwningArray<T>.
  public static method WhenAllOwned<T>(
    owned tasks: OwningArray<OwningTask<T>>
  ): OwningTask<OwningArray<T>> where T: class;

  public static method WhenAny(tasks: Array<Task>): Task<int>;   // index of the first task to complete (v0.162)
  public static method FireAndForget(owned task: Task);
end Task;
```

`FireAndForget` registers a continuation that logs any exception the task produces via `Debug.WriteLine`, then disposes the task. The calling code relinquishes ownership (the parameter is `owned`); the runtime supervises the task's lifecycle.

The parameter type `Task` accepts any subtype: `Task`, `Task<T>`, and `OwningTask<T>` all satisfy the parameter. For `OwningTask<T>` (Pattern B), the runtime's task disposal cascades through any unconsumed owned result — `FireAndForget(someOwningTask)` is semantically "let it run to completion; if it produces a result, dispose it; if it produces an exception, log it." This is the right pattern when the caller wants the underlying operation to complete its side effects (e.g., an async write whose `OwningTask<WriteHandle>` would normally yield a handle, but the caller doesn't need the handle).

**`FireAndForget` is the only supported way to discard a `Task`** as of v0.66. The discard form `_ := someAsyncCall();` is rejected at compile time because `Task` has a destructor that handles continuation cleanup; the discard would skip it. The two legal patterns are:

```bezel
Task.FireAndForget(DoTelemetryAsync());      // supervised; exceptions logged
using var t := DoTelemetryAsync();           // unsupervised; disposed at scope end
```

`FireAndForget` is the right choice when the task should run to completion and any exceptions it raises should be logged. The `using var` form binds the task to a scope; at scope end, the task's `Dispose()` runs but does not block waiting for completion ([§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)). The underlying operation continues to run in the background; the caller no longer observes its completion or exceptions. (In checked disposal mode, a `DisposedAccessException` raised in such an abandoned continuation is reported through the diagnostic channel before being swallowed — spec §30.10, v0.179.) Use `using var` only when "fire and forget without supervision" is exactly what's wanted; for tasks whose failures should be surfaced, use `Task.FireAndForget` or hold the task and `await` it explicitly.

The exception-logging mechanism for `FireAndForget` is implementation-defined; implementations may provide a global handler.

<a id="sec-fromexception-and-fromcancelled-for-class-t"></a>
#### `FromException` and `FromCancelled` for class T

These factories are correctly typed as `Task<T>` regardless of whether T is a class type. A faulted or cancelled task never produces a result value — `await` on such a task re-raises the original exception (`FromException`) or `OperationCanceledException` (`FromCancelled`). Because no value flows from producer to awaiter, the Pattern A vs Pattern B distinction that motivates `OwningTask<T>` does not apply. `Task.FromException<FileStream>(ex)` produces `Task<FileStream>`, not `OwningTask<FileStream>`; the awaiter never observes a `FileStream` instance and so has no ownership to discharge.

Code that needs an immediately-faulted task in a context expecting `OwningTask<T>` writes an **`async` wrapper that throws** — the throw in the synchronous prefix faults the returned task (spec [§30.9](Bezel_Language_Spec.md#sec-exception-handling-in-async-methods)), and the fault surfaces at the caller's await:

```bezel
public async method FailFast(): OwningTask<MyClass>
  throw new InvalidOperationException("no MyClass available");
end FailFast;
```

An `OwningTaskCompletionSource<T>` cannot express this shape as a return value (v0.184 — an earlier revision showed `return src.Task;` out of the `using var src` scope, which the source-owns-task contract of spec [§30.11](Bezel_Language_Spec.md#sec-common-semantics) rejects as a borrow escaping its source; the same example also passed an exception of unstated provenance where `SetException` requires a visibly-owned argument — a caught borrow additionally needs a fresh cause-chained instance, [§6.6](#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet)). The TCS remains the tool for **imperative** completion — callback bridging inside a live scope — not for minting detached faulted tasks.

<a id="sec-whenall-for-class-t-whenallowned"></a>
#### `WhenAll` for class T: `WhenAllOwned`

The generic `WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>` aggregates results from non-class T (value types, `string`, runtime-managed reference types). For class T held by `OwningTask<T>`, use `WhenAllOwned<T>`:

```bezel
public async method LoadAllConfigs(paths: Array<string>): OwningTask<OwningArray<Config>>
  var tasks := new OwningArray<OwningTask<Config>>();
  for var path in paths do
    tasks.Add(LoadConfigAsync(path));    // each task is OwningTask<Config>; transfers in
  end;
  return await Task.WhenAllOwned(owned tasks); // tasks consumed; returned array owns the configs
end LoadAllConfigs;
```

The caller's pattern matches `WhenAll`: build an array of tasks, await `WhenAllOwned`, receive an array of results. The differences are in the type and ownership flow:

- The input array is `OwningArray<OwningTask<Config>>`, not `Array<OwningTask<Config>>`. Each element is itself owned (OwningTask<T> for class T is single-consumer per language spec [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal); the array owns its tasks for the duration).
- The input parameter is `owned OwningArray<...>`: the caller transfers ownership of the array (and its contents) to `WhenAllOwned`. After the call, the caller no longer holds the array.
- The result is `OwningTask<OwningArray<Config>>`: a Pattern B task whose result is an owned array of owned configs. The caller awaits with `using var`, gaining scope-bound disposal of the whole structure.

Implementation sketch (v0.179: the *behavior* shown — await in input order, first-fault re-raise, disposal of already-produced and laggard results — is now contract, spec §30.10 "Aggregation contracts"; the code itself remains illustrative):

```bezel
public static async method WhenAllOwned<T>(
  owned tasks: OwningArray<OwningTask<T>>
): OwningTask<OwningArray<T>> where T: class
  using var taskArray := owned tasks;                 // owned param adopted (v0.180 marker); disposes at end
  var results := new OwningArray<T>(capacity := taskArray.Count);
  for var task in taskArray.Drain() do                // drain yields owned OwningTask<T>
    results.Add(await task);                          // await produces owned T (Pattern B);
                                                      //   Add(owned item) is an owned-consuming
                                                      //   context, satisfying §30.5
  end;
  return results;                                     // ownership transfers to caller
end WhenAllOwned;
```

The implementation uses `OwningArray<T>.Drain()` to take ownership of each task out of the input array, awaits each (Pattern B), and transfers the awaited result into the result array via `Add(owned item)`. The pattern works because every collection slot in the chain is an owned-element slot, and because `Add(owned item)` is an owned-consuming context that admits `await` directly per the loosened Pattern B rule (language spec [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)). The for-loop's iteration variable `task` is owned (per the new rule for iterators returning `IOwningEnumerable<T>`, language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) — "Iterator yield ownership"), and is consumed by the await on each iteration.

For genuine concurrency (the underlying tasks running in parallel), the caller should start the tasks *before* passing the array to `WhenAllOwned`. Each task's underlying operation begins when the task is constructed; awaiting blocks for completion. The `WhenAllOwned` body awaits sequentially, but the tasks themselves run in parallel because they were started earlier.

For producer-consumer pipelines that yield owned values incrementally, an async iterator remains the clean idiom. When the iterator's return type is `IOwningAsyncEnumerable<T>`, the consumer takes ownership of each yielded value, with the standard owned-discharge rules applying to the iteration variable inside the `await for` body (language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) — "Iterator yield ownership"):

```bezel
public async method OpenFiles(paths: Array<string>): IOwningAsyncEnumerable<FileStream>
  for var path in paths do
    yield return await OpenFileAsync(path);
  end;
end OpenFiles;

// Consumer takes ownership of each FileStream:
public async method ProcessAll(paths: Array<string>): Task
  await for var file in OpenFiles(paths) do
    using var f := owned file; // discharge on entry — covers all exit edges (v0.180 marker)
    await ProcessAsync(f);
  end;
end ProcessAll;
```

The `using var f := owned file;` shape on entry to the body — the `owned` marker is the v0.180 binding-position transfer spelling (language spec [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows); unmarked, the binding would only borrow-pin) — is the canonical way to discharge the iteration variable, exactly as for synchronous owned-element iteration (language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for)).

The async-iterator form is preferable when the consumer processes each result before requesting the next; `WhenAllOwned` is preferable when the consumer needs all results at once.

<a id="sec-domain"></a>
### 6.8 `Domain`

Static class for spawning execution domains (language spec [§30.14](Bezel_Language_Spec.md#sec-multiple-domains)). The compiler enforces the call-site rules of §30.14: the entry must be a non-capturing lambda or a static method reference, and the argument and result types must be sendable (the derived type property of language spec [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)/[§30.14](Bezel_Language_Spec.md#sec-sendable-types)).

```bezel
public static class Domain
  // No result.
  public static method Spawn(entry: () -> ()): owned Task;
  public static method Spawn<A>(entry: (owned A) -> (), owned arg: A): owned Task
      where A: class;                                      // A sendable (compiler-checked)
  public static method Spawn<A>(entry: (A) -> (), arg: A): owned Task
      where A: notnull;                                    // A sendable value type or string

  // Class-typed result: ownership transfers to the awaiter (Pattern B).
  public static method Spawn<R>(entry: () -> owned R): owned OwningTask<R>
      where R: class;                                      // R sendable (compiler-checked)
  public static method Spawn<A, R>(entry: (owned A) -> owned R, owned arg: A): owned OwningTask<R>
      where A: class, R: class;
  public static method Spawn<A, R>(entry: (A) -> owned R, arg: A): owned OwningTask<R>
      where A: notnull, R: class;

  // Value-typed or string result (Pattern A).
  public static method Spawn<R>(entry: () -> R): owned Task<R>
      where R: notnull;                                    // R sendable value type or string
  public static method Spawn<A, R>(entry: (owned A) -> R, owned arg: A): owned Task<R>
      where A: class, R: notnull;
  public static method Spawn<A, R>(entry: (A) -> R, arg: A): owned Task<R>
      where A: notnull, R: notnull;

  // Async entries (v0.179; spec §30.14 "Async entries"): the runtime awaits the
  // returned task inside the spawned domain; the join handle tracks that task.
  public static method Spawn(entry: () -> Task): owned Task;
  public static method Spawn<A>(entry: (owned A) -> Task, owned arg: A): owned Task
      where A: class;
  public static method Spawn<A>(entry: (A) -> Task, arg: A): owned Task
      where A: notnull;

  public static method Spawn<R>(entry: () -> OwningTask<R>): owned OwningTask<R>
      where R: class;
  public static method Spawn<A, R>(entry: (owned A) -> OwningTask<R>, owned arg: A): owned OwningTask<R>
      where A: class, R: class;
  public static method Spawn<A, R>(entry: (A) -> OwningTask<R>, arg: A): owned OwningTask<R>
      where A: notnull, R: class;

  public static method Spawn<R>(entry: () -> Task<R>): owned Task<R>
      where R: notnull;
  public static method Spawn<A, R>(entry: (owned A) -> Task<R>, owned arg: A): owned Task<R>
      where A: class, R: notnull;
  public static method Spawn<A, R>(entry: (A) -> Task<R>, arg: A): owned Task<R>
      where A: notnull, R: notnull;
end Domain;
```

The `where` clauses above are the declared constraints; sendability is the stronger compiler-checked requirement and is diagnosed at the call site naming the failing rule (it is not expressible as a `where` constraint — it is a derived type property). For more than one argument, pass a sendable class aggregating them, an `OwningArray<T>` of sendable elements, or a `ChannelSender<T>` ([§6.9](#sec-channel)) and send the rest.

**Async entries (v0.179).** The nine async rows mirror the nine synchronous rows. The entry is a capture-free `async` lambda or a static `async` method reference; the runtime runs it as the spawned domain's first continuation and awaits the returned task **within that domain** (the `async Main` analog, spec §33.2). The join handle completes when that task settles; a fault anywhere in the entry's async chain surfaces at the join as `DomainFaultedException`. The entry's task object never crosses the boundary; for the `OwningTask<R>` rows the runtime is the single Pattern B consumer, and only the sendable result crosses. Overload resolution prefers the async rows for any entry whose return type is in the `Task` family; the synchronous generic rows never bind `R` to a task type (spec §30.14).

The join handle completes when the entry's task completes. A fault in the entry surfaces at the join as `DomainFaultedException`:

```bezel
public class DomainFaultedException: Exception
  public readonly property FaultStackTrace: string;   // stack trace text of the original exception
  // Message carries the original exception's message text.
end DomainFaultedException;
```

The original exception object never crosses the boundary (exception classes are not in general sendable); `Message` and `FaultStackTrace` are copied strings. `DomainFaultedException` requires `import System.Threading` like the rest of this module.

There is no built-in cross-domain cancellation: `CancellationToken` is a borrow-bearing record and not sendable. The pattern is a dedicated channel carrying a cancel message, polled by the spawned domain at its safe points.

<a id="sec-channel"></a>
### 6.9 `Channel<T>` and `ChannelSender<T>`

Multiple-producer, single-consumer channel carrying sendable values between domains with ownership transfer at both ends (language spec [§30.15](Bezel_Language_Spec.md#sec-channels)). Both types are compiler-known and internally synchronized; together with `Mutex`, `Semaphore`, the atomics (`Atomic<T>`/`AtomicInteger<T>`), and the data-owning locks (`Locked<T>`/`RwLocked<T>`, v0.186) — stdlib [§6.2](#sec-mutex-and-lock)–[§6.4](#sec-atomict-and-atomicintegert), [§6.11](#sec-locked-and-rwlocked) — and, by deep immutability rather than machinery, `FrozenArray<T>` behind `Shared` handles or as a `static readonly` singleton ([§4.19](#sec-frozen-array), v0.186), they are the standard types two domains may reach simultaneously (the sendable, internally-synchronized set; language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)). T must be sendable; the instantiation is rejected otherwise (language spec [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)). A **mutable static** `Channel<T>` field is rejected by language spec [§22.12](Bezel_Language_Spec.md#sec-static-members) rule 2 (v0.181), and a `static readonly Channel<T>` draws rule 4's warning: the channel object is the single-consumer *receiving* end and must stay confined to one consuming domain — when producers across many domains need a process-global send endpoint, publish a `ChannelSender<T>` static instead (rule 2's synchronized set).

The signature surface dispatches on T's category, as with `OwningBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)). For class-typed (sendable) T:

```bezel
public class Channel<T>: IDisposable                       // the receiving end; class T form
  public constructor();                                    // unbounded
  public constructor(capacity: int);                       // BOUNDED (v0.174, #809): back-pressure;
                                                           //   capacity >= 1, ArgumentException otherwise

  public readonly property Count: int;                     // items currently buffered
  public readonly property IsClosed: bool;                 // true once closed (see below)

  public method NewSender(): owned ChannelSender<T>;       // mint a sender endpoint

  public method Receive(): OwningTask<T>;                   // await transfers ownership of one item
  public method TryReceive(out owned item: T?): bool;      // non-blocking; false when none available
  public method ReceiveAll(): IOwningAsyncEnumerable<T>;  // v0.186: drain form — see below
  destructor;    // closes the channel; disposes every undelivered class-typed
                 //   item in reverse insertion order (language spec §30.15)
end Channel;

public class ChannelSender<T>: IDisposable                 // class T form
  public method Send(owned item: T);                       // transfers ownership into the channel;
                                                           //   never blocks; on a FULL bounded channel
                                                           //   raises ChannelFullException (v0.174);
                                                           //   ChannelClosedException if closed
  public async method SendAsync(owned item: T): Task;      // v0.174 (#809): awaits capacity on a
                                                           //   bounded channel (immediate when space
                                                           //   or unbounded); ChannelClosedException
                                                           //   if the channel closes while waiting
  destructor;    // disposing the last sender closes the channel (language spec §30.15)
end ChannelSender;
```

For sendable value-typed T and `string`, the ownership modifiers drop out: `Receive(): Task<T>`, `TryReceive(out item: T?): bool`, `ReceiveAll(): IAsyncEnumerable<T>`, `Send(item: T)`, `SendAsync(item: T): Task`.

Semantics (normative statement: language spec [§30.15](Bezel_Language_Spec.md#sec-channels)):

- `Channel<T>` and `ChannelSender<T>` are themselves sendable: each endpoint is an independently owned object and may be moved to another domain (as a spawn argument or through another channel). Typical use keeps the channel in the receiving domain and moves senders out.
- **Closing.** Disposing the last sender closes the channel; buffered items remain receivable, and after the drain `Receive` raises `ChannelClosedException`. Disposing the `Channel<T>` closes it immediately and disposes every undelivered class-typed item in reverse insertion order; subsequent `Send` raises `ChannelClosedException`. `TryReceive` returns `false` for both empty-open and drained-closed; consult `IsClosed` to distinguish.
- **Ordering.** Per-sender FIFO; cross-sender interleaving is an unspecified consistent total order; each item is received exactly once.
- **`ReceiveAll` (v0.186).** The non-exceptional drain form: `await for var item in inbox.ReceiveAll() do … end;` yields each item with `Receive`'s exact transfer semantics (ownership transfers for class T), suspends while empty-and-open, and completes the enumeration **normally** on drained-and-closed — `ChannelClosedException` stops being control flow for the *normal* exit. At most one consumption may be in progress: beginning a `ReceiveAll` while another is active or while a `Receive` task is pending, and calling `Receive`/`TryReceive` while one is active, raise `InvalidOperationException` (the channel is single-consumer; this names the obligation). The enumerator is affine like every single-pass source (language spec [§29.11](Bezel_Language_Spec.md#sec-multiple-enumeration)). Zero new checking rules — a restatement of `Receive` over the [§4.13](#sec-owning-enumeration-interfaces) async-iteration interfaces (normative statement: language spec [§30.15](Bezel_Language_Spec.md#sec-channels)).
- **Receive abandonment (v0.179).** Disposing a pending `Receive` task deregisters the waiter **without consuming an item** (normative statement: language spec §30.15); an item being handed off at the instant of disposal is returned to the channel's head. Applies to both the `OwningTask<T>` and `Task<T>` forms of `Receive`.

```bezel
public class ChannelClosedException: Exception
end ChannelClosedException;

public class ChannelFullException: Exception        // v0.174: sync Send on a full bounded channel
end ChannelFullException;
```

**Bounded channels (v0.174, #809).** `new Channel<T>(capacity)` creates a channel that buffers at most `capacity` items. `SendAsync` **awaits capacity**: the returned task completes when the item has been accepted (immediately when space is free, and always immediately on an unbounded channel). The synchronous `Send` keeps its never-blocks contract on every channel — on a *full* bounded channel it raises **`ChannelFullException`** (a defined fault, §30.3's no-blocking discipline: a producer that wants to wait says so with `await`). If the channel closes while a `SendAsync` is waiting, the pending item is **disposed by the channel** (exactly like buffered undelivered items) and the task faults with `ChannelClosedException`. Ordering and exactly-once delivery are unchanged. A faulting send — `ChannelFullException`, or `ChannelClosedException` from either send form — disposes the class-typed item before the exception surfaces; a channel never retains an item it did not accept (v0.184; language spec [§30.15](Bezel_Language_Spec.md#sec-channels)).

Deferred, each with its reason recorded in the v0.174 rationale entry: **capacity-zero rendezvous** (couples producer/consumer scheduling; addable later as `capacity: 0` without breaking), **`TrySend`** (the failure path would need to hand the owned item *back* — an ownership-protocol design of its own), **cancellation of a pending send** (same give-back protocol, plus it rides #1413's foundations), **cancellable and multi-source receive** (`Receive(token)`, select/`ReceiveAny` — the v0.179 dispose-deregistration rule defines *abandonment*; a first-class primitive needs the same hand-the-owned-item-back protocol and is deferred with the send-side family), and **multi-consumer receiver cloning** (drags `Shared<T>`'s §28.13 domain-locality questions; worker pools compose today from N single-consumer channels).

---

<a id="sec-taskscope"></a>
### 6.10 `TaskScope` (v0.175, #808)

The structured-concurrency nursery: a lexically-scoped owner of concurrently-running tasks. At scope exit every child has completed or been cancelled-and-joined; a child fault cancels its siblings; the first fault re-raises at the (mandatory) `await Join()`.

```bezel
public class TaskScope: IAsyncDisposable
  public constructor();                                    // fresh internal CancellationTokenSource
  public readonly property Token: CancellationToken;       // the scope's token — pass to children
  public method CancelAfter(delay: TimeSpan);              // deadline for the whole scope
  public method Cancel();                                  // cancel all children now

  public method Spawn(scoped body: () -> Task);            // start a child (§28.14 storage-bounded);
                                                           //   InvalidOperationException once closed
  public async method Join(): Task;                        // await all children; re-raise the FIRST
                                                           //   fault (first-fault-wins, no aggregation)
  // await using only (see below); DisposeAsync drains and never raises
end TaskScope;
```

- **Consumption is `await using` only** — a plain `using var`, or dropping the scope undisposed, is a compile error naming this rule: a synchronous destructor cannot await the drain, and live children hold scope-bounded borrows (§28.14), so no synchronous escape is sound.
- **Not live across a `yield` (v0.184; spec [§30.7](Bezel_Language_Spec.md#sec-asynchronous-programming-async-iterators)):** inside an async iterator, a `TaskScope` may not be held across a `yield return` — consumer abandonment at that suspension would take [§28.12](Bezel_Language_Spec.md#sec-asynchronous-disposal)'s synchronous-fallback path, and `TaskScope` has no destructor by design (no synchronous escape is sound), so children would be neither cancelled nor joined and the quiescence guarantee above would be silently voided. Fan out inside a non-yielding `async` helper called between yields instead. (A degraded synchronous destructor — cancel plus detached drain, the #1422 shape — was considered and is recorded as the fallback only if this static rule proves too restrictive.)
- **`await Join()` is mandatory on the normal path** (compile error where provable, `InvalidOperationException` from `DisposeAsync` otherwise): the join-and-re-raise happens at a source-visible `await`, never at an unmarked `end`. `DisposeAsync` (the exception/early-exit edge) cancels the scope's token, drains remaining children, and **always swallows** their faults — `Join` is the sole re-raise site. The drain may suspend unboundedly (the §28.12 close-edge note); bound it with `CancelAfter`.
- **Fault policy**: first fault wins and cancels siblings via the scope's token (cooperative, §30.8); later faults are swallowed (the recorded §13.5 no-aggregation posture).
- **`Spawn` after close** (after `Join` returned or disposal began) raises `InvalidOperationException` — deterministic, never a silent extension of the drain.
- **Quiescence (normative, §28.14)**: `DisposeAsync` completes only after every child task has completed and been reclaimed; no `scoped` callback runs after it returns.
- **Self-capture (v0.180)**: a child body may capture the scope itself and values borrow-derived from it — `scope.Token` inline, or the bound intermediate `var tok := scope.Token;` — admitted by §28.14's self-capture rule. This is how children observe cooperative cancellation and how re-registration (`scope.Spawn` from inside a child) is expressed.
- Intra-domain: `TaskScope` is not sendable; cross-domain fan-out remains channels + `Domain.Spawn`.

<a id="sec-locked-and-rwlocked"></a>
### 6.11 `Locked<T>` and `RwLocked<T>` (v0.186)

Data-owning locks (language spec [§30.12 "Data-owning locks"](Bezel_Language_Spec.md#sec-data-owning-locks) — the
normative statement of the interior admission, guard borrow-sourcing, the guard-liveness checks, blocking discipline,
the read-guard write-restriction, the lock-disposal posture, visibility, and set memberships; design rationale
[§12.8](Bezel_Design_Rationale.md#sec-locked-rwlocked)). The lock **owns its
interior**: construction consumes an owned `T`; access is only through a scoped guard whose `Value` is a borrow
sourced from the guard, so the `using var` scope *is* the critical section and the borrow-escape rules enforce the
lock discipline — including against announced kills (a guard binding cannot be explicitly `Dispose()`d or discarded
while a borrow from its `Value` is live, and a body-local lock binding cannot be killed while one of its guards is
live; language spec §30.12/§28.3); the destructor disposes the interior. `T` must be a **sendable class type**
(checked at instantiation): owning collections of sendable elements (`OwningDictionary`, `OwningSet`, …) and classes
with wholly-owned state qualify; borrowing collections of class elements do not.

```bezel
public class Locked<T> where T: class          // compiler-known; T must additionally be a
                                               //   sendable class type (checked at instantiation)
  public constructor(owned value: T);          // the lock owns the interior

  public method Lock(): owned LockGuard<T>;                          // blocks; §30.12 discipline
  public method TryLock(timeout: TimeSpan): owned LockGuard<T>?;     // null on timeout (Try shape (b))
  public async method LockAsync(token: CancellationToken := CancellationToken.None): OwningTask<LockGuard<T>>;
  public async method TryLockAsync(timeout: TimeSpan, token: CancellationToken := CancellationToken.None): OwningTask<LockGuard<T>?>;

  destructor;                                  // disposes the interior
end Locked;

public class LockGuard<T>
  // No public constructor; obtained from Locked<T>.
  public readonly property Value: T;           // borrow of the interior; source: this guard
  destructor;                                  // releases the lock
end LockGuard;

public class RwLocked<T> where T: class        // same interior admission as Locked<T>
  public constructor(owned value: T);

  public method Read(): owned ReadGuard<T>;                          // shared; blocks
  public method TryRead(timeout: TimeSpan): owned ReadGuard<T>?;
  public async method ReadAsync(token: CancellationToken := CancellationToken.None): OwningTask<ReadGuard<T>>;
  public async method TryReadAsync(timeout: TimeSpan, token: CancellationToken := CancellationToken.None): OwningTask<ReadGuard<T>?>;

  public method Write(): owned WriteGuard<T>;                        // exclusive; blocks
  public method TryWrite(timeout: TimeSpan): owned WriteGuard<T>?;
  public async method WriteAsync(token: CancellationToken := CancellationToken.None): OwningTask<WriteGuard<T>>;
  public async method TryWriteAsync(timeout: TimeSpan, token: CancellationToken := CancellationToken.None): OwningTask<WriteGuard<T>?>;

  destructor;                                  // disposes the interior
end RwLocked;

public class ReadGuard<T>
  public readonly property Value: T;           // borrow; WRITE-RESTRICTED (language spec §30.12)
  destructor;                                  // releases the shared hold
end ReadGuard;

public class WriteGuard<T>
  public readonly property Value: T;           // borrow; full access
  destructor;                                  // releases the exclusive hold
end WriteGuard;
```

The canonical process-global registry — the `static owned readonly` idiom (rule-4 warning suppressed by
[§22.12](Bezel_Language_Spec.md#sec-static-members) rule 4's v0.186 suppression list; `owned` enrolls program-exit
disposal and clears [§22.13](Bezel_Language_Spec.md#sec-static-initialization-order)'s disposal warning; a **mutable**
static of a lock type remains rejected — put the mutability behind the guard):

```bezel
public static class Sessions
  private static owned readonly _map: Locked<OwningDictionary<string, Session>>
      := new Locked<OwningDictionary<string, Session>>(new OwningDictionary<string, Session>());

  public static async method Touch(id: string): Task
    using var g := await _map.LockAsync();
    g.Value[id] := Session.Renewed(g.Value[id]);   // owning indexer-set disposes the displaced session
  end Touch;                                       // guard disposed ⇒ lock released
end Sessions;
```

Semantics and obligations (normative statement: language spec §30.12):

- **Acquisition mirrors `Mutex` ([§6.2](#sec-mutex-and-lock))**: blocking forms are rejected lexically inside `async`
  bodies and hit the runtime `InvalidOperationException` guard where a domain continuation would actually park; the
  `Async` forms are Pattern B (`using var g := await x.LockAsync();`). `TryLock(timeout)`/`TryRead`/`TryWrite` return
  `null` on timeout (Try shape (b), [§1](#sec-conventions)). **Non-reentrant**: a holder re-acquiring waits on itself
  — including `Write()` while holding any guard of the same `RwLocked<T>`; and a second `Read()` while already
  holding a read guard may block behind a queued writer (fairness is unspecified) — never nest acquisitions of one
  lock.
- **Read guards are write-restricted.** Any number of `ReadGuard<T>`s may be live at once, across domains; a
  `WriteGuard<T>` excludes all guards. In the body holding the guard, the compiler rejects all stores, `move`, and
  `ref`/`out` passing through a `ReadGuard`-sourced borrow; **calling a mutating method through a read guard, or
  passing the read borrow into any callee as a plain argument that then stores through it, is the recorded programmer
  obligation** (language spec §30.12) — reader purity beyond the checked stores is yours; violations are cross-domain
  data races no mode detects. When in doubt, take `Write()`.
- **Disposing the lock disposes the interior.** Never dispose a lock — or drop a last `Shared` handle — while any
  guard is outstanding: checked disposal mode detects a lock destroyed with live guards and faults deterministically;
  unchecked it is undefined behavior (language spec §30.12). Nest `using var` guards inside the scope that owns the
  lock.
- **Cross-domain use**: `Shared<Locked<T>>`/`Shared<RwLocked<T>>` are sendable ([§28.13](Bezel_Language_Spec.md#sec-shared-ownership));
  clone a handle per domain and `Get().LockAsync()` — the `Shared<Mutex>` idiom with the state attached. Both lock
  types are themselves sendable; all three guard types are **not** (domain-bound, like `Lock`).
- **Fairness and ordering** between waiting readers and writers are unspecified; no upgrade/downgrade between guard
  kinds (deferred — evidence first). No lock-ordering machinery: deadlock between two locks is out of scope, as
  everywhere; use the actor idiom when ordering gets complicated.
- **Guard across `await`**: legal but discouraged; an implementation **should** warn (on by default) when a guard is
  live across a suspension point — narrow the `using var` scope instead.
- **Visibility**: guard release → next acquisition is a [§30.13](Bezel_Language_Spec.md#sec-cross-thread-visibility)
  edge-1 happens-before edge (write release orders before subsequent read/write acquisitions; read releases order
  before the next write acquisition).

<a id="sec-parallel"></a>
### 6.12 `Parallel` (v0.186)

Compiler-known data-parallel combinators over a lazy, process-wide team of worker domains (normative checking and
runtime semantics: language spec [§30.14 "Data-parallel combinators"](Bezel_Language_Spec.md#sec-data-parallel-combinators)).
The `body`/`selector`/`combiner` parameters are **spawn positions**: capture-free (a non-capturing lambda or a static
method reference), with element and result sendability compiler-checked at the call site — the same admission as
`Domain.Spawn` entries ([§6.8](#sec-domain)). This surface is not writable in user Bezel (function values are never
sendable); being compiler-known is what lets its call sites reuse the spawn classifier.

```bezel
public static class Parallel
  // The team's width: the value of Environment.ProcessorCount (§2.4) at team
  // creation, fixed per process. `degree := 0` on the methods below means this value;
  // 1 <= degree caps the workers used by that call; degree < 0 raises ArgumentException.
  public static readonly property DefaultDegree: int;

  // ---- value elements (sendable value types and string): chunks cross as copies ----
  public static method Map<A, R>(items: Array<A>, body: (A) -> R,
      degree: int := 0): owned Task<Array<R>>
      where A: notnull, R: notnull;              // Pattern A result: the task owns the
                                                 //   array, awaiters borrow (the §6.7 WhenAll<T> shape)

  public static method Reduce<A, R>(items: Array<A>, seed: R,
      selector: (A) -> R,                        // spawn position
      combiner: (R, R) -> R,                     // spawn position; MUST be associative
      degree: int := 0): owned Task<R>
      where A: notnull, R: notnull;

  public static method ForEach<A>(items: Array<A>, body: (A) -> (),
      degree: int := 0): owned Task
      where A: notnull;

  // ---- class elements: ownership partitions in, results partition back ----
  public static method Map<A, R>(owned items: OwningArray<A>, body: (owned A) -> owned R,
      degree: int := 0): owned OwningTask<OwningArray<R>>
      where A: class, R: class;                  // A, R sendable (compiler-checked)

  public static method ForEach<A>(owned items: OwningArray<A>, body: (owned A) -> (),
      degree: int := 0): owned Task
      where A: class;

  // ---- frozen elements (§4.19): zero-copy — one Shared handle cloned per worker ----
  public static method Map<A, R>(items: Shared<FrozenArray<A>>, body: (A) -> R,
      degree: int := 0): owned Task<Array<R>>
      where A: notnull, R: notnull;
  public static method Reduce<A, R>(items: Shared<FrozenArray<A>>, seed: R,
      selector: (A) -> R, combiner: (R, R) -> R,
      degree: int := 0): owned Task<R>
      where A: notnull, R: notnull;
  public static method ForEach<A>(items: Shared<FrozenArray<A>>, body: (A) -> (),
      degree: int := 0): owned Task
      where A: notnull;
end Parallel;
```

As with `Domain.Spawn`, the `where` clauses are the declared constraints and **sendability is the stronger
compiler-checked requirement**, diagnosed at the call site naming the failing rule. (Return types follow the
`Domain.Spawn` owned-task convention — deliberately, and noted against §6.7's unmarked `WhenAll` rows.)

- **Snapshot-at-call** (normative, spec §30.14): borrowed `Array<A>` inputs are chunk-copied synchronously by the
  call itself; owned inputs are consumed and partitioned; `Shared<FrozenArray<A>>` inputs clone one handle per
  participating worker (the borrow of the caller's handle is call-scoped; the caller keeps its handle) and copy no
  element.
- **Results in input order**; first fault wins, surfaced at the `await` as `DomainFaultedException` (implementations
  are encouraged to name the failing element index in `Message`); later faults swallowed; moved-in unprocessed items
  disposed by the runtime.
- **`Reduce` seed semantics (v0.186, stated plainly).** Each worker folds its chunk starting from `seed`; per-worker
  partials combine in an unspecified tree. A `seed` that is not `combiner`'s identity is therefore incorporated once
  per participating worker. The serial-fold correspondence holds exactly when the combiner is associative and the
  seed is its identity; for floating-point, reassociation may change the last bits even then. `Reduce` over an empty
  `items` returns `seed`. Associativity is a documented requirement, not a checked one.
- **Empty inputs** complete immediately (`Map` with an empty array; `ForEach` likewise).

**Deliberately not in v1** (recorded deferrals, not oversights): *async bodies* (`(A) -> Task<R>` — I/O fan-out has
`TaskScope` intra-domain and channels cross-domain; mixing the models needs its own design); *class-element `Reduce`*
(an owned-threading combiner protocol; use class-element `Map` plus a serial merge); *a `CancellationToken`
parameter* (tokens are not sendable — cross-domain cancellation stays a channel message); *user-declared spawn
positions* (would expose the classifier; evidence first). The in-place fork-join extension (`Parallel.Join`,
disjoint span lending) is designed-and-recorded, not shipped — rationale
[§12.9](Bezel_Design_Rationale.md#sec-data-parallelism).

Channels remain the right tool for heterogeneous, streaming, rate-mismatched work ([§6.9](#sec-channel),
[§30.15](Bezel_Language_Spec.md#sec-channels)); these combinators replace the *abuse* of channels for uniform
data-parallel maps, not channels.

<a id="sec-system-io"></a>
## 7. `System.IO`

<a id="sec-stream"></a>
### 7.1 `Stream`

Abstract base for byte-oriented streams.

```bezel
public abstract class Stream
  public abstract readonly property CanRead: bool;
  public abstract readonly property CanWrite: bool;
  public abstract readonly property CanSeek: bool;

  public abstract method Read(buffer: Array<byte>, offset: int, count: int): int;
  public abstract method Write(buffer: Array<byte>, offset: int, count: int);
  public method Read(buffer: Span<byte>): int;             // v0.165: span carries its own range
  public method Write(buffer: ReadOnlySpan<byte>);          //   (§4.14 convention: slice first)
  public abstract method Flush();

  public abstract method Seek(offset: long, origin: SeekOrigin): long;
  public abstract readonly property Length: long;
  public abstract property Position: long;

  public async virtual method ReadAsync(buffer: Array<byte>, offset: int, count: int,
                                         token: CancellationToken := CancellationToken.None): Task<int>;
  public async virtual method WriteAsync(buffer: Array<byte>, offset: int, count: int,
                                          token: CancellationToken := CancellationToken.None): Task;

  // Convenience methods
  public method ReadAll(): owned Array<byte>;            // reads from current position to end
  public async method ReadAllAsync(token: CancellationToken := CancellationToken.None): OwningTask<Array<byte>>;
end Stream;
```

**Windowed-form contract (v0.165, #1375):** in `Read`/`Write(buffer, offset, count)` the window `[offset, offset+count)` ranges over the buffer's **`Count`** (like every other bounds rule in this library); out-of-range raises `IndexOutOfRangeException`. `Read` writes into *existing* elements and never grows the array — the canonical read loop allocates its buffer with the sized constructor (`new Array<byte>(64 * 1024, 0)`, §4.4) and passes `count := buffer.Count`. The **synchronous** span overloads carry their range in the value and are the preferred forms for synchronous I/O; the asynchronous path uses the `Array<byte>` windowed form exclusively (the async span overloads were removed — see the changelog below, #1438).

**Override points (v0.184).** `Read`/`Write(buffer, offset, count)`, `Flush`, `Seek`, `Length`, `Position`, and the `CanRead`/`CanWrite`/`CanSeek` properties are `abstract` — every concrete stream supplies them. `ReadAsync`/`WriteAsync` are **`virtual`** with a **base default that delegates to the synchronous `Read`/`Write`**; the default **blocks the calling domain for the I/O's duration** — legal only where blocking is legal, and a [§30.12](Bezel_Language_Spec.md#sec-thread-synchronization) pool-starvation hazard (a blocked continuation pins one pool worker) with **no compiler/runtime backstop for I/O**. A stream performing real I/O (`FileStream`, [§7.2](#sec-filestream)) **overrides** them with genuinely asynchronous implementations; a memory-backed stream whose synchronous ops never block correctly inherits the default. The span overloads `Read(Span<byte>)`/`Write(ReadOnlySpan<byte>)` and the `ReadAll`/`ReadAllAsync` conveniences are **fixed base implementations** over the windowed/async forms — not override points; a subclass that overrides the windowed/async forms gets the correct behavior through them.

`Stream` is abstract and declares no `Dispose` behavior of its own; each concrete subclass defines its disposal (e.g. `FileStream` flushes and closes, [§7.2](#sec-filestream)). The `destructor;`-absence convention of [§1](#sec-conventions) does **not** imply that a `Stream` subclass's disposal is a no-op.

```bezel
public enum SeekOrigin: byte
  Begin, Current, End;
end SeekOrigin;
```

> **Async span overloads removed (v0.179, #1438).** `Stream.ReadAsync(buffer: Span<byte>, token)` and `WriteAsync(buffer: ReadOnlySpan<byte>, token)` are removed. A span is a borrow-bearing record (§4.14, spec §9.7); an `async` method captures its parameters into a heap-allocated state-machine frame that outlives the synchronous call and lives until the task settles, so a span parameter would leave a caller-owned buffer borrowed inside a frame the caller is under no obligation to keep alive to settlement — a dangling borrow with no `DisposedAccessException` backstop on the raw buffer window (spec §30.1, the borrow-bearing-parameter prohibition). Asynchronous byte I/O therefore uses the heap-owned windowed form exclusively — `ReadAsync(buffer: Array<byte>, offset, count, token)` / `WriteAsync(buffer: Array<byte>, offset, count, token)` (already declared) — or the owned-result `ReadAllAsync(): OwningTask<Array<byte>>`. The **synchronous** span overloads `Read(buffer: Span<byte>)` / `Write(buffer: ReadOnlySpan<byte>)` are unaffected: a span across a synchronous call frame is sanctioned by spec §8.6. Callers holding a `Span<byte>`/`ReadOnlySpan<byte>` window over an `Array<byte>` pass the backing array with the window's `Start`/`Length` as `offset`/`count`; windows over an `OwningBuffer<byte>` copy into an `Array<byte>` for the async call, or use the synchronous overload where a synchronous write suffices.

<a id="sec-filestream"></a>
### 7.2 `FileStream`

```bezel
public class FileStream: Stream
  public constructor(path: string, mode: FileMode);
  public constructor(path: string, mode: FileMode, access: FileAccess);
  // Stream operations plus file-specific:
  public readonly property Path: string;
  destructor;    // flushes buffered writes and closes the OS file handle
end FileStream;

public enum FileMode: byte
  CreateNew, Create, Open, OpenOrCreate, Truncate, Append;
end FileMode;

public enum FileAccess: byte
  Read, Write, ReadWrite;
end FileAccess;
```

**Disposal (flush-on-dispose).** Disposing a `FileStream` — at `using var` scope exit or an explicit `Dispose()` — flushes any buffered writes and releases the OS file handle. A `FileStream` opened for writing must be disposed (or explicitly `Flush()`ed) for buffered data to reach the file; the `using var` idiom guarantees this on every exit edge, including exceptions. `FileStream` overrides `ReadAsync`/`WriteAsync` ([§7.1](#sec-stream)) with genuinely asynchronous OS I/O — not the blocking base default.

<a id="sec-file"></a>
### 7.3 `File`

Static convenience methods.

```bezel
public static class File
  public static method ReadAllText(path: string): string;
  public static method WriteAllText(path: string, contents: string);
  public static method ReadAllBytes(path: string): owned Array<byte>;
  public static method WriteAllBytes(path: string, contents: Array<byte>);
  public static method WriteAllBytes(path: string, contents: ReadOnlySpan<byte>);   // v0.165 (#1358): serializer-output path, zero-copy
  public static method ReadAllLines(path: string): owned Array<string>;
  public static method WriteAllLines(path: string, lines: IEnumerable<string>);

  public static async method ReadAllTextAsync(path: string,
                                                token: CancellationToken := CancellationToken.None): Task<string>;
  public static async method WriteAllTextAsync(path: string, contents: string,
                                                 token: CancellationToken := CancellationToken.None): Task;

  public static method Exists(path: string): bool;
  public static method Delete(path: string);
  public static method Copy(source: string, destination: string, overwrite: bool);
  public static method Move(source: string, destination: string);
end File;
```

<a id="sec-textreader-textwriter-directory-path"></a>
### 7.4 `TextReader`, `TextWriter`, `Directory`, `Path`

```bezel
public abstract class TextReader
  public abstract method Read(): int;             // next char as int, or -1 at EOF
  public abstract method ReadLine(): string?;      // null at EOF
  public method ReadToEnd(): string;
  destructor;       // releases the underlying resource
end TextReader;

public abstract class TextWriter
  public abstract method Write(value: string);
  public abstract method Write(value: char);
  public method WriteLine();
  public method WriteLine(value: string);
  public abstract method Flush();
  destructor;       // flushes and releases the underlying resource
end TextWriter;
```

`Directory` and `Path` (v0.165, #1356 — the previous elision is replaced by the surface the compiler already implements):

```bezel
public static class Directory
  public static method Exists(path: string): bool;
  public static method CreateDirectory(path: string);
  public static method Delete(path: string);                       // IOException if non-empty
  public static method Delete(path: string, recursive: bool);
  public static method GetFiles(path: string): owned Array<string>;
  public static method GetDirectories(path: string): owned Array<string>;
end Directory;

public static class Path
  public static method Combine(a: string, b: string): string;
  public static method GetFileName(path: string): string;
  public static method GetExtension(path: string): string;         // "" when none
  public static method GetFileNameWithoutExtension(path: string): string;
  public static method GetDirectoryName(path: string): string;
  public static method GetFullPath(path: string): string;
end Path;
```

Failures raise the §7.5 IO exception types (`IOException`, `FileNotFoundException`).

<a id="sec-io-exception-types"></a>
### 7.5 IO exception types

`IOException`, `FileNotFoundException`, `DirectoryNotFoundException`, `EndOfStreamException`, `PathTooLongException`. All derive from `IOException`, which derives from `Exception`. None are auto-imported; users `import System.IO;` to get them.

---

<a id="sec-system-diagnostics"></a>
## 8. `System.Diagnostics`

<a id="sec-stopwatch"></a>
### 8.1 `Stopwatch`

```bezel
public class Stopwatch
  public constructor();
  public static method StartNew(): owned Stopwatch;

  public readonly property IsRunning: bool;
  public readonly property Elapsed: TimeSpan;
  public readonly property ElapsedMilliseconds: long;

  public method Start();
  public method Stop();
  public method Restart();
  public method Reset();
end Stopwatch;
```

<a id="sec-debug-and-trace"></a>
<a id="sec-debug"></a>
### 8.2 `Debug`

Conditional logging facilities.

```bezel
public static class Debug
  public static method Assert(condition: bool);
  public static method Assert(condition: bool, message: string);
  public static method WriteLine(message: string);
end Debug;
```

Calls to `Debug.*` may be elided in release builds, depending on the compiler's `--debug` flag. Side effects in `Debug.Assert` arguments are unspecified.

---

<a id="sec-primitive-type-members"></a>
## 9. Primitive type members

Each primitive keyword aliases a standard library type ([§3.4](Bezel_Language_Spec.md#sec-keyword-aliases-for-built-in-types) of the language spec). The types expose the following members. Throughout this section, declarations use the System-namespace PascalCase names (`Int32`, `String`, `Double`, `Boolean`, etc.); the lowercase keywords (`int`, `string`, `double`, `bool`, etc.) are reserved tokens and cannot themselves be used as type-declaration identifiers.

The numeric and boolean primitives are declared as records (value types, language spec [§3.1](Bezel_Language_Spec.md#sec-type-kinds)) annotated as compiler-intrinsic — their runtime representation is a fixed-width machine integer or floating-point value, not the field layout the declaration suggests. `String` is the exception: it is a class, since `string` is a reference type with content equality (language spec [§4.6](Bezel_Language_Spec.md#sec-string)).

<a id="sec-integer-types"></a>
### 9.1 Integer types

`SByte`, `Byte`, `Int16`, `UInt16`, `Int32`, `UInt32`, `Int64`, `UInt64`. The example below shows `Int32`; the others have analogous shapes with their respective `MinValue`/`MaxValue`.

Each integer type is declared as a record (a value type, per language spec [§3.1](Bezel_Language_Spec.md#sec-type-kinds)) with the `compiler-intrinsic` annotation indicating that the underlying representation is a fixed-width machine integer rather than the field layout suggested by the declaration. User code cannot redeclare or extend these types.

```bezel
/// Compiler-intrinsic; aliased by the keyword `int`.
public record Int32: IComparable<int>, IEquatable<int>
  public static const MinValue: int := -2_147_483_648;
  public static const MaxValue: int := 2_147_483_647;

  public static method Parse(s: string): int;
  public static method TryParse(s: string, out result: int): bool;
  public static method Parse(s: string, radix: int): int;        // 2, 8, 10, 16

  public method ToString(): string;
  public method ToString(format: string): string;
  public method ToString(radix: int): string;

  public method CompareTo(other: int): int;
  // Equals(other: int): bool — auto-generated from IEquatable<int>
  //   per language spec §9.2; not shown explicitly.
end Int32;
```

Records implement `IEquatable<T>` implicitly through the auto-generated equality (language spec [§9.2](Bezel_Language_Spec.md#sec-body-records)): `Equals(other: int): bool` exists on `Int32` and is invoked when calling `someInt.Equals(otherInt)` directly or when an `IEquatable<int>`-constrained generic uses `Equals` through the constraint. The same convention applies to every primitive declaration in [§9.1](#sec-integer-types)–[§9.5](#sec-char).

<a id="sec-double-and-float"></a>
### 9.2 `Double` and `Float`

```bezel
/// Compiler-intrinsic; aliased by the keyword `double`.
public record Double: IComparable<double>, IEquatable<double>
  public static const MinValue: double;
  public static const MaxValue: double;
  public static const Epsilon: double;
  public static const PositiveInfinity: double;
  public static const NegativeInfinity: double;
  public static const NaN: double;

  public static method IsNaN(value: double): bool;
  public static method IsInfinity(value: double): bool;
  public static method IsPositiveInfinity(value: double): bool;
  public static method IsNegativeInfinity(value: double): bool;
  public static method IsFinite(value: double): bool;

  public static method Parse(s: string): double;
  public static method TryParse(s: string, out result: double): bool;

  // Equality variants
  public static method IeeeEquals(a: double, b: double): bool;       // strict IEEE
  public static method BitwiseEquals(a: double, b: double): bool;    // bit-pattern equal

  public method ToString(): string;
  public method ToString(format: string): string;
  public method CompareTo(other: double): int;
end Double;
```

`IeeeEquals` returns `false` for any pair involving `NaN`, including `NaN`-vs-`NaN`. `BitwiseEquals` returns `true` only when the two `double` values have identical bit patterns; it distinguishes `+0.0` from `-0.0` and distinguishes signaling from quiet NaN payloads. The default `=` operator is the reflexive form described in language spec [§4.3.1](Bezel_Language_Spec.md#sec-equality-semantics-for-floating-point-types).

`Float` has the analogous shape, with IEEE 754 single-precision constants and `float`-typed parameters:

```bezel
/// Compiler-intrinsic; aliased by the keyword `float`.
public record Float: IComparable<float>, IEquatable<float>
  public static const MinValue: float;
  public static const MaxValue: float;
  public static const Epsilon: float;
  public static const PositiveInfinity: float;
  public static const NegativeInfinity: float;
  public static const NaN: float;

  public static method IsNaN(value: float): bool;
  public static method IsInfinity(value: float): bool;
  public static method IsPositiveInfinity(value: float): bool;
  public static method IsNegativeInfinity(value: float): bool;
  public static method IsFinite(value: float): bool;

  public static method Parse(s: string): float;
  public static method TryParse(s: string, out result: float): bool;

  // Equality variants
  public static method IeeeEquals(a: float, b: float): bool;       // strict IEEE
  public static method BitwiseEquals(a: float, b: float): bool;    // bit-pattern equal

  public method ToString(): string;
  public method ToString(format: string): string;
  public method CompareTo(other: float): int;
end Float;
```

`Float`'s `MinValue`, `MaxValue`, `Epsilon`, `PositiveInfinity`, `NegativeInfinity`, and `NaN` are the IEEE 754 binary32 standard values. The same `IeeeEquals`/`BitwiseEquals` distinction described for `Double` applies to `Float` with the appropriate bit-pattern width.

<a id="sec-decimal"></a>
### 9.3 `Decimal`

```bezel
/// Compiler-intrinsic; aliased by the keyword `decimal`.
public record Decimal: IComparable<decimal>, IEquatable<decimal>
  public static const MinValue: decimal;
  public static const MaxValue: decimal;
  public static const Zero: decimal;
  public static const One: decimal;

  public static method Parse(s: string): decimal;
  public static method TryParse(s: string, out result: decimal): bool;

  public method ToString(): string;
  public method ToString(format: string): string;
  public method CompareTo(other: decimal): int;
  public method Round(digits: int): decimal;
end Decimal;
```

`Parse` accepts an optional leading sign (`+`/`-`) followed by decimal digits with at most one `.`; **surrounding whitespace is not accepted** and raises `FormatException` (specified in v0.137 — the strict-fault model of `Encoding` ([§3.2](#sec-encoding)) applies: silent tolerance hides data problems; a caller that means to accept padded input trims explicitly). A value outside the representable range raises `OverflowException`. `TryParse` returns `false` in every case where `Parse` would raise.

<a id="sec-boolean"></a>
### 9.4 `Boolean`

```bezel
/// Compiler-intrinsic; aliased by the keyword `bool`.
public record Boolean: IEquatable<bool>
  public static method Parse(s: string): bool;            // "true"/"false", case-insensitive
  public static method TryParse(s: string, out result: bool): bool;

  public method ToString(): string;                       // "True" or "False"
end Boolean;
```

<a id="sec-char"></a>
### 9.5 `Char`

```bezel
/// Compiler-intrinsic; aliased by the keyword `char`.
public record Char: IComparable<char>, IEquatable<char>
  public static const MinValue: char;             // U+0000
  public static const MaxValue: char;             // U+10FFFF (excluding surrogates)

  public static method IsDigit(c: char): bool;
  public static method IsLetter(c: char): bool;
  public static method IsLetterOrDigit(c: char): bool;
  public static method IsWhiteSpace(c: char): bool;
  public static method IsUpper(c: char): bool;
  public static method IsLower(c: char): bool;
  public static method IsControl(c: char): bool;
  public static method IsPunctuation(c: char): bool;

  public static method ToUpper(c: char): char;
  public static method ToLower(c: char): char;

  public method ToString(): string;
  public method CompareTo(other: char): int;
end Char;
```

<a id="sec-string"></a>
### 9.6 `String`

The most extensive primitive type. `String` is treated as both a primitive (with the `string` keyword) and a class. It implements `IEnumerable<char>`, so strings may be iterated with `for` (language spec [§4.6](Bezel_Language_Spec.md#sec-string)).

```bezel
public class String: IEnumerable<char>, IComparable<string>, IEquatable<string>     // alias: string
  public readonly property Length: int;            // number of code points (O(1))
  public readonly property IsAscii: bool;          // true iff every code point < 128 (O(1))
  public readonly property [i: int]: char;                  // O(1)

  // Construction
  public static method From(value: char): string;
  public static method From(buffer: Array<char>, start: int, count: int): string;
  public static method From(value: CString): string;                  // FFI-boundary, owned-instance source
  public static method From(value: WString): string;                  // FFI-boundary, owned-instance source
  public static method FromCStringPointer(p: IntPtr): string;         // FFI-boundary, raw-pointer source
  public static method FromWStringPointer(p: IntPtr): string;         // FFI-boundary, raw-pointer source
  public static method Repeat(c: char, count: int): string;
  public static method Join(separator: string, items: IEnumerable<string>): string;

  // Inspection
  public static method IsNullOrEmpty(value: string?): bool;
  public static method IsNullOrWhiteSpace(value: string?): bool;
  public method Contains(value: string): bool;
  public method Contains(value: char): bool;
  public method StartsWith(value: string): bool;
  public method EndsWith(value: string): bool;
  public method IndexOf(value: char): int;
  public method IndexOf(value: string): int;
  public method LastIndexOf(value: char): int;
  public method LastIndexOf(value: string): int;

  // Substring
  public method Substring(startIndex: int): string;
  public method Substring(startIndex: int, length: int): string;
  public readonly property [r: Range<int>]: string;       // slicing form (§4.6 of the language spec)

  // Span views (v0.104)
  public method AsSpan(): ReadOnlyStringSpan;             // full string as a span
  public method AsSpan(start: int): ReadOnlyStringSpan;   // from offset to end
  public method AsSpan(start: int, length: int): ReadOnlyStringSpan;
  public method AsSpan(r: Range<int>): ReadOnlyStringSpan; // parallels [r: Range<int>]: string

  // Modification (returns a new string; original unchanged)
  public method Trim(): string;
  public method Trim(chars: IEnumerable<char>): string;
  public method TrimStart(): string;
  public method TrimEnd(): string;
  public method ToUpper(): string;
  public method ToLower(): string;
  public method Replace(oldChar: char, newChar: char): string;
  public method Replace(oldValue: string, newValue: string): string;
  public method PadLeft(totalWidth: int): string;
  public method PadLeft(totalWidth: int, paddingChar: char): string;
  public method PadRight(totalWidth: int): string;
  public method PadRight(totalWidth: int, paddingChar: char): string;
  public method Insert(startIndex: int, value: string): string;
  public method Remove(startIndex: int): string;
  public method Remove(startIndex: int, count: int): string;

  // Splitting
  public method Split(separator: char): owned Array<string>;
  public method Split(separator: string): owned Array<string>;
  public method Split(separators: Array<char>): owned Array<string>;

  // Comparison and equality.
  // All comparison and equality methods in this version are ordinal (code-point-
  // based), including the `ignoreCase` overloads — case-insensitive comparison
  // compares each code point's simple Unicode case-folded form, with no locale
  // or culture-specific tailoring. Culture-aware comparison will be provided by
  // System.Globalization in a future revision.
  public method CompareTo(other: string): int;            // ordinal
  public static method Compare(a: string, b: string): int;
  public static method Compare(a: string, b: string, ignoreCase: bool): int;
  public method Equals(other: string?): bool;             // satisfies IEquatable<string>'s
                                                          //   Equals(string) under §11.2's
                                                          //   parameter-nullability widening
  public method Equals(other: string?, ignoreCase: bool): bool;

  // Object overrides
  public override method Equals(other: Object?): bool;    // content-based, not reference-based
  public override method GetHashCode(): int;              // content-based, agrees with Equals
  public override method ToString(): string;              // returns this

  // Conversion
  public method ToCharArray(): owned Array<char>;

  // Iteration
  public method GetEnumerator(): owned IEnumerator<char>;

  // Operators
  public static method operator +(left: string, right: string): string;
end String;
```

`Replace`, `Trim`, `ToUpper`, etc. return new `string` values; `string` is immutable. A `StringBuilder` ([§3.1](#sec-stringbuilder)) is appropriate for accumulation.

`Length` and `s[i]` are O(1) for every string (language spec [§4.6](Bezel_Language_Spec.md#sec-string)). The internal representation may be 1, 2, or 4 bytes per code point, selected per-string at construction; the choice is invisible to programmers but enables `IsAscii` for the common ASCII-only case. For char-level non-allocating substring views, use `AsSpan()` to obtain a `ReadOnlyStringSpan` ([§3.3](#sec-readonlystringspan)) — added in v0.104. For byte access to a string's contents, use `Encoding.UTF8.GetBytes` in `System.Text` ([§3.2](#sec-encoding)): the one-shot form `GetBytes(s)` returns a fresh owned `Array<byte>` and is always correct; the allocation-conscious form — repeated encoding in a loop, I/O paths reusing a buffer — is `GetByteCount(s)` with `GetBytes(s, destination: Span<byte>)` (v0.165), which encodes into a caller-reused `OwningBuffer<byte>` with zero per-call allocation, for every string, ASCII or not. (The ASCII-only zero-copy byte view of v0.106, `ReadOnlyAsciiByteSpan`, is retired — [§3.4](#sec-readonlyasciibytespan), v0.188.)

The `+` operator is declared explicitly here, not as compiler magic; the compiler resolves `s + t` to this overload like any other operator.

<a id="sec-arrayt-extension-methods"></a>
### 9.7 `Array<T>` extension methods

In addition to the methods declared on `Array<T>` itself ([§4.4](#sec-arrayt)), the LINQ extensions in [§5](#sec-system-collections-linq) apply to `Array<T>` because it implements `IEnumerable<T>`.

Multi-dimensional arrays are deferred to a future revision; the current standard library provides only one-dimensional `Array<T>` (heap) and `[N]T` (inline, language spec [§15.1](Bezel_Language_Spec.md#sec-inline-arrays)).

<a id="sec-ranget"></a>
### 9.8 `Range<T>`

The type underlying range expressions (language spec [§16](Bezel_Language_Spec.md#sec-range-expressions)). `Range<T>` is a compiler-intrinsic record. `T` is restricted by the compiler to the integer types (`byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`) and `char`. Static interface members now exist (language spec [§11.8](Bezel_Language_Spec.md#sec-static-interface-members), v0.176) and the standard library defines `ISteppable<T>` ([§2.19](#sec-static-abstractions)), so this restriction is now expressible as a `where` clause; the intrinsic enforcement is retained pending a recorded compiler follow-up.

```bezel
/// Compiler-intrinsic; T restricted to integer types and char.
public record Range<T>: IEnumerable<T>
  public readonly property Start: T?;              // null if open-ended on the lower side
  public readonly property End: T?;                // null if open-ended on the upper side
  public readonly property Step: long;             // default 1; signed to support descending
  public readonly property IsEndInclusive: bool;   // true for `..`, false for `..<`
  public readonly property IsOpenEnded: bool;      // true if Start or End is null
  public readonly property IsEmpty: bool;          // true if the range contains no values

  public method Contains(value: T): bool;
  public method GetEnumerator(): owned IEnumerator<T>;
end Range;
```

`Contains(v)` is `true` iff the range is nonempty under iteration and `v` lies within the endpoint extent (exclusive upper bound excluded) — an empty range contains nothing, and `Contains` agrees with range patterns on the stepless forms (language spec [§16.3](Bezel_Language_Spec.md#sec-range-operations), v0.162). `Step` affects membership only through direction/emptiness. This is the operation invoked by the `in` operator (spec [§17.5](Bezel_Language_Spec.md#sec-the-in-operator)).

`GetEnumerator` produces an enumerator that yields values from `Start` to `End`, incrementing by `Step`. Calling `GetEnumerator` on an open-ended range (`IsOpenEnded` is `true`) raises `InvalidOperationException`. Because `Range<T>` implements `IEnumerable<T>`, ranges compose with LINQ and other IEnumerable-based APIs:

```bezel
import System.Collections.Linq;
var evenSquares := (1..20).Where((x) => x mod 2 = 0).Select((x) => x * x);
var total := (1..100).Sum();
```

The compiler may optimize `for var i in rangeExpr` into a direct counted loop that bypasses `GetEnumerator`; see language spec [§16.5](Bezel_Language_Spec.md#sec-iteration) and [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for).

---

<a id="sec-ffi-boundary-types"></a>
## 10. FFI-boundary types

`CString`, `WString`, `RawBytes`, `IntPtr`, and `UIntPtr` are FFI-boundary types. The first three are classes with `owned`-returning factory methods and idempotent `Dispose`. `IntPtr` and `UIntPtr` are compiler-intrinsic value types representing pointer-sized integers — they are documented here for completeness, but their members are detailed in language spec [§32.7](Bezel_Language_Spec.md#sec-pointers-and-intptr). The types of §10.0–§10.4 live in the `System` module and are auto-imported (language spec [§2.3](Bezel_Language_Spec.md#sec-always-available-names), stated as of v0.186). The typed foreign-memory surface of [§10.5](#sec-system-interop-ptr)–[§10.6](#sec-system-interop-interop) lives in **`System.Interop`**, which is *not* auto-imported — the `import System.Interop;` line is the audit marker that a file performs foreign-memory access.

<a id="sec-intptr-and-uintptr"></a>
### 10.0 `IntPtr` and `UIntPtr`

```bezel
/// Compiler-intrinsic; pointer-sized integer (32 or 64 bits depending on target).
public record IntPtr
  public static const Zero: IntPtr;

  public static method operator =(a: IntPtr, b: IntPtr): bool;
  public static method operator <>(a: IntPtr, b: IntPtr): bool;
end IntPtr;

/// Compiler-intrinsic; unsigned pointer-sized integer.
public record UIntPtr
  public static const Zero: UIntPtr;

  public static method operator =(a: UIntPtr, b: UIntPtr): bool;
  public static method operator <>(a: UIntPtr, b: UIntPtr): bool;
end UIntPtr;
```

`IntPtr` and `UIntPtr` are opaque pointer-sized integers. They are not dereferenced from Bezel code; their purpose is to flow opaque addresses through FFI calls (the result of `dlopen`, the `void*` argument of a callback, etc.). `Zero` is the null-pointer value, declared as a compiler-provided `const` per the [§1](#sec-conventions) convention for compiler-intrinsic types.

The `=` and `<>` operators support null tests (`if handle = IntPtr.Zero then ...`). Unlike the primitive value-type wrappers in [§9.1](#sec-integer-types)–[§9.5](#sec-char), `IntPtr` and `UIntPtr` cannot rely on the language-intrinsic comparison rule for primitive types (language spec [§3.5](Bezel_Language_Spec.md#sec-operators-on-primitive-types)): that rule applies to the keyword-aliased primitive types (`int`, `long`, etc.) and to `string`, not to general record types. Nor can the auto-equality machinery (language spec [§9.2](Bezel_Language_Spec.md#sec-body-records)) supply meaningful equality — `IntPtr` and `UIntPtr` have no auto-properties exposing their underlying pointer-sized value, so the auto-generated `=` would be the zero-property "every-pair-equal" form, which is wrong for pointer comparison. Equality for these types is therefore declared explicitly as operator overloads; the implementations are compiler intrinsics that compare the underlying pointer-sized integer representation.

Explicit conversions to and from `long` and `ulong` (language spec [§32.7](Bezel_Language_Spec.md#sec-pointers-and-intptr)) are also compiler intrinsics: `IntPtr(longValue)` and `long(ptr)` use the standard explicit-conversion syntax ([§5.2](Bezel_Language_Spec.md#sec-explicit-conversions) of the language spec) and are not declared as operator overloads on the record. See language spec [§32.7](Bezel_Language_Spec.md#sec-pointers-and-intptr) for FFI usage.

<a id="sec-cstring"></a>
### 10.1 `CString`

A null-terminated UTF-8 byte sequence, suitable as an argument or return value for C functions taking `const char*` or `char*`.

```bezel
public class CString
  // No public constructor; obtained via factory methods.

  public static method From(s: string): owned CString;
  public static method FromBytes(bytes: Array<byte>): owned CString;

  public readonly property Length: int;       // byte length, excluding the null terminator
  public readonly property Pointer: IntPtr;   // v0.186; address of the buffer — see prose
  public method ToBytes(): owned Array<byte>;

  destructor;       // releases the underlying buffer when this CString
                    //   allocated it (CString.From / FromBytes); for a CString
                    //   wrapping a native-allocated buffer, the disposal
                    //   behavior is governed by the FFI declaration. See prose.
end CString;
```

`CString.From(s)` allocates a fresh, owned `CString` whose contents are the UTF-8 encoding of `s` followed by a single null byte. The caller owns the result; when disposed, the underlying buffer is released.

`CString.FromBytes(bytes)` **copies** the bytes of `bytes` into a fresh, owned buffer and appends a null terminator; the source array is unaffected and the result's buffer is independent of it (the same copy-in semantics as `RawBytes.From` in [§10.3](#sec-rawbytes), and as the borrow parameter `bytes: Array<byte>` requires — a borrowed argument cannot be retained past the call). The caller is responsible for ensuring the bytes are valid UTF-8 and contain no interior null byte; an interior null, by C-string convention, terminates the string early when the buffer is read as a `char*`. Unlike `From(string)` — which encodes a Bezel string's code points — `FromBytes` takes the bytes verbatim, so it is the path for already-encoded or possibly-ill-formed UTF-8 obtained from I/O or another source.

A `CString` returned from a native function is wrapped without copying; the Bezel side must understand the native API's ownership contract (see language spec [§32.6](Bezel_Language_Spec.md#sec-string-marshalling)). Disposing such a `CString` calls the native deallocator only when constructed via `CString.From` or `FromBytes`. For native-allocated strings, the FFI declaration's documentation must specify how the native allocation should be released; this is beyond the scope of `CString` itself.

**`Pointer`** (v0.186) exposes the underlying buffer's address (the same contract as `RawBytes.Pointer`, [§10.3](#sec-rawbytes): valid until the `CString` is disposed) — the element source for `char*[]` argv-building over `OwningBuffer<IntPtr>` ([§4.14](#sec-owned-buffers-and-spans)). Native retention of the address past the synchronous call is the trusted-lifetime obligation of language spec [§32.10](Bezel_Language_Spec.md#sec-boundary-contract).

<a id="sec-wstring"></a>
### 10.2 `WString`

A null-terminated UTF-16 sequence, suitable for Windows APIs and other UTF-16-using ABIs.

```bezel
public class WString
  // No public constructor; obtained via factory methods.

  public static method From(s: string): owned WString;

  public readonly property Length: int;       // length in UTF-16 code units, excluding the null terminator
  public readonly property Pointer: IntPtr;   // v0.186; address of the buffer — see prose

  destructor;       // releases the underlying buffer
end WString;
```

`WString.From(s)` allocates a fresh, owned `WString` whose contents are the UTF-16 encoding of `s` followed by a single zero code unit (two zero bytes). Surrogate pairs are produced for code points above the BMP per UTF-16 rules.

**`Pointer`** (v0.186) exposes the underlying buffer's address, valid until the `WString` is disposed — the same contract as `CString.Pointer` ([§10.1](#sec-cstring)) and `RawBytes.Pointer` ([§10.3](#sec-rawbytes)). Native retention past the synchronous call is the trusted-lifetime obligation of language spec [§32.10](Bezel_Language_Spec.md#sec-boundary-contract).

<a id="sec-rawbytes"></a>
### 10.3 `RawBytes`

A pointer-and-length pair representing arbitrary binary data.

```bezel
public class RawBytes
  // No public constructor; obtained via factory methods.

  public static method From(buffer: Array<byte>): owned RawBytes;
  public static method FromForeign(p: IntPtr, length: int,
      freeOnDispose: bool := false): owned RawBytes;    // v0.186 — see prose

  public readonly property Length: int;
  public readonly property Pointer: IntPtr;

  public method CopyTo(destination: Array<byte>, destOffset: int);

  destructor;       // releases the underlying buffer if owned by this instance
end RawBytes;
```

`RawBytes.From(buffer)` allocates a fresh, owned `RawBytes` containing a copy of the source bytes. Mutations to the source after construction do not affect the `RawBytes`; the buffer is independent.

`Pointer` exposes the underlying memory address as an `IntPtr`, suitable for passing to native APIs that expect `void*`. The pointer is valid until the `RawBytes` is disposed.

**`FromForeign`** (v0.186) wraps `[p, p + length)` **without copying**. With `freeOnDispose := false` (the default) the result is a *view*: `Length`, `Pointer`, and `CopyTo` operate over foreign memory and disposal releases nothing — the destructor's existing "if owned by this instance" contract already carries the vocabulary. With `freeOnDispose := true`, disposal passes `p` to the C runtime's `free(3)` — the "returns a malloc'd buffer the caller must free" contract (POSIX `getline`'s buffer, `realpath(path, NULL)`). Two trust obligations, stated plainly: (a) if the buffer was not allocated by the same C runtime's `malloc` family, disposal is undefined behavior; (b) a custom deallocator (a library's `libfoo_free`) is *not* expressible through this flag — that contract belongs to a wrapper class owning both handle and buffer (language spec [§32.13](Bezel_Language_Spec.md#sec-c-compatible-records)). Cross-domain note: `RawBytes` is a class and not sendable (language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)'s class rule — not `sealed`), so a `FromForeign` view never crosses a domain boundary; stated here so the invariant is explicit.

<a id="sec-conversions-to-bezel-types"></a>
### 10.4 Conversions to Bezel types

Two conversion families exist for reading C-side strings:

- **From a caller-owned `CString` instance:** `string.From(c: CString): string` and `string.From(w: WString): string` copy the contents into a fresh Bezel `string`. The source `CString` or `WString` retains its ownership and disposal obligation.
- **From a raw pointer (no ownership):** `string.FromCStringPointer(p: IntPtr): string` and `string.FromWStringPointer(p: IntPtr): string` read through a pointer the Bezel side does not own and produce a copied Bezel `string`. Use these when the FFI declaration's return type is `IntPtr` (a borrowed pointer; see language spec [§32.6](Bezel_Language_Spec.md#sec-string-marshalling)) rather than `CString`/`WString` (a caller-owned instance).

Both forms are O(n) in the source length and do not retain a reference to the source after return.

Behavior at the boundaries:

- `string.FromCStringPointer(IntPtr.Zero)` and `string.FromWStringPointer(IntPtr.Zero)` raise `ArgumentNullException`. Callers must check for the null pointer before invoking the conversion (the typical pattern is `if p <> IntPtr.Zero then ... end`).
- For non-null pointers, the conversion reads bytes (`FromCStringPointer`) or 16-bit code units (`FromWStringPointer`) starting at the pointer until a null terminator is found. If the pointer does not refer to a properly null-terminated buffer, behavior is undefined — the read may continue past the intended end of the string, returning garbage or causing a segmentation fault. The Bezel side cannot validate; the FFI declaration is trusted to expose only properly-formed strings.

Each FFI-boundary type has a corresponding `string.From` overload ([§9.6](#sec-string)) for `CString` and `WString`, returning a Bezel `string`. For `RawBytes`, callers convert through `Array<byte>` via `CopyTo` and pass the byte array to the appropriate decoder (e.g. `Encoding.UTF8.GetString`).

<a id="sec-system-interop-ptr"></a>
### 10.5 `System.Interop`: `Ptr<T>`

```bezel
// Compiler-intrinsic value record; module System.Interop (NOT auto-imported).
// Representation identical to IntPtr. T is gated (per-instantiation, language
// spec §3.6-style diagnostic) to the FFI-POINTEE SET: integer types, float,
// double, bool, IntPtr, UIntPtr — and C-compatible records (language spec
// §32.13). Nested Ptr<Ptr<T>> is NOT admitted (language spec §32.12).
public record Ptr<T>
  public static const Zero: Ptr<T>;                    // compiler-provided, like IntPtr.Zero

  public static method FromIntPtr(p: IntPtr): Ptr<T>;  // the only untyped->typed entry: EXPLICIT
  public method ToIntPtr(): IntPtr;

  // Every member below is TRUSTED BUT UNVERIFIED (language spec §32's preamble
  // posture, verbatim): the address's validity, alignment, and pointee type are
  // the programmer's declaration; a wrong one is undefined behavior.
  public method Load(): T;                             // read  *p
  public method Store(value: T);                       // write *p
  public method LoadAt(index: int): T;                 // read  p[index]  (index scaled by T's ABI stride)
  public method StoreAt(index: int, value: T);         // write p[index]
  public method Add(count: int): Ptr<T>;               // p + count elements; negative counts allowed

  public method CopyToArray(count: int): owned Array<T>;   // count elements, copied Bezel-side
  public method CopyFromArray(source: Array<T>);           // source.Count elements written at p

  // Equality is declared explicitly, NOT left to record synthesis — the §10.0
  // IntPtr precedent, for the same reason: Ptr<T> has no auto-properties, so the
  // synthesized `=` would be the zero-property "every-pair-equal" form. The
  // intrinsic compares the address representation.
  public static method operator =(a: Ptr<T>, b: Ptr<T>): bool;
  public static method operator <>(a: Ptr<T>, b: Ptr<T>): bool;
end Ptr;
```

The pointee gate mirrors language spec [§32.12](Bezel_Language_Spec.md#sec-foreign-memory): `T` is drawn from the FFI-pointee set — integer types, `float`, `double`, `bool`, `IntPtr`, `UIntPtr`, and C-compatible records (language spec [§32.13](Bezel_Language_Spec.md#sec-c-compatible-records)); any other instantiation is rejected at the instantiation site with a diagnostic naming the set. `Ptr<Ptr<U>>` is not admitted; pointer-array walking is `Ptr<IntPtr>` plus explicit re-typing.

`CopyToArray(count)` copies `count` elements from foreign memory into a fresh Bezel-owned `Array<T>`; `CopyFromArray(source)` writes `source.Count` elements at `p`. Both are explicit whole-copies — foreign data becomes Bezel data at exactly one visible point.

Equality is declared explicitly rather than left to record synthesis for the same reason as `IntPtr`'s ([§10.0](#sec-intptr-and-uintptr)): `Ptr<T>` has no auto-properties, so the synthesized `=` would be the zero-property "every-pair-equal" form. `Zero` is the null pointer, a compiler-provided `const` per the [§1](#sec-conventions) convention for compiler-intrinsic types.

`Ptr<T>` computes **sendable** under language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)'s value-type rule — ratified at v0.186 with the cross-domain trust bullet of language spec [§32.10](Bezel_Language_Spec.md#sec-boundary-contract): foreign memory reached through a crossed pointer is outside domain isolation, and unsynchronized cross-domain access to the same address is a data race and undefined behavior.

<a id="sec-system-interop-interop"></a>
### 10.6 `System.Interop`: `Interop`

```bezel
/// Static intrinsics for the FFI boundary. Module System.Interop (NOT auto-imported).
public static class Interop
  // The calling thread's C errno, read directly. Valid only when read BEFORE the
  // next extern call or runtime operation on this thread — and NEVER across an
  // await: a continuation may resume on a DIFFERENT OS thread (language spec
  // §30.13), so a post-await read is another thread's errno. Read it first,
  // synchronously, in the same continuation as the failing call.
  public static method LastErrno(): int;               // v0.186

  // The process-lifetime code address of a callback admissible under the STATIC
  // rule of language spec §32.8; F gated per-instantiation to FFI-compatible
  // function types. One-way by design: no address→function inverse exists.
  public static method CodeAddress<F>(callback: F): IntPtr;   // v0.186
end Interop;
```

`LastErrno()` reads the calling thread's C `errno` directly. The window discipline is real, not decorative: read it into a local **before** constructing an exception message — interpolation allocates, and allocation is a runtime operation that may clobber the thread-local — and **never across an `await`** (a continuation may resume on a different OS thread, language spec [§30.13](Bezel_Language_Spec.md#sec-runtime-model): a post-await read is another thread's errno). Language spec [§32.9](Bezel_Language_Spec.md#sec-error-handling) shows the canonical use.

`CodeAddress<F>(callback)` returns the process-lifetime code address of a callback admissible under the **static** rule of language spec [§32.8](Bezel_Language_Spec.md#sec-callbacks) — a static method reference or a non-capturing lambda; a capture-bearing argument is rejected with §32.8's diagnostic, and the `env` clause does not apply (there is no bounding call). `F` is gated per-instantiation to FFI-compatible function types. One-way by design: no address→function inverse exists (language spec [§32.14](Bezel_Language_Spec.md#sec-deferred-ffi-surface)).

---

<a id="sec-modules-deferred-to-future-revisions"></a>
## 11. Modules deferred to future revisions

The following standard library modules are anticipated but not specified in this version. Their absence is intentional; the current scope focuses on what the language specification depends on.

- `System.Net` and `System.Net.Http` — networking and HTTP. The language specification's async examples use a hypothetical `HttpClient`; a real one will live here.
- `System.Text.Json` and `System.Text.Regex` — a general JSON DOM/streaming API and regular expressions. Note this is distinct from the serialization JSON *format agent* (`JsonSerializer`/`JsonDeserializer` in `System.Serialization`, [§2.15](#sec-concrete-serialization-formats)), which is provided: `System.Serialization` maps the [§2.14](#sec-serialization-vocabulary) token stream to and from JSON for whole-value (de)serialization, whereas a future `System.Text.Json` would offer document-tree navigation and ad-hoc querying of arbitrary JSON not driven by a target type. The two are complementary, as they are in other ecosystems that provide both a serializer and a document API.
- `System.Security.Cryptography` — cryptographic primitives.
- `System.Globalization` — culture-aware formatting and comparison.
- `System.Reflection` — runtime metadata beyond `typeof` and `GetType`. This splits three ways (rationale [§18](Bezel_Design_Rationale.md#sec-things-deferred)): type-lattice metadata is **delivered** on the `Type` class ([§2.13](#sec-the-type-class), v0.113); member-access reflection is **declined, not deferred**; generic-instantiation decomposition remains deferred, pending an unbound-generic `typeof`.
- `System.Diagnostics.Process` — child-process spawning and management.

The shape of these modules is open. Their inclusion in a future revision will be additive; nothing in this version will need to change to accommodate them.
