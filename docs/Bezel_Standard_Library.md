# Bezel Standard Library Specification

**Companion to the Bezel Language Specification, v0.131 — June 2026**

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
- Compiler-intrinsic types (primitive wrappers, `Range<T>`) may declare `const` members whose values are provided by the compiler rather than by a source-level initializer expression. In the declarations below, a `const` field shown without `:= value` (e.g. `public static const MaxValue: double;`) is such a compiler-provided constant. User-defined code may not omit `const` initializers (see language spec [§22.12](Bezel_Language_Spec.md#sec-static-members)).
- **Elided interface-required members.** When a class declares it implements an interface, the members required by that interface are part of the class's public contract whether or not the declaration shows them explicitly. Some declarations show every interface-required member; others elide the redundant ones with a marker comment such as `// plus standard ICollection<T> operations` or `// ICollection<T> operations plus:`. Both forms are equivalent — the elided members are still part of the public contract, with signatures determined by the interface declaration plus the implementation's type substitution (per the [§11.2](Bezel_Language_Spec.md#sec-interface-declaration) exact-match rule). For interfaces whose substitution produces a member with the same name but a different parameter type than another method on the class — `Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>` is the leading example, where ICollection's `Add(item: T)` substitutes to `Add(item: DictionaryEntry<K, V>)` while Dictionary also offers `Add(key: K, value: V)` — both forms are public members of the class and both are reachable.

This document is normative for the names, signatures, and observable behavior listed; it is not normative for performance characteristics, internal data structures, or the choice of algorithm. Implementations are free to optimize.

---

<a id="sec-system-auto-imported"></a>
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
  // ... corresponding Try forms

  public static method ToString(value: int): string;
  public static method ToString(value: int, radix: int): string;   // 2, 8, 10, 16
  // ... corresponding ToString forms for other primitives
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
  public static method GetEnvironmentVariables(): Dictionary<string, string>;

  public static method Exit(exitCode: int);
end Environment;
```

`GetEnvironmentVariables()` returns a `Dictionary<string, string>`. Although `Environment` itself is auto-imported via `System` ([§2.3](Bezel_Language_Spec.md#sec-always-available-names) of the language spec), the `Dictionary` type lives in `System.Collections.Generic` and is not auto-imported; callers using the return value typically `import System.Collections.Generic;` to access dictionary members.

<a id="sec-datetime-timespan-dateonly-timeonly"></a>
### 2.5 `DateTime`, `TimeSpan`, `DateOnly`, `TimeOnly`

Value types for representing points in time and durations. All are records.

```bezel
public record DateTime
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

public record TimeSpan
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

`DateOnly` and `TimeOnly` are similar value records carrying only the date or time components, respectively.

<a id="sec-guid"></a>
### 2.6 `Guid`

A 128-bit globally unique identifier.

```bezel
public record Guid
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
  public method NextLong(): long;
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
end BitConverter;
```

`FloatToInt32Bits` and `DoubleToInt64Bits` return the raw IEEE 754 bit pattern of the floating-point value as a signed integer of matching width. The integer return types are `int` (32 bits) and `long` (64 bits) — Bezel's standard width-named primitives, equivalent to the boxed record types `Int32` and `Int64` ([§9.1](#sec-integer-types)) that appear in the method names. `Int32BitsToFloat` and `Int64BitsToDouble` perform the reverse — interpreting the integer's bit pattern as a float.

These are pure bit-reinterpretation operations: the bit patterns are preserved exactly. For `NaN` values, the specific bit pattern (signaling vs quiet, payload bits) is preserved, which is what bit-level tools (serialization, hashing, exact comparison) need.

The method names use the sized record-type names (`Int32`, `Int64`) consistently with the byte-buffer accessors of [§4.14](#sec-owned-buffers-and-spans). The `Float`/`Double` portion uses the primitive keyword names (Bezel has no separate `Single` record type; `float` is the only 32-bit floating-point type). No unsigned forms (e.g. `FloatToUInt32Bits`) are provided — the bit-pattern integer is treated as opaque by callers most of the time, and the signed form interoperates with the standard bitwise operators identically.

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

**Kind predicates.** For any `Type`, exactly one of `IsClass`, `IsInterface`, `IsRecord`, `IsEnum`, `IsPrimitive`, `IsInlineArray`, `IsFunctionType` holds. These map the type kinds of language spec [§3.1](Bezel_Language_Spec.md#sec-type-kinds) onto a runtime predicate surface, with two deliberate flattenings: `string` and the heap array types (`Array<T>`, `OwnedArray<T>`) report `IsClass` (they are reference types — language spec [§3.1](Bezel_Language_Spec.md#sec-type-kinds) lists `string` and heap arrays as reference types reaching `Object` directly, and treating them as classes here is consistent with how the language and standard library describe them elsewhere); the unit type `()` reports `IsValueType` with none of the seven primary predicates true (it is a degenerate value type and the rare `typeof(())` is not expected in practice, but the surface does not claim a primary kind for it). `IsValueType` is a derived convenience equal to `IsPrimitive or IsEnum or IsRecord or IsInlineArray` (and additionally true for the unit type); it does not add a primary category. The "exactly one of seven" guarantee therefore holds for every type a program can name with `typeof` or obtain from `GetType` except the unit type, which no realistic program reifies.

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

**`ISerializer` / `IDeserializer` are the format abstraction.** A concrete format implements them once; the standard library is expected to provide `JsonSerializer`/`JsonDeserializer` and a compact `BinarySerializer`/`BinaryDeserializer` pair (their full definitions are deferred — see rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection) and [§18](Bezel_Design_Rationale.md#sec-things-deferred)). The interfaces are format-agnostic by construction: they speak only of primitives (`WriteInt`, `WriteString`, …) and structure (`BeginObject`/`EndObject`, `BeginArray`/`EndArray`). They name no format. A type that serializes itself against `ISerializer` is automatically serializable to every format, present and future, without change — exactly the property reflection-based serializers achieve at the cost of runtime type inspection, achieved here with none.

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

**Why `IDeserializable` has no method in this version.** Serialization is an *instance* operation (an existing value writes itself); deserialization is a *static* one (a format reader produces a new value — there is no instance yet). A static entry point cannot be expressed as an interface method until static interface members exist (a separately deferred feature, rationale [§18](Bezel_Design_Rationale.md#sec-things-deferred)). Rather than block deserialization on that keystone, v0.114 recognizes the deserialization entry point **by name**, exactly as the user-defined collection-literal protocol recognizes `FromCollectionLiteral` ([§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol) of the language spec): a type that declares `: IDeserializable` and provides a static method of the shape

```bezel
public static method Deserialize(d: IDeserializer): T;          // value-type
                                                                //   target (record,
                                                                //   enum): no `owned`
public static method Deserialize(d: IDeserializer): owned T;     // class target:
                                                                //   `owned` return
// T = the declaring type, written concretely.
```

is deserializable. The recognized factory's return type is the declaring type itself, written concretely. The `owned` modifier appears exactly when the declaring type is a class (a class instance is an owned heap value transferred to the caller — language spec [§28.8](Bezel_Language_Spec.md#sec-ownership-transfer)); for a value-type target (record or enum, the synthesis-eligible common case) the return is a plain by-value `T` with no `owned` modifier, because `owned` on a non-class return is semantically rejected (language spec [§22.1](Bezel_Language_Spec.md#sec-method-declarations) — the modifier applies only to class-typed returns). Bezel has no `Self` type (that feature is separately deferred), so the return type is spelled with the concrete type name. `IDeserializable` is a *marker* whose presence says "this type provides the recognized `Deserialize` factory," in the same way `: IEquatable<T>` opts a record into synthesized equality without the user writing the comparison. The hand-written form today, for the record (value-type) case:

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

As of v0.119, a **class** can also be synthesized, by declaring `serialized tree` on its header — an explicit assertion that its serialized object graph is a tree (no aliasing among serialized edges, no cycles):

```bezel
public class Order(id: int) serialized tree : ISerializable, IDeserializable
  public owned Customer: Customer;        // nested owned class, also tree
  public owned Lines: OwnedArray<owned LineItem>;
  public readonly property Id: int => id;
  // compiler synthesizes Serialize / Deserialize; each owned child
  // recurses through its own synthesized members
end Order;
```

As of v0.121, a class whose serialized graph *aliases* — the same object reachable by more than one serialized path — declares `serialized graph` instead, asserting a directed acyclic graph (shared references allowed, no cycles):

```bezel
public class Invoice(number: string) serialized graph : ISerializable, IDeserializable
  public owned Seller: Company;           // owned: in the owning spanning tree
  public owned Buyer: Company;            // owned
  public AccountManager: Employee;        // borrow: a back-edge, e.g. an
                                          //   Employee owned by Seller
  public readonly property Number: string => number;
end Invoice;
```

The synthesized `Deserialize` for a class returns `owned Invoice` (a class instance is an owned heap value transferred to the caller). It reconstructs the owning spanning tree (the `owned` edges) in owner-before-borrower order; each borrowing back-edge (here `AccountManager`) is supplied **as a constructor argument**, resolved against an identity table that already holds the target because a directed acyclic graph always admits such an order. Because every reference field is bound through the constructor, before any constructor body runs, no constructed object ever has an unbound non-nullable reference — definite assignment (language spec [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields)) holds with no nullable relaxation and no observable intermediate state. Graph shape cannot be verified statically, so the synthesized deserializer **enforces it dynamically**: `serialized tree` raises on any repeated object identity; `serialized graph` binds a repeated reference to the already-constructed object (identity preserved, not duplicated — Bezel refuses the silent-duplication failure mode of reference-unaware serializers) but raises `SerializationException` on a **cycle** (a reference whose target is still under construction).

As of v0.122, a base class whose components may hold derived instances declares an explicit `polymorphic` registry alongside its graph-shape clause:

```bezel
public abstract class Shape serialized graph
    polymorphic ("circle": Circle, "square": Square)
    : ISerializable, IDeserializable
end Shape;

public class Circle(radius: double) serialized tree : Shape
  public readonly property Radius: double => radius;
end Circle;
```

The registry is a closed, explicitly written map from a wire discriminator to a permitted concrete type — **no reflective subtype discovery** (the design principle of language spec [§5.18](Bezel_Design_Rationale.md#sec-type-identity-not-reflection); this is what prevents a serialized discriminator from becoming an arbitrary-type construction primitive, the defect behind reflective-`$type` remote-code-execution vulnerabilities elsewhere). The synthesized serializer writes the concrete type's discriminator as a leading `$type` field; the synthesized deserializer reads it **first**, looks it up in the registry, and delegates to that type's synthesized `Deserialize`. An unregistered discriminator (read) or an unregistered run-time type (write) raises `SerializationException` — the closed set enforced on both directions. Polymorphic dispatch composes with every graph-shape clause (each registered type reconstructs under its own clause, including `serialized cyclic`).

As of v0.124, a class whose serialized graph may contain **cycles** declares `serialized cyclic`:

```bezel
public class Node(label: string) serialized cyclic : ISerializable, IDeserializable
  public owned Next: Node;          // owned forward edge
  public Parent: Node;              // back-edge: may close a cycle
  public readonly property Label: string => label;
  // constructor is self-contained: assigns only its own fields,
  // reads no back-edge, never lets `this` escape — so it is
  // cyclic-serialization-eligible
end Node;
```

`serialized cyclic` deserializes via a **two-phase construction**: every object in the group is constructed first (back-edge fields not yet bound), then all back-edges are bound through the identity/`$ref` table. This is sound only because every participating class has a **self-contained constructor** — a statically-checked rule (its constructor body, anything it chains to via `this(...)`/`base(...)`, and any field/property initializer that runs during construction may assign its own fields but must not read a back-edge, call an instance method on `this`, let `this` escape, or call a virtual method; the rule is transitive over the whole construction, see language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis)). A class in a `serialized cyclic` group whose constructor is not self-contained is a **compile error**, not a runtime fault; such a class hand-writes `Serialize`/`Deserialize` (the only remaining reason to). Definite assignment (language spec [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields)) is preserved unmodified: a non-nullable back-edge is never observed unset, because the only code running between the two phases is self-contained constructors that provably cannot read a back-edge (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis); the design proof is rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection)).

This completes the serialization stack: every class object graph — tree, DAG, or cyclic — is synthesizable, with polymorphic edges resolved through an explicit registry, the only remaining reason to hand-write `Serialize`/`Deserialize` being a class whose constructor cannot be made self-contained. The concrete format implementations are provided as of v0.115 — see [§2.15](#sec-concrete-serialization-formats). The staging was deliberate: vocabulary (v0.114, the one piece that cannot be retrofitted), then concrete formats (v0.115), record/enum synthesis (v0.116), field-name customization (v0.117), operation-scoped unknown-field tolerance (v0.118), tree-shaped class synthesis (v0.119), the deferred-binding working-out (v0.120, rationale only), shared-reference DAG synthesis (v0.121), polymorphic dispatch (v0.122), the cyclic-case proof discharge (v0.123, rationale only), then cyclic class synthesis (v0.124), each an independent non-breaking addition.

<a id="sec-concrete-serialization-formats"></a>
### 2.15 Concrete serialization formats

The standard library provides two concrete implementations of the [§2.14](#sec-serialization-vocabulary) agent interfaces: a JSON pair (`JsonSerializer`/`JsonDeserializer`) and a compact binary pair (`BinarySerializer`/`BinaryDeserializer`). Both are ordinary classes implementing `ISerializer`/`IDeserializer`; they introduce no language feature. They live in `System.Serialization` and are *not* auto-imported (unlike the vocabulary interfaces) — a program that performs serialization names the format explicitly with `import System.Serialization`.

Both serializers write to, and both deserializers read from, an `OwnedBuffer<byte>`-backed stream. The serializer accumulates bytes and exposes the result; the deserializer is constructed over a `ReadOnlySpan<byte>` of the encoded form.

```bezel
public class JsonSerializer: ISerializer, IDisposable
  public constructor();
  public constructor(options: JsonOptions);
  // ISerializer surface (§2.14) implemented for JSON text.
  public method ToBytes(): owned OwnedBuffer<byte>;   // UTF-8 encoded result
  public method ToString(): string;                   // convenience: UTF-8 → string
end JsonSerializer;

public class JsonDeserializer: IDeserializer, IDisposable
  public constructor(input: ReadOnlySpan<byte>);      // UTF-8 JSON
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
  public method ToBytes(): owned OwnedBuffer<byte>;
end BinarySerializer;

public class BinaryDeserializer: IDeserializer, IDisposable
  public constructor(input: ReadOnlySpan<byte>);                     // strict
  public constructor(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool);
  // IDeserializer surface (§2.14) implemented for the compact binary format.
end BinaryDeserializer;
```

**JSON mapping.** The `ISerializer` token stream maps to JSON as follows. `WriteInt`/`WriteUInt` produce JSON numbers; `WriteFloat` produces a JSON number, with non-finite values (`NaN`, `±Infinity`) raising `SerializationException` (JSON has no representation for them — a binary format is the correct choice when non-finite floats must round-trip). `WriteDecimal` produces a JSON number written without precision loss. `WriteBool` produces `true`/`false`; `WriteString` produces a quoted, escaped JSON string; `WriteChar` produces a one-character JSON string. `WriteNull` produces `null`. `BeginObject`/`WriteFieldName`/`EndObject` produce a JSON object; the `fieldCount` argument to `BeginObject` is advisory (JSON objects are not length-prefixed) and is ignored by `JsonSerializer`. `BeginArray`/`EndArray` produce a JSON array; the `elementCount` argument is likewise advisory. On the read side, `JsonDeserializer.BeginObject()` and `BeginArray()` return `-1` (JSON does not prefix counts), so consumers use `ReadFieldName()` driven by the object's textual extent and `HasMoreElements()` for arrays. `TryReadNull()` consumes and returns `true` iff the next value is `null`.

**Binary mapping.** The compact binary format is length-prefixed and self-describing enough to be read back without the schema, but it is *not* a stable interchange format across Bezel versions — it is intended for same-version persistence and IPC, where compactness and speed matter and the producer and consumer share a build. Integers use a variable-length encoding; `BeginObject`/`BeginArray` write the count, so `BinaryDeserializer.BeginObject()` and `BeginArray()` return the actual element count and `HasMoreElements()` is answered from it. Non-finite floats round-trip exactly (the format stores IEEE 754 bit patterns via `BitConverter`, [§2.12](#sec-bitconverter)). Programs needing a documented, version-stable wire format use JSON or an explicit hand-written format; the binary format trades interchange stability for size and speed.

**Ownership and lifetime.** All four classes are `IDisposable`; the serializers own an internal growable byte buffer, the deserializers own parser state. The idiom is scope-bound:

```bezel
import System.Serialization;

public method SavePoint(p: Point): owned OwnedBuffer<byte>
  using var s := new JsonSerializer();
  p.Serialize(s);                       // p implements ISerializable (§2.14)
  return s.ToBytes();                   // ownership of the buffer transfers out
end SavePoint;

public method LoadPoint(bytes: ReadOnlySpan<byte>): Point
  using var d := new JsonDeserializer(bytes);
  return Point.Deserialize(d);          // recognized static factory (§2.14)
end LoadPoint;
```

`ToBytes()` returns an `owned OwnedBuffer<byte>` ([§4.14](#sec-owned-buffers-and-spans)) — a fresh buffer whose ownership transfers to the caller; the serializer may be disposed immediately after. `JsonSerializer.ToString()` is a convenience that UTF-8-decodes the same bytes into a `string`. A deserializer constructed over a `ReadOnlySpan<byte>` borrows that span for the deserializer's lifetime; the backing bytes must outlive the `using var d` scope, which the example satisfies because the span parameter outlives the method body.

**Errors.** Malformed input, type mismatches (a `ReadInt` when the next token is a string), depth-limit violations, and trailing-data conditions raise `SerializationException` (declared in `System.Serialization`). `SerializationException` carries a byte offset (for binary) or line/column (for JSON) to locate the fault. The deserializer interfaces' `Try*`-shaped members (`TryReadNull`) do not throw; every other read member throws on mismatch rather than returning a sentinel, because a deserialization mismatch is a structural error in the input, not an expected control-flow branch.

**Unknown-field policy.** A field present in the input that matches no declared component of the target type is, by default, a `SerializationException` — the strict default established when synthesis shipped (v0.116), because silently dropping unrecognized input masks producer/consumer schema drift and field-name typos. This behavior is a property of the deserialization *operation*, not of the target type: the same type may legitimately be read strictly when consuming a trusted internal message and leniently when consuming a third-party producer that adds fields freely, a decision that belongs to the caller performing the read, not to the type's serialization contract (rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection)). It is therefore configured on the deserializer: `JsonOptions.IgnoreUnknownFields` (default `false`) for JSON, the `ignoreUnknownFields` constructor parameter for `BinaryDeserializer`. When set, the deserializer skips an unrecognized field's value (via `IDeserializer.SkipValue`, [§2.14](#sec-serialization-vocabulary), which consumes the value including any nested object/array structure) and continues rather than raising. The synthesized `Deserialize` (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis)) is identical regardless of policy — it always delegates an unrecognized field to the deserializer; strictness lives entirely in the deserializer's configuration. This policy governs only *unknown* fields; missing required fields, duplicates, and type mismatches remain unconditional errors regardless of the setting, and a *missing* declared field is tolerated only by the per-member `serialized optional` clause (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis)) — the two leniencies are orthogonal and compose.

`JsonSerializer`, `JsonDeserializer`, `BinarySerializer`, `BinaryDeserializer`, `JsonOptions`, and `SerializationException` are declared in `System.Serialization` and require an explicit `import System.Serialization`. The vocabulary interfaces they implement (`ISerializer`/`IDeserializer`/`ISerializable`/`IDeserializable`, [§2.14](#sec-serialization-vocabulary)) remain auto-imported, so a type's `: ISerializable` conformance never requires the import — only the code that picks a concrete format does.

---

<a id="sec-system-text-auto-imported"></a>
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
  public abstract readonly property Name: string;

  public static readonly property Utf8: Encoding;
  public static readonly property Utf16: Encoding;
  public static readonly property Utf32: Encoding;
  public static readonly property Ascii: Encoding;
end Encoding;
```

The static properties (`Utf8`, `Utf16`, `Utf32`, `Ascii`) return singleton instances of implementation-provided concrete subclasses. The concrete subclass types are not part of the public API; callers access them through the `Encoding` base type. The singleton instances are program-lifetime objects with no resources to release; their `Dispose()` is a no-op. They are not marked `static owned` — they are exempt from the ownership-discharge requirement by the same reasoning as non-`owned` static class-typed fields ([§22.13](Bezel_Language_Spec.md#sec-static-initialization-order) of the language spec).

`Encoding` is `abstract` to permit implementation-provided concrete subclasses. User code may subclass `Encoding` for custom encodings; user subclasses are subject to normal class disposal rules (they may have destructors and owned fields).

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

**Why a separate type from `ReadOnlySpan<char>`.** `ReadOnlySpan<char>` (stdlib [§4.14](#sec-owned-buffers-and-spans)) views into an `OwnedBuffer<char>` — a contiguous char buffer with each slot occupying `sizeof(char)` bytes. A `string`'s internal storage is adaptive (1, 2, or 4 bytes per code point per language spec [§4.6](Bezel_Language_Spec.md#sec-string)), so a string-backed view cannot share storage with a buffer-backed view. Two source types would force `ReadOnlySpan<T>`'s `Buffer` field to be polymorphic over source type, which would either change the existing record's shape (breaking v0.99-v0.103) or require a general source-polymorphism mechanism that v0.104 does not introduce. The separate `ReadOnlyStringSpan` type leaves `ReadOnlySpan<char>` unchanged and makes the source visible at API boundaries ("does this method want a buffer-backed view or a string-backed view?").

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
### 3.4 `ReadOnlyAsciiByteSpan`

A read-only byte view over the contiguous code points of an ASCII string. The natural companion to `string.AsAsciiByteSpan()` for FFI and I/O paths that need UTF-8 bytes from an ASCII string without going through `Encoding.UTF8.GetBytes` (which allocates a fresh byte array). Applies only to ASCII strings — the construction APIs check `IsAscii` and reject non-ASCII inputs.

```bezel
public record ReadOnlyAsciiByteSpan(Source: string, Start: int, Length: int): IReadOnlySpan<byte>

  public property [i: int]: byte
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return byte(Source[Start + i]);
    end;
  end;

  public readonly property IsEmpty: bool => Length = 0;

  // Range construction
  public method Slice(start: int, length: int): ReadOnlyAsciiByteSpan =>
    new ReadOnlyAsciiByteSpan(Source, this.Start + start, length);

  // Search
  public method IndexOf(value: byte): int;
  public method Contains(value: byte): bool;

  // Equality (own type and cross-source against ReadOnlySpan<byte>)
  public method SequenceEqual(other: ReadOnlyAsciiByteSpan): bool;
  public method SequenceEqual(other: ReadOnlySpan<byte>): bool;
  public method SequenceEqual(other: IReadOnlySpan<byte>): bool; // v0.108 — interface form

  // Bulk copy (v0.109)
  public method CopyTo(destination: Span<byte>);    // copy bytes to destination;
                                                     //   lengths must match
end ReadOnlyAsciiByteSpan;
```

A `ReadOnlyAsciiByteSpan` is a borrow-bearing record ([§9.7](Bezel_Language_Spec.md#sec-records-and-disposal) of the language spec) viewing into a contiguous range of an ASCII `string`. The `Source` field carries a borrow of a string; per the borrow-bearing-record rule, the span's lifetime is bounded by the source's lifetime. Strings are reference-typed and runtime-managed ([§4.6](Bezel_Language_Spec.md#sec-string)), so the lifetime story simplifies (as with `ReadOnlyStringSpan` in [§3.3](#sec-readonlystringspan)): as long as anything reachable from the program holds the string, the span remains valid.

The type is purely user-level Bezel — no compiler intrinsic. The record machinery, borrow lifetime tracking, and indexer dispatch are all standard language features.

**Why a separate type from `ReadOnlySpan<byte>` and `ReadOnlyStringSpan`.** `ReadOnlySpan<byte>` views into an `OwnedBuffer<byte>` and cannot be backed by a string without source polymorphism (the same design problem solved by introducing `ReadOnlyStringSpan` in v0.104). `ReadOnlyStringSpan` views chars, not bytes — it has the right source (a string) but the wrong element type (`char`, 32 bits per element). The new type combines a string source with byte element type, with construction validated to keep the byte cast safe.

**ASCII invariant and the byte cast.** Construction APIs (`String.AsAsciiByteSpan` and `String.TryAsAsciiByteSpan` in [§9.6](#sec-string)) check that every code point in the source is in the range `0..127`. Once constructed, the indexer reads each char via the string's indexer and narrows to byte unconditionally — the ASCII check at construction guarantees no narrowing-overflow exception will fire at access time. The cast `byte(Source[Start + i])` is therefore safe for every index in the span's range.

`Slice` produces a sub-span over the same source; the ASCII invariant is preserved automatically because the source has not changed. No re-check is performed.

**Bounds-check semantics.** The indexer bounds-checks `i` against the span's `Length` (not just the source string's length); `IndexOutOfRangeException` is thrown if `i < 0` or `i >= Length`. This matches the v0.105 bounds-check rule for `Span<T>`/`ReadOnlySpan<T>` and the v0.104 rule for `ReadOnlyStringSpan`.

**Per-access cost.** When the string is stored in 1-byte-per-code-point representation (the typical case for ASCII strings under the adaptive-storage model of language spec [§4.6](Bezel_Language_Spec.md#sec-string)), the byte cast is effectively a direct memory read; the "zero-copy" claim holds end-to-end. When the string is stored in wider representation, the indexer reads the wider value (2 or 4 bytes) and narrows to byte; the view is still allocation-free, but the per-access cost is a width-dispatched read plus a narrowing operation. Implementations are expected to use 1-byte storage for ASCII strings whenever practical, but the language does not require it; programs requiring guaranteed contiguous-byte access for FFI can still use `Encoding.UTF8.GetBytes(s)` ([§3.2](#sec-encoding)), which produces a fresh `Array<byte>` at the cost of one allocation.

**Search and equality.** `IndexOf(value: byte)` walks the span's range, returning the first index whose byte matches `value`, or `-1` if none. `Contains(value: byte)` returns `IndexOf(value) >= 0`. `SequenceEqual(other: ReadOnlyAsciiByteSpan)` compares two ASCII byte spans pairwise. The cross-source overload `SequenceEqual(other: ReadOnlySpan<byte>)` compares against a buffer-backed byte span — useful for "does this ASCII header start with these known constant bytes" patterns. There is no corresponding overload on `ReadOnlySpan<byte>` (no extension of v0.99-v0.105 surface); for the symmetric direction, callers invoke the method on the `ReadOnlyAsciiByteSpan` receiver.

**Iteration.** `for var b in span do ... end` is admitted. The desugaring is a direct indexed loop over the span's range, parallel to the buffer-backed span rule of language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for). The iteration variable's type is `byte` and the loop body sees a copy of each byte.

**Construction.** Callers construct `ReadOnlyAsciiByteSpan` via the `AsAsciiByteSpan` / `TryAsAsciiByteSpan` methods on `string` ([§9.6](#sec-string)) rather than via the constructor directly. The direct constructor is admitted but does not perform an ASCII check itself — passing a non-ASCII string to the constructor would produce a span whose indexer might silently lose information on access (the byte cast would narrow a code point above 127 to its low 8 bits, an obvious bug). The `AsAsciiByteSpan` / `TryAsAsciiByteSpan` methods are the supported entry points; calling the constructor directly is allowed but is the caller's responsibility to invoke only on a verified ASCII source.

**What this type is not.** `ReadOnlyAsciiByteSpan` is not a general byte view — it is specifically tied to ASCII strings. Binary parsing on byte buffers should use `ReadOnlySpan<byte>` ([§4.14](#sec-owned-buffers-and-spans)), which has the `ReadInt32LittleEndian` / `WriteUInt64BigEndian` / etc. accessors of v0.103. The two types have different motivating use cases (ASCII text vs binary data) and do not share API surface; the cross-source `SequenceEqual` overload is the only bridge.

---

<a id="sec-system-collections-generic"></a>
## 4. `System.Collections.Generic`

The collection types. None are auto-imported except the iteration-interface family (the read-only and mutating interfaces), which is auto-imported per language spec [§2.3](Bezel_Language_Spec.md#sec-always-available-names).

The language separates **borrowing** collections (store references the caller continues to own; element disposal is the caller's responsibility) from **owning** collections (own their elements; element disposal is the collection's responsibility). Each kind has its own concrete class and its own interfaces; the two kinds share read-only iteration interfaces (`IEnumerable<T>`, `IReadOnlyCollection<T>`, `IReadOnlyList<T>`) because reading produces borrows in either case.

The naming convention: borrowing collection types use their plain names (`Array<T>`, `LinkedList<T>`, etc.); owning collection types are prefixed `Owned…` (`OwnedArray<T>`, `OwnedLinkedList<T>`, etc.). The owning types constrain `where T: class` (and, for `OwnedDictionary<K, V>`, `where V: class`) because ownership only makes sense for class-typed elements.

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
public interface IOwnedCollection<T>: IReadOnlyCollection<T>, IDisposable
  where T: class
  readonly property IsReadOnly: bool;
  method Add(owned item: T);
  method Clear();                            // disposes every element
  method Contains(item: T): bool;            // borrow-taking query
  method Remove(item: T): bool;              // disposes the removed element
end IOwnedCollection;

public interface IOwnedList<T>: IReadOnlyList<T>, IOwnedCollection<T>
  where T: class
  property [i: int]: T
    get;
    set(owned);                              // takes owned T; disposes previous value
  end;
  method Insert(index: int, owned item: T);
end IOwnedList;
```

The two owning-collection interfaces. They extend the borrowing read-only interfaces (`IReadOnlyCollection<T>`, `IReadOnlyList<T>`) for unified read-only access, and they additionally extend `IDisposable` because owning collections are themselves owned objects (their `Dispose()` cascades through their elements).

The mutating methods take `owned` parameters where appropriate: `Add` and `Insert` take ownership of the new element; the indexer setter takes ownership and disposes the previous slot value. Read-side methods (`Contains`, the indexer getter, iteration through the read-only-collection ancestor) produce borrows.

The `where T: class` constraint expresses that owning collections only make sense for class-typed elements. Value types and runtime-managed types (`string`, boxed values, function values) have no disposal obligation; storing them in an "owning" collection would carry no semantic content. The constraint is enforced at instantiation: `OwnedArray<int>`, `OwnedArray<string>`, `OwnedArray<MyRecord>` are all compile errors.

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
ReportEach(myPool);            // OwnedArray<Connection> → IReadOnlyList<Connection> ✓
```

The two hierarchies do *not* share mutating interfaces. `IList<T>` (borrowing) and `IOwnedList<T>` (owning) are unrelated types with no subtyping; code that abstracts over "any mutable list" must pick one. This is intentional — the semantic of `Add` differs between the two (borrow-taking vs. owned-taking), so there is no useful single abstraction over both.

For draining iteration that transfers ownership, the owning collections expose `Drain()` returning `OwnedEnumerable<T>` (declared in [§4.13](#sec-owning-enumeration-interfaces)).

<a id="sec-arrayt"></a>
### 4.4 `Array<T>`

Borrowing resizable array. Stores references that the caller continues to own (or value-typed elements directly).

```bezel
public class Array<T>: IList<T>
  public constructor();
  public constructor(capacity: int);

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
end Array;

public extension Array<T> where T: IComparable<T>
  public method Sort();                    // in place
  public method Sorted(): owned Array<T>;  // fresh sorted copy
end;
```

A collection literal `[1, 2, 3]` constructs an `Array<T>` when the target type is `Array<T>`; it constructs an inline array `[N]T` when the target is an inline array. See language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals) for the contextual-typing rules.

`Array<T>` stores borrows: when the element type is a class, the elements are references whose ownership remains with whoever passed them in. The array's `Dispose()` releases the backing storage but does not dispose the elements. To dispose owned class instances when clearing the collection, either call `DisposeElements()` (which disposes each class-typed element then clears) or use `OwnedArray<T>` ([§4.5](#sec-ownedarrayt)) instead.

`Reverse`/`Sort` versus `Reversed`/`Sorted` follow a uniform naming convention: the imperative-verb form (`Reverse`, `Sort`) mutates the receiver in place and returns nothing; the past-participle form (`Reversed`, `Sorted`) leaves the receiver unchanged and returns a fresh `owned Array<T>` with the elements in the new order. The result's ownership mirrors `ToArray(): owned Array<T>` — it is a borrowing array (a fresh backing buffer holding the same element borrows), so the caller is responsible for disposing the array itself, not its elements. `Sorted` comes in two forms, mirroring `Sort`: the comparer-taking method on `Array<T>`, and the parameterless `Sorted()` in the `where T: IComparable<T>` extension that sorts by the natural order. `OwnedArray<T>` deliberately has **no** value copy-forms — neither `Reversed`/`Sorted` nor `ToArray` — because its elements are uniquely owned: a copy would either double-own each element (two arrays claiming the same owned instances) or drain the source (leaving it empty). To reorder an `OwnedArray<T>`, use the in-place `Reverse()`/`Sort()`; to move its elements into a new owning array, drain it (`Drain()`), which transfers ownership and empties the original.

`AsSpan()` and its overloads return an `ArraySpan<T>` (writable view) over a contiguous range of the array; `AsReadOnlySpan()` returns a `ReadOnlyArraySpan<T>`. These views are non-allocating record constructions and admit `Slice`, search, equality, and bulk-mutation methods. See [§4.15](#sec-arrayspant-readonlyarrayspant) for the full description; note that the mutation surface differs from `Span<T>`'s — it is uniform `Fill`/`CopyFrom`/`Reverse` for all T, without the class/value split that `Span<T>` requires, because `Array<T>` slots store borrows rather than owned values. `AsSpan` returns the writable form by default (matching `Array<T>`'s mutable nature); callers wanting read-only access either call `AsReadOnlySpan` directly or call `.AsReadOnly()` on the writable form. The four overload signatures of each form accept range parameters in the same style as v0.104's `string.AsSpan()`: no-arg (full used range, `0..<Count`), `(start)` (from offset to end), `(start, length)`, or `(r: Range<int>)`. Bounds-check semantics follow the standard Bezel convention: `IndexOutOfRangeException` if `start` or `length` is out of range against the array's `Count`.

<a id="sec-ownedarrayt"></a>
### 4.5 `OwnedArray<T>`

Owning resizable array. Owns its elements; disposal cascades through them.

```bezel
public class OwnedArray<T>: IOwnedList<T> where T: class
  public constructor();
  public constructor(capacity: int);

  public readonly property Count: int;
  public readonly property Capacity: int;

  public property [i: int]: T
    get;                                     // borrow of slot
    set(owned);                              // takes owned T; disposes previous value
  end;

  public method Add(owned item: T);
  public method AddRange(owned items: OwnedArray<T>);   // drains items into self
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
  public method TryPopFirst(out owned item: T): bool;
  public method TryPopLast(out owned item: T): bool;
  public method Drain(): OwnedEnumerable<T>;             // iteration drains the source
end OwnedArray;

public extension OwnedArray<T> where T: class, IComparable<T>
  public method Sort();
end;
```

`OwnedArray<T>` owns each element. `Add`, `Insert`, and the indexer setter take ownership; `RemoveAt`, `Remove`, `Clear`, and the array's own `Dispose()` dispose the affected elements. `Sort`'s comparer parameter takes borrows of T (the comparer doesn't consume; it inspects).

The indexer getter produces a borrow of the slot's owned element; the borrow is valid until the slot is mutated (by indexer set, RemoveAt, Clear, or any operation that disposes the slot's contents). To take an element out, use `PopAt(i)` (returns owned T and removes the slot) or `move arr[i]` (returns owned T and leaves the slot in moved-from state; the caller must reassign before the next exit point — see language spec [§28.8.1](Bezel_Language_Spec.md#sec-the-move-expression)).

`Drain()` returns an `OwnedEnumerable<T>` ([§4.13](#sec-owning-enumeration-interfaces)) whose iteration transfers ownership of each element to the consumer. A `for var x in arr.Drain() do ... end` loop has owned iteration variables and consumes the array. Falling off the loop body without discharging each iteration variable is a compile error.

There is no `ToArray()` on `OwnedArray<T>` that produces a non-owning copy: copying the array would manufacture duplicate ownership of each element. To produce a fresh `OwnedArray<T>` with the same elements, the source array must be drained — the original ceases to own its elements.

`OwnedArray<T>` does not declare `DisposeElements()` — disposing the array automatically disposes every element. The method would be redundant.

A collection literal `[c1, c2, c3]` with target type `OwnedArray<Connection>` constructs an owning array and transfers ownership of each element in:

```bezel
public method Build(): owned OwnedArray<Connection>
  return [
    Connect("a"),                          // each Connect returns owned Connection
    Connect("b"),
    Connect("c")
  ];                                       // collection literal targets OwnedArray<Connection>
end Build;
```

<a id="sec-ownedarrayt-reference-implementation"></a>
<a id="sec-reference-implementation"></a>
#### Reference implementation

`OwnedArray<T>` composes over `OwnedBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)) in pure Bezel; the standard library provides this implementation directly, and user code is welcome to inspect or extend it.

```bezel
public class OwnedArray<T>: IOwnedList<T> where T: class
  private owned _storage: OwnedBuffer<T>;
  private _count: int := 0;

  public constructor()
    this(8);
  end constructor;

  public constructor(capacity: int)
    _storage := new OwnedBuffer<T>(if capacity > 0 then capacity else 1);
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
    var fresh := new OwnedBuffer<T>(newCapacity);
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
end OwnedArray;
```

The implementation is straightforward Bezel: a class with an owned `OwnedBuffer<T>` field, a count, and the standard ownership-transferring operations. The `move`-and-null pattern from [§28.8.1](Bezel_Language_Spec.md#sec-the-move-expression) of the language spec discharges every slot before reassignment; the compiler verifies the field-restoration rule statically.

`GrowTo` is the resize operation. It allocates a fresh buffer, moves every live element from the old buffer to the new one (nulling the old slot after each move to satisfy the restoration rule), then disposes the old buffer. The old buffer's destructor sees only null slots and does no further work.

`Drain()`, `PopFirst()`, `PopLast()`, and the rest follow the same pattern: take from a slot via `move`, restore the slot with `null`, adjust `_count`. None of the operations require compiler-intrinsic knowledge of `OwnedArray<T>` itself — the intrinsic surface is confined to `OwnedBuffer<T>`.

The implementation example above omits some methods for brevity; the production standard library version provides the full surface listed in the class declaration above, all built over the same `OwnedBuffer<T>` primitive.

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

A `LinkedList<T>` may be constructed with a sequence literal (language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)); textual order is head-to-tail: `using var l: LinkedList<int> := [10, 20, 30];` produces a list whose `First` is `10` and `Last` is `30`. The owning form `OwnedLinkedList<T>` accepts the same literal with element expressions producing owned values.

`List<T>` is not defined in this version. The name is reserved.

<a id="sec-ownedlinkedlistt"></a>
### 4.7 `OwnedLinkedList<T>`

Owning doubly-linked list.

```bezel
public class OwnedLinkedList<T>: IOwnedCollection<T> where T: class
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
  public method TryPopFirst(out owned item: T): bool;
  public method TryPopLast(out owned item: T): bool;
  public method Drain(): OwnedEnumerable<T>;
end OwnedLinkedList;
```

`LinkedListNode<T>` (declared in [§4.6](#sec-linkedlistt)) is reused. On `OwnedLinkedList<T>`, the node's `Value` property returns a borrow of the node's element; the node still owns the element until the node is removed. To take an element out, use `RemoveNode` (which disposes), `PopFirst` / `PopLast` (transfer ownership to caller), or `Drain()`.

The `move` operator does not extend to linked-list nodes in this version — removing a value from a specific node would leave the node in an inconsistent state, and the surrounding-node-pointer integrity makes the move-out-and-restore pattern impractical. Future revisions may add a node-level move form.

<a id="sec-dictionaryk-v"></a>
### 4.8 `Dictionary<K, V>`

Borrowing hash-based map. `K` must provide consistent `Equals` and `GetHashCode`.

```bezel
public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<K>);
  public constructor(capacity: int, comparer: IEqualityComparer<K>);

  public readonly property Count: int;
  public readonly property Keys: ICollection<K>;
  public readonly property Values: ICollection<V>;

  public property [key: K]: V;

  public method Add(key: K, value: V);
  public method ContainsKey(key: K): bool;
  public method Contains(key: K): bool;            // alias for ContainsKey;
                                                   //   enables `key in dict`
  public method Remove(key: K): bool;
  public method TryGetValue(key: K, out value: V): bool;
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

Indexer `dict[key]` raises `KeyNotFoundException` if the key is not present. Use `TryGetValue` for safe lookup.

Iteration over a `Dictionary<K, V>` yields entries in insertion order. Removing a key and re-adding it places it at the end of the iteration order. `Clear()` resets the order. `Contains(key)` is an alias for `ContainsKey(key)`, enabling the `in` operator (language spec [§17.5](Bezel_Language_Spec.md#sec-the-in-operator)) to test key membership: `key in dict` is equivalent to `dict.ContainsKey(key)`.

**Construction via collection literal.** A `Dictionary<K, V>` may be constructed with the dictionary-literal form `[k1: v1, k2: v2, ...]` (language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)):

```bezel
using var ages: Dictionary<string, int> := [
  "alice": 30,
  "bob":   25
];
```

The key expressions have type `K`, the value expressions type `V`. Literal element order is the insertion order, which is observable because iteration is insertion-ordered. Duplicate keys that are all compile-time constants are a compile error; otherwise the last occurrence wins. The empty dictionary is `[]` with a `Dictionary<K, V>`-typed target.

<a id="sec-owneddictionaryk-v"></a>
### 4.9 `OwnedDictionary<K, V>`

Owning hash-based map. Owns its values; keys are value-like (typically `string`, `int`, or a record with value equality). The typical owning-map form.

```bezel
public class OwnedDictionary<K, V>: IDisposable where V: class
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
  public method TryGetValue(key: K, out value: V): bool;       // out is borrow
  public method GetOrAdd(key: K, factory: (K) -> owned V): V;  // returns borrow

  // Drain
  public method PopValue(key: K): owned V;     // raises if not present
  public method TryPopValue(key: K, out owned value: V): bool;
  public method Drain(): OwnedEnumerable<DrainedEntry<K, V>>;
                                               // iteration drains the dictionary;
                                               //   each yielded entry owns its Value

  public method Clear();                        // disposes every value
end OwnedDictionary;

public class DrainedEntry<K, V>: IDisposable where V: class
  public constructor(key: K, owned value: V);  // ownership of value transfers in
  public readonly property Key: K;             // borrow / value copy of the key
  public owned Value: V;                        // owned — DrainedEntry is a class,
                                                //   so it may own its Value field
end DrainedEntry;
```

`OwnedDictionary<K, V>` requires `V: class` (the value type is owned). Keys can be any type with the standard equality contract.

The interface conformance is `IDisposable` only — not `IOwnedCollection<DictionaryEntry<K, V>>`. The `DictionaryEntry<K, V>` record (used by the borrowing `Dictionary<K, V>` in [§4.8](#sec-dictionaryk-v)) is a record, and records cannot have an owned `Value` field per language spec [§28.5](Bezel_Language_Spec.md#sec-owned-fields); a borrowing entry view over an owning dictionary would hand out a borrow of a value the dictionary still owns, which is the upcast-iteration behavior, not an ownership-transferring drain. For the ownership-transferring case, `Drain()` (below) yields `DrainedEntry<K, V>` — a *class*, which per the same [§28.5](Bezel_Language_Spec.md#sec-owned-fields) rule *may* own its `Value`. Default `for var entry in dict do` iteration is not provided; use either the key-iteration pattern (borrowing) or `Drain()` (ownership-transferring):

```bezel
// Borrowing key-iteration, one value taken at a time:
public method DrainAndProcess(dict: OwnedDictionary<string, Connection>)
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
public method DrainAndProcess2(dict: OwnedDictionary<string, Connection>)
  for var entry in dict.Drain() do
    using var e := entry;                      // entry is owned; bind to discharge
    Process(e.Key, e.Value);                   // e.Value is the owned Connection
  end;
  // dict is now empty; each entry (and its Value) disposed at loop-body end
end DrainAndProcess2;
```

`Drain()` returns `OwnedEnumerable<DrainedEntry<K, V>>` ([§4.13](#sec-owning-enumeration-interfaces)). Iterating it transfers ownership of each `DrainedEntry<K, V>` to the consumer; the entry is a class that owns its `Value`, so binding the iteration variable with `using var` (or otherwise discharging it) disposes both the entry shell and, via the standard owned-field cascade (language spec [§28.5](Bezel_Language_Spec.md#sec-owned-fields)), the contained value. Partial iteration drains only the entries yielded; the rest remain in the dictionary. This is the single-call drain; no language feature beyond the existing owning-class and `OwnedEnumerable<T>` machinery is required — `DrainedEntry<K, V>` owns its `Value` exactly as any class owns an `owned` field. The `Key` is carried by value (or as a borrow for class-typed K, following the standard field rules).

`GetOrAdd(key, factory)`'s factory produces an `owned V`; the dictionary stores the value (taking ownership) and the call returns a borrow of the now-stored value.

There is no `DisposeElements()` on `OwnedDictionary<K, V>` — the dictionary's `Dispose()` cascade handles every value disposal.

The `Keys` and `Values` properties return read-only borrowing views; their type is `IReadOnlyCollection<K>` and `IReadOnlyCollection<V>` (the unmodified types, since reading produces borrows). Code that needs to mutate the dictionary calls `Add`, `Remove`, etc. directly.

**Construction via collection literal.** An `OwnedDictionary<K, V>` may be constructed with the dictionary-literal form `[k1: v1, ...]`; each value expression must produce an `owned V`, and ownership transfers into the dictionary:

```bezel
using var pool: OwnedDictionary<string, Connection> := [
  "primary": Connect("p"),
  "backup":  Connect("b")
];
```

If a literal contains duplicate keys that are not all compile-time constants, the last value wins and each displaced earlier value is disposed at construction (consistent with the runtime last-write-wins rule). The empty owned dictionary is `[]` with an `OwnedDictionary<K, V>`-typed target.

`OwnedDictionary` does **not** support owned keys (`OwnedDictionary<owned K, V>` and similar forms are not provided). Realistic key types are value-like (string, int, records, etc.); cases for owned class-typed keys are rare to nonexistent in practice, and the v0.96-era four-way Add declaration that admitted them was contortion for a use case that didn't motivate the complexity.

<a id="sec-owneddictionaryk-v-reference-implementation"></a>
#### Reference implementation

`OwnedDictionary<K, V>` composes over `OwnedBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)) in pure Bezel. The implementation is more involved than `OwnedArray<T>`'s because mutation through a chain of borrowed pointers is incompatible with Bezel's borrow-lifetime tracker (see "Note on chain mutation" below); the implementation works in terms of *drain-and-rebuild* on each bucket.

The structure: each bucket of an `OwnedBuffer<Entry<K, V>>` holds an owned `Entry<K, V>` (or null), where each entry has an `owned Next: Entry<K, V>?` field forming a chain of synonym-hash entries. The `Value` field is declared nullable (`owned Value: V?`) so the `move`-and-null restoration pattern can be used during take operations; in committed state every entry's `Value` is non-null.

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

public class OwnedDictionary<K, V>: IDisposable where V: class
  private owned _buckets: OwnedBuffer<Entry<K, V>>;
  private _count: int := 0;
  private _comparer: IEqualityComparer<K>;

  // ... constructors, BucketIndex, Count, Keys, Values, etc.

  public method Add(key: K, owned value: V)
    var idx := BucketIndex(key);
    // Drain the bucket's chain so we can walk it as owned local state
    // (Bezel's borrow tracker rejects walks-with-mutation; see note below).
    // Reverse the chain as we walk; if we don't find the key, the new entry
    // is prepended to the reversed chain — chain reversal is unobservable
    // because OwnedDictionary doesn't promise insertion order.
    var chain := move _buckets[idx];
    _buckets[idx] := null;
    var rebuilt: Entry<K, V>? := null;
    var foundDup := false;
    while chain <> null do
      var rest := move chain.Next;
      chain.Next := rebuilt;          // restore (with the rebuilt-chain head)
      rebuilt := chain;                // ownership transfers in
      if not foundDup and _comparer.Equals(chain.Key, key) then
        foundDup := true;
      end;
      chain := rest;
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
        rebuilt := chain;
      end;
      chain := rest;
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
end OwnedDictionary;
```

The structural insight: **every bucket mutation is expressed as taking the chain head with `move`, walking the owned chain as local state, and storing a rebuilt chain back into the bucket**. No method mutates a chain through a borrow into shared storage. Every `move buf[i]` and `move entry.X` is followed by an explicit restoration (`buf[i] := newValue`, `entry.X := null` or `entry.X := newOwner`). The compiler verifies the field-restoration rule statically.

**Note on chain mutation.** A naïve implementation might walk the chain with a borrow `probe: Entry<K, V>?` and surgically modify links (`prev.Next := probe.Next`, etc.). Bezel's borrow-lifetime tracker rejects this pattern: `probe`'s borrow source is rooted in `_buckets[idx]`, and mutating any link along the chain — or mutating the slot itself — invalidates the borrow chain, after which subsequent reads of `probe.X` are borrow-after-mutation errors. The drain-and-rebuild pattern sidesteps the issue by walking the chain as owned local state. The cost is `O(chain length)` work per mutation rather than `O(1)`. In practice, chain lengths are bounded by the dictionary's load factor (typically ≤ 8), so the constant factor is small; for use cases where this cost is unacceptable, an open-addressing implementation can be substituted without changing the public API.

**Note on iteration order.** The drain-and-rebuild pattern reverses chain order on every bucket mutation. Iteration over `OwnedDictionary<K, V>` therefore yields entries in an order that depends on insertion-and-mutation history rather than purely on insertion order. The standard library makes no insertion-order guarantee for `OwnedDictionary` (in contrast to `Dictionary<K, V>` at [§4.8](#sec-dictionaryk-v), which does); programs that need ordered iteration use a different collection.

**Why no compiler intrinsic for `OwnedDictionary`.** None of the operations above require compiler-intrinsic knowledge of `OwnedDictionary<K, V>`. Every `move` goes through the field-restoration rule that the compiler enforces on `move buf[i]` and on `move entry.X`. The intrinsic surface of the owning-collection family is once again confined to `OwnedBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)).

<a id="sec-sett"></a>
<a id="sec-sett-and-ownedsett"></a>
### 4.10 `Set<T>` and `OwnedSet<T>`

Borrowing and owning hash-based sets. `T` must provide consistent `Equals` and `GetHashCode`.

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

public class OwnedSet<T>: IOwnedCollection<T> where T: class
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<T>);

  // IOwnedCollection<T> operations plus:
  public method TryAdd(owned item: T): bool;          // returns whether added
                                                       //   (false if duplicate;
                                                       //   passed item is disposed)
  // Drain
  public method Take(query: T): owned T;               // borrow-taking query;
                                                       //   returns owned matched
  public method TryTake(query: T, out owned removed: T?): bool;
  public method Drain(): OwnedEnumerable<T>;

  public method UnionWith(owned other: OwnedSet<T>);   // drains other into self;
                                                       //   duplicates disposed
  public method IntersectWith(other: IEnumerable<T>);  // disposes elements not in other
  public method ExceptWith(other: IEnumerable<T>);     // disposes elements in other
  public method IsSubsetOf(other: IEnumerable<T>): bool;
  public method IsSupersetOf(other: IEnumerable<T>): bool;
end OwnedSet;
```

`OwnedSet<T>` inherits `Add(owned item: T)` from `IOwnedCollection<T>` — this form returns `()` and silently disposes the item on duplicate. For callers that want to know whether the add succeeded, `TryAdd(owned item: T): bool` returns the insertion result; on `false` (duplicate), the passed-in item is disposed. The separate name avoids the return-type conformance issue ([§11.2](Bezel_Language_Spec.md#sec-interface-declaration) of the language spec requires exact return-type match on overrides; the borrowing `Set<T>` similarly inherits `Add(item: T)` returning `()` and provides `TryAdd(item: T): bool` for the bool-returning case).

`OwnedSet<T>.Add` is the case where the duplicate-rejection rule has an ownership consequence. When the caller passes `owned item` and the set already contains an equal element, `Add` returns `()` and *disposes the passed-in item* — the caller relinquished ownership at the call site, and the set has no use for the duplicate. Callers who need to react to the duplicate path should call `TryAdd` instead, or pre-check with `Contains`:

```bezel
if not pool.Contains(candidate) then
  pool.Add(candidate);     // ownership transfers
end;
// On the duplicate path, candidate retains ownership.
```

`Take(query: T)` finds the equal element in the set, removes it, and returns ownership. The query and the stored element are equal by the set's `Equals` rule but may be distinct instances. `TryTake` is the no-throw variant.

A `Set<T>` may be constructed with a sequence literal (language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)); duplicates (per `T`'s equality contract) are silently collapsed, the first textual occurrence retained: `using var s: Set<int> := [1, 2, 2, 3];` yields `{1, 2, 3}`. For `OwnedSet<T>`, each later duplicate's owned element is disposed at construction. The empty set is `[]` with a `Set<T>`-typed (or `OwnedSet<T>`-typed) target.

<a id="sec-stackt-and-queuet"></a>
<a id="sec-stackt-ownedstackt-queuet-and-ownedqueuet"></a>
### 4.11 `Stack<T>`, `OwnedStack<T>`, `Queue<T>`, and `OwnedQueue<T>`

LIFO and FIFO collections.

```bezel
public class Stack<T>: ICollection<T>
  public constructor();
  public method Push(item: T);
  public method Pop(): T;             // raises if empty
  public method Peek(): T;
  public method TryPop(out item: T): bool;
  public method TryPeek(out item: T): bool;
end Stack;

public class OwnedStack<T>: IOwnedCollection<T> where T: class
  public constructor();
  public method Push(owned item: T);
  public method Pop(): owned T;                   // raises if empty; transfers out
  public method Peek(): T;                         // borrow of top element
  public method TryPop(out owned item: T): bool;
  public method TryPeek(out item: T): bool;       // out is borrow
  public method Clear();                           // disposes every element
  public method Drain(): OwnedEnumerable<T>;
end OwnedStack;

public class Queue<T>: ICollection<T>
  public constructor();
  public method Enqueue(item: T);
  public method Dequeue(): T;
  public method Peek(): T;
  public method TryDequeue(out item: T): bool;
  public method TryPeek(out item: T): bool;
end Queue;

public class OwnedQueue<T>: IOwnedCollection<T> where T: class
  public constructor();
  public method Enqueue(owned item: T);
  public method Dequeue(): owned T;
  public method Peek(): T;                         // borrow of front element
  public method TryDequeue(out owned item: T): bool;
  public method TryPeek(out item: T): bool;       // out is borrow
  public method Clear();                           // disposes every element
  public method Drain(): OwnedEnumerable<T>;
end OwnedQueue;
```

For class T on the borrowing forms, `Pop`, `Peek`, and `Dequeue` produce borrows whose source is the same as the source of whatever was pushed or enqueued; the stack and queue are transit points, not sources. Storing the popped value in a long-lived field of another object is a borrow-lifetime violation per language spec [§28.6](Bezel_Language_Spec.md#sec-leak-prevention).

On the owning forms, `Pop` and `Dequeue` return ownership; the collection no longer holds the element. This resolves the borrow-source story cleanly: when the stack owns its elements, the popped element transfers ownership to the caller with the standard discharge obligation.

**Construction via collection literal.** All four types accept a sequence literal (language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)). The ordering convention matters and differs between stack and queue:

- `Queue<T>` / `OwnedQueue<T>`: the first literal element is at the front (dequeued first). `using var q: Queue<int> := [1, 2, 3];` dequeues `1`, `2`, `3` in that order.
- `Stack<T>` / `OwnedStack<T>`: the literal lists elements bottom-to-top — the first element is pushed first (ends up at the bottom), the last element is on top (popped first). `using var s: Stack<int> := [1, 2, 3];` pops `3`, `2`, `1` in that order.

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
public interface OwnedEnumerable<T>: IDisposable where T: class
  method GetOwnedEnumerator(): owned OwnedEnumerator<T>;
end OwnedEnumerable;

public interface OwnedEnumerator<T>: IDisposable where T: class
  readonly property Current: owned T;   // destructive: transfers ownership
  method MoveNext(): bool;
end OwnedEnumerator;
```

The owning iteration interfaces. `OwnedEnumerator<T>.Current` is **destructive**: the first read after a successful `MoveNext` transfers ownership of the most-recently-yielded value to the caller; a second read without an intervening `MoveNext` is undefined behavior (concrete enumerators typically throw `InvalidOperationException`). The for-loop desugaring at language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) reads `Current` exactly once per iteration, matching the destructive semantics naturally.

A `for var x in src do ... end` where `src` is an `OwnedEnumerable<T>` produces owned iteration variables. The loop body must discharge each `x` on every normal-exit edge (fall-through, `break`, `continue`, `return`); the standard owned-local discharge rules ([§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)) apply. On exception edges the iteration variable is auto-disposed by the implicit `try`/`finally` rule of [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) "Exception-path discharge."

These interfaces are auto-imported alongside the borrowing iteration family.

The async variants:

```bezel
public interface OwnedAsyncEnumerable<T>: IAsyncDisposable where T: class
  method GetOwnedAsyncEnumerator(token: CancellationToken := CancellationToken.None): owned OwnedAsyncEnumerator<T>;
end OwnedAsyncEnumerable;

public interface OwnedAsyncEnumerator<T>: IAsyncDisposable where T: class
  readonly property Current: owned T;   // destructive
  method MoveNextAsync(): Task<bool>;
end OwnedAsyncEnumerator;
```

`OwnedAsyncEnumerable<T>` is the return type for async iterator methods that yield owned values (asynchronously-produced owned resources, async drain operations on owning collections). The destructive-Current rule applies to `OwnedAsyncEnumerator<T>` the same way; `await for var x in src do ... end` produces owned iteration variables that must be discharged in the body.

A type may implement both `IEnumerable<T>` and `OwnedEnumerable<T>`. `OwnedArray<T>` does so: default iteration via `IEnumerable<T>` yields borrows of T (the standard read-only-iteration semantics); drain iteration via `OwnedEnumerable<T>` yields owned T values that the consumer must discharge. The for-loop desugaring picks based on the static type of the source expression: `for var x in arr do` desugars via `IEnumerable<T>` (borrowing); `for var x in arr.Drain() do` desugars via `OwnedEnumerable<T>` (owning).


<a id="sec-owned-buffers-and-spans"></a>
<a id="sec-ownedbuffert-spant-readonlyspant"></a>
### 4.14 `OwnedBuffer<T>`, `Span<T>`, `ReadOnlySpan<T>`

A runtime-sized storage primitive and two non-owning view types built over it. Together they are the foundation over which the resizable owning collections (`OwnedArray<T>`, `OwnedDictionary<K, V>`, etc.) compose, and they give user code a safe way to express "a fixed-but-runtime-sized buffer of slots" without any `unsafe` mechanism.

The slot semantics dispatch on T's category:

- **Class-typed T** (classes, interfaces): slots hold `T?` (nullable references) and the buffer owns each non-null slot's referent. Setter takes `owned T?` and disposes the previous value before storing the new one. `move buf[i]` is admitted; the field-restoration rule applies. The buffer's destructor cascades through slots, disposing every non-null one. This is the v0.99 design and is unchanged in v0.102.
- **Value-typed T** (primitives, records without owned fields, enums, non-move-only inline arrays): slots hold `T` directly (no nullability wrapper). Setter takes `T` and simply overwrites the slot. `move buf[i]` is *not* admitted (values have no ownership to transfer; reading is sufficient). The buffer's destructor releases its backing storage but performs no per-slot disposal.

The class-typed semantics were the v0.99–v0.101 design; v0.102 generalizes by adding the value-typed case. The two categories share the same `Length`, `Slice`, indexer-access, and other category-agnostic surface; they differ in the slot type, the setter modifier, and the admissibility of `move`.

**What is not admitted.** Runtime-managed reference types (`string`, function types) are not admitted as T — they have no disposal obligation, so the "owning" semantics add nothing, and they are stored more naturally in borrowing collections (`Array<string>`, `Array<(int) -> int>`). Move-only value types (currently only the owning inline arrays `[N owned]T` and `[N owned]T?`, [§15.3](Bezel_Language_Spec.md#sec-move-only-value-types) of the language spec) are also not admitted in v0.102 — the slot semantics for buffers of move-only types require additional design work and are deferred (see [§18](Bezel_Design_Rationale.md#sec-things-deferred) in the rationale).

<a id="sec-ownedbuffert"></a>
#### `OwnedBuffer<T>`

The declaration's surface depends on T's category. For class-typed T:

```bezel
public class OwnedBuffer<T>: IEnumerable<T?>            // class T only
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
end OwnedBuffer;
```

For value-typed T:

```bezel
public class OwnedBuffer<T>                              // no IEnumerable conformance
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

  // Span views (v0.109)
  public method AsSpan(): Span<T>;                          // full buffer, writable
  public method AsSpan(start: int): Span<T>;                // from offset to end
  public method AsSpan(start: int, length: int): Span<T>;
  public method AsSpan(r: Range<int>): Span<T>;             // parallels Range slicing

  public method AsReadOnlySpan(): ReadOnlySpan<T>;
  public method AsReadOnlySpan(start: int): ReadOnlySpan<T>;
  public method AsReadOnlySpan(start: int, length: int): ReadOnlySpan<T>;
  public method AsReadOnlySpan(r: Range<int>): ReadOnlySpan<T>;
end OwnedBuffer;
```

The two presentations are different specializations of the same generic class. The compiler resolves T's category at instantiation and emits the appropriate slot semantics. Users see the form corresponding to the T they instantiate.

**Class-typed T (unchanged from v0.99).** `OwnedBuffer<T>` is a class with fixed-at-construction storage of N slots, each holding `T?` (a nullable reference). Once allocated, `Length` is immutable. Non-null slots are owned by the buffer; the buffer's destructor disposes every non-null slot in implementation-defined order. The single-argument constructor produces a buffer with all slots initialized to `null`. The two-argument constructor calls `init(i)` for each `i` in `0..<length`, taking ownership of each returned value. The indexer's setter takes ownership of the assigned value and disposes whatever the slot previously held. `move buf[i]` is admitted and produces an owned `T?` value, leaving the slot in moved-from state; the field-restoration rule of [§28.8.1](Bezel_Language_Spec.md#sec-the-move-expression) applies. The natural restoration for a nullable slot is `buf[i] := null`.

**Value-typed T (new in v0.102).** `OwnedBuffer<T>` is a class with fixed-at-construction storage of N slots, each holding `T` directly (no nullability wrapper). Once allocated, `Length` is immutable. The single-argument constructor requires T to have a default value (every primitive does; records with default-able fields do; enums with a zero-equivalent member do; user-defined types without defaults do not). It initializes every slot to `default(T)`. For types without a default, only the two-argument constructor with an explicit init function is admitted. The two-argument constructor calls `init(i)` for each `i` in `0..<length`, copying the returned value into the slot. The indexer's setter overwrites the slot's previous value with the new one — no disposal is performed (values are not owned). `move buf[i]` is not admitted; values are read with the plain indexer and copied at the use site.

The "Owned" in the name refers to the buffer-class's ownership of *its allocated storage*, not to slot-level ownership. For class-typed T, the buffer also owns its slot contents (the ownership semantics that motivated the name). For value-typed T, the buffer owns only its allocation; the slot contents are just data. In both cases the buffer is `IDisposable`: its destructor releases the allocation. The name is retained for continuity with the v0.99 design and for the class-typed case where slot-ownership remains the dominant concern.

**Span views (v0.109).** `AsSpan()` and its overloads return a `Span<T>` (writable view) over a contiguous range of the buffer; `AsReadOnlySpan()` returns a `ReadOnlySpan<T>`. These views are non-allocating record constructions and admit the standard span surface (`Slice`, indexer, search, equality, bulk mutation per T's category). The methods are pure ergonomics — `buf.AsSpan(0, 100)` reads more naturally than `new Span<T>(buf, 0, 100)` and matches the established pattern from v0.104's `string.AsSpan()` and v0.107's `Array<T>.AsSpan()`. `AsSpan` returns the writable form by default (matching the buffer's mutability); callers wanting read-only access either call `AsReadOnlySpan` directly or call `.AsReadOnly()` on the writable form. The four overload signatures of each form accept range parameters: no-arg (full buffer, `0..<Length`), `(start)` (from offset to end), `(start, length)`, or `(r: Range<int>)`. Bounds-check semantics follow the standard Bezel convention: `IndexOutOfRangeException` if `start` or `length` is out of range against the buffer's `Length`.

**Iteration.** For class-typed T, `OwnedBuffer<T>` implements `IEnumerable<T?>`; iteration yields borrows of `T?` (slot contents, possibly null). The `for var x in buf do ... end` form works both via the enumerator protocol and via the direct-indexed-loop optimization that [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) permits for concretely-typed sources. For value-typed T, `OwnedBuffer<T>` does *not* implement `IEnumerable<T>` (the type-parameter-category dispatch would require conditional interface implementation, which Bezel does not currently provide as a general mechanism). `for var x in buf do ... end` over a value-typed OwnedBuffer desugars to a direct indexed loop per the [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) extension; the iteration variable has type T and is a copy of the slot. Users who need IEnumerable<T> conformance on a value-typed buffer can wrap or convert.

**Why this is a compiler-known primitive.** The `OwnedBuffer<T>` class needs intrinsic-level support primarily for one operation: **allocation of N slots where N is a runtime constructor parameter**. There is no user-level Bezel syntax for "a class field holding a runtime-determined-size array of slots"; inline arrays `[N owned]T?` and `[N]T` require compile-time N. The remaining surface (the indexer's interaction with the `move` expression form for class T, the destructor's per-slot disposal pass for class T, the iteration support) reuses mechanisms that `OwnedArray<T>` and similar collections already exercise, just operating on the runtime-sized backing. The intrinsic boundary of the owning-collection family shrinks to this one class.

**Disposal.** For class-typed T, the buffer's destructor disposes every non-null slot. Slot-disposal order within a single buffer is implementation-defined. For value-typed T, the destructor releases the backing storage but performs no per-slot work. Both cases satisfy the `IDisposable` contract; both are non-trivial-dispose ([§28.9](Bezel_Language_Spec.md#sec-runtime-managed-types) of the language spec) in the sense that the user must discharge the buffer (typically via `using var` or by storing it as an `owned` field of a longer-lived class).

**Borrow-bearing slots.** When T is a class type whose instances themselves carry borrows from external sources (a class with a borrow-bearing record field, an interface backed by such a class, or any class whose fields include reachable borrows), the buffer participates in the same borrow-lifetime tracking as any other class-typed storage: the buffer may not outlive any source from which a slot borrows. The compiler tracks slot-borrow sources element-wise where possible and falls back to whole-buffer tracking when slot-borrow sources cannot be statically separated. For value-typed T whose values may themselves carry borrows (records with borrow-bearing fields), the same rule applies to the values stored in slots.

<a id="sec-spant"></a>
#### `Span<T>`

The declaration's surface, like `OwnedBuffer<T>`'s, depends on T's category.

For class-typed T:

```bezel
public record Span<T>(Buffer: OwnedBuffer<T>, Start: int, Length: int): IReadOnlySpan<T?>
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
public record Span<T>(Buffer: OwnedBuffer<T>, Start: int, Length: int): IReadOnlySpan<T>
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

A `Span<T>` is a borrow-bearing record ([§9.7](Bezel_Language_Spec.md#sec-records-and-disposal) of the language spec) viewing into a contiguous range of an `OwnedBuffer<T>`. The `Buffer` field carries a borrow of an `OwnedBuffer<T>`; per the borrow-bearing-record rule, the span's lifetime is bounded by the borrow's source, so a span cannot outlive the buffer it views.

`Span<T>` is purely user-level Bezel — no compiler intrinsic. The record machinery, borrow lifetime tracking, and indexer dispatch are all standard language features.

**Read access.** For class T: `span[i]` returns a borrow of the slot's value (which is `T?` because the underlying buffer's slots are nullable). For value T: `span[i]` returns a borrow of the slot (`T`, non-nullable). The borrow's source is the underlying `OwnedBuffer<T>` (transitively reached through the span's `Buffer` field). For class T with potentially-null slots, programs that view into a buffer known to have non-null contents at every slot can use `span[i]!` for the null-forgiving access, raising `NullReferenceException` if the slot is null.

The indexer bounds-checks against the span's `Length` (not just the underlying buffer's length) as of v0.105. If `i < 0` or `i >= Length`, `IndexOutOfRangeException` is thrown. The check fires even when `Start + i` would be in the underlying buffer's range — the point of the span's range restriction is to enforce that callers cannot read outside the slice the span advertises. Earlier revisions (v0.99-v0.104) silently delegated to the buffer's indexer, which only checked against the buffer's length; this was a latent correctness bug, fixed in v0.105.

**Write access.** For class T: `span[i] := value` takes ownership of the assigned value (per the `set(owned)` setter) and stores it in the underlying buffer slot. The underlying buffer's owned-setter disposes whatever the slot previously held; the new value replaces it. `move span[i]` is also admitted: it takes ownership of the slot's current contents (an owned `T?`) and leaves the slot moved-from; the field-restoration rule of [§28.8.1](Bezel_Language_Spec.md#sec-the-move-expression) applies, and the natural restoration is `span[i] := null`. The setter and `move` form bounds-check against the span's `Length` (same rule as the getter, applied as of v0.105).

For value T: `span[i] := value` simply overwrites the slot with the new value. No ownership transfer happens; no previous value is disposed. `move span[i]` is *not* admitted — values are read with the plain indexer and copied at the use site. Writing through a value-typed span is mechanically the same as writing through a value-typed buffer directly.

A span carries the full read/write capability of the underlying buffer for its viewed range; what distinguishes it from holding the whole buffer is *range restriction*, not access mode. Programs that want a read-only view use `ReadOnlySpan<T>` instead.

`Slice(start, length)` produces a sub-span that views into the same buffer. The slice's lifetime is the same as the original span's — both are borrows of the same source. The slice has the same access mode as its parent (a `Span<T>` slices to a `Span<T>`; a `ReadOnlySpan<T>` slices to a `ReadOnlySpan<T>`).

`AsReadOnly()` returns a `ReadOnlySpan<T>` viewing the same buffer range. Bezel has no implicit record-to-record conversions, so passing a `Span<T>` where a `ReadOnlySpan<T>` is expected requires the explicit call. This is friction the C# precedent does not have (C# admits an implicit conversion from `Span<T>` to `ReadOnlySpan<T>`); Bezel's choice trades that convenience for the broader principle that record-type identity is nominal and conversions between records are always explicit. The `AsReadOnly()` call is cheap (record construction, no allocation) and makes the API-boundary intent visible at the call site.

**Iteration.** `for var x in span do ... end` is admitted for any T. The desugaring is a direct indexed loop over the span's range, parallel to the inline-array rule of [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) of the language spec: spans do not implement `IEnumerable<T?>` (a record carrying a borrow cannot be boxed into a heap-bound interface reference without losing the borrow source), so the standard enumerator path does not apply. The iteration variable's type follows the slot type: `T?` for class T, `T` for value T. For class T, the loop body sees a borrow of each slot's value; for value T, it sees a borrow of the slot (which, for primitive T, is effectively the value). To consume slot contents inside the loop body for class T, use `move span[i]` followed by an explicit assignment to restore the slot.

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
public record ReadOnlySpan<T>(Buffer: OwnedBuffer<T>, Start: int, Length: int): IReadOnlySpan<T?>
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
public record ReadOnlySpan<T>(Buffer: OwnedBuffer<T>, Start: int, Length: int): IReadOnlySpan<T>
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

**Why not stack-allocated like C#'s `Span<T>`?** C#'s `Span<T>` is a `ref struct` — stack-only, cannot escape to the heap. This gives it the strongest possible lifetime guarantees but constrains where it can appear (no `Span<T>` fields in classes, no `Span<T>` in async state machines, no `Span<T>` returned across await points). Bezel's `Span<T>` is an ordinary record subject to borrow-bearing-record rules; it can be stored anywhere the borrow's lifetime allows, including as a class field when the field's enclosing class outlives the borrow source, or across `await` points in async methods when the underlying buffer's lifetime extends past the suspension. Programs whose buffer lifetimes cannot be statically proven sufficient are rejected by the borrow tracker. The Bezel choice trades the unconditional stack-only safety of C# for a much smaller language footprint — no new "ref struct" type kind is introduced. The lifetime tracker handles the common cases (parser scratch buffers, byte-buffer reads, slice-and-walk patterns, async pipelines whose buffer outlives the awaits) without a special type kind.

**Construction patterns.** A buffer's owner produces spans by handing out `new Span<T>(buf, start, length)` or `new ReadOnlySpan<T>(buf, start, length)` directly, or by exposing helper methods:

```bezel
public class FrameBuffer
  private owned _pixels: OwnedBuffer<Pixel>;
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

`TryRead*` variants return `false` instead of throwing and assign the `out` parameter to `default(T)` on the false path (per the language spec [§22.3](Bezel_Language_Spec.md#sec-parameter-modifiers) requirement that every `out` parameter be assigned on every exit path). They suit incremental parsers that accept short inputs as end-of-stream rather than as errors.

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

`ArraySpan<T>` is the writable form; `ReadOnlyArraySpan<T>` is the read-only form. The split parallels `Span<T>` / `ReadOnlySpan<T>` ([§4.14](#sec-owned-buffers-and-spans)) but with `Array<T>` as the source instead of `OwnedBuffer<T>`. The two span families coexist; they are not interchangeable (a method that takes `Span<T>` does not accept `ArraySpan<T>` and vice versa) because the underlying storage models differ — `OwnedBuffer<T>` owns its slots for class T, while `Array<T>` borrows them.

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

**Read access.** `span[i]` returns the value at position `Start + i` in the source array. The return type follows the array's element type — `T` is whatever the array was instantiated with (a class type for `Array<MyClass>`, a nullable class type for `Array<MyClass?>`, a value type for `Array<int>`). Unlike `Span<T>` over `OwnedBuffer<T>`, there is no `T?` wrapping; the source array does not store nullable wrappers for class T (the array stores borrows directly, and a non-nullable `Array<MyClass>` has non-null borrow slots).

**Write access.** `span[i] := value` stores `value` into the source array's slot at `Start + i`. For class T (whether nullable or not), the assigned value is a borrow — no ownership transfer; the previous slot contents are not disposed (they were borrows, not owned values). For value T, the value is copied into the slot. The setter takes the value with the default parameter convention (no `owned` modifier); this differs from `Span<T>`'s class-T setter, which takes `owned T?` because the underlying `OwnedBuffer<T>` owns its slots.

`move arrSpan[i]` is **not admitted** for any T. Borrowed slots have no ownership to transfer; the plain indexer-get is the only read form. This differs from class-typed `Span<T>` (which admits `move` because `OwnedBuffer<T>` slots are owned).

**Bounds-check semantics.** The indexer bounds-checks against the span's `Length` (not just the underlying array's `Count`); `IndexOutOfRangeException` is thrown if `i < 0` or `i >= Length`. The same rule applies to the setter. This matches the v0.105 bounds-check rule for `Span<T>`/`ReadOnlySpan<T>` and the v0.104/v0.106 rules for the string-backed spans.

**Iteration.** `for var x in arrSpan do ... end` is admitted. The desugaring is a direct indexed loop over the span's range, parallel to the OwnedBuffer-backed span rule of language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for). The iteration variable's type is T and the loop body sees a borrow of each slot (the borrow's source is the underlying array, reached through the span's `Source` field).

**Search and equality.** `IndexOf` walks the span's range, returning the first index whose slot equals `value`, or `-1` if none. `Contains` returns `IndexOf(value) >= 0`. The comparison uses `Equals` per the standard convention. `SequenceEqual(other: ArraySpan<T>)` compares two writable array spans pairwise; the `ReadOnlyArraySpan<T>` overload supports comparison across the access-mode boundary without requiring an `AsReadOnly()` call at the comparison site.

**Bulk mutation.** Unlike `Span<T>` (whose mutation API splits by T's category — `Clear`/`MoveFrom`/`MoveTo` for class T owning slots, `Fill`/`CopyFrom`/`CopyTo` for value T), `ArraySpan<T>` provides a single uniform mutation surface for all T: `Fill(value)` assigns `value` to every slot; `CopyFrom(source)` copies slot-by-slot from a source span (lengths must match, else `ArgumentException`); `CopyTo(destination)` copies slot-by-slot to a destination span (lengths must match); `Reverse()` reverses the slot ordering in place. The uniform API is admissible because `Array<T>` slots do not own their contents — there is no ownership transfer to worry about, no dispose-previous semantics, no need for the category-dependent split. For class T, the assigned-into slot stores a borrow (`Fill(myClass)` makes every slot borrow `myClass`); for value T, the slot stores a copy. Both fit naturally under `Fill`. `CopyTo` was added in v0.109 alongside the broader v0.109 ergonomics pass.

**No in-place modification of value slots.** Bezel has no ref returns ([§22.14](Bezel_Language_Spec.md#sec-no-params-no-ref-returns)), so `arrSpan[i].Field := x` (modifying a value-type slot in place through the span) is not available. The pattern is `var v := arrSpan[i]; v.Field := x; arrSpan[i] := v;` — read-modify-write through assignment. This limitation applies equally to `Array<T>` itself; the span inherits it.

**Construction.** Callers construct `ArraySpan<T>` via the `AsSpan()` family on `Array<T>` ([§4.4](#sec-arrayt)) rather than via the constructor directly. The direct constructor is admitted but is rarely needed.

**Caveat: array mutation during span lifetime.** Unlike `OwnedBuffer<T>` (which has a fixed `Length`), `Array<T>`'s `Count` can change during the array's lifetime via `Add`, `Insert`, `Remove`, `RemoveAt`, and `Clear`. A span's `Length` is fixed at construction; subsequent mutation of the underlying array does not update the span. If the array shrinks below `Start + Length` after the span was constructed, accesses through the span (which pass the span's bounds check but then index into the array beyond its new `Count`) will throw `IndexOutOfRangeException` from the array's own bounds check. If the array grows or its contents shift via `Insert` or `RemoveAt`, the span's `(Start, Length)` continues to point at the same logical positions in the array — which may now hold different elements than the span originally viewed. This is a real footgun and a structural difference from the `OwnedBuffer<T>`-backed spans (whose underlying buffer cannot resize). The intended usage pattern is short-lived spans: construct the span, pass it to a method that reads or writes through it, return — without resizing the source array in between. Programs that need a stable view across mutations should copy the relevant range into a fresh `OwnedBuffer<T>` or `Array<T>` instead.

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

An interface unifying the read-only access surface of every span type in the standard library. Implemented by all six concrete span types — `Span<T>`, `ReadOnlySpan<T>`, `ArraySpan<T>`, `ReadOnlyArraySpan<T>`, `ReadOnlyStringSpan`, `ReadOnlyAsciiByteSpan` — with the element type parameterized to match each concrete type's natural indexer return type.

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
| `Span<T>` for class T | `IReadOnlySpan<T?>` (OwnedBuffer slots are nullable) |
| `Span<T>` for value T | `IReadOnlySpan<T>` |
| `ReadOnlySpan<T>` for class T | `IReadOnlySpan<T?>` |
| `ReadOnlySpan<T>` for value T | `IReadOnlySpan<T>` |
| `ArraySpan<T>` | `IReadOnlySpan<T>` (Array slots are non-nullable T or nullable T?, whichever T was instantiated as) |
| `ReadOnlyArraySpan<T>` | `IReadOnlySpan<T>` |
| `ReadOnlyStringSpan` | `IReadOnlySpan<char>` |
| `ReadOnlyAsciiByteSpan` | `IReadOnlySpan<byte>` |

A method that wants to accept any read-only span of `MyClass` typically takes `IReadOnlySpan<MyClass?>` (which matches both OwnedBuffer-backed nullable-slot spans and Array-backed `MyClass?` spans). Code holding a non-nullable Array span (`ArraySpan<MyClass>`) cannot pass it to that parameter directly — it implements `IReadOnlySpan<MyClass>`, not `IReadOnlySpan<MyClass?>` — because the interface's TElement parameter is invariant. Authors who want full breadth across nullability provide two overloads (one for `IReadOnlySpan<T>`, one for `IReadOnlySpan<T?>`); authors who want the nullable form should use it consistently and accept that callers may need an explicit conversion at the boundary.

**Boxing and lifetime.** Spans are borrow-bearing records ([§9.7](Bezel_Language_Spec.md#sec-records-and-disposal) of the language spec). A span passed as an `IReadOnlySpan<TElement>` parameter is boxed per the standard rule for borrow-bearing records (language spec [§9.7.1](Bezel_Language_Spec.md#sec-boxing-of-borrow-bearing-records)): the boxed copy holds the same borrows as the original, the boxed value's lifetime is bounded by the source of every borrow it carries, and the compiler enforces that the boxed value does not escape the source scope. The interface parameter therefore admits any span whose source outlives the call frame — which is the common case for synchronous calls — without further annotation.

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

**SequenceEqual and the interface.** `SequenceEqual` *is* on the interface, with parameter type `IReadOnlySpan<TElement>`. This admits cross-source equality through the interface — `byteSpan.SequenceEqual(asciiSpan)` where one is a `ReadOnlySpan<byte>` and the other is a `ReadOnlyAsciiByteSpan` compiles when both are passed via `IReadOnlySpan<byte>`. The cost is two virtual-dispatched indexer reads per element; the convenience is real for comparison code that doesn't care which source type the operands came from.

**What about writable spans.** v0.108 does not introduce an `IMutableSpan` (or similar) writable interface. The writable side has substantive semantic differences across span families — `Span<T>`'s class-T setter takes `owned T?` and disposes previous slot contents; `ArraySpan<T>`'s setter takes a borrow with no disposal; value-T setters are plain copies. Unifying these under a single writable interface would either pick one model and exclude the others, or fork the contract by source kind, which defeats the unification. The read-only side is genuinely uniform: every span supports indexer-get returning the slot's element, and that's enough for one interface. The writable side may be revisited if a partial unification (e.g. covering only the non-owned-slot writable spans) proves valuable.

---

<a id="sec-system-collections-linq"></a>
## 5. `System.Collections.Linq`

LINQ-style extension methods on `IEnumerable<T>`. All are lazy unless documented otherwise.

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
  public method LastOrNull(): T?;
  public method Single(): T;                                         // throws if not exactly one
  public method SingleOrNull(): T?;
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

`First`/`Last`/`Single`/`ElementAt` throw `InvalidOperationException` (or `IndexOutOfRangeException` for `ElementAt`) on missing-element; the `*OrNull` variants return `null` instead.

`ToArray()` returns a fresh `Array<T>` containing the materialized sequence. The return is `owned` — the caller is responsible for disposing it. There is no `ToList()` because Bezel does not have a dynamic-array type named `List<T>` (`Array<T>` is the resizable form; `LinkedList<T>` is the linked form; use `new LinkedList<T>(source)` if a linked list is wanted).

`Sum` and `Average` are provided as per-type overloads rather than as a single generic constrained on a numeric interface. The reason is that a generic numeric constraint would require static interface members (so `Sum`'s body could reference `T.Zero` and `T.operator +`), and this version does not have that feature. The overload set covers the standard numeric primitives. Aggregating user-defined numeric types is done via `Aggregate(seed, fold)`.

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
  public method Register(callback: () -> ()): owned Subscription;
end CancellationToken;

public class CancellationTokenSource
  public constructor();

  public static method WithCancelAfter(delay: TimeSpan): owned CancellationTokenSource;

  public readonly property Token: CancellationToken;
  public readonly property IsCancellationRequested: bool;

  public method Cancel();
  public method CancelAfter(delay: TimeSpan);

  destructor;       // signals cancellation to any tokens still observing,
                           // then releases the registered-callback list
end CancellationTokenSource;
```

`Register` invokes the callback when cancellation is requested; the returned `Subscription` removes the registration when disposed.

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
  public constructor(message: string, inner: Exception);
end TimeoutException;
```

Raised by operations that exceed a deadline. Distinct from `OperationCanceledException`, which signals cancellation by an explicit `CancellationToken`; `TimeoutException` is for time-budget exhaustion where no token is involved. Library methods accepting a `TimeSpan` timeout typically raise this when the deadline elapses without the operation completing. Available without import, but only when `System.Threading` is imported — it is not in the auto-imported set.

<a id="sec-mutex-and-lock"></a>
### 6.2 `Mutex` and `Lock`

```bezel
public class Mutex
  public constructor();
  public method Acquire(): owned Lock;
  public method TryAcquire(timeout: TimeSpan): owned Lock?;
  public async method AcquireAsync(token: CancellationToken := CancellationToken.None): OwnedTask<Lock>;
  public async method TryAcquireAsync(timeout: TimeSpan, token: CancellationToken := CancellationToken.None): OwnedTask<Lock?>;
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

`AcquireAsync` and `TryAcquireAsync` are the async counterparts. `AcquireAsync` returns a `OwnedTask<Lock>` (Pattern B, single-consumer); `TryAcquireAsync` returns `OwnedTask<Lock?>` — the same nullable shape inside a Pattern B task. Consumers write:

```bezel
using var guard := await mutex.AcquireAsync();
// critical section; guard disposed at scope end.

using var maybe := await mutex.TryAcquireAsync(TimeSpan.FromSeconds(5));
if maybe <> null then
  // critical section
end;
```

The nullable-result shape replaces the `out owned guard` form used in earlier versions: the `out` parameter contract ([§22.3](Bezel_Language_Spec.md#sec-parameter-modifiers) of the language spec) requires the parameter to be assigned on every exit path, but the failure path of `TryAcquire` has no meaningful value to assign. Returning `owned Lock?` resolves the contradiction — the `null` value is itself the failure signal — and produces simpler call sites.

<a id="sec-semaphore"></a>
### 6.3 `Semaphore`

```bezel
public class Semaphore
  public constructor(initialCount: int);
  public constructor(initialCount: int, maxCount: int);

  public method Acquire(): owned SemaphoreToken;
  public method TryAcquire(timeout: TimeSpan): owned SemaphoreToken?;
  public async method AcquireAsync(token: CancellationToken := CancellationToken.None): OwnedTask<SemaphoreToken>;
  public async method TryAcquireAsync(timeout: TimeSpan, token: CancellationToken := CancellationToken.None): OwnedTask<SemaphoreToken?>;
end Semaphore;

public class SemaphoreToken
  destructor;       // releases the held permit
end SemaphoreToken;
```

`AcquireAsync` returns `OwnedTask<SemaphoreToken>`; awaiters consume the task under Pattern B (language spec [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)) — the await must appear in an owned-consuming context. The natural call site:

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

`Value`'s read and write are atomic with respect to other `Value` reads, `Value` writes, and the `Exchange`/`CompareExchange` methods on the same instance. The accessors are compiler-intrinsic; they are not auto-property storage.

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

`AtomicInteger<T>` is *not* a subtype of `Atomic<T>` — it is a separate class with the same load/store/CAS surface plus the arithmetic methods. The duplication is deliberate: a generic method that wants "any primitive atomic" should accept the relevant interface (none currently exists, since static interface members are deferred) or be overloaded for both classes; promoting `AtomicInteger<T>` to a subtype would imply substitutability that the type system cannot otherwise express.

`Increment`, `Decrement`, and `Add` perform the operation atomically and return the post-operation value. Overflow follows the standard integer-arithmetic rule ([§6.1](Bezel_Language_Spec.md#sec-integer-arithmetic) of the language spec): an operation whose mathematical result is not representable in `T` raises `OverflowException`.

<a id="sec-events-ievent-eventsource"></a>
### 6.5 Events: `IEvent`, `EventSource`

The types underlying `event` declarations (language spec [§24](Bezel_Language_Spec.md#sec-events)). The generic parameter `T` is the event's **payload type** — the type of value passed to handlers when the event fires.

For a synchronous event `event E: T`, the handler type is `(T) -> ()`. For an asynchronous event `async event E: T`, the handler type is `(T) -> Task`.

```bezel
public interface IEvent<out T>
  method Subscribe(handler: (T) -> ()): owned Subscription;
end IEvent;

public interface IAsyncEvent<out T>
  method Subscribe(handler: (T) -> Task): owned Subscription;
end IAsyncEvent;

public class EventSource<T>: IEvent<T>
  // No public constructor; obtained via event declaration.
  public method Subscribe(handler: (T) -> ()): owned Subscription;
  public method Raise(payload: T);

  public method Add(handler: (T) -> ());
  public method Remove(handler: (T) -> ());
end EventSource;

public class AsyncEventSource<T>: IAsyncEvent<T>
  // No public constructor; obtained via event declaration.
  public method Subscribe(handler: (T) -> Task): owned Subscription;
  public async method RaiseAsync(payload: T): Task;

  public method Add(handler: (T) -> Task);
  public method Remove(handler: (T) -> Task);
end AsyncEventSource;
```

Outside the declaring class, the static type of an event member is `IEvent<T>` (sync) or `IAsyncEvent<T>` (async); `Raise`, `RaiseAsync`, `Add`, and `Remove` are not on the interface, leaving `Subscribe` as the only entry point. Inside the declaring class, the static type is `EventSource<T>` (or `AsyncEventSource<T>`), where `Add`, `Remove`, `Raise`, and `Subscribe` are all available. The compiler-recognized syntactic forms `event += handler` and `event -= handler` rewrite to `event.Add(handler)` and `event.Remove(handler)` respectively. See language spec [§24.3](Bezel_Language_Spec.md#sec-the-type-of-an-event-member) / [§24.4](Bezel_Language_Spec.md#sec-subscribing).

The `Add`, `Remove`, and `Raise` methods are declared `public` rather than `internal` because the visibility restriction is enforced by the **type fence**: an `EventSource<T>` reference is obtainable only inside the declaring class (the field type from outside is the interface, not the class). User code that smuggles an `EventSource<T>` reference out of the declaring class — for example, by exposing it through a public method or by reflection — gains the ability to call `Raise` directly, but doing so is a deliberate breach of the event-pattern contract that the language does not attempt to defend against. The conventional design is to never expose the `EventSource<T>` itself; the public surface is always the interface.

`IEvent<T>` and `IAsyncEvent<T>` are covariant in T (`out T`): `IEvent<Dog>` is a subtype of `IEvent<Animal>`, the same direction as `IEnumerable<out T>`. An event source that fires `Dog` payloads can be exposed as an `IEvent<Animal>` to consumers; subscribers register `(Animal) -> ()` handlers, which accept any `Animal` (including `Dog`), and each fired `Dog` is passed in as an `Animal`. The variance check is sound because T appears once in `Subscribe(handler: (T) -> ())`, in the parameter position of `(T) -> ()` (contravariant) which is itself a method parameter (contravariant) — composing to a covariant position overall.

`EventSource<T>` and `AsyncEventSource<T>` (the producer-side classes) remain invariant because their `Raise(payload: T)` method places T in input position, which is incompatible with `out`. Since `EventSource<T>` is visible only inside the declaring class, the user-facing variance comes from `IEvent<out T>` exclusively.

For no-payload events (declared without a type, e.g. `public static event ApplicationExiting;`), the payload is `()`. `EventSource<()>` provides a convenience `Raise()` overload that takes no arguments, equivalent to `Raise(())`.

`Subscription` (the return type of `Subscribe`) is declared in [§2.11](#sec-subscription) and lives in the auto-imported `System` module. v0.76 moved it from `System.Threading` so that all register-and-unregister APIs returning a `Subscription` work without requiring an explicit threading-module import.

<a id="sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet"></a>
### 6.6 `TaskCompletionSource<T>` and `OwnedTaskCompletionSource<T>`

See [§30.11](Bezel_Language_Spec.md#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet) of the language spec.

```bezel
// For non-class T (value types, string, runtime-managed reference types).
// Instantiating with a class T is a compile error pointing to OwnedTaskCompletionSource<T>.
public class TaskCompletionSource<T>
  public constructor();
  public readonly property Task: Task<T>;

  public method SetResult(result: T);
  public method SetException(exception: Exception);
  public method SetCancelled();

  public method TrySetResult(result: T): bool;
  public method TrySetException(exception: Exception): bool;
  public method TrySetCancelled(): bool;
end TaskCompletionSource;

// For class T. Result is transferred via `owned` parameter; produced task is Pattern B.
public class OwnedTaskCompletionSource<T> where T: class
  public constructor();
  public readonly property Task: OwnedTask<T>;

  public method SetResult(owned result: T);
  public method SetException(exception: Exception);
  public method SetCancelled();

  public method TrySetResult(owned result: T): bool;
  public method TrySetException(exception: Exception): bool;
  public method TrySetCancelled(): bool;
end OwnedTaskCompletionSource;

// Non-generic form for void-completion tasks
public class TaskCompletionSource
  public constructor();
  public readonly property Task: Task;
  public method SetResult();
  public method SetException(exception: Exception);
  public method SetCancelled();
  public method TrySetResult(): bool;
  public method TrySetException(exception: Exception): bool;
  public method TrySetCancelled(): bool;
end TaskCompletionSource;
```

<a id="sec-task-static-helpers"></a>
### 6.7 `Task` static helpers

See [§30.10](Bezel_Language_Spec.md#sec-standard-library-task-helpers) of the language spec for the full list. They live on `Task` and `Task<T>`.

```bezel
public class Task
  // For non-class T (value types, string, runtime-managed reference types).
  // For class T, use FromOwnedResult instead.
  public static method FromResult<T>(value: T): Task<T>;

  // For class T. Produces a Pattern B task; awaiters consume under `using var x := await ...`.
  public static method FromOwnedResult<T>(owned value: T): OwnedTask<T> where T: class;

  // For any T: a faulted task never produces its result, so the Pattern A vs
  // Pattern B distinction does not apply. `FromException<MyClass>(ex)` is correctly
  // typed as Task<MyClass> even when MyClass is a class type — there is no value
  // to transfer ownership of, only an exception to re-raise on await.
  public static method FromException<T>(exception: Exception): Task<T>;
  public static method FromCancelled<T>(): Task<T>;

  public static method Delay(milliseconds: int): Task;
  public static method Delay(milliseconds: int, token: CancellationToken): Task;
  public static method Delay(duration: TimeSpan): Task;
  public static method Delay(duration: TimeSpan, token: CancellationToken): Task;

  public static method WhenAll(tasks: Array<Task>): Task;

  // For non-class T (value types, string, runtime-managed reference types).
  public static method WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>;

  // For class T. Aggregates OwnedTask<T> results into an owned array of owned elements.
  // The caller transfers ownership of the input array (and its tasks) to WhenAllOwned;
  // the returned task produces an owned OwnedArray<T>.
  public static method WhenAllOwned<T>(
    owned tasks: OwnedArray<OwnedTask<T>>
  ): OwnedTask<OwnedArray<T>> where T: class;

  public static method WhenAny(tasks: Array<Task>): Task<Task>;
  public static method FireAndForget(owned task: Task);
end Task;
```

`FireAndForget` registers a continuation that logs any exception the task produces via `Debug.WriteLine`, then disposes the task. The calling code relinquishes ownership (the parameter is `owned`); the runtime supervises the task's lifecycle.

The parameter type `Task` accepts any subtype: `Task`, `Task<T>`, and `OwnedTask<T>` all satisfy the parameter. For `OwnedTask<T>` (Pattern B), the runtime's task disposal cascades through any unconsumed owned result — `FireAndForget(someOwnedTask)` is semantically "let it run to completion; if it produces a result, dispose it; if it produces an exception, log it." This is the right pattern when the caller wants the underlying operation to complete its side effects (e.g., an async write whose `OwnedTask<WriteHandle>` would normally yield a handle, but the caller doesn't need the handle).

**`FireAndForget` is the only supported way to discard a `Task`** as of v0.66. The discard form `_ := someAsyncCall();` is rejected at compile time because `Task` has a destructor that handles continuation cleanup; the discard would skip it. The two legal patterns are:

```bezel
Task.FireAndForget(DoTelemetryAsync());      // supervised; exceptions logged
using var t := DoTelemetryAsync();           // unsupervised; disposed at scope end
```

`FireAndForget` is the right choice when the task should run to completion and any exceptions it raises should be logged. The `using var` form binds the task to a scope; at scope end, the task's `Dispose()` runs but does not block waiting for completion ([§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)). The underlying operation continues to run in the background; the caller no longer observes its completion or exceptions. Use `using var` only when "fire and forget without supervision" is exactly what's wanted; for tasks whose failures should be surfaced, use `Task.FireAndForget` or hold the task and `await` it explicitly.

The exception-logging mechanism for `FireAndForget` is implementation-defined; implementations may provide a global handler.

<a id="sec-fromexception-and-fromcancelled-for-class-t"></a>
#### `FromException` and `FromCancelled` for class T

These factories are correctly typed as `Task<T>` regardless of whether T is a class type. A faulted or cancelled task never produces a result value — `await` on such a task re-raises the original exception (`FromException`) or `OperationCanceledException` (`FromCancelled`). Because no value flows from producer to awaiter, the Pattern A vs Pattern B distinction that motivates `OwnedTask<T>` does not apply. `Task.FromException<FileStream>(ex)` produces `Task<FileStream>`, not `OwnedTask<FileStream>`; the awaiter never observes a `FileStream` instance and so has no ownership to discharge.

Code that needs an immediately-faulted task in a context expecting `OwnedTask<T>` should construct one through `OwnedTaskCompletionSource<T>` ([§6.6](#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet)):

```bezel
using var src := new OwnedTaskCompletionSource<MyClass>();
src.SetException(ex);
return src.Task;       // typed as OwnedTask<MyClass>
```

The TCS path is heavier than `FromException` and is only needed when the static return type of the calling method is `OwnedTask<T>`.

<a id="sec-whenall-for-class-t-whenallowned"></a>
#### `WhenAll` for class T: `WhenAllOwned`

The generic `WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>` aggregates results from non-class T (value types, `string`, runtime-managed reference types). For class T held by `OwnedTask<T>`, use `WhenAllOwned<T>`:

```bezel
public async method LoadAllConfigs(paths: Array<string>): OwnedTask<OwnedArray<Config>>
  var tasks := new OwnedArray<OwnedTask<Config>>();
  for var path in paths do
    tasks.Add(LoadConfigAsync(path));    // each task is OwnedTask<Config>; transfers in
  end;
  return await Task.WhenAllOwned(tasks); // tasks consumed; returned array owns the configs
end LoadAllConfigs;
```

The caller's pattern matches `WhenAll`: build an array of tasks, await `WhenAllOwned`, receive an array of results. The differences are in the type and ownership flow:

- The input array is `OwnedArray<OwnedTask<Config>>`, not `Array<OwnedTask<Config>>`. Each element is itself owned (OwnedTask<T> for class T is single-consumer per language spec [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal); the array owns its tasks for the duration).
- The input parameter is `owned OwnedArray<...>`: the caller transfers ownership of the array (and its contents) to `WhenAllOwned`. After the call, the caller no longer holds the array.
- The result is `OwnedTask<OwnedArray<Config>>`: a Pattern B task whose result is an owned array of owned configs. The caller awaits with `using var`, gaining scope-bound disposal of the whole structure.

Implementation sketch (illustrative, not part of the public contract):

```bezel
public static async method WhenAllOwned<T>(
  owned tasks: OwnedArray<OwnedTask<T>>
): OwnedTask<OwnedArray<T>> where T: class
  using var taskArray := tasks;                       // owned param to using var; disposes at end
  var results := new OwnedArray<T>(capacity := taskArray.Count);
  for var task in taskArray.Drain() do                // drain yields owned OwnedTask<T>
    results.Add(await task);                          // await produces owned T (Pattern B);
                                                      //   Add(owned item) is an owned-consuming
                                                      //   context, satisfying §30.5
  end;
  return results;                                     // ownership transfers to caller
end WhenAllOwned;
```

The implementation uses `OwnedArray<T>.Drain()` to take ownership of each task out of the input array, awaits each (Pattern B), and transfers the awaited result into the result array via `Add(owned item)`. The pattern works because every collection slot in the chain is an owned-element slot, and because `Add(owned item)` is an owned-consuming context that admits `await` directly per the loosened Pattern B rule (language spec [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)). The for-loop's iteration variable `task` is owned (per the new rule for iterators returning `OwnedEnumerable<T>`, language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) — "Iterator yield ownership"), and is consumed by the await on each iteration.

For genuine concurrency (the underlying tasks running in parallel), the caller should start the tasks *before* passing the array to `WhenAllOwned`. Each task's underlying operation begins when the task is constructed; awaiting blocks for completion. The `WhenAllOwned` body awaits sequentially, but the tasks themselves run in parallel because they were started earlier.

For producer-consumer pipelines that yield owned values incrementally, an async iterator remains the clean idiom. When the iterator's return type is `OwnedAsyncEnumerable<T>`, the consumer takes ownership of each yielded value, with the standard owned-discharge rules applying to the iteration variable inside the `await for` body (language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) — "Iterator yield ownership"):

```bezel
public async method OpenFiles(paths: Array<string>): OwnedAsyncEnumerable<FileStream>
  for var path in paths do
    yield return await OpenFileAsync(path);
  end;
end OpenFiles;

// Consumer takes ownership of each FileStream:
public async method ProcessAll(paths: Array<string>): Task
  await for var file in OpenFiles(paths) do
    using var f := file;      // discharge on entry — covers all exit edges
    await ProcessAsync(f);
  end;
end ProcessAll;
```

The `using var f := file;` shape on entry to the body is the canonical way to discharge the iteration variable, exactly as for synchronous owned-element iteration (language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for)).

The async-iterator form is preferable when the consumer processes each result before requesting the next; `WhenAllOwned` is preferable when the consumer needs all results at once.

---

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
  public abstract method Flush();

  public abstract method Seek(offset: long, origin: SeekOrigin): long;
  public abstract readonly property Length: long;
  public abstract property Position: long;

  public async method ReadAsync(buffer: Array<byte>, offset: int, count: int,
                                 token: CancellationToken := CancellationToken.None): Task<int>;
  public async method WriteAsync(buffer: Array<byte>, offset: int, count: int,
                                  token: CancellationToken := CancellationToken.None): Task;

  // Convenience methods
  public method ReadAll(): owned Array<byte>;            // reads from current position to end
  public async method ReadAllAsync(token: CancellationToken := CancellationToken.None): OwnedTask<Array<byte>>;
end Stream;

public enum SeekOrigin: byte
  Begin, Current, End;
end SeekOrigin;
```

<a id="sec-filestream"></a>
### 7.2 `FileStream`

```bezel
public class FileStream: Stream
  public constructor(path: string, mode: FileMode);
  public constructor(path: string, mode: FileMode, access: FileAccess);
  // Stream operations plus file-specific:
  public readonly property Path: string;
end FileStream;

public enum FileMode: byte
  CreateNew, Create, Open, OpenOrCreate, Truncate, Append;
end FileMode;

public enum FileAccess: byte
  Read, Write, ReadWrite;
end FileAccess;
```

<a id="sec-file"></a>
### 7.3 `File`

Static convenience methods.

```bezel
public static class File
  public static method ReadAllText(path: string): string;
  public static method WriteAllText(path: string, contents: string);
  public static method ReadAllBytes(path: string): owned Array<byte>;
  public static method WriteAllBytes(path: string, contents: Array<byte>);
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

`Directory` and `Path` are static utility classes following established conventions; full signatures are omitted here.

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

Records implement `IEquatable<T>` implicitly through the auto-generated equality (language spec [§9.2](Bezel_Language_Spec.md#sec-body-records)): `Equals(other: int): bool` exists on `Int32` and is invoked when calling `someInt.Equals(otherInt)` directly or when an `IEquatable<int>`-constrained generic uses `Equals` through the constraint. The same convention applies to every primitive-wrapper record in [§9.1](#sec-integer-types)–[§9.5](#sec-char).

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

  // Substring
  public method Substring(startIndex: int): string;
  public method Substring(startIndex: int, length: int): string;
  public readonly property [r: Range<int>]: string;       // slicing form (§4.6 of the language spec)

  // Span views (v0.104)
  public method AsSpan(): ReadOnlyStringSpan;             // full string as a span
  public method AsSpan(start: int): ReadOnlyStringSpan;   // from offset to end
  public method AsSpan(start: int, length: int): ReadOnlyStringSpan;
  public method AsSpan(r: Range<int>): ReadOnlyStringSpan; // parallels [r: Range<int>]: string

  // ASCII byte views (v0.106)
  // Throws InvalidOperationException if `not IsAscii`.
  public method AsAsciiByteSpan(): ReadOnlyAsciiByteSpan;
  // Returns false (and out value default) if `not IsAscii`.
  public method TryAsAsciiByteSpan(out value: ReadOnlyAsciiByteSpan): bool;

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

`Length` and `s[i]` are O(1) for every string (language spec [§4.6](Bezel_Language_Spec.md#sec-string)). The internal representation may be 1, 2, or 4 bytes per code point, selected per-string at construction; the choice is invisible to programmers but enables `IsAscii` for the common ASCII-only case. For char-level non-allocating substring views, use `AsSpan()` to obtain a `ReadOnlyStringSpan` ([§3.3](#sec-readonlystringspan)) — added in v0.104. For ASCII byte views — useful in FFI and I/O paths that need UTF-8 bytes without a per-call allocation — use `AsAsciiByteSpan()` to obtain a `ReadOnlyAsciiByteSpan` ([§3.4](#sec-readonlyasciibytespan)) — added in v0.106. The byte view is throwing-on-non-ASCII; a `TryAsAsciiByteSpan(out value: ReadOnlyAsciiByteSpan): bool` form is provided for fallible cases. When the string requires non-ASCII byte access (full UTF-8, allocating), use `Encoding.UTF8.GetBytes(s)` in `System.Text` ([§3.2](#sec-encoding)).

The `+` operator is declared explicitly here, not as compiler magic; the compiler resolves `s + t` to this overload like any other operator.

<a id="sec-arrayt-extension-methods"></a>
### 9.7 `Array<T>` extension methods

In addition to the methods declared on `Array<T>` itself ([§4.4](#sec-arrayt)), the LINQ extensions in [§5](#sec-system-collections-linq) apply to `Array<T>` because it implements `IEnumerable<T>`.

Multi-dimensional arrays are deferred to a future revision; the current standard library provides only one-dimensional `Array<T>` (heap) and `[N]T` (inline, language spec [§15.1](Bezel_Language_Spec.md#sec-inline-arrays)).

<a id="sec-ranget"></a>
### 9.8 `Range<T>`

The type underlying range expressions (language spec [§16](Bezel_Language_Spec.md#sec-range-expressions)). `Range<T>` is a compiler-intrinsic record. `T` is restricted by the compiler to the integer types (`byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`) and `char`; this restriction cannot be expressed through a `where` clause in the current type system and is enforced as a compiler-intrinsic rule.

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

`Contains` tests membership by endpoints and inclusivity; `Step` does not affect `Contains`. This is the operation invoked by the `in` operator (language spec [§17.5](Bezel_Language_Spec.md#sec-the-in-operator)).

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

`CString`, `WString`, `RawBytes`, `IntPtr`, and `UIntPtr` are FFI-boundary types. The first three are classes with `owned`-returning factory methods and idempotent `Dispose`. `IntPtr` and `UIntPtr` are compiler-intrinsic value types representing pointer-sized integers — they are documented here for completeness, but their members are detailed in language spec [§32.7](Bezel_Language_Spec.md#sec-pointers-and-intptr).

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
  public method ToBytes(): owned Array<byte>;

  destructor;       // releases the underlying buffer when this CString
                    //   allocated it (CString.From / FromBytes); for a CString
                    //   wrapping a native-allocated buffer, the disposal
                    //   behavior is governed by the FFI declaration. See prose.
end CString;
```

`CString.From(s)` allocates a fresh, owned `CString` whose contents are the UTF-8 encoding of `s` followed by a single null byte. The caller owns the result; when disposed, the underlying buffer is released.

`CString.FromBytes(bytes)` wraps an existing byte array; the bytes must already be null-terminated, and the caller is responsible for ensuring valid UTF-8.

A `CString` returned from a native function is wrapped without copying; the Bezel side must understand the native API's ownership contract (see language spec [§32.6](Bezel_Language_Spec.md#sec-string-marshalling)). Disposing such a `CString` calls the native deallocator only when constructed via `CString.From` or `FromBytes`. For native-allocated strings, the FFI declaration's documentation must specify how the native allocation should be released; this is beyond the scope of `CString` itself.

<a id="sec-wstring"></a>
### 10.2 `WString`

A null-terminated UTF-16 sequence, suitable for Windows APIs and other UTF-16-using ABIs.

```bezel
public class WString
  // No public constructor; obtained via factory methods.

  public static method From(s: string): owned WString;

  public readonly property Length: int;       // length in UTF-16 code units, excluding the null terminator

  destructor;       // releases the underlying buffer
end WString;
```

`WString.From(s)` allocates a fresh, owned `WString` whose contents are the UTF-16 encoding of `s` followed by a single zero code unit (two zero bytes). Surrogate pairs are produced for code points above the BMP per UTF-16 rules.

<a id="sec-rawbytes"></a>
### 10.3 `RawBytes`

A pointer-and-length pair representing arbitrary binary data.

```bezel
public class RawBytes
  // No public constructor; obtained via factory methods.

  public static method From(buffer: Array<byte>): owned RawBytes;

  public readonly property Length: int;
  public readonly property Pointer: IntPtr;

  public method CopyTo(destination: Array<byte>, destOffset: int);

  destructor;       // releases the underlying buffer if owned by this instance
end RawBytes;
```

`RawBytes.From(buffer)` allocates a fresh, owned `RawBytes` containing a copy of the source bytes. Mutations to the source after construction do not affect the `RawBytes`; the buffer is independent.

`Pointer` exposes the underlying memory address as an `IntPtr`, suitable for passing to native APIs that expect `void*`. The pointer is valid until the `RawBytes` is disposed.

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

---

<a id="sec-modules-deferred-to-future-revisions"></a>
## 11. Modules deferred to future revisions

The following standard library modules are anticipated but not specified in this version. Their absence is intentional; the current scope focuses on what the language specification depends on.

- `System.Net` and `System.Net.Http` — networking and HTTP. The language specification's async examples use a hypothetical `HttpClient`; a real one will live here.
- `System.Text.Json` and `System.Text.Regex` — a general JSON DOM/streaming API and regular expressions. Note this is distinct from the serialization JSON *format agent* (`JsonSerializer`/`JsonDeserializer` in `System.Serialization`, [§2.15](#sec-concrete-serialization-formats)), which is provided: `System.Serialization` maps the [§2.14](#sec-serialization-vocabulary) token stream to and from JSON for whole-value (de)serialization, whereas a future `System.Text.Json` would offer document-tree navigation and ad-hoc querying of arbitrary JSON not driven by a target type. The two are complementary, as they are in other ecosystems that provide both a serializer and a document API.
- `System.Security.Cryptography` — cryptographic primitives.
- `System.Globalization` — culture-aware formatting and comparison.
- `System.Reflection` — runtime metadata beyond `typeof` and `GetType`. Reserved pending design of the broader reflection model.
- `System.Diagnostics.Process` — child-process spawning and management.
- `System.Threading.Channels` — channel-style producer/consumer queues for async coordination.

The shape of these modules is open. Their inclusion in a future revision will be additive; nothing in this version will need to change to accommodate them.
