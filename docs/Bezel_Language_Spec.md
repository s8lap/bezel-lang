# Bezel Language Specification

**Version 0.138 — June 2026**

Bezel is a systems language designed to feel high-level. It has no garbage collector; instead, the compiler tracks ownership of every heap object and ensures that each one is disposed exactly once. Borrowing lets you pass references without transferring ownership, with lifetimes checked at compile time rather than enforced by a runtime. The result aims to give you the safety of a managed language and the predictability of a manual one, without the ceremony of either.

This document is the normative reference for the language. It defines syntax, type system, ownership rules, and the semantics of every construct. The specification is partitioned into a Core level and a set of Extended features; Appendix D defines the levels and what a conformance claim at each means. It is intentionally dense — written to be precise rather than introductory — and is best read alongside its two companion documents: the design rationale, which explains *why* each decision was made and records the alternatives considered, and the standard library, which catalogs the types and methods the language ships with.

If this is your first encounter with Bezel, the rationale's opening section is a gentler starting point: it sketches the goals of the language and the shape of its memory model before the spec's formal definitions begin. If you want concrete code first, the short companion guides (memory and ownership; errors and nullability) work from worked examples. Once you know what you are looking for, this document is the place to look it up.

**Changes since v0.133** are summarized at the end of the design rationale; this file is the normative reference and does not narrate its own history.

<a id="sec-contents"></a>
## Contents

1. [Lexical Structure](#1-lexical-structure)
2. [Modules and Compilation](#2-modules-and-compilation)
3. [Type System](#3-type-system)
4. [Built-in Types](#4-built-in-types)
5. [Type Conversions](#5-type-conversions)
6. [Arithmetic and Overflow](#6-arithmetic-and-overflow)
7. [Nullability](#7-nullability)
8. [The Object Class](#8-the-object-class)
9. [Records](#9-records)
10. [Enums](#10-enums)
11. [Classes and Interfaces](#11-classes-and-interfaces)
12. [Inheritance and Dispatch](#12-inheritance-and-dispatch)
13. [Visibility](#13-visibility)
14. [Generics](#14-generics)
15. [Arrays](#15-arrays)
16. [Range Expressions](#16-range-expressions)
17. [Expressions and Operators](#17-expressions-and-operators)
18. [Literals and Strings](#18-literals-and-strings)
19. [The `case` Construct](#19-the-case-construct)
20. [The `if` Expression](#20-the-if-expression)
21. [Statements](#21-statements)
22. [Methods and Constructors](#22-methods-and-constructors)
23. [Properties and Indexers](#23-properties-and-indexers)
24. [Events](#24-events)
25. [Operator Overloading](#25-operator-overloading)
26. [Extension Blocks](#26-extension-blocks)
27. [Lambdas and Function Types](#27-lambdas-and-function-types)
28. [Resource Management](#28-resource-management)
29. [Iteration](#29-iteration)
30. [Asynchronous Programming](#30-asynchronous-programming)
31. [Exception Handling](#31-exception-handling)
32. [Foreign Function Interface](#32-foreign-function-interface)
33. [Program Entry Point](#33-program-entry-point)
34. [Comments and Documentation](#34-comments-and-documentation)
35. [Appendix A: Grammar](#appendix-a-grammar)
36. [Appendix B: Reserved Words](#appendix-b-reserved-words)
37. [Appendix C: Compiler-Enforced Invariants](#appendix-c-compiler-enforced-invariants)
38. [Appendix D: Conformance Levels](#appendix-d-conformance-levels)

---

<a id="sec-lexical-structure"></a>
## 1. Lexical Structure

<a id="sec-source-files"></a>
### 1.1 Source files

A Bezel source file is a sequence of Unicode code points, encoded in UTF-8. Source files use the `.bzl` extension. Line endings are LF, CR, or CRLF.

<a id="sec-tokens"></a>
### 1.2 Tokens

A token is a keyword, identifier, literal, operator, or punctuation symbol. Whitespace and comments separate tokens but are not themselves tokens.

<a id="sec-identifiers"></a>
### 1.3 Identifiers

An identifier begins with a Unicode letter or underscore (`_`) and continues with letters, digits, or underscores. Identifiers are case-sensitive: `Foo`, `foo`, and `FOO` are distinct. The bare identifier `_` is a valid identifier name everywhere it appears as a name; in three specific positions (pattern, out-argument, discard-statement left-hand side) it has additional meaning as a discard, described in [§17.13](#sec-the-discard-_).

The convention is `PascalCase` for types, methods, properties, and other public members; `_camelCase` for private fields; `camelCase` for parameters and locals. The compiler does not enforce these conventions. The lowercase keyword `string` and the standard-library type name `String` (`System.String`) name the same type — they are interchangeable in source — but the lexer treats them as different tokens: `string` is a reserved keyword (Appendix B) producing a primitive type token; `String` is an ordinary PascalCase identifier whose meaning is established by name resolution against the auto-imported standard library. The same dual-form rule applies to the other primitive-keyword aliases listed in [§3.4](#sec-keyword-aliases-for-built-in-types).

<a id="sec-keywords"></a>
### 1.4 Keywords

Reserved words may not be used as identifiers, except for the contextual keywords listed in Appendix B which have meaning only in specific syntactic positions.

All keywords are lowercase. Type identifiers like `Object`, `Array`, and `Exception` are not keywords; they are PascalCase names defined in the standard library or compiler intrinsics.

<a id="sec-comments"></a>
### 1.5 Comments

Three forms:

- `// ...` — line comment, extends to end of line
- `/* ... */` — block comment, does not nest
- `/// ...` — documentation comment, attached to the immediately following declaration

Documentation comments use XML element syntax. See [§34](#sec-comments-and-documentation) for the recognized tag set and details. The compiler preserves the comments for documentation generators but does not interpret their content.

<a id="sec-statements-and-termination"></a>
### 1.6 Statements and termination

Every statement terminates with a semicolon. Block-bodied constructs terminate with `end`, optionally followed by a label, then a semicolon. Three categories of block construct admit different label forms:

- **Named constructs** — class, interface, record, enum bodies; method bodies; property accessor blocks. The optional label is an *identifier*: `end ClassName;`, `end methodName;`. When supplied, the identifier must match the construct's declared name exactly; a mismatch is a compile error.
- **Keyword-labelled constructs** — constructors, destructors, operator overload bodies. These have no user-chosen name, so the optional label is the construct keyword: `end constructor;`, `end destructor;`, `end operator +;`. When supplied, the keyword (and operator token, where applicable) must match the construct kind exactly.
- **Plain-end constructs** — all statement-level blocks (`if`, `while`, `for`, `with`, `do … end`, `try`), extension blocks, extern blocks, and lambda or arm bodies introduced with `do`. These admit no label of any form; only `end;` is valid. In particular, even though these constructs are introduced by keywords (`if`, `while`, `try`, etc.), the construct-keyword form from the second category does not apply here: `end if;`, `end while;`, `end try;`, `end with;`, and similar are not accepted. The opening keyword and the indented body together identify the construct unambiguously; the closing `end;` needs no further qualification.

In all three categories the unlabelled form `end;` is always valid; the label is for readability across long bodies and carries no semantic weight beyond compile-time verification that it matches.

---

<a id="sec-modules-and-compilation"></a>
## 2. Modules and Compilation

<a id="sec-module-declarations"></a>
### 2.1 Module declarations

Every source file declares its module on its first non-comment, non-import line:

```bezel
module BankingApp.Transactions;
```

A module name is a single identifier or a sequence of identifiers separated by dots. The dotted form is a single name; it does not establish nesting. `BankingApp` and `BankingApp.Transactions` are unrelated modules.

Multiple files with identical `module` declarations belong to the same module. Files in the same module share `internal` access ([§13](#sec-visibility)).

<a id="sec-imports"></a>
### 2.2 Imports

```bezel
import System.Collections.Generic;
```

`import` makes the public names of the named module visible in the current file. Imports are file-scoped: an import in one file has no effect on other files in the same module.

<a id="sec-always-available-names"></a>
### 2.3 Always-available names

The following names are visible in every source file without import.

**Language primitives.** `bool`, `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal`, `char`, `string`. The unit type `()`. `Object`, `Array<T>`, `OwnedArray<T>`. Inline arrays `[N]T` (with size N as part of the type). `Type`. `IDisposable`, `IAsyncDisposable`. `IEnumerable<out T>`, `IEnumerator<out T>`, `IAsyncEnumerable<out T>`, `IAsyncEnumerator<out T>`. `OwnedEnumerable<T>`, `OwnedEnumerator<T>`, `OwnedAsyncEnumerable<T>`, `OwnedAsyncEnumerator<T>`. `IReadOnlyCollection<out T>`, `IReadOnlyList<out T>`, `ICollection<T>`, `IList<T>`, `IOwnedCollection<T>`, `IOwnedList<T>`. `Task`, `Task<T>`, `OwnedTask<T>`. `Subscription`. `CancellationToken`. `IEvent<out T>`, `IAsyncEvent<out T>`. The exception types listed in [§31.4](#sec-standard-exception-types).

`Subscription` is declared in `System` (standard library [§2.11](Bezel_Standard_Library.md#sec-subscription)) and is auto-imported. It is the return type of event `.Subscribe()` methods and `CancellationToken.Register()` — operations that pervade idiomatic Bezel code. `CancellationToken` is also auto-imported (despite living in `System.Threading`) because it appears in the `await for ... with` desugaring ([§30.7](#sec-asynchronous-programming-async-iterators)), in default parameter values on standard async methods, and in `IAsyncEnumerable<T>.GetAsyncEnumerator` — making it a de-facto language-level type. `IEvent<T>` and `IAsyncEvent<T>` are auto-imported because they are the public types of every event member viewed from outside the declaring class ([§24.3](#sec-the-type-of-an-event-member)); requiring an explicit `import System.Threading` to subscribe to any event would defeat the design. Importing `System.Threading` is not required to use `Subscription`, `CancellationToken`, `IEvent<T>`, or `IAsyncEvent<T>`; it is required for `CancellationTokenSource`, `Mutex`, `EventSource<T>`, `AsyncEventSource<T>`, and the other threading types.

The iteration-interface family — `IEnumerable<T>`, `IEnumerator<T>`, `IAsyncEnumerable<T>`, `IAsyncEnumerator<T>`, `IReadOnlyCollection<T>`, `IReadOnlyList<T>`, `ICollection<T>`, `IList<T>`, plus the owning variants `OwnedEnumerable<T>`, `OwnedEnumerator<T>`, `OwnedAsyncEnumerable<T>`, `OwnedAsyncEnumerator<T>`, `IOwnedCollection<T>`, `IOwnedList<T>` — is auto-imported because these interfaces participate in core language constructs: `IEnumerable<T>` and the async variant in the `for` and `await for` desugarings ([§29.2](#sec-desugaring-of-for)); the read-only and mutating interfaces in standard-library types' inheritance clauses (`Array<T>: IList<T>`, `OwnedArray<T>: IOwnedList<T>`, etc.) and in pattern-frequent variable type ascriptions. The stdlib documents their full declarations in [§4](Bezel_Standard_Library.md#sec-system-collections-generic); the language treats them as primitives for visibility purposes, in the same way it treats `Array<T>` and `Object` (also documented in stdlib but always visible).

**Auto-imported standard library.** `System` (for `Console`, `Math`, `Convert`, `Environment`, `DateTime`, `TimeSpan`, `Guid`, `Random`, `IComparable<T>`, `IEquatable<T>`, `IDisposable`, `IAsyncDisposable`, `ISerializer`, `IDeserializer`, `ISerializable`, `IDeserializable`) and `System.Text` (for `StringBuilder`).

All other library modules require explicit `import`.

Language primitives cannot be shadowed. Auto-imported names may be shadowed by local declarations.

<a id="sec-compilation-unit"></a>
### 2.4 Compilation unit

The module is the unit of compilation. The compiler takes all source files belonging to a module, plus its dependencies, and produces a single native artifact:

- An **executable** is a standalone native binary. It requires exactly one `Main` method as defined in [§33](#sec-program-entry-point).
- A **library** is a shared native library (`.so`, `.dylib`, or `.dll`). It exposes its `public` types and methods.

<a id="sec-dependency-resolution"></a>
### 2.5 Dependency resolution

Dependencies are declared through `import` statements. The compiler resolves each dependency by reading the imported module's compiled artifact, which embeds public-interface metadata (type signatures, method signatures, generic constraints).

Circular type-level dependencies between modules are permitted. Circular static-initialization dependencies are not; the compiler detects and rejects them.

<a id="sec-build-phases"></a>
### 2.6 Build phases

1. Parse all source files of the module.
2. Resolve all type declarations across the module and its dependencies.
3. Type-check method bodies, field initializers, and expressions.
4. Generate native code; emit the artifact with embedded interface metadata.

Build-tool integration and project-file format are implementation-defined.

---

<a id="sec-type-system"></a>
## 3. Type System

<a id="sec-type-kinds"></a>
### 3.1 Type kinds

Bezel types fall into these kinds:

- **Primitive value types** — `bool`, the integer types, `float`, `double`, `decimal`, `char`. ([§4](#sec-built-in-types))
- **`string`** — a reference type with content equality and runtime-managed lifetime. ([§4.6](#sec-string))
- **Records** — user-defined value types with positional or body declarations. ([§9](#sec-records))
- **Enums** — user-defined value types backed by an integer constant. ([§10](#sec-enums))
- **Classes** — user-defined reference types with inheritance and disposal. ([§11](#sec-classes-and-interfaces)–[§12](#sec-inheritance-and-dispatch))
- **Interfaces** — user-defined contracts. An interface variable is a reference; its underlying value may be a class instance, a boxed record, a boxed enum, or a boxed primitive ([§3.2](#sec-reference-vs-value-types), [§8.6](#sec-boxing)). ([§11](#sec-classes-and-interfaces))
- **Arrays** — `[N]T` (inline, value-typed, fixed compile-time size) and `Array<T>` / `OwnedArray<T>` (heap-allocated, class types, may be used resizably or fixed). ([§15](#sec-arrays))
- **Function types** — `(T1, ..., Tn) -> R`, reference types. ([§27](#sec-lambdas-and-function-types))
- **The unit type `()`** — exactly one value, also written `()`.

Additionally, *type aliases* ([§27.8](#sec-type-aliases)) may be declared at module level. An alias is not a separate kind of type; it is a naming convenience for an existing one. Identity, equality, and method dispatch all see through the alias.

Every type derives from `Object`. Primitives, records, enums, inline arrays, the unit type, and value-typed instances generally derive via boxing ([§8.6](#sec-boxing)); classes, interfaces, `string`, heap arrays (`Array<T>`, `OwnedArray<T>`), and function types are reference types whose values are already heap-allocated and reach `Object` directly through the inheritance chain.

<a id="sec-reference-vs-value-types"></a>
### 3.2 Reference vs. value types

Every type is exactly one of two storage classes. The classification is total and disjoint.

A **value type** is copied on assignment; each variable holds an independent value. Records, enums, primitives, inline arrays (`[N]T`, including `[N]T?` and `[N owned]T?`/`[N owned]T`), and the unit type are value types. (Owning inline arrays are a special sub-category — *move-only value types* — that transfer rather than copy on assignment; see [§15.3](#sec-move-only-value-types).)

A **reference type** is shared on assignment; multiple variables may refer to the same heap-allocated object. Classes (including heap-array classes `Array<T>` and `OwnedArray<T>`), interfaces, `string`, and function types are reference types.

An interface-typed variable holds a reference: to a class instance directly, to a record value boxed at assignment ([§8.6](#sec-boxing)), to an enum value boxed at assignment, or to a primitive value boxed at assignment. Interfaces play two roles simultaneously — as **contracts** (specifying the operations a conforming type must provide) and as **reference types** (variables of interface type are heap references with the standard reference semantics). The roles compose rather than conflict: the contract aspect determines which types satisfy a given interface; the reference-type aspect determines what an interface-typed variable holds and how it is shared.

The classification has direct consequences in three places:

- **Assignment.** Value-typed `:=` copies; reference-typed `:=` aliases.
- **Ownership and disposal.** Only class types and class-implementing interface variables carry a disposal obligation ([§28](#sec-resource-management)); value types and runtime-managed reference types ([§28.9](#sec-runtime-managed-types)) do not.
- **Generic constraints.** `where T: value` admits value types only; `where T: class` admits non-nullable reference types of any kind. See [§14.2](#sec-constraints).

<a id="sec-nullability"></a>
### 3.3 Nullability

A reference type is non-nullable by default. A trailing `?` produces its nullable form. Value types may also be wrapped in `?` to produce an optional. See [§7](#sec-nullability-2).

<a id="sec-keyword-aliases-for-built-in-types"></a>
### 3.4 Keyword aliases for built-in types

Each primitive keyword aliases a standard library type. `string`, though a reference type rather than a primitive value type ([§3.1](#sec-type-kinds), [§3.2](#sec-reference-vs-value-types)), is included in this table because its lowercase keyword follows the same aliasing convention. The two forms are interchangeable:

| Keyword | Standard library name |
|---|---|
| `bool` | `System.Boolean` |
| `byte` | `System.Byte` |
| `sbyte` | `System.SByte` |
| `short` | `System.Int16` |
| `ushort` | `System.UInt16` |
| `int` | `System.Int32` |
| `uint` | `System.UInt32` |
| `long` | `System.Int64` |
| `ulong` | `System.UInt64` |
| `float` | `System.Single` |
| `double` | `System.Double` |
| `decimal` | `System.Decimal` |
| `char` | `System.Char` |
| `string` | `System.String` |

Static members of the standard library type are accessible through either form: `int.Parse`, `int.MaxValue`, `double.IsNaN`, `string.IsNullOrEmpty`.

`Object` has no lowercase keyword alias. Keyword aliases are reserved for primitive value types; `Object` is a class and is referenced as `Object` unconditionally. Code coming from C#, where `object` is a keyword for `System.Object`, should write `Object` in Bezel.

<a id="sec-operators-on-primitive-types"></a>
### 3.5 Operators on primitive types

The arithmetic, comparison, bitwise, and shift operators on primitive numeric types are **compiler intrinsics**. They are not declared as `operator` methods on the primitive-wrapper records (`Int32`, `Double`, `Boolean`, etc.) in the standard library; the compiler translates them directly to machine-level operations rather than calling user-visible methods.

The intrinsic operators are:

- **Arithmetic** on `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal`: `+`, `-`, `*`, `/`, unary `-`. Plus `mod` on integer types and `decimal` ([§6.1](#sec-integer-arithmetic)).
- **Comparison** on every primitive (including `bool`, `char`, `string`): `=`, `<>`, `<`, `<=`, `>`, `>=`.
- **Bitwise** on `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`: `&`, `|`, `^`, `~` (unary). Not valid on `bool`, `char`, or floating-point types ([§6.5](#sec-bitwise-operations)). Boolean operations use `and`/`or`/`not` (next bullet), which short-circuit and have boolean-only semantics.
- **Shift** on the integer types: `<<`, `>>`.
- **Logical** on `bool`: `and`, `or`, `not` (with short-circuit evaluation, [§17.2](#sec-notes-on-operators)).

Operand-type combinations follow the numeric-promotion rules in [§6](#sec-arithmetic-and-overflow) — for example, `int + long` promotes the `int` to `long` and uses the `long + long` intrinsic. The set of admitted combinations is fixed; primitives do not declare cross-type `operator +` methods that user code could inspect.

By contrast, operators declared on user-defined record and class types are regular `operator` method declarations subject to [§25](#sec-operator-overloading)'s overload-declaration rules. `String.operator +` (concatenation) is one such regular declaration: the compiler may constant-fold it for string-literal operands, but the method is real and could in principle have been written by user code. The same applies to user-defined operator overloads on records and classes.

`string` straddles the two categories: it is a reference type (and not listed among the primitive value types in [§3.1](#sec-type-kinds)), but its comparison operators (`=`, `<>`, `<`, `<=`, `>`, `>=`) are intrinsics, not declared overloads. Two reasons: classes — which `string` is — cannot overload `=` and `<>` per [§25.2](#sec-operator-overloading-restrictions) (they are reserved for reference equality on most classes; `string` overrides this with content equality through compiler intrinsics); and the ordering operators on `string` are defined uniformly as ordinal lexicographic comparison ([§4.6](#sec-string)), which the compiler implements directly rather than dispatching to a method. The asymmetry — `string`'s `+` is a declared overload while its `=` is intrinsic — reflects this rule about classes and equality, not a categorical statement that `string` is "primitive."

This split — intrinsics for primitive numeric operators and class-typed equality, declared methods for user-defined and string-concatenation operators — keeps the std lib's primitive-wrapper declarations short (the wrappers exist mainly to host static methods like `Parse` and `MaxValue`, not to enumerate every operator) while preserving the regular operator-overload mechanism for user types.

<a id="sec-derived-type-properties"></a>
### 3.6 Derived type properties

Several rules in this specification classify types by a predicate computed from the type's *structure* — its declaration and, for a generic type, its type arguments — rather than by anything the programmer declares. Such a predicate is a **derived type property**. This section defines the mechanism once; the individual properties are defined where their subject matter lives and obey the common rules below.

The derived type properties of this revision:

| Property | Defined in | Classifies |
|---|---|---|
| **trivial-dispose** | [§28](#sec-resource-management) intro, [§28.9](#sec-runtime-managed-types) | classes whose `Dispose()` does nothing: no destructor, no `owned` fields, no owning-inline-array fields, trivial-dispose base |
| **borrow-bearing** | [§9.7](#sec-records-and-disposal) | records (and inline arrays, [§15.1](#sec-inline-arrays)) carrying at least one borrow of a non-runtime-managed reference type |
| **auto-equality-bearing** | [§9.2](#sec-body-records) | records for which `=`, `GetHashCode`, and `ToString` are synthesized |
| **serialization-eligible** | [§9.8](#sec-serialization-synthesis) | records and enums for which `Serialize`/`Deserialize` are synthesized |
| **element category** | [§28.11](#sec-collection-element-ownership), stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans) | every type, as collection-element: value, runtime-managed reference, class/interface, or move-only value — the classification on which collection and buffer slot semantics dispatch |
| **sendable** | [§30.14](#sec-sendable-types) | types whose values may cross an execution-domain boundary (spawn arguments and results, channel elements) |

Common rules, normative for every derived type property:

1. **Computed, never declared.** A derived type property cannot be asserted, opted into, or waived by annotation. It follows from structure; the only way to change it is to change the structure.
2. **Per-instantiation evaluation for generics.** For a generic type or member, the property is evaluated at each instantiation, after substituting the type arguments. A generic body type-checks under the weakest assumption its constraints establish; an instantiation that violates a property-dependent requirement is rejected *at the instantiation site*, with a diagnostic naming the property, the offending type argument, and the structural reason ([§28.5](#sec-owned-fields)'s generic-owned-field check and the owning-collection element restriction of [§14.4](#sec-owned-as-a-generic-argument) follow this pattern).
3. **Part of the public contract.** A declaration change that flips a derived type property of a public type — adding a destructor flips trivial-dispose; adding a computed property in the wrong form flips auto-equality-bearing — is a breaking change for clients relying on it, exactly as a signature change is. Library documentation should state the properties a type's clients are expected to rely on.
4. **Named in diagnostics.** A diagnostic produced by a property-dependent rule names the property in the words of the table above, so the reader can find the defining section.

Dispatch of *behavior* (not merely admissibility) on a derived type property — as the element category does for buffer and collection slot semantics ([§28.11](#sec-collection-element-ownership), stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)) — is reserved to compiler-known standard-library types; it is not a user-available mechanism in this revision.

New rules added to this specification that need a structural classification of types must define it as a derived type property under the rules of this section, rather than introducing a parallel mechanism.

---

<a id="sec-built-in-types"></a>
## 4. Built-in Types

<a id="sec-bool"></a>
### 4.1 `bool`

Two values: `true` and `false`. No implicit conversion to or from any other type. Conditions of `if`, `while`, and `case` (in scrutinee-less form) require `bool`.

<a id="sec-integer-types"></a>
### 4.2 Integer types

| Type | Width | Range |
|---|---|---|
| `byte` | 8, unsigned | 0 to 255 |
| `sbyte` | 8, signed | −128 to 127 |
| `short` | 16, signed | −32 768 to 32 767 |
| `ushort` | 16, unsigned | 0 to 65 535 |
| `int` | 32, signed | −2 147 483 648 to 2 147 483 647 |
| `uint` | 32, unsigned | 0 to 4 294 967 295 |
| `long` | 64, signed | −2⁶³ to 2⁶³ − 1 |
| `ulong` | 64, unsigned | 0 to 2⁶⁴ − 1 |

An integer literal without a suffix has type `int` if it fits in `int`'s range; otherwise the literal requires a suffix. Suffixes are case-insensitive: `L` (long), `U` (uint), `UL` or `LU` (ulong — both orderings accepted).

<a id="sec-floating-point-types"></a>
### 4.3 Floating-point types

| Type | Width | Description |
|---|---|---|
| `float` | 32 | IEEE 754 single-precision |
| `double` | 64 | IEEE 754 double-precision |

A floating-point literal (one with a decimal point or exponent) has type `double` by default. The `f` suffix produces `float`. IEEE 754 values `Infinity`, `-Infinity`, and `NaN` are valid; floating-point division by zero produces one of these rather than raising an exception.

<a id="sec-equality-semantics-for-floating-point-types"></a>
### 4.3.1 Equality semantics for floating-point types

The equality operators `=` and `<>` ([§17.3](#sec-the-=-and-operators)) follow reflexive-equality semantics rather than IEEE 754:

- `NaN = NaN` is **true**. Any non-NaN value compared with NaN by `=` is **false**.
- `-0.0 = +0.0` is **true**.
- For all other pairs, `=` agrees with IEEE 754 equality.

The relational operators `<`, `<=`, `>`, `>=` follow IEEE 754: any comparison involving `NaN` is false, including `NaN < NaN`, `NaN > NaN`, `NaN <= NaN`, and `NaN >= NaN`.

The asymmetry is deliberate. Equality is the operation used by hash tables, sets, and `case` patterns; programmers expect a value to be equal to itself, and an irreflexive `=` makes `Set<double>` and `Dictionary<double, _>` misbehave when NaN is inserted. The relational operators retain IEEE semantics so numeric algorithms continue to work as published.

For strict IEEE comparison (where `NaN` is unequal to itself), use `Double.IeeeEquals(a, b)` and `Float.IeeeEquals(a, b)` from the standard library. For bitwise comparison (which distinguishes the bit patterns of `+0.0` and `-0.0` and of distinct NaN payloads), use `Double.BitwiseEquals`/`Float.BitwiseEquals`.

`decimal` has no `Infinity`, `NaN`, or signed zero, so the question does not arise: `=` on `decimal` is straightforward value equality.

<a id="sec-decimal"></a>
### 4.4 `decimal`

High-precision decimal floating-point. 28–29 significant decimal digits. Exact representation of finite decimal fractions. The `m` suffix produces a `decimal` literal: `1.50m`. Division by zero raises `DivideByZeroException`. `decimal` has no `Infinity` or `NaN`.

<a id="sec-char"></a>
### 4.5 `char`

A single Unicode scalar value, 32 bits wide. Surrogate code points (U+D800 to U+DFFF) are not valid `char` values.

Character literals: `'a'`, `'\n'`, `'\u0041'`, `'\U0001F600'`. Character values compare by code-point value: `'A' < 'B'`.

<a id="sec-string"></a>
### 4.6 `string`

An immutable sequence of `char` (Unicode scalar values). A reference type with content-based equality. `string` implements `IEnumerable<char>`, so strings may be iterated with `for`:

```bezel
for var c in "hello" do Console.WriteLine(c); end;
```

Operations:

- `s.Length` returns the number of code points in `s`. O(1).
- `s[i]` returns the `char` at code-point position `i`, zero-indexed. O(1). Out-of-range raises `IndexOutOfRangeException`.
- `s[r]` for a `Range<int>` value `r` returns the substring spanning `r`. O(k) for a result of length `k`. Endpoints follow the standard rules for `Range<int>` over `s`'s indices: a closed `s[a..b]` includes both ends, a half-open `s[a..<b]` excludes the upper, and the open-ended forms (`s[..b]`, `s[a..]`, `s[..]`) extend to the start or end of the string. Out-of-range endpoints raise `IndexOutOfRangeException`; an empty range produces the empty string. The result is a fresh `string`; mutating one does not affect the other (strings are immutable, so this is observably true regardless of representation).
- `s = t` and `s <> t` compare by code-point sequence. O(min(|s|, |t|)).
- `s < t`, `s > t`, `s <= t`, `s >= t` perform ordinal lexicographic comparison of the code-point sequence. Locale-aware comparison is provided by standard library methods.
- `s + t` concatenates two strings. The compiler does not implicitly convert other types to `string`; use interpolation or `.ToString()`.
- `c in s` tests whether the character `c` appears in `s` (equivalent to `s.Contains(c)`, [§17.5](#sec-the-in-operator)).

The user-visible model is "a sequence of code points indexed in O(1)." The internal representation is implementation-defined: an implementation is expected to choose a per-string storage width — typically 1, 2, or 4 bytes per code point, selected at construction based on the largest code point in the value — to keep the common case (predominantly-ASCII text) compact while preserving O(1) indexing for every string. The choice of width is invisible at the language level; programs observe only the `char` sequence and its length.

A pure-ASCII string (every code point in U+0000–U+007F) is identical in memory layout to a UTF-8 byte sequence. Implementations expose this through the runtime predicate `s.IsAscii: bool` (standard library [§9.6](Bezel_Standard_Library.md#sec-string)). A zero-copy byte view is provided as `ReadOnlyAsciiByteSpan` via `s.AsAsciiByteSpan()` and `s.TryAsAsciiByteSpan(out value): bool` (standard library [§3.4](Bezel_Standard_Library.md#sec-readonlyasciibytespan)); the former throws on non-ASCII input, the latter returns `false`. For FFI and IO callers needing UTF-8 bytes from an ASCII string this is the allocation-free path. When the string is not ASCII, or when a fresh owning byte array is required regardless, `Encoding.UTF8.GetBytes(s)` is `O(n)` and always correct.

`string` is **runtime-managed**: its memory is reclaimed automatically. `Dispose()` on a string is a no-op. String fields cannot be marked `owned`. See [§28.9](#sec-runtime-managed-types).

Whether the runtime interns strings (shares storage for identical string values) is implementation-defined. String literals appearing in source code may be interned at compile time. The `=` operator on strings always performs content comparison ([§4.6](#sec-string) above); implementations may short-circuit via reference equality when both operands refer to the same object, but this optimization is invisible to programs.

<a id="sec-the-unit-type"></a>
### 4.7 The unit type

`()` is the type with exactly one value, also written `()`. It is the return type of methods declared without an explicit return type.

Wrapping the unit type with `?` (yielding `()?`) is permitted by the grammar and produces an optional with two values, `null` and `()`. It is rarely useful; the natural representation of "presence or absence" for a procedure-shaped result is `bool`.

The unit type is a value type and can in principle be boxed via the `Object` derivation chain ([§8.6](#sec-boxing)) — `var o: Object := ();` is grammatically valid and produces a boxed `()`. The result is rarely meaningful; treat the unit value as a sentinel that you don't store in `Object`-typed variables. The interface-substitution description in [§3.1](#sec-type-kinds) omits boxed unit for the same reason.

<a id="sec-ffi-only-primitive-types"></a>
### 4.8 FFI-only primitive types

The standard library defines `IntPtr`, `UIntPtr`, `CString`, `WString`, and `RawBytes` for use at the FFI boundary. `IntPtr` and `UIntPtr` are compiler intrinsics. The `CString`, `WString`, and `RawBytes` APIs are documented in standard library [§10](Bezel_Standard_Library.md#sec-ffi-boundary-types). See [§32](#sec-foreign-function-interface) for FFI-side usage.

---

<a id="sec-type-conversions"></a>
## 5. Type Conversions

<a id="sec-implicit-conversions"></a>
### 5.1 Implicit conversions

A value of one type implicitly converts to another when no programmer-visible information is lost — or, for the floating-point cases noted below, when the operation is universally safe enough that explicit casts would create unjustified noise.

| From | Implicitly converts to |
|---|---|
| `sbyte` | `short`, `int`, `long`, `float`, `double`, `decimal` |
| `byte` | `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |
| `short` | `int`, `long`, `float`, `double`, `decimal` |
| `ushort` | `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |
| `int` | `long`, `float`, `double`, `decimal` |
| `uint` | `long`, `ulong`, `float`, `double`, `decimal` |
| `long` | `float`, `double`, `decimal` |
| `ulong` | `float`, `double`, `decimal` |
| `float` | `double` |
| `char` | `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |

Implicit conversions to `float` from integers wider than 24 bits, and to `double` from integers wider than 53 bits, may lose precision but never overflow.

The following are **never implicit**:

- Between signed and unsigned types of the same or narrower width (e.g. `int → uint`, `uint → int`, `short → ushort`). Widening from unsigned to a strictly larger signed type (e.g. `byte → short`, `ushort → int`) is permitted because no value is lost.
- Between `decimal` and binary floats (`float`, `double`).
- Between an enum and its underlying integer.
- Between integers and `bool`.

A non-nullable reference type implicitly converts to its nullable form ([§7](#sec-nullability-2)).

<a id="sec-explicit-conversions"></a>
### 5.2 Explicit conversions

Explicit conversion uses function-call syntax: the target type's name applied to the source value.

```bezel
var y: short := short(x);
var i: int := int(d);             // truncates toward zero
var c: char := char(n);           // n must be a valid code point
var f: double := double(m);       // decimal to double
```

Integer narrowing raises `OverflowException` if the value does not fit. Floating-point to integer truncates toward zero; `NaN`, `±Infinity`, and out-of-range values raise `OverflowException`. Integer to floating-point never overflows; precision may be lost.

Casting an integer to `char` requires the integer to fall in U+0000–U+10FFFF excluding the surrogate range; otherwise `OverflowException`.

<a id="sec-nullable-casts"></a>
### 5.3 Nullable casts

```bezel
var y: long? := long?(x);             // nullable to nullable; null stays null
var m: int := int(n);                 // raises NullReferenceException if n is null
```

Casting a nullable to its non-nullable form requires the value to be non-null at runtime. The safer forms are `n!` (assert non-null then cast, [§7.8](#sec-null-forgiving)) and `n ?? default` (substitute a default).

<a id="sec-enum-casts"></a>
### 5.4 Enum casts

Enums require explicit casts to and from their underlying integer type:

```bezel
var n: int := int(p);                 // enum to int
var p: Priority := Priority(1);       // int to enum
```

Casting an integer that does not correspond to a declared constant produces a value not equal to any declared constant; the call site is responsible for validating.

<a id="sec-non-int-underlying-types"></a>
#### Non-`int` underlying types

`int(p)` is the conventional spelling for "convert an enum value to its underlying integer," but the cast is only well-typed when the enum's underlying type is `int`. For an enum whose underlying type is `short`, `long`, or another integer type, the natural form names the underlying type — `short(p)`, `long(p)`, etc. The cast `int(p)` on a non-`int`-backed enum is treated as a two-step conversion: enum-to-underlying followed by integer-to-`int` widening (or narrowing, with overflow checks per [§6.1](#sec-integer-arithmetic)). For `byte`-backed `Priority`, `int(p)` is `int(byte(p))`, which always succeeds because `byte` widens to `int`. For `long`-backed `HttpStatus`, `int(s)` is `int(long(s))`, which raises `OverflowException` if the value exceeds `int.MaxValue` or is below `int.MinValue`. The general rule: name the underlying type for round-trip safety, and use `int(...)` only when the underlying type is `int` or fits in `int` without loss.

<a id="sec-out-of-range-integer-to-enum-casts"></a>
#### Out-of-range integer-to-enum casts

`E(n)` for an integer `n` outside the underlying type's range is a compile error if `n` is a constant the compiler can evaluate; at runtime, `E(n)` raises `OverflowException` when the value being cast exceeds the underlying type's range. Within the underlying type's range but not corresponding to any declared constant, the cast succeeds and produces a "raw" enum value that is not equal to any declared constant — useful when round-tripping through serialization, dangerous when used as if it were a declared constant. Code that consumes enum values from external sources (parsing, network protocols) should validate against the declared constants explicitly.

---

<a id="sec-arithmetic-and-overflow"></a>
## 6. Arithmetic and Overflow

<a id="sec-integer-arithmetic"></a>
### 6.1 Integer arithmetic

Operators `+`, `-`, `*`, `/`, `mod`, unary `-`, `++`, and `--` on integer types are checked for overflow. An operation whose mathematical result is not representable in the result type raises `OverflowException`.

```bezel
var a: int := 2_000_000_000;
var b: int := 2_000_000_000;
var c := a + b;                       // OverflowException
```

`++` and `--` are postfix-only and statement-only; they do not produce a value. See [§17.6](#sec-the-increment-and-decrement-operators).

Division `/` between two integers truncates toward zero. `mod` follows the dividend's sign:

```bezel
var q := -7 / 3;                      // -2
var r := -7 mod 3;                    // -1
```

Division and `mod` raise `DivideByZeroException` when the right operand is zero.

`mod` is defined on integer types and on `decimal` (where it follows the same dividend-sign rule and raises `DivideByZeroException` when the divisor is zero). It is **not** defined on `float` or `double`; an expression of the form `x mod y` where either operand has a binary floating-point type is a compile error. The reason is that `mod` carries no floating-point semantics in this spec — the natural choice (IEEE 754 remainder, or C `fmod`) is a deliberate decision rather than a syntactic afterthought, and naming it explicitly avoids the ambiguity. The standard library exposes `Math.Fmod(x: double, y: double): double` for the C-compatible same-sign-as-dividend remainder, and `Math.IEEERemainder(x: double, y: double): double` for the IEEE 754 nearest-even form.

<a id="sec-mixed-type-arithmetic"></a>
### 6.2 Mixed-type arithmetic

When operands have different types, the smaller-range operand is implicitly promoted to the larger type using [§5.1](#sec-implicit-conversions). The result has the larger type. If no implicit conversion exists between the operand types (e.g. `double` and `decimal`), the operation is a compile error and an explicit cast is required.

<a id="sec-floating-point-arithmetic"></a>
### 6.3 Floating-point arithmetic

`float` and `double` arithmetic follows IEEE 754. Overflow produces `±Infinity`. Operations involving `NaN` propagate `NaN`. Division by zero produces `±Infinity` or `NaN`. No exception is raised.

Modulo on floating-point values is intentionally not provided as an operator. Use `Math.Fmod` (C-style same-sign-as-dividend) or `Math.IEEERemainder` (IEEE 754 nearest-even) instead; see [§6.1](#sec-integer-arithmetic).

<a id="sec-decimal-arithmetic"></a>
### 6.4 Decimal arithmetic

`decimal` arithmetic raises `OverflowException` on overflow and `DivideByZeroException` on division by zero. There is no `Infinity` or `NaN` for `decimal`.

<a id="sec-bitwise-operations"></a>
### 6.5 Bitwise operations

Operators `&` (AND), `|` (OR), `^` (XOR), `<<` (left shift), `>>` (right shift), and unary `~` (complement) operate on the binary representation of integer values. They are not valid on `bool`, `char`, or floating-point types.

`~x` is the bitwise complement; for signed types, `~x = -(x + 1)`. The keyword `not` is boolean negation, valid only on `bool`. `not` is not user-overloadable; types wanting domain-specific inversion provide a named method (e.g. `.Negate()`) or overload `~` if a bitwise reading applies. `~` and `not` are not interchangeable.

Left shift fills with zeros on the right. Right shift fills with the sign bit on signed types, zero on unsigned. The shift count is reduced modulo the operand's bit width.

Compound assignments: `&=`, `|=`, `^=`, `<<=`, `>>=`. There is no `~=` (unary operators have no compound form).

<a id="sec-compound-assignments"></a>
### 6.6 Compound assignments

`+=`, `-=`, `*=`, `/=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, `??=`. `mod` has no compound assignment.

---

<a id="sec-nullability-2"></a>
## 7. Nullability

<a id="sec-non-nullable-and-nullable-types"></a>
### 7.1 Non-nullable and nullable types

A plain reference type is non-nullable; a `?` suffix produces its nullable form:

```bezel
var name: string := "Alice";        // non-nullable
var middle: string? := null;        // nullable
```

`string` and `string?` are distinct types. The compiler enforces the distinction at every assignment, parameter pass, and return.

A non-nullable reference type implicitly converts to its nullable form. The reverse requires a null check or `!`.

<a id="sec-nullable-value-types"></a>
### 7.2 Nullable value types

A value type wrapped in `?` is an optional: it either holds a value or `null`. `int?`, `Priority?`, and `DateTime?` are optional value types.

<a id="sec-assigning-null"></a>
### 7.3 Assigning null

Assigning `null` to a non-nullable target is a compile error.

<a id="sec-member-access-on-nullable-values"></a>
### 7.4 Member access on nullable values

Member access, method calls, and indexing on a nullable value are compile errors unless the compiler has narrowed it to non-null:

```bezel
var user: User? := GetUser();
if user <> null then
  Console.WriteLine(user.Name);     // narrowed to User here
end;
```

Narrowing also applies after early return, after `is not null`, and inside the controlled block of a successful `is` test ([§12.7](#sec-type-tests-is-is-not-as)).

The narrowing of a variable is invalidated if the variable is reassigned within the controlled block, or if the variable is itself passed as a `ref` argument inside the controlled block (the callee may reassign it). The compiler does not attempt deeper analysis of which methods could reassign which variables transitively; only direct reassignment and direct `ref`-passing invalidate narrowing.

<a id="sec-narrowing-applies-to-locals-only"></a>
#### Narrowing applies to locals only

The compiler narrows only local variables — never fields or properties. A field readable by another thread, or one capturable by a closure, may be reassigned to `null` between a null check and a subsequent use, producing a TOCTOU (time-of-check / time-of-use) bug that the compiler cannot prevent.

The standard pattern is to copy a nullable field to a local before narrowing:

```bezel
var u := _user;
if u <> null then
  u.DoStuff();          // u is narrowed; safe regardless of _user changes
end;
```

Local-variable narrowing applies to the copy, which no other thread can observe. Accessing a nullable field without first copying it to a local is a compile error when the access requires non-null.

<a id="sec-null-conditional-access"></a>
### 7.5 Null-conditional access (`?.`)

`x?.M` returns null if `x` is null; otherwise it performs the access. The result type is always nullable:

```bezel
var name: string? := user?.Name;
var length: int? := user?.Name?.Length;
user?.Log("Accessed");
```

`?.` short-circuits: if any segment of a chain is null, the remainder is not evaluated.

<a id="sec-null-conditional-call-args"></a>
### 7.6 Null-conditional call (`?(args)`)

A function value of nullable function type may be invoked with `?(...)`:

```bezel
var handler: ((int) -> ())? := null;
handler?(42);                       // no-op when null
```

The result type is nullable (or `()` for procedure-typed values).

<a id="sec-null-coalescing-and-="></a>
### 7.7 Null-coalescing (`??` and `??=`)

`a ?? b` returns `a` if non-null, else `b`. The right operand's type must be compatible with the non-nullable form of the left.

```bezel
var displayName: string := user?.Name ?? "Anonymous";
_cache ??= new Cache();             // assign only if currently null
```

<a id="sec-null-forgiving"></a>
### 7.8 Null-forgiving (`!`)

`x!` asserts non-nullness. If `x` is null at runtime, `NullReferenceException` is raised. The result is the non-nullable form of the expression. `!` is the only postfix `!` operator in the language; there is no prefix `!`.

<a id="sec-definite-assignment-for-non-nullable-fields"></a>
### 7.9 Definite assignment for non-nullable fields

Every non-nullable field must be either:
- assigned a value in every constructor before the constructor returns, or
- given a field initializer at its declaration.

Definite-assignment analysis applies the same rule to non-nullable locals: a non-nullable local must be assigned before it is read.

The guarantee this rule exists to provide is operational: **no non-nullable field is ever observed unset**. For ordinary (hand-written) construction the constructor-scoped rule above delivers that guarantee directly — by the time any code can observe the object, every non-nullable field has been assigned. One compiler-controlled construction mode delivers the same guarantee by a different mechanism: the synthesized two-phase deserialization of a `serialized cyclic` object group ([§9.8](#sec-cyclic-class-synthesis)). There, a back-edge field is assigned in a second phase, after its constructor returns but before the constructed group is released to any caller, and the self-contained-constructor eligibility rule ([§9.8](#sec-cyclic-class-synthesis)) statically guarantees no construction-time code in the group can read the field in the interval. Definite assignment is **preserved, not relaxed**: the rule's operational guarantee (no observable unset non-nullable field) holds in full; the two-phase window is treated as a single construction unit for definite-assignment purposes, and it is available only to compiler-synthesized deserialization, never to hand-written constructors, which remain bound by the constructor-scoped rule above with no exception. This is the only construction mode in the language not covered verbatim by the two bullets, and it exists precisely so that cyclic object-graph deserialization need not weaken the rule for everyone else (rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection)).

<a id="sec-default-initialization"></a>
#### Default initialization

For fields of value type (primitives, records, enums, inline arrays, the unit type), the must-initialize rule is satisfied vacuously when the type has a well-defined default value: zero for numerics, `false` for `bool`, the all-default record for record types whose fields all have defaults, the all-zero/null inline array for inline-array types whose elements have defaults. A static or instance field of such a value type is implicitly initialized to that default. An explicit initializer is permitted and overrides the default. For value types whose default is not well-defined — records with non-nullable reference fields, inline arrays with non-nullable reference elements, owning inline arrays with non-nullable element type — the field requires an explicit initializer per the same rules as non-nullable reference fields below.

For fields of nullable reference type (declared with `?`), the must-initialize rule is satisfied vacuously by `null`: nullable reference fields are implicitly initialized to `null`.

For fields of non-nullable reference type — class types (including `Array<T>`, `OwnedArray<T>`), interfaces, `string`, function types, declared without `?` — there is no default value, so the must-initialize rule is enforced strictly: the field requires either a field initializer or assignment in every constructor (for instance fields) or in the static constructor (for static fields). The compiler emits a definite-assignment error if neither path supplies a value.

The same three-way split applies in the standard `var`-declaration case: a value-typed local can be declared without an initializer (gets the default); a nullable local can be declared without an initializer (gets `null`); a non-nullable reference-typed local must be assigned before it is read.

<a id="sec-generics-and-nullability"></a>
### 7.10 Generics and nullability

Inside an unconstrained generic body, `T` is treated conservatively as possibly nullable. Storage of type `T` should be declared `T?`.

`where T: notnull` restricts T to non-nullable types. Inside the body, `T` is non-nullable.

`where T: class` restricts T to non-nullable reference types — classes (including `Array<T>`, `OwnedArray<T>`), interfaces, `string`, function types.

`where T: value` restricts T to non-nullable value types — records, enums, primitives, inline arrays (all forms, including `[N owned]…` owning forms), and the unit type. A nullable value type (e.g. `int?`) does not satisfy the constraint ([§14.2](#sec-constraints)); inside the body, T is the non-nullable value type and `T?` is its nullable companion. Inside the body, operations that copy T (assignment, parameter passing by value, returning by value) are checked at instantiation: for move-only inline arrays ([§15.3](#sec-move-only-value-types)), copy operations are replaced by moves, and any operation the generic body uses that requires copy semantics (e.g., reading T twice without an intervening assignment) is rejected at instantiation with a message naming the move-only type.

`where T: SomeClass` and `where T: IFoo` are *type-bound* constraints. They restrict T to non-nullable types conforming to the named type:
- `where T: SomeClass` admits SomeClass and its subclasses. Interfaces do not extend classes in Bezel, so this constraint admits class types only.
- `where T: IFoo` admits any non-nullable type that conforms to `IFoo`: classes that implement it (directly or by inheritance), records that implement it, and interfaces that extend it (directly or transitively). See [§14.2](#sec-constraints) for the conformance rule and worked examples.

Inside the generic body, T is non-nullable under any type-bound constraint.

`T?` always means "the nullable form of T," regardless of constraint. Under `where T: notnull`, `T` is the non-nullable form and `T?` is the nullable companion.

<a id="sec-lifted-operators"></a>
### 7.11 Lifted operators

For a nullable value type `T?` whose underlying type `T` is a primitive on which a built-in operator ([§6](#sec-operators-on-primitive-types)) is defined, that operator **lifts** to the nullable form. Lifting is the natural completion of the null-safe equality already defined for `=`/`<>` ([§8.4](#sec-=-and)): the operators are defined on `T?` operands in terms of the underlying operator on `T`, with `null` handled uniformly. The rules differ by operator category.

**Arithmetic, bitwise, and shift operators.** The binary operators `+`, `-`, `*`, `/`, `mod`, `&`, `|`, `^`, `<<`, `>>` and the unary operators `-` and `~` lift to yield `T?`. A lifted binary operator produces `null` if *either* operand is `null`; a lifted unary operator produces `null` if its operand is `null`. A mixed expression in which one operand is `T?` and the other is the non-nullable `T` lifts as well — the non-nullable operand is treated as present (never `null`), and the result is `T?`. The runtime checks each operator already performs — `OverflowException` on integer overflow ([§6.1](#sec-integer-arithmetic)), `DivideByZeroException` on integer or `decimal` division/`mod` by zero ([§6.1](#sec-integer-arithmetic)) — apply only when both operands are present (non-null); when the lifted result is `null` by reason of a null operand, no underlying operation runs and no check fires.

**Relational operators.** The relational operators `<`, `<=`, `>`, `>=` lift to yield **`bool`** — *not* `bool?`. A lifted relational comparison yields `false` if *either* operand is `null`. This mirrors the rule that a `null` participant in an ordering comparison is not "less than" or "greater than" anything; the result is simply `false`. (Equality `=` and `<>` are unchanged — they are already null-safe per [§8.4](#sec-=-and), where `null = null` is `true` and `null = nonNull` is `false`.)

**No three-valued logic.** Lifting does *not* introduce three-valued (`bool?`) logical operators. The boolean operators `and`, `or`, `not` ([§17.2](#sec-notes-on-operators)) operate only on `bool`; there is no lifted `and`/`or`/`not` over `bool?`, and a `bool?` operand to a boolean operator is a compile error. Code that needs to combine `bool?` values handles the `null` case explicitly (e.g. with `??` ([§7.7](#sec-null-coalescing-and-=)) or a narrowing check).

**User-defined operators.** Lifting applies to the built-in primitive operators only. Lifting of user-defined operators ([§25](#sec-operator-overloading)) declared on record or class types is **deferred**; a future revision may define how (or whether) a user-declared `operator +` on a value type lifts to its nullable form.

**Safety.** A lifted arithmetic/bitwise/shift result stays typed `T?`. It is therefore subject to the same narrowing discipline as any other nullable value: [§7.4](#sec-member-access-on-nullable-values) narrowing (or a null-coalescing/null-forgiving form) is still required before the result is used in a non-nullable position. Lifting never silently produces a non-nullable value from nullable operands; it only defines what the operators *mean* on nullable operands.

---

<a id="sec-the-object-class"></a>
## 8. The Object Class

<a id="sec-definition"></a>
### 8.1 Definition

```bezel
public class Object
  public method GetType(): Type;
  public virtual method Equals(other: Object?): bool;
  public virtual method GetHashCode(): int;
  public virtual method ToString(): string;
  public static method ReferenceEquals(a: Object?, b: Object?): bool;
end Object;
```

`Object` is a compiler intrinsic; method bodies are provided by the runtime. The declaration above defines the public surface. User code cannot redeclare or replace `Object`.

`Object` is the universal root. Every type derives from it: classes (including heap-array classes `Array<T>` and `OwnedArray<T>`) directly through the inheritance chain, with `string`, interfaces, and function types reaching `Object` similarly via their reference-type heap representation; records, enums, primitives, inline arrays, and the unit type derive via boxing ([§8.6](#sec-boxing)) when used in `Object`-typed positions (subject to the inline-array boxing rules in [§8.6](#sec-boxing-of-inline-arrays)).

<a id="sec-gettype"></a>
### 8.2 `GetType`

`v.GetType()` returns the `Type` value for the runtime type of `v`. For value types, the call boxes the value before reading its type tag.

`GetType` is non-virtual; runtime type identity is fixed at allocation and cannot be overridden.

`GetType` complements the `typeof(T)` expression ([§17.10](#sec-the-typeof-operator)), which produces the `Type` for a statically named type.

<a id="sec-equals-and-gethashcode"></a>
### 8.3 `Equals` and `GetHashCode`

`Equals` defines value-level equality. The compiler generates a default appropriate to each type kind:

| Type kind | Default `Equals` | Default `GetHashCode` |
|---|---|---|
| Class | Reference equality | Identity-based |
| Positional record | Field-by-field value equality | Hash of all positional fields |
| Body-only record | Compile error if `=`/`<>` used; no default | Same |
| Enum | Underlying integer comparison | Hash of integer value |
| Primitive | Value comparison | Value-based |
| Inline array `[N]T`, `[N]T?` | Element-by-element value equality ([§15.1.2](#sec-inline-array-members)) | Hash combining element hashes in slot order |
| Inline array `[N owned]T`, `[N owned]T?` | Element-by-element equality via borrow ([§15.3](#sec-move-only-value-types)) | Hash combining element hashes in slot order |

`GetHashCode` must satisfy: if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`. Hash collisions on non-equal values are permitted. The auto-generated `GetHashCode` for positional records combines per-field hashes in declaration order; the combining function is order-dependent, so records with the same field types in different positions produce different hashes for different orderings (e.g. `Pair(1, 2)` and `Pair(2, 1)` have different hashes). The choice of combining algorithm is implementation-defined, subject to the satisfaction of the hash-equality invariant.

A type that overrides `Equals` must also override `GetHashCode`. The compiler enforces this.

<a id="sec-=-and"></a>
### 8.4 `=` and `<>`

`a = b` invokes `a.Equals(b)`, with a null-safe wrapper: `null = null` is `true`; `null = nonNull` and `nonNull = null` are `false`. `<>` is the negation.

For classes, `=` always invokes `Equals`. Operator overloading of `=` and `<>` is not permitted on classes; classes customize equality by overriding `Equals`. Records and enums may overload `=` and `<>`.

`Object.ReferenceEquals(a, b)` compares object identity, ignoring any `Equals` override. Passing value-type arguments always returns `false` (each boxing creates a fresh object); the compiler warns.

<a id="sec-tostring"></a>
### 8.5 `ToString`

`ToString` is invoked implicitly inside string interpolation ([§18.5](#sec-interpolated-strings)) via virtual dispatch. The compiler-generated default returns:

- For classes: the type's qualified name.
- For positional records: a representation of fields, e.g. `"Point(X: 10, Y: 20)"`.
- For body records that satisfy the auto-equality conditions ([§9.2](#sec-body-records) — only auto-properties, no manually-declared `Equals` or `GetHashCode`): a representation of auto-properties, formatted as `"TypeName(Prop1: value1, Prop2: value2)"`. The auto-`ToString` is part of the same auto-equality package: a body record gets auto-`Equals`, auto-`GetHashCode`, and auto-`ToString` together, or it gets none and the type-name default applies. This matches the analogous design in C# records and lets `Console.WriteLine(person)` produce useful output for auto-equality records without ceremony.
- For body records that do not satisfy auto-equality conditions, and for enums: the type's qualified name.

<a id="sec-boxing"></a>
### 8.6 Boxing

When a value type is used where `Object` (or an interface implemented by the value type) is expected, the value is **boxed**: a heap-allocated wrapper holds the value. Boxed values are runtime-managed ([§28.9](#sec-runtime-managed-types)).

<a id="sec-boxing-of-the-unit-value"></a>
#### Boxing of the unit value

The unit type `()` has exactly one value, so all boxings of `()` may share a single heap object. Implementations are permitted (but not required) to intern boxed `()` such that every boxing produces the same reference. Programs should not rely on boxed unit values being either reference-equal or reference-distinct; both interpretations are conforming. The only guarantee is the standard one: a boxed `()` value, when unboxed, yields `()`.

<a id="sec-nullable-value-type-boxing"></a>
#### Nullable value-type boxing

When a nullable value type `T?` is boxed:

- If the value is `null`, the result is a null `Object` reference.
- If the value is non-null, the result is a boxed `T` (the non-nullable form). The nullable wrapper is stripped during boxing.

Unboxing reverses the process: a null `Object` reference unboxes to `null` of the target nullable type; a non-null boxed `T` unboxes to a non-null `T?`.

<a id="sec-boxing-of-borrow-bearing-records"></a>
#### Boxing of borrow-bearing records

Boxing a borrow-bearing record ([§9.7](#sec-records-and-disposal)) is subject to the same borrow-lifetime constraints as any other operation on a borrow-bearing value. The boxed copy on the heap holds the same borrowed references as the source record; it does not extend their lifetimes. The compiler enforces: a boxed borrow-bearing record value may not be stored in a location that could outlive the source of any borrow the record contains. In practice, this means a borrow-bearing record may be boxed for a short-lived interface call (e.g. passing to a method accepting `Object`) provided the boxed value does not escape the borrow's source scope. Storing a boxed borrow-bearing record in a field, returning it from a method, or capturing it in a closure that escapes the source scope is rejected by the same rules as the unboxed form ([§9.7](#sec-records-and-disposal), [§28.3](#sec-local-variables-ownership-and-borrows)).

<a id="sec-boxing-of-inline-arrays"></a>
#### Boxing of inline arrays

Non-owning inline arrays (`[N]T` and `[N]T?`) are ordinary value types and box per the general rule. When the element type is a reference type, the boxed array carries borrows; the same borrow-lifetime constraints apply as for boxed borrow-bearing records (above).

Move-only inline arrays (`[N owned]T` and `[N owned]T?`) cannot be boxed. Boxing a value type copies the value into a heap wrapper, but move-only types forbid copying ([§15.3](#sec-move-only-value-types)). A program that needs the contents of an owning inline array to cross a boxed-value boundary must explicitly transfer ownership — typically by draining the inline array into a heap collection (`OwnedArray<T>`), which is itself a class type and is passed by reference rather than boxed.

<a id="sec-dispose-is-not-on-object"></a>
### 8.7 `Dispose` is not on `Object`

`Dispose()` is class-only. The compiler generates `IDisposable` on every non-static class. Static classes, records, enums, primitives, and inline arrays do not implement `IDisposable`.


---

<a id="sec-records"></a>
## 9. Records

A record is a value type. Records are stack-allocated or stored inline in their containing object. Records have value semantics: assignment copies the value.

<a id="sec-positional-records"></a>
### 9.1 Positional records

A record may declare positional fields in parentheses after its name:

```bezel
record Point(X: int, Y: int);
record Person(Name: string, Age: int);
```

For each positional record, the compiler generates:

- A primary constructor whose parameters are the positional fields, in declaration order.
- Public read-only properties, one per positional field, with the same names.
- Value equality (`=`, `<>`) based on all positional fields.
- `GetHashCode` based on all positional fields.
- `ToString` formatted as `"TypeName(Field1: value1, Field2: value2)"`.

Positional field names are PascalCase by convention; the compiler does not transform them.

A positional-only record ends with `;`:

```bezel
record Point(X: int, Y: int);
```

<a id="sec-body-records"></a>
### 9.2 Body records

A body record declares members in a block. The body may contain methods, expression-bodied properties, additional constructors, and operator overloads:

```bezel
record Rectangle
  public readonly property Width: decimal;
  public readonly property Height: decimal;

  public constructor(width: decimal, height: decimal)
    Width := width;
    Height := height;
  end constructor;

  public method Area(): decimal => Width * Height;
end Rectangle;
```

<a id="sec-auto-generated-equality"></a>
#### Auto-generated equality

A body-only record receives auto-generated `=`, `<>`, `GetHashCode`, and `ToString` over its **auto-properties** (and only its auto-properties) when every member of the record is one of:

- An auto-property with a primitive type, an enum, `string`, another record with auto-equality, or a sealed reference type with content equality.
- A method or expression-bodied property whose value is a pure function of the record's auto-properties — these do not contribute storage, do not participate in equality, and do not disqualify the record from auto-equality.
- An overload of a non-equality operator.
- A constructor.

Such a record is said to be **auto-equality-bearing** — a derived type property ([§3.6](#sec-derived-type-properties)). The generated `=` compares each auto-property by its declared type's `=`; the generated `GetHashCode` combines per-property hashes; the generated `ToString` produces `"TypeName(Prop1: value1, Prop2: value2)"` over the auto-properties in declaration order (matching the positional-record form in [§8.5](#sec-tostring)). Computed (expression-bodied) properties are not part of the equality comparison or the ToString output even when they are visible on the public surface — equality and the auto-formatted representation are always over storage, never over derived values.

A body record is **not** auto-equality-bearing — and `=` on it without an explicit overload is a compile error — when it contains:

- An explicit field declaration (storage that is not an auto-property).
- A computed property whose getter has observable side effects, or whose value depends on state outside the record's auto-properties (an external clock, a global, a mutable field of a captured object).
- A non-trivial constructor body that performs work other than property initialization.

Authors who want explicit equality on a record outside the auto-equality-bearing set declare `=` and `<>` overloads in the body, along with `Equals(Object?)`, `Equals(T)`, and `GetHashCode` overrides as needed.

The compile error applies to the `=` and `<>` operators **and to direct calls of `.Equals(Object?)` and `.GetHashCode()`** on the record. Calling `someRecord.Equals(other)` or `someRecord.GetHashCode()` on a non-auto-equality-bearing record without a user-defined override is a compile error pointing to the same fix as the `=` error: declare custom equality, or simplify the record to an auto-equality-bearing form. The error covers `Equals(Object?)`, `Equals(T)` (when the record implements `IEquatable<T>`), and `GetHashCode`.

This rule is stricter than C#'s — C#'s `record` types always auto-generate equality, but C# *classes* fall back to reference equality on `.Equals(object?)` silently. Bezel records do not have C#'s "always auto-generate" guarantee (body records can opt out by including non-auto-property storage), and the silent reference-equality fallthrough on a value type would be a footgun: users expect content equality on records, and a body record's reference-equality `Equals` is virtually never the intended semantics. Forcing a compile error puts the choice in the user's hands.

The error is at the call site, not the declaration site; declaring a non-auto-equality-bearing record is fine on its own. The error fires when user code attempts to invoke equality on it. Calls through `Object`-typed receivers (`var o: Object := someRecord; o.Equals(...)`) bypass the static-type check and reach the inherited `Object.Equals` at runtime, returning reference equality — the static-type-erasure escape hatch is unavoidable but is rare in practice.

A body-only record with zero auto-properties (and no other storage) is auto-equality-bearing by the membership rule above. The generated `=` returns `true` for every pair of values; the generated `GetHashCode` returns a fixed implementation-defined constant; the generated `ToString` produces `"TypeName()"` — the type name followed by empty parens, the natural specialization of the property-listing format for zero properties. Such a record represents a singleton-by-value: every instance is equal to every other. The `with` expression on such a record is a compile error (no fields to replace), and `default(T)` produces the (single) value.

`with` ([§9.5](#sec-non-destructive-update-with)) and `default` ([§17.11](#sec-the-default-operator)) are independent of equality; they apply to any record that satisfies their own rules.

<a id="sec-combined-form"></a>
### 9.3 Combined form

A record may have both positional fields and a body:

```bezel
record Vector2(X: double, Y: double)
  public method Length(): double => Math.Sqrt(X * X + Y * Y);

  public static method operator +(a: Vector2, b: Vector2): Vector2 =>
    new Vector2(a.X + b.X, a.Y + b.Y);
end Vector2;
```

The auto-generated members are present and the body adds further members. Conflicts:

- The body may not declare a constructor with the same parameter list as the auto-generated primary constructor.
- The body may not declare a property with the same name as a positional field's auto-generated property.

<a id="sec-mutability"></a>
### 9.4 Mutability

Properties auto-generated from positional fields are read-only. A record declared only with positional fields is immutable.

In the body form, properties may be declared with any mutability. Mutable records are permitted but are inadvisable: value-equality and hashing rely on the value being stable.

<a id="sec-non-destructive-update-with"></a>
### 9.5 Non-destructive update: `with`

A `with` expression produces a new record value with named fields replaced:

```bezel
var p1 := new Point(1, 2);
var p2 := p1 with Y := 99 end;          // Point(1, 99)
var p3 := p2 with X := 0, Y := 0 end;   // Point(0, 0)
```

Syntax: `recordExpr with Name := value, ... end`. A trailing comma after the last assignment is permitted. The same `with ... end` shape applies to class instances after `new` (object initializer blocks, [§22.11](#sec-object-initializer-blocks)); the difference is semantic, not syntactic. For a record source, `with` produces a copy with the named fields replaced — the original record value is unchanged. For a class source (always a freshly-constructed instance), `with` assigns through the named properties' setters, mutating the new instance.

The named fields must be auto-generated positional-field properties (regardless of their read-only status), auto-properties declared in the record body, or public fields. For body-only records, `with` targets any auto-property that has backing storage; expression-bodied (computed) properties are not valid targets. All other fields keep their values from the source record. The result has the same static type as the source. The `with` operation constructs the copy directly from the source's storage; it does not invoke setters, so read-only positional-field properties are valid targets even though they have no setter.

`with` copies field values directly; it does not invoke the record's constructor. A constructor that validates cross-field invariants (e.g. `Width > 0 and Height > 0`) is not re-run during `with`. If a record enforces invariants that may be violated by independent field replacement, the record should provide a named method for constrained updates rather than relying on `with`.

`with` does not apply to existing class instances for non-destructive update; classes use property assignment. The `with ... end` syntax on a `new` class expression is an object initializer block ([§22.11](#sec-object-initializer-blocks)), which mutates the freshly-constructed instance rather than copying it.

The `with` keyword has three uses across the language: record non-destructive update (this section), object initializer blocks on `new` expressions ([§22.11](#sec-object-initializer-blocks)), and cancellation in `await for ... with token` ([§30.7](#sec-asynchronous-programming-async-iterators)). The three are syntactically distinguished by context: record `with` follows a record-typed expression; object initializer `with` follows a `new` class expression; `await for`'s `with` follows the iteration source and precedes `do`.

<a id="sec-records-and-inheritance"></a>
### 9.6 Records and inheritance

A record cannot derive from another record or from a class. A record may implement any number of interfaces:

```bezel
record Point(X: int, Y: int): IComparable<Point>
  public method CompareTo(other: Point): int =>
    case
      X <> other.X => case
        X < other.X => -1,
        _ => 1
      end,
      Y <> other.Y => case
        Y < other.Y => -1,
        _ => 1
      end,
      _ => 0
    end;
end Point;
```

Records are implicitly sealed. A record may not inherit from another record or from a class (other than the implicit `Object` root). A record may implement one or more interfaces; the interface names appear after a colon following the record name or positional fields.

<a id="sec-records-and-disposal"></a>
### 9.7 Records and disposal

Records cannot have destructors, `owned` fields, or implement `IDisposable`. Records also cannot have inline-array fields whose slots carry the `owned` marker (`[N owned]T?` or `[N owned]T`); such fields are move-only value types ([§15.3](#sec-move-only-value-types)), and records support only copy semantics. Reference-typed fields in records are always borrows; the record does not own what they point to. See [§28](#sec-resource-management).

A record type is a **borrow-bearing record** — a derived type property ([§3.6](#sec-derived-type-properties)) — if it holds at least one *borrow-carrying* field of a non-runtime-managed type. A field is borrow-carrying when:

- Its type is a reference type (class or interface, nullable or not, including the `T?` forms).
- Its type is an inline array whose element type is a reference type (`[N]C`, `[N]C?` for class-or-interface C).
- Its type is another borrow-bearing record (transitivity).

Reference-typed fields whose types are runtime-managed (`string`, function types, boxed value types) do not make the record borrow-bearing because runtime-managed values have unbounded lifetime by construction; the same is true of inline-array fields whose element type is a runtime-managed reference type. Values of a borrow-bearing record are subject to the borrow lifetime rules of [§28.3](#sec-local-variables-ownership-and-borrows). In particular:

- A borrow-bearing record value may not outlive the source of any borrow it contains. For inline-array-field borrows, this applies element-wise: every slot's source must outlive the record.
- A borrow-bearing record may not be stored in a static field of any type, *except* when every reference-typed field (and every reference-typed slot of every inline-array field) of the value is `null` (the **null-source** form). A static field of borrow-bearing record type may hold a value whose borrow-carrying components are all null; the type's accessors must handle this state explicitly. This exception exists to allow standard library "sentinel" values like `CancellationToken.None`, whose semantics are defined by the absence of a source rather than by holding one.
- A borrow-bearing record may not be returned from a method whose return-type rules cannot establish a sufficient lifetime for every borrow the record carries.
- A borrow-bearing record may not be captured by a closure that escapes its source's scope.

For typical short-lived uses — a record passed by value to a synchronous call frame, a `CancellationToken` ([§30.8](#sec-cancellation)) handed to an async call that completes before the source is disposed — these rules are automatic and require no annotation.

As noted above, a record whose only reference-typed fields are of runtime-managed types (`string`, function types, boxed value types) is **not** a borrow-bearing record; runtime-managed values have unbounded lifetime by construction, so no borrow-lifetime rules apply.

<a id="sec-serialization-synthesis"></a>
### 9.8 Serialization synthesis

A record or enum that declares conformance to `ISerializable` and/or `IDeserializable` (standard library [§2.14](Bezel_Standard_Library.md#sec-serialization-vocabulary)) and satisfies the **synthesis-eligibility rule** below has the corresponding member bodies generated by the compiler. This is the same kind of opt-in-by-conformance, structural compile-time generation as auto-equality ([§9.2](#sec-body-records)); the eligibility rule, the recursive component check, and the compile-error discipline deliberately mirror it. Declaring the conformance is the opt-in; the synthesis is automatic when eligible and a compile error at the conformance declaration when not.

<a id="sec-synthesis-eligibility"></a>
#### Synthesis-eligibility

A type is **serialization-eligible** — a derived type property ([§3.6](#sec-derived-type-properties)) — when it is a record or an enum and every serialized component is itself serializable:

- **Enums** are always serialization-eligible. The synthesized form writes the member *name* as a string (via `WriteString`) and reads it back by matching against the declared members; a value that is not a declared constant (a "raw" enum value, [§10](#sec-enums)) raises `SerializationException` on write and on read. Writing the name rather than the underlying integer makes the encoded form stable against reordering or renumbering of members and self-describing in text formats; programs that need the integer form serialize `int(e)` (or the underlying-typed cast) explicitly through a hand-written `Serialize`.
- **Positional records** ([§9.1](#sec-positional-records)) are serialization-eligible when every positional field's type is serialization-eligible. The synthesized form is an object whose field names are the positional-field names in declaration order.
- **Body records** ([§9.2](#sec-body-records)) are serialization-eligible under exactly the auto-equality membership conditions of [§9.2](#sec-body-records) — only auto-properties (no explicit-field storage, no side-effecting computed members) — *and* additionally every auto-property's type is serialization-eligible. The serialized components are the auto-properties in declaration order; computed (expression-bodied) properties are not serialized, exactly as they do not participate in equality or `ToString`.
- A component type is **serialization-eligible** when it is: a primitive (`bool`, the integer types, `float`, `double`, `decimal`, `char`); `string`; an enum; another serialization-eligible record; a serialization-eligible **synthesized class** (a class with a `serialized tree`, `serialized graph`, or `serialized cyclic` clause, below); a nullable `T?` whose `T` is serialization-eligible (encoded as the value or `WriteNull`/`TryReadNull`); or a standard sequence collection (`Array<T>`, `OwnedArray<T>`, `LinkedList<T>`, etc., and inline arrays `[N]T`) whose element type is serialization-eligible (encoded with `BeginArray`/`EndArray`). A standard dictionary (`Dictionary<K, V>`, `OwnedDictionary<K, V>`) is serialization-eligible when both `K` and `V` are serialization-eligible (encoded as an array of key/value pairs to admit non-string keys uniformly).

A type that declares `: ISerializable` or `: IDeserializable` but is **not** serialization-eligible is a **compile error at the conformance declaration**, naming the first offending member and the reason (mirroring the auto-equality non-membership error of [§9.2](#sec-body-records), but reported at the declaration rather than the use site, because the conformance declaration is the explicit opt-in and the obstruction should be reported where the promise is made). The error names the remedy: remove the conformance, make the offending component serializable, or hand-write `Serialize`/`Deserialize` (which suppresses synthesis — see below).

<a id="sec-tree-shaped-class-synthesis"></a>
##### Tree-shaped and shared-reference class synthesis

A class may opt into serialization synthesis by declaring a `serialized` graph-shape clause on its header (in addition to the `: ISerializable`/`: IDeserializable` conformance). There are two forms, asserting progressively weaker shapes:

- **`serialized tree`** asserts the class's serialized object graph is a **tree**: along the serialized edges, no two references alias the same object, and there are no cycles.
- **`serialized graph`** asserts the serialized object graph is a **directed acyclic graph**: serialized edges *may* alias a shared object (the same object reachable by more than one path), but there are no cycles.

A class with either clause is serialization-eligible when every serialized component is itself serialization-eligible by the rule above (which recursively admits other `serialized tree`/`serialized graph` classes and owned collections of them). The two clauses differ only in how aliasing is treated; cycles are rejected by both. (`sealed` is irrelevant to serialization: a sealed class follows the same eligibility rules and wire form as any other class, clarified in v0.136.)

When a class-typed component appears in a **record**, it participates only as an *owning* edge: the record field must be `owned` and its class type must carry a graph-shape clause. A record cannot hold a *borrowing* class-typed component in its serialized set — a record declares no owning spanning tree against which a back-edge could resolve — and such a record is not serialization-eligible (a compile error at the conformance declaration, naming the borrowing field; clarified in v0.136).

The serialized components of a class are its `owned` and value-typed fields and auto-properties together with its **borrowing** reference fields (non-owning class-typed fields whose target is owned elsewhere in the same serialized graph), in declaration order. The owning (`owned`) and value edges form the **owning spanning tree**; the borrowing fields are **back-edges** into that tree. This owning-tree/back-edge partition is read directly from the `owned`/borrow annotations the language already requires — it is not inferred and not guessed.

**Synthesized `Serialize`.** The serializer assigns each owned object an identity tag the first time it is reached through an owning edge, and writes the object's components there. A borrowing field is *not* written inline; it is written as a reference to the tag of the object it borrows (which is necessarily present in the owning tree, because a borrow's lifetime is bounded by an owner that outlives it — the language already guarantees the owner exists within the graph). Under `serialized tree`, the serializer additionally asserts no tag is ever reached twice through owning edges and no reference targets an already-emitted-as-reference object; a violation is the programmer's contract breach, surfaced at read (below). Under `serialized graph`, a shared object is written once at its owning-tree position and referenced by tag elsewhere.

**Synthesized `Deserialize`.** The synthesized factory has the class return shape of [§2.14](Bezel_Standard_Library.md#sec-serialization-vocabulary) — `Deserialize(d: IDeserializer): owned T` (a class instance is an owned heap value transferred to the caller, [§28.8](#sec-ownership-transfer)). It reconstructs the owning spanning tree first, in **owner-before-borrower order**: each owned object is constructed by an ordinary call to its constructor, exactly as in the tree case, and every borrowing field is supplied **as a constructor argument** — the reference is resolved against the tag→object table, which already contains the target because the owning tree is built in an order where every borrow target precedes every borrower. A directed acyclic graph always admits such an order; this is precisely the property `serialized graph` asserts. Because each borrowing field is bound *through the constructor*, before any constructor body runs, there is no program point at which a constructed object has an unbound non-nullable reference field: [§7.9](#sec-definite-assignment-for-non-nullable-fields) is satisfied unmodified, with no nullable-field relaxation and no observable intermediate state. The ownership contract for the owning edges lives at the constructor signature, a static program point, exactly as for records and tree-shaped classes; the borrowing arguments are ordinary borrows whose lifetime is established by the constructed owning tree.

**Constructor selection (clarified in v0.136).** When the class declares more than one constructor, the synthesized `Deserialize` uses the *unique* constructor whose parameter list binds exactly the serialized components and borrowing fields, matched by name (order-insensitive; `serialized "<name>"` renames do not affect the parameter-name match, which uses the declared identifiers). If no constructor matches, or more than one does, the conformance declaration is a compile error naming the candidate constructors — the same compile-error discipline as the eligibility rule, reported where the opt-in is declared.

Because graph shape is an *assertion the compiler cannot verify statically*, the synthesized deserializer **enforces it dynamically**:

- Under `serialized tree`, any repeated object identity in the input (a reference where the tree assertion forbids one) raises `SerializationException` rather than silently duplicating — unchanged from the prior tree-only behavior.
- Under `serialized graph`, a reference whose target has already been constructed is bound to that object (identity preserved, not duplicated — the specific failure mode of reference-unaware serializers, which Bezel refuses). A **cycle** — a reference encountered while its own target is still being constructed (the target is not yet in the tag→object table because its construction has not completed) — raises `SerializationException`. A cycle has no owner-before-borrower order, so it is exactly the case `serialized graph` does not assert and the deserializer detects rather than mis-handles.

`serialized tree` and `serialized graph` are **type-scoped** because graph shape is an intrinsic property of the type's serialization contract — a class serializes with the same shape for every caller, format, and call site — which is the placement criterion of rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection) (contrast the deserializer's *operation*-scoped unknown-field policy of stdlib [§2.15](Bezel_Standard_Library.md#sec-concrete-serialization-formats), a property of a particular read).

**The graph-shape clauses are exhaustive as of v0.124.** Every class object graph is a tree, a directed acyclic graph, or a general (possibly cyclic) graph; `serialized tree`, `serialized graph`, and `serialized cyclic` cover all three. Declaring a weaker shape than the data actually exhibits (`serialized tree` on a graph that aliases, or `serialized graph` on a graph that cycles) is a contract violation surfaced as a `SerializationException` at deserialization — never a silently wrong result. `serialized cyclic` makes no shape restriction on the data, but it imposes a *static* eligibility restriction on the participating classes (the self-contained-constructor rule below); a class that cannot satisfy it hand-writes `Serialize`/`Deserialize` (still fully supported), the only remaining reason to do so.

A field typed as a base class holding a derived instance — **polymorphic dispatch** — is covered as of v0.122 via the explicit `polymorphic` registry below; absent that registry, a base-class-typed component remains a synthesis-eligibility compile error under any graph-shape clause (the synthesized `Deserialize` would have no way to choose the concrete type), and such components must be hand-written.

<a id="sec-cyclic-class-synthesis"></a>
##### Cyclic class synthesis

`serialized cyclic` permits a serialized object graph that contains cycles — a reference whose target is the object currently under construction, or any object not yet fully constructed. This is the one shape `serialized graph` rejects (it raises on exactly such a reference). The wire form is the identity/`$ref` mechanism the [shared-reference DAG case](#sec-tree-shaped-class-synthesis) already established, with one extension: a `$ref` may target an object whose construction has not completed (a forward reference into the in-progress group). No new wire vocabulary is introduced; only the deserializer's handling of an as-yet-unbuilt target changes.

**The self-contained-constructor eligibility rule.** A class is **cyclic-serialization-eligible** only if its construction is *self-contained*. "Construction" here means everything that runs while an instance is being built: the constructor body, every constructor it chains to via `this(...)` or `base(...)` ([§12.8](#sec-constructor-chaining)) transitively up the chain, and every field and property initializer that runs as part of that construction ([§11.1](#sec-class-declaration)). All of this construction-time code, taken together, may assign the instance's own fields and may read its own constructor parameters and its own value-typed and `owned` fields, but it must **not**:

1. read any field that is a back-edge in the serialized graph (a borrowing reference participating in the cycle);
2. call any instance method on `this` (directly, or via `self`, or implicitly);
3. allow `this` to escape — pass it as an argument, store it in any location, capture it in a closure, or return it; or
4. call a virtual or abstract method.

The rule is **transitive over the constructor chain and over field/property initializers**: a base-class constructor or an initializer expression that violates any clause makes the class cyclic-serialization-ineligible exactly as a violation in the most-derived constructor body would. For the field- and property-initializer portion this is largely a restatement of an existing universal restriction rather than a new constraint: [§22.8](#sec-field-initializers) already forbids *every* class's field initializers from referencing `this`, instance members, `base`, virtual methods, or any expression that would observe `this` indirectly, so an initializer structurally cannot read a back-edge (clause 1), call an instance or virtual method on `this` (clauses 2, 4), or let `this` escape through a `this`-observing expression (clause 3). The genuinely load-bearing extension here is the **constructor-chain** coverage: a chained or base constructor's *body* (step 1/3 of [§22.8](#sec-field-initializers)'s sequence) is ordinary construction-time code that [§22.8](#sec-field-initializers) does *not* restrict — it may freely read its own fields and call virtual methods — so the self-contained rule must reach into it, and does. This is a strict subset of the escape analysis the borrow checker already performs ([§28.3](#sec-local-variables-ownership-and-borrows)); it requires no new analysis machinery, only that the existing analysis be run over the transitive construction-time code rather than a single body. A class declared `serialized cyclic` (or reachable as a serialized component of a `serialized cyclic` group) whose construction is not self-contained is a **compile error at the conformance declaration**, naming the offending constructor, chained constructor, or initializer and the clause it violates and the remedy (make construction self-contained, or hand-write `Deserialize`). The restriction is not a runtime check and never a silent fault.

**Two-phase construction.** The synthesized `Deserialize` for a `serialized cyclic` group is a single operation with two phases over the set of objects in the group:

1. *Allocate-and-own.* Every object in the group is constructed by an ordinary call to its constructor, in **owning-tree post-order** (every `owned` child constructed before the parent that owns it) — the same owner-before-borrower order the [shared-reference DAG case](#sec-tree-shaped-class-synthesis) uses, and always available because the owning edges form a tree: exclusive single ownership with move semantics ([§28.3](#sec-local-variables-ownership-and-borrows)) makes an *owning* cycle unrepresentable (a cycle can only be closed by a *borrowing* back-edge, never an owning one). Owning (`owned`) and value components are read and passed as constructor arguments exactly as for `serialized tree`/`serialized graph` (the owned child already exists, by the post-order). A back-edge field (a cycle-participating borrow) is left in a defined not-yet-bound state; because every participating constructor is self-contained, no constructor body reads any back-edge field, calls a method that could, or lets `this` reach code that could.
2. *Bind-backs.* After every object in the group exists, each back-edge field is assigned to its now-constructed target, resolved through the `$ref` identity table.

The result is released to the caller only after phase two completes. **Definite assignment ([§7.9](#sec-definite-assignment-for-non-nullable-fields)) is preserved unmodified**: a back-edge field is non-nullable and is never *observed* unset, because the only code that runs between phase one and phase two is self-contained constructor bodies, which the eligibility rule guarantees cannot read a back-edge. The intermediate state is provably unobservable; this is the discharged proof of rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection) made normative. This is the same guarantee `serialized graph` provides by a different argument (DAG: every back-edge bound through the constructor, so there is no intermediate state; cyclic: back-edges bound in a window provably free of observers), and in neither case is [§7.9](#sec-definite-assignment-for-non-nullable-fields) relaxed, excepted for synthesized code, or worked around with nullability.

**Enforcement and the contract.** `serialized cyclic` asserts nothing the deserializer must dynamically reject about *shape* — a tree or a DAG is a degenerate cyclic graph and deserializes correctly under the two-phase construction. What it does require is the static eligibility rule, enforced at compile time. The synthesized `Serialize` writes the group using the identity/`$ref` table exactly as `serialized graph` does (a cycle simply means a `$ref` resolves to an object earlier in the same in-progress walk). A class may declare `serialized cyclic` conservatively even when its graphs happen to be acyclic; the only cost is the self-contained-constructor restriction. `serialized cyclic` is **type-scoped** for the same reason `serialized tree`/`serialized graph` are: graph shape is an intrinsic property of the type's serialization contract (rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection)). It composes with the `polymorphic` registry below — a cyclic group may contain polymorphic edges, each resolved through its registry and then reconstructed under its concrete type's own clause.

<a id="sec-polymorphic-dispatch-synthesis"></a>
##### Polymorphic dispatch

A base class whose serialized components may, at run time, hold instances of registered derived classes declares a `polymorphic` registry as part of its graph-shape clause:

```
serialized graph polymorphic ("circle": Circle, "square": Square)
```

The registry is a closed, explicitly written map from a **wire discriminator string** to a **permitted concrete derived type**. There is no reflective discovery of subtypes: the only types the synthesized deserializer will ever construct for this hierarchy are those the registry names. This is the [§5.18](Bezel_Design_Rationale.md#sec-type-identity-not-reflection) explicit-registration-never-reflection principle applied directly — it is the reason a serialized type discriminator cannot be turned into an arbitrary-type construction primitive (the defect that made reflective `$type` binders a remote-code-execution vector elsewhere).

Each registry entry's discriminator must be a non-empty string literal, unique within the clause. Each named type must be assignable to the declaring class and must itself carry a graph-shape clause (`serialized tree`, `serialized graph`, or `serialized cyclic`) making it serialization-eligible; a registered type that is not serialization-eligible is a compile error at the registry. A `polymorphic` registry may appear only together with a graph-shape assertion (`serialized tree`/`serialized graph`/`serialized cyclic`); the assertion governs how each concrete instance's own edges reconstruct, and the registry governs which concrete type is chosen — the two are orthogonal and compose. The declaring base class may be concrete (it registers a discriminator for itself like any other case) or abstract (it registers none for itself; an input discriminator naming the base is then a `SerializationException`).

**Synthesized `Serialize`.** When writing a component whose static type is a polymorphic base, the synthesized serializer writes the discriminator of the value's concrete run-time type as a distinguished leading field (the discriminator field name is `$type`, reserved within an object written for a polymorphic base; it precedes all component fields), followed by that concrete type's own synthesized form. A run-time type not present in the registry is a `SerializationException` at serialization — the closed set is enforced on both directions, never silently widened.

**Interaction with the identity/`$ref` mechanism.** When a polymorphic object is also a shared or cyclic reference target (the graph aliases or cycles through it), the discriminator and the identity tag coexist by a single rule: the object's **first** occurrence carries its identity tag and, because it is polymorphic, its `$type` discriminator, then its body; every **subsequent** occurrence is a pure `$ref` to that identity tag and carries **no** `$type` and no body. The concrete type is fixed at the first occurrence; a back-reference neither repeats nor may contradict it. On read, a `$ref` is resolved against the identity table (or deferred to phase-two bind-back under `serialized cyclic`) with no further dispatch — dispatch happened once, at the referent's first occurrence. This makes the wire form unambiguous for independent implementations regardless of how polymorphism, aliasing, and cycles combine.

**Synthesized `Deserialize`.** The discriminator is read **first**, before any component of the object, and consumed by dispatch — it is not bound as an ordinary component and is never subject to field-name customization, `serialized optional`, or the unknown-field policy (it is structural, not a data field). The deserializer looks the discriminator up in the registry, selects the concrete type's synthesized `Deserialize`, and delegates the remainder of the object to it (returning `owned Base`, the derived instance transferred as the base type, [§28.8](#sec-ownership-transfer)). A discriminator string absent from the registry raises `SerializationException` — the closed-set assertion enforced dynamically, the same assert-and-enforce discipline as graph shape. Because the discriminator is consumed before component binding, it has no interaction with back-edge binding (whether the constructor-argument binding of [shared-reference DAG synthesis](#sec-tree-shaped-class-synthesis) or the two-phase binding of [cyclic synthesis](#sec-cyclic-class-synthesis)): dispatch selects *which* concrete type's reconstruction path runs; that path is then exactly the tree, graph, or cyclic reconstruction already specified for that concrete type.

`serialized polymorphic` is **type-scoped** for the same reason graph shape is: the permitted-subtype set is an intrinsic property of the base type's serialization contract — the hierarchy serializes against the same registry for every caller, format, and call site — which is the placement criterion of rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection). Polymorphic dispatch **composes uniformly with every graph-shape clause**: it composes with `serialized tree`, `serialized graph`, and `serialized cyclic`, and a registered type is reconstructed under whichever of those its own clause asserts. A `serialized cyclic` group may contain polymorphic edges; dispatch selecting a concrete type does not interact with that type's two-phase construction or its self-contained-constructor obligation — the discriminator is consumed before any component or back-edge binding, so polymorphism and the cyclic unobservability argument are fully orthogonal.

Classes that declare **neither** clause remain outside synthesis entirely and participate only by hand-writing `Serialize` and a recognized static `Deserialize` ([§2.14](Bezel_Standard_Library.md#sec-serialization-vocabulary) of the standard library), exactly as before. A graph-shape clause is the only route by which a class enters synthesis, and it is deliberately explicit: unlike a record (whose value semantics make shape automatic and unannotated), a class's reference semantics make graph shape a property the programmer must assert and the deserializer must police.

<a id="sec-synthesized-serialize"></a>
#### Synthesized `Serialize`

For an eligible record, the synthesized `Serialize(s: ISerializer)` body is:

1. `s.BeginObject(n)` where `n` is the number of serialized components.
2. For each component in declaration order: `s.WriteFieldName("<name>")` followed by the component's encoding — `s.WriteInt`/`WriteString`/etc. for a primitive or `string`; the nested value's `Serialize(s)` for a serializable record; the enum-name string for an enum; `BeginArray`/per-element encoding/`EndArray` for a collection; `WriteNull` for a null nullable.
3. `s.EndObject()`.

For an eligible enum, the synthesized `Serialize` is `s.WriteString("<member-name>")` (no surrounding object), raising `SerializationException` if the value is not a declared constant.

<a id="sec-synthesized-deserialize"></a>
#### Synthesized `Deserialize`

The synthesized recognized static factory has the value-type return shape of [§2.14](Bezel_Standard_Library.md#sec-serialization-vocabulary) — `Deserialize(d: IDeserializer): T` for a record or enum `T` (plain by-value; no `owned`, records and enums being value types). Its body:

1. `d.BeginObject()`; then repeatedly `d.ReadFieldName()` and dispatch on the name, reading each recognized component with the mirror operation (`d.ReadInt`, the nested type's `Deserialize`, etc.) into a local. When a read field name matches no declared component, the synthesized code calls `d.SkipValue()` ([§2.14](Bezel_Standard_Library.md#sec-serialization-vocabulary)) — it does not itself decide whether an unknown field is an error. Whether reaching an unknown field is permitted is the *deserializer's* configured policy: a strict deserializer (the default) raises `SerializationException` from `ReadFieldName`/`SkipValue` on an unrecognized field; a deserializer configured to tolerate unknown fields skips the value and continues. Then `d.EndObject()`.
2. Construct the result by calling the record's constructor with the read components in positional order (for a positional record, the primary constructor; for a body record, the constructor the auto-equality machinery already relies on). For an enum, read the name string and map it to the declared constant.

Field order in the input need not match declaration order — dispatch is by field name. A **missing** required component (one neither present nor `serialized optional`), a **duplicate** field name, a type mismatch, or a name not matching any enum constant raises `SerializationException` unconditionally — these are structural faults in the input regardless of policy. An **unknown** field (present in the input, matching no declared component) is the one case governed by *operation* policy rather than the synthesized code: it is an error by default and is skipped when the deserializer is configured to ignore unknown fields (stdlib [§2.15](Bezel_Standard_Library.md#sec-concrete-serialization-formats)). This policy is deliberately a property of the deserialization operation, not an annotation on the type — the same type may be read strictly in one context and leniently in another, a decision that belongs to the caller with the input, not to the type's contract (rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection)). The synthesized code is identical regardless of policy: it always skips an unrecognized field via the deserializer; strictness lives entirely in the deserializer. The construction is an ordinary constructor call at a static program point, so a record's field types, nullability, and definite-assignment rules are checked normally; this is the structural reason synthesis is sound where reflection was not ([§5.18](Bezel_Design_Rationale.md#sec-type-identity-not-reflection)).

<a id="sec-field-name-customization"></a>
#### Field-name customization

By default the synthesized form uses each component's declared name as its serialized field name (and each enum member's declared name as its serialized value). A `serialized` clause on a positional field, an auto-property, or an enum constant customizes this. The clause vocabulary is closed (no user-defined forms, no parameters interpreted by external machinery, no runtime introspection — this is a compiler-recognized declaration modifier in the sense of [§18](Bezel_Design_Rationale.md#sec-things-deferred)'s narrow-modifier escape hatch, deliberately *not* an attribute system). It expresses three independent effects — rename, exclude, tolerate — where rename and tolerate may combine and exclude is mutually exclusive with both:

- **`serialized "<name>"`** — the component is serialized and deserialized under `<name>` instead of its declared identifier. The string must be a non-empty literal and must be unique among the serialized names of the type's components (a collision is a compile error at the conformance declaration, mirroring the eligibility-error discipline). On an enum constant, the constant is encoded as `<name>` rather than its identifier.
- **`serialized optional`** — read/write tolerance for that component. On write, if the component is a nullable `T?` whose value is null, the field is omitted (rather than written as `WriteNull`); otherwise it is written normally. On read, a missing field yields `default(T)` instead of raising `SerializationException`; a present field is read normally. This is the per-field, *missing*-field tolerant-mode opt-in; it is orthogonal to the *unknown*-field policy, which is a property of the deserialization operation (the deserializer's configuration, stdlib [§2.15](Bezel_Standard_Library.md#sec-concrete-serialization-formats)) rather than of the member or the type — the two leniencies compose freely. Not admitted on enum constants.
- **`serialized "<name>" optional`** — both of the above on the same component: renamed *and* tolerant. This is the common real-world case (an externally-named field that may be absent), so the grammar admits the combination directly rather than forcing a choice.
- **`serialized skip`** — the component is excluded from both directions entirely. The synthesized `Serialize` does not write it; the synthesized `Deserialize` does not read it and supplies the component's `default(T)` ([§17.11](#sec-the-default-operator)) to the constructor. A `serialized skip` component whose type has no well-defined default is a compile error at the conformance declaration (the synthesized `Deserialize` would have no value to construct with). `skip` is mutually exclusive with a name or `optional` (excluding a field and renaming/tolerating it are contradictory; the grammar does not admit the combination). Not admitted on enum constants ([Appendix A](#sec-appendix-a) grammar note): an enum value is a single member, not an omittable field.

The clause attaches to the member it affects and is total: a component with no `serialized` clause is coded under its own name, always. There is no separate exhaustive key list to keep synchronized, so adding a component never silently drops it from serialization — the failure mode of an all-or-nothing customization list (rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection)) does not arise. A `serialized` clause on a member of a type that declares no serialization conformance, or on a member of a type whose serialization is hand-written (synthesis suppressed — see below), is a compile error: the clause only has meaning as an input to synthesis, and silently ignoring it would be the kind of inert-looking-but-load-bearing annotation [§18](Bezel_Design_Rationale.md#sec-things-deferred) warns against.

`serialized skip` interacts with the eligibility rule by *removing* its component from consideration: a field that would otherwise make the type ineligible (a non-serializable component type) but is marked `serialized skip` does not disqualify the type, because it is no longer a serialized component. This is the supported way to make an otherwise-eligible record serializable when one field is of a non-serializable type and is not needed in the encoded form.

<a id="sec-suppression-and-interaction"></a>
#### Suppression and interaction

Hand-writing `Serialize` (respectively a recognized static `Deserialize`) **suppresses** synthesis of that direction, exactly as a manually declared `Equals`/`GetHashCode` suppresses auto-equality. The two directions are independent: a type may synthesize one and hand-write the other. Declaring `: ISerializable` without an eligible structure and without a hand-written `Serialize` is the compile error above; declaring it *with* a hand-written `Serialize` is always valid regardless of eligibility (the hand-written body is the contract).

Synthesis composes with auto-equality and `ToString`: a body record may be simultaneously auto-equality-bearing and serialization-eligible; the feature sets are independent packages over the same auto-properties. The `serialized` clause affects only serialization synthesis — it has no effect on auto-equality, `ToString`, `with`, or any other use of the member. v0.117 adds one closed grammar production for the `serialized` clause ([Appendix A](#sec-appendix-a)); apart from that clause, `: ISerializable, IDeserializable` remains ordinary interface-conformance syntax ([§11](#sec-classes-and-interfaces)) and the normative content is the eligibility rule, the generated-body semantics, and the customization semantics defined here. The wire shapes are normative so that independent implementations agree; the rationale for the specific choices (names not integers for enums, declaration order, key/value-pair dictionaries) and for the modifier-not-attribute design is in [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection).

---

<a id="sec-enums"></a>
## 10. Enums

An enum is a value type whose values are drawn from a fixed, named set of constants. Each value is backed by an integer at runtime. An enum is not implicitly convertible to or from its underlying integer type.

<a id="sec-basic-syntax"></a>
### 10.1 Basic syntax

```bezel
enum Priority
  Low, Medium, High;
end Priority;
```

This declares constants `Priority.Low`, `Priority.Medium`, `Priority.High` of type `Priority`. The default underlying values are 0, 1, 2 — assigned in declaration order from zero.

<a id="sec-explicit-values"></a>
### 10.2 Explicit values

```bezel
enum HttpStatus
  Ok := 200,
  Created := 201,
  NotFound := 404,
  ServerError := 500;
end HttpStatus;
```

Values must be compile-time constants of the underlying type. Unspecified values continue from the previous explicit value, incrementing by 1.

<a id="sec-underlying-type"></a>
### 10.3 Underlying type

The default underlying type is `int`. A different integer type may be specified with a colon:

```bezel
enum HttpStatus: short
  Ok := 200,
  NotFound := 404;
end HttpStatus;
```

The underlying type must be one of `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`. All declared values must fit in the underlying type's range.

<a id="sec-methods-properties-static-members"></a>
### 10.4 Methods, properties, static members

An enum may declare instance methods, expression-bodied read-only properties, static methods, and static read-only fields:

```bezel
enum Priority
  Low, Medium, High;

  public method IsUrgent(): bool => this = High;

  public property Name: string => case this
    Low => "Low",
    Medium => "Medium",
    High => "High"
  end;

  public static readonly All: Array<Priority> := [Low, Medium, High];

  public static method Parse(text: string): Priority?
    return case text
      "Low" => Low,
      "Medium" => Medium,
      "High" => High,
      _ => null
    end;
  end Parse;
end Priority;
```

Inside an instance method, `this` refers to the current enum value.

The `Name` property's `case` lists only the declared constants. If an out-of-range enum value reaches the property — possible when an integer is cast to the enum type via `Priority(int)` and the integer is not one of `0`/`1`/`2` ([§5.4](#sec-enum-casts)) — the case expression raises `MatchFailureException` per the standard non-exhaustive-`case` rule ([§19.6](#sec-exhaustiveness)). Code that needs total coverage over the value space should add a discard arm (`_ => "Unknown"`) or use the integer-checked cast forms.

Static readonly fields on enums are initialized lazily on first use, following the same rules as class static fields ([§22.13](#sec-static-initialization-order)). Enum constants are compile-time values and are available before any static readonly initializer runs; a static readonly field initializer on an enum may freely reference the enum's declared constants.

<a id="sec-restrictions"></a>
### 10.5 Restrictions

An enum may not declare:

- Instance fields (the runtime representation is the underlying integer).
- Constructors.
- Block-form properties or properties with backing storage.
- Nested types.
- Indexers.
- Inheritance from another type.

Enum methods may not be declared `virtual`, `abstract`, `override`, or `sealed override`. Enums are implicitly sealed (no inheritance) and have no virtual-dispatch surface, so these modifiers carry no meaning on enum members and are rejected at compile time. Enum methods are always dispatched statically.

An enum may implement one or more interfaces. The interface names appear after the underlying type (if any) following a colon. The enum must provide matching public members for every interface member:

```bezel
enum Priority: IComparable<Priority>
  Low, Medium, High;

  public method CompareTo(other: Priority): int =>
    case
      int(this) < int(other) => -1,
      int(this) > int(other) => 1,
      _ => 0
    end;
end Priority;
```

When both an underlying type and interfaces are specified, the underlying type comes first: `enum Priority: int, IComparable<Priority>`.

<a id="sec-case-matching"></a>
### 10.6 Case matching

Inside a `case` whose scrutinee is an enum, the enum prefix may be omitted:

```bezel
case p
  Low => Console.WriteLine("low"),
  Medium, High => Console.WriteLine("medium or high")
end;
```

---

<a id="sec-classes-and-interfaces"></a>
## 11. Classes and Interfaces

<a id="sec-class-declaration"></a>
### 11.1 Class declaration

A class is a reference type with optional inheritance, optional interfaces, and a member body:

```bezel
public class BankAccount: IBankAccount
  // members
end BankAccount;
```

A class may declare at most one base class and any number of interfaces. The base class, if any, is listed first, followed by interfaces, all comma-separated. There is no semicolon after the inheritance list.

If no base class is specified, the implicit base is `Object`.

<a id="sec-primary-constructors"></a>
#### Primary constructors

A class may declare parameters in parentheses after its name, analogous to a record's positional fields:

```bezel
public class BankAccount(name: string, initialBalance: decimal): IBankAccount
  private _name: string := name;
  private _balance: decimal := initialBalance;
end BankAccount;
```

The compiler generates a primary constructor whose parameter list matches the parenthesized declaration. The primary constructor's parameters are in scope throughout the class body — including field initializers, property initializers, the inheritance clause's base call, and the bodies of methods, properties, constructors, and destructors. Parameters referenced only in initializers do not generate backing fields; parameters referenced in method, property, constructor, or destructor bodies are captured into compiler-generated private fields. The capture is invisible at the source level — the parameter name resolves to the parameter in initializer positions and to the captured field in body positions, and the two reads observe the same value.

For each primary-constructor parameter, the compiler does **not** generate an auto-property (unlike records). The parameters are initialization-time values, optionally captured for body use; the class author decides which fields or properties to declare publicly. This keeps the class's public API explicit.

If the class has a base class that requires constructor arguments, the base's arguments appear in parentheses after the base class name in the inheritance list:

```bezel
public class SavingsAccount(name: string, balance: decimal, rate: decimal): BankAccount(name, balance)
  private _rate: decimal := rate;
end SavingsAccount;
```

This desugars to a primary constructor that chains to `base(name, balance)` before executing field initializers.

A class with a primary constructor may also declare additional constructors in its body using the `constructor` keyword; such constructors must chain to the primary constructor via `this(...)`:

```bezel
public class BankAccount(name: string, initialBalance: decimal)
  private _name: string := name;
  private _balance: decimal := initialBalance;

  public constructor(name: string)
    this(name, 0m);
  end constructor;
end BankAccount;
```

The primary constructor's visibility follows the class's visibility. The body may not redeclare a constructor with the same parameter list as the primary constructor.

Primary constructors interact with `owned` parameters: a primary-constructor parameter may be declared `owned`, and the same ownership-transfer rules as regular constructor parameters apply:

```bezel
public class ResourceHolder(owned resource: Stream)
  private owned _resource: Stream := resource;
end ResourceHolder;
```

<a id="sec-interface-declaration"></a>
### 11.2 Interface declaration

```bezel
interface IBankAccount
  method Deposit(amount: decimal);
  method Withdraw(amount: decimal);
  property Balance: decimal;
  readonly property AccountNumber: string;
  event BalanceChanged: BalanceChangedEvent;
end IBankAccount;
```

Interface members are implicitly public, have no body, and end with `;`. A class implements an interface implicitly: by declaring matching public members.

A member **matches** an interface member when it is declared `public`, has the same name, the same arity, the same parameter modifiers (`ref`, `out`, `owned`), parameter types that are exactly equal under the interface's substitution, and a return type that is exactly equal under the interface's substitution. Two narrow widenings are permitted on parameter types:

1. **Nullability widening on parameters.** An implementation may declare a parameter as `T?` where the interface requires `T` (the implementation accepts more inputs than the interface promises to pass). The reverse — interface `T?`, implementation `T` — is rejected, because the implementation would refuse a `null` the interface promised to accept.
2. **No widening elsewhere.** Parameter types other than the nullability case must match exactly. Return types must match exactly (no covariance in this version). Parameter modifiers must match exactly: an interface declaring `out T` requires the implementation to declare `out T`, and the same applies to `ref` and `owned`.

The asymmetry on nullability mirrors the standard contravariant-parameter / covariant-return reasoning: implementations may accept more than they were asked to. Bezel admits the parameter-nullability case because it is the common one (most concrete `Equals` implementations want to accept `null` cleanly), and because the type system can verify it without further machinery.

<a id="sec-owned-in-interface-signatures"></a>
#### `owned` in interface signatures

The "must match exactly" rule above applies to every position where `owned` may appear. Specifically, an implementing method's signature and the interface method's signature must agree on:

- **`owned` as a parameter modifier** (e.g. `M(owned t: T)`). Interface declaring `M(owned t: T)` requires implementation to declare `M(owned t: T)`. Either both have `owned` or neither does.
- **`owned` on an `out` parameter** (e.g. `M(out owned t: T)`). Interface declaring `M(out owned t: T)` requires implementation to declare `M(out owned t: T)`; interface declaring `M(out t: T)` requires implementation to declare `M(out t: T)`.
- **Use of an owning generic type** (e.g. `M(): OwnedTask<T>`). Interface declaring `M(): OwnedTask<T>` requires implementation to declare `M(): OwnedTask<T>`; interface declaring `M(): Task<T>` requires implementation to declare `M(): Task<T>`. (Per [§14.4](#sec-owned-as-a-generic-argument), `OwnedTask<T>` and `Task<T>` are distinct named types with no implicit conversion; the same applies to `OwnedArray<T>` vs `Array<T>` and the other owning/borrowing collection pairs.)
- **`owned` inside function-type parameters or return types** (e.g. `M(callback: (owned T) -> ())`). Same rule — exact match.

A mismatch in any position is a compile error at the implementing class. Two reasons for the strict rule:

1. **Loosening from `owned T` to `T` in a return type would leak ownership.** If the interface declares `Acquire(): OwnedTask<Connection>` and the implementation declares `Acquire(): Task<Connection>`, callers calling through the interface receive `OwnedTask<Connection>` per the static type and write `using var c := await source.Acquire();`. But the implementation produced a Pattern A task whose result is borrowed — the `using var` would dispose a borrow whose source is the task, which is also being disposed, leaving the disposal pattern incoherent.

2. **Tightening from `T` to `owned T` in a parameter would create undischarged ownership.** If the interface declares `Send(payload: T)` and the implementation declares `Send(owned payload: T)`, callers calling through the interface pass a borrow per the interface contract, but the implementation expects ownership transfer. The implementation would attempt to take ownership of a value the caller intended to retain, leaving the value's original owner with a dangling reference.

Bezel's stance is uniform: `owned` is part of the type-and-modifier signature, and signatures match exactly. There is no `owned`-direction variance.

Interfaces may declare events. An interface event member has type `IEvent<T>` (synchronous) or `IAsyncEvent<T>` (asynchronous, declared with `async event`). The implementing class provides the matching event declaration; the compiler-generated `EventSource<T>` or `AsyncEventSource<T>` backing satisfies the interface's `IEvent<T>` / `IAsyncEvent<T>` contract because `EventSource<T>` implements `IEvent<T>` (and `AsyncEventSource<T>` implements `IAsyncEvent<T>`). Outside the implementing class, the event is accessed through the interface, which exposes only `Subscribe`. See [§24.7](#sec-events-on-interfaces).

An interface may extend other interfaces:

```bezel
interface IList<T>: IReadOnlyList<T>, ICollection<T>
  property [i: int]: T;
end IList;
```

Multiple-interface inheritance is permitted. Diamond resolution: a single implementation in the implementing class satisfies all interface methods with the same signature. A read-write member satisfies a read-only interface member of the same name and type: if `IReadOnlyList<T>` declares `readonly property [i: int]: T` and `IList<T>` declares `property [i: int]: T`, a single read-write indexer in the implementing class satisfies both. `Add`, `Clear`, `Remove`, and the rest of the mutating surface in the example above are inherited from `ICollection<T>`. `Count` reaches `IList<T>` through two inheritance paths — through `IReadOnlyList<T>` via `IReadOnlyCollection<T>`, and through `ICollection<T>` via `IReadOnlyCollection<T>` again — but both paths substitute to the same property declared on `IReadOnlyCollection<T>`, so the implementing class satisfies both inherited declarations with a single property.

<a id="sec-member-body"></a>
### 11.3 Member body

A class body may contain fields, methods, properties, indexers, events, constructors, a destructor, operator overloads, nested types, and class-level constants.

Every class member must declare its visibility explicitly ([§13](#sec-visibility)). Interface members do not declare visibility (they are always public).

A type may not declare two members that are not distinguishable from each other. Two members **clash** when they have the same name and the type system cannot tell them apart: two fields, properties, events, or nested types with the same name; or two methods, constructors, or operator overloads with the same name that are not distinguishable as overloads ([§22.6](#sec-overloading)) — that is, that do not differ in number of parameters, parameter types, or the `ref`/`out` modifiers in a participating position. A clash is a compile error at the second declaration, reported at the type, and names the conflicting members. Legal overloads — members of the same name that *are* distinguishable per [§22.6](#sec-overloading) — are unaffected; this rule rejects only the duplicates that overloading does not admit. The same rule applies to interface members within an interface declaration.

<a id="sec-block-endings"></a>
### 11.4 Block endings

A class ends with `end ClassName;`. Members may end with `end MemberName;` or `end;`; the named form is recommended. Control structures inside method bodies always end with plain `end;`.

<a id="sec-modifier-order"></a>
### 11.5 Modifier order

When a member declaration carries multiple modifiers, they appear in this order:

```
visibility static async (abstract | virtual | override | sealed override) owned readonly member-kind Name ...
```

`abstract`, `virtual`, `override`, and `sealed override` are mutually exclusive — at most one may appear. A non-virtual member carries none of them. `sealed override` seals a previously-virtual member against further overriding. `sealed` alone is a class-level modifier that prevents inheritance ([§12.4](#sec-sealing)); it does not appear on individual members.

`owned` ([§28.5](#sec-owned-fields)) appears between the virtual-modifier slot and `readonly`. It applies only to fields; methods, properties, and other members do not carry `owned`. The combination `static owned readonly` is the canonical form for a static field that owns a long-lived resource ([§22.13](#sec-static-initialization-order)).

For events, only `visibility`, `static`, and `async` are applicable. The compiler rejects out-of-order modifiers.

<a id="sec-nested-types"></a>
### 11.6 Nested types

A class body may declare nested classes, records, enums, and interfaces. Nested types may be `public`, `internal`, `protected`, or `private`. A nested type's effective visibility is the more restrictive of its declared visibility and its enclosing class's visibility.

```bezel
public class Parser
  public record Token(Kind: TokenKind, Text: string, Line: int);

  public enum TokenKind
    Identifier, Number, Operator, EndOfFile;
  end TokenKind;
end Parser;
```

A nested type is referenced from outside as `Outer.Nested`. A nested class has access to all members (including `private`) of its enclosing class when it holds an instance of the enclosing class. The enclosing class similarly has full access to its nested types' members.

Nested types may themselves contain nested types. Nested types in records are permitted but uncommon.

<a id="sec-static-classes"></a>
### 11.7 Static classes

A class declared `static` may contain only static members:

```bezel
public static class Utils
  public static method Max(a: int, b: int): int => a if a > b else b;
end Utils;
```

A static class may not be instantiated: `new` on a static class is a compile error. A static class may not have instance fields, instance methods, instance properties, indexers, constructors (other than a static constructor), destructors, or `owned` fields. A static class may not be inherited from and does not implement interfaces.

Because a static class cannot be instantiated, it does not implement `IDisposable` — the compiler-generated `Dispose()` rule ([§8.7](#sec-dispose-is-not-on-object), [§28](#sec-resource-management)) applies only to non-static classes. The `abstract` and `static` modifiers are mutually exclusive, and `sealed` and `static` are mutually exclusive; combining either pair is a compile error. The combination `sealed abstract` is permitted and creates a **closed abstract class**: a class that cannot be instantiated directly and cannot be inherited from outside the declaring module. Classes derived from a `sealed abstract` base must be declared in the same module; the compiler rejects subclasses in other modules. This combination is the recommended way to define a closed hierarchy for exhaustive `case` matching ([§19.6](#sec-exhaustiveness)):

```bezel
public sealed abstract class Shape
  public abstract method Area(): double;
end Shape;

public sealed class Circle(radius: double): Shape
  public readonly property Radius: double := radius;
  public override method Area(): double => Math.Pi * Radius * Radius;
end Circle;

public sealed class Square(side: double): Shape
  public readonly property Side: double := side;
  public override method Area(): double => Side * Side;
end Square;

public sealed class Triangle(baseLength: double, height: double): Shape
  public readonly property Base: double := baseLength;
  public readonly property Height: double := height;
  public override method Area(): double => 0.5 * Base * Height;
end Triangle;
```

Because `Shape` is `sealed abstract`, no class outside the declaring module may extend it. The compiler can therefore prove exhaustiveness of `case` matches over `Shape` without requiring a `_` arm, even across module boundaries.

---

<a id="sec-inheritance-and-dispatch"></a>
## 12. Inheritance and Dispatch

<a id="sec-members-are-non-virtual-by-default"></a>
### 12.1 Members are non-virtual by default

A method, property, or indexer is non-virtual unless marked `virtual`. To override a virtual member in a derived class, mark the matching member `override`. Visibility, return type, parameter types, and parameter modifiers must match the base member exactly.

Parameter modifiers that must match: `ref`, `out`, `owned`. Return modifiers that must match: `owned`. A mismatch — for example, a base method declaring `Foo(owned x: T)` and the override declaring `Foo(x: T)` — is a compile error. The compiler does not infer or relax these modifiers across the override boundary, because they affect the method's contract rather than its implementation.

Fields, constructors, destructors, and static members may not be virtual.

<a id="sec-hiding-is-not-allowed"></a>
### 12.2 Hiding is not allowed

Declaring a member in a derived class with the same name as a non-virtual base member is a compile error. To override a virtual base member, use `override`. To add a new member, choose a different name.

<a id="sec-base-member"></a>
### 12.3 `base.Member`

Inside a derived member, `base.Name(...)` calls the base class's implementation, bypassing virtual dispatch. `base` is legal only inside instance members of a derived class.

<a id="sec-sealing"></a>
### 12.4 Sealing

`sealed` on an override prevents further overriding. `sealed` on a class prevents inheritance from that class:

```bezel
public sealed override method Speak(): string => "Woof!";

public sealed class Bulldog: Dog
  // ...
end Bulldog;
```

`sealed` may not appear on a non-virtual, non-override member.

<a id="sec-abstract-classes-and-members"></a>
### 12.5 Abstract classes and members

`abstract` on a class permits abstract members and forbids direct instantiation. `abstract` on a member declares it without a body; every concrete subclass must override it.

```bezel
public abstract class Shape
  public abstract method Area(): decimal;
  public abstract property Name: string;

  public method Describe(): string => $"A {Name} with area {Area()}";
end Shape;
```

An abstract member ends with `;` and has no `end`. Abstract members are implicitly virtual; the `virtual` keyword is not written.

A class containing one or more abstract members must itself be `abstract`. An abstract class need not override every abstract member of its base.

`new` on an abstract class is a compile error.

<a id="sec-async-and-abstract"></a>
### 12.6 `async` and `abstract`

`async abstract` is a compile error. An abstract method whose implementations are expected to be asynchronous declares a `Task` or `Task<T>` return type without `async`:

```bezel
public abstract class DataSource
  public abstract method LoadAsync(): Task<string>;
  public abstract method SaveAsync(data: string): Task;
end DataSource;

public class FileSource: DataSource
  public async override method LoadAsync(): Task<string>
    using var fs := new FileStream(_path);
    return await fs.ReadAllAsync();
  end LoadAsync;

  public async override method SaveAsync(data: string): Task
    await File.WriteAllTextAsync(_path, data);
  end SaveAsync;
end FileSource;
```

The same rule applies to interface members.

<a id="sec-type-tests-is-is-not-as"></a>
### 12.7 Type tests: `is`, `is not`, `as`

`is T` tests whether a value is an instance of `T` and produces `bool`. When the test succeeds and the tested expression is a simple variable, the variable is **flow-narrowed** to `T` within the controlled block:

```bezel
if animal is Dog then
  animal.Bark();        // animal narrowed to Dog
end;
```

Narrowing applies to the original variable. Reassignment within the controlled block invalidates the narrowing.

`is null` tests whether the value is null. `is not null` tests whether it is non-null and narrows to the non-nullable form.

`is not T` (for a non-null type T) does not narrow in the true branch (the value could be many non-T types). In the `else` branch, the value is known to be an instance of `T`, and the variable is narrowed to `T`.

In `else` branches: for `is null` and `is not null`, narrowing applies (the variable is known-null or non-null respectively); for `is T`, no narrowing in the else branch (the variable could be many non-T types); for `is not T`, the variable is narrowed to `T` in the else branch.

When two `is` tests are joined with `and`, both narrowings are in effect throughout the combined true branch.

`is` does not bind. There is no `is T name` syntax. To bind, use `as`:

```bezel
var dog: Dog? := animal as Dog;
if dog <> null then
  Console.WriteLine(dog.Name);
end;
```

`as` returns the value cast to the target type, or `null` if the cast fails. Its result is always nullable.

**Ownership.** Both `is` and `as` are **non-destructive on their operand**: the operation inspects the runtime type tag without moving from the operand. After `if x is T then ... end`, `x` retains whatever ownership state it had before the test; after `var r := x as T`, `x` still owns (or borrows) what it owned (or borrowed) before. The narrowing that `is` enables ([§12.7](#sec-type-tests-is-is-not-as) above) and the borrow that `as` produces ([§17.4](#sec-the-is-is-not-and-as-operators)) do not transfer ownership; transferring ownership across a type test requires an explicit move from the original variable after narrowing.

<a id="sec-constructor-chaining"></a>
### 12.8 Constructor chaining

A derived class's constructor may chain to a base constructor:

```bezel
public constructor(name: string, balance: decimal, rate: decimal)
  base(name, balance);
  _rate := rate;
end constructor;
```

A `base(...)` call, if present, appears before the constructor body — it is syntactically distinct from the body's statements (see grammar A.4, `CtorChain`). If neither `base(...)` nor `this(...)` is written, the compiler inserts an implicit `base()` call (which requires the base class to have a parameterless constructor; otherwise the chain must be explicit).

`this(...)` chains to another constructor in the same class. At most one of `base(...)` or `this(...)` may appear, and only in the chaining position before the body.

A constructor in an abstract class is permitted; it runs only via `base(...)` from a concrete subclass.

<a id="sec-variance"></a>
### 12.9 Variance

Generic interface type parameters may be marked `out` (covariant) or `in` (contravariant):

```bezel
interface IEnumerable<out T>
  method GetEnumerator(): owned IEnumerator<T>;
end IEnumerable;

interface IComparer<in T>
  method Compare(a: T, b: T): int;
end IComparer;
```

`out` requires T to appear only in output positions (return types, read-only property types, `out` parameters). `in` requires T to appear only in input positions. The compiler enforces position constraints.

Position is composed across nested type constructors. A type parameter inside another generic type inherits position from that wrapper's variance: T inside `IEnumerator<T>` (where `IEnumerator` is `out T'`) has the same position the wrapper itself sits in. Function types are themselves variant (covariant in return, contravariant in parameter, see below), and the same composition rule applies. The practical consequence — relevant for event interfaces — is that T appearing inside the *parameter* of a function type that is itself the *parameter* of a method ends up in covariant position, because contravariant-of-contravariant is covariant. A concrete worked example: in `IEvent<out T>`, the method `Subscribe(handler: (T) -> ())` places T inside `(T) -> ()` (contravariant in T) which is itself a parameter of `Subscribe` (an input, contravariant), giving T a covariant position overall — `out T` is sound.

Type parameters without `out` or `in` are invariant. Classes, records, and concrete generic types are always invariant.

Variance applies only when the type argument is a reference type. `IEnumerable<int>` is a perfectly valid type and is used pervasively; what is rejected is the *variant assignment* of `IEnumerable<int>` to `IEnumerable<Object>`, which would require per-element boxing at every access. For value-typed T, the only legal assignment of `IEnumerable<T>` is to another `IEnumerable<T>` (or to a non-variant supertype like `IEnumerable<T>` itself or `Object`). Reference-typed T admits the full covariant chain — `IEnumerable<Dog>` is assignable to `IEnumerable<Animal>` and to `IEnumerable<Object>` because the underlying references compose without conversion.

Function types are automatically variant — covariant in return type, contravariant in parameter types.

<a id="sec-variance-and-interface-inheritance"></a>
#### Variance and interface inheritance

A generic interface may extend a generic base whose variance is different from its own. The most common case in the standard library: `IList<T>` is invariant in T (it has mutating methods) and extends `IReadOnlyList<out T>` (which is covariant in T):

```bezel
public interface IReadOnlyCollection<out T>: IEnumerable<T>
  readonly property Count: int;
end IReadOnlyCollection;

public interface IReadOnlyList<out T>: IReadOnlyCollection<T>
  readonly property [i: int]: T;
end IReadOnlyList;

public interface IList<T>: IReadOnlyList<T>, ICollection<T>
  property [i: int]: T;
end IList;
```

The extending interface instantiates the base's variant parameter with its own type parameter, holding the deriving interface's variance. The substitution is `IReadOnlyList<T>` where the bound T is whatever T the deriving interface has; if `IList<T>` has T invariant, then the inherited `IReadOnlyList<T>` is also invariant in T from `IList<T>`'s perspective. The base's covariance is *available* when accessed through the base type — `IReadOnlyList<Dog>` is assignable to `IReadOnlyList<Animal>` — but accessing the same instance through `IList<Dog>` does not admit the covariant assignment, because `IList<T>` is invariant.

This means an instance whose runtime type implements `IList<Dog>` is assignable to:
- `IList<Dog>` (exact match)
- `IReadOnlyList<Dog>`, `IReadOnlyList<Animal>`, `IReadOnlyList<Object>` (covariant chain via the read-only view)
- `IReadOnlyCollection<Dog>`, `IReadOnlyCollection<Animal>`, `IReadOnlyCollection<Object>` (covariant chain via the read-only-collection view)
- `ICollection<Dog>`, `IEnumerable<Dog>`, `IEnumerable<Animal>`, `IEnumerable<Object>` (other inherited interfaces, with their own variance)

It is *not* assignable to `IList<Animal>` — the variance of the base interface does not propagate through the deriving interface's signature.

The position-composition rules above continue to apply within each interface in isolation. The compiler enforces position constraints on each interface declaration independently: `IReadOnlyList<out T>` is checked with T in covariant position throughout its body; `IList<T>` is checked with T in any position throughout its body. The inheritance relation only fixes the type argument substituted into the base — it does not alter what positions T may legally appear in within the derived interface.

<a id="sec-inheritance-cycles"></a>
### 12.10 Inheritance cycles

An inheritance cycle is a compile error. A type may not, directly or transitively, list itself among its base types — neither as the base class of a class, nor as an extended interface of an interface. The relation followed is the union of base-class edges ([§11.1](#sec-class-declaration)) and extended-interface edges ([§11.2](#sec-interface-declaration)); a cycle is any sequence of types where each lists the next as a base type and the sequence returns to its start. The simplest case is a type naming itself (`class C: C`); the general case is a chain (`class A: B`, `class B: C`, `class C: A`) and the mixed class/interface chains the relation admits. The compiler detects the cycle and reports an error naming the participating types; the cycle is never silently broken or otherwise tolerated.

---

<a id="sec-visibility"></a>
## 13. Visibility

<a id="sec-levels"></a>
### 13.1 Levels

Four levels:

| Level | Visible to |
|---|---|
| `public` | Anyone who can see the enclosing context |
| `internal` | Code in the same module |
| `protected` | The declaring class and all classes derived from it |
| `private` | The declaring class only |

`internal` and `protected` are incomparable: a derived class in a different module sees `protected` but not `internal`; an unrelated class in the same module sees `internal` but not `protected`.

<a id="sec-top-level-types"></a>
### 13.2 Top-level types

Top-level types may be `public` or `internal`. They may not be `protected` (no enclosing class) or `private` (no file-scope concept). The default is `internal`:

```bezel
public class BankAccount
  // ...
end BankAccount;

class Helper           // implicitly internal
  // ...
end Helper;
```

<a id="sec-class-members"></a>
### 13.3 Class members

Every class member must declare its visibility explicitly, with two exceptions:

- A `static constructor` carries no visibility keyword (the runtime is the only caller; see [§22.7](#sec-constructors)).
- A `destructor` carries no visibility keyword. A destructor is an implementation hook called by the compiler-generated `Dispose()` method; it is not directly callable by user code. The customization point is `destructor`; the public surface is `Dispose()` (see [§28.4](#sec-the-destructor)).

Outside these two cases, omitting the visibility keyword is a compile error.

Interface members do not declare visibility; they are implicitly public.

<a id="sec-modules-and-internal"></a>
### 13.4 Modules and `internal`

A module is the set of all source files declaring the same `module Name.Path;`. The dotted form is a single name; files in `Foo` get no special access to files in `Foo.Bar`.

A file declares exactly one module.

<a id="sec-no-combined-visibilities"></a>
### 13.5 No combined visibilities

Bezel does not provide combined forms (no `protected internal`, no `private protected`).

<a id="sec-accessor-visibility"></a>
### 13.6 Accessor visibility

A property or indexer's `get` or `set` may carry its own visibility, which must be more restrictive than the property's overall visibility:

```bezel
public property Name: string
  get => _name;
  private set => _name := value;
end Name;
```

An accessor without explicit visibility inherits the property's overall visibility. An accessor visibility *less* restrictive than the property is a compile error.

The auto-property shorthand `public property Name: string private set;` is equivalent to the block form.

<a id="sec-visibility-and-inheritance"></a>
### 13.7 Visibility and inheritance

- An override must declare the same visibility as the base member.
- **Every type appearing in a member's signature must be at least as visible as the member.** The rule covers: field types, parameter types, return types, property types, indexer parameter and result types, event payload types, and types named in `where` clauses on a public/protected member. The rule applies at every visibility level — a `public` member's signature types must be `public` (or more permissive); an `internal` member's signature types must be `internal` (or more); a `protected` member's must be at least `protected`; a `private` member is unconstrained (since `private` is the most restrictive level).
- A class's base class and implemented interfaces must be at least as visible as the class.

The signature-type-visibility rule applies to a member's *declared* signature. Generic-method type parameters whose substitution at the call site could narrow visibility are not constrained — the caller's substitution is the caller's choice, not the declarer's.

```bezel
public class Outer
  private class NestedPrivate
    // ...
  end NestedPrivate;
  
  // ERROR: _x is internal, but NestedPrivate is private — the field's
  //        type is not as visible as the field.
  internal _x: NestedPrivate;

  // OK: also private, matches NestedPrivate's visibility.
  private _y: NestedPrivate;
end Outer;
```

---

<a id="sec-generics"></a>
## 14. Generics

<a id="sec-generic-declarations"></a>
### 14.1 Generic declarations

Classes, interfaces, records, and methods may be generic:

```bezel
class MyArray<T>
  // ...
end MyArray;

interface IComparable<T>
  method CompareTo(other: T): int;
end IComparable;

record Pair<TFirst, TSecond>(First: TFirst, Second: TSecond);

static class Utils
  public static method Swap<T>(ref a: T, ref b: T)
    var temp := a;
    a := b;
    b := temp;
  end Swap;
end Utils;
```

Type parameters use angle brackets. Multiple parameters are comma-separated.

<a id="sec-constraints"></a>
### 14.2 Constraints

The `where` clause restricts the substitutions a type parameter accepts:

| Constraint | Accepts |
|---|---|
| *(none)* | Any type, nullable or not. T is treated conservatively as possibly nullable. |
| `where T: notnull` | Non-nullable types only (value or reference). A nullable type, value or reference, is excluded. |
| `where T: class` | Non-nullable reference types only — classes (including `Array<T>`, `OwnedArray<T>`, and other heap collection types), interfaces, `string`, function types. |
| `where T: value` | Non-nullable value types — records, enums, primitives, inline arrays (all forms), the unit type. A nullable value type (e.g. `int?`) does **not** satisfy `value`. Generic body must use only move-compatible operations on T if T may be a move-only inline array ([§15.3](#sec-move-only-value-types)). |
| `where T: SomeClass` | Non-nullable subtypes of `SomeClass` (the class itself and its descendants). Class types only; interfaces do not extend classes in Bezel. |
| `where T: IFoo` | Non-nullable types conforming to `IFoo`: classes implementing it, records implementing it, and interfaces extending it. See "Interface conformance" below. |
| `where T: new()` | Any type with an accessible parameterless constructor, or any positional record whose every field has a well-defined default value. |

Constraints may be combined: `where T: class, IDisposable, new()` requires a non-nullable reference type that implements `IDisposable` and has a public parameterless constructor.

For `where T: new()`, the constraint is satisfied by a positional record `R(F1: T1, ..., Fn: Tn)` exactly when `default(R)` is well-defined per [§17.11](#sec-the-default-operator) — i.e., every Fi has a well-defined default. Inside a generic body, `new T()` for a *record* substitution is equivalent to `default(T)` — the parameterless constructor produces the zero-valued record. This admits records like `record Point(X: int, Y: int)` while rejecting records that contain non-nullable reference fields. For a *class* substitution, `new T()` produces an owned reference via the class's parameterless constructor under the standard ownership-discharge rules of [§28.3](#sec-local-variables-ownership-and-borrows).

A type-bound constraint — naming a class or interface — implies non-null T. The reasoning is that nullable types `MyClass?` and `IFoo?` are distinct from their non-nullable forms ([§7.1](#sec-non-nullable-and-nullable-types)) and do not themselves implement the interfaces of their non-nullable counterparts: a `null` value satisfies no interface contract. Constraining on a class or interface name therefore admits only non-nullable substitutions, the same way `where T: class` does. Inside the generic body, T is non-nullable; `T?` is its nullable companion.

**`value` requires a non-nullable value type.** A nullable value type — `int?`, `Priority?`, `DateTime?`, and the nullable form of any record, enum, or inline array — does *not* satisfy `where T: value`. The `value` constraint admits only the non-nullable value type itself; `where T: value` then makes `T?` the nullable companion inside the body. The reasoning matches the type-bound case above: a nullable value type is a distinct type from its non-nullable form ([§7.1](#sec-non-nullable-and-nullable-types)), and its representation differs (it carries the additional `null` state). `where T: notnull` likewise excludes every nullable type — value or reference — for the same reason. To accept a nullable value type, write the constraint against the underlying value type and use `T?` in the body.

`T?` always denotes the nullable form of T. `default(T)` produces the zero/null value of T ([§17.11](#sec-the-default-operator)).

Contradictory constraint combinations (e.g. `class, value`) are compile errors.

<a id="sec-interface-conformance"></a>
#### Interface conformance

For an interface I and a candidate type T, T **conforms to** I exactly when one of the following holds:

1. **T is a class** that lists I (or any interface extending I) in its inheritance clause, or that inherits from a class which conforms to I. Implementation may be direct (the class declares the matching members) or inherited (a base class declares them).
2. **T is a record** that lists I (or any interface extending I) in its inheritance clause and declares matching members. Records are sealed ([§9.6](#sec-records-and-inheritance)), so conformance is always direct, never inherited.
3. **T is an interface** that has I in its extension chain — either I is named directly in T's inheritance clause, or T extends an interface that itself conforms to I.

`where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per the rule above. Inside the generic body, T's surface includes IFoo's members, regardless of which conformance category T belongs to; the body cannot tell whether T was satisfied by a class, a record, or an interface, and does not need to.

<a id="sec-worked-example-where-t-idisposable"></a>
#### Worked example: `where T: IDisposable`

Every class implicitly implements `IDisposable` ([§28.2](#sec-the-idisposable-contract)), and records cannot implement `IDisposable` ([§9.7](#sec-records-and-disposal)). Applying the conformance rule:

- Every non-nullable class type conforms (rule 1).
- No record type conforms (rule 2 fails because records cannot implement `IDisposable`).
- Interface types conform when they extend `IDisposable` (rule 3): `IAsyncDisposable`, `IEnumerator<T>`, `IAsyncEnumerator<T>`, and any user-defined interface that lists `IDisposable` in its extension clause.

So `where T: IDisposable` admits the same *class* substitutions as `where T: class` (every class qualifies under both) but a strictly smaller set of *interface* substitutions (only `IDisposable`-extending interfaces, versus all interfaces under `where T: class`). The difference rarely matters in practice — programmers usually write `where T: IDisposable` when the body calls `Dispose()`, and that's exactly when admitting only `IDisposable`-conforming interfaces is what was wanted.

<a id="sec-worked-example-where-t-icomparableint"></a>
#### Worked example: `where T: IComparable<int>`

`IComparable<int>` is implemented by `Int32` (the standard-library record wrapping `int`; stdlib [§9.1](Bezel_Standard_Library.md#sec-integer-types)) and by user-defined records and classes that implement it. Applying the conformance rule:

- `Int32` (rule 2): conforms. `Int32` is a record, so it falls under the record-conformance clause.
- Any user record listing `IComparable<int>` in its inheritance clause (rule 2): conforms. For example, `record Wrapper(value: int): IComparable<int>`.
- Any user class implementing `IComparable<int>` (rule 1): conforms.
- An interface `IIntComparable: IComparable<int>` (rule 3): conforms.

`where T: IComparable<int>` therefore admits a mix of class, record, and interface substitutions — illustrating that interface constraints are not class-only, unlike the class-bound constraint `where T: SomeClass`.

<a id="sec-generics-variance"></a>
### 14.3 Variance

See [§12.9](#sec-variance).

<a id="sec-owned-and-generic-type-arguments"></a>
<a id="sec-owned-as-a-generic-argument"></a>
<a id="sec-substitution-of-owned-bearing-type-arguments"></a>
<a id="sec-merged-declarations-and-form-dependent-members"></a>
<a id="sec-conditioning-method-existence-on-ts-modifier"></a>
### 14.4 `owned` and generic type arguments

**`owned` does not propagate through generic type arguments.** A generic argument's type is whatever class, value, or interface type is supplied; the `owned` modifier — which expresses an ownership-tracking obligation at a declaration position — does not attach to type arguments.

Specifically: the following are **not valid types** as of v0.97:

- `Array<owned T>`, `LinkedList<owned T>`, `Set<owned T>`, `Stack<owned T>`, `Queue<owned T>`, `Dictionary<owned K, V>`, `Dictionary<K, owned V>`, `Dictionary<owned K, owned V>` — replaced by named owning collection types (stdlib [§4](Bezel_Standard_Library.md#sec-system-collections-generic)): `OwnedArray<T>`, `OwnedLinkedList<T>`, `OwnedSet<T>`, `OwnedStack<T>`, `OwnedQueue<T>`, `OwnedDictionary<K, V>` (with `V: class`).
- `Task<owned T>` — replaced by `OwnedTask<T>` (stdlib [§6](Bezel_Standard_Library.md#sec-system-threading)).
- `IEnumerable<owned T>`, `IEnumerator<owned T>`, `IAsyncEnumerable<owned T>`, `IAsyncEnumerator<owned T>` — replaced by `OwnedEnumerable<T>`, `OwnedEnumerator<T>`, `OwnedAsyncEnumerable<T>`, `OwnedAsyncEnumerator<T>`.
- `IList<owned T>`, `ICollection<owned T>` — replaced by `IOwnedList<T>`, `IOwnedCollection<T>`.

In every case, the v0.97 type is a separate named class or interface declared in the standard library. The pattern is uniform: where v0.96 (and earlier revisions back to v0.78) used `<owned T>` to express "the owned-element form of this generic," v0.97 uses an explicit `Owned…` type with its own declaration, members, and inheritance.

This is a deliberate simplification. The owned and non-owned forms of these types have genuinely different semantics — an owning collection's `Add` takes ownership of the element; a borrowing collection's `Add` takes a borrow. Earlier revisions tried to express this difference within a single generic parameterization via substitution rules, modifier-conditioning constraints, and inheritance projection; the resulting machinery accumulated complexity each time a new corner case was discovered. v0.97 collapses the machinery into separate types. Rationale [§63](Bezel_Design_Rationale.md#sec-v0-96-to-v0-97-changelog) narrates the decision in full.

The `owned` modifier itself remains. It appears in declaration positions: parameter declarations (`method Add(owned item: T)`), return type declarations (`method Acquire(): owned Connection`), field declarations (`private owned _resource: Connection`), local variables through `using var` (`using var c := Connect()`), and `out` parameter declarations (`out owned p: T`). These uses are unchanged from v0.77.

The admissible positions for `owned` are enumerated in [§28.1](#sec-where-the-owned-modifier-can-appear); types and positions that do not admit `owned` (record fields, inline-array element types, generic type arguments) reject the modifier at the position itself, not through any substitution rule.

<a id="sec-generic-constraints-and-class-types"></a>
#### Generic constraints and class types

Generic methods and types can constrain their parameters to class types via `where T: class` ([§14.2](#sec-constraints)). A class type C satisfies `where T: class`. A generic method with a `where T: class` constraint may be called with T = C; the call site supplies a value of type C (whether the value is owned or a borrow at that site is determined by the declaration position of the parameter the call argument flows into, not by T's identity).

Generic types that store class-typed values do not, by themselves, become "owning" by having a class type argument. `Array<MyClass>` stores borrows of `MyClass` — references whose ownership remains with whoever passed them in. To get an owning collection, the user reaches for `OwnedArray<MyClass>` — a different class with different semantics.

**Class-only restriction on owning generics.** Most standard-library owning types (`OwnedArray<T>`, `OwnedLinkedList<T>`, `OwnedSet<T>`, `OwnedStack<T>`, `OwnedQueue<T>`, `OwnedDictionary<K, V>`, `OwnedTask<T>`, `OwnedEnumerable<T>`, `OwnedEnumerator<T>`, `OwnedAsyncEnumerable<T>`, `OwnedAsyncEnumerator<T>`, `IOwnedCollection<T>`, `IOwnedList<T>`) restrict their element type T to **proper class types**: types declared with the `class` keyword. The `where T: class` constraint in their declarations is a slight imprecision — per [§14.2](#sec-constraints) `where T: class` admits all non-nullable reference types (classes, interfaces, `string`, function types) — but the standard-library owning types are compiler-enforced to reject the runtime-managed reference types (`string`, function values). The reason: those types are runtime-managed ([§28.9](#sec-runtime-managed-types)) and have no disposal obligation; owning them is degenerate. The error message at instantiation names the rejected type and points the reader to the relevant stdlib section.

`OwnedBuffer<T>`, `Span<T>`, and `ReadOnlySpan<T>` (stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)) are exceptions to this restriction as of v0.102. They admit T to be either class-typed (with ownership semantics as above) or value-typed (with plain value semantics — no per-slot ownership, no Dispose cascade, no `move` admission). They still reject the runtime-managed reference types (`string`, function values) and move-only value types (the owning inline arrays `[N owned]T` and `[N owned]T?`, [§15.3](#sec-move-only-value-types)).

**Nullable class types and interfaces are admitted.** The owning-type instantiation rule checks the *underlying* type, not the nullable-wrapper. `OwnedTask<Lock?>` (a Pattern B task whose result is `owned Lock | null`) is admitted because `Lock` is a class type; the nullability of the result is independent of the disposal obligation. The same applies to interface types: `OwnedArray<IConnection>` is admitted because `IConnection` may be implemented by class types with real disposal obligations; iteration produces borrows of `IConnection`, and `Add(owned)` takes ownership of an `IConnection` reference whose runtime dispatch resolves to a real class's `Dispose()` at disposal time.

A user-defined generic type that needs to own its elements composes over `OwnedArray<T>` (or another `Owned…` collection type):

```bezel
public class OwnedCache<T> where T: class
  private owned _items: OwnedArray<T>;

  public constructor() _items := new(); end;

  public method Add(owned item: T)
    _items.Add(item);
  end Add;

  public method Drain(): OwnedEnumerable<T>
    return _items.Drain();
  end Drain;
end OwnedCache;
```

This composition pattern works under the existing field-ownership and generic-type rules; no special mechanism is required. User-defined generic types with class-typed parameters that don't store anything owned (read-only views, transformations) just declare their members in the normal way.

<a id="sec-historical-references"></a>
#### Historical references

Rationale [§52](Bezel_Design_Rationale.md#sec-v0-77-to-v0-78-changelog) through [§62](Bezel_Design_Rationale.md#sec-v0-95-to-v0-96-changelog) describe the v0.78–v0.96 mechanisms by which `owned` *was* a generic-argument modifier — the opt-in list, the three substitution rules (idempotence, admissibility-stripping, non-admissibility-stripping), the `where T is owned` / `where T is not owned` constraint forms, and the inheritance-projection rule. Those mechanisms are removed in v0.97; this section's anchors (`sec-substitution-of-owned-bearing-type-arguments`, `sec-merged-declarations-and-form-dependent-members`, `sec-conditioning-method-existence-on-ts-modifier`) are preserved so that historical changelog links resolve, but the *content* those links once described is no longer present. Readers who want to understand the v0.78–v0.96 forms should consult those revisions' archived documents.

<a id="sec-arrays"></a>
## 15. Arrays

Bezel provides two array kinds:

- **Inline arrays** `[N]T` — value-typed, fixed size set at compile time, stored inline in their enclosing context (stack frame, class storage, or record layout). No heap allocation, no `Dispose`.
- **Heap arrays** `Array<T>` and `OwnedArray<T>` — class types from the standard library, allocated with `new`, require disposal. May be used at fixed size (by not calling resize methods) or resizably (the natural use).

The two kinds serve different purposes. Inline arrays are a value-type storage primitive: they live in the place that holds them, copy by value (when copyable), and have no allocation cost. Heap arrays are the general-purpose container family: they grow and shrink, support iteration protocols, and carry the ownership obligations associated with their allocated storage.

<a id="sec-inline-arrays"></a>
<a id="sec-fixed-size-arrays"></a>
### 15.1 Inline arrays

The type `[N]T` is an inline array of N elements of type T. N is a compile-time integer constant; T is any type subject to the element-type rules below.

```bezel
var pixels: [16]byte;                    // 16 zero-initialized bytes
var matrix: [9]float;                    // 9 zero-initialized floats
var rgb:    [3]byte := [255, 128, 64];   // explicit initialization
```

**Size.** N must be a compile-time integer expression. Integer literals and `const` declarations whose value is a compile-time integer expression are admitted; arithmetic on these (e.g., `[BufferSize * 2]byte`) is also admitted. N must be greater than zero; `[0]T` is a compile error.

The size N is part of the type. `[3]int` and `[4]int` are distinct types that do not interconvert.

**Element type.** The element type T may be:

- A value type (primitive or record): the array stores the values directly, inline.
- A class type (`C` or `C?`): the array stores references inline; the referenced objects live on the heap as usual.
- A runtime-managed reference type (`string`, function values): the array stores references inline; the underlying memory is managed by the runtime.

Inline arrays may not be elements of inline arrays in the form `[N][M]T` — multi-dimensional inline arrays are deferred to a future revision. The current spec admits only one-dimensional inline arrays.

**No nullable inline-array types.** Inline arrays themselves do not have a nullable form: `([3]int)?` is not admitted. The element type may be nullable (`[3]int?` is well-formed and denotes an inline array of nullable int), but the array as a whole always has a value. The grammar parses `[3]int?` greedily as "inline array of `int?`," not as "nullable inline array of `int`." The motivation: inline arrays are stored inline in their host, so a "null" inline array would have no natural representation (every slot would have to indicate "the whole array is absent"), and the use cases for a sentinel "no array yet" value are well-served by either a nullable wrapping record or a nullable class-typed reference to a heap array.

**Initialization.** When the element type T is *zero-initializable* — every value-typed T whose fields recursively zero-initialize, or any nullable type T? — an inline array declaration without an explicit initializer is filled with zero or null values per slot:

```bezel
var counts: [10]int;             // 10 zeros
var maybe:  [5]Connection?;       // 5 nulls
var verts:  [3]Vertex;            // 3 zero-initialized Vertex records (if Vertex zero-initializes)
```

When the element type is a non-nullable class type or a record without default initialization, an explicit initializer is required at declaration. The initializer is a comma-separated list of N expressions in brackets:

```bezel
var rgb: [3]byte       := [255, 128, 64];
var pt:  [2]Point      := [Point(0, 0), Point(1, 1)];   // value-typed Point record
```

For non-nullable class-typed elements (`[N]C` without the `owned` slot marker), the elements stored in the array are *borrows* — the initializer expressions must be borrows of class instances already owned elsewhere, and those owners must outlive the inline array. To construct an inline array that *owns* its non-null class-typed slots, use the `[N owned]T` form ([§15.1.1](#sec-owning-inline-arrays)) with an initializer of owned expressions; ownership transfers from each expression into the array.

The list must contain exactly N elements; mismatch is a compile error.

<a id="sec-owning-inline-arrays"></a>
#### 15.1.1 Owning inline arrays

The form `[N owned]T` (or `[N owned]T?`) denotes an inline array whose slots own their (non-null) class-typed contents. T must be a class or interface type; value types and runtime-managed reference types are not admitted with the `owned` marker. When T is a generic type parameter, the class-or-interface restriction is checked at *instantiation* — a generic constraint `where T: class` admits broader types (including runtime-managed reference types) per [§14.2](#sec-constraints), so instantiating an inline array `[N owned]T?` with T = `string` or another non-class-or-interface type is a compile error at the instantiation site, paralleling the generic-class-fields rule of [§28.5](#sec-owned-fields).

```bezel
// Non-null owned slots; initializer required at declaration
var fixed: [3 owned]Connection := [
  Connect("primary"), Connect("secondary"), Connect("tertiary")
];

// Nullable owned slots; defaults to all null
var pool: [16 owned]Connection?;
```

An owning inline array is a *move-only* value type ([§15.3](#sec-move-only-value-types)). Assignment transfers ownership rather than copying. Copying is forbidden, since copying would duplicate ownership of each slot's contents.

When an owning inline array's lifetime ends, the array's auto-dispose pass disposes each non-null slot. For locals, this happens at scope end via the standard owned-local discharge ([§28.3](#sec-local-variables-ownership-and-borrows)); for class fields, the enclosing class's destructor invokes the disposal.

The `move arr[i]` form is admitted on both `[N owned]T?` and `[N owned]T` slots ([§28.11](#sec-collections-do-not-own-elements)). For nullable slots, the field-restoration rule is satisfied by assigning `null` to the slot. For non-nullable slots, restoration requires assigning a fresh owned T (e.g., a constructor call, another moved value, or a borrow promoted to ownership via a factory) — the same rule as for non-nullable `OwnedArray<T>` slots ([§4.5](Bezel_Standard_Library.md#sec-ownedarrayt) of the standard library). Code that needs "leave the slot empty after take" requires the nullable form; non-nullable slots must always hold a value at every observable point.

<a id="sec-inline-array-members"></a>
<a id="sec-members-and-operations"></a>
#### 15.1.2 Members and operations

Every inline array `[N]T` (with any modifier combination) exposes:

- `arr.Length: int` — compile-time constant N.
- Indexer `arr[i]` for read and write. Bounds checking applies; a constant index proven in-range at compile time elides the check.

Iteration over an inline array uses the standard `for var x in arr do … end` form. The compiler emits a direct indexed loop without involving the `IEnumerable<T>` protocol: inline arrays are not class types and do not implement interfaces. For value-typed T, each iteration variable `x` is a copy of the slot's value; for class-typed T, each `x` is a borrow of the slot's reference. For `[N owned]…` arrays, iteration produces borrows into the owned slots (the iteration variable is a borrow; the slot retains ownership); to consume slot contents, use explicit `move arr[i]` followed by restoration (assignment of `null` on `[N owned]T?` arrays, or assignment of a fresh owned T on `[N owned]T` arrays). Passing an inline array to a method expecting `IEnumerable<T>` is rejected: the array is a value type, not a reference type, and does not satisfy the interface contract. Callers who need to forward the contents to an IEnumerable-taking method materialize a heap array by iterating the inline array's slots into a fresh `Array<T>`.

Two inline-array values of the same type are equal iff their elements are pairwise equal under the element type's equality. Inline arrays follow record-equality discipline: structural equality grounded in element equality, with no special cases. Owning inline arrays (`[N owned]…`) admit equality only via element borrow; the comparison reads each slot without taking ownership.

**Lifetime of inline arrays carrying borrows.** An inline array `[N]C` or `[N]C?` whose element type C is a reference type (class or interface) carries N (potentially null) borrows into its slots. When such an array is passed by value, returned from a method, stored as a field, or captured by a closure, the same borrow-lifetime rules apply that govern borrow-bearing records ([§9.7](#sec-records-and-disposal), [§28.3](#sec-local-variables-ownership-and-borrows)): the array value may not outlive the source of any borrow in any of its slots. The compiler tracks this element-wise where possible; where it cannot prove safety, the program is rejected. Inline arrays of value-typed elements, runtime-managed reference-typed elements (`string`, function values), or that carry only owned slots (`[N owned]…`) are not borrow-bearing.

<a id="sec-inline-array-ffi"></a>
<a id="sec-ffi"></a>
#### 15.1.3 FFI

Inline arrays of FFI-compatible primitive elements (`[N]byte`, `[N]int`, `[N]float`, and similar) have C-equivalent storage layout: N elements packed contiguously, no header. Such arrays may appear in `extern` method signatures and marshal directly to C arrays of the same size and element type.

Inline arrays of class-typed, interface-typed, record-typed, or runtime-managed-reference-typed elements do not have C-equivalent layout (each slot is a managed reference or a structure not directly mappable to C). They are not admitted in extern signatures. Multi-dimensional inline arrays (nested forms like `[N][M]T`) are deferred to a future revision and likewise not admitted at FFI boundaries.

<a id="sec-arrayt"></a>
<a id="sec-heap-arrays"></a>
### 15.2 Heap arrays

`Array<T>` and `OwnedArray<T>` are the heap-allocated array types, documented in standard library [§4.4](Bezel_Standard_Library.md#sec-arrayt) and [§4.5](Bezel_Standard_Library.md#sec-ownedarrayt) respectively. They are class types allocated with `new` and requiring disposal via `using var`, owned field, or explicit `.Dispose()`.

Both support resizing operations. Users who want fixed-size discipline simply do not call resizing methods; the array's `Count` matches its construction-time capacity throughout its lifetime. The language does not provide a separate "fixed-size heap array" type; the discipline of not resizing is sufficient.

```bezel
// General use — resizable
using var data := new Array<int>();
data.Add(1); data.Add(2); data.Add(3);

// Fixed-size discipline — pre-allocate via literal, don't resize
using var snapshot: Array<int> := [1, 2, 3];
// snapshot.Count == 3 throughout its lifetime; no Add calls

// Owning heap array
using var pool: OwnedArray<Connection> := [
  Connect("a"), Connect("b"), Connect("c")
];
```

<a id="sec-move-only-value-types"></a>
### 15.3 Move-only value types

A move-only value type is a value type whose instances cannot be duplicated. Assignment transfers the value from source to destination, leaving the source in moved-from state. Reading from a moved-from location is a compile error.

The single move-only value type kind in this specification is the owning inline array (`[N owned]T` or `[N owned]T?`, see [§15.1.1](#sec-owning-inline-arrays)). Other value types — primitives, records, and inline arrays without the `owned` modifier — remain copyable.

**Rules.** Where an ordinary value type V supports an operation, a move-only V substitutes a moved variant or rejects the operation:

- **Local-to-local assignment** `var y := x;`: copies for an ordinary value type; moves for a move-only value type. After the move, `x` is in moved-from state and may not be read until reassigned.
- **Whole-value reassignment** `arr := newValue;` on a move-only local or field: the assignment is plain — the compiler does not insert automatic cleanup of the previous slots. If the LHS holds any slot that is not in moved-from state at the assignment site, those slots leak. The programmer satisfies the discipline by first emptying every owned slot of the LHS (via `move arr[i]` followed by `using var` or explicit Dispose on each taken value, or by passing the whole array to a method declared `(owned V)` that consumes it). This rule parallels [§28.7](#sec-reassigning-owned-fields) for owned class fields. A move-only field of a class is subject to the same rule. The compiler emits a warning when reassignment is reachable from a state where some slot is provably non-moved-from; the warning identifies the slots at risk and points at the standard discharge mechanisms.
- **Parameter passing**: a parameter declared `(x: V)` for an ordinary V is by value (copy); for a move-only V, it is by value-move (transfer ownership). The caller's argument expression is in moved-from state after the call. A parameter declared `(x: owned V)` is identical to plain `(x: V)` for a move-only V; the modifier is admitted for documentation but is redundant.
- **Return**: returning a move-only value transfers it to the caller. The callee's local is moved-from after the return.
- **Field placement**: a move-only value may be a field of a class. The class's destructor disposes the field's owned slots. A move-only value may not be a field of a record (records support only copy semantics, not move semantics).
- **Local lifetime**: a move-only local's owned slots are auto-disposed at scope end by the compiler. Unlike owned class-typed locals ([§28.3](#sec-local-variables-ownership-and-borrows)), the array as a whole does not require an explicit discharge call (no `Dispose()` to invoke; the array is not a class). However, the field-restoration rule still applies to individual slots: any slot moved-from via `move arr[i]` must be reassigned before any exit edge, because auto-dispose on a moved-from slot is not defined. The standard restoration mechanisms apply: assign a fresh owned value, or for `[N owned]T?` arrays, assign `null` to mark the slot empty.
- **Exception-path discharge**: on exception edges, the slots are auto-disposed by the implicit `try`/`finally` rule ([§28.3](#sec-local-variables-ownership-and-borrows)). The same mechanism that handles owned class-typed locals on exception paths handles move-only inline-array locals.
- **Equality**: move-only values support equality via borrowing comparison (reads each slot without moving), provided their element types support equality.

`using var` is admitted for move-only inline arrays per the general rule of [§28.10](#sec-using-var) (which admits `using var` for any value-type or runtime-managed RHS). For a move-only inline-array local, `using var` has no disposal effect (scope-end auto-dispose runs whether or not `using var` is applied), and its only observable consequence is preventing reassignment. Programs that want to express "this owning inline-array binding is fixed at construction and may not be rebound" may use `using var arr: [16 owned]Connection?;`; programs that don't need the reassignment lock use plain `var`. Both are admitted; both produce identical runtime behavior at scope end.

**Move-only is determined by structure, not by declaration.** Move-only is not a user-declarable property. The language determines whether a value type is move-only by inspecting its structure:

- Inline arrays with the `owned` modifier (`[N owned]T?` or `[N owned]T`) are move-only.
- All other value types — primitives, records, inline arrays without `owned` — are copyable.

A user-defined value-type-with-move-only-semantics is not provided in this revision. The move-only property exists to support owning inline arrays and may be extended in a future revision if a real use case for user-defined move-only types emerges.

<a id="sec-collection-literals"></a>
### 15.4 Collection literals

A collection literal is a comma-separated list of expressions in square brackets:

```bezel
[1, 2, 3]
[Connect("a"), Connect("b")]
[]                                // empty literal
```

A collection literal has no standalone type. It is well-typed only in a context that provides the target type, either via type annotation or via the parameter type of a method call.

```bezel
var a: [3]int := [1, 2, 3];                       // inline array
using var b: Array<int> := [1, 2, 3];             // heap array
using var c: OwnedArray<Connection> := [          // owning heap array
  Connect("a"), Connect("b"), Connect("c")
];

ProcessBuffer([1, 2, 3]);                          // type from parameter
```

A collection literal in a position where no target type is determinable is a compile error:

```bezel
var x := [1, 2, 3];        // ERROR: cannot infer target type
```

`return [1, 2, 3]` is admitted when the enclosing method's return type determines the target type.

The target types that admit collection literals are:

- Inline arrays `[N]T`, `[N]T?`, `[N owned]T`, `[N owned]T?` (the literal must have exactly N elements; for owning targets, each element expression must produce an owned value).
- `Array<T>` (heap-borrowing) and `OwnedArray<T>` (heap-owning).
- `Set<T>` and `OwnedSet<T>` (sets).
- `LinkedList<T>` and `OwnedLinkedList<T>` (doubly-linked lists).
- `Stack<T>` and `OwnedStack<T>` (LIFO stacks).
- `Queue<T>` and `OwnedQueue<T>` (FIFO queues).
- `Dictionary<K, V>` and `OwnedDictionary<K, V>` (dictionaries; key-value pair syntax `[k: v, ...]`).
- Any user-defined type declaring the `FromCollectionLiteral` static factory ([§15.4.1](#sec-the-user-defined-sequence-literal-protocol)); sequence-literal form only.

The **sequence literal** form `[e1, e2, ...]` (bare expressions) targets every sequence-shaped type above — inline arrays, `Array<T>`/`OwnedArray<T>`, `Set<T>`/`OwnedSet<T>`, `LinkedList<T>`/`OwnedLinkedList<T>`, `Stack<T>`/`OwnedStack<T>`, `Queue<T>`/`OwnedQueue<T>`, and user-defined types per [§15.4.1](#sec-the-user-defined-sequence-literal-protocol). The **dictionary literal** form `[k1: v1, k2: v2, ...]` (colon-separated pairs) targets only the built-in `Dictionary<K, V>` and `OwnedDictionary<K, V>`. The two forms are syntactically distinguished by whether the elements contain a top-level colon; a literal cannot mix bare expressions and colon pairs (doing so is a compile error).

**Element-order semantics by target.** The literal's textual element order maps to the target's natural construction order:

- Inline arrays, `Array<T>`, `OwnedArray<T>`, `LinkedList<T>`, `OwnedLinkedList<T>`: elements are stored in textual order; index 0 (or the list head) is the first literal element.
- `Queue<T>`, `OwnedQueue<T>`: the first literal element is at the front of the queue (the next to be dequeued); the last literal element is at the back. `[1, 2, 3]` dequeues `1`, then `2`, then `3`.
- `Stack<T>`, `OwnedStack<T>`: the first literal element is pushed first and ends up at the bottom; the last literal element is on top (the next to be popped). `[1, 2, 3]` pops `3`, then `2`, then `1`. This matches the reading "the literal lists elements bottom-to-top."
- `Set<T>`, `OwnedSet<T>`: order is not semantically meaningful; duplicates are collapsed (see below). Iteration order of the resulting set follows the set type's documented iteration order (stdlib [§4.10](Bezel_Standard_Library.md#sec-sett-and-ownedsett)), not the literal order.
- `Dictionary<K, V>`, `OwnedDictionary<K, V>`: insertion order follows textual order; this matters because dictionary iteration is insertion-ordered (stdlib [§4.8](Bezel_Standard_Library.md#sec-dictionaryk-v)).

For owning targets, the literal's element expressions must produce owned values; ownership transfers from each expression into the constructed collection. This applies uniformly to `OwnedArray<T>`, `OwnedLinkedList<T>`, `OwnedStack<T>`, `OwnedQueue<T>`, `OwnedSet<T>`, and `OwnedDictionary<K, V>` (value position).

For `Set<T>`, the literal elements must have type `T`. Duplicate elements (per `T`'s equality contract) are silently collapsed; the first occurrence in textual order is retained. For `OwnedSet<T>`, duplicates are silently collapsed and each later duplicate's owned element is disposed at construction (the set retains only the first instance per equality class).

For `Dictionary<K, V>` and `OwnedDictionary<K, V>`, each element in the literal uses `key: value` syntax: the sub-expression left of the colon is the key (type `K`), the sub-expression right of the colon is the value (type `V`). Duplicate keys are a compile error when all duplicate keys are compile-time constants; when duplicates are not statically detectable, the last value in textual order wins, and for `OwnedDictionary` the displaced earlier value is disposed at construction. The key-value pair syntax is recognized only when the target type is `Dictionary<K, V>` or `OwnedDictionary<K, V>`; in any other target context a colon inside a literal element is a compile error.

**Examples.**

```bezel
// Sequence literals targeting different sequence types
using var list:  LinkedList<int> := [10, 20, 30];
using var stack: Stack<int>      := [1, 2, 3];        // 3 is on top
using var queue: Queue<int>      := [1, 2, 3];        // 1 is at the front
using var set:   Set<string>     := ["a", "b", "a"];  // collapses to {"a", "b"}

// Dictionary literal
using var ages: Dictionary<string, int> := [
  "alice": 30,
  "bob":   25,
  "carol": 41
];

// Nested: dictionary of arrays
using var groups: Dictionary<string, Array<int>> := [
  "primes": [2, 3, 5, 7],
  "evens":  [2, 4, 6, 8]
];

// Nested: dictionary of dictionaries
using var config: Dictionary<string, Dictionary<string, int>> := [
  "server": ["port": 8080, "timeout": 30],
  "client": ["retries": 3]
];

// Record-typed keys (no tuples in this version; use a named record)
record Point(X: int, Y: int);
using var labels: Dictionary<Point, string> := [
  Point(0, 0): "origin",
  Point(1, 0): "east"
];

// Owning collections — element expressions must produce owned values
using var conns: OwnedArray<Connection> := [Connect("a"), Connect("b")];
using var pool:  OwnedDictionary<string, Connection> := [
  "primary": Connect("p"),
  "backup":  Connect("b")
];
```

**Empty literals.** The empty literal `[]` is admitted for every collection target listed above (every collection type has a well-defined empty value) but is rejected for inline arrays unless `N` is `0`, and is rejected when no target type is determinable. The empty dictionary literal is also `[]` (there is no `[:]`-style disambiguator) — the target type determines whether `[]` constructs an empty sequence collection or an empty dictionary. An empty literal in a non-collection or undeterminable context is the usual "cannot infer target type" compile error.

**Why `[k: v, ...]` rather than an alternative.** The colon-pair syntax was chosen over the arrow form `[k => v]` (which would overload the `=>` token already used for case arms and expression-bodied accessors), over a brace form `{k: v}` (which would introduce a fourth bracket kind competing for visual distinctness, where `[]` already carries collection-literal meaning), and over a tuple-list form `[(k, v), ...]` (which requires tuple types Bezel does not provide; the named-record workaround `[Pair(k, v), ...]` is verbose and reads as a list of pairs rather than a mapping). The colon-pair form is compact, familiar from JSON/Python/Swift/Ruby, visually distinct from the bare sequence literal, and reuses no token in a conflicting role. See rationale [§5.16](Bezel_Design_Rationale.md#sec-collection-literals-for-complex-collections).

User-defined collection types opt into sequence-literal construction via the protocol of [§15.4.1](#sec-the-user-defined-sequence-literal-protocol). The dictionary-literal form `[k: v, ...]` remains restricted to the built-in `Dictionary<K, V>` and `OwnedDictionary<K, V>`; a user-extensible dictionary-literal protocol is deferred (rationale [§18](Bezel_Design_Rationale.md#sec-things-deferred)).

<a id="sec-user-defined-collection-literal-protocol"></a>
<a id="sec-the-user-defined-sequence-literal-protocol"></a>
#### 15.4.1 The user-defined sequence-literal protocol

A user-defined type opts into sequence-literal construction by declaring a static method named exactly `FromCollectionLiteral` with one of two recognized signatures. The presence of such a method is the opt-in; a type without it is not a sequence-literal target, and a sequence literal in a context targeting such a type is the usual "cannot infer target type" / "no applicable target" compile error.

**Borrowing form.** For a type `C<T>` whose elements are borrows (the type does not own its elements):

```bezel
public static method FromCollectionLiteral(elements: ReadOnlySpan<T>): owned C<T>;
```

**Owning form.** For a type `C<T>` that owns its elements (`where T: class`):

```bezel
public static method FromCollectionLiteral(elements: owned OwnedArray<T>): owned C<T>;
```

Exactly one of the two forms may be declared on a given type. Declaring both is a compile error (the literal's element-expression ownership would not unambiguously select a target). The method must be `public`, must be `static`, must be declared directly on the collection type (not inherited from a base class or supplied by an extension block), and must return `owned C<T>` where `C<T>` is the enclosing type at its own arity. The element type is read directly from the parameter's element type — `T` in `ReadOnlySpan<T>` or `OwnedArray<T>` — and is the type against which every literal element expression is checked.

**Lowering.** For a sequence literal `[e1, e2, ..., en]` with determined target type `C<U>` that declares `FromCollectionLiteral`:

- *Borrowing form.* The compiler materializes a temporary `ReadOnlySpan<U>` over the n element values (the storage for the span may be stack-allocated; its lifetime encloses only the `FromCollectionLiteral` call) and emits `C<U>.FromCollectionLiteral(thatSpan)`. Each `ei` is type-checked against `U` and must be implicitly convertible to it. The temporary span and its backing storage are released after the call returns; the factory must not retain the span beyond its own execution (the span is a borrow-bearing value subject to [§28.6](#sec-leak-prevention) — storing it in the constructed collection's field is a borrow-lifetime violation, diagnosed in the factory body, not at the literal site).
- *Owning form.* The compiler materializes a temporary `owned OwnedArray<U>`, evaluates each `ei` (which must produce an owned `U`) and transfers it into the temporary in textual order, then emits `C<U>.FromCollectionLiteral(thatArray)` transferring ownership of the temporary array into the call. The factory drains the array (typically via the array's `move`-based element extraction) and is responsible for the array's discharge per the standard owned-parameter rules ([§28.3](#sec-local-variables-ownership-and-borrows)). If element evaluation throws partway, the already-evaluated owned elements and the partially-filled temporary array are disposed by the standard exception-path discharge ([§28.3](#sec-local-variables-ownership-and-borrows)), exactly as for an `OwnedArray<U>` literal.

The empty literal `[]` targeting such a type is admitted: the borrowing form receives an empty `ReadOnlySpan<U>`; the owning form receives an empty `OwnedArray<U>`. The factory is responsible for handling the empty case (it always must, since an empty collection is a valid general input).

**Result ownership.** `FromCollectionLiteral` returns `owned C<U>`; the literal expression therefore produces an owned value, subject to the standard owned-result discharge rules ([§28.3](#sec-local-variables-ownership-and-borrows)) — identical to the ownership treatment of a built-in `Array<U>` / `OwnedArray<U>` literal.

**Recognition category.** This is name-based recognition: the compiler looks up a specifically-named `public static` method with a recognized signature. It is distinct from the interface-based iteration protocol ([§29.1](#sec-the-protocol)) and from the single structural-dispatch rule for `Contains` / `in` ([§17.5](#sec-the-in-operator)). The dictionary-literal form is *not* covered by this protocol; only the bare sequence-literal form `[e1, ..., en]` participates. Rationale [§5.16](Bezel_Design_Rationale.md#sec-collection-literals-for-complex-collections) explains the choice of mechanism, the contrast with C#'s `[CollectionBuilder]` attribute, and the forward-compatibility path to an interface-based form should static interface members be added.

**Example.**

```bezel
public class RingBuffer<T>
  private _slots: Array<T>;
  private _head: int;

  public constructor(capacity: int)
    _slots := new Array<T>(capacity);
    _head := 0;
  end;

  public method Push(item: T) ... end;

  public static method FromCollectionLiteral(elements: ReadOnlySpan<T>): owned RingBuffer<T>
    var rb := new RingBuffer<T>(elements.Length);
    for var i in 0..<elements.Length do
      rb.Push(elements[i]);
    end;
    return rb;
  end FromCollectionLiteral;
end RingBuffer;

// Usage — identical syntax to built-in collections:
using var rb: RingBuffer<int> := [1, 2, 3, 4];
using var empty: RingBuffer<int> := [];
```

<a id="sec-length-vs-count"></a>
<a id="sec-length-and-count"></a>
### 15.5 Length and Count

| Member | Used by | Meaning |
|---|---|---|
| `.Length` | `[N]T` (inline), `string` | Size, compile-time constant for inline arrays |
| `.Count` | `Array<T>`, `OwnedArray<T>`, and other resizable collections | Current size; may change for resizable collections |

Heap arrays expose `.Count`. A heap array used in fixed-size discipline still has `.Count`, equal to its capacity, throughout its lifetime; the language does not enforce immutability of `.Count`, only the user's discipline.

<a id="sec-slicing"></a>
### 15.6 Slicing

Range slicing `arr[a..b]`, `arr[..b]`, `arr[a..]`, `arr[..]` is supported on `Array<T>`. Slicing produces a new `Array<T>` containing copies of the references (or values) in the range; mutating the slice does not affect the original.

Out-of-bounds endpoints raise `IndexOutOfRangeException`. An empty range produces an empty array.

Slicing is not supported on inline arrays in this version. A copy-returning slice would need the range bounds to be compile-time constants (the result type `[K]T` encodes its length in the type), and a borrowing slice would need a span type over inline-array storage — but no inline-array span type exists (`Span<T>` and `ReadOnlySpan<T>` view `OwnedBuffer<T>`; `ArraySpan<T>` views heap `Array<T>`; none views `[N]T`). A span over inline arrays is deferred indefinitely (rationale [§5.15](Bezel_Design_Rationale.md#sec-owned-buffers-and-spans), [§18](Bezel_Design_Rationale.md#sec-things-deferred)). Users who want a sub-range of an inline array iterate the index range explicitly:

```bezel
for var i in 2..5 do Process(arr[i]); end;
```

Slicing is not admitted on `OwnedArray<T>`. The copy-returning semantics would manufacture a second owner for each owned element, violating the ownership contract. Users who want a range-bounded ownership transfer use `Drain()` plus filtering, or a sequence of `PopAt` calls. Users who want a borrowing view of a sub-range iterate the index range explicitly — the indexer-get produces a borrow, which respects ownership without taking it.

Multi-dimensional arrays are deferred to a future revision. The current spec does not provide `T[,]` or `[N, M]T`-style multi-dimensional inline arrays.

The copy-returning semantics for `Array<T>` slicing are stable. Callers wanting a non-allocating sub-range view of an `Array<T>` use `arr.AsSpan(start, length)` to obtain an `ArraySpan<T>` (standard library [§4.15](Bezel_Standard_Library.md#sec-arrayspant-readonlyarrayspant)) — a record carrying a borrow of the array, no allocation. Slicing (`arr[start..end]`) and span-view construction (`arr.AsSpan(start, length)`) coexist as complementary operations; the first manufactures a fresh independent array, the second produces a borrowing view.

---

<a id="sec-range-expressions"></a>
## 16. Range Expressions

A range is a value representing a contiguous span of an ordered type. Ranges are first-class: they may be stored, passed, and returned.

<a id="sec-construction"></a>
### 16.1 Construction

```bezel
var r := 1..10;                       // inclusive: 1 through 10
var h := 0..<10;                      // half-open: 0 through 9
```

`a..b` is inclusive on both ends; `a..<b` is inclusive on the lower bound, exclusive on the upper.

<a id="sec-open-ended-ranges"></a>
### 16.2 Open-ended ranges

A range may omit one endpoint, producing an open-ended range used for slicing and pattern matching:

```bezel
arr[2..5]            // elements 2 through 5 inclusive
arr[..5]             // from start through 5
arr[10..]            // from 10 through last
arr[..]              // all
```

In a pattern, `..<0` matches "any value less than 0" and `31..` matches "any value at or above 31."

<a id="sec-the-ranget-type"></a>
### 16.3 The `Range<T>` type

A range over `T` has type `Range<T>`. `T` is restricted by the compiler to the integer types (`byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`) and `char`. Other substitutions are rejected. This restriction cannot be expressed through a `where` clause in the current type system; it is enforced as a compiler-intrinsic rule. A future revision is expected to replace this restriction with a generic constraint once static interface members enable an `ISteppable<T>` or equivalent interface.

`Range<T>` is a compiler-intrinsic record that implements `IEnumerable<T>`:

```bezel
/// Compiler-intrinsic; T restricted to integer types and char.
public record Range<T>: IEnumerable<T>
  public readonly property Start: T?;
  public readonly property End: T?;
  public readonly property Step: long;
  public readonly property IsEndInclusive: bool;
  public readonly property IsOpenEnded: bool;
  public readonly property IsEmpty: bool;

  public method Contains(value: T): bool;
  public method GetEnumerator(): owned IEnumerator<T>;
end Range;
```

`Start` and `End` are nullable; a `null` endpoint indicates the range is open-ended on that side. For closed ranges (both endpoints present), both are non-null.

`Step` is a signed 64-bit integer to support descending iteration over both signed and unsigned `T`. The default step is 1. For `T = ulong`, the magnitude of `Step` is bounded above by `long.MaxValue`; ranges over `ulong` cannot specify a step that exceeds `2^63 - 1`. This is rarely an issue in practice — typical `ulong` strides fit comfortably — but the limit is worth noting for code that computes a step at runtime.

The `Step` type is independent of `T`'s range, so values that exceed `T`'s representable range are accepted at construction. The bounds are enforced at iteration time: a range like `0..10 step 256` over `T = byte` constructs without error, but the enumerator advances by 256 (interpreted as a `long`) and the first computed value past the start is `256`, which exceeds the upper endpoint, so the iteration yields only the start value and terminates. A range like `0..255 step 128` over `T = byte` yields `0` and `128`, then would compute `256` which overflows the range's upper endpoint and terminates. No `OverflowException` is raised — overflow at the endpoint is the iteration's termination condition, not an error. Code computing a step at runtime should validate against `T`'s range if a stricter check is desired.

The same termination rule applies to overflow of `T`'s representable range, even when the range's declared endpoint is at or near `T`'s maximum value. A range like `0..ulong.MaxValue step 1000000000000000000` over `T = ulong` yields a sequence of values that approach `ulong.MaxValue` and stop when the next computed value would exceed `ulong.MaxValue` — whether or not the next value would also exceed the declared endpoint. The enumerator computes the next value internally as `long`-arithmetic-relative-to-the-current `T` value; if the addition would overflow `T`'s representable range, iteration terminates at the last value that fit. Iteration over `T = byte` from `200 step 100` similarly yields `200` then `(byte)44` would overflow; iteration terminates at `200`. The rule is unconditional silent termination on either overshoot of the range endpoint or overflow of the type's range, whichever happens first.

<a id="sec-iteration-over-rangechar"></a>
#### Iteration over `Range<char>`

For `T = char`, the validity of `T`'s range is itself non-contiguous: code points U+D800 through U+DFFF are surrogate code points and not valid `char` values ([§4.5](#sec-char)). `char` ranges follow the same termination rule as for numeric overflow — iteration stops when the next computed code point would be invalid.

Concretely: a range from `'\u0000'` upward (whether closed at U+10FFFF, open-ended, or stepping to a value past U+D7FF) yields code points U+0000 through U+D7FF, then terminates. The surrogate range U+D800 through U+DFFF is not skipped over; iteration treats the surrogate boundary as the effective upper edge of `char`'s lower segment. Code that wants to iterate the full Unicode code point range, with the surrogate gap treated as a no-op, must iterate two ranges:

```bezel
for var c in '\u0000'..'\uD7FF' do Process(c); end;
for var c in '\uE000'..'\U0010FFFF' do Process(c); end;
```

A range whose `Start` is U+E000 or higher iterates that upper segment without surrogate concerns; the surrogate-boundary rule applies only when iteration would cross from U+D7FF to U+D800. A descending range starting at U+10FFFF yields code points down through U+E000 and terminates when the next computed value would be U+DFFF.

Step strides that would land within the surrogate range terminate at the same boundary. `'\uD7F0'..'\uE010' step 4` yields U+D7F0, U+D7F4, U+D7F8, U+D7FC, then would compute U+D800 — a surrogate — and terminates. No `InvalidOperationException` is raised; the surrogate boundary is iteration termination, not an error.

Surrogate code points themselves cannot be written as `char` literals — `'\uD800'` and similar escapes are compile errors per [§18.3](#sec-character-literals) and [§18.8](#sec-escape-sequences-non-verbatim), and `char(0xD800)` raises `OverflowException` at runtime per [§5.2](#sec-explicit-conversions). Range endpoints, however, are integer-valued at construction; a `Range<char>` constructed from the integer endpoints by other means (e.g. by deserialization from a saved value) is bounded by `char`'s validity contract regardless of how the endpoints were spelled. The `Contains` operation uses the integer-membership rule of [§17.5](#sec-the-in-operator) and does not exclude surrogates from membership testing — for a range `r` whose integer extent covers U+D800, the integer-level membership test for U+D800 returns `true`, but iteration of `r` will not yield that value. The asymmetry is deliberate: `Contains` answers a mathematical question about the range's extent, while iteration produces a sequence of valid `char` values.

`IsOpenEnded` is `true` when `Start` is `null` or `End` is `null`.

`IsEmpty` is `true` when the range contains no values (the lower bound exceeds the upper bound in the direction of iteration, or both endpoints are present and equal with an exclusive upper bound).

`Contains` is the operation invoked by `in` ([§17.5](#sec-the-in-operator)). Membership is determined by endpoints and inclusivity; `Step` does not affect `Contains`.

`GetEnumerator` produces an enumerator that yields values from `Start` to `End` (inclusive or exclusive per `IsEndInclusive`), incrementing by `Step`. Calling `GetEnumerator` on an open-ended range raises `InvalidOperationException`. The compiler warns when an open-ended range literal appears directly in a `for` statement or other iteration context.

<a id="sec-stride"></a>
### 16.4 Stride

A range may include a `step` modifier. The step is part of the range value:

```bezel
var evens := 0..100 step 2;          // 0, 2, 4, …, 100
var down := 100..0 step -1;          // 100, 99, …, 0
var fives := 0..<50 step 5;          // 0, 5, 10, …, 45
```

Rules:

- Step's sign must agree with the range's direction. A range whose direction disagrees with its step's sign produces an empty iteration.
- When the compiler can prove at compile time that a range's direction and step sign disagree (both endpoints and step are constants or constant after folding), it emits a warning: *"this range produces an empty iteration."* The warning is not produced for runtime-computed steps, where sign mismatches may be intentional.
- A literal step of zero is a compile error; a computed step of zero is a runtime error (`ArgumentException`).
- `step` may not be applied to open-ended ranges.
- `step` is not legal in `case` patterns (patterns test membership; stride has no meaning).

<a id="sec-iteration"></a>
### 16.5 Iteration

A range may be iterated with `for` ([§21.4](#sec-the-for-statement)):

```bezel
for var i in 1..10 do Console.WriteLine(i); end;
for var i in 0..<n do Process(i); end;
for var i in 0..100 step 2 do Console.WriteLine(i); end;
```

`..` produces inclusive iteration; `..<` excludes the upper bound.

Because `Range<T>` implements `IEnumerable<T>`, ranges compose with all IEnumerable-based APIs:

```bezel
import System.Collections.Linq;
var evenSquares := (1..20).Where((x) => x mod 2 = 0).Select((x) => x * x);
```

<a id="sec-compiler-optimization"></a>
#### Compiler optimization

The compiler may optimize `for var i in rangeExpr do ... end` into a counted loop that bypasses the `IEnumerable` protocol, provided the observable behavior is identical. For range literals with constant endpoints, this produces tight loop code with no heap allocation. For range variables, the compiler may still optimize when it can prove the range is closed.

<a id="sec-iteration-open-ended-ranges"></a>
#### Open-ended ranges

Iterating an open-ended range raises `InvalidOperationException` at the call to `GetEnumerator`, before any iteration occurs. The compiler emits a warning when an open-ended range literal appears in an iteration context:

```bezel
for var i in 10.. do ... end;     // WARNING: open-ended range cannot be iterated
```

For ranges stored in variables, the check is deferred to runtime:

```bezel
var r: Range<int> := 1..10 if someCondition else 10..;
for var i in r do ... end;        // InvalidOperationException if r is open-ended
```

Open-ended ranges are designed for slicing ([§15.6](#sec-slicing)) and pattern matching ([§19.3](#sec-patterns)), not iteration.

<a id="sec-empty-ranges-and-equal-bound-ranges"></a>
### 16.6 Empty ranges and equal-bound ranges

A range whose lower bound exceeds its upper bound (in the direction of iteration) is empty. Iterating an empty range is a no-op. Matching against an empty range is a non-match. Construction is not an error.

A range whose bounds are equal is a special case where direction is undefined:

- An **inclusive** equal-bounds range (`5..5`, `5..5 step 1`, `5..5 step -1`) yields exactly one value — the bound itself. The step's sign does not matter; the iteration produces the start, the next computed value (start ± step) is past the inclusive end, and iteration terminates.
- A **half-open** equal-bounds range (`5..<5`, `5..<5 step n`) is empty for any step. The lower bound equals the upper bound and the upper bound is excluded, so no value is in range.

Equal-bound ranges with `step 0` are governed by [§16.4](#sec-stride): a literal step of 0 is a compile error; a computed step of 0 raises `ArgumentException` at iteration time.


---

<a id="sec-expressions-and-operators"></a>
## 17. Expressions and Operators

<a id="sec-precedence"></a>
### 17.1 Precedence

Operators are evaluated according to the following table, from highest to lowest precedence.

| Level | Category | Operators | Associativity |
|---|---|---|---|
| 1 | Primary | `.` `?.` `[ ]` `( )` `?( )` `new` `!` (postfix) | left-to-right |
| 2 | Unary | `+` `-` `not` `~` (prefix), `await` | right-to-left |
| 3 | Multiplicative | `*` `/` `mod` | left-to-right |
| 4 | Additive | `+` `-` | left-to-right |
| 5 | Shift | `<<` `>>` | left-to-right |
| 6 | Range | `..` `..<` (with optional `step`) | non-associative |
| 7 | Bitwise AND | `&` | left-to-right |
| 8 | Bitwise XOR | `^` | left-to-right |
| 9 | Bitwise OR | `\|` | left-to-right |
| 10 | Relational | `<` `>` `<=` `>=` `in` `is` `is not` `as` | left-to-right |
| 11 | Equality | `=` `<>` | left-to-right |
| 12 | Null-coalescing | `??` | right-to-left |
| 13 | Logical AND | `and` | left-to-right |
| 14 | Logical OR | `or` | left-to-right |
| 15 | Conditional | `expr if cond else expr`, `case ... end`, `throw expr` | right-to-left |
| 16 | Assignment | `:=` `+=` `-=` `*=` `/=` `&=` `\|=` `^=` `<<=` `>>=` `??=` | right-to-left |

<a id="sec-notes-on-operators"></a>
### 17.2 Notes on operators

- `and` and `or` short-circuit: the right operand is not evaluated if the result is determined by the left.
- `??` is right-associative: `a ?? b ?? c` means `a ?? (b ?? c)`. `??` binds tighter than `and`/`or`, so `a ?? b or c` parses as `(a ?? b) or c`.
- `?.`, `?(...)`, and `!` (postfix) are primary-level. They bind as tightly as `.` and `[ ]`.
- `case ... end` and `throw expr` are listed at level 15 nominally; both are self-delimited (`case` by `end`, `throw` by being a transfer of control), so their precedence is rarely visible to readers.
- Assignments and compound assignments are statements, not expressions; they produce no value.
- `++` and `--` are statement-only ([§17.6](#sec-the-increment-and-decrement-operators)) and do not appear in the precedence table. They are not expression operators; they cannot be combined with other operators.
- `mod` has no compound-assignment form; `mod=` is not valid syntax. The rare uses where it would apply (`x := x mod c`) are written explicitly by design — `mod` is uncommon in compound forms.
- Range operators `..` and `..<` bind looser than arithmetic and shift, but tighter than the bitwise, relational, equality, logical, and conditional operators. Worked examples:
  - `0..n+1` parses as `0..(n+1)` — Range looser than Additive.
  - `a..b*2 step 2` parses as `a..(b*2) step 2` — Range looser than Multiplicative, and `step`'s operand binds at the same level as the range bounds.
  - `1<<2..1<<10` parses as `(1<<2)..(1<<10)` — Range looser than Shift.
  - `score in 60..100` parses as `score in (60..100)` — Range tighter than Relational.
  - `r = 0..10` parses as `r = (0..10)` — Range tighter than Equality.
  - `0..10 if cond else 20..30` parses as `(0..10) if cond else (20..30)` — Range tighter than Conditional.
- The placement of Range in the bitwise band (level 6) is deliberate. Earlier revisions placed Range at high precedence (just below Primary), which forced explicit parentheses around any arithmetic in range bounds. v0.69 moved Range to the lowest binary level, but that broke `score in 60..100` and similar `in`-with-range idioms. v0.71 settled on level 6, which makes both arithmetic-in-bounds (`0..n+1`) and `in`-with-range (`x in a..b`) parse naturally without parentheses; bitwise-with-range mixes are rejected by the mandatory-parenthesization rules below.
- `await` is a unary-precedence prefix operator (level 2). Worked examples:
  - `await x.y` parses as `await (x.y)` — primary access binds tighter than unary `await`.
  - `await x + 1` parses as `(await x) + 1` — `await` is unary-level, additive `+` is looser, so the additive operator combines the awaited result with `1`. To await the *sum*, write `await (x + 1)`.
  - `await -x` parses as `await (-x)` — both `await` and unary `-` are at level 2, and the level is right-recursive in the grammar, so `await` wraps a unary expression that itself begins with `-`.
  - `(await x) + 1` is the explicit form for "first await, then add."
  - `await await x` is permitted (rare but valid: awaiting a `Task<Task<T>>` gives a `Task<T>`, which is then awaited again).
- Parentheses `( )` always override precedence.

<a id="sec-mandatory-parenthesization"></a>
#### Mandatory parenthesization

The following operator pairs are **non-associative across categories** and require explicit parenthesization when mixed in the same expression. The compiler rejects unparenthesized mixes with a syntax error suggesting where to place parentheses.

- Bitwise (`&`, `|`, `^`) with equality (`=`, `<>`) or relational (`<`, `<=`, `>`, `>=`).
  *Example:* `a & 1 = 0` is a compile error; write `(a & 1) = 0`.
- Bitwise with logical (`and`, `or`).
  *Example:* `flags & MASK and ready` is a compile error; write `(flags & MASK) <> 0 and ready`.
- Shift (`<<`, `>>`) with arithmetic (`+`, `-`).
  *Example:* `a << 2 + 1` is a compile error; write `a << (2 + 1)` or `(a << 2) + 1`.
- Range (`..`, `..<`) with bitwise (`&`, `|`, `^`).
  *Example:* `mask & 0..n` is a compile error; write `(mask & 0)..n` or `mask & (0..n)` to disambiguate. (In practice, bitwise-with-range mixes are almost certainly bugs, and the explicit parentheses make the intent visible.)

These mandatory-parenthesization rules cost nothing on programs that mean what they say and catch the precedence pitfalls C inherited (notably the `a & MASK = 0` bug). They apply only across the listed categories; within a single category the table's associativity governs as usual.

<a id="sec-the-=-and-operators"></a>
### 17.3 The `=` and `<>` operators

`=` and `<>` invoke `Equals` with a null-safe wrapper ([§8.4](#sec-=-and)). For classes, `=` cannot be overloaded; for records and enums, it can. For inline arrays, `=` cannot be overloaded (inline arrays are neither classes, records, nor enums, and the [§26.2](#sec-what-can-be-extended) extension rules forbid extending them); the default structural equality of [§8.3](#sec-equals-and-gethashcode) is used.

<a id="sec-the-is-is-not-and-as-operators"></a>
### 17.4 The `is`, `is not`, and `as` operators

Type tests and narrowing are described in [§12.7](#sec-type-tests-is-is-not-as). Two operator-level details are worth noting here.

**Precedence.** `is`, `is not`, and `as` bind at the relational precedence level (level 10, [§17.1](#sec-precedence)). `is not` is a single operator with two-token spelling — the parser does not accept `not is`, and `not (x is T)` is a different expression (a boolean negation of a relational test) with the same truth value but no narrowing in the negated branch.

**`as` on value types.** When the target type is a value type, `x as T` produces `T?`: the boxed `Object`-or-interface input is unboxed to the value type if compatible, and the result is `null` otherwise. So `someObj as int` has type `int?`. The result type of `as` is always nullable, independent of whether the target is a value or reference type.

**`as` and ownership.** `x as T` is **non-destructive on its operand**: the operation reads `x` and inspects its runtime type without moving from it. The result is a *borrow* of `x` (more precisely, a borrow with the same source as `x`), regardless of whether `x` is owned or borrowed.

The result is borrowed even when the operand is owned because the result type is nullable, and `null` cannot carry ownership. A successful cast yields a non-null borrow narrowable to the target type; a failed cast yields `null`. In both cases, the original `x` retains its ownership state — the cast neither transfers ownership out of `x` nor disposes `x`'s value. Code that needs the result to be owned must move from `x` explicitly into a target-typed owned location after narrowing:

```bezel
using var thing: Animal := Acquire();          // owned
var maybeDog := thing as Dog;                  // borrow; thing still owned
if maybeDog <> null then
  // maybeDog is a borrowed Dog reference; thing remains the owner.
  // To transfer ownership, the standard owned-local rules of §28.3 apply
  // to `thing`, not to the result of `as`.
end;
```

This design matches the analogous rule for `is`: type tests do not move from their operand. See the producer table in [§28.3](#sec-local-variables-ownership-and-borrows).

<a id="sec-the-in-operator"></a>
### 17.5 The `in` operator

`x in collection` evaluates to `true` if `collection.Contains(x)` is true. The right operand must expose a `Contains(T): bool` method (instance or extension):

```bezel
var found := 42 in numbers;
var inRange := score in 60..100;
var hasChar := 'a' in "abcdef";
```

`in` produces `bool`. If the left operand is nullable, it must be checked before use. `in` cannot be overloaded directly; types customize membership by providing `Contains`.

The `Contains` lookup is structural rather than interface-based: any accessible method named `Contains` taking one parameter of compatible type and returning `bool` qualifies, whether it is an instance member or an extension method ([§26](#sec-extension-blocks)). This is the only structural-dispatch rule in Bezel's expression surface (the iteration protocol of [§29.1](#sec-the-protocol) is interface-based by contrast; the user-defined collection-literal protocol of [§15.4.1](#sec-the-user-defined-sequence-literal-protocol) is name-based — it requires a method of one *exact* name, not any method of a matching shape, so it is recognition-by-declared-name rather than structural duck-typing). The structural form was chosen here because `Contains` predates `in` in many existing types — including `string`, `Range<T>`, `Array<T>`, `Set<T>`, and `Dictionary<K, V>` — and forcing them all through a common `IContainer<T>` interface would add a layer that callers never write or see.

Inline arrays (`[N]T`, [§15.1](#sec-inline-arrays)) do not declare a `Contains` method and cannot be extended ([§26.2](#sec-what-can-be-extended)), so `x in inlineArr` is not admitted. Programs that need a membership test against an inline array iterate the array explicitly (`for var item in arr do if item = x then ... end; end;`) or materialize the contents into a heap collection that does expose `Contains`.

<a id="sec-the-increment-and-decrement-operators"></a>
### 17.6 The increment and decrement operators

`++` and `--` are postfix-only. They are statements, not expressions:

```bezel
var c := 5;
c++;            // c is 6
c--;            // c is 5
```

`x := y++` is a compile error; `++c` is a parse error. There is no prefix form.

The operand must be an assignable lvalue (a local variable, a writable field, an indexer slot, or a property with a setter). Read-only locations cannot be incremented. For a value-type operand whose type T overloads `operator ++`, the statement `x++` is equivalent to `x := T.operator++(x)`; the new value replaces the old through the lvalue's normal assignment path. For built-in numeric types, `x++` is equivalent to `x := x + 1` (and likewise `x--` to `x := x - 1`), with the same overflow semantics as `+` and `-` ([§6.1](#sec-integer-arithmetic)).

For reference-type operands, `++` and `--` apply only when the reference type overloads the operator; the overload returns a new instance, and the statement reassigns the local or field to the new instance. Overloading `++`/`--` on a class is permitted but uncommon; the `Counter` example in [§25.3](#sec-examples) demonstrates the typical record use.

When `++` or `--` is applied to an `owned` field of class type T whose `operator ++` / `operator --` returns a new instance, the previous value of the field is overwritten and the [§28.7](#sec-reassigning-owned-fields) reassignment rule applies: the previous owned value must be disposed before the field is overwritten, and the operator's body must produce an owned value to be stored back. The compiler warns on `++` and `--` against owned class-typed fields for the same reason it warns on direct reassignment without prior disposal — the leak risk is identical. Authors of class-typed types that overload `++` or `--` should document the intended use; in practice these overloads are far easier to reason about on records, where ownership is not at issue.

<a id="sec-increment-decrement-on-indexer-slots"></a>
#### Increment/decrement on indexer slots

For an indexer slot `expr[i]` (or `expr[i, j, ...]` for a multi-dim indexer), `expr[i]++` desugars to a read-modify-write through the indexer:

```bezel
// Source:                         // Desugars to:
counts[category]++;                do
                                     var __coll := counts;
                                     var __idx := category;
                                     __coll[__idx] := __coll[__idx] + 1;
                                   end;
```

The full desugaring is:

```bezel
do
  var __coll := expr;
  var __idx := i;
  __coll[__idx] := __coll[__idx] + 1;
end;
```

Each subexpression in the LHS — the collection expression and every index expression — is evaluated exactly once. The indexer's getter and setter are each called once. For a value-type element T that overloads `operator ++`, the assigned value is `T.operator++(__coll[__idx])` instead of `__coll[__idx] + 1`.

Implementations may not deduplicate the getter+setter pair: every `arr[i]++` performs one logical read followed by one logical write through the indexer. For collections where this cost matters (e.g. an `arr[k]++` on a `Dictionary<K, int>` that pays two hash lookups), users should write the read and write explicitly:

```bezel
var v := dict[key];
dict[key] := v + 1;
```

The same desugaring applies to `--`. The same desugaring applies to `+=`, `-=`, and the other compound assignments on indexer slots, with the operator changed accordingly.

When the indexer's element type is a class T whose `operator ++` / `operator --` returns a new instance, the desugaring would assign a fresh owned value to a slot. For a non-owning collection (`Array<MyClass>`), the slot does not own its content per [§28.11](#sec-collections-do-not-own-elements), so the assignment of an owned `new`-result to a non-owned slot would leak; per [§28.6](#sec-leak-prevention), this is rejected at compile time. For an owned-element collection (`OwnedArray<MyClass>`), the slot owns its content; the assignment disposes the previous value and installs the new one (per [§28.11](#sec-collections-do-not-own-elements)'s indexer-set rule), which is the same rule as for an owned-field reassignment. `arr[i]++` is therefore admitted on `OwnedArray<MyClass>` when MyClass overloads `operator ++`, and rejected on `Array<MyClass>`. The asymmetry is the same as for direct slot assignment.

<a id="sec-the-new-operator"></a>
### 17.7 The `new` operator

`new T(args)` constructs an instance of T. For classes, `new` produces an owned reference ([§28](#sec-resource-management)). For records, `new` produces a value.

`new` is not used to construct inline arrays. Inline arrays (`[N]T`, [§15.1](#sec-inline-arrays)) are constructed by collection literals (`[expr, expr, ...]`, contextually typed to the array's declared type, [§15.4](#sec-collection-literals)) or by the implicit zero/null fill at declaration when the element type is zero-initializable. `new [3]int(0, 1, 2)` is rejected by the type checker — inline arrays do not declare constructors, and the `new` syntax has no meaning on them.

A field initializer or typed local declaration may use the `new()` shorthand when the type is unambiguous:

```bezel
private _items: Array<int> := new();
private _config: Config := new(verbose: true);
var xs: Array<int> := new();
```

The shorthand is permitted in field initializers, typed local-variable declarations, return statements (when the method has a declared return type), and default parameter values. It is not permitted in general expression contexts where the type would have to be inferred from broader context (overload resolution, `var` without annotation).

<a id="sec-the-null-forgiving-operator"></a>
### 17.8 The null-forgiving operator `!`

`x!` asserts that `x` is non-null. The result type is the non-nullable form of the expression's type. If `x` is null at runtime, `NullReferenceException` is raised.

The null-forgiving operator preserves ownership: if `x` has ownership state *owned* (a moved-from-after-this read pattern applies to the result, just as for a plain owned read), `x!` is owned; if `x` is borrowed, `x!` is borrowed with the same source. The operator is purely a type-system assertion (with a runtime null check); it does not move, copy, or borrow beyond what reading `x` already does.

`!` is the only postfix `!` operator. There is no prefix `!`; boolean negation is `not`.

<a id="sec-the-nameof-operator"></a>
### 17.9 The `nameof` operator

`nameof(expr)` returns the source-level name of an identifier as a compile-time `string` constant.

```bezel
nameof(amount)              // "amount"
nameof(_balance)            // "_balance"
nameof(BankAccount)         // "BankAccount"
nameof(BankAccount.Deposit) // "Deposit"
nameof(System.Console)      // "Console"
```

Rules:

- For a dotted expression, the result is the rightmost identifier.
- For a type parameter `T`, `nameof(T)` returns the source-level name as written in the declaration: `"T"`, `"TElement"`, etc. It does not return the substituted runtime type's name.
- Method calls, indexings, arithmetic, and literals are compile errors: `nameof(Foo())`, `nameof(a + b)`, `nameof(arr[0])`, `nameof(42)`.

For the fully-qualified name of a type, use `typeof(T).FullName` ([§17.10](#sec-the-typeof-operator)). `nameof` deliberately returns only the rightmost identifier; programmers wanting the namespace path go through `typeof`.

The result is a compile-time constant: usable in `const` initializers, default parameter values, attribute arguments, and any other constant context. There is no runtime cost.

<a id="sec-the-typeof-operator"></a>
### 17.10 The `typeof` operator

`typeof(TypeName)` returns the `Type` value for a statically named type:

```bezel
var t: Type := typeof(BankAccount);
Console.WriteLine(t.Name);        // "BankAccount"
Console.WriteLine(t.FullName);    // "BankingApp.BankAccount"
```

`Type` is a sealed class. Its full surface is defined in the standard library ([§2.13](Bezel_Standard_Library.md#sec-the-type-class)); the members most commonly used are:

```bezel
public sealed class Type
  public readonly property Name: string;            // "BankAccount"
  public readonly property FullName: string;        // "BankingApp.BankAccount"

  // Kind predicates — see stdlib §2.13 for the full set and the exact mapping
  // to the §3.1 type kinds.
  public readonly property IsClass: bool;
  public readonly property IsInterface: bool;
  public readonly property IsRecord: bool;
  public readonly property IsEnum: bool;
  public readonly property IsValueType: bool;

  // Lattice queries.
  public readonly property BaseType: Type?;
  public method Implements(iface: Type): bool;
  public method IsAssignableFrom(other: Type): bool;

  // Enum metadata (valid only when IsEnum; otherwise raises
  // InvalidOperationException).
  public readonly property EnumNames: Array<string>;
  public readonly property EnumValues: Array<long>;
end Type;
```

The `Type` surface is deliberately confined to type *identity* and *lattice metadata*. There is no member enumeration, field/method access by name, attribute reading, or dynamic construction: Bezel provides type identity, not member-access reflection. The reasoning — the ownership system has no static site to carry the contract for a reflectively-accessed `owned` member, and member reflection is the maximal violation of the no-hidden-magic principle — is given in rationale [§5.18](Bezel_Design_Rationale.md#sec-type-identity-not-reflection).

The runtime guarantees a single `Type` instance per type, so `=` and `Object.ReferenceEquals` agree on `Type` values. For generic instantiations, each distinct substitution produces a distinct `Type`: `typeof(Array<int>)` and `typeof(Array<string>)` are different `Type` values, but `typeof(Array<int>)` evaluated at two different call sites produces the same instance. Inside a generic body, `typeof(T)` returns the `Type` of the substituted type argument at runtime — it produces a different `Type` for each distinct substitution.

`typeof` accepts:

- Primitive types: `typeof(int)`, `typeof(string)`.
- User-defined types: `typeof(BankAccount)`, `typeof(Priority)`.
- Generic instantiations: `typeof(Array<int>)`, `typeof(Dictionary<string, int>)`.
- Inline-array types: `typeof([3]int)`, `typeof([16 owned]Connection?)` ([§15.1](#sec-inline-arrays)). Each `[N]T` combination produces a distinct `Type` value; `typeof([3]int)` and `typeof([4]int)` are different.
- Type parameters inside a generic body: `typeof(T)` returns the runtime substituted type.

`typeof` does not accept variables, method names, or literals — those are compile errors. For the runtime type of a *value*, use `value.GetType()` ([§8.2](#sec-gettype)).

`typeof(this)` and `typeof(base)` are compile errors. `this` and `base` are values, not type names. To obtain the runtime type of the receiver, use `this.GetType()`. To obtain the static type of the enclosing class, write the class name explicitly: `typeof(BankAccount)`.

<a id="sec-the-default-operator"></a>
### 17.11 The `default` operator

`default(T)` produces the zero value of type T:

- Numeric types: `0`, `0.0`, `0m`, etc.
- `bool`: `false`.
- `char`: `'\0'`.
- Enums: the value with underlying integer 0 (which need not be a declared constant). The compiler emits a warning when `default(T)` is used on an enum type that has no declared constant with underlying value 0, since the resulting value is not equal to any named constant.
- Reference types: `null` if the location is nullable.
- Records whose fields all have well-defined zero values: the record value with each field at its zero value. For positional records, the positional fields are considered. For body-only records, the backing storage of all auto-properties is considered. Expression-bodied (computed) properties have no backing storage and do not affect `default` eligibility.
- Inline arrays whose element type has a well-defined zero value: the array with each slot at the element's zero value. `default([3]int)` is `[0, 0, 0]`; `default([5]Connection?)` is five `null`s. Owning inline-array forms (`[N owned]T?`) are admitted: `default([16 owned]Connection?)` produces 16 null slots and is a valid (move-only) value.
- The unit type: `()`.

`default(T)` is a compile error for:

- A non-nullable reference type.
- A record any of whose declared fields is a non-nullable reference type. This check is recursive: if a record field is itself a record type, that inner record's fields are also checked.
- An inline array whose element type is a non-nullable reference type. `default([3]Connection)` (non-nullable Connection elements) is a compile error; the user must supply explicit initializers. Owning inline arrays `[N owned]T` (non-nullable owning) are likewise an error for the same reason.

In a generic body:

- For unconstrained `T`, `default(T)` is permitted but its result is treated as possibly null and cannot be assigned to a non-nullable T-typed location.
- Under `where T: notnull` with value-type substitutions, `default(T)` is non-null.
- Under `where T: value`, `default(T)` is always non-null.
- Under `where T: class` (or any type-bound constraint that implies non-null T per [§14.2](#sec-constraints)), `default(T)` is a compile error — the constraint guarantees T is non-nullable, and the null value `default(T)` would produce does not fit T's contract. To express the nullable counterpart, write `default(T?)`, which produces `null` of type `T?`.

<a id="sec-the-throw-expression"></a>
### 17.12 The `throw` expression

`throw` is an expression. Its operand is optional:

- `throw expr` raises the supplied exception. Valid in any expression position.
- `throw` (no operand) re-raises the currently-handled exception with its stack trace preserved. Valid only as an expression appearing inside a catch arm body ([§31.2.4](#sec-bare-re-raise)).

Both forms have the bottom type — assignable to any expected type — because control transfers to the matching handler before any value is produced.

```bezel
public property [i: int]: double => case i
  0 => X,
  1 => Y,
  2 => Z,
  _ => throw new IndexOutOfRangeException()
end;

var name := user.Name if user <> null else
            throw new InvalidOperationException("user is null");

var config := LoadConfig() ?? throw new InvalidOperationException("no config");

try
  DoWork();
catch
  var ex: TransientException => Retry(ex),
  var ex: Exception => throw          // bare throw: re-raises the current exception
end;
```

The statement form `throw;` (with the trailing semicolon, [§21.8](#sec-throw)) continues to work and is equivalent to the bare expression form followed by a semicolon. The two forms are interchangeable inside a catch arm body; the expression form is shorter and is preferred when the re-raise is the entire body of an arm.

Bare `throw` outside a catch arm body is a compile error, with the same diagnostic as the statement form `throw;` outside a catch context.

<a id="sec-the-discard-_"></a>
### 17.13 The discard `_`

The identifier `_` is a *discard* — a sink that consumes a value without binding it to a name. Discard is permitted in three positions; in every other position `_` is treated as a normal identifier (it may, for example, be used as a private field name).

**Pattern position.** Inside a `case` arm or destructuring pattern, `_` matches any value without binding.

```bezel
case shape
  Circle(_) => "circle",
  Rectangle(_, _) => "rectangle",
  _ => "other"
end;
```

**Out-argument position.** As a positional argument with the `out` modifier, `_` discards the out-parameter result.

```bezel
if dict.TryGetValue(key, out _) then    // we only care that it was present
  // ...
end;
```

`out _` does not introduce a variable; the receiving slot is internal to the compiler and is not observable. The argument is type-inferred from the parameter's declared type. A type annotation on `out _` is not permitted (`out _: int` is a syntax error), mirroring the rule for `out var name` ([§22.4](#sec-inline-out-declarations)) — the discard takes its type from the parameter without ambiguity.

**Discard statement.** The form `_ := expression;` evaluates `expression` and discards its value. The discard statement is syntactically distinct from assignment: `_` is not an lvalue, and the form is recognized lexically by the leading `_` token.

The discard statement's legality depends on the static type of `expression`:

- **Value types, `string`, primitives, and other runtime-managed types:** discard is always permitted. No disposal is required, so the discard is purely cosmetic — it suppresses the [§21.9](#sec-statement-form-of-expressions) *unused result* warning.

  ```bezel
  _ := ComputeChecksum();                // returns int; cosmetic discard
  _ := someStringBuilder.ToString();     // returns string; runtime-managed
  ```

  Inline arrays without owned slots (`[N]T`, `[N]T?`) are ordinary value types and follow this rule.

- **Move-only inline arrays:** `_ := expression;` where `expression` produces a move-only value (an owning inline array, `[N owned]T?` or `[N owned]T`, [§15.3](#sec-move-only-value-types)) is a compile error. The move-only value has owned slots whose disposal would run silently at the discard site; the rule parallels the non-trivial-dispose class case below. The remedies are to bind the value with `var name := expression;` and use it explicitly (auto-dispose runs at scope end), or to consume the value by an ownership-transferring operation.

- **Trivial-dispose class types ([§28.9](#sec-runtime-managed-types)):** discard is permitted. The class has a no-op `Dispose()` by definition, so abandoning the value cannot leak a destructor invocation. Memory is reclaimed by an implementation-defined mechanism (see [§28](#sec-resource-management)'s preamble).

  ```bezel
  _ := new Random();                     // Random is trivial-dispose
  _ := new StringBuilder();              // trivial-dispose
  ```

- **Non-trivial-dispose class types:** discard is a **compile error**. The error message identifies the destructor, owned field, or owning-inline-array field that makes the class non-trivial-dispose, and suggests the three legal alternatives:

  ```
  error: cannot discard a value of type FileStream because its destructor
         performs cleanup. Choose one:
           - using var _ := expression;        // dispose at scope end
           - var name := expression; ...; name.Dispose();
           - (if Task) Task.FireAndForget(expression);
  ```

- **Interface-typed expressions:** discard is a **compile error**, full stop. The rule is unconditional regardless of whether the interface extends `IDisposable` and regardless of whether any visible implementation is trivial-dispose. The reason: an interface reference may bind to any conforming implementation, and a future implementation added in any module may carry a non-trivial destructor. Permitting discard on interfaces that "happen to be trivial-dispose today" creates source-breaking changes when an implementation upgrade adds a destructor ([§28.9](#sec-runtime-managed-types)'s "Trivial-dispose classification is part of the API surface" subsection already addresses this for class types, but the interface case is worse — implementations live in other modules and may not be visible to the discard site at all). The cheap fix at the discard site is `using var _ := expr;`, which calls `Dispose()` at scope end whether or not the runtime type's `Dispose()` does any work.

- **`Task` and `Task<T>`:** discard is a compile error (Tasks have a destructor that handles continuation cleanup and exception propagation). Use `Task.FireAndForget(expression)` for supervised fire-and-forget, or `using var _ := expression;` to dispose at scope end. The error message names `Task.FireAndForget` specifically.

```bezel
_ := DoTelemetryAsync();                 // ERROR: Task is non-trivial-dispose
Task.FireAndForget(DoTelemetryAsync());  // OK — supervised

using var _ := new FileStream(path);     // OK — disposed at scope end
```

The discard form exists to acknowledge a deliberately-unused value, not to escape the disposal contract. The disposal contract is universal: a class instance with a non-trivial `Dispose()` reaches an owned location, becomes the operand of `using var`, or is explicitly disposed.

<a id="sec-recap-of-the-difference-between-discard-and-using-var-_"></a>
#### Recap of the difference between discard and `using var _`

```bezel
_ := expression;            // value evaluated and dropped on the floor;
                            //   only legal for non-disposing types.

using var _ := expression;  // value bound to an unnamed local that is
                            //   disposed at scope end; works for any
                            //   class type.
```

When the choice is between "I don't care about the result" and "I want the result disposed at scope end," choose `using var _ := …` for any value that needs disposal; choose `_ := …` only when the type is provably trivial-dispose or a value type and the goal is to silence the unused-result warning.

Note the distinction between discarding and binding:

```bezel
_ := ComputeResult();             // DISCARD: legal only when ComputeResult's return type
                                  //   is value-typed, runtime-managed, or trivial-dispose.
var _ := ComputeResult();         // BINDING: declares a local variable named "_" and assigns.
                                  //   The local is subject to normal unused-variable
                                  //   warnings AND the standard ownership rules — if the
                                  //   value needs disposal, this form does not satisfy that.
using var _ := ComputeResult();   // BINDING with disposal at scope end. Works for any
                                  //   class type, including non-trivial-dispose.
```

The forms are syntactically distinct. `_ :=` (no `var`) is the discard; `var _ :=` is a binding to a local whose name happens to be `_`; `using var _ :=` is the same binding with the using-var disposal rule. Outside the three discard positions (pattern, out-argument, discard statement), `_` is an ordinary identifier.

A worked example: `var _ := 5; Console.WriteLine(_);` declares a local named `_` with value 5 and prints it. The second `_` is the local — it appears in expression position, where `_` is just an identifier — not a discard. The discard is statement-level only (`_ := expr;`); it cannot be read.

---

<a id="sec-literals-and-strings"></a>
## 18. Literals and Strings

<a id="sec-numeric-literals"></a>
### 18.1 Numeric literals

Integer literals:
```bezel
42                  // int
0xFF                // hex
0b10101010          // binary
1_234_567           // digit separators allowed except at start/end
42L                 // long
42U                 // uint
42UL                // ulong
```

Floating-point and decimal literals:
```bezel
3.14                // double
3.14f               // float
-1.23e-4            // scientific notation, double
1.23456789m         // decimal
1.23e2m             // scientific notation, decimal
```

Suffixes are case-insensitive: `L`/`l`, `U`/`u`, `F`/`f`, `M`/`m`. The compiler does not silently promote integer literals; `3_000_000_000` is a compile error without an `L` suffix.

<a id="sec-boolean-and-null-literals"></a>
### 18.2 Boolean and null literals

`true`, `false`, `null`.

<a id="sec-character-literals"></a>
### 18.3 Character literals

```bezel
'a'         '\n'      '\t'      '\r'      '\\'      '\''
'\u0041'              // 4 hex digits, BMP
'\U0001F600'          // 8 hex digits, full code point range
'\x41'                // 1–4 hex digits
```

Escape sequences resolving to U+D800–U+DFFF are compile errors.

<a id="sec-string-literals"></a>
### 18.4 String literals

```bezel
"Hello, Bezel!"
"Line 1\nLine 2"      // escapes processed
```

<a id="sec-interpolated-strings"></a>
### 18.5 Interpolated strings

A `$` prefix introduces interpolation. Any expression may appear inside `{}`:

```bezel
var greeting := $"Hello, {name}!";
var msg := $"({point.X}, {point.Y}) - area = {point.Area()}";
```

An interpolation may include alignment and a format specifier:

```
{expression}
{expression,alignment}
{expression:format}
{expression,alignment:format}
```

- Alignment is a signed integer; positive right-aligns in a field of that width, negative left-aligns. Wider results overflow the field.
- Format is a type-specific string passed to the value's formatter.

```bezel
$"{amount:F2}"           // "1234.50"
$"{ratio:P1}"            // "85.0%"
$"{value:X}"             // "FF"
$"{date:yyyy-MM-dd}"
$"|{name,20}|"           // right-aligned in 20-character field
$"{amount,12:F2}"
```

The lexer tracks brace nesting inside interpolations. Nested string literals within `{...}` are recognized by their own opening `"` and parsed as complete strings; they do not terminate the outer string. `case ... end` and other complex expressions inside `{...}` are handled by full expression parsing.

<a id="sec-verbatim-strings"></a>
### 18.6 Verbatim strings

A `@` prefix suppresses escape processing. Verbatim strings may span multiple lines; line breaks become part of the string value.

```bezel
var path := @"C:\Users\Bezel\Program.bzl";
var sql := @"SELECT name, age
FROM users
WHERE active = 1";
```

Inside a verbatim string, `""` produces a single `"`.

<a id="sec-interpolated-verbatim-strings"></a>
### 18.7 Interpolated verbatim strings

`$@` combines both:

```bezel
var documentPath := $@"C:\Users\{user}\Documents\report.pdf";
```

<a id="sec-escape-sequences-non-verbatim"></a>
### 18.8 Escape sequences (non-verbatim)

| Escape | Meaning |
|---|---|
| `\n` | Newline (LF) |
| `\r` | Carriage return |
| `\t` | Horizontal tab |
| `\\` | Backslash |
| `\"` | Double quote |
| `\'` | Single quote |
| `\0` | Null character |
| `\a` | Alert (bell) |
| `\b` | Backspace |
| `\f` | Form feed |
| `\v` | Vertical tab |
| `\uXXXX` | Unicode scalar value, BMP only |
| `\UXXXXXXXX` | Unicode scalar value, full range |
| `\xH..H` | Hex escape, 1–4 digits, BMP only |

The `\x` escape greedily consumes the maximum number of valid hexadecimal digits, up to 4. In the string `"\x41BC"`, the escape is `\x41BC` (U+41BC), not `\x41` followed by `BC`. To produce the two-digit escape `\x41` followed by the literal characters `BC`, use concatenation (`"\x41" + "BC"`) or the `\u` form (`"\u0041BC"`).

Surrogate escapes (U+D800–U+DFFF) are compile errors.

---

<a id="sec-the-case-construct"></a>
## 19. The `case` Construct

`case` is multi-way pattern matching. It is a single construct that produces a value; in statement position, the value is discarded.

<a id="sec-forms"></a>
### 19.1 Forms

**Scrutinee form** — `case expression` followed by arms. The scrutinee is evaluated once and matched against each pattern in order.

```bezel
var grade := case score
  90..100 => "A",
  80..89 => "B",
  70..<80 => "C",
  _ => "F"
end;
```

**Scrutinee-less form** — `case` with no expression, followed by arms whose patterns are full boolean expressions. The first arm whose expression is `true` is selected.

```bezel
var grade := case
  score >= 90 => "A",
  score >= 80 => "B",
  score >= 70 => "C",
  _ => "F"
end;
```

<a id="sec-arms"></a>
### 19.2 Arms

Each arm has the form `pattern => body`. An arm body has three possible shapes:

- **An expression** (any Bezel expression — literal, method call, another `case`, etc.). This is the only form valid in expression-position `case` (where the whole `case` produces a value).
- **A single statement** (e.g. `=> _x := y;`, `=> return;`, `=> throw new ArgumentException("nope");`). Valid in *statement-position* `case` only. The statement's terminating `;` ends the arm body, and the arm separator `,` follows. This is the convenient form for the common one-line arm body that is a statement rather than an expression.
- **A `do … end` block** of statements. Has type `()` and produces no value, so it is valid in statement-position `case` only. Use this when the arm body needs multiple statements.

Multi-statement logic in *expression-position* arms is extracted into a helper method or local lambda — both produce values, and the extraction makes the arm's intent visible at its declaration site:

```bezel
var description := case score
  > 90 => "excellent",
  > 70 => "good",
  > 50 => "passing",
  _    => Classify(score)            // helper for the multi-step case
end;

private static method Classify(score: int): string
  Log("low score: " + score.ToString());
  return if score < 0 then "invalid" else "failing";
end Classify;
```

Calls to a helper are zero-overhead in the common case (the compiler is permitted to inline them) and keep the arm body to a single expression. The extraction also gives the per-arm logic a name a reader can use, and localizes any side effects the arm performs — both of which are valuable independent of the syntactic question. Rationale [§5.13](Bezel_Design_Rationale.md#sec-no-block-expressions) discusses why Bezel does not provide a value-returning `do ... end` form.

`return` inside an arm exits the enclosing callable, not the arm. `case` cannot be exited "early" via `return`; restructure into helper methods.

Arms are comma-separated. A trailing comma after the last arm is optional.

<a id="sec-patterns"></a>
### 19.3 Patterns

**Constants:** `1`, `42`, `"hello"`, `Priority.High`.

**Comma-separated alternatives:** `2, 3, 4, 5` matches any of these. The arm separator and the pattern-alternative separator are both `,`; the parser disambiguates by the `=>` token, which always follows the complete pattern. In `1, 2 => "low", 3 => "high"`, the first arm matches 1 or 2; the second arm matches 3.

**Inclusive and exclusive ranges:** `90..100`, `0..<60`. Open-ended forms allowed: `..<0` (less than 0), `31..` (at least 31).

**Relational:** `> 70`, `< 50`, `<= 100`, `>= 0`. Compares the scrutinee against the right-hand operand.

**Type patterns without binding:** `string`, `int`, `Dog`. Subtypes match: a `Dog` value matches an `Animal` pattern.

**Type patterns with binding:** `var n: int`, `var d: Dog`. The bound name is in scope only within that arm's body.

**Record destructuring:** `Point(x, y)`, `MyRecord(a, b, c)`. The type name followed by parenthesized bindings matches the type and binds positional fields by position. The number of bindings must match the record's positional-field count. Individual positions may be discarded with `_`: `Point(_, y)`. Destructuring patterns compose with `when` guards and with subtype matching.

Destructuring applies to records only — the positional binding is over a record's positional fields ([§9.1](#sec-positional-records)). Classes do not have a positional-destructuring surface, even when they declare a primary constructor ([§22.10](#sec-methods-and-constructors-primary-constructors)): primary-constructor parameters are not auto-promoted to public properties, and the positional shape is not part of the class's pattern-match surface. To match a class hierarchy, use a type-test pattern (`Dog`, `var d: Dog`) and access fields through the bound name. The [§19.6](#sec-exhaustiveness) exhaustive-match example over a `sealed abstract` class hierarchy uses this form.

Inside a positional destructuring pattern, a naked identifier is interpreted as a binding for that position — `Point(x, y)` binds `x` and `y` (it does not type-test the positions against types named `x` and `y`). The naked-identifier form is the common shorthand; the explicit `Point(var x, var y)` is also accepted and means the same thing. The naked-identifier form is *not* available outside positional destructuring: at the top level of a pattern, an identifier alone is parsed as a type test, so a binding requires the explicit `var` form (`var x` or `var x: T`). Nested patterns inside a destructuring (e.g. `Outer(Inner(x))`) follow the same rule recursively.

**Var patterns:** `var t`. Matches any value, binds it. Almost always combined with a `when` guard.

**Discard `_`:** Matches any value, binds nothing. When used as a top-level pattern (a catch-all arm), it must be the last arm (see [§19.6](#sec-exhaustiveness)); inside destructuring patterns (`Point(_, y)`), `_` may appear in any position with no last-arm restriction.

**Pattern binding and ownership.** A pattern binding (`var d: Dog`, the naked identifier `x` in `Point(x, y)`, or the unconstrained `var t`) is **non-destructive on the scrutinee**: the scrutinee retains whatever ownership state it had, and the bound name is a borrow whose source is the scrutinee's source. This matches the `as` operator's behavior ([§17.4](#sec-the-is-is-not-and-as-operators)). When the scrutinee is an owned local of class type and the user wants to transfer ownership through a pattern match, the user moves from the original scrutinee variable after the case construct selects an arm — the case construct itself never moves from the scrutinee. For value-type scrutinees, the binding is a copy by the standard value-type rules; for runtime-managed scrutinees (`string`, function values), the binding is a shared reference. The narrowing produced by the case arm is purely informational about the type tag; it does not change the ownership state of the scrutinee.

<a id="sec-guards"></a>
### 19.4 Guards

A `when` clause adds a runtime guard:

```bezel
case temperature
  var t when t > 30 => Console.WriteLine($"Hot: {t}"),
  var t when t < 0 => Console.WriteLine($"Freezing: {t}"),
  var t => Console.WriteLine($"Mild: {t}")
end;
```

Arms are evaluated in textual order. The first arm whose pattern matches *and* whose guard is true is selected.

<a id="sec-type-pattern-matching-never-matches-null"></a>
### 19.5 Type-pattern matching never matches `null`

A bare type pattern `T` matches only non-null values. A typed binding pattern `var n: T` also matches only non-null values — the type annotation makes it a type-constrained binding, not an unconditional binding. `null` is matched only by the explicit `null` pattern, by untyped `var x` patterns (which bind any value including null), or by `_`.

<a id="sec-exhaustiveness"></a>
### 19.6 Exhaustiveness

If `_` appears, it must be the last arm; placing it earlier is a compile error.

A `case` with no `_` arm is permitted. In statement position, an unmatched value falls through with no action. In expression position, an unmatched value raises `MatchFailureException` at runtime; the compiler warns when it cannot prove exhaustiveness statically.

The compiler proves exhaustiveness statically in the following cases:

- **Enum scrutinee:** every declared constant has a matching pattern (literal pattern, `var` binding, or `_`).
- **Bool scrutinee:** both `true` and `false` are covered.
- **Sealed class hierarchy scrutinee:** when the scrutinee's static type is a `sealed abstract` class, or an `abstract` class whose only direct subclasses are themselves `sealed` or recursively form a closed set, and every leaf type in the closed hierarchy is matched by a type pattern (with or without binding), no `_` arm is required. The leaves may be matched in any order; the analysis is purely on type coverage. Class leaves are matched by type-test patterns (`var c: Circle => ...`); positional destructuring ([§19.3](#sec-patterns)) applies to record leaves, not to class leaves. A `sealed abstract` class ([§11.7](#sec-static-classes)) provides the strongest guarantee: no subclass outside the declaring module can exist, so exhaustiveness holds across module boundaries.
- **Nullable scrutinee:** under the rules above plus an explicit `null` pattern.

```bezel
public sealed abstract class Shape ... end Shape;
public sealed class Circle: Shape ... end Circle;
public sealed class Square: Shape ... end Square;
public sealed class Triangle: Shape ... end Triangle;

var area := case shape
  var c: Circle => Math.Pi * c.Radius * c.Radius,
  var s: Square => s.Side * s.Side,
  var t: Triangle => 0.5 * t.Base * t.Height
end;                       // exhaustive: every leaf covered, no _ needed
```

When the hierarchy is open (the base is not sealed, or has subclasses that are themselves not sealed), exhaustiveness cannot be proven statically and the warning is emitted unless the user supplies `_`.

<a id="sec-exhaustiveness-across-module-boundaries"></a>
#### Exhaustiveness across module boundaries

Exhaustiveness is established at compile time against the visible set of leaf types. When a `case` matches over a sealed hierarchy declared in another module, the closed set is computed from that module's source as it appeared at compile time. If a downstream version of the declaring module adds a new sealed leaf, existing exhaustive matches in dependent modules become non-exhaustive — recompiling against the new version will emit the missing-arm warning, and at runtime an unmatched value raises `MatchFailureException`.

The recommended discipline is treating any addition of a leaf type to a sealed hierarchy as a source-breaking change for consumers. Alternatively, a producer who anticipates extension points can leave the base class non-sealed; consumers will then be required to supply a `_` arm, which handles future leaves automatically.

<a id="sec-termination"></a>
### 19.7 Termination

A `case` ends with `end`. The trailing `;` belongs to the enclosing statement, not the case:

```bezel
var grade := case score
  90..100 => "A",
  _ => "F"
end;                   // ; ends the assignment

case month
  1 => Console.WriteLine("Jan"),
  _ => Console.WriteLine("Other")
end;                   // ; ends the statement
```

Because `case ... end` is self-delimited, it composes inside any expression position without parenthesization, including inside string interpolations and as another arm's body.

<a id="sec-inside-enum-scrutinees"></a>
### 19.8 Inside enum scrutinees

When the scrutinee has an enum type, the enum prefix may be omitted in patterns. See [§10.6](#sec-case-matching) for details and examples.

---

<a id="sec-the-if-expression"></a>
## 20. The `if` Expression

The `if` expression selects between two values:

```
trueValue if condition else falseValue
```

Both branches must have a common type.

```bezel
var max := a if a > b else b;
var status := "adult" if age >= 18 else "minor";
```

The form composes recursively, right-associatively:

```bezel
var grade := "A" if score >= 90 else
             "B" if score >= 80 else
             "C" if score >= 70 else
             "F";
```

For more than two or three branches, `case` is preferred.

Precedence is level 15 ([§17.1](#sec-precedence)): `a ?? b if c else d` parses as `(a ?? b) if c else d`. When an `if` expression appears inside another expression with potentially-confusing grouping, parenthesize.

---

<a id="sec-statements"></a>
## 21. Statements

Every statement terminates with `;`. Control structures terminate with `end;`.

<a id="sec-variable-declarations"></a>
### 21.1 Variable declarations

```bezel
var balance: decimal;                 // explicit type, no initializer
var count: int := 0;                  // explicit type with initializer
var greeting := "Hello";              // type inferred
const Pi: double := 3.14159;          // const with explicit type
const MaxUsers := 1000;               // const with inferred type
```

A non-nullable local must be assigned before it is read ([§7.9](#sec-definite-assignment-for-non-nullable-fields)).

<a id="sec-the-if-statement"></a>
### 21.2 The `if` statement

```bezel
if amount > 0 then
  Balance := Balance + amount;
end;

if score >= 90 then
  grade := "A";
elif score >= 80 then
  grade := "B";
elif score >= 70 then
  grade := "C";
else
  grade := "F";
end;
```

The condition must be `bool`.

<a id="sec-the-while-statement"></a>
### 21.3 The `while` statement

```bezel
while i < 10 do
  Console.WriteLine(i);
  i := i + 1;
end;
```

<a id="sec-the-for-statement"></a>
### 21.4 The `for` statement

`for` iterates any iterable source. This includes values whose type implements `IEnumerable<T>` ([§29](#sec-iteration-2)), inline arrays `[N]T` (which do not implement `IEnumerable<T>` but support direct indexed iteration through a compiler intrinsic, [§15.1.2](#sec-inline-array-members)), and `Range<T>` values (which the compiler may optimize into counted loops):

```bezel
for var i in 1..10 do Console.WriteLine(i); end;
for var i in 0..<n step 2 do Process(i); end;
for var t in transactions do Process(t); end;

for var entry in dict do
  Console.WriteLine($"{entry.Key} -> {entry.Value}");
end;
```

`for` always declares its loop variable with `var`. An optional type annotation must match or be implicitly convertible from the source's element type:

```bezel
for var x: int in source do ... end;
```

The loop variable is freshly bound on each iteration: a lambda captured inside the body sees the value of that specific iteration.

If the source is empty, the body does not execute.

For async iteration, see `await for` ([§30.7](#sec-asynchronous-programming-async-iterators)).

<a id="sec-break-and-continue"></a>
### 21.5 `break` and `continue`

`break` exits the immediately enclosing loop. `continue` skips the rest of the current iteration and advances to the next. Neither is legal outside a loop; doing so is a compile error.

There are no labeled loops and no labeled `break`/`continue`. For multi-level exits, extract into a method and use `return`, or use a boolean flag.

<a id="sec-do-end-blocks"></a>
### 21.6 `do ... end` blocks

A `do ... end` block groups statements as a single statement and limits the lifetime of locals. Two purposes:

1. **Scoping.** Locals declared in a block are visible only until the matching `end`. `using var` resources ([§28.10](#sec-using-var)) are disposed when the block ends.
2. **Multi-statement bodies.** Anywhere a single statement is expected, `do ... end` containing several statements stands in.

```bezel
public method ProcessLargeFile(filename: string)
  var result := 0;

  do
    using var fs := new FileStream(filename);
    var data := fs.ReadAll();
    result := ProcessData(data);
  end;
  // fs has been disposed here

  Console.WriteLine($"Result = {result}");
end ProcessLargeFile;
```

When a `do ... end` block ends:
1. `using var` declarations within the block are disposed in reverse declaration order.
2. Local variables declared within the block go out of scope.

Loops use `do` as part of their syntax; `for ... do ... end` and `while ... do ... end` are not separate `do ... end` blocks.

<a id="sec-the-return-statement"></a>
### 21.7 The `return` statement

```bezel
return;                   // exit with no value
return expression;        // exit with a value
```

`return` exits the nearest enclosing **callable** — method, constructor, destructor, lambda, property accessor, or indexer accessor. It is unaffected by `do ... end`, loops, `case` arms, and `try`/`catch`/`finally`.

Form rules:

- A method or accessor with a return type must `return expression;` on every exit path.
- A method or accessor with no return type uses `return;` to exit early; falling off the end is implicit.
- Constructors and destructors use `return;` to exit early. `return expression;` is a compile error.
- Lambdas follow the same rule as methods.

`return` inside a `finally` block is a compile error.

`return` inside a lambda exits the lambda, not the method that defined it.

<a id="sec-throw"></a>
### 21.8 `throw`

`throw new SomeException(...)` raises an exception. `throw;` (inside a catch arm's body) re-raises the current exception as a statement. The expression form `throw` (without the trailing semicolon, [§17.12](#sec-the-throw-expression)) is equivalent and may appear anywhere an expression is expected within a catch arm body. See [§31](#sec-exception-handling).

<a id="sec-statement-form-of-expressions"></a>
### 21.9 Statement form of expressions

A method call, an `await` expression, a `case` in statement position, or any other expression whose result is `()` may appear as a statement on its own line, terminated with `;`.

An expression statement whose result is a non-`()` type is permitted but the compiler emits a warning: the result is silently discarded, which may indicate a bug (a return value the programmer forgot to use). To suppress the warning, use the discard form `_ := expression;` ([§17.13](#sec-the-discard-_)) — but only when the result type allows it. For non-trivial-dispose class types and for `Task`, the warning is replaced by a hard error directing the caller to `using var _ := expression;`, an explicit `.Dispose()`, or `Task.FireAndForget` as appropriate.

Method calls returning `()` and `await` expressions on `Task` (no result) are the common cases; no warning is emitted for these.

---

<a id="sec-methods-and-constructors"></a>
## 22. Methods and Constructors

<a id="sec-method-declarations"></a>
### 22.1 Method declarations

```bezel
public method Add(a: int, b: int): int
  return a + b;
end Add;

public method PrintGreeting(name: string)
  Console.WriteLine($"Hello, {name}!");
end PrintGreeting;
```

A method has a return type after `:`, or no `:` clause for a procedure (return type `()`).

<a id="sec-expression-bodied-methods"></a>
### 22.2 Expression-bodied methods

When the body is a single expression, `=> expression` replaces the block:

```bezel
public method Area(): decimal => Width * Height;
public method Max(a: int, b: int): int => a if a > b else b;
```

<a id="sec-parameter-modifiers"></a>
### 22.3 Parameter modifiers

- `ref` — pass by reference. The caller writes `ref` at the call site. The variable must be initialized before the call.
- `out` — output parameter. The caller writes `out`. The variable need not be initialized before the call. The callee must assign every `out` parameter on every exit path; failing to do so is a compile error.
- `owned` — ownership transfer. See [§28.8](#sec-ownership-transfer).
- `out owned` — output parameter that produces an owned value. The callee assigns an owned value through the parameter; the caller receives ownership and must discharge it (typically with `out using var` at the call site, [§22.4](#sec-inline-out-declarations)). The combination `out owned` is the only modifier compound; the others are mutually exclusive.

<a id="sec-inline-out-declarations"></a>
### 22.4 Inline `out` declarations

```bezel
var success := parser.TryParse("42", out var num);
```

`out var name` declares a new variable inline. Its type is inferred from the method's `out` parameter declaration; an explicit type annotation is not permitted in the inline form.

The variable's scope is the enclosing block, from the call onward. It is in scope in both branches of any surrounding `if` and after the call.

For disposal-aware inline declarations, use `out using var`:

```bezel
factory.TryCreate(config, out using var conn);
// conn disposed at end of enclosing block
```

When a method parameter is declared `out owned`, the callee produces an owned value through the output parameter, and the caller's call-site syntax must reflect this. The accepted forms at the call site of an `out owned` parameter are `out using var x` (a scope-bound owned local) and `out owned var x` (an owned local with the standard owned-local discharge rules applying); both produce an owned location for the callee to write through. Plain `out var x` is **not** accepted at the call site of an `out owned` parameter — the resulting value's ownership would have nowhere to go. The full call-site / parameter-modifier compatibility rules are given in [§22.6](#sec-overloading).

<a id="sec-optional-parameters-and-named-arguments"></a>
### 22.5 Optional parameters and named arguments

```bezel
public method Log(message: string, level: string := "INFO", timestamp: bool := true)
  // ...
end Log;

Log("System started", timestamp := false);
Log(level := "ERROR", message := "Failed");
```

Named arguments may appear in any order after positional arguments.

<a id="sec-evaluation-timing"></a>
#### Evaluation timing

A default-value expression is evaluated **at each call site** where the default is used — not at the method's declaration site. The expression is re-evaluated every time a caller omits the parameter; the syntax `level: string := "INFO"` does not bind a single shared `"INFO"` instance to the parameter once and reuse it across calls.

This rule lets default-value expressions be non-constant:

```bezel
public method OpenAsync(path: string, timeout: TimeSpan := TimeSpan.FromSeconds(30))
public method LogAt(message: string, when: DateTime := DateTime.Now)
public method NewBuffer(size: int := 4096, fill: byte := 0)
```

`TimeSpan.FromSeconds(30)` and `DateTime.Now` evaluate per call. The first two examples produce a fresh value each time the default is taken; the third uses an integer literal whose evaluation is observably the same per call. There is no Python-style "default value is constructed once at definition and shared across calls" footgun: each call that omits a parameter materializes the default expression's value fresh into the call's frame.

For default values whose expression has owned-allocation semantics (constructing a new class instance, returning an `owned` from a factory), the per-call evaluation produces a fresh owned instance for that call's frame; the parameter receives ownership through the standard parameter-passing rules ([§22.3](#sec-parameter-modifiers), [§28.8](#sec-ownership-transfer)). A default-value expression that produces an owned class instance must therefore be valid in `owned` parameter position — the most natural pattern is a factory method returning `owned T`.

<a id="sec-permitted-default-value-expressions"></a>
#### Permitted default-value expressions

A default-value expression must be type-compatible with the parameter's declared type and must be free of side-effects on captured locals (the expression is a function-of-call-site, not a function-of-the-frame-containing-the-call). Specifically, a default-value expression may:

- be a literal, constant, or `default(T)`;
- call static methods, including factory methods that return owned instances;
- read other static state (with the usual static-initialization rules);
- read other parameters appearing **earlier** in the same parameter list (e.g. `M(a: int, b: int = a + 1)` is permitted; later-parameter forward references are not).

A default-value expression may not:

- reference instance state (`this`, instance fields, instance properties) — the default belongs to the method's signature, not to a particular receiver's frame;
- reference local variables (the call site's frame is not in scope);
- contain `await` (defaults are not async expressions; an async default would suspend an in-progress call before the body started, which has no clear semantics).

<a id="sec-comparison-with-c#"></a>
#### Comparison with C#

C# requires default-value expressions to be compile-time constants (`int x = 42` is fine; `DateTime t = DateTime.Now` is a compile error). Bezel's call-site evaluation is strictly more expressive: anywhere C#'s rule allows a default, Bezel does too, and additional cases (factory calls, `Now`-style defaults) become available without resorting to `null`-then-coalesce patterns.

The C# choice was driven by ABI stability: the default value is encoded into the method's metadata at the *call site* (the compiler inlines the constant into each caller), so changing the default in the callee doesn't update existing callers without recompilation. Bezel does not make this metadata-encoding promise — defaults are part of the called method's signature, evaluated by the callee or by code generated at the call site against the callee's signature, depending on implementation. Recompilation against a new version of a library may pick up a changed default; this is expected and matches the rest of Bezel's source-compatibility model.

<a id="sec-overloading"></a>
### 22.6 Overloading

Methods and constructors may be overloaded by parameter list. Overload resolution considers number, types, and the `ref`/`out` modifiers — overloads differing in any of these are distinguishable.

<a id="sec-owned-and-overload-resolution"></a>
#### `owned` and overload resolution

`owned` on an **ordinary parameter** (without `out`) does *not* participate in overload resolution. Two overloads that differ only by `owned` on a parameter — e.g. `M(t: T)` and `M(owned t: T)` — are a compile error. The reason is that ordinary `owned` describes a contract about the argument's lifetime rather than its type; the call-site syntax (`M(value)`) does not visibly distinguish the two forms, so permitting them as overloads would force the caller to know the resolution outcome to write a correct ownership transfer. APIs that need both ownership-transferring and borrowing forms must use distinct method names — `Append` versus `AppendOwned`, or the `FromResult` / `FromOwnedResult` pattern.

`owned` on an **`out` parameter** is different: the call-site syntax visibly distinguishes the two cases. `out var x` produces a borrow location; `out using var x` (or the inline `out owned var x`, [§22.4](#sec-inline-out-declarations)) produces an owned location. The static distinction is observable at the call site, so resolution can proceed without ambiguity.

Therefore: **`out T` and `out owned T` are distinct overloads.** Two methods differing only in the `owned` qualifier on an `out` parameter are permitted, and overload resolution selects between them based on the call-site form:

- `M(out var x)` and `M(out var x: T)` resolve to the `out T` overload.
- `M(out using var x)`, `M(out using var x: T)`, and `M(out owned var x)` resolve to the `out owned T` overload.

If only one overload exists, the call-site syntax must match it: a caller cannot write `out using var` to call a method whose `out` parameter is plain (the result wouldn't have an owned source), and a caller cannot write plain `out var` to call a method whose `out` parameter is `out owned` (the resulting value's ownership would have nowhere to go).

This carve-out exists for `out` specifically. `ref` parameters do not admit `owned` (a `ref` location is borrowed by definition); `in` parameters do not exist as a separate modifier in Bezel. The carve-out applies only where the call-site syntax visibly distinguishes the two forms.

<a id="sec-practical-pattern"></a>
#### Practical pattern

The common use is a "try" method that exposes both shapes:

```bezel
public class ConnectionPool
  // Try to fetch a cached connection; caller borrows from the pool.
  public method TryGetCached(name: string, out conn: Connection?): bool;

  // Try to fetch and remove from the pool; caller takes ownership.
  public method TryGetCached(name: string, out owned conn: Connection?): bool;
end ConnectionPool;

// Caller A: borrows a cached connection (pool retains it)
if pool.TryGetCached("primary", out var c) then
  UseConnection(c!);    // c is Connection?; the bool guard tells us non-null
end;

// Caller B: takes ownership of a cached connection (pool no longer holds it)
if pool.TryGetCached("primary", out using var c) then
  UseConnection(c!);    // c is owned Connection?; disposed at scope end
end;
```

Both call sites are unambiguous; the resolution is by call-site syntax.

The implementation of the take-form (the `out owned` overload) uses [§28.8.1](#sec-the-move-expression)'s `move` expression on the pool's internal owned-element collection to transfer ownership from pool to caller. A typical body using a `OwnedDictionary<string, Connection>` for the pool storage has the shape:

```bezel
private owned _cache: OwnedDictionary<string, Connection>;

public method TryGetCached(name: string, out owned conn: Connection?): bool
  if not _cache.ContainsKey(name) then
    conn := null;
    return false;
  end;
  conn := _cache.PopValue(name);    // takes ownership, removes from dictionary
  return true;
end TryGetCached;
```

The out parameter is typed `Connection?` (nullable) so that the false-return path can write `null` — `default(Connection)` is a compile error for a non-nullable reference type ([§17.11](#sec-the-default-operator)), and the type system requires every `out` parameter to be assigned on every return path. Callers consume with `out using var` and null-check after the bool guard:

```bezel
if pool.TryGetCached("primary", out using var c) then
  // c is owned Connection? here — but the bool guard tells us it's non-null
  UseConnection(c!);    // ! is fine; the API contract guarantees non-null on true return
end;
```

The `!` is the documentation point: the API contract is "true return implies c is non-null," and the caller asserts that explicitly. A future revision that adds *flow-sensitive nullability narrowing across `out` parameters* would let the caller write `UseConnection(c)` without the `!`, but in this version the explicit `!` is required.

The `PopValue` method on `OwnedDictionary<K, V>` ([§28.11](#sec-collections-do-not-own-elements), std lib [§4.9](Bezel_Standard_Library.md#sec-owneddictionaryk-v)) removes the entry and returns ownership of the value. After the call, the pool no longer holds the connection; the `out owned` parameter `conn` carries the obligation to the caller.

For pool implementations that prefer index-based storage, `OwnedArray<Connection>.PopAt(i)` provides the analogous take-and-remove operation. For the simplest case of a single-slot cache, an `owned Connection?` field with `move _slot!` followed by `_slot := null` (per [§28.8.1](#sec-the-move-expression)'s nullable-field detach pattern) is also valid — the choice between dictionary, array, and single-field forms is about how the pool indexes its connections, not about how ownership flows out.

<a id="sec-implicit-conversion-ranking"></a>
#### Implicit conversions and tie-breaking

The rules above (number, type, `ref`/`out`, and the `owned`-on-`out` carve-out) determine which overloads are **applicable** to a call. When more than one overload remains applicable because an argument matches several candidates through different implicit conversions ([§5.1](#sec-implicit-conversions)), the compiler selects among them by the following precedence. This tie-breaking is applied only after arity and named-argument binding ([§22.5](#sec-optional-parameters-and-named-arguments)) have already filtered the set to the applicable candidates.

For a single argument position, one candidate is **better** than another when the better candidate requires *no* implicit conversion (the argument's static type matches the parameter type exactly) and the other requires an implicit conversion. An exact match is the only relation that distinguishes one applicable candidate from another at an argument position; two candidates that both require an implicit conversion at the same position are **equally ranked** there, regardless of which conversion each uses. Bezel does not rank one built-in implicit conversion above another: §5.1 admits a conversion only when no programmer-visible information is lost (or, for the wide-integer-to-float cases, when an explicit cast would be unjustified noise), so all admitted implicit conversions are treated as co-equal for resolution purposes.

A candidate is **unambiguously better** than another when it is better-or-equal on every argument position and strictly better (an exact match where the other converts) on at least one. The call resolves to a candidate that is unambiguously better than every other applicable candidate.

If no candidate is unambiguously better — because each of two candidates is exact on a different argument, or because the remaining candidates all rely on implicit conversions that this rule treats as equally ranked — the call is **ambiguous** and is a compile error. The compiler reports the error and cites the candidate overloads; it never silently selects one. The caller resolves the ambiguity by adding an explicit cast ([§5.2](#sec-explicit-conversions)) to an argument so that exactly one candidate becomes an exact match (or so that all but one candidate become inapplicable).

If no candidate is applicable at all — every candidate would require a conversion that §5.1 does not admit as implicit — the call is a compile error with no candidates cited, because no overload was reachable.

This mirrors the resolution of overloaded method references ([§27.6](#sec-method-references-to-overloaded-methods)), where the absence of a unique winner is likewise a compile error citing the candidates, and it follows the same "explicit over implicit" stance that motivates Bezel's omission of user-defined conversion operators ([§25.4](#sec-no-user-defined-conversion-operators)): when two overloads serve a call equally well, the compiler declines to guess and requires the call site to be explicit.

**Example 1 — exact match preferred over a conversion.**

```bezel
public class Logger
  public method Log(level: int);
  public method Log(level: long);
end Logger;

var lg := new Logger();
var n: int := 10;

lg.Log(n);            // n is int: exact match — calls Log(int), not the int→long widening
lg.Log(42);           // 42 is an int literal: exact match — calls Log(int)
lg.Log(42L);          // 42L is a long literal: exact match — calls Log(long)
```

**Example 2 — two equally-ranked conversions: ambiguous.**

```bezel
public class Converter
  public method Convert(n: long): string => "long";
  public method Convert(n: float): string => "float";
end Converter;

var cvt := new Converter();
var a: int := 10;

cvt.Convert(a);
// a is int. Neither candidate is an exact match.
//   Convert(long):  int→long  is implicit (§5.1).
//   Convert(float): int→float is implicit (§5.1).
// Both conversions are admitted by §5.1 and are equally ranked: neither
// candidate is better at the single argument position.
// Ambiguous call — compile error citing both candidates.
// Disambiguate with an explicit cast so exactly one is exact:
//   cvt.Convert(long(a));     // selects Convert(long)
//   cvt.Convert(float(a));    // selects Convert(float)
```

**Example 3 — exact on one argument, conversion on the other: unambiguously better.**

```bezel
public class Pair
  public method Apply(x: int, y: long): string => "int,long";
  public method Apply(x: int, y: int): string  => "int,int";
end Pair;

var p := new Pair();
var a: int := 5;
var b: int := 9;

p.Apply(a, b);
// arg x (int): exact in both — tie.
// arg y (int): exact in Apply(int,int); requires int→long in Apply(int,long).
// Apply(int,int) is better-or-equal on x and strictly better on y ⇒ unambiguously better.
// Resolves to Apply(int, int).
```

**Example 4 — each candidate exact on a different argument: ambiguous.**

```bezel
public class Calculator
  public method Compute(a: int, b: long): string => "int,long";
  public method Compute(a: long, b: int): string => "long,int";
end Calculator;

var calc := new Calculator();
var x: int := 5;
var y: int := 10;

calc.Compute(x, y);
// arg a (int): exact in Compute(int,long); int→long conversion in Compute(long,int).
// arg b (int): int→long conversion in Compute(int,long); exact in Compute(long,int).
// Each candidate is strictly better on a different argument; neither dominates.
// Ambiguous call — compile error citing both candidates.
// Disambiguate:
//   calc.Compute(x, long(y));    // makes Compute(int,long) exact on both ⇒ selects it
//   calc.Compute(long(x), y);    // makes Compute(long,int) exact on both ⇒ selects it
```

**Example 5 — no reachable candidate: error with no candidates cited.**

```bezel
public class Transformer
  public method Transform(x: int);
  public method Transform(x: decimal);
end Transformer;

var t := new Transformer();
var n: float := 2.5f;

t.Transform(n);
// float→int is not implicit (§5.1: float's only implicit target is double).
// float→decimal is not implicit (§5.1: decimal and binary floats never mix implicitly).
// No candidate is applicable ⇒ compile error, no candidates cited (none reachable).
// The caller must convert explicitly, e.g. t.Transform(int(n)) or t.Transform(decimal(n)).
```

**Example 6 — applicability filtering removes a candidate, leaving one winner.**

```bezel
public class Handler
  public method Process(n: sbyte): string => "sbyte";
  public method Process(n: ushort): string => "ushort";
end Handler;

var h := new Handler();
var val: byte := 50;

h.Process(val);
// byte→sbyte is NOT implicit (§5.1: signed/unsigned same-or-narrower width never implicit).
//   ⇒ Process(sbyte) is not applicable.
// byte→ushort IS implicit (§5.1, byte row).
//   ⇒ Process(ushort) is the only applicable candidate.
// Resolves to Process(ushort). No ambiguity — only one candidate survived applicability.
```

The rule applies identically to constructor overload sets ([§22.7](#sec-constructors)): a `new T(...)` call is resolved among `T`'s constructors by the same exact-match-then-ambiguity precedence.

<a id="sec-constructors"></a>
### 22.7 Constructors

```bezel
public constructor(name: string, balance: decimal)
  _name := name;
  _balance := balance;
end constructor;
```

Constructors return no value. Multiple constructors are permitted; chaining uses `this(...)` or `base(...)` in the chaining position before the body ([§12.8](#sec-constructor-chaining)). When a class has a primary constructor ([§22.10](#sec-methods-and-constructors-primary-constructors)), an additional restriction applies: only the primary may chain to `base(...)`; convenience constructors must chain via `this(...)`.

A static constructor runs once before the type's first use. It carries no access modifier (the runtime is the only caller) and no parameters:

```bezel
class Utils
  static constructor()
    // ...
  end constructor;
end Utils;
```

A static constructor body may use `try`/`catch`. An exception caught inside the body does not transition the type to the failed state — the constructor completes normally and static initialization is considered successful. The failed state is entered only when an exception escapes the static initializer entirely. If the static initializer (the static field initializers, the static constructor body, or both) throws an *uncaught* exception, the type is left in a permanently failed state:

- Every subsequent access to a static member of the type — field read, field write, method call, property read or write, indexer, event subscription — raises `TypeInitializationException` whose `InnerException` is the original exception.
- The state is not recoverable: re-running static initialization is not attempted.
- Static fields whose initializers had not yet run remain at their default values, but those defaults are not observable: every access raises. For non-nullable reference fields, where no default value exists ([§7.9](#sec-definite-assignment-for-non-nullable-fields)), this rule is satisfied vacuously — every access to the type raises before any field read could occur, so the absence of a value is never observed. The compiler's definite-assignment analysis is not violated, because the failed-state machinery intercepts every access before any read of the field.
- Static fields whose initializers completed successfully before the failing initializer have their values; if any such field is `owned`, its value will be disposed at program exit per [§22.13](#sec-static-initialization-order). The runtime makes no guarantee about ordering of disposal across types when one of the types is in a failed state.

The exception thrown on first failure is the one cached and rethrown; multiple subsequent accesses see the same cached `TypeInitializationException` instance.

The rules above resolve into two scenarios:

| Scenario | Outcome |
|---|---|
| Static initializer (field initializer or static constructor body) throws an exception that escapes the initializer. | Type enters permanently failed state. Every subsequent access raises `TypeInitializationException` whose `InnerException` is the original. Owned static fields whose initializers had already completed are disposed at program exit. |
| Static initializer encounters an exception but catches it via `try`/`catch` inside the body (or the throwing field initializer is preceded by a successful one). | Type initializes normally. The caught exception did not escape; subsequent member access proceeds without raising. Remaining initializers and the constructor body run as usual. |

<a id="sec-field-initializers"></a>
### 22.8 Field initializers

A field declared in a class or record may include an initializer:

```bezel
class BankAccount
  private _balance: decimal := 0m;
  private owned _transactions: Array<Transaction> := new Array<Transaction>();
  private _createdAt: DateTime := DateTime.Now;
end BankAccount;
```

A field declared `readonly` may be assigned only in a field initializer or in a constructor body; assignment anywhere else is a compile error. `readonly` on a field is analogous to `readonly` on an auto-property ([§23.1](#sec-auto-properties)).

Field initializers run as part of construction in textual declaration order. The full sequence for a constructor:

0. Primary-constructor parameters ([§22.10](#sec-methods-and-constructors-primary-constructors)) that are referenced in body positions (method, property, constructor, or destructor bodies) are captured into their compiler-generated private fields; parameters referenced only in initializer positions are not captured ([§11.1](#sec-class-declaration)). This step is omitted for classes without a primary constructor.
1. The base or chained constructor call (`base(...)`, `this(...)`, or implicit `base()`). For a `this(...)` chain, the chained constructor runs to completion (its own steps 0 through 3) before control returns here.
2. Field initializers, in declaration order.
3. The constructor body.

Primary-constructor parameter capture occurs first so that the `base(...)`/`this(...)` argument expressions (step 1), field initializers (step 2), and the body (step 3) may reference the captured parameters by name. Step 0 is observable only through ordering: the captured fields are initialized before any user-written code runs.

Field initializers may reference: constants, literals, static members of any class, `new` expressions, lambdas that capture only the above, primary-constructor parameters ([§22.10](#sec-methods-and-constructors-primary-constructors)), and `typeof` expressions.

Field initializers may **not** reference: `this`, instance members, `base`, virtual methods, or any expression that would observe `this` indirectly.

The base-call-before-field-initializers ordering is a v0.73 normalization. Earlier revisions ran field initializers first, then the base call. The new order matches the order already used by primary-constructor inheritance ([§22.10](#sec-methods-and-constructors-primary-constructors)'s "base call before field initializers" rule has been the spec for primary ctors all along) and aligns with C#, Java, and C++. The change makes exception safety simpler — when a derived field initializer throws, the base subobject is fully constructed and can be cleanly disposed by the runtime — and removes the inconsistency between primary-ctor and convenience-ctor ordering. The change is essentially source-compatible: [§22.8](#sec-field-initializers)'s restrictions on what field initializers may reference (no `this`, no instance members, no `base`) mean the only programs whose observable behavior changes are those whose `base(...)` arguments depended on the side effects of a derived field initializer running first, which is a rare and unidiomatic pattern.

<a id="sec-owned-fields-and-exception-safety"></a>
### 22.9 Owned fields and exception safety

When construction throws, the compiler unwinds the partially-constructed instance in the reverse of the construction order ([§22.8](#sec-field-initializers)):

- An exception during the **base or chained constructor call** (step 1) leaves no derived state to clean up — the base ctor's own exception-safety logic disposes whatever it had constructed of the base subobject. The exception propagates to the derived ctor's caller; the derived instance is never observable.
- An exception during a **field initializer** (step 2) triggers disposal of any earlier derived owned fields whose initializers completed, in reverse declaration order, followed by the base subobject's `Dispose()` (which the runtime invokes because the base ctor completed successfully). The exception then propagates.
- An exception in the **constructor body** (step 3) triggers disposal of every owned field assigned during initialization (step 2) and the base subobject. Owned fields assigned imperatively in the body are also disposed if assigned successfully before the throwing statement.

The compiler generates the necessary `try`/`finally` around each phase. The base-first construction order ([§22.8](#sec-field-initializers)) is what makes this story clean: by the time any derived state exists, the base subobject is fully constructed and its destructor can run cleanly during unwinding.

<a id="sec-methods-and-constructors-primary-constructors"></a>
### 22.10 Primary constructors

A class may declare its primary constructor's parameters in the class header:

```bezel
public class BankAccount(name: string, initialBalance: decimal)
  private _name: string := name;
  private _balance: decimal := initialBalance;

  public method Deposit(amount: decimal)
    _balance := _balance + amount;
  end Deposit;
end BankAccount;
```

The parameters are in scope:

- Inside field initializers.
- Inside the inheritance clause's base call (see below).
- Inside the body of any method, property, constructor, or destructor of the class. The compiler captures them as private fields automatically.

<a id="sec-capture-rule-and-layout-stability"></a>
#### Capture rule and layout stability

A primary-constructor parameter is captured as a compiler-generated private field **if and only if** it is referenced in a body position — inside the body of any method, property accessor, constructor, or destructor. Parameters used only in field initializers or in the inheritance clause's base call are *not* captured: their values flow directly into the field they initialize or the base argument they supply, and no separate parameter-named field exists.

```bezel
public class Greeter(name: string, prefix: string)
  // `prefix` flows into _label's initializer and is never used elsewhere — not captured.
  private _label: string := prefix + ": ";
  
  // `name` is referenced in Greet's body — captured as a private field.
  public method Greet(): string => _label + name;
end Greeter;
```

Here `Greeter` has two private fields after compilation: the explicit `_label`, and a compiler-generated capture field for `name`. There is no field for `prefix`.

This capture-iff-used rule keeps the generated field set small, but it has a **layout-stability consequence**: adding or removing a body-position reference to a primary-constructor parameter changes whether that parameter is captured as a field. Removing `Greet` from the example above removes the capture of `name`; adding a new method that references `prefix` adds a capture of `prefix`. The change is observable through any binary-metadata format keyed on the field set (binary serialization, native ABI). It is *not* observable through member-access reflection: Bezel has no field-enumeration or by-name member-access reflection, and that capability is declined rather than deferred (rationale [§18](Bezel_Design_Rationale.md#sec-things-deferred), [§5.18](Bezel_Design_Rationale.md#sec-type-identity-not-reflection)) — so the only programs that can observe a capture-set change are those reaching the field set through a binary format, not through runtime reflection.

Programs that need stable field layout across revisions — for binary serialization, framework-keyed reflection, or ABI guarantees with native code — should not rely on primary-constructor capture. The remedies, in order of preference:

1. Declare the field explicitly with an initializer that references the parameter (`private _name: string := name;`). The field's name is then under the programmer's control, and adding or removing body references doesn't change its existence.
2. Declare the parameter-derived value as a public property (`public readonly property Name: string := name;`). Public properties are part of the type's public surface and are therefore already stable.
3. Use a non-primary constructor with explicit field assignments. This is the most ceremony but the most stable.

The capture-iff-used rule is intentional and matches C# 12's primary-constructor design. Both languages treat primary constructors as ergonomic shorthand for short-lived data-carrier-style classes where ABI stability is a non-concern. Code that writes a primary constructor is implicitly opting into "the compiler chooses my field layout based on body content"; code that needs field-layout guarantees uses explicit declarations.

Primary-constructor parameters are **not** automatically promoted to public properties. To expose one, declare a property explicitly:

```bezel
public class BankAccount(name: string, initialBalance: decimal)
  public readonly property Name: string := name;
  private _balance: decimal := initialBalance;
end BankAccount;
```

When a class with a primary constructor has a base class, the base call appears in the inheritance clause:

```bezel
public class SavingsAccount(name: string, initialBalance: decimal, rate: decimal)
    : BankAccount(name, initialBalance)
  private _rate: decimal := rate;
end SavingsAccount;
```

This replaces an explicit `base(...)` call.

A class with a primary constructor may declare additional convenience constructors. Each must chain to another constructor via `this(...)`; only the primary may chain to `base(...)`. The single-base-call rule keeps the base initialization logic and any captured-parameter side effects in one place: every instance of the class passes through the primary, regardless of which convenience entry point was used. Convenience constructors that need a different base call must be expressed as a different design — typically a derived class with its own primary, or a static factory method that constructs and post-configures.

<a id="sec-object-initializer-blocks"></a>
### 22.11 Object initializer blocks

`new T(args) with ... end` constructs an instance and assigns to public settable properties or public fields:

```bezel
var t := new WaferTest(zUp := true) with
  SaveResults := true,
  NotifyUi := false
end;
```

Equivalent to:

```bezel
var t := new WaferTest(zUp := true);
t.SaveResults := true;
t.NotifyUi := false;
```

Only `Name := value` pairs are permitted; no statements, control flow, or method calls. Targets must be public settable properties or public fields. Assignments execute in textual order, after the constructor returns. If an assignment throws, preceding assignments are not rolled back, but the partially-initialized instance itself **is** disposed: the object initializer block participates in the [§28.3](#sec-local-variables-ownership-and-borrows) exception-path discharge rule the same way any owned `new` result does. The instance produced by `new T(...)` is an owned value (sourceless return); during the object initializer block's setter sequence, the instance is in an unattached-owned state, and if any setter throws, the instance is disposed by the implicit `try`/`finally` cleanup before the exception propagates. The end effect: an object initializer block that fails midway leaks neither memory nor partially-attached resources, just like a plain `new T(...)` whose result is dropped on an exception path.

The semantics of object initialization (mutation through setters) does not apply to records. The `with ... end` syntax following a `new RecordType(...)` expression is parsed by the same grammar production but resolved by the type-checker as the record's `with` operation ([§9.5](#sec-non-destructive-update-with)) — a copy with the named fields replaced — rather than as setter mutation. The resolution is purely a function of the constructed type's kind, invisible at the call site beyond the choice of class vs. record.

<a id="sec-static-members"></a>
### 22.12 Static members

Static methods, properties, fields, events, and constructors are declared with `static`. A class-level `const` is implicitly static; writing `static const` is redundant but permitted for clarity — both forms declare the same constant.

`const` declarations (with or without `static`) require a compile-time-constant expression and a primitive value type or `string`. `static readonly` accepts any value but is read at runtime.

A static member of a *generic* class is accessed by supplying the type arguments on the type name in receiver position — `Registry<int>.Register()`, `Cache<string>.Default`. Each distinct instantiation has its own static state ([§22.13](#sec-static-initialization-order)). This is one form of the broader "generic arguments in an expression" family the grammar admits ([Appendix A.7](#sec-expressions)): generic type arguments on a name in receiver position (the static-member form here), and explicit method type arguments at a call site, `obj.Method<int>(args)`. Both supply generic arguments where an expression is expected; the parser resolves the `<…>` against the generics-vs-less-than ambiguity as described in the grammar.

<a id="sec-static-initialization-order"></a>
### 22.13 Static initialization order

Within a single type, static fields are initialized in textual declaration order.

Across types, a type's static fields are initialized lazily on first use. Once initialization begins for a type, all of its static fields are initialized before any of its methods can run. The static constructor, if any, runs after all static field initializers complete.

Forward references between `static readonly` fields of the same type are a compile error. Specifically: if the initializer expression for a `static readonly` field F reads a `static readonly` field G of the same type, and G is declared *after* F in the source, the reference is rejected at compile time. The error message names both fields and points to the textual ordering rule.

This rule is stricter than C#'s, which silently observes the field's default value during such forward references. Bezel's stance is that the default-value behavior is a footgun: for non-nullable reference-typed `static readonly` fields no default value exists at all, and even where a default does exist (value types, nullable references), the reading code rarely intends to observe the default — the more common cause is a declaration-order mistake. Rejecting at compile time forces the user to either reorder the declarations or, for genuinely cyclic cases, move the logic into the static constructor where ordering is explicit.

A static constructor body may freely reference any of the type's static fields in any order; the constructor body runs after all field initializers have completed, so no forward-reference question arises there. Code that needs cyclic or order-independent static initialization should use the static constructor.

Forward references between `const` declarations are not subject to this rule: a `const` initializer must be a compile-time-constant expression ([§22.12](#sec-static-members)), so forward references between consts are either resolved at compile time (the textual order doesn't matter — consts have no temporal initialization) or are themselves a compile error for non-constant operands.

Cycles in static initialization across types are rejected by the compiler.

Enum constants are compile-time values and may be referenced freely from static initializers without triggering initialization.

`await` may not appear in static field initializers or static constructor bodies. Static initialization runs synchronously during type loading; suspending would leave the type in a partially-initialized state visible to other threads. Asynchronous initialization should be performed in an explicitly-called `async` method, with the result stored in a `static readonly` field via a synchronous wrapper. The standard pattern is to call `task.Wait()` to block until completion, then read `task.Result` for the value (the synchronous escape hatches noted in [§30.3](#sec-the-task-and-taskt-types)); the wrapper itself is a non-`async` method that performs the sequence.

<a id="sec-disposal-of-static-owned-fields"></a>
#### Disposal of static `owned` fields

A static field may be marked `owned`. Static `owned` fields are disposed at program exit, after `Main` returns (whether normally or by uncaught exception), in **reverse order of static initialization** across all types — the type whose static initializer ran last is the first to have its static `owned` fields disposed. Within a single type, multiple static `owned` fields are disposed in reverse declaration order, mirroring the instance-field disposal rule ([§28.4](#sec-the-destructor)).

A static field of owning-inline-array type (`[N owned]T?` or `[N owned]T`, [§15.1.1](#sec-owning-inline-arrays)) participates in the same program-exit disposal pass even though it does not carry the `owned` field modifier — the type-level `owned` slot marker conveys the obligation. At program exit, each non-null slot's `Dispose()` is called in the same reverse-of-initialization order as for `static owned` fields, and slot order within the array is unobservable (slots are disposed independently). A static field of owning-inline-array type whose initializer threw is not disposed (the array's slots were never observable); a static field whose initializer completed successfully before a later failing initializer is disposed normally.

A static `owned` field whose initializer threw is not disposed (its value was never produced). A static `owned` field on a type left in a failed state ([§22.7](#sec-constructors)) whose initializer *did* complete successfully before the failing initializer is disposed normally.

The runtime guarantees disposal occurs on a single, dedicated shutdown thread; no other Bezel code runs during the shutdown sequence. Native libraries holding callbacks into Bezel code should withdraw or release them before disposal begins.

Disposal at program exit is largely cosmetic from the operating system's perspective — process termination reclaims process-private resources regardless. The motivation for the rule is consistency: the same `owned` mechanism that governs instance fields also governs static fields, so the same code patterns work in both places. Programs that rely on a particular shutdown ordering should organize their static state explicitly rather than depend on the program-exit ordering above.

<a id="sec-static-class-typed-fields-without-owned"></a>
#### Static class-typed fields without `owned`

A `static readonly` field of class type or interface type that is not marked `owned` is exempt from the [§28.8](#sec-ownership-transfer) ownership-discharge requirement. The value lives for the duration of the program; the runtime reclaims its memory at process exit. The field is not disposed at program exit and must not hold resources that require explicit cleanup.

This exemption exists because many static fields hold long-lived, resource-free objects — caches, registries, singleton configuration — whose only "resource" is heap memory. Requiring `owned` on every such field would force program-exit disposal ordering for objects that have no meaningful cleanup.

If the static field holds a resource-bearing object, it must be marked `static owned readonly`, and the disposal-at-exit rules above apply. The compiler **warns** (not errors) when a `static readonly` field of a class type whose `Dispose()` is non-trivial — a class with a user-defined destructor, with `owned` fields of its own, or with owning-inline-array fields — is declared without `owned`. For interface-typed static fields, the warning is conservative: the compiler emits it whenever the interface's set of permitted runtime types could include a class with a non-trivial `Dispose()`, which is the typical case. The warning text identifies the class (or the interface, with a note about its open implementation surface) and points at two remedies: (a) add the `owned` modifier to enroll the field in program-exit disposal, or (b) keep the field as-is if the singleton's lifetime is intentionally process-bound and its non-disposal at exit is acceptable.

The warning is not promoted to an error because the singleton-with-resources pattern is widespread and idiomatic — a process-lifetime HTTP client, a database connection pool, a logger — and the cost of forcing every such case to either declare `owned` or restructure outweighs the benefit. Bezel's stance matches C# here: the lifetime contract is process-bound, and process termination handles the implicit "release."

##### Disposal under abnormal termination

The disposal-at-exit guarantees above apply only to **graceful shutdown** — `Main` returning normally, `Main` returning by an uncaught exception that the runtime intercepts, or an explicit `Environment.Exit()` call (std lib [§2.4](Bezel_Standard_Library.md#sec-environment)) that runs through the standard shutdown sequence. Disposal does **not** run under any of these conditions:

- A `kill -9` / `TerminateProcess` external signal.
- A panic / fatal error in the runtime that bypasses orderly shutdown.
- An `abort()` from native code reached via FFI.
- A power loss or other unrecoverable hardware fault.

Programs whose correctness depends on disposal-time effects (writing a final log entry, releasing an external lock recorded outside the process) must not rely on `static owned` disposal alone — they need an external mechanism (a separate process holding the lock with a TTL, a write-ahead log, a watchdog) to recover when disposal does not run. This is the same caveat that applies to any cleanup mechanism that runs at process shutdown; Bezel's promise is "Dispose runs reliably during normal termination," not "Dispose runs in every case."

<a id="sec-no-params-no-ref-returns"></a>
### 22.14 No `params`, no ref returns

There is no `params` keyword. A variadic-style signature uses `Array<T>` with a collection literal at the call site:

```bezel
public method Log(messages: Array<string>) ...
Log(["hello", "world"]);
```

There are no ref returns. The compiler is expected to apply return-value optimization and copy elision for large value types.


---

<a id="sec-properties-and-indexers"></a>
## 23. Properties and Indexers

A property is a member that looks like a field on access. An indexer is a property whose name is replaced by a parameter list in square brackets.

<a id="sec-auto-properties"></a>
### 23.1 Auto-properties

```bezel
public property Name: string;                         // read-write
public property Id: int private set;                  // read-write with private setter
public readonly property AccountNumber: string;       // read-only
public readonly property Pi: double := 3.141592653589793;  // read-only with initializer
```

The compiler generates the backing field. A `readonly` auto-property may be assigned only in a field initializer or in a constructor.

<a id="sec-expression-bodied-read-only-properties"></a>
### 23.2 Expression-bodied read-only properties

```bezel
public property Area: decimal => Width * Height;
public property IsEmpty: bool => _count = 0;
public property FullName: string => $"{FirstName} {LastName}";
```

<a id="sec-block-form-properties"></a>
### 23.3 Block-form properties

```bezel
public property Balance: decimal
  get => _balance;
  set => do
    if value < 0m then throw new ArgumentException("Balance cannot be negative"); end;
    _balance := value;
  end;
end Balance;
```

The implicit setter parameter is `value`. Each accessor uses `=> expression` or `=> do ... end`. A read-only property in block form omits `set`; a write-only property omits `get`.

Accessor visibility ([§13.6](#sec-accessor-visibility)) may make a getter or setter more restrictive than the property as a whole.

**Explicit setter parameter modifier.** The setter's implicit `value` parameter has no modifier by default and takes a borrow of the property's declared type. For owning collections (like `OwnedArray<T>`'s indexer setter) the setter needs to take ownership of the new value (and dispose the previous slot value). Write an explicit `set(owned)` parameter spec to declare an owned-taking setter:

```bezel
public class OwnedArray<T> where T: class
  public property [i: int]: T
    get;                     // implicit value: T, borrow-returning
    set(owned);              // explicit owned: takes owned T;
                             //   disposes previous slot value
  end;
end OwnedArray;
```

The spelling `set(owned)` keeps the implicit name `value`; the spelling `set(owned x)` renames the parameter to `x`. The parameter's type is always the property's declared type, so it is not repeated. The form is admitted on block-form properties and indexers; abstract/interface accessor bodies (`;`-terminated) carry the spec the same way (`set(owned);`). The standard-library owning collection types (`OwnedArray<T>`, `OwnedDictionary<K, V>`) use this form for their indexer setters; the owning-collection interfaces (`IOwnedList<T>`) declare it on their abstract indexer setters.

<a id="sec-indexers"></a>
### 23.4 Indexers

An indexer is a property whose name is replaced by `[parameters]`:

```bezel
public property [i: int]: string => _items[i];        // expression-bodied

public property [row: int, col: int]: double => _grid[row * _width + col];

public property [key: string]: int
  get => _values[key];
  set => do
    if value < 0 then throw new ArgumentException("must be non-negative"); end;
    _values[key] := value;
  end;
end;
```

A block-form indexer's `end` clause has no name to reference; it is plain `end;`.

Inside a member of a class with its own indexer, `this[i]` is required (the brackets need a left-hand side).

Indexers may be declared in interfaces with the body-less form. Use `readonly` to expose only a getter:

```bezel
interface IList<T>: IReadOnlyList<T>, ICollection<T>
  property [i: int]: T;
end IList;
```

Indexers may be overloaded by parameter type list. They participate in `virtual`/`override`/`sealed`/`abstract` like methods.

Records may declare read-only indexers. Body-form records with mutable properties may declare read-write indexers; positional-only records may not.

<a id="sec-properties-and-indexers-restrictions"></a>
### 23.5 Restrictions

- Enums may not declare indexers.
- Static indexers are not supported.
- `ref` and `out` parameters are not allowed in indexer parameter lists.
- Assigning to a member of a value-typed indexer result is a compile error: `g[0].X := 5` would modify a discarded copy. Read, modify, write back through the setter.
- Properties and indexers whose declared type is a move-only value type (an owning inline array, [§15.3](#sec-move-only-value-types)) are not admitted. A getter would need to either copy the move-only value (forbidden by move-only semantics) or move it out of the backing storage (which would leave the storage in moved-from state, conflicting with the property abstraction). Programs that need to expose an owning inline-array field interactively use a regular public field (with the inline array's `owned` slot marker conveying the ownership) or, for transfer-out semantics, an explicit method that returns the owned value.

---

<a id="sec-events"></a>
## 24. Events

An event is a named publication point on a class. The class controls when the event fires; external code subscribes but cannot fire the event or inspect the subscriber list.

<a id="sec-declaration"></a>
### 24.1 Declaration

An event declaration names a **payload type** — the type of value passed to handlers when the event fires:

```bezel
public event Clicked: ClickedEvent;
public event ValueChanged: ValueChangedEvent;
public static event ApplicationExiting;              // no payload; implicit ()
```

The payload type may be any type. Records are strongly recommended ([§24.2](#sec-payload-records)). When no payload type is specified, the payload is the unit type `()`.

Static events use `static`.

Asynchronous events require the `async` modifier. Async events invoke handlers that return `Task`; see [§24.5](#sec-raising-events).

```bezel
public async event DataReceived: DataReceivedEvent;
public async event ProcessCompleted: ProcessResult;
```

Modifier order: `visibility static async event`. Events do not support `virtual`, `override`, `abstract`, `sealed`, or `readonly`.

Events may be declared on interfaces; see [§24.7](#sec-events-on-interfaces). The interface form omits the body and ends with `;`, like other interface members.

The compiler generates a private `owned` backing field of type `EventSource<T>` (or `AsyncEventSource<T>`) for each event declaration. The backing field is initialized during construction and disposed when the containing object is disposed, following the standard owned-field rules ([§28.5](#sec-owned-fields)). For static events, the backing field is a static owned field with program-exit disposal ([§22.13](#sec-static-initialization-order)).

<a id="sec-payload-records"></a>
### 24.2 Payload records

Event payloads should be records. There is no required base class. The sender, if any, is a field of the payload record, typed precisely:

```bezel
record ClickedEvent(Sender: Button, X: int, Y: int, Button: MouseButton);
record ValueChangedEvent(Sender: Slider, OldValue: decimal, NewValue: decimal);
record DataReceivedEvent(Sender: Connection, Payload: Array<byte>, Timestamp: DateTime);
record ProcessResult(Success: bool, Message: string);
```

Using a record provides named fields, destructuring in handlers, and a single point of documentation for the event's data contract.

<a id="sec-the-type-of-an-event-member"></a>
### 24.3 The type of an event member

An event member has two faces, distinguished by whether the access is from inside the declaring class or from outside.

For a synchronous event `event E: T`, the handler type is `(T) -> ()`. For an asynchronous event `async event E: T`, the handler type is `(T) -> Task`. These handler types are derived from the payload type and are not written in the event declaration.

**Outside the declaring class**, an event member has type `IEvent<T>` (synchronous events) or `IAsyncEvent<T>` (async events). These interfaces expose only the subscribe-side operations and are covariant in the payload type ([§12.9](#sec-variance)):

```bezel
public interface IEvent<out T>
  method Subscribe(handler: (T) -> ()): owned Subscription;
end IEvent;

public interface IAsyncEvent<out T>
  method Subscribe(handler: (T) -> Task): owned Subscription;
end IAsyncEvent;
```

`T` is the event's payload type. `out T` is sound because T appears once, in the parameter position of `(T) -> ()` (contravariant), which is itself a method parameter (contravariant) — composing to a covariant position overall. `EventSource<T>` and `AsyncEventSource<T>` (the producer-side classes) remain invariant because their `Raise(payload: T)` method places T in input position.

**Inside the declaring class** (and inside derived classes for `protected` events), the same member has type `EventSource<T>` (or `AsyncEventSource<T>`), which extends the interface above with `Raise` (or `RaiseAsync`) and provides the `+=`/`-=` operators ([§24.4](#sec-subscribing)):

```bezel
public class EventSource<T>: IEvent<T>
  // No public constructor; obtained via event declaration.
  public method Subscribe(handler: (T) -> ()): owned Subscription;
  public method Raise(payload: T);
  public method Add(handler: (T) -> ());
  public method Remove(handler: (T) -> ());
end EventSource;

public class AsyncEventSource<T>: IAsyncEvent<T>
  public method Subscribe(handler: (T) -> Task): owned Subscription;
  public async method RaiseAsync(payload: T): Task;
  public method Add(handler: (T) -> Task);
  public method Remove(handler: (T) -> Task);
end AsyncEventSource;
```

Inside the declaring class, the syntactic forms `event += handler` and `event -= handler` are recognized by the compiler and rewritten to `event.Add(handler)` and `event.Remove(handler)` respectively. They are **not** user-overloadable operators — `operator +=` and `operator -=` cannot be declared on any type ([§25.1](#sec-overloadable-operators)) — and they do not interact with the compound-assignment statement form. The `+=` and `-=` syntax on events is a fixed, language-defined desugaring, available only when the static type of the left operand is `EventSource<T>` or `AsyncEventSource<T>`.

The compiler enforces visibility: outside the declaring class, the member's static type is the interface (`IEvent<T>` or `IAsyncEvent<T>`), so neither `Add`/`Remove` nor `Raise`/`RaiseAsync` are in scope. External code subscribes through `Subscribe` (which is on the interface) and unsubscribes by disposing the returned `Subscription`. The split is type-driven, not a special access-control rule.

For no-payload events (declared without a type), the payload is `()`. `EventSource<()>` has a convenience `Raise()` overload that takes no arguments, equivalent to `Raise(())`.

<a id="sec-subscribing"></a>
### 24.4 Subscribing

The `+=` and `-=` syntax and the `.Subscribe()` method serve different contexts. The `+=`/`-=` forms are available only inside the declaring class (and inside derived classes for `protected` events), where the event's static type is `EventSource<T>` or `AsyncEventSource<T>`. Outside the declaring class, the event's static type is `IEvent<T>` (or `IAsyncEvent<T>`), which exposes only `.Subscribe()`. External code subscribes through `.Subscribe()` and unsubscribes by disposing the returned `Subscription`.

<a id="sec-with-named-methods-inside-the-declaring-class"></a>
#### With named methods (inside the declaring class)

```bezel
button.Clicked += OnButtonClicked;
button.Clicked -= OnButtonClicked;
```

Where the handler has signature `(ClickedEvent) -> ()`:

```bezel
public method OnButtonClicked(e: ClickedEvent)
  UpdateStatus(e.X, e.Y);
end OnButtonClicked;
```

`+=` adds a handler; `-=` removes the first matching handler. If no match is found, `-=` is a no-op. These syntactic forms desugar to `event.Add(handler)` and `event.Remove(handler)` respectively ([§24.3](#sec-the-type-of-an-event-member)); they are not user-overloadable operators and not the compound-assignment statements of [§17](#sec-expressions-and-operators) — `event += handler` does not read `event` and does not assign to it.

<a id="sec-with-subscribe"></a>
#### With `.Subscribe()`

For lambdas, or any subscription whose lifetime should be tied to a scope:

```bezel
using var sub := button.Clicked.Subscribe((e) => UpdateStatus(e.X, e.Y));
```

`.Subscribe()` returns an owned `Subscription` that implements `IDisposable`. Disposing the subscription removes the handler. Storing the subscription in an `owned` field ties its lifetime to the containing object.

`.Subscribe()` is available everywhere — inside or outside the declaring class — because it is on the public interface.

<a id="sec-raising-events"></a>
### 24.5 Raising events

Inside the declaring class:

```bezel
Clicked.Raise(new ClickedEvent(this, x, y, button));                    // synchronous
await DataReceived.RaiseAsync(new DataReceivedEvent(this, payload, t)); // asynchronous
ApplicationExiting.Raise();                                              // no-payload
```

`Raise` and `RaiseAsync` invoke handlers in subscription order. With zero subscribers, both methods are no-ops — there is no null check at the call site, no `?.`, no `Invoke` indirection.

The synchronous form `Raise` returns `()` and runs each handler to completion before invoking the next. The asynchronous form `RaiseAsync` returns a `Task` that completes when every handler's task completes; each handler's task is awaited before the next is invoked.

<a id="sec-handler-exceptions"></a>
#### Handler exceptions

If a synchronous handler throws, subsequent handlers are not invoked and the exception propagates out of `Raise` to the raiser. If an asynchronous handler's task faults, subsequent handlers are not invoked and the exception propagates from the awaited `RaiseAsync`. If an asynchronous handler's task is cancelled, the behavior is the same as a fault: subsequent handlers are not invoked and `OperationCanceledException` propagates from the awaited `RaiseAsync`.

Handlers that must run independently of one another should catch their own exceptions:

```bezel
connection.DataReceived += async (e) => do
  try
    await ProcessPayloadAsync(e.Payload);
  catch
    var ex: Exception => Log($"Handler failed: {ex.Message}")
  end;
end;
```

To run all handlers regardless of failures and aggregate exceptions, the raiser must implement that policy explicitly — `Raise`/`RaiseAsync` do not aggregate.

Calling `Raise` or `RaiseAsync` outside the declaring class is a compile error: the static type of the event member outside the declaring class is the interface, which has no `Raise` method. Derived classes may raise `protected` events declared on their base class because the protected access exposes the `EventSource` form.

<a id="sec-thread-safety"></a>
### 24.6 Thread safety

The event subscriber list is not synchronized. Within a single execution domain ([§30.13](#sec-runtime-model)) it does not need to be: continuations never run concurrently, so subscribe, unsubscribe, and raise operations — whether reached from sync or async code — are serialized by construction, and no external synchronization is required. (When handlers are async, the subscriber list may still change between a raise's suspension points; that is interleaving, not a data race.) External synchronization (`Mutex`, [§30.12](#sec-thread-synchronization)) is required only where event operations are reachable from foreign threads through the FFI ([§32](#sec-foreign-function-interface)).

<a id="sec-events-on-interfaces"></a>
### 24.7 Events on interfaces

An interface may declare event members. The interface form omits the body and ends with `;`, like other interface members:

```bezel
public record PropertyChangedEvent(Sender: Object, PropertyName: string);

public interface INotifyPropertyChanged
  event PropertyChanged: PropertyChangedEvent;
end INotifyPropertyChanged;
```

For asynchronous events, `async event` is used:

```bezel
public interface IDataSource
  async event DataReceived: DataReceivedEvent;
end IDataSource;
```

An implementing class declares the event normally; the compiler-generated `EventSource<T>` (or `AsyncEventSource<T>`) backing field satisfies the interface contract because `EventSource<T>` implements `IEvent<T>` and `AsyncEventSource<T>` implements `IAsyncEvent<T>`:

```bezel
public class Person: INotifyPropertyChanged
  public event PropertyChanged: PropertyChangedEvent;

  private _name: string := "";

  public property Name: string
    get => _name;
    set => do
      if _name <> value then
        _name := value;
        PropertyChanged.Raise(new PropertyChangedEvent(this, nameof(Name)));
      end;
    end;
  end Name;
end Person;
```

Outside the implementing class, the event is accessed through the interface, where its type is `IEvent<T>`. Only `Subscribe` is in scope:

```bezel
var p: INotifyPropertyChanged := new Person();
using var sub := p.PropertyChanged.Subscribe((e) =>
  Console.WriteLine($"Changed: {e.PropertyName}"));
```

The interface contract guarantees subscription works; raising remains restricted to code that holds the implementing class's static type, just as for class-declared events.

Interface events follow the same modifier rules as class events: only `async` is meaningful in the interface declaration. `static`, `virtual`, `override`, `abstract`, and `sealed` do not apply (interfaces have no implementations to be virtual or static).

An interface event declaration cannot specify a payload type that is itself unimplementable (e.g. a private nested type). The payload type's visibility must be at least as accessible as the interface itself.

---

<a id="sec-operator-overloading"></a>
## 25. Operator Overloading

Records, enums, and classes may overload operators. Operators are declared as `public static` methods using the `operator` keyword.

<a id="sec-overloadable-operators"></a>
### 25.1 Overloadable operators

- Unary: `+`, `-`, `~`, `++`, `--`
- Binary: `+`, `-`, `*`, `/`, `mod`, `&`, `|`, `^`, `<<`, `>>`
- Comparison: `<`, `>`, `<=`, `>=`, `=`, `<>`

`not` is **not** user-overloadable. It is the boolean-negation operator and applies only to `bool` ([§6.5](#sec-bitwise-operations)); types wanting a domain-specific inversion provide a named method (`.Negate()`, `.Inverted()`) or overload `~` if a bitwise reading applies. This is the one place where Bezel explicitly excludes a unary operator from the overload set in favor of a named method, so that the boolean reading of `not` is preserved at every call site.

The following operators are also not overloadable, for various reasons:

- The null-forgiving `!`, the null-coalescing `??`, and the type-test operators `is`, `is not`, `as` — these have null-aware or type-system semantics fixed by the language; user types do not customize them.
- Range operators `..` and `..<` — these construct values of the language-built-in `Range<T>` type ([§16](#sec-range-expressions)); user types do not customize them.
- The `in` operator — customization is via the structural `Contains` lookup ([§17.5](#sec-the-in-operator)).
- Assignment and compound-assignment operators (`:=`, `+=`, `-=`, `*=`, `/=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, `??=`) — Bezel has no compound-operator overload mechanism; the compound forms desugar mechanically to read-modify-write using the underlying binary operator's overload.
- The `+=` and `-=` syntactic forms used with events ([§24.4](#sec-subscribing)) — these are a fixed, language-defined desugaring available only on `EventSource<T>` and `AsyncEventSource<T>`; they are not operator overloads and are not part of the user-overloadable surface.

<a id="sec-operator-overloading-restrictions"></a>
### 25.2 Restrictions

- `=` and `<>` may **not** be overloaded on classes or on inline arrays. For classes, `=` always invokes `Equals`. For inline arrays, the language-defined structural equality of [§8.3](#sec-equals-and-gethashcode) is the only `=` available; inline arrays cannot be extended ([§26.2](#sec-what-can-be-extended)), so the overload mechanism does not reach them. Records and enums may overload `=` and `<>`.
- Comparison operators must be overloaded as a complete group: `<`, `>`, `<=`, `>=` together; `=` and `<>` together.
- `++` and `--` are postfix-only and statement-only. There is no separate prefix overload; the single overload is used in the one permitted position.

When a record overloads `=` and `<>`, the overloaded operators are invoked for `a = b` and `a <> b` directly — they do not delegate to `Equals`. The auto-generated or user-supplied `Equals(Object?)` and `Equals(T)` methods continue to exist and may have different (though usually identical) semantics; they are reachable via `.Equals(...)` call syntax. Authors who overload `=` should ensure consistency with the type's `Equals` and `GetHashCode` implementations to avoid surprising behavior in collections and pattern matching, which use `Equals` rather than `=`.

<a id="sec-examples"></a>
### 25.3 Examples

```bezel
record Vector2(X: double, Y: double)
  public static method operator +(left: Vector2, right: Vector2): Vector2 =>
    new Vector2(left.X + right.X, left.Y + right.Y);

  public static method operator *(v: Vector2, scalar: double): Vector2 =>
    new Vector2(v.X * scalar, v.Y * scalar);
end Vector2;

record Counter(Value: int)
  public static method operator ++(c: Counter): Counter =>
    new Counter(c.Value + 1);
end Counter;
```

Bitwise complement is overloadable for types with a natural bit-pattern or set-complement reading:

```bezel
record BitMask(Bits: ulong)
  public static method operator ~(m: BitMask): BitMask =>
    new BitMask(~m.Bits);
end BitMask;
```

For domain-specific inversion that does not match a bitwise reading, define a named method instead:

```bezel
record TriBool(Value: int)
  public method Negate(): TriBool => case Value
    0 => new TriBool(1),
    1 => new TriBool(0),
    _ => this
  end;
end TriBool;
```

The named method form makes the inversion explicit at the call site (`t.Negate()` rather than `not t`), which is preferable when the operation departs from the boolean reading of `not` ([§14.4](Bezel_Design_Rationale.md#sec-not-is-not-user-overloadable) of the design rationale).

When an operator overload is block-bodied (statements followed by `end`, rather than the expression form `=> ...`), the optional name in the closing `end` is written as `end operator OpToken;` — e.g. `end operator ~;` or `end operator +;`. The named-end form is recommended for non-trivial operator bodies.

<a id="sec-no-user-defined-conversion-operators"></a>
### 25.4 No user-defined conversion operators

User-defined types — records and classes — cannot declare implicit or explicit conversion operators. There is no syntactic form for declaring "convert from S to T" on a user type; the explicit-cast expression `T(value)` ([§5.2](#sec-explicit-conversions)) and the implicit-conversion rules ([§5.1](#sec-implicit-conversions)) apply only to the built-in conversions enumerated in [§5](#sec-type-conversions) and to the compiler-intrinsic conversions on `IntPtr`/`UIntPtr` ([§32.7](#sec-pointers-and-intptr)).

To express a conversion between user types, define a named factory or instance method:

```bezel
// Construction-style conversion: factory method on the destination type.
record Money(Amount: decimal, Currency: string)
  public static method FromDecimal(amount: decimal, currency: string): Money =>
    new Money(amount, currency);
end Money;

// Projection-style conversion: instance method on the source type.
record UserDto(Id: int, Name: string, Email: string)
  public method ToUser(): User => new User(Id, Name, Email);
end UserDto;
```

The named-method form is preferred over conversion operators for several reasons:

- The conversion is visible at the call site (`UserDto.ToUser()` rather than `(User)dto`); a reader sees what is happening without needing to look up declarations.
- The conversion's name can communicate intent (`FromDecimal`, `FromInt`, `ToReadOnly`) where a cast cannot.
- Overload resolution does not have to consider user-defined conversions; the conversion is a regular method call subject to ordinary resolution rules.
- The pattern composes with the rest of the language (extension methods, fluent chaining) without requiring any conversion-specific extensions.

C# supports both `implicit` and `explicit` user-defined conversion operators. Bezel deliberately omits both. The implicit form is widely considered a misfeature in retrospect even within the C# community — it makes overload resolution opaque and lets unintended conversions slip into expressions silently. The explicit form is less harmful but offers no advantage over a named method with the same compile-time check. Bezel's stance — visible conversions everywhere, with the compiler enforcing the visibility — is consistent with [§5.1](#sec-implicit-conversions)'s rule that implicit conversions exist only when no programmer-visible information is lost, and with [§6.2](#sec-mixed-type-arithmetic)'s prohibition on implicit `decimal`/binary-float mixing.

The compiler-intrinsic conversions on `IntPtr` and `UIntPtr` ([§32.7](#sec-pointers-and-intptr)) — explicit casts to and from `long` and `ulong` — are not user-defined and are not covered by this rule. They exist because `IntPtr`/`UIntPtr` are intrinsic FFI-boundary types whose explicit-cast forms are part of the compiler's pointer-handling surface, not a user-overridable mechanism.

---

<a id="sec-extension-blocks"></a>
## 26. Extension Blocks

An extension block adds methods and computed read-only properties to a type without modifying the type's source.

<a id="sec-extension-blocks-declaration"></a>
### 26.1 Declaration

```bezel
extension string
  public method IsBlank(): bool => this.Length = 0 or this.Trim().Length = 0;
  public property Words: Array<string> => this.Split(' ');
end;
```

An extension block is declared at module level. It may be `public` or `internal` (default `internal`). The block ends with `end;` (no name to repeat).

Inside the block, methods and computed properties are written as if they were instance members. `this` refers to the receiver.

<a id="sec-what-can-be-extended"></a>
### 26.2 What can be extended

- Classes (including `Object`).
- Interfaces.
- Records.
- Enums.
- Built-in primitive types.

What cannot be extended:

- Type parameters (`extension T` is meaningless).
- Nullable types directly (`extension int?`). Extend the non-nullable form; handle nullability in the body.
- Function types.
- Inline-array types (`extension [N]T`). Inline arrays are value-typed but their identity is per-size — `[3]int` and `[4]int` are distinct types — so an extension declaration would have to fix N, making the form rarely useful. Operations that should work across multiple inline-array sizes are typically written as free-standing methods that take a specific inline-array type per overload, or that take a heap-array view (`Array<T>`) that the caller constructs from the inline array's contents.

<a id="sec-extension-blocks-restrictions"></a>
### 26.3 Restrictions

- Only methods and computed read-only properties.
- No fields, auto-properties, constructors, destructors, or `owned` fields.
- No operator overloads.
- No indexers.
- No `virtual`, `override`, `abstract`, or `sealed` — extensions are statically dispatched.
- Extension members see only the extended type's public API. From the same module, `internal` members are also visible.

<a id="sec-generic-extensions"></a>
### 26.4 Generic extensions

```bezel
extension IEnumerable<T> where T: IComparable<T>
  public method Sort(): Array<T>
    var items := this.ToArray();
    // ...
    return items;
  end Sort;
end;
```

The block-level type parameter is in scope throughout the block. Methods may introduce their own type parameters:

```bezel
extension IEnumerable<T>
  public method Map<U>(transform: (T) -> U): IEnumerable<U>
    var result := new Array<U>();
    for var item in this do
      result.Add(transform(item));
    end;
    return result;
  end Map;
end;
```

A `where` clause on the block constrains which instantiations of the extended type the extension applies to.

<a id="sec-resolution"></a>
### 26.5 Resolution

When the compiler encounters `x.Foo(args)`:

1. **Instance members win.** If `x`'s declared type (or any base type) has a matching `Foo`, that is selected.
2. **Extension members.** If no instance member matches, the compiler searches all imported modules for extension blocks on `x`'s type, base types, or implemented interfaces. When an extension block is declared on a generic type (e.g. `extension IEnumerable<T>`), the block-level type parameter is inferred from the receiver's type: if `x` has type `Array<int>` and `Array<int>` implements `IEnumerable<int>`, then `T` is inferred as `int`. The inference follows the same unification rules as generic method type inference.
3. **Most specific type wins.** Extensions on the most specific type are preferred per the ordering below.
4. **Ambiguity.** If two equally-specific extensions match, the call is a compile error; the programmer must remove a conflicting import or qualify the extension's defining type at the call site.

Adding a real method to a type silently takes priority over an extension with the same name — by design.

Extensions are resolved on the **compile-time** type of the receiver, not the runtime type. Polymorphism requires `virtual`, not extensions.

<a id="sec-specificity-ordering"></a>
#### 26.5.1 Specificity ordering

For receiver type R, an extension on type E1 is **more specific** than an extension on E2 when one of the following holds (in order):

1. E1 is a subtype of E2 and E2 is not a subtype of E1. (Derived class beats base class; class beats interface implemented by that class; derived interface beats extended interface.)
2. E1 and E2 are both generic instantiations of the same generic type, and E1's type arguments are *more concrete* than E2's. A type argument is more concrete than another when it is a non-parameter type and the other is a generic type parameter, or when both are non-parameter and the rule applies recursively to their type arguments.
3. E1 has more constraints in its `where` clause than E2, and every constraint of E2 is implied by E1's constraints.

If none of the above relations holds in either direction, the extensions are equally specific and rule 4 above produces an ambiguity error.

```bezel
extension IEnumerable<T>
  public method First(): T
    for var item in this do return item; end;
    throw new InvalidOperationException("empty sequence");
  end First;
end;

extension IEnumerable<int>
  public method First(): int
    // optimized for int (an implementation may, for example,
    // short-circuit on the `Array<int>` storage layout).
    for var item in this do return item; end;
    throw new InvalidOperationException("empty sequence");
  end First;
end;

var xs: Array<int> := [1, 2, 3];
var s := xs.First();                          // selects the IEnumerable<int> overload
```

<a id="sec-multiple-blocks-for-the-same-type"></a>
### 26.6 Multiple blocks for the same type

Multiple `extension` blocks targeting the same type may appear across files in the same module. The compiler merges them. Duplicate signatures are a compile error.

<a id="sec-ownership-in-extensions"></a>
### 26.7 Ownership in extensions

The receiver is always a borrow; extensions cannot store `this`, return it as `owned`, or dispose it. Other parameters may be `owned`, following normal method rules. Extensions may return `owned` values.

---

<a id="sec-lambdas-and-function-types"></a>
## 27. Lambdas and Function Types

Functions are first-class values: stored, passed, returned, held in fields.

<a id="sec-function-types"></a>
### 27.1 Function types

A function type is written `(params) -> returnType`:

```bezel
var add: (int, int) -> int;
var isPositive: (int) -> bool;
var log: (string) -> ();              // procedure
var noop: () -> ();
var now: () -> DateTime;
```

The parallel with method declarations:

| Method | Function type |
|---|---|
| `method Foo(x: int): bool` | `(int) -> bool` |
| `method Foo(x: int)` | `(int) -> ()` |
| `method Foo(): int` | `() -> int` |
| `method Foo()` | `() -> ()` |
| `method Foo(owned t: T)` | `(owned T) -> ()` |
| `method Foo(ref n: int)` | `(ref int) -> ()` |
| `method Foo(out r: int)` | `(out int) -> ()` |
| `method Foo(out owned r: T)` | `(out owned T) -> ()` |

A method without a return type has function type returning `()`.

Function types may include the parameter modifiers `owned`, `ref`, `out`, and the combination `out owned`. The modifier is part of the type — `(T) -> ()` and `(owned T) -> ()` are distinct function types, and the same applies to the other modifier forms. Function-type compatibility requires modifiers to match exactly; there is no implicit conversion between modified and unmodified parameter slots. The available modifier combinations match those of method declarations ([§22.3](#sec-parameter-modifiers) and grammar A.4).

Function types may include parameter names. Names are documentation only; they do not affect type identity:

```bezel
var handler: (itemId: int, quantity: int, priority: int) -> bool;
```

`(int, int, int) -> bool` and the named form are the same type. Calls are positional.

<a id="sec-lambda-parameter-modifiers"></a>
#### Lambda parameter modifiers

Lambda parameters may carry the same modifiers when the target function type expects them:

```bezel
var attach: (owned EventHandler) -> () := (owned h) => _handlers.Add(h);
```

When the target type is known from context, the lambda's parameter modifiers may be omitted in source — the compiler infers them from the target. When inferring is not possible (a `var` introduction with no target context), the modifier must be written explicitly to produce a function value of the modified type.

<a id="sec-nullable-function-types"></a>
### 27.2 Nullable function types

```bezel
var callback: ((int) -> bool)?;       // nullable function returning bool
var nullableReturn: (int) -> bool?;   // non-null function returning a nullable bool
```

The parentheses around the function type are required before `?`. Without them, the `?` would attach to the return type.

<a id="sec-lambda-expressions"></a>
### 27.3 Lambda expressions

Expression body:
```bezel
var double := (x: int) => x * 2;
var greet := (name: string) => $"Hello, {name}!";
```

Block body:
```bezel
var processItem := (item: Item) => do
  var normalized := Normalize(item);
  var enriched := Enrich(normalized);
  return enriched;
end;
```

Inside a value-returning block-bodied lambda, `return` is required on every exit path.

For procedure lambdas, the body is a single expression whose result is discarded:
```bezel
var log := (msg: string) => Console.WriteLine(msg);
```

<a id="sec-parameter-type-inference"></a>
### 27.4 Parameter type inference

When the lambda's target type is known from context, parameter type annotations may be omitted:

```bezel
var doubled := numbers.Select((x) => x * 2);
var positives := numbers.Where((x) => x > 0);
```

In top-level contexts with no target type (`var f := (x) => x * 2;`), parameter types must be specified.

<a id="sec-closures"></a>
### 27.5 Closures

A lambda may reference variables from its enclosing scope. Captured variables are heap-allocated in a closure object. Modifications to a captured variable inside the lambda are visible outside, and vice versa.

```bezel
public method MakeCounter(): () -> int
  var count := 0;
  return () => do
    count := count + 1;
    return count;
  end;
end MakeCounter;
```

For `for` loop variables, each iteration produces a fresh binding. A lambda captured inside the body sees that iteration's value.

<a id="sec-method-references"></a>
### 27.6 Method references

When a lambda's body is just a call to an existing method, the method reference may be used directly:

```bezel
var parsed := strings.Select(int.Parse);
var lengths := names.Select(string.Length);       // property reference
var sorted := numbers.OrderBy(Math.Abs);          // static method
var greetings := names.Select(greeter.Greet);    // bound instance method
```

Method references work for static methods, instance methods (instance is captured), properties, and extension methods. A property reference produces the getter; its type is `(Receiver) -> PropertyType` for instance properties and `() -> PropertyType` for static properties. Whether the property also has a setter is irrelevant; setter references are not supported. The reference's signature must match the target function type exactly.

For an extension method, the reference uses the extended type's name as qualifier; the resulting function has the receiver as its first parameter:

```bezel
import System.Collections.Linq;
var counter: (IEnumerable<int>) -> int := IEnumerable<int>.Count;
```

Indexers cannot be used as method references; the parameter list makes the natural function type ambiguous.

<a id="sec-method-references-to-overloaded-methods"></a>
#### Method references to overloaded methods

When the named method (or property) is overloaded, the reference is resolved using the **target function type** at the use site. The compiler selects the overload whose parameter types and return type unify with the target's parameter and return types under the standard implicit-conversion rules ([§5.1](#sec-implicit-conversions)):

```bezel
var sortedByAbs := numbers.OrderBy(Math.Abs);     // double or int? Resolved
                                                  //   from numbers.ElementType
                                                  //   and OrderBy's target type.
```

If `numbers` has type `Array<int>`, `OrderBy<TKey>` expects a `(int) -> TKey`; the reference `Math.Abs` resolves to the `Math.Abs(int): int` overload. If `numbers` is `Array<double>`, the same reference resolves to `Math.Abs(double): double`.

When no target type is available (e.g., `var f := Math.Abs;` with no annotation), the reference is ambiguous and the compiler reports an error pointing to the candidate overloads. The user disambiguates by adding a type annotation: `var f: (int) -> int := Math.Abs;`.

When a name `T.M` could resolve to either a *static* member of `T` or an *instance method reference* of `T` (a method requiring a receiver), the static member wins. The instance form has no separate syntax in this version — if both interpretations apply and the programmer wants the instance form, the disambiguation is to use a lambda: `(t: T) => t.M(...)`. This case is rare in practice; it arises only when `T` declares an instance method named `M` and a static member also named `M`, which is unusual style.

<a id="sec-calling-function-values"></a>
### 27.7 Calling function values

```bezel
var sum := add(2, 3);

var maybeHandler: ((int) -> ())? := null;
maybeHandler?(42);                      // null-conditional call; no-op when null
```

The null-conditional call result is nullable.

<a id="sec-equality-of-function-values"></a>
### 27.7.1 Equality of function values

Function-typed values use reference equality for `=` and `<>`. Two function values are equal only when they refer to the same underlying function object — the same closure instance, the same method reference bound to the same receiver. Two separately-constructed lambdas with identical source text are *not* equal, even if they capture the same variables. A method reference `obj.Method` compared with another `obj.Method` evaluation is not guaranteed to be equal (each evaluation may produce a distinct function object).

The practical consequence: function values should not be used as dictionary keys or set elements. The `Contains` check on an event's subscriber list (for `-=` removal via `Remove`) uses reference equality, which is why `+=`/`-=` with named method references works reliably (the same method reference resolves to the same function pointer) but `+=`/`-=` with separately-constructed lambdas does not (each lambda is a fresh closure object).

The reliable pattern for `+=` followed by later `-=` is to store the function value once and use the same value for both operations:

```bezel
private _onClicked: (ClickedEvent) -> () := (e) => UpdateStatus(e.X, e.Y);
// ...
button.Clicked += _onClicked;
// later:
button.Clicked -= _onClicked;          // same reference; matches and unsubscribes
```

The anti-pattern, which silently fails to unsubscribe:

```bezel
button.Clicked += (e) => UpdateStatus(e.X, e.Y);
// later:
button.Clicked -= (e) => UpdateStatus(e.X, e.Y);   // a different lambda, no match
```

For named methods, the same store-once pattern is recommended because the compiler's caching of method references (`obj.Method`) is implementation-defined.

<a id="sec-type-aliases"></a>
### 27.8 Type aliases

A `type` declaration introduces an alias for any existing type:

```bezel
type UserId = int;
type ConnectionPool = Dictionary<string, Array<Connection>>;

/// <summary>Handles an HTTP response.</summary>
/// <param name="status">The HTTP status code.</param>
/// <returns>True if handled successfully.</returns>
type ResponseHandler = (status: int, body: string, elapsed: TimeSpan) -> bool;
```

A type alias is a pure synonym — it does not create a new type. `UserId` and `int` are interchangeable.

For genuinely distinct types (`UserId` and `OrderId` should not be interchangeable despite both being `int`), use a single-field record:

```bezel
record UserId(Value: int);
record OrderId(Value: int);
```

Type aliases may be declared at module level.

<a id="sec-generic-type-aliases"></a>
#### Generic type aliases

A type alias may declare type parameters:

```bezel
type Result<T> = (success: bool, value: T?, error: string?);
type Lookup<K, V> = Dictionary<K, Array<V>>;
type Predicate<T> = (T) -> bool;
type Reducer<T, TAccum> = (TAccum, T) -> TAccum;
```

Each instantiation of the alias is interchangeable with the corresponding instantiation of the aliased type. `Lookup<string, int>` and `Dictionary<string, Array<int>>` are the same type.

Generic aliases may carry constraints, written exactly as on generic methods:

```bezel
type Sorted<T> = IOrderedEnumerable<T> where T: IComparable<T>;
```

The constraint applies to the alias's parameter; uses of the alias must satisfy it.

A generic alias may not reference itself (no recursive aliases) and must resolve to a concrete type after substitution. Aliases of aliases are permitted:

```bezel
type IntPredicate = Predicate<int>;       // Predicate<int>, which is (int) -> bool
```

Type aliases — generic or not — see through to the aliased type for identity, equality, method dispatch, and overload resolution. They are a naming convenience, not a separate type kind.

---

<a id="sec-resource-management"></a>
## 28. Resource Management

Bezel has no garbage collector. Class instances are heap-allocated by `new` and must be disposed exactly once. Records, enums, and primitives need no disposal — their memory is reclaimed when their containing scope exits.

A small set of types are **runtime-managed** and exempt from manual disposal: `string`, boxed value types, and lambda closures ([§28.9](#sec-runtime-managed-types)).

For **trivial-dispose** class instances ([§28.9](#sec-runtime-managed-types)) — classes with no destructor, no owned fields, no fields of owning-inline-array type ([§15.1.1](#sec-owning-inline-arrays)), and a trivial-dispose base; this is a derived type property ([§3.6](#sec-derived-type-properties)) — abandoning a reference does not leak any user-defined cleanup, because there is no cleanup to leak. Memory for such instances is reclaimed by an implementation-defined mechanism (reference counting, region allocation, or another scheme) that runs no user code; the choice is invisible to programs because no destructor needs to run. This mechanism is *not* a garbage collector in the sense Bezel avoids: it does not scan the heap, does not pause threads, does not invoke user code, and does not affect timing of any operation a program can observe. It is the bookkeeping needed to satisfy the universal contract — every class instance with a non-trivial `Dispose()` reaches an owned location, becomes the operand of `using var`, or is explicitly disposed — without forcing programmers to write `using var _ := new MarkerObject();` for objects whose disposal does nothing.

The discard form `_ := expr;` ([§17.13](#sec-the-discard-_)) is restricted accordingly: it is rejected at compile time for class types whose `Dispose()` is non-trivial, including all `Task` types. For supervised fire-and-forget on tasks, use `Task.FireAndForget` ([§30.10](#sec-standard-library-task-helpers)).

<a id="sec-summary-of-disposal-obligations-by-type-kind"></a>
#### Summary of disposal obligations by type kind

| Type kind | Disposal required? | Mechanism |
|---|---|---|
| Classes | Yes — exactly once | `using var` ([§28.10](#sec-using-var)), `owned` fields ([§28.5](#sec-owned-fields)), explicit `.Dispose()` |
| Interface-typed locations | Yes — treated as class | `using var` or `owned` field; discard via `_ := …` is rejected unconditionally ([§17.13](#sec-the-discard-_)), even when every visible implementation is trivial-dispose, because a future implementation may carry a non-trivial destructor |
| Records | No | Stack-allocated or inline; no heap block to release |
| Enums | No | Value type; no heap block |
| Primitives | No | Value type; no heap block |
| `string` | No — runtime-managed | `Dispose()` is a no-op ([§28.9](#sec-runtime-managed-types)) |
| Boxed value types | No — runtime-managed | `Dispose()` is a no-op ([§28.9](#sec-runtime-managed-types)) |
| Function values | No — runtime-managed | `Dispose()` is a no-op ([§28.9](#sec-runtime-managed-types)) |
| Inline arrays `[N]T` (without `owned` slots) | No | Value type; stored inline in enclosing context; no separate allocation to dispose ([§15.1](#sec-inline-arrays)) |
| Inline arrays `[N owned]T?` or `[N owned]T` (with `owned` slots) | No, indirectly | Move-only value type; the array's host (class field, local) discharges by disposing each (non-null) slot ([§15.3](#sec-move-only-value-types)) |
| `Array<T>` and other borrowing collections | Yes — class types | Dispose releases backing storage; elements are borrows and are not disposed ([§28.11](#sec-collections-do-not-own-elements)) |
| `OwnedArray<T>` and other owning collections | Yes — class types | Dispose cascades through owned elements ([§28.11](#sec-collections-do-not-own-elements)) |

<a id="sec-where-the-owned-modifier-can-appear"></a>
### 28.1 Where the `owned` modifier can appear

The `owned` modifier appears in specific declaration positions. It is **admissible** in the following positions:

- **Method parameter declarations.** `method M(owned p: T)` — the parameter takes ownership of the value at the call site; the method body is responsible for discharging it before returning.
- **Return type declarations.** `method Acquire(): owned Connection` — the return value is an owned reference; the caller assumes responsibility for its disposal.
- **Field declarations on classes.** `private owned _resource: Connection` — the field stores an owned reference; the containing class's destructor disposes it.
- **Local variables via `using var`.** `using var c := Connect()` — the local is owned and is disposed at scope exit.
- **`out` parameter declarations.** `out owned p: T` — the parameter is an out-position that produces an owned reference for the caller to take.
- **Function-type parameter slots.** `(owned T) -> ()` — the parameter slot of a function type may carry `owned`. The modifier is part of the function type's identity (see [§27.1](#sec-function-types)); `(T) -> ()` and `(owned T) -> ()` are distinct types.
- **Function-type return slots.** `() -> owned Connection` — the return slot of a function type may carry `owned`. The same identity rule applies.
- **Lambda parameters.** `(owned x) => ...` or `(owned x: T) => ...` — a lambda's parameter list carries the same modifiers as the target function type expects.
- **Inline-array slot marker.** `[N owned]T` and `[N owned]T?` — the `owned` keyword placed between the size and the closing bracket marks the inline array's slots as owned (see [§15.1.1](#sec-owning-inline-arrays)). The array becomes a move-only value type ([§15.3](#sec-move-only-value-types)). This is the type-level position for owning inline arrays; an owning-inline-array field on a class does **not** additionally carry the `owned` field modifier (the slot marker already conveys the obligation).

The modifier is **not admissible** in the following positions:

- **Record fields.** Records cannot have owned fields per [§28.5](#sec-owned-fields).
- **Inline-array element types.** Writing `[N](owned T)` is not a well-formed syntax — `owned` on inline arrays appears in the slot-marker position `[N owned]T`, not on the element type. To hold owned elements in a resizable collection, use `OwnedArray<T>`.
- **Generic type arguments.** This version does not admit `owned` on generic type arguments ([§14.4](#sec-owned-as-a-generic-argument)). Use named owning collection types (stdlib [§4](Bezel_Standard_Library.md#sec-system-collections-generic)) or `OwnedTask<T>` (stdlib [§6](Bezel_Standard_Library.md#sec-system-threading)) when the use case requires an owning generic.

Function-type parameters and return types *do* admit `owned` — `(owned T) -> ()` is the function-type form of `method M(owned t: T)`, and is a distinct type from `(T) -> ()` per [§27.1](#sec-function-types). The modifier is part of the function type's identity.

The rules for how `owned` interacts with method bodies, control flow, and disposal are in the subsections that follow.

<a id="sec-the-idisposable-contract"></a>
### 28.2 The `IDisposable` contract

Every class implicitly implements `IDisposable` via a compiler-generated `Dispose()`:

```bezel
interface IDisposable
  method Dispose();
end IDisposable;
```

`Dispose()` on any class instance is idempotent: the first call performs the disposal; subsequent calls are no-ops. The compiler-generated `Dispose()` is thread-safe with respect to idempotency: concurrent calls from multiple threads will execute the disposal logic exactly once; all other concurrent callers observe the no-op path. (Within a single execution domain concurrent calls cannot arise — [§30.13](#sec-runtime-model) — and they cannot arise across domains either, since domains share no mutable state ([§30.14](#sec-multiple-domains)); the thread-safety guarantee matters where disposal is reachable from foreign threads through the FFI.)

<a id="sec-checked-disposal"></a>
#### Use after disposal; checked disposal mode

Use of an instance after its first `Dispose()` is a program error. How the error manifests depends on the *disposal-checking mode*, a per-build choice between two conformance modes:

- **Unchecked (release) mode** — use of a disposed instance is undefined behavior. This is the default.
- **Checked mode** — any instance member access on a logically-disposed instance — an instance method call (including interface and virtual dispatch), a property or indexer accessor invocation, or a field access through the reference — raises `DisposedAccessException` ([§31.4](#sec-standard-exception-types)). A further `Dispose()` call remains a no-op, not an error, in both modes.

The two modes give identical observable behavior for every program that never uses a disposed instance. A program that would raise `DisposedAccessException` in checked mode has undefined behavior in unchecked mode: unchecked mode is the release-build relaxation of the checked semantics, not a different language.

Checked mode is implementable with state the language already requires: idempotent disposal implies a per-instance disposed flag, and the split between logical disposal and physical reclamation ([§28.12](#sec-asynchronous-disposal)) gives the implementation latitude to defer reclaiming a disposed instance's memory in checked mode (quarantine, delayed free) so that the flag remains readable after disposal. The check itself is a flag test on member entry; implementations are expected to omit it entirely in unchecked builds, so unchecked builds pay no cost for the mode's existence.

Implementations should additionally emit a compile-time warning when a local variable is used after an explicit `Dispose()` call on the same variable that dominates the use site on every path — the statically-decidable subset of use-after-dispose.

`Dispose()` is not on `Object`; it is class-only. Records, enums, primitives, and inline arrays do not implement `IDisposable`.

User code does not implement `Dispose()` directly. The customization points are:

- A `destructor` for cleanup logic.
- `owned` fields for fields that should be disposed with the containing object.

<a id="sec-local-variables-ownership-and-borrows"></a>
### 28.3 Local variables, ownership, and borrows

Bezel tracks two kinds of class-typed reference at compile time:

- **Owned** — the holder is responsible for disposing the referenced object exactly once.
- **Borrowed** — the holder is not responsible for disposal; the reference is valid only for as long as some other holder (the *source*) keeps the object alive.

The `owned` keyword ([§28.5](#sec-owned-fields), [§28.8](#sec-ownership-transfer)) marks fields, parameters, and return types as owned. Local variables are not annotated; their ownership state is inferred from how they are introduced.

<a id="sec-ownership-state-of-locals"></a>
#### Ownership state of locals

For a local variable `x` of class type T (or `T?` for nullable):

- If `x` is declared `using var x := expr;`, then `x` is **owned**, and is additionally subject to scope-based disposal at the end of the enclosing block.
- If `x` is declared `var x := expr;`, the ownership state of `x` is the ownership state of `expr` (see *Producers* below). Plain `var` does not change the state.
- If `x` is declared `var x: T;` without an initializer, `x` is **uninitialized** and may not be read until assigned.

Reassignment to an existing `var x` is permitted only when the new value's ownership state matches `x`'s current state. The compiler tracks state per-variable across the body. At a control-flow join point — where two branches that produced different ownership states for the same variable merge — the result is the **meet** in the lattice owned ⊑ borrowed: a variable that is owned on one branch and borrowed on the other is rejected at the merge, because no single state is correct for the join. The owned branch had a disposal obligation that the borrowed branch did not, and silently dropping the obligation is unsound.

To compile such a merge, the program must equalize the branches before the merge. The two standard patterns are:

```bezel
// Pattern 1: equalize to borrowed — both branches bind x to a source
// that outlives the merge.
var x: Stream;                       // x will be a borrow at the merge
if condition then
  using var scratch := new Stream(); // owned: used and discharged in-branch
  scratch.WarmUp();
  primary.Reset(scratch);            // scratch's work is done here
  x := primary.Stream;               // x borrows primary (outlives the if)
else
  x := otherSource.Stream;           // x borrows otherSource (outlives the if)
end;
// At the merge, x is borrowed on both branches, and both sources
// (primary, otherSource) outlive the merge point. The using var
// scratch was discharged at its own scope end; x never borrowed it.

// Pattern 2: produce an owned value on both branches.
var x: Stream;
if condition then
  x := new Stream();
else
  x := otherSource.CloneStream();   // returns owned Stream
end;
// At the merge, x is owned on both branches.
// x must reach an owned slot or be disposed before going out of scope.
```

The lattice ordering is direction-correct: `owned` is the *stronger* state (carries an obligation), `borrowed` is the *weaker* state (carries none). The natural meet at a join would be the weaker state, but the language instead rejects the merge when branches disagree — silently demoting an owned obligation to a borrowed state at the join would leak the resource on the owned-branch path. The program must equalize the branches explicitly (the two patterns above).

When both branches produced the same state, the merge is straightforward: owned ⊓ owned = owned (with the discharge obligation carried forward); borrowed ⊓ borrowed = borrowed (with the source's lifetime constraint carried forward, requiring the two sources to agree or be reconcilable to a common ancestor).

Equalizing to borrowed (Pattern 1) does not merely require both branches to *be* borrows — it requires each branch's borrow source to outlive the merge point. Binding `x` in one branch to a borrow of a `using var` that is discharged at that branch's end does **not** equalize the merge: it produces a borrow whose source is already gone by the time `x` is read after the `if`, and the borrow-lifetime rule rejects it exactly as it rejects any other use of a borrow past its source's scope. The `using var` in such a branch must complete its own work and be discharged *within* the branch; `x` must take its post-merge value from a source that lives past the `if` (as `primary` and `otherSource` do above).

<a id="sec-producers"></a>
#### Producers

The ownership state of an expression is determined by the expression form:

| Expression | Ownership |
|---|---|
| `null` literal | Context-typed. The literal is admissible in any position whose declared type is nullable. For ownership: `null` has no value to own, so it is **compatible with either an owned target or a borrowed target**. Assigning `null` to an owned location leaves that location in the owned-currently-null state (disposal of null is a no-op). Assigning `null` to a borrowed location is the borrowed-currently-null value with no source. For a local declared `var x: T? := null;`, the local's ownership state is determined by the *next* non-null assignment to `x`: assigning an owned value makes `x` owned thereafter; assigning a borrowed value makes `x` borrowed. Before that next assignment, `x` is in a polymorphic null state that supports either subsequent transition. Locals that remain at null for their entire lifetime are statically dead-storable; the compiler may emit a warning but the program is well-formed. |
| `new T(...)` | Owned |
| Collection literal (`[1, 2, 3]`) targeting a reference type (`Array<T>`, `OwnedArray<T>`, `Set<T>`, `Dictionary<K, V>`, etc.) | Owned |
| Collection literal (`[1, 2, 3]`) targeting an inline-array type (`[N]T`, including `[N owned]T?`) | Not applicable — produces a value. For move-only `[N owned]…` targets, the value is move-only ([§15.3](#sec-move-only-value-types)). |
| Method call returning `owned T` | Owned |
| Method call returning `T` (no `owned`) | Borrowed (source: as documented by the callee; see *Factory methods* below) |
| Field access `obj.f` where `f` is `owned` | Borrowed (source: `obj`) |
| `move obj.f` where `f` is `owned` | Owned. Field is left in moved-from state and must be reassigned before the surrounding method exits ([§28.8.1](#sec-the-move-expression)). |
| Field access `obj.f` where `f` is not `owned` | Borrowed (source: `obj`) |
| Indexer access `coll[i]` | Borrowed (source: `coll`) |
| `move coll[i]` where `coll` is an owning collection that admits slot-move (heap: `OwnedArray<T>`, `OwnedDictionary<K, V>`, `OwnedBuffer<T>` for class-typed T, or a `Span<T>` view into a class-typed `OwnedBuffer<T>`; inline: `[N owned]T?`, `[N owned]T`) | Owned. Slot is left in moved-from state and must be reassigned before the surrounding method exits ([§28.8.1](#sec-the-move-expression), [§28.11](#sec-collections-do-not-own-elements)). For `OwnedBuffer<T>` / `Span<T>` with value-typed T, `move coll[i]` is not admitted (values have no ownership to transfer). `ReadOnlySpan<T>`, `ArraySpan<T>`, `ReadOnlyArraySpan<T>`, `ReadOnlyStringSpan`, and `ReadOnlyAsciiByteSpan` do not admit `move` for any T (`ReadOnlySpan<T>` and the string-backed forms have no writable indexer; the `Array<T>`-backed spans have borrowed slots with no ownership to transfer). |
| Local-variable read `x` where `x` is owned (a class-typed owned local, or a local of move-only value type per [§15.3](#sec-move-only-value-types)) | When the read appears in an owned-consuming position (assignment to an owned slot, return under an `owned` return type, an `owned` argument; for move-only locals, any assignment to an owning location), the read transfers ownership out of `x` and `x` becomes moved-from (see *Move semantics*). When the read appears in a borrow-consuming position (a non-`owned` argument, an indexer or property access, or — for move-only locals — a borrow of the value through indexing or equality comparison), `x` retains ownership and the value is borrowed for the consuming operation. |
| Local-variable read `x` where `x` is borrowed | Borrowed (source: same as `x`'s source) |
| Parameter `p` declared `owned p: T` | Owned |
| Parameter `p` (no modifier) | Borrowed (source: caller) |
| `this` | Borrowed (source: caller) |
| Function call producing an ordinary value type, `string`, or runtime-managed type | Not applicable ([§28.9](#sec-runtime-managed-types)) |
| Function call producing a move-only value type (an owning inline array `[N owned]T?` or `[N owned]T`) | Owned. The result is a move-only value ([§15.3](#sec-move-only-value-types)); the caller takes ownership and must discharge by binding into a local (auto-disposed at scope end via the move-only-local rule), into an owning-inline-array slot, or into a class field of move-only type. |
| `x as T` | Borrowed (source: same as `x`'s source if `x` is borrowed; same source as `x` if `x` is owned). The cast does not move from `x`; `x` retains its ownership state. The result is nullable. See [§17.4](#sec-the-is-is-not-and-as-operators) for the rationale. |
| Captured borrow inside a closure | Borrowed (source: same as the captured local's source). The closure value itself is runtime-managed; the captured borrow's lifetime is tracked separately. |
| Captured owned local inside a closure | Borrowed (source: the local's enclosing scope). Capturing does not transfer ownership; the closure cannot outlive the local's scope. See [§28.9](#sec-runtime-managed-types). |

<a id="sec-factory-methods-and-sourceless-returns"></a>
#### Factory methods and sourceless returns

When a method returns a class type `T` (no `owned`) and the call has no identifiable borrow source — because the method is `static`, or because the return is documented as a freshly-allocated value rather than a reference into existing state — the return is treated as **owned** by the caller. The caller must discharge the ownership obligation (store in an `owned` field, bind with `using var`, pass to an `owned` parameter, or explicitly dispose).

This rule applies to static factory methods (`Stopwatch.StartNew()`, `File.ReadAllLines(...)`, `string.Split(...)` returning `Array<string>`, LINQ materialization methods like `ToArray()` returning `Array<T>`) and to any instance method whose documentation states that the return is freshly allocated. The `owned` return-type annotation remains the preferred way to communicate ownership transfer across method boundaries ([§28.8](#sec-ownership-transfer)); the sourceless-return rule exists as a safety net for methods that produce fresh values but omit the annotation.

When the method has an identifiable borrow source — an instance method returning state from its receiver, or a method whose documentation names the source — the return is borrowed from that source, and the caller has no disposal obligation.

The compiler determines the borrow source as follows: for an instance method, the default source is the receiver; for a static method, there is no default source. When the compiler cannot identify a source for a non-`owned` class-typed return from a static method, it treats the return as owned and emits a warning recommending that the callee add `owned` to its return type. Implementations are encouraged to add `owned` annotations to static factory methods proactively.

**Iterator methods are exempt from this warning.** A method whose body uses `yield return` or `yield break` (synchronous or async, returning `IEnumerable<T>`, `IEnumerator<T>`, `IAsyncEnumerable<T>`, `IAsyncEnumerator<T>`, `OwnedEnumerable<T>`, `OwnedEnumerator<T>`, `OwnedAsyncEnumerable<T>`, or `OwnedAsyncEnumerator<T>` per [§29.3](#sec-iterator-methods)) is a factory by construction — its return is the freshly-constructed state machine — and the natural return type is the iterable interface, not `owned IEnumerable<T>` (or `owned OwnedEnumerable<T>`). The factory rule applies to iterator-method return values exactly as it does to other sourceless static returns: callers must discharge the ownership obligation (typically via the for-loop's implicit `using var __source` binding). The warning is suppressed because adding `owned` to every iterator return type would be redundant noise. The standard library's iterator-returning methods (`IEnumerable<T>` extensions like `Where`, `Select`, etc., and the owning-collection `Drain()` methods returning `OwnedEnumerable<T>`) follow this convention.

<a id="sec-move-semantics-for-owned-locals"></a>
#### Move semantics for owned locals

Reading an owned local `x` in a position that requires an owned value (assigning to an owned slot, returning under an `owned` return, passing as an `owned` argument) **transfers ownership out of `x`**. After the transfer, `x` is in the **moved-from** state: it may not be read again until reassigned, and it has no disposal obligation. Reading an owned local in a position that accepts a borrow leaves `x` unchanged.

```bezel
public method Wrap(s: Stream): owned Wrapper
  using var copy := s.Clone();      // copy is owned + scope-bound
  return new Wrapper(copy);          // ERROR: copy is using-var, cannot move out
end Wrap;

public method Wrap2(owned s: Stream): owned Wrapper
  return new Wrapper(s);             // OK: s is moved into Wrapper's constructor
                                     // s is now in moved-from state
end Wrap2;
```

A `using var` is owned but is additionally bound to its scope: it cannot be moved out of, only borrowed from. To transfer a `using var`'s value out, the programmer must either reassign the contents (via an owning method that `take`s the underlying object and replaces the `using var`'s contents — not provided in this version) or restructure to introduce the value as a non-`using var` owned local.

<a id="sec-borrow-lifetime-rules"></a>
#### Borrow lifetime rules

A borrowed value's lifetime is the lifetime of its *source*. The compiler enforces:

- A borrowed local may not be returned from a method whose return type is `owned`.
- A borrowed local may not be returned from a method at all unless the return type is the same kind of borrow with a clearly-defined source (in this version, that means: receiver-borrows from extension methods, value-typed returns, and runtime-managed returns).
- A borrowed local may not be assigned to any field of any object — neither owned nor non-owned. Fields hold either owned references or runtime-managed references; class-typed fields that hold a borrow are not expressible in this version.
- A borrowed local may not be captured by a closure that may outlive the borrow's source. For closures that are immediately invoked or whose lifetime is statically bounded by the borrow's source (a `using var` local of function type, an extension method's parameter), capture is permitted.
- The same capture-and-escape rule applies to **method references** ([§27.6](#sec-method-references)) that bind an instance receiver: `obj.Method` produces a function value whose captured environment includes `obj`. If `obj` is a borrow, the resulting function value is subject to the closure-escape rules above as if `obj` had been captured by an explicit lambda. The function value's runtime-managed status ([§28.9](#sec-runtime-managed-types)) governs the *function value's own* lifetime; it does not relax the lifetime constraints on borrows captured by the function value.

A borrowed value may be passed as a non-`owned` argument to another call: the callee further borrows for the duration of the call.

<a id="sec-owned-local-discharge-requirement"></a>
#### Owned-local discharge requirement

An owned local must reach an owned slot before it goes out of scope, or be explicitly disposed. The slots that count as discharging the obligation:

- Storage in an `owned` field.
- Return under an `owned` return type.
- Move into an `owned` parameter at a call site.
- Move into another `using var`/owned local that takes over the obligation.
- An explicit `.Dispose()` call.

The compiler errors when an owned local is dropped from a *normal-exit* control-flow path without reaching one of the above. The error is the same as the [§28.8](#sec-ownership-transfer) rule for raw `new` expressions, generalized.

**Exception-path discharge.** Owned locals and owned parameters that are live when an exception propagates out of their enclosing block are disposed implicitly during stack unwinding. The compiler treats every owned local and owned parameter as if it were wrapped in an implicit `try`/`finally` whose `finally` disposes the value if it is still owned (i.e., not already moved-from and not already explicitly disposed). This guarantees that exceptions do not produce leaks even though the user wrote a plain `var` rather than `using var`, or accepted an `owned` parameter rather than re-binding it. The discharge-requirement rule above only constrains *normal* exit: on a normal return or fall-through from the enclosing block, the owned local or parameter must reach a discharge point statically; on exception exits, the implicit auto-dispose runs.

This rule is what makes plain `var` owned locals safely usable as accumulators that are eventually moved out by `return` or by an `owned` argument. The intent of a plain `var` is "I will discharge this explicitly somewhere"; the intent of a `using var` is "auto-dispose at scope end, never move out." On exception paths, both behave identically — the value is disposed by the implicit cleanup pass. The distinction matters only on normal-exit paths: `using var` is non-movable; plain `var` is movable but must reach a discharge point.

The implicit cleanup ordering follows reverse declaration order across multiple owned locals in the same scope, mirroring the destructor's field-disposal rule ([§28.4](#sec-the-destructor)). When the exception passes through nested scopes, the inner scope's locals are disposed before the outer scope's, in the same way `using var` cleanups nest.

The implicit cleanup does **not** apply to:
- Locals that are already in moved-from state at the point the exception unwinds past them.
- Locals that were already explicitly disposed before the throwing operation.
- Borrows (locals not in the owned state); these have no disposal obligation.
- Value-typed and runtime-managed-typed locals; these have no `Dispose` to call.

A user who wants to perform additional cleanup on the exception path (logging the partial result, performing a partial-state recovery) writes an explicit `try`/`finally` or `try`/`catch`/`throw` around the relevant operations. The implicit auto-dispose composes with explicit `finally` blocks: the explicit `finally` runs first (with the local still in scope), then the implicit auto-dispose runs as the scope unwinds further.

<a id="sec-diagnostics"></a>
#### Diagnostics

The compiler must produce error messages that name the variable, its current ownership state, and the rule violated. Recommended messages:

- *"Owned value in `x` would be dropped here without disposal."* (When an owned local goes out of scope undisposed.)
- *"Borrow held by `x` would outlive its source `y`."* (When a borrow escapes.)
- *"Owned value in `x` was already moved out at line N."* (When a moved-from local is read.)
- *"Cannot move out of `using var` `x`; `using var` is scope-bound."* (When a `using var` is moved.)

These rules are sufficient to ensure that every owned class instance is disposed exactly once at runtime, that every borrow refers to a live object, and that no owned reference is silently leaked.

<a id="sec-the-destructor"></a>
### 28.4 The `destructor`

A class may declare at most one destructor. Its body runs exactly once, the first time `Dispose()` is called.

```bezel
class FileStream
  private _handle: IntPtr;

  public constructor(path: string)
    using var cpath := CString.From(path);
    _handle := NativeOpen(cpath);
  end constructor;

  destructor
    if _handle <> IntPtr.Zero then
      NativeClose(_handle);
      _handle := IntPtr.Zero;
    end;
  end destructor;
end FileStream;
```

The compiler generates a public, idempotent `Dispose()` that:

1. Runs the destructor body, if any.
2. Disposes all owned-bearing fields in reverse declaration order. This covers fields marked `owned` (class- or interface-typed; the field's value's `Dispose()` is called) and fields of owning-inline-array type (`[N owned]T?` or `[N owned]T`; each non-null slot's `Dispose()` is called per [§15.1.1](#sec-owning-inline-arrays)). Inline-array fields without `owned` slots, value-typed fields, and runtime-managed fields require no disposal.
3. Chains to the base class's `Dispose()`.

After step 3 has propagated to the root of the hierarchy, the runtime reclaims the object's memory by an implementation-defined mechanism (reference counting, region allocation, or another scheme; see Design Rationale [§4](Bezel_Design_Rationale.md#sec-memory-management)). Memory reclamation is not part of the user-observable `Dispose()` contract — programs cannot rely on the precise moment at which an object's storage becomes available for reuse.

Destructors run in subclass-to-base order. `Dispose()` cannot be overridden; the customization point is `destructor`.

There is no `disposing` parameter, no manual `_disposed` flag, no `base.Dispose()` call.

<a id="sec-owned-fields"></a>
### 28.5 Owned fields

A field marked `owned` belongs to the containing object exclusively:

```bezel
class BankAccount
  private owned _transactions: Array<Transaction>;       // non-nullable, owned
  private owned _cache: TransactionCache?;               // nullable, owned
  private _owner: Customer;                              // back-reference, not owned
end BankAccount;
```

The `owned` modifier:

1. **Auto-disposes** the field when the containing object is disposed (after the destructor body runs, in reverse declaration order; nullable fields are checked first).
2. **Prevents leaks** by rejecting code that would let an owned reference escape the containing object.

Restrictions:

- Only fields whose type is a **class type or an interface type** may be `owned`. Interface-typed owned fields are admitted because runtime dispatch resolves disposal to the underlying class's `Dispose()`; an `owned IConnection` field disposes whichever class implementation it currently holds.
- `string` fields may not be `owned`. String memory is runtime-managed.
- Function-typed fields may not be `owned`. Function values are runtime-managed ([§28.9](#sec-runtime-managed-types)).
- Inline-array fields (`[N]T`, `[N]T?`) without the `owned` slot marker may not be `owned`. The array's slots hold borrows (or value-typed copies); there is no per-slot ownership to manage.
- Inline-array fields with the `owned` slot marker (`[N owned]T`, `[N owned]T?`) are move-only value types that carry owned slots ([§15.3](#sec-move-only-value-types)). They are admissible as class fields *without* a separate `owned` modifier on the field; the inline array's own `owned` slot marker already signals that the class's destructor must dispose each non-null slot. Writing `private owned slots: [16 owned]Connection?;` is rejected as a double-marking error — the field modifier is omitted because the type-level marker already conveys the obligation.
- Local variables are not marked `owned`; use `using var`.
- Records may not have `owned` fields, and may not have inline-array fields with the `owned` slot marker (records support only copy semantics, not move semantics; see [§15.3](#sec-move-only-value-types)).

**Generic class fields.** When a generic class declares `private owned _slot: T` (where T is a type parameter), the restriction "only fields of class or interface types may be `owned`" is checked at *instantiation*. A `where T: class` constraint admits classes, interfaces, `string`, and function types ([§14.2](#sec-constraints)) — broader than what `owned` fields admit. Instantiating the generic with a type that satisfies `where T: class` but is neither a class nor an interface (`string`, function types) is a compile error at the instantiation site, with a message naming the rejected type. The generic body itself type-checks under the assumption that T is a class-or-interface type; programmers writing such generics should use `where T: class` and document that runtime-managed reference types are not admitted, or use a more specific constraint like `where T: IDisposable` (which automatically excludes records and runtime-managed types per [§14.2](#sec-constraints)'s interface-conformance rule).

A class with only owned-bearing fields (`owned` class/interface fields and/or owning-inline-array fields) and no native resources needs no destructor — the compiler-generated cleanup is sufficient.

<a id="sec-leak-prevention"></a>
### 28.6 Leak prevention

The compiler enforces:

- An owned field's value may not be returned from a method unless the return type is also declared `owned` (see [§28.8](#sec-ownership-transfer)).
- An owned field's value may not be assigned to a non-owned field of any other class.

```bezel
public method GetTransactions(): Array<Transaction>
  return _transactions;          // ERROR: cannot leak owned field
end GetTransactions;
```

Passing an owned field as an argument is permitted; the callee borrows for the duration of the call and may not store the reference in a long-lived location.

<a id="sec-methods-returning-this"></a>
#### Methods returning `this`

A method that returns `this` returns a borrow of the receiver — the return type is the class type, not `owned`. The caller receives a reference whose lifetime is bounded by the receiver's lifetime. This enables method chaining:

```bezel
using var sb := new StringBuilder();
sb.Append("hello").Append(" ").Append("world");    // each Append returns a borrow of sb
```

The chained calls all operate on the same object. The returned borrow may not be assigned to an `owned` field or returned as `owned` — it is the receiver under a different name. The compiler infers the borrow from the method's non-`owned` return type.

When a chain originates from a `new` expression — `new StringBuilder().Append("a").Append("b")` — the owned value produced by `new` is the root of the chain. The compiler treats the overall chain expression as owned (its root is a `new`). The borrow-returning intermediate calls do not lose the root's ownership; the chain as a whole must reach an owned location or `using var`, just as any other `new` expression must.

<a id="sec-reassigning-owned-fields"></a>
### 28.7 Reassigning owned fields

Assignment to an owned field is a plain assignment. The compiler does not insert automatic cleanup of the previous value. The programmer disposes the previous value first:

```bezel
public method Resize(newSize: int)
  _buffer.Dispose();
  _buffer := new Array<byte>(newSize);
end Resize;
```

For nullable owned fields, guard the disposal:

```bezel
if _cache <> null then
  _cache.Dispose();
end;
_cache := newCache;
```

Forgetting to dispose the previous value leaks memory but cannot dangle (the leak-prevention rules ensure no other reference exists).

<a id="sec-disposers-are-expected-not-to-throw"></a>
#### Disposers are expected not to throw

The pattern `field.Dispose(); field := new T(...);` is exception-unsafe if `Dispose` throws: the field is left in disposed state but still holds a reference whose use is undefined behavior ([§28.2](#sec-the-idisposable-contract)). For non-nullable owned fields, recovery from a throwing disposer is essentially impossible — the field cannot be left null and the new value has not been constructed.

Disposers are therefore expected not to throw under any condition the program is expected to handle. Standard library disposers do not throw under normal operation; user-written destructors should follow the same convention. A disposer that throws should be considered a programming error. Methods that need to perform fallible cleanup should expose a separate `Close` or `Flush` method, leave `Dispose` to handle the unrecoverable cleanup, and let the destructor swallow exceptions from late cleanup with a logged warning.

<a id="sec-compiler-warning-on-undisposed-overwrite"></a>
#### Compiler warning on undisposed overwrite

Implementations should emit a warning when a write to an `owned` field is not preceded by a `Dispose()` call (or, for nullable owned fields, by a null check guarding a `Dispose()`) on the same field within the same method, unless the field is provably null or in moved-from state at the write site. A field is provably in moved-from state when an earlier `move obj.f` ([§28.8.1](#sec-the-move-expression)) without an intervening reassignment dominates the write site on every path. The warning highlights the leak that the rule above acknowledges as possible. False positives are addressable by an explicit `field.Dispose();` immediately before the write, even when the programmer knows the previous value to be null — the explicit dispose call is idempotent ([§28.2](#sec-the-idisposable-contract)).

<a id="sec-reassigning-owned-locals"></a>
#### Reassigning owned locals

The same rules apply to owned locals declared with plain `var`. Assignment to an owned local is a plain assignment; the compiler does not insert automatic cleanup of the previous value. The programmer either disposes the previous value first (`x.Dispose(); x := newValue;`), moves the previous value out (`y := x; x := newValue;`, leaving `x` moved-from before the reassignment), or relies on the local being in moved-from state already (which is automatic after a prior `move`, `using var` transfer, or owned-argument call). The same compiler-warning rule of [§28.7](#sec-compiler-warning-on-undisposed-overwrite) applies: an undisposed-overwrite warning is emitted unless a visible `Dispose()` precedes the reassignment, or the local is provably in moved-from or null state at the assignment site. `using var` locals are not reassignable at all ([§28.10](#sec-using-var)), so this rule covers only plain `var` owned locals.

<a id="sec-ownership-transfer"></a>
### 28.8 Ownership transfer

`owned` may appear on a parameter or return type to mark ownership transfer across method boundaries:

```bezel
public method TakeOwnership(owned resource: Stream)
  _resource := resource;     // moves into owned field; previous _resource
                             // must have been disposed first per §28.7
end TakeOwnership;

public static method NewBuffer(size: int): owned Array<byte>
  return new Array<byte>(size);   // owned by virtue of new, returned as owned
end NewBuffer;
```

When a parameter is `owned`, the caller relinquishes ownership and may not use the argument after the call. When a return type is `owned`, the caller receives ownership and is responsible for disposal.

The owned-field assignment in `TakeOwnership` assumes `_resource` has been disposed (or was never set), as required by [§28.7](#sec-reassigning-owned-fields). When a class needs both "set initial resource in constructor" and "replace resource later," the constructor handles the first assignment and the replacement method disposes the existing value before the assignment:

```bezel
public method ReplaceResource(owned resource: Stream)
  _resource.Dispose();
  _resource := resource;
end ReplaceResource;
```

Moving an owned field's contents *out* of the field uses the `move` expression form ([§28.8.1](#sec-the-move-expression)). `move x.f` reads the value of an owned field as an owned value, leaving the field in *moved-from* state; the field must be reassigned before the method exits. The pattern for "atomically take this field's contents and return them as owned" is:

```bezel
public method ReplaceAndReturn(): owned Stream
  var fresh := new Stream();         // build replacement first
  var old := move _resource;          // take from field
  _resource := fresh;                  // restore field
  return old;                          // return the taken value
end ReplaceAndReturn;
```

The `move` expression is the only way to read an owned field's value as an owned value; all other reads of an owned field produce a borrow ([§28.3](#sec-local-variables-ownership-and-borrows)). The compiler enforces the field-restoration rule statically: every code path from the move site to the method's exit must reassign the field. See [§28.8.1](#sec-the-move-expression) for the full rule.

A `new` expression produces an owned value. Its result must reach an owned location (`owned` field, `using var`, `owned` parameter, `owned` return) within the same method, or be explicitly disposed before the method returns. The compiler warns when a `new` expression's result is silently discarded.

Implicit boxing ([§8.6](#sec-boxing)) is **not** subject to this rule. When a value type is boxed to be used where `Object` or an interface is expected, the boxed wrapper is a runtime-managed value ([§28.9](#sec-runtime-managed-types)), not an owned `new`-result. The boxing itself is not a `new` expression and produces no disposal obligation; `Console.WriteLine(42)` does not need to discharge anything.

<a id="sec-the-move-expression"></a>
### 28.8.1 The `move` expression

`move x.f` reads the value of an owned field `x.f` as an owned value, transferring ownership to the read site and leaving the field in *moved-from* state. `move coll[i]` is the analogous form for an owned-element collection slot ([§28.11](#sec-collections-do-not-own-elements)); the rules below are written for the field case and apply to the collection-slot case with "field" read as "slot." Local variables already support the move-on-read semantics described in [§28.3](#sec-local-variables-ownership-and-borrows) and do not use the `move` keyword. Static fields are not eligible — their lifetime is process-bound, and moving out of them would create the same restoration obligation across method boundaries that the rules below specifically scope to a single method.

The `move` expression's static type is the slot's element type (the `T` in `owned T`); the result is owned and must be discharged like any other owned value.

<a id="sec-the-field-restoration-rule"></a>
#### The field-restoration rule

> Within a method body that performs `move x.f`, every control-flow path from the move site to the method's exit must execute an assignment `x.f := value;` between the move and the exit. The compiler enforces this statically; failure to satisfy it is a compile error.

The exit paths considered include normal return, fall-through (for `()`-returning methods), `throw`, `yield break` (in iterator methods), and any other transfer of control that leaves the method body. The rule applies to every reachable exit; an unreachable exit (one the compiler proves cannot occur) does not contribute an obligation.

The error message identifies the move site, the unrestored exit path, and the three standard remedies: assign a value on the offending path; move the move into the branch where reassignment is reachable; or, if the field should sometimes hold no value, declare it nullable and assign `null` after the move.

```bezel
// ERROR: field _resource was moved at line 2 and not reassigned
//        on the path through the false branch of `if replace` (line 3).
public method Wrong(replace: bool): owned Stream
  var old := move _resource;
  if replace then
    _resource := new Stream();
  end;
  return old;
end Wrong;
```

The fix is to reassign on every path:

```bezel
public method Right(replace: bool): owned Stream
  var fresh := new Stream();
  var old := move _resource;
  _resource := fresh;
  return old;
end Right;
```

Or to scope the move to the branch where reassignment is mandatory:

```bezel
public method Right2(replace: bool): owned Stream?
  if not replace then return null; end;
  var fresh := new Stream();
  var old := move _resource;
  _resource := fresh;
  return old;
end Right2;
```

<a id="sec-exception-paths-are-not-exempt"></a>
#### Exception paths are not exempt

A throwable expression between the move and the reassignment introduces an exit path on which the reassignment has not run. If the destructor on `this` is reached via that exception path, it would observe the field in moved-from state. The field-restoration rule applies to exception edges as well as normal edges:

```bezel
// ERROR: if DangerousFactory throws, _resource is moved but never reassigned;
//        the exception edge is an unrestored exit.
public method WrongOrder(): owned Stream
  var old := move _resource;
  var fresh := DangerousFactory();
  _resource := fresh;
  return old;
end WrongOrder;
```

The fix is to perform any throwable construction *before* the move, leaving only non-throwable assignments and the return between the move and the exits:

```bezel
public method RightOrder(): owned Stream
  var fresh := DangerousFactory();   // may throw — but no move yet
  var old := move _resource;          // cannot throw
  _resource := fresh;                  // cannot throw (assignment, not call)
  return old;                          // cannot throw
end RightOrder;
```

The general rule — *build before take* — is the resource-management discipline this rule encodes. It is the same discipline that motivates strong exception-safety guarantees in C++ RAII code and that drives the "build a complete replacement, then commit it" pattern in transactional code.

For the purpose of the analysis, the following expressions are non-throwable:

- assignments (including compound assignments) where neither the LHS nor the RHS is a throwable expression
- field reads
- local-variable reads
- literals
- `move x.f` itself (the move is a state transition, not a runtime check)
- `null`-coalescing where neither operand is throwable

Method calls; indexer accesses (on inline arrays `[N]T`, on resizable collection classes such as `Array<T>` whose indexer can throw `IndexOutOfRangeException` or `KeyNotFoundException`, and on any user-declared indexer); division and modulo (which can throw `DivideByZeroException`); arithmetic on overflow-checked types; explicit casts in function-call form (e.g., `short(x)` and `int(d)`, which can throw `OverflowException` for numeric narrowing per [§5.2](#sec-explicit-conversions) or `NullReferenceException` for nullable-to-non-nullable casts per [§5.3](#sec-nullable-casts)); explicit enum casts from integer values (which throw `OverflowException` for out-of-range values per [§5.4](#sec-enum-casts)); and `await` are throwable. The `!` null-forgiving operator is throwable when the target is null.

The `as` operator is **not** throwable: a failed `as` cast returns `null` rather than raising ([§12.7](#sec-type-tests-is-is-not-as), [§17.4](#sec-the-is-is-not-and-as-operators)). Code that uses `as` between a move and the field-restoration point does not introduce an unrestored exception edge.

<a id="sec-loops-and-conditional-control-flow"></a>
#### Loops and conditional control flow

Loops that may execute zero times do not satisfy the rule:

```bezel
// ERROR: the while loop may run zero times, leaving _resource moved
//        but unreassigned.
public method WrongLoop(): owned Stream
  var old := move _resource;
  while SomeCondition() do
    _resource := MakeStream();
    break;
  end;
  return old;
end WrongLoop;
```

The fix is the same as the conditional case: ensure the reassignment is unconditional. Restructure so the reassignment dominates the exits:

```bezel
public method RightLoop(): owned Stream
  var fresh := MakeStream();
  while SomeCondition() do
    fresh := MakeStream();   // refine fresh in the loop
  end;
  var old := move _resource;
  _resource := fresh;
  return old;
end RightLoop;
```

The analysis reuses the control-flow graph and dominance frontiers that the existing definite-assignment analysis builds for non-nullable locals ([§7.9](#sec-definite-assignment-for-non-nullable-fields)); no new machinery is required.

<a id="sec-constructors-and-destructors"></a>
#### Constructors and destructors

Inside a constructor body, the field has no value to move until it has been assigned. A `move x.f` before the field's first assignment is a definite-assignment violation, the same rule as for any other read of an unassigned non-nullable slot ([§7.9](#sec-definite-assignment-for-non-nullable-fields)). After the first assignment, `move` is permitted, with the same field-restoration rule applying to constructor exit paths.

Inside a destructor body, the field is being read for final disposal. `move x.f` inside a destructor takes the field's value out, which causes the compiler-generated auto-dispose pass for that field to skip it (the field is in moved-from state and has nothing to dispose). The taken value must still be discharged through one of the standard mechanisms — assigned to a local with `using var`, passed as `owned` to a helper, or explicitly disposed. The destructor's exit obligation is satisfied if the moved value reaches a discharge point; the field-restoration rule does *not* require reassignment inside a destructor, because the object is being torn down and no future read of the field will occur.

<a id="sec-interaction-with-using-var"></a>
#### Interaction with `using var`

`using var u := move x.f;` is the canonical pattern for "take the field's value out and consume it scope-bound." The `using var` discharges the moved value at scope end; the field-restoration rule still applies at the surrounding method's exits.

```bezel
public method ProcessAndReplace()
  var fresh := new Stream();
  using var old := move _resource;        // moved into using var; disposed at scope end
  _resource := fresh;                      // restore
  // ... use old ...
  // at scope end: old.Dispose()
end ProcessAndReplace;
```

The using-var consumes the moved value; the explicit reassignment satisfies the field-restoration rule. No swap helper is needed.

<a id="sec-detaching-via-a-nullable-field"></a>
#### Detaching via a nullable field

A class that wants "take the resource away and leave the field empty" declares the field nullable and assigns `null` after the move:

```bezel
private owned _resource: Stream?;        // nullable

public method Detach(): owned Stream?
  if _resource = null then return null; end;
  var taken := move _resource!;          // move out the non-null contents
  _resource := null;                      // restore (to null)
  return taken;
end Detach;
```

The reassignment to `null` satisfies the field-restoration rule; the destructor's auto-dispose pass treats the null field correctly per [§28.5](#sec-owned-fields).

For non-nullable owned fields, "leave the field empty" is not expressible — by design, a non-nullable field's type contract says a value is always present. Code that needs sometimes-empty semantics declares the field nullable.

<a id="sec-interaction-with-overload-resolution-and-parameter-passing"></a>
#### Interaction with overload resolution and parameter passing

`move x.f` produces an owned value, so it is suitable as the argument to an `owned` parameter:

```bezel
public method TransferToHelper()
  var fresh := new Stream();
  TakeOwnership(move _resource);          // owned argument; helper consumes
  _resource := fresh;
end TransferToHelper;
```

The argument expression `move _resource` runs as part of argument evaluation. The field is moved-from from the moment of the move expression's evaluation; the reassignment to `_resource` after the call satisfies the rule. If `TakeOwnership` itself throws, the field has been moved but not reassigned — an exception-edge violation. As with all exception-path concerns, the fix is to construct the replacement and reassign before the call:

```bezel
public method TransferToHelperSafe()
  var fresh := new Stream();
  var taken := move _resource;
  _resource := fresh;                      // restore before the call that may throw
  TakeOwnership(taken);                    // owned argument, no field state at risk
end TransferToHelperSafe;
```

This is the same "build then take then restore" discipline, with the call moved to after the restore.

<a id="sec-runtime-managed-types"></a>
### 28.9 Runtime-managed types

The following types are exempt from manual disposal. Their `Dispose()` is a no-op (or, for types that are not classes and therefore have no `Dispose()` at all, the type's lifetime is managed by the runtime without user involvement).

- `string` — pervasive; created by literals, interpolation, concatenation, and library methods.
- Boxed value types — created implicitly when a value type is used where `Object` (or an interface) is expected.
- **Function-typed values** — values of any function type `(T1, ..., Tn) -> R`, regardless of whether they capture. This includes lambda closures (capturing or not), method references (`obj.Method` and `Type.StaticMethod`), and any value whose static type is a function type. Function-typed fields, parameters, and locals are not subject to `owned` annotations and do not appear in `using var`.

  Runtime management governs the *function value itself* — the heap-allocated closure object that backs the function value. It does not relax the lifetime rules on values the closure captures. A lambda or method reference that captures a borrowed class reference is subject to the borrow-escape rules in [§28.3](#sec-local-variables-ownership-and-borrows): the function value may be runtime-managed, but if it outlives the captured borrow's source, the program is rejected. The compiler tracks captured borrows separately from the function value's own lifetime.

  Capturing an *owned* class-typed local does **not** transfer ownership into the closure. Instead, the capture is treated as a borrow whose source is the local's enclosing scope; the closure may not outlive that scope. The original `using var` (or other owned location) retains the disposal obligation. This rule is what makes function-typed values runtime-managed without contradicting the no-GC promise: a closure cannot extend the lifetime of an owned class instance, so reclaiming the closure never leaves a destructor un-run. Programs that genuinely need to extend an owned class's lifetime past a closure boundary should restructure to keep the owned location in scope (for example, by storing the value in an owned field of a longer-lived object) or use `Task.FireAndForget` for the specific case of fire-and-forget tasks.

  Capturing a *move-only* local (an owning inline array, [§15.3](#sec-move-only-value-types)) follows the same rule: the closure captures a borrow of the move-only value whose source is the local's enclosing scope. The closure may not outlive that scope. A move-only capture cannot be a copy (copying is forbidden for move-only types) and cannot be a move (the local is still in scope and accessible via its name; moving into the closure would leave the local in moved-from state in surprising ways). The borrow form is the only consistent option. Reads of slots through the captured borrow are admitted; `move arr[i]` through the captured borrow is not (the closure does not own the array). Programs that need to consume slot contents through a closure must arrange for the closure to take an owned-parameter argument and invoke it with `move`d slots from outside.

Note that **inline arrays** `[N]T` (see [§15.1](#sec-inline-arrays)) are not on this list. Inline arrays are value types, stored inline in their enclosing context; they have no separate allocation to reclaim and no disposal contract independent of their host. Inline arrays with owned slots (`[N owned]T?`) are move-only value types ([§15.3](#sec-move-only-value-types)) that participate in the ownership system as part of their host's lifecycle. Earlier revisions of this specification described a runtime-managed fixed-size-array form `T[]` here; that form has been retired in favor of inline arrays and the heap arrays `Array<T>` / `OwnedArray<T>` ([§15.2](#sec-heap-arrays)).

The compiler does not warn about undisposed runtime-managed values and does not require them to appear in `using var` or `owned` declarations.

The implementation of runtime-managed types' lifecycle (reference counting, region allocation, or other) is not part of this specification.

<a id="sec-trivial-dispose-classes"></a>
#### Trivial-dispose classes

A class whose `Dispose()` is provably a no-op — it has no destructor, no `owned` fields, no fields of owning-inline-array type (`[N owned]T?` or `[N owned]T`, [§15.1.1](#sec-owning-inline-arrays)), and its base class (if any, other than `Object`) is itself trivial-dispose — is a **trivial-dispose class**. Trivial-dispose classes have weaker discharge requirements than other classes:

- A `new` expression of a trivial-dispose class type whose result is not stored in an `owned` location or `using var` does *not* produce the standard ownership-discharge warning. (Earlier revisions emitted an informational warning here; v0.66 drops the warning entirely. There is nothing to discharge.)
- The discard statement `_ := expr;` ([§17.13](#sec-the-discard-_)) is legal when `expr`'s static type is a trivial-dispose class. It is a compile error for class types that are not trivial-dispose.
- Memory for trivial-dispose instances is reclaimed by an implementation-defined mechanism ([§28](#sec-resource-management)'s preamble) that runs no user code.

`using var` on a trivial-dispose class is permitted and has no observable effect other than scope limitation.

Standard library classes that are trivial-dispose include `Random` ([§2.7](Bezel_Standard_Library.md#sec-random) of the standard library) and the singleton `Encoding` subclass instances ([§3.2](Bezel_Standard_Library.md#sec-encoding) of the standard library). User-defined classes with no destructor, no owned fields, and no owning-inline-array fields are automatically trivial-dispose.

<a id="sec-trivial-dispose-for-generic-class-instantiations"></a>
#### Trivial-dispose for generic class instantiations

For a generic class `C<T>` (or `C<T1, T2, ...>`), the trivial-dispose check is performed on the **instantiation** rather than on the generic declaration. The compiler substitutes the actual type arguments and applies the rules above to the resulting concrete class.

```bezel
public class Box<T>
  public Value: T;
  // No destructor, no owned fields. Box<T> is trivial-dispose for any T.
end Box;

public class OwnedBox<T> where T: class
  public owned Value: T;
  // Has an owned field. OwnedBox<T> is NOT trivial-dispose for any T.
end OwnedBox;

_ := new Box<int>();              // OK (trivial-dispose)
_ := new Box<MyClass>();          // OK (trivial-dispose; Box doesn't own its T)
_ := new OwnedBox<MyClass>();     // ERROR (owned T field; non-trivial-dispose)
```

Per-instantiation analysis means trivial-dispose status is computed *after* type substitution, so a generic class's trivial-dispose status follows from how it uses T, not from T's own properties. The standard collection types — `Array<T>`, `Dictionary<K, V>`, `Set<T>`, `LinkedList<T>`, `Stack<T>`, `Queue<T>` — are trivial-dispose for every non-owned choice of type argument because they do not own their elements ([§28.11](#sec-collections-do-not-own-elements)). A program that holds `Array<MyClass>` may discard the array (the elements remain owned by their original sources, unaffected by the array's reclaim); a program that holds `OwnedBox<MyClass>` may not (the box's owned T field requires disposal).

The owned-element variants of the standard collection types — `OwnedArray<T>`, `OwnedDictionary<K, V>`, `OwnedSet<T>`, etc. — are **not** trivial-dispose: they own their elements and disposal cascades through to the elements. A program that holds `OwnedArray<MyClass>` *must* discharge the array's ownership through one of the standard mechanisms; the implicit reclaim is not sufficient because element disposal needs to run. `OwnedBuffer<T>` is similarly non-trivial-dispose: for class-typed T, its destructor cascades through every non-null slot; for value-typed T, the cascade is empty but the buffer's allocation still must be released.

The rule does not require the generic to be parameterized only by type arguments that are *themselves* trivial-dispose. `Array<MyClass>` is trivial-dispose even if `MyClass` is not, because the array's own surface contains nothing that needs disposal — `MyClass` instances are owned elsewhere, and the array's reclaim doesn't dispose them. The key question is **does the generic class itself need to do disposal work**, not whether its element type does.

A generic class with conditional structure based on the type argument — for example, an `OwnedIfClass<T>` that has an `owned T` field only when T is a class type — would have per-instantiation status: trivial-dispose for value-type instantiations, non-trivial-dispose for class-type instantiations. The language does not admit such conditional declarations (the `owned` modifier requires its field's type to be a class or interface, expressed at declaration time, not conditionally on the substitution), so this case does not arise in practice; the rule is stated for completeness.

<a id="sec-trivial-dispose-classification-is-part-of-the-api-surface"></a>
#### Trivial-dispose classification is part of the API surface

Adding a destructor, an `owned` field, or an owning-inline-array field to a class promotes it from trivial-dispose to non-trivial-dispose. This is a **source-breaking change**: dependent code that used `_ := new ThatClass();` or relied on relaxed discharge rules will fail to compile after the upgrade.

API authors should:

- Treat the trivial-dispose status of public classes as part of their published contract.
- Document trivial-dispose status in the class's API documentation.
- Avoid adding a destructor to a previously-trivial class without bumping the major version of the containing module.

When the addition is necessary (the class genuinely now owns a resource), the standard migration-guide pattern applies: list the affected discard sites and direct callers to `using var _ := …`, an explicit `.Dispose()`, or `Task.FireAndForget` (for tasks).

<a id="sec-using-var"></a>
### 28.10 `using var`

`using var` declares a local that is disposed when the enclosing block ends:

```bezel
public method ProcessFile(filename: string)
  using var fs := new FileStream(filename);
  using var buffer := new Array<byte>(4096);
  var data := fs.ReadAll();
  // fs and buffer disposed here in reverse order
end ProcessFile;
```

Disposal happens on every exit path: normal completion, `break`, `return`, or exception.

`using var` is implicitly non-reassignable. Reassigning a `using var` variable is a compile error. To use a different resource later in the method, introduce a new `do ... end` block.

`using var` accepts a value-type or runtime-managed right-hand side. For ordinary value types and runtime-managed types, the disposal at scope end is a no-op (these types do not require disposal per [§28.9](#sec-runtime-managed-types)), and the only effect is the reassignability restriction. For move-only inline arrays (`[N owned]…`, [§15.3](#sec-move-only-value-types)), the local's owned slots are auto-disposed at scope end regardless of whether `using var` is used; the only additional effect of `using var` on a move-only local is the reassignability restriction. The uniform admissibility of `using var` across value-typed, runtime-managed, and move-only RHSes makes it safe to use uniformly in generic code or in compiler-generated desugarings (such as the for-loop body of [§29.2](#sec-desugaring-of-for)) without inspecting the underlying type kind.

To dispose before the enclosing method returns, nest in a `do ... end`:

```bezel
public method Process()
  var result := 0;
  do
    using var fs := new FileStream("data.txt");
    result := fs.ReadCount();
  end;     // fs disposed here
  Console.WriteLine($"count = {result}");
end Process;
```

<a id="sec-using-var-initialized-from-a-borrow"></a>
#### `using var` initialized from a borrow

When the right-hand side of a `using var` declaration is a borrow rather than an owned value (a method call without an `owned` return, an indexer access, a field read), the `using var` does **not** assume a disposal obligation: at scope end, no `Dispose()` is called, because the value is not owned by this scope. The `using var` still **pins** the borrow's source for the duration of the scope — meaning the borrow lifetime rules of [§28.3](#sec-local-variables-ownership-and-borrows) treat the source as in-scope for the using-var's entire lexical extent. Concretely: the source must remain live (not moved, not disposed) until the using-var's scope ends, exactly as if the borrow were used at every point up to the end of the scope. Pinning is a compile-time constraint, not a runtime mechanism; the compiler enforces it by extending the source's required-liveness range to cover the using-var's scope.

This case is what allows the for-loop desugaring ([§29.2](#sec-desugaring-of-for)) to wrap an arbitrary source in `using var __source := source` without taking ownership when `source` is borrowed; programmers also encounter it directly when using `using var` for scope-pinning of a borrowed iterable.

<a id="sec-collections-do-not-own-elements"></a>
<a id="sec-collection-element-ownership"></a>
### 28.11 Collection element ownership

Bezel has two families of collection types:

- **Borrowing collections** (`Array<T>`, `LinkedList<T>`, `Dictionary<K, V>`, `Set<T>`, `Stack<T>`, `Queue<T>`) store references that the caller continues to own. The collection's `Dispose()` releases the backing storage but does **not** dispose the elements. Element disposal is the caller's responsibility, typically arranged via `DisposeElements()` or an explicit iteration.

- **Owning collections** (`OwnedArray<T>`, `OwnedLinkedList<T>`, `OwnedDictionary<K, V>`, `OwnedSet<T>`, `OwnedStack<T>`, `OwnedQueue<T>`) own their elements. The collection's `Dispose()` cascades through every element. `Add` takes ownership; the indexer setter disposes the previous slot value before installing the new one; `Remove`/`RemoveAt`/`Clear` dispose the affected elements. Owning collections require `where T: class` (and, for `OwnedDictionary<K, V>`, `where V: class`) — owning makes sense only for class-typed values.

The two families are declared separately in the standard library ([§4](Bezel_Standard_Library.md#sec-system-collections-generic)). A program chooses the form that matches the intended ownership semantics. There is no implicit conversion between the two families; an `OwnedArray<Connection>` is not an `Array<Connection>` and vice versa.

<a id="sec-element-slot-policy"></a>
#### The element slot policy

Every standard collection's element semantics follow from one classification, not from per-collection rules. A collection's slots are classified by the collection's family and the element type's *element category* — a derived type property ([§3.6](#sec-derived-type-properties)):

| Element category | Borrowing family slots | Owning family slots |
|---|---|---|
| value type (non-move-only) | copies | *(rejected at instantiation)* |
| runtime-managed reference (`string`, function types, boxed values; [§28.9](#sec-runtime-managed-types)) | shared references | *(rejected at instantiation)* |
| class / interface type | borrows | owned |
| move-only value type ([§15.3](#sec-move-only-value-types)) | *(rejected at instantiation)* | *(rejected at instantiation)* |

(The owning family admits only class/interface element types; see the instantiation-site restriction in [§14.4](#sec-owned-as-a-generic-argument). Move-only element types are deferred for both families — see stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans) and the rationale.) For dictionaries the classification applies independently to the key slots and the value slots: an owning dictionary owns its *values*; its keys are value-like and classified per the borrowing column (stdlib [§4.9](Bezel_Standard_Library.md#sec-owneddictionaryk-v)).

The slot kind alone determines what every element operation does:

| Operation | copy slots | shared-reference slots | borrow slots | owned slots |
|---|---|---|---|---|
| store (`Add`, `Insert`, indexer-set) | copies the value in | stores the reference | stores the borrow; the element's source must outlive the collection's use of it | takes `owned`; indexer-set disposes the previous value first (skipped for a moved-from slot) |
| `Remove` / `RemoveAt` | drops the copy | drops the reference | drops the borrow; the caller's ownership is unaffected | **disposes** the removed element |
| `Clear()` | drops all | drops all | drops all | **disposes** every element |
| `Dispose()` of the collection | frees storage | frees storage | frees storage; elements untouched | **disposes** every live element — reverse-insertion order for ordered collections, implementation-defined order for unordered — then frees storage |
| indexer-get / default iteration | copy out | reference out | borrow out (source: the element's original owner; the collection is a transit point) | borrow out (source: the collection) |
| ownership out | *(none — nothing owned)* | *(none)* | *(none)* | `move coll[i]`, the `Pop*` family, `Drain()` |
| `DisposeElements()` | equivalent to `Clear()` | equivalent to `Clear()` | disposes every class-typed element, then empties | *(not declared — `Dispose()` already cascades)* |

The per-collection clauses in the remainder of this section and in the standard library document ([§4](Bezel_Standard_Library.md#sec-system-collections-generic)) are presentations of this table for each concrete type; they do not introduce element semantics of their own, and a per-collection statement that diverges from the table is a defect in this specification. The storage embodiment of the owning column is `OwnedBuffer<T>` (stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)), the primitive over which the resizable owning collections compose; the borrowing family's storage remains a compiler intrinsic in this revision, because a user-expressible borrow-storing primitive needs its own borrow-lifetime rules (storing borrows in fields is otherwise rejected, [§28.3](#sec-local-variables-ownership-and-borrows)) — that design is deferred, recorded in the rationale.

<a id="sec-borrowing-collections"></a>
#### Borrowing collections

For a borrowing collection type (`Array<T>`, `Dictionary<K, V>`, etc.), the standard operations are:

- **`Dispose()`** frees the collection's backing storage. Elements are **not** disposed.
- **`Clear()`** removes all elements without disposing them.
- **`Add(item: T)`**, **`Insert(index, item: T)`**, **indexer-set** `coll[i] := value` accept borrowed references and store them.
- **`Remove(item)`** removes the first equal element (by `Equals`) and returns `bool`. **`RemoveAt(index)`** removes the element at the given index and returns `()`. Neither method disposes the removed element on a borrowing collection — the caller continues to own it (if it had ownership) or it goes out of scope (if it was borrowed transit). The **indexer-get** `coll[i]` produces a borrowed reference whose source is whichever owner the borrow originally came from. The collection is a transit point, not a source.

The interface `ICollection<T>` declares `DisposeElements()`, which disposes every class-typed element and empties the collection. For dictionaries this disposes both keys and values when they are class-typed. For value-only types `DisposeElements()` is equivalent to `Clear()`. `DisposeElements()` is the explicit form for the case where the holding class semantically owns the elements but the collection's *type* is borrowing — typically when the holding class assembles the collection's contents and is responsible for cleaning them up itself.

The recommended pattern when a class semantically owns a collection-of-resources but uses a borrowing collection type:

```bezel
class BankAccount
  private owned _transactions: Array<Transaction>;

  public constructor()
    _transactions := new Array<Transaction>();
  end constructor;

  destructor
    _transactions.DisposeElements();
  end destructor;
end BankAccount;
```

`owned` on the field handles container disposal; the destructor adds the element-disposal step. This pattern is valid and appropriate when the class wants explicit control over element disposal (perhaps because not every element should be disposed, or because the cleanup is interleaved with other work). For the common case where the class *always* owns every element and disposes them all together, `OwnedArray<Transaction>` is the cleaner choice — the destructor's explicit `DisposeElements()` call is replaced by the automatic cascade.

For asymmetric ownership in a dictionary (a class owning the values but not the keys, or vice versa), iterate explicitly:

```bezel
for var entry in _index do
  entry.Value.Dispose();
end;
_index.Clear();
```

The standard library does not provide separate `DisposeKeys` or `DisposeValues` methods; explicit iteration is the more honest expression of intent. For collections where ownership of values *is* the natural model, the owning form (next subsection) is preferred.

<a id="sec-owning-collections"></a>
#### Owning collections

An owning collection (`OwnedArray<T>`, `OwnedDictionary<K, V>`, `OwnedStack<T>`, etc.) owns its elements. The standard library [§4](Bezel_Standard_Library.md#sec-system-collections-generic) declares the owning-collection family. The signature changes that distinguish owning from borrowing collections are:

- **`Add(owned item: T)`** accepts an owned argument. The caller transfers ownership to the collection.
- **`Insert(index: int, owned item: T)`** likewise.
- **Indexer set** `coll[i] := value` requires `value` to be owned. The previous value at that slot is disposed before the assignment installs the new one. (See the explanatory paragraph below for why this is *not* the same rule as [§28.7](#sec-reassigning-owned-fields)'s for owned-field assignment, where disposal of the previous value is the programmer's responsibility.)
- **`Remove(item: T)`** and **`RemoveAt(index: int)`** dispose the removed element.
- **`Clear()`** disposes every element.
- **`Dispose()`** disposes every element, then frees the backing storage.
- **Indexer get** `coll[i]` produces a borrow of the slot's value, exactly as for borrowing collections. Reading does not transfer ownership; `move coll[i]` is the explicit-take form ([§28.8.1](#sec-the-move-expression) — see below for the collection-slot extension).

The "indexer-set disposes the previous value" rule is a different rule from [§28.7](#sec-reassigning-owned-fields)'s rule for owned-field assignment, where the compiler does *not* insert automatic cleanup and the programmer must `Dispose()` the previous value explicitly. The asymmetry is deliberate: the indexer setter is a method (the compiler-generated `set_Item`), and method bodies can perform arbitrary work — including disposal of the previous value — before storing the new one. Field assignment, by contrast, is a primitive operation with no method body to host disposal logic. The indexer-set form is therefore able to encapsulate the disposal-then-assign sequence as a single method call, while owned-field assignment leaves the dispose-first step explicit at the assignment site. The two forms produce equivalent behavior; only the syntactic location of the disposal differs. For a slot already in moved-from state (after `move coll[i]`), the indexer setter detects the empty slot at runtime and skips the disposal step before installing the new value.

User-written *property setters* over an owned field are also method bodies and may perform disposal explicitly in their body (`set => do _cache.Dispose(); _cache := value; end;`). The compiler does **not** auto-insert disposal in property setters on owned fields, even though the setter is a method — the auto-disposal rule is specific to the compiler-synthesized indexer setter on owning collections, not a general "owned-field-targeting setter" feature. User code that wants disposal-on-set in a property setter writes it explicitly, the same way owned-field assignment is written explicitly elsewhere.

In addition, owning collections expose **drain methods** that transfer ownership of elements out of the collection:

- **`PopAt(index: int): owned T`** removes the element at the given index and returns it as owned. The slot is closed (subsequent indices shift down for sequence types; the dictionary entry is removed for `OwnedDictionary`).
- **`PopFirst(): owned T`** and **`PopLast(): owned T`** for the common cases on `OwnedArray`, `OwnedLinkedList`. `OwnedStack.Pop` and `OwnedQueue.Dequeue` are the equivalent operations for those types, returning `owned T`.
- **`Drain(): OwnedEnumerable<T>`** returns an iterable that drains the source as it is iterated. After full iteration, the source collection is empty. Partial iteration drains only the elements yielded; the rest remain. The user iterates with `for var item in coll.Drain() do ... end;`, where `item` is owned and must be discharged inside the loop body (`using var u := item;` is the typical form).

`DisposeElements()` is **not** declared on owning collections. Disposing the collection already disposes the elements; the explicit method would be redundant.

Default iteration `for var item in coll do ... end` (where `coll` has static type `OwnedArray<T>`, etc.) produces *borrows* of each element via the collection's `IEnumerable<T>` conformance. The collection still owns its elements after iteration. To consume, iterate over `coll.Drain()`, which has static type `OwnedEnumerable<T>` and yields owned values.

```bezel
public method UseEach()
  using var pool := new OwnedArray<Connection>();
  pool.Add(Connect("primary"));      // ownership transfers in
  pool.Add(Connect("secondary"));     // ownership transfers in

  // Default iteration: each conn is a borrow
  for var conn in pool do
    conn.Ping();                      // pool still owns conn afterward
  end;

  // Drain iteration: each conn is owned
  for var conn in pool.Drain() do
    using var c := conn;              // discharge each owned element
    c.SendShutdown();
  end;
  // pool is now empty; using var on pool disposes the (empty) array
end UseEach;
```

When an owning collection is itself disposed (whether at the end of a `using var` scope, in a destructor's auto-dispose pass, or at static-program-exit for a static `owned` field), every element it still holds is disposed first, then the backing storage is freed. The element-disposal order is implementation-defined for unordered collections (`OwnedDictionary`, `OwnedSet`), and is reverse-insertion-order for ordered collections (`OwnedArray`, `OwnedLinkedList`, `OwnedStack`, `OwnedQueue`).

<a id="sec-move-on-a-collection-slot"></a>
#### `move` on a collection slot

The `move` expression form ([§28.8.1](#sec-the-move-expression)) extends to owning-collection slots. `move coll[i]` reads the i-th element of an owning collection as an owned value, leaving the slot in moved-from state.

The `move coll[i]` form is admitted on:

- Owning heap collections that support indexer syntax — `OwnedArray<T>`, `OwnedDictionary<K, V>`, and `OwnedBuffer<T>` for class-typed T (the dynamic-size owning storage primitive, stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)). `OwnedBuffer<T>` for value-typed T does *not* admit `move` — values have no ownership to transfer; the plain indexer-get suffices.
- `Span<T>` for class-typed T (stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)), which delegates to the underlying `OwnedBuffer<T>`. `move span[i]` is equivalent to `move span.Buffer[span.Start + i]` — it takes ownership of the slot's value and leaves the slot in moved-from state in the *underlying buffer*, not in the span (the span is a stateless view; it carries no slot state of its own). As of v0.105, `move span[i]` bounds-checks against `span.Length` before delegating; if `i < 0` or `i >= span.Length`, `IndexOutOfRangeException` is thrown and no slot is moved-from. This matches the bounds-check behavior of the span's plain indexer-get and indexer-set. The field-restoration rule applies to the buffer slot in the usual way; the natural restoration is `span[i] := null`. `Span<T>` for value-typed T does not admit `move` (mirroring the underlying buffer). `ReadOnlySpan<T>` does not admit `move` for any T (it exposes no writable indexer and no take operation).
- Owning inline arrays — both `[N owned]T?` and `[N owned]T` (see [§15.1.1](#sec-owning-inline-arrays)).

`Array<T>`, `Dictionary<K, V>`, and the other borrowing collections do not admit `move coll[i]` — their slots hold borrows, not owned values; there is no ownership to transfer. The same applies to `ArraySpan<T>` and `ReadOnlyArraySpan<T>` (stdlib [§4.15](Bezel_Standard_Library.md#sec-arrayspant-readonlyarrayspant)), which view into a borrowing `Array<T>` source: `move arrSpan[i]` is rejected at compile time. The string-backed read-only span types `ReadOnlyStringSpan` (stdlib [§3.3](Bezel_Standard_Library.md#sec-readonlystringspan)) and `ReadOnlyAsciiByteSpan` (stdlib [§3.4](Bezel_Standard_Library.md#sec-readonlyasciibytespan)) also do not admit `move` — strings are immutable and their character data is reference-shared; no ownership transfer applies. The plain indexer-get is the only read form for all these types.

`OwnedStack<T>`, `OwnedQueue<T>`, `OwnedLinkedList<T>`, and `OwnedSet<T>` do not have indexers (`stack[i]`, `linkedList[i]`, etc. do not parse), so `move` cannot syntactically apply to their slots. These structured-or-unordered collection types provide their own ownership-out methods (`Pop`, `Dequeue`, `Take`, etc.) that perform the equivalent take-and-discharge in a single call.

The field-restoration rule applies: the slot must be reassigned before the surrounding method exits. For nullable element types (`OwnedArray<T>` internally storing `T?`, `OwnedDictionary<K, V>`, `[N owned]T?`, `OwnedBuffer<T>` for class-typed T which stores `T?`), assigning `null` to a moved-from slot satisfies the rule. For non-nullable element types (`[N owned]T`), restoration requires assigning a fresh owned T value — a constructor call, another moved value, or a borrow promoted to ownership. Code that needs "leave the slot empty after take" must use the nullable form. (For value-typed `OwnedBuffer<T>` the rule is moot — `move buf[i]` is not admitted, so no restoration obligation arises.)

**Indexer setter on a moved-from slot.** When the slot is in moved-from state, assigning to it through the indexer setter does *not* trigger the owned-setter's "dispose previous value" semantics — the previous value was already taken out by `move`, so there is no previous value to dispose. The slot transitions directly from moved-from to holding the new value. This matches the analogous rule for owned-local reassignment ([§28.7](#sec-reassigning-owned-locals)) where a moved-from local accepts reassignment without triggering an undisposed-overwrite warning. The compiler's per-slot moved-from tracking is what makes this distinguishable from a non-moved-from slot, where the setter does dispose the previous value.

The compiler tracks moved-from state per slot when the index is a compile-time constant or a local read. For variable indices that the compiler cannot prove are constant within the method, the analysis is conservative: after `move coll[i]` with a non-constant `i`, the *entire collection* is treated as having one or more slots in moved-from state. The admissible restoration operations depend on the collection kind:

- For heap owning collections (`OwnedArray<T>`, `OwnedDictionary<K, V>`, and class-typed `OwnedBuffer<T>`): `coll[j] := value` for any `j` (treated as restoration of one slot if `j` matches the moved index, or as unrelated reassignment otherwise), `coll.Clear()` for those that provide it, or `coll.Dispose()` (which dispose the entire collection, satisfying the obligation by destruction). `OwnedBuffer<T>` does not provide `Clear()` directly; the equivalent is a loop assigning `null` to every slot or simply disposing the buffer.
- For inline owning arrays (`[N owned]T?` or `[N owned]T`): `arr[j] := value` for any `j` (for nullable forms, `arr[j] := null` is also a valid restoration that marks slots empty). Inline arrays have no `Clear()` or `Dispose()` method (they are value types, not classes); restoration is per-slot assignment only.

The `PopAt` family of methods covers the common case where the user wants to take an element out and let the collection close the gap; `move coll[i]` is the building block for advanced cases where the slot is taken and immediately reassigned to a new value:

```bezel
public method ReplaceAt(index: int, owned newValue: T)
  var old := move pool[index];      // take from slot
  pool[index] := newValue;           // restore slot with new value
  old.Dispose();                     // dispose the old value
  // (or `using var taken := move pool[index];` for scope-bound disposal)
end ReplaceAt;
```

The same exception-safety rules apply: any throwable expression between the move and the reassignment must be an exit path on which the slot has been restored. The "build before take" discipline from [§28.8.1](#sec-the-move-expression) carries over directly.

<a id="sec-compatibility-between-borrowing-and-owning-forms"></a>
#### Compatibility between borrowing and owning forms

`Array<T>` and `OwnedArray<T>` are distinct, unrelated types. There is no implicit conversion between them in either direction. A program that has an `OwnedArray<T>` and needs to expose it as a borrowing surface (`Array<T>`) must drain the owning array (consuming each owned element) and rebuild a borrowing array of borrows; the spec provides no shorthand for this because the operation always loses information (the original owner of each element is the caller's responsibility once drained).

The most common case where this conversion is *not* what the programmer wants: passing an `OwnedArray<T>` to a method expecting `Array<T>`. The intent is usually "let the callee read the elements" — for which the answer is to expose `IEnumerable<T>`, `IReadOnlyCollection<T>`, or `IReadOnlyList<T>` (the borrowing views). Both `Array<T>` and `OwnedArray<T>` implement these read-only iteration interfaces, so a consumer signature using one of them accepts either source:

```bezel
public method ReportEach(items: IEnumerable<Connection>)
  for var c in items do Console.WriteLine(c.Name); end;
end ReportEach;

public method UseOwnedPool()
  using var pool := new OwnedArray<Connection>();
  pool.Add(Connect("a"));
  pool.Add(Connect("b"));
  ReportEach(pool);     // OK: OwnedArray<Connection> implements IEnumerable<Connection>
                        //     iteration yields borrows
end UseOwnedPool;
```

A generic helper works the same way:

```bezel
public method ReportEachGeneric<T>(items: IEnumerable<T>) where T: class
  for var c in items do Console.WriteLine(c.Name); end;
end ReportEachGeneric;

// Call with Array<Connection>: T inferred as Connection.
// Call with OwnedArray<Connection>: T inferred as Connection.
// Same generic instantiation for both source forms.
```

The borrowing-iteration interfaces (`IEnumerable<T>`, `IReadOnlyList<T>`, `IReadOnlyCollection<T>`) are the shared abstractions between the two collection families. The mutating interfaces (`ICollection<T>`/`IList<T>` for borrowing; `IOwnedCollection<T>`/`IOwnedList<T>` for owning) are separate hierarchies, intentionally so — `Add` has different semantics in each (borrow-taking vs. owned-taking). Code that abstracts over mutation must pick one hierarchy.

<a id="sec-asynchronous-disposal"></a>
### 28.12 Asynchronous disposal

For resources requiring asynchronous cleanup, classes may implement `IAsyncDisposable`:

```bezel
interface IAsyncDisposable
  method DisposeAsync(): Task;
end IAsyncDisposable;
```

`DisposeAsync` is declared without an `async` modifier on the interface; `async` is an implementation-side modifier and never appears on interface method declarations. Implementations add `async` to the matching method and use `await` freely.

A class that implements `IAsyncDisposable` also has its compiler-generated synchronous `Dispose`. The two are distinct phases of the disposal lifecycle:

1. **Logical disposal** runs once. For `await using`, the awaited `DisposeAsync()` performs whatever asynchronous cleanup the class defines; the destructor body (if any) is **not** also run, because `DisposeAsync` supersedes it. For plain `using` (or a synchronous `Dispose()` call) on a class that implements both, the destructor body runs synchronously and `DisposeAsync` is **not** invoked — the class's two cleanup paths are alternatives, not a sequence. A user-written `DisposeAsync` should not call `Dispose` itself.
2. **Physical reclamation** runs once, after logical disposal has completed: owned fields are disposed in reverse declaration order, the base class's reclamation chains, and the heap block is released. This phase is the same regardless of which logical-disposal path ran. For `await using`, the runtime invokes physical reclamation as the second step of the implicit `Dispose()` it issues after `DisposeAsync` returns; the destructor body that would normally run in the synchronous path is skipped.

A class that implements `IAsyncDisposable` and has resources requiring cleanup **should** also declare a `destructor` that performs the synchronous fallback of the same cleanup. If a class implements `IAsyncDisposable` but has no destructor, synchronous `Dispose()` performs only physical reclamation (owned-field disposal and base chaining) — any cleanup that `DisposeAsync` would have performed is skipped. The compiler emits a warning when a class implements `IAsyncDisposable` without a destructor: *"Class implements IAsyncDisposable but has no destructor; synchronous Dispose() will skip async cleanup."*

A second call to `Dispose()` or `DisposeAsync()` after either has run is a no-op ([§28.2](#sec-the-idisposable-contract)'s idempotence rule). The runtime tracks completion at the granularity of the object, not per-method.

`await using` is the async counterpart of `using var`:

```bezel
public async method ProcessAsync(): Task
  // ConnectAsync and SendAsync are illustrative: the standard library's
  // System.Net module is deferred to a future revision (std lib §11).
  await using var connection := await ConnectAsync(url);
  await connection.SendAsync(data);
  // connection.DisposeAsync() awaited at scope end
end ProcessAsync;
```

`await using` is valid only inside an async method or async lambda.

When a class implements both `IDisposable` and `IAsyncDisposable`, prefer `await using` in async contexts.

<a id="sec-shared-ownership"></a>
### 28.13 Shared ownership: `Shared<T>` and `Weak<T>`

The ownership model is single-owner: every disposal obligation has exactly one holder. `Shared<T>` (stdlib [§2.16](Bezel_Standard_Library.md#sec-shared-and-weak)) is the scoped exception — a compiler-known class whose instances (*handles*) jointly own one underlying value; the value is disposed exactly once, when the last handle is disposed. Shared ownership is opt-in, visible at every site (`Clone()` is an explicit call), and **domain-local** (`Shared<T>` is not sendable, [§30.14](#sec-sendable-types)).

```bezel
using var primary := new Shared<Connection>(Connect("db"));   // consumes the owned Connection; count = 1
using var second  := primary.Clone();                          // count = 2; an independent owned handle
second.Get().Ping();                                           // borrow of the underlying Connection
// scope end: second disposed (count 1), then primary disposed (count 0) → Connection disposed
```

The semantics, stated against the existing model:

- **Handles are ordinary owned class instances.** Every rule of this chapter applies to a handle unchanged: it is produced owned (by `new Shared<T>(...)` or `Clone()`), discharged by `using var`, an `owned` field, an `owned` parameter or return, `move`, or explicit `Dispose()`; branch joins, exception-path discharge, and the leak-prevention rules of [§28.6](#sec-leak-prevention) see nothing new. A handle may be stored in owning collections (`OwnedArray<Shared<T>>`) like any class instance. The *only* novel behavior is inside the compiler-known type: the shared count, and last-one-disposes.
- **`new Shared<T>(owned value: T)`** consumes an owned T — the constructor is an owned-consuming position like any other ([§28.3](#sec-local-variables-ownership-and-borrows)); after construction the handle's disposal obligation *is* the value's. `T` must be a proper class type; runtime-managed and value types are rejected at instantiation ([§14.4](#sec-owned-as-a-generic-argument) pattern — they have no disposal obligation to share).
- **`Clone(): owned Shared<T>`** produces a new handle and increments the count. Cost and intent are visible at the call site; there is no implicit handle duplication anywhere in the language (assigning a handle moves or borrows it per the ordinary rules — it never silently clones).
- **`Get(): T`** produces a *borrow of the underlying value whose source is the handle*, valid while that handle lives — the same shape as the owning-collection indexer-get ([§28.11](#sec-element-slot-policy) owned column: borrow out, source: the collection). The borrow does not extend the value's lifetime; the handle does. [§28.6](#sec-leak-prevention)'s owned-field-return rule is not violated: `Get()` is the compiler-known accessor dispensation that owning-collection reads already have.
- **Disposal.** Disposing a handle decrements the count; disposing the last live handle disposes the underlying value (running its destructor cascade normally). Handle disposal is idempotent per [§28.2](#sec-the-idisposable-contract). The exactly-once invariant of [Appendix C](#sec-appendix-c) is preserved: the value's `Dispose()` runs once, from the last handle.
- **Cycles leak.** A value that (transitively, through owned fields) holds a `Shared<T>` handle participating in its own ownership keeps its count above zero forever — the standard reference-counting hazard. The compiler does not detect ownership cycles through `Shared<T>`. Break cycles with `Weak<T>`.
- **`Weak<T>`** (stdlib [§2.16](Bezel_Standard_Library.md#sec-shared-and-weak)) is a non-owning observer minted from a handle (`shared.Downgrade(): owned Weak<T>`). A weak handle does not keep the underlying value alive; **`TryUpgrade(out owned s: Shared<T>?): bool`** atomically produces a new strong handle if the value is still alive (Try pattern, `out using var` at the call site — [§22.3](#sec-parameter-modifiers)'s `out owned` rules). Weak handles are themselves ordinary owned class instances and are disposed like any other.
- **Aliasing.** Multiple handles give multiple borrows of one mutable value within a domain. This is serial-execution-safe ([§30.13](#sec-runtime-model)) and carries the same *logical* aliasing hazards as any other long-lived borrow; nothing about `Shared<T>` relaxes use-after-dispose ([§28.2](#sec-checked-disposal)) for borrows taken from a handle that is then disposed.
- **Not sendable.** The count is not synchronized; handles and weak handles are domain-local ([§30.14](#sec-sendable-types)). A synchronized cross-domain form is deferred; see the design rationale.

---

<a id="sec-iteration-2"></a>
## 29. Iteration

`for` operates on any value whose type implements `IEnumerable<T>`. The protocol is interface-based; there is no duck-typing fallback.

<a id="sec-the-protocol"></a>
<a id="sec-iterator-protocol"></a>
### 29.1 The protocol

```bezel
interface IEnumerable<out T>
  method GetEnumerator(): owned IEnumerator<T>;
end IEnumerable;

interface IEnumerator<out T>: IDisposable
  method MoveNext(): bool;
  readonly property Current: T;
end IEnumerator;
```

Calling `GetEnumerator()` produces a fresh enumerator positioned just before the first element. `MoveNext()` advances the cursor and returns `true` if a new element became available, `false` if the sequence is exhausted. `Current` returns the element at the current position; reading `Current` before the first successful `MoveNext` or after `MoveNext` returns `false` is undefined behavior — concrete enumerators typically throw `InvalidOperationException`.

`IEnumerator<T>` extends `IDisposable`. An enumerator with no resources to release simply has an empty `Dispose` body; the compiler generates one if the implementor writes none.

`GetEnumerator()` returns `owned IEnumerator<T>`: the caller takes ownership of the enumerator and must dispose it. The standard `for` and `await for` desugarings ([§29.2](#sec-desugaring-of-for), [§30.7](#sec-asynchronous-programming-async-iterators)) wrap the enumerator in a `using var` (or `await using var`) automatically, so loop forms satisfy the obligation without the user writing the disposal explicitly. Code that calls `GetEnumerator()` manually — outside a loop form — must do so under the `using var x := source.GetEnumerator();` shape.

<a id="sec-current-on-owned-element-enumerators"></a>
<a id="sec-current-semantics-on-borrowing-and-owning-enumerators"></a>
#### `Current` semantics on borrowing and owning enumerators

`IEnumerator<T>` and `OwnedEnumerator<T>` (declared in stdlib [§4.12](Bezel_Standard_Library.md#sec-borrowing-enumeration-interfaces), [§4.13](Bezel_Standard_Library.md#sec-owning-enumeration-interfaces)) are distinct interfaces with different `Current` semantics:

- **`IEnumerator<T>.Current`** is **non-destructive**: it returns a borrow of the most-recently-yielded value, which the enumerator continues to own (if it has ownership of the source) or re-borrow (if the source is borrow-based). Multiple reads between `MoveNext` calls produce additional borrows of the same value. The async variant `IAsyncEnumerator<T>.Current` has the same non-destructive semantics relative to `MoveNextAsync`.

- **`OwnedEnumerator<T>.Current`** is **destructive**: the first read after a successful `MoveNext` transfers ownership of the most-recently-yielded value to the caller, leaving the enumerator's internal slot empty. A second read without an intervening `MoveNext` is undefined behavior — concrete enumerators typically throw `InvalidOperationException`. Reading `Current` and *not* discharging the resulting owned value is a compile error at the call site (per the standard owned-value discharge rules). The async variant `OwnedAsyncEnumerator<T>.Current` has the same destructive semantics relative to `MoveNextAsync`.

`OwnedEnumerator<T>` is reached via:
- `OwnedEnumerable<T>.GetOwnedEnumerator()` — typically called by the for-loop desugaring when the source is `OwnedEnumerable<T>`.
- Drain methods on owning collections (`OwnedArray<T>.Drain()` etc.) — their return type is `OwnedEnumerable<T>`, and the resulting enumerator is `OwnedEnumerator<T>`.
- Iterator methods using `yield return` whose declared return type is `OwnedEnumerable<T>` (or async equivalents).

Default iteration over an owning collection (`for var x in coll do ... end` where `coll: OwnedArray<C>`) uses the collection's `IEnumerable<C>` conformance — the borrowing path — and produces borrows of C. To opt into owned-transferring iteration, call `coll.Drain()` (which returns `OwnedEnumerable<C>`) and iterate over the result. The for-loop desugaring at [§29.2](#sec-desugaring-of-for) picks based on the static type of the source expression: a source typed `IEnumerable<T>` desugars via `IEnumerator<T>` (non-destructive); a source typed `OwnedEnumerable<T>` desugars via `OwnedEnumerator<T>` (destructive).

The for-loop desugaring reads `__enumerator.Current` exactly once per iteration, matching the destructive-read semantics naturally on the owning path. The destructive read is invisible to user code that uses `for` or `await for` loops — the loop body sees `var x := __enumerator.Current` (or its `for var x in ...` equivalent), and `x` is owned and subject to the standard discharge rules. Users who call `MoveNext`/`MoveNextAsync` and read `Current` manually (outside a loop form) need to be aware that `Current` is single-use per advance for the owning variant.

Implementations of `OwnedEnumerator<T>` or `OwnedAsyncEnumerator<T>` may choose either of two storage strategies for the most-recently-yielded value: store it in a private owned field that is moved out by `Current`'s getter (using the [§28.8.1](#sec-the-move-expression) `move` form internally), or maintain the value in transient state that exists only between the advance call and the next `Current` read. Both strategies satisfy the contract; the choice is implementation-detail.

<a id="sec-desugaring-of-for"></a>
### 29.2 Desugaring of `for`

```bezel
for var x in source do
  // body
end;
```

When `source` is typed as `IEnumerable<T>` (or implements `IEnumerable<T>` through inheritance), the desugaring is:

```bezel
do
  using var __source := source;
  using var __enumerator := __source.GetEnumerator();
  while __enumerator.MoveNext() do
    var x := __enumerator.Current;
    // body
  end;
end;
```

When `source` is typed as `OwnedEnumerable<T>` (or implements it), the desugaring uses the owning-enumerator path:

```bezel
do
  using var __source := source;
  using var __enumerator := __source.GetOwnedEnumerator();
  while __enumerator.MoveNext() do
    var x := __enumerator.Current;           // x is owned; discharge required
                                              //   on every body-exit edge
    // body
  end;
end;
```

The iteration variable `x` is a **plain owned local**, not a `using var`. This is intentional: the user must be able to discharge `x` by moving it into another owned slot (e.g., `dest.Add(x)`, `using var c := x` for scope-bound disposal, or `return x`), and `using var` would forbid the move. The compiler enforces the discharge requirement by treating the loop body as a scope whose every exit edge (fall-through to the next iteration, `break`, `continue`, `return`, `throw`) must dispose or move `x`. The "fall-through to the next iteration" edge is checked the same way as the body's end: at the closing `end` of the body, `x` must be in moved-from or disposed state. The compiler-generated check is identical to the [§28.3](#sec-local-variables-ownership-and-borrows) owned-local discharge rule applied to the iteration variable.

The choice is made by the static type of `source`. If `source`'s static type implements both `IEnumerable<T>` and `OwnedEnumerable<T>` (as `OwnedArray<T>` does), the desugaring uses the `IEnumerable<T>` path — default iteration is borrowing. To opt into the owning path, the source must be expressed at static type `OwnedEnumerable<T>`, typically by calling `.Drain()` on an owning collection.

Both `__source` and `__enumerator` are disposed on every exit path. The loop variable is freshly bound each iteration. `break` exits the surrounding `do ... end`, triggering disposal of both. `continue` jumps to the next `MoveNext` call.

When `source` is a value-type expression, a runtime-managed expression (e.g. a string), or a borrowed local of class type, the `using var __source := source;` is a no-op (per [§28.9](#sec-runtime-managed-types) for runtime-managed cases; per [§28.10](#sec-using-var) a `using var` initialized from a borrow does not assume a disposal obligation but does pin the source for the loop's duration). When `source` is a fresh class-typed expression (an iterator-method call producing an `IEnumerable<T>`, a `new` expression), the wrapping `using var` ensures the iterable is disposed when the loop ends. This closes the leak that would arise from `for var x in GetThings() do ... end` when `GetThings()` returns a heap-allocated state-machine iterable.

When `source` is itself an owned local — including a `using var` of class type or any other owned class-typed local — the desugaring is adjusted: `using var __source := source;` is replaced by a borrow that pins `source` for the loop's duration without assuming the disposal obligation. This case mirrors the borrowed-local case above; the obligation stays with the original owner. In particular, `for var line in stream do ... end` where `stream` is a `using var` does not move out of `stream` (which would be a [§28.3](#sec-local-variables-ownership-and-borrows) violation) and does not double-dispose at the for-loop's end.

When `source` is a `Range<T>` value ([§16.3](#sec-the-ranget-type)), the compiler may replace the entire desugaring above with a direct counted loop, provided the observable behavior is identical. For range literals with constant endpoints, this avoids heap-allocating an enumerator. See [§16.5](#sec-iteration) for details.

When `source` has inline-array type (`[N]T`, including the borrow-bearing and owning slot-marker forms), the desugaring is a direct indexed loop over the array's slots — inline arrays do not implement `IEnumerable<T>` and the enumerator path of [§29.2](#sec-desugaring-of-for) does not apply ([§15.1.2](#sec-inline-array-members)). The iteration variable's type and ownership state follow the slot kind: a copy for value-typed slots, a borrow for class-typed slots (whether the slot marker is `owned` or not — for owning slots the array retains ownership, and the loop body sees a borrow that does not consume the slot). Direct `move arr[i]` inside the loop body is admitted, but the field-restoration rule ([§28.8.1](#sec-the-move-expression)) applies: every moved-from slot must be reassigned before the surrounding method exits. Iterating an `[N owned]T` non-nullable owning array and moving from every slot without restoration is therefore rejected; the typical pattern for "consume every slot" uses `[N owned]T?` and assigns `null` after each move.

When `source` has type `Span<T>` or `ReadOnlySpan<T>` (stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)), the desugaring is also a direct indexed loop over the span's range, parallel to the inline-array case. Spans do not implement `IEnumerable<T?>` — the enumerator returned by `IEnumerable<T>.GetEnumerator()` is an ordinary class instance that would need to hold a borrow of the underlying buffer across its own lifetime, which the borrow tracker cannot bound through the enumerator interface — so the enumerator path of this section does not apply. The iteration variable's type follows the span's slot type: `T?` for class-typed T (the loop body sees a borrow of each slot's value, possibly null) and `T` for value-typed T (the loop body sees a borrow of the slot — effectively a value copy for primitive T). The borrow's source is the underlying `OwnedBuffer<T>`, reached transitively through the span's `Buffer` field. Direct `move span[i]` inside the loop body is admitted for class-typed `Span<T>` (not for value-typed `Span<T>`, not for any `ReadOnlySpan<T>`); the same field-restoration rule applies to the underlying buffer slot, and the typical restoration is `span[i] := null`.

When `source` has type `OwnedBuffer<T>` for value-typed T (stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)), the desugaring is a direct indexed loop, parallel to the span case. The iteration variable's type is T and the loop body sees a borrow of each slot's value. (For class-typed `OwnedBuffer<T>`, the type implements `IEnumerable<T?>` and the standard enumerator path applies, with the optimization-elision rule above; the observable result is the same as a direct-indexed loop.)

When `source` has type `ReadOnlyStringSpan` (stdlib [§3.3](Bezel_Standard_Library.md#sec-readonlystringspan)), the desugaring is a direct indexed loop, parallel to the `ReadOnlySpan<T>` case. `ReadOnlyStringSpan` does not implement `IEnumerable<char>` — the enumerator returned by `IEnumerable<T>.GetEnumerator()` is an ordinary class instance that would need to hold the span (or its borrow of the source string) across its own lifetime, and the borrow tracker cannot bound that lifetime through the enumerator interface. The iteration variable's type is `char` and the loop body sees a copy of each code point (chars are value-typed primitives). The borrow's source is the underlying `string`, reached through the span's `Source` field.

When `source` has type `ReadOnlyAsciiByteSpan` (stdlib [§3.4](Bezel_Standard_Library.md#sec-readonlyasciibytespan)), the desugaring is a direct indexed loop, parallel to the `ReadOnlyStringSpan` case above. The iteration variable's type is `byte` and the loop body sees a copy of each byte (the byte produced by the cast `byte(Source[Start + i])`, safe per the ASCII invariant established at span construction). The borrow's source is the underlying `string`, reached through the span's `Source` field.

When `source` has type `ArraySpan<T>` or `ReadOnlyArraySpan<T>` (stdlib [§4.15](Bezel_Standard_Library.md#sec-arrayspant-readonlyarrayspant)), the desugaring is a direct indexed loop, parallel to the `Span<T>` / `ReadOnlySpan<T>` case. Neither type implements `IEnumerable<T>` — the same enumerator-escape reasoning given for `ReadOnlyStringSpan` above applies. The iteration variable's type is T and the loop body sees a borrow of each slot (the borrow's source is the underlying `Array<T>`, reached through the span's `Source` field). The iteration-variable type follows the array's element type directly — `T` is whatever the array was instantiated with (no `T?` wrapping, unlike the OwnedBuffer-backed span case for class T, because `Array<T>` slots store borrows rather than owned nullable references). `move arrSpan[i]` is not admitted in the loop body for any T, since `Array<T>` slots are borrows with no ownership to transfer.

When `source` has type `IReadOnlySpan<TElement>` (stdlib [§4.16](Bezel_Standard_Library.md#sec-ireadonlyspan)), the desugaring is a direct indexed loop over `0..<source.Length`, accessing each element via `source[i]`. The iteration variable's type is `TElement`. Indexer and `Length` access through the interface are virtual dispatches (vtable lookup, indirect call); the iteration is correspondingly slower than iterating a concrete span type. Authors writing inner loops should iterate over concrete span types where peak per-element throughput matters; the interface-typed for-loop is the convenient form for code where breadth across span sources is more important than performance. The lifetime constraint is the standard one: the `IReadOnlySpan<TElement>` value (a boxed borrow-bearing record per the rule of [§9.7.1](#sec-boxing-of-borrow-bearing-records)) may not outlive the source of the underlying span's borrow, which the compiler tracks through the boxed-value lifetime constraint.

More generally, the compiler may elide the `IEnumerable`/`IEnumerator` protocol and the associated heap allocation whenever it can prove that the observable behavior is identical to the desugared form. For heap arrays (`Array<T>`, `OwnedArray<T>`) and strings, the compiler is expected to emit direct indexed loops as an optimization (these types do implement `IEnumerable<T>`, but the protocol is elided when the compiler can see the type concretely). For inline arrays (`[N]T`), the direct indexed loop is mandatory rather than optimizational — inline arrays are not class types and do not implement `IEnumerable<T>` at all ([§15.1.2](#sec-inline-array-members)). For range literals with constant endpoints, the compiler emits tight counted loops with no allocation. These optimizations ensure that the common `for` patterns — iterating an array, a string, or a numeric range — incur no disposal overhead from `IEnumerator<T>: IDisposable`.

<a id="sec-iterator-yield-ownership"></a>
#### Iterator yield ownership

(This subsection covers `yield return` semantics; the yield construct itself is introduced in [§29.3](#sec-iterator-methods). The placement here keeps it close to the for-loop desugaring with which it interacts most directly.)

The behavior of `yield return expression;` depends on the static type of the yielded expression *and* on the iterator method's declared return type:

- **Owned class-typed expression yielded from an iterator returning `IEnumerable<T>` or `IEnumerator<T>`.** The yield transfers ownership to the iterator's enumerator. The enumerator owns the most-recently-yielded value; the next `MoveNext()` (or `MoveNextAsync()`) call disposes the previous value before producing the next. The consumer's iteration variable in `for var x in src do ... end` is a **borrow** of the enumerator's owned slot, valid until the iteration body completes one full pass and `MoveNext` is called again. Disposing the enumerator (`break`, exhaustion, or explicit `Dispose()`) disposes any outstanding yielded value the iterator still owns.

  This is the rule that has applied since v0.78. It is the appropriate model for iterators whose consumers borrow each value briefly and discard it: filtering, formatting, dispatching to handlers.

- **Owned class-typed expression yielded from an iterator returning `OwnedEnumerable<T>`, `OwnedEnumerator<T>`, `OwnedAsyncEnumerable<T>`, or `OwnedAsyncEnumerator<T>`.** The yield transfers ownership *through* the enumerator to the consumer on each `MoveNext()` (or `MoveNextAsync()` for the async case). The enumerator does not retain ownership between yields — the value is handed off as part of the advance boundary. The consumer's iteration variable in `for var x in src do ... end` (or `await for var x in src do ... end` for the async case) is **owned** and must be discharged inside the loop body. Falling off the body without discharging is a compile error, with an error message identifying the iteration variable and pointing at the standard remedies (`using var u := x;` for scope-bound disposal, an `owned`-consuming method call, or an explicit `x.Dispose()`).

  This is the model for iterators whose consumers take ownership of each value: drain operations on owning collections (synchronous), async iterators producing owned resources (async pipelines such as `OwnedAsyncEnumerable<FileStream>`).

  The discharge requirement applies on every *normal-exit* edge from the loop body. The iteration variable is a local of the body's scope, and the standard owned-local discharge rules ([§28.3](#sec-local-variables-ownership-and-borrows)) apply to it directly: `break`, `continue`, `return`, `yield break`, and fall-through to the next iteration all count as normal-exit edges. The compiler requires the iteration variable to be discharged on every such edge between the binding point and the exit. The same rule applies inside an `await for` body. On *exception* edges — whether from an explicit `throw` or from a method call that propagates an exception out of the body — the iteration variable is auto-disposed by the implicit `try`/`finally` rule of [§28.3](#sec-local-variables-ownership-and-borrows) "Exception-path discharge"; no user-written discharge is required for the exception case. Code that conditionally `break`s before the natural discharge point must move the discharge before the conditional, typically by binding the iteration variable to a `using var` immediately on entry to the body:

  ```bezel
  for var conn in pool.Drain() do
    using var c := conn;        // discharge on entry — covers all exit edges
    if c.IsExpired() then
      break;                     // c disposed by using-var on the way out
    end;
    c.Use();
  end;
  ```

  The shape `using var c := <iteration variable>` is the standard way to convert the iteration variable's "must discharge on every edge" obligation into the simpler "scope-bound disposal" obligation. The implicit move-on-read from the iteration variable into the using-var is a single discharge that the compiler can verify dominates every exit; subsequent `break`/`continue`/`return`/exception edges are then clean.

  On `break`, on an exception thrown from the body, or on any other early exit from the for-loop, the enumerator's `Dispose()` runs. The enumerator at that point is between yields (it has produced values, the consumer has taken ownership of them, and the consumer's per-iteration discharge has either run or not). The enumerator has no per-yield ownership to release — every produced value either reached the consumer's owned slot (where the standard discharge rules apply) or was never produced. The enumerator's `Dispose()` runs only the per-iterator cleanup (closing files, releasing source resources) defined by the iterator method's body.

  When the iterator method's body sources the yielded values from an owning collection (the typical implementation of `Drain()`), it does so via the collection's removal operations — `PopFirst()`, `PopAt(i)`, `Pop()`, `Dequeue()`, etc. These operations remove and return the owned element in a single step, leaving the collection in a consistent post-call state without invoking the field-restoration rule. A naïve implementation using `move coll[i]` per slot without reassignment would *not* compile: each `move` leaves the slot in moved-from state, and the field-restoration rule ([§28.8.1](#sec-the-move-expression)) requires reassignment before the iterator method exits. The `Pop`-style operations sidestep the issue by removing the slot rather than emptying it. If the iterator is disposed before exhaustion — `break`, exception, manual `Dispose()` — the source collection's state is "consumed elements gone, remaining elements intact," consistent with the prefix actually iterated.

- **Borrowed class-typed expression.** The yield is a re-borrow whose source is the original borrow's source. The lifetime constraint propagates: the iterator's enumerator may not outlive the source. The compiler enforces this by attaching the borrow source to the enumerator's lifetime, just as for any other borrow-bearing structure. This case applies regardless of whether the iterator's return type is `IEnumerable<T>` or `OwnedEnumerable<T>` — but the latter is unusual; an iterator declared to yield owned T but yielding borrows would have to source the ownership from somewhere, typically by wrapping each borrow in a fresh allocation, which is rarely the intent.

- **Value-typed, `string`-typed, or runtime-managed expression.** No ownership transfers. The yield is a copy (for value types) or a shared reference (for `string` and other runtime-managed types). The enumerator does not dispose anything for these yield types; the next `MoveNext` simply produces the next value. The consumer's iteration variable is a copy (value types) or shared reference (runtime-managed) by the standard rules of those type kinds. The iterator's return type for these cases is `IEnumerable<T>`; the `OwnedEnumerable<T>` form is not available because `OwnedEnumerable<T>` requires `T: class` (stdlib [§4.13](Bezel_Standard_Library.md#sec-owning-enumeration-interfaces)). The class-only requirement is uniform across the owning collection family — owning makes sense only for class-typed values.

When the body holds a borrow across a `MoveNext` (or `MoveNextAsync`) boundary — for example, by stashing the iteration variable in an outer field — the borrow becomes invalid as soon as the next advance runs. This is the standard borrow-lifetime rule ([§28.3](#sec-local-variables-ownership-and-borrows)) applied to iterator-yielded values. To retain a yielded value across iterations, the consumer must materialize it explicitly: dispose-and-replace into an owned slot if the producer documents the yield as transferable, or copy if the value is a record. This rule applies to the *first* of the owned-class-typed cases above (iterator returns `IEnumerable<T>` or `IAsyncEnumerable<T>` and yields owned values); for iterators returning `OwnedEnumerable<T>`, `OwnedEnumerator<T>`, `OwnedAsyncEnumerable<T>`, or `OwnedAsyncEnumerator<T>`, the iteration variable is owned and the consumer is responsible for it for the body's duration regardless of advance timing — the value's lifetime is bounded by the consumer's discharge, not by the enumerator's next call.

Disposing the enumerator (`break` from a `for` or `await for` loop, exhaustion, or `Dispose()`/`DisposeAsync()` on a manually-held enumerator) disposes any outstanding yielded value the iterator owns. For yield types that the iterator does not own (value types, `string`, runtime-managed types, or any case where ownership transfers to the consumer per the rules above), there is nothing for the enumerator to dispose.

<a id="sec-iterator-methods"></a>
### 29.3 Iterator methods

A method that uses `yield return` or `yield break` is an **iterator method**. The compiler translates the body into the equivalent state-machine implementation of `IEnumerator<T>`.

```bezel
public method Squares(n: int): IEnumerable<int>
  for var i in 0..<n do
    yield return i * i;
  end;
end Squares;

public method TakeWhilePositive(numbers: IEnumerable<int>): IEnumerable<int>
  for var n in numbers do
    if n <= 0 then
      yield break;
    end;
    yield return n;
  end;
end TakeWhilePositive;
```

`yield return expression;` produces the next value and suspends. `yield break;` terminates iteration. Falling off the end of the method has the same effect as `yield break`.

An iterator method's return type is `IEnumerable<T>` (re-iterable: each call to `GetEnumerator()` runs the body again from the start) or `IEnumerator<T>` (single-use: the call returns the enumerator directly). An **async** iterator method (one declared `async` that uses `yield`; `await` is permitted but not required) has return type `IAsyncEnumerable<T>` (re-iterable) or `IAsyncEnumerator<T>` (single-use), the async counterparts of the synchronous forms ([§30.7](#sec-asynchronous-programming-async-iterators)).

<a id="sec-lazy-evaluation"></a>
### 29.4 Lazy evaluation

An iterator method's body does not run when the method is called. Calling produces an `IEnumerable<T>` (or `IEnumerator<T>`); the body runs incrementally as the consumer calls `MoveNext()`.

<a id="sec-return-not-allowed-in-iterator-methods"></a>
### 29.5 `return` not allowed in iterator methods

A method that uses `yield` cannot use plain `return`, including `return;`. Use `yield break` to exit early. This rule applies equally to synchronous iterators (returning `IEnumerable<T>`) and async iterators (declared `async`, returning `IAsyncEnumerable<T>`). Although async methods normally use `return` to complete a task, an async iterator is first and foremost an iterator — `yield` controls the flow, and `yield break` terminates it.

<a id="sec-restrictions-inside-try-catch-finally"></a>
### 29.6 Restrictions inside `try`/`catch`/`finally`

- `yield return` or `yield break` inside a `try` block whose surrounding statement has a `catch`: forbidden.
- `yield return` or `yield break` inside a `try` block whose surrounding statement has only a `finally`: allowed.
- `yield` inside a catch arm's body or a `finally` block: forbidden.

`using var` compiles to `try`/`finally` (no `catch`), so it is the preferred resource-management mechanism inside iterators.

<a id="sec-disposal-of-partially-consumed-iterators"></a>
### 29.7 Disposal of partially-consumed iterators

When a consumer stops iterating early — `break`, exception, or `using var` ending before exhaustion — the iterator is disposed. The state machine:

- Calls `Dispose()` on every `using var` active at the point of suspension.
- Runs `finally` clauses active at the point of suspension.
- Transitions to a completed state; subsequent `MoveNext()` returns `false`; second `Dispose()` is a no-op.

<a id="sec-iterators-with-owned-parameters"></a>
### 29.8 Iterators with `owned` parameters

An iterator method may take `owned` parameters. The state machine takes ownership at construction time. When the iterator is disposed, the state machine disposes any `owned` parameter that has not been moved out of by the body.

The body may treat the `owned` parameter as it would in any other method. The example below uses `using var` to make scope-bounded disposal explicit; this transfers ownership from the parameter to the local, after which the parameter is in moved-from state and the state machine's auto-dispose does not run for it (the local's `using var` does the work):

```bezel
public method ReadAndClose(owned reader: StreamReader): IEnumerable<string>
  using var r := reader;                  // ownership moves: parameter → local
  while not r.AtEnd do
    yield return r.ReadLine();
  end;
  // r disposed by using var when the iterator completes or is disposed early.
end ReadAndClose;
```

If the body does not move ownership out, the state machine disposes the parameter at iterator disposal — after `using var` cleanups within the body and after `finally` clauses. The order across multiple owned parameters is reverse declaration order, mirroring [§28.4](#sec-the-destructor)'s destructor rule.

The same machinery handles iterator parameters of move-only value type (owning inline arrays, [§15.3](#sec-move-only-value-types)). The parameter is moved into the state machine at construction time; the body sees a move-only local; at iterator disposal, the state machine auto-disposes the array's remaining non-moved-from slots in the same pass that handles owned class-typed parameters. Slots the body moved out of (via `move arr[i]` and onward transfer) are not in the state machine's responsibility set; the field-restoration rule of [§28.8.1](#sec-the-move-expression) governs the body's discipline.

```bezel
public method Echo(owned source: StreamReader): IEnumerable<string>
  while not source.AtEnd do
    yield return source.ReadLine();
  end;
  // No using var; source is disposed by the iterator state machine
  // when the consumer disposes the enumerator.
end Echo;
```

Moving the parameter out of the iterator (via return, an `owned` argument call, or a `using var` re-bind) puts the parameter in the moved-from state, and the state machine's auto-dispose for that parameter does not run.

<a id="sec-yield-not-allowed-in-lambdas"></a>
### 29.9 `yield` not allowed in lambdas

`yield` is a method-level construct only.

<a id="sec-empty-iterators"></a>
### 29.10 Empty iterators

An iterator method with only `yield break;` is the canonical empty iterator. An empty body (no `yield`) is not an iterator method.

```bezel
public method Empty<T>(): IEnumerable<T>
  yield break;
end Empty;
```

<a id="sec-multiple-enumeration"></a>
### 29.11 Multiple enumeration

Each `GetEnumerator()` call produces an independent enumerator. Iterating the same source twice with two `for` loops works as expected for collection types.

For lazy or single-pass iterables (a streaming reader), repeat enumeration is a property of the specific implementation.

<a id="sec-mutation-during-iteration"></a>
### 29.12 Mutation during iteration

Modifying a collection while iterating it invalidates the active enumerator. Standard collections detect this and throw `InvalidOperationException` on the next `MoveNext()`.

The standard pattern for removing during iteration:

```bezel
var toRemove := new Array<Transaction>();
for var t in transactions do
  if t.IsExpired then toRemove.Add(t); end;
end;
for var t in toRemove do
  transactions.Remove(t);
end;
```

<a id="sec-async-iterators"></a>
### 29.13 Async iterators

`await for` iterates `IAsyncEnumerable<T>` (borrowing) and `OwnedAsyncEnumerable<T>` (owning) sources. See [§30.7](#sec-asynchronous-programming-async-iterators).

---

<a id="sec-asynchronous-programming"></a>
## 30. Asynchronous Programming

Asynchronous methods run within an *execution domain* — a unit of serial execution scheduled by the runtime ([§30.13](#sec-runtime-model)). The compiler transforms an async method's body into a state machine that suspends and resumes at each `await`.

<a id="sec-async-methods"></a>
### 30.1 `async` methods

A method declared `async` may use `await`. The return type must be one of:

- `Task` — no result value.
- `Task<T>` for some type T — a result of type T, with no transferable ownership obligation on the awaiter.
- `OwnedTask<T>` for some class type T — a result whose ownership transfers to the awaiter ([§30.5](#sec-result-lifetime-and-task-disposal)).
- `IAsyncEnumerable<T>` — for re-iterable async iterators that contain `yield return` ([§30.7](#sec-asynchronous-programming-async-iterators)). The yielded values are borrowed by the consumer.
- `IAsyncEnumerator<T>` — for single-use async iterators with borrowed-by-consumer values.
- `OwnedAsyncEnumerable<T>` for class T — for async iterators that yield owned values; each consumer iteration variable is owned and must be discharged inside the loop body.
- `OwnedAsyncEnumerator<T>` for class T — single-use owned-yielding async iterator.

```bezel
public async method DownloadAsync(url: string): Task<string>
  using var client := new HttpClient();
  using var task := client.GetAsync(url);
  return await task;
end DownloadAsync;
```

Calling an async method begins execution synchronously on the calling thread until the first `await` of an incomplete task, at which point the method suspends and returns its `Task` to the caller. If the method completes before encountering an incomplete `await`, the returned task is already in a completed state.

`async` may appear on instance methods, static methods, and lambdas. It may not appear on constructors, destructors, or property/indexer accessors.

Modifier order: `visibility static async ... method ...`.

An async method's `return expression;` completes the task with the value. `return;` (in `Task`-returning forms) completes the task with no value. Falling off the end of a `Task`-returning async method is equivalent to `return;`. A `Task<T>`-returning async method must `return` a value on every path.

<a id="sec-await"></a>
### 30.2 `await`

`await` suspends the current async method until a task completes:

```bezel
var result := await someTask;
```

The shape above is the simple case — appropriate when `someTask` returns a value type, `string`, or `Task` (no value). When the task's result type is a class, one of the patterns in [§30.5](#sec-result-lifetime-and-task-disposal) applies; the bare assignment shown here is rejected.

Behavior:

- If the task is already completed, `await` returns the result synchronously.
- If the task is faulted, `await` re-raises the exception.
- If the task was cancelled, `await` raises `OperationCanceledException`.
- If the task is running, the async method suspends; when the task completes, the continuation is queued to its domain ([§30.13](#sec-runtime-model)).

`await` is valid only inside an `async` method or `async` lambda.

<a id="sec-the-task-and-taskt-types"></a>
<a id="sec-the-task-taskt-and-ownedtaskt-types"></a>
### 30.3 The `Task`, `Task<T>`, and `OwnedTask<T>` types

```bezel
public class Task
  public readonly property IsCompleted: bool;
  public readonly property IsFaulted: bool;
  public readonly property IsCancelled: bool;

  public method Wait();

  destructor;       // runs continuation cleanup and disposes any
                    //   unconsumed `owned` result. Task is non-trivial-
                    //   dispose, which is why discard `_ := someAsyncCall();`
                    //   is rejected ([§17.13](#sec-the-discard-_)).
end Task;

public class Task<T>: Task
  public readonly property Result: T;
end Task;

public class OwnedTask<T>: Task where T: class
  // Owning task: result transfers ownership to the awaiter (Pattern B).
  // Single-consumer by type. Reading the Result property is rejected
  // because Result would require borrow-source rules that this class
  // does not provide; consume via `await` only.
end OwnedTask;
```

`Wait()` and `Result` are synchronous escape hatches for non-async code. They block the calling thread until the task completes; inside async methods, prefer `await`. Reading `Result` on a faulted task re-raises the exception; reading `Result` on a cancelled task raises `OperationCanceledException`.

`OwnedTask<T>` does not expose a `Result` property — the result is single-consumer and ownership-transferring, so a "read result" operation would have no well-defined semantics (a second read would yield a moved-from value). Consumers must `await` an `OwnedTask<T>` in an owned-consuming position to obtain the result (see [§30.5](#sec-result-lifetime-and-task-disposal) Pattern B).

`Task`, `Task<T>`, and `OwnedTask<T>` are classes — they require disposal, with the patterns described in [§30.5](#sec-result-lifetime-and-task-disposal). Listing `IDisposable` explicitly in the inheritance clause would be redundant since every class implicitly implements it ([§28.2](#sec-the-idisposable-contract)). The `destructor;` form above follows the standard library documentation-shorthand convention (stdlib [§1](Bezel_Standard_Library.md#sec-conventions)); the actual destructor body is provided by the runtime and runs the continuation-cleanup logic described in [§30.10](#sec-standard-library-task-helpers) (the `FireAndForget` and `using var`-disposal-without-await cases).

<a id="sec-multiple-awaiters"></a>
### 30.4 Multiple awaiters

A `Task` or `Task<T>` may be awaited from multiple places, sequentially or concurrently. Every awaiter sees the same eventual state. The underlying computation runs once. Continuations resume in unspecified order, but never concurrently with one another within a domain ([§30.13](#sec-runtime-model)).

`OwnedTask<T>` is single-consumer per the Pattern B rules in [§30.5](#sec-result-lifetime-and-task-disposal): once awaited for a successful (non-faulted, non-cancelled) result, the task is spent, and a second await raises `InvalidOperationException`. Sharing an `OwnedTask<T>` across multiple potential awaiters is not the intended pattern. For fan-out with a class-typed result, use `Task<T>` (Pattern A); for single-consumer ownership-transferring resource production, use `OwnedTask<T>` (Pattern B).

<a id="sec-result-lifetime-and-task-disposal"></a>
### 30.5 Result lifetime and task disposal

The task owns any class-typed result it produces. The pattern by which a result is consumed is selected by the producer's return type — there is no syntactic peeking at `await`'s context.

<a id="sec-producer-side-return-type-rules"></a>
#### Producer-side return-type rules

- `Task` and `Task<T>` for value type, `string`, or runtime-managed T: produced as today; no ownership obligation crosses the await boundary.
- `Task<T>` for **class** T: the task may be awaited from multiple places. Every awaiter receives a borrow of the same underlying result. The result's lifetime is tied to the task — disposing the task disposes the result. This is **Pattern A**.
- `OwnedTask<T>` for **class** T: the task produces a single owned result. Awaiting transfers ownership to the awaiter. The task may be awaited at most once for a successful (non-faulted, non-cancelled) result. This is **Pattern B**.
- `OwnedTask<T>` for value type, `string`, or runtime-managed T: a compile error. `owned` on a non-class result is meaningless.

`OwnedTask<T>` is the right choice for resource acquisition — operations whose entire purpose is to produce a fresh, single-consumer resource (a connection, a lock token, a parsed document). `Task<T>` is the right choice for fan-out: multiple subscribers reading the same eventual result.

<a id="sec-consumer-side-rules"></a>
#### Consumer-side rules

For `await someTask`, the consumer's obligations are determined by the task's static return type:

**Value type, `string`, or runtime-managed result:** any form is permitted. The result is a value or shared-immutable reference.

```bezel
var n := await CountBytesAsync(url);     // result is int
var s := await FetchStringAsync(url);    // result is string
```

For these forms, the awaited task expression may be a bare producer call (as above); the await operator is **owned-consuming with respect to its operand** when the operand is an owned task value. The implicit task-disposal performed by the runtime as part of the await satisfies the discharge requirement on the operand — the user does not write a separate `using var` for the task. This parallels the Pattern B operand rule below, except that here no ownership obligation crosses the await boundary on the *result*; only the task value itself is consumed.

**`Task<T>` for class T (Pattern A):** the awaited task expression must have a *syntactically-visible owning location* whose lifetime the compiler can bound, so that the borrow produced by the await has a determinable lifetime. The general rule:

> The awaited task expression must be either (a) a *reference to* an owning location whose lifetime is visible at the await site (such locations include using-var-bound locals, owned parameters, owned fields of `this`, and owned-element collection slots), or (b) a producer call whose result is itself directly consumed by a Pattern A return in the enclosing async method. In all other positions — bare producer calls in argument, var-binding, or expression positions — the borrowed result would have no syntactically-visible source, and the await is a compile error.

The principle resolves into the following examples of admitted task-expression positions; the list is not exhaustive but illustrates the rule:

- **A reference to a `using var` or `await using var`-bound local** of static type `Task<T>`. The canonical form. The await produces a borrow whose source is the local; the borrow is valid within the local's scope.
- **A reference to an `owned` parameter** of the enclosing method, of static type `Task<T>`. The parameter's lifetime spans the method body; the borrow is valid within the body.
- **A reference to an `owned` field** of `this`, of static type `Task<T>`. The field's lifetime spans `this`'s lifetime, and the borrow is valid within method calls on `this`.
- **An indexer access on an owning collection** whose static type is `OwnedArray<Task<T>>` (or analogous), where the collection itself occupies an owning location. The await produces a borrow whose source is the collection; the borrow is valid within the collection's lifetime. The same applies to dictionary indexer access into `OwnedDictionary<K, Task<T>>`.
- **An iteration variable** in `for var task in tasks do ... end` where `tasks` is `Array<Task<T>>` or `OwnedArray<Task<T>>` (borrowing iteration, not Drain). Each `task` is a borrow whose source is `tasks`; the await within the loop body produces a borrow valid for the loop body.
- **A producer call directly inside `return` of an async method whose return type is `Task<U>` for class U** (Pattern A) — i.e., `return await Producer();`. The producer's task is owned by the enclosing state machine; the borrow flows into the outer Pattern A task's result slot, whose lifetime is bounded by the outer task.

The principle the list illustrates: any expression that produces an owned-task value (whether obtained directly or by borrowing into an owning slot) has a source the compiler can name. The principle also rejects: bare producer calls in argument or other expression positions, e.g. `Process(await Producer())` or `var x := await Producer();` without binding the task first. The borrowed result of these forms would have no syntactically-visible source whose lifetime the compiler can bound. The fix is to bind the producer's result to a `using var` first:

```bezel
using var task := Producer();
Process(await task);
```

The borrow produced by `await task` (where `task` occupies one of the admitted positions) may flow to:

- A `var` binding: `var r := await task;` — `r` is a non-owned local with the same source as `task`.
- A non-owned argument: `Process(await task);`.
- A `return` expression whose return type accepts a borrow with the same source (e.g., `return await task;` from an async method returning Pattern A `Task<T>`).

The borrow may **not** flow to:

- A `using var` binding: `using var r := await task;` is rejected — `using var` requires an owned right-hand side, but the await produced a borrow.
- An assignment to an `owned` field or owned-element collection slot — would manufacture ownership the source did not grant.
- An `owned` parameter argument.
- A `return` expression whose return type is `owned T` or carries ownership.

```bezel
using var task := FetchAsync(url);
var response := await task;              // borrowed reference, OK
ProcessResponse(response);
// At end of scope: task is disposed; response disposed with it.
```

A bare `var x := await someTask;` where `someTask` is not in one of the admitted positions is a compile error. The error message identifies the offending await and references the principle above; the typical fix is to introduce a `using var task := Producer(); var x := await task;` pair.

**`OwnedTask<T>` for class T (Pattern B):** the await must appear in an **owned-consuming context**. Owned-consuming contexts are positions in the AST where the type system requires the value reaching that position to be discharged. They include:

- The right-hand side of `using var x := await ...` or `await using var x := await ...` — the canonical form. Ownership of the result transfers to the declared local.
- The right-hand side of `var x := await ...` — `var` introduces an owned local of class type T when the right-hand side is owned, and the standard owned-local discharge rules ([§28.3](#sec-local-variables-ownership-and-borrows)) apply to `x` from the binding point forward.
- The right-hand side of an assignment to an owned-element collection slot via the indexer setter (`coll[i] := await ...`), or to an `owned` field that is in moved-from state at the assignment point (`field := await ...` after `move field`).
- An argument expression bound to an `owned` parameter of a method call, e.g., `coll.Add(await someTask)` where `Add(owned item: T)`.
- A `return` expression in a method whose return type is `owned T` (or whose return type wraps `owned T` such as `OwnedTask<T>`).
- A `yield return` expression in an iterator method whose return type is `OwnedEnumerable<T>`, `OwnedEnumerator<T>`, `OwnedAsyncEnumerable<T>`, or `OwnedAsyncEnumerator<T>`. Ownership transfers from the yielding method through the enumerator to the consumer per [§29.2](#sec-desugaring-of-for) "Iterator yield ownership."

The task is disposed implicitly without disposing its result; it cannot be awaited again, and a second await raises `InvalidOperationException`.

The await operator on a `OwnedTask<T>` for class T is itself **owned-consuming with respect to its operand**: when the awaited expression is an owned task value (an owned local of `OwnedTask<T>`, an iteration variable from draining an `OwnedArray<OwnedTask<T>>`, an `owned` parameter, or any other owned-typed expression of `OwnedTask<T>`), the await consumes the task as part of the operation and satisfies the discharge requirement on the operand. The implicit task-disposal that the runtime performs is the same operation that satisfies the type system's discharge rule; one is the runtime view, the other is the static-analysis view of the same event.

In practice this means the natural patterns work without explicit task disposal:

```bezel
// Iteration variable from draining: task is owned, await consumes it
for var task in taskArray.Drain() do
  results.Add(await task);     // task discharged by the await; no further obligation
end;

// Local variable of owned task type
var theTask := SomeFactory();   // theTask: owned OwnedTask<T>
using var result := await theTask;  // theTask discharged by the await
```

In neither case does the user need a separate `using var` to discharge the task itself — the await's implicit disposal *is* the discharge.

Pattern B tasks are single-consumer by type — once awaited they are spent, and sharing a `OwnedTask<T>` across multiple potential awaiters is not the intended pattern. In the typical case the operand is owned (an owned local, an owned parameter, or an iteration variable from draining an array of tasks), and the discharge rule applies as described above. When the operand is borrowed, the await still disposes the task implicitly per the runtime semantics, but no discharge is owed for it. The `using var task := SomeFactory(); var x := await task;` shape, where `task` is `using var`-bound and then awaited, is one common form; the await's implicit disposal happens first, and the using-var's disposal at scope end is a no-op per the idempotency rule ([§28.2](#sec-the-idisposable-contract)).

```bezel
// Canonical form: using-var binding
using var token := await semaphore.AcquireAsync();
DoCriticalWork();
// At end of scope: token disposed, releasing the semaphore.

// Plain var binding — x is an owned local subject to standard discharge rules
var token := await semaphore.AcquireAsync();
TransferToHelper(token);    // owned local consumed by owned parameter

// Direct ownership transfer to a method call accepting owned
results.Add(await loadTask);     // results: OwnedArray<T>; Add takes owned

// Direct return
public async method Acquire(): OwnedTask<Connection>
  return await ConnectAsync(url);     // owned T flows to owned T return
end Acquire;
```

A bare `var x := await someTask;` *does* compile under v0.90: `x` is an owned local, and the standard owned-local discharge rules then require `x` to reach an owned location before scope exit. The compile error appears later — at scope exit, if `x` was never discharged. Earlier (pre-v0.89) versions rejected `var x := await someTask` outright at the await site; v0.90 defers to the standard owned-local rules instead, which produces the same safety with a more uniform diagnostic.

The earlier (pre-v0.89) wording of this rule required the await to be the immediate operand of `using var` or `await using var` specifically. v0.89 loosened the rule to admit any owned-consuming context; v0.90 simplifies the list by recognizing that `var x := <owned>` is itself an owned-consuming context (it produces an owned local). The rule's safety is unchanged: every owned-consuming context is one whose type system already requires the produced value to be discharged.

For an asynchronously-disposable owned result, use `await using var`:

```bezel
// ConnectAsync is illustrative; see §28.12 note.
await using var connection := await ConnectAsync(url);
await connection.SendAsync(data);
// At end of scope: connection.DisposeAsync() awaited.
```

For a `OwnedTask<T>` not awaited in an owned-consuming context — for example, an `await` whose result is discarded, or appears in a non-owned-consuming expression position — the await is a compile error. The error message names the owned-consuming positions admitted by the rule above and points to the `using var` form as the canonical choice.

<a id="sec-disposing-an-unconsumed-ownedtaskt"></a>
#### Disposing an unconsumed `OwnedTask<T>`

If a `OwnedTask<T>` is dropped or disposed before its result has been transferred — for example, by `using var` on the task itself with no await — disposing the task disposes the result if one was produced. This makes "fire and forget then drop" safe for owned-result tasks.

```bezel
do
  using var t := semaphore.AcquireAsync();
  // No await; nothing transferred. If t completes before we leave the block,
  // its produced token is disposed (the semaphore is released).
end;
```

When the task has not completed at the point of disposal, the `Dispose()` call attaches a continuation that disposes the produced result whenever it appears, then returns immediately. The dispose call does not block waiting for completion. The underlying operation continues to run; any side effects of the operation still occur, and the result, when produced, is disposed by the continuation. The task transitions to its terminal disposed state immediately for the purposes of any further awaits — a second await on a disposed task raises `InvalidOperationException`.

A task that has been awaited and faulted, or awaited and cancelled, has no owned result to dispose; disposal is a no-op for the result.

<a id="sec-multiple-awaits-on-ownedtaskt"></a>
#### Multiple awaits on `OwnedTask<T>`

A `OwnedTask<T>` is single-consumer by type. Awaiting one twice raises `InvalidOperationException` on the second await even when the first await completed successfully — the result has already been moved out.

For a `Task<T>` (no `owned`) for class T, multiple awaits are permitted ([§30.4](#sec-multiple-awaiters)); each receives a borrow.

<a id="sec-constructing-a-completed-task-with-a-class-typed-result"></a>
#### Constructing a completed task with a class-typed result

The factory `Task.FromResult<T>(value: T)` produces a completed task whose result is `value`. It is restricted to non-class T (value types, `string`, runtime-managed reference types); calling it with a class-typed T is a compile error.

For class T, use `Task.FromOwnedResult<T>(owned value: T): OwnedTask<T>` instead. The caller transfers ownership of `value` to the task; the produced task is Pattern B (single-consumer, owned result), and consumers await it under the `using var x := await ...` shape:

```bezel
Task.FromResult(42);                               // T = int, value-typed
Task.FromResult("done");                           // T = string, runtime-managed
Task.FromOwnedResult<MyClass>(new MyClass());      // T = MyClass, returns OwnedTask<MyClass>
```

The same split applies to `TaskCompletionSource<T>`: the standard form is for non-class T (value, string, runtime-managed) and produces `Task<T>`; `OwnedTaskCompletionSource<T> where T: class` is the form for class T and produces `OwnedTask<T>`. See [§30.10](#sec-standard-library-task-helpers) and [§30.11](#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet).

There is no factory for producing a Pattern A `Task<T>` of class T from an externally-supplied owned value; Pattern A class-typed tasks arise from async methods whose body returns a value (the compiler inserts the appropriate ownership transfer into the task's slot) and from awaiting other Pattern A sources. Imperative completion of a Pattern A class-typed task is a future-revision question; the cases that motivate it are uncommon, and the available patterns (writing an async wrapper that awaits an `OwnedTaskCompletionSource` and re-publishes the result) cover the practical needs.

<a id="sec-idiom-async-resource-acquisition"></a>
#### Idiom: async resource acquisition

The combination of `OwnedTask<T>` and Pattern B is what makes resource-acquisition APIs natural:

```bezel
public class Semaphore
  public async method AcquireAsync(): OwnedTask<SemaphoreToken>;
end Semaphore;

// Caller:
using var token := await sem.AcquireAsync();
DoWork();
// token disposed at scope end -> semaphore released.
```

Compare to the v0.52 workaround ("allocate synchronously, initialize asynchronously"), which had no synchronous step for *acquisition*. The owned-result task expresses the intent directly.

<a id="sec-async-lambdas"></a>
### 30.6 Async lambdas

```bezel
var fetcher: (string) -> Task<string> := async (url) => do
  using var client := new HttpClient();
  using var task := client.GetAsync(url);
  return await task;
end;

var quick: (string) -> Task<int> := async (url) => await CountBytesAsync(url);

var loader: (string) -> OwnedTask<Resource> := async (path) => do
  return await LoadResourceAsync(path);     // Pattern B return; the lambda's
                                             //   state machine forwards the
                                             //   owned Resource through.
end;
```

The function type of an async lambda has a `Task`, `Task<T>`, or `OwnedTask<T>` return type. The choice follows the same rules as for async methods ([§30.1](#sec-async-methods)): `Task` for void, `Task<T>` for borrowing or value-typed results, `OwnedTask<T>` for class-typed results that transfer ownership to the awaiter (Pattern B; see [§30.5](#sec-result-lifetime-and-task-disposal)).

<a id="sec-asynchronous-programming-async-iterators"></a>
### 30.7 Async iterators

An async iterator combines `yield` and `await`. The method is `async` and returns `IAsyncEnumerable<T>`:

```bezel
public async method ReadLinesAsync(path: string): IAsyncEnumerable<string>
  // OpenAsync, ReadLineAsync, and AtEnd are illustrative; the example shows
  // the async-iterator shape, not a stream API in the standard library.
  await using var fs := await OpenAsync(path);
  while not fs.AtEnd do
    yield return await fs.ReadLineAsync();
  end;
end ReadLinesAsync;
```

```bezel
interface IAsyncEnumerable<out T>
  method GetAsyncEnumerator(token: CancellationToken): owned IAsyncEnumerator<T>;
end IAsyncEnumerable;

interface IAsyncEnumerator<out T>: IAsyncDisposable
  method MoveNextAsync(): Task<bool>;
  readonly property Current: T;
end IAsyncEnumerator;
```

Consume with `await for`:

```bezel
public async method ProcessFileAsync(path: string): Task
  await for var line in ReadLinesAsync(path) do
    Process(line);
  end;
end ProcessFileAsync;
```

`await for` is valid only inside an async method or async lambda. It desugars analogously to `for`: it acquires an async enumerator with `await using`, calls `MoveNextAsync()` with `await` in a loop.

<a id="sec-cancellation-in-await-for"></a>
#### Cancellation in `await for`

`await for` accepts an optional `with` clause to pass a `CancellationToken` to the async enumerator:

```bezel
await for var line in ReadLinesAsync(path) with token do
  Process(line);
end;
```

The `with token` clause passes `token` as the argument to `GetAsyncEnumerator(token)`. When omitted, `CancellationToken.None` is passed. The `with` clause is syntactically distinct from the record `with` expression — it appears after the `in` source expression and before `do`, and its only legal content is a single expression of type `CancellationToken`.

<a id="sec-async-iteration-with-owned-yields"></a>
#### Async iteration with owned yields

For async iterators that produce owned values, declare the return type as `OwnedAsyncEnumerable<T>` (where `T: class`) and `await for` over the result:

```bezel
public async method OpenFilesAsync(paths: IEnumerable<string>): OwnedAsyncEnumerable<FileStream>
  for var path in paths do
    yield return await OpenAsync(path);    // each yielded FileStream is owned
  end;
end OpenFilesAsync;

public async method ProcessAllAsync(paths: IEnumerable<string>): Task
  await for var fs in OpenFilesAsync(paths) do
    using var f := fs;                      // discharge owned iteration variable
    await ProcessOneAsync(f);
  end;
end ProcessAllAsync;
```

The `await for` desugaring picks `GetOwnedAsyncEnumerator()` (and the destructive `Current`) when the source has static type `OwnedAsyncEnumerable<T>`. Each iteration variable is owned and must be discharged on every exit edge of the loop body — the same rule as for synchronous `for` over `OwnedEnumerable<T>` ([§29.2](#sec-desugaring-of-for) "Iterator yield ownership"). When the source implements both `IAsyncEnumerable<T>` and `OwnedAsyncEnumerable<T>`, the static type of the source expression selects the desugaring; default iteration uses `IAsyncEnumerable<T>` (borrowing), and the owning path requires the source to be typed `OwnedAsyncEnumerable<T>` explicitly.

The `with token` clause works the same way for owning async iterators — `await for var fs in OpenFilesAsync(paths) with token do ...` passes the token to `GetOwnedAsyncEnumerator(token)`.

**`await` inside `finally` (clarified in v0.137).** Inside an async iterator, `await` may not appear in a `finally` block — a compile error. The `yield` placement restrictions of [§29.6](#sec-restrictions-inside-try-catch-finally) apply unchanged; this rule additionally restricts `await`, and only inside async *iterators* (an ordinary `async` method may await in its `finally`, [§30.9](#sec-exception-handling-in-async-methods)). The reason is disposal: when a consumer abandons iteration early, the iterator's pending `finally` blocks run synchronously inside `Dispose` ([§29.7](#sec-disposal-of-partially-consumed-iterators)) — there is no async driver at that point, so a suspension inside the `finally` would have nowhere to resume; the continuation would be silently discarded, which is precisely the defined-fault philosophy's forbidden outcome. A `finally` that needs asynchronous cleanup awaits *before* yielding (hoisting the resource acquisition), or the type implements the cleanup in its `Dispose` path. This mirrors the catch-guard rule of [§30.9](#sec-exception-handling-in-async-methods): positions evaluated during synchronous unwinding or dispatch are synchronous.
### 30.8 Cancellation

Cancellation is cooperative via `CancellationToken`. The standard library defines the surface (the full declaration is in std lib [§6.1](Bezel_Standard_Library.md#sec-cancellationtoken-and-cancellationtokensource); the members referenced by the language rules are shown here):

```bezel
public record CancellationToken
  public static readonly property None: CancellationToken;
  public readonly property IsCancellationRequested: bool;
  public method ThrowIfCancellationRequested();
  public method Register(callback: () -> ()): owned Subscription;
end CancellationToken;

public class CancellationTokenSource: IDisposable
  public readonly property Token: CancellationToken;
  public readonly property IsCancellationRequested: bool;
  public method Cancel();
  public method CancelAfter(delay: TimeSpan);
end CancellationTokenSource;
```

`CancellationToken` is a record holding a borrowed reference to its source. The token observes the source's state; when `Cancel()` is called on the source, all tokens derived from it report `IsCancellationRequested == true`. Per the record-disposal rules ([§9.7](#sec-records-and-disposal)), the token does not own the source; the source must outlive any tokens that observe it. For typical short-lived uses (token passed as a parameter to async calls that complete before the source is disposed), this is automatic.

Reading `IsCancellationRequested` on a `CancellationToken` involves reading through the borrowed reference to the `CancellationTokenSource`. This read is guaranteed to be atomic and to observe the effect of a `Cancel()` call from another thread without additional synchronization — the runtime ensures the necessary memory ordering. The `ThrowIfCancellationRequested()` method is similarly thread-safe.

`CancellationToken` is auto-imported as a language primitive ([§2.3](#sec-always-available-names)). `CancellationTokenSource` and the other threading types live in `System.Threading` and require explicit `import System.Threading;`.

`CancellationToken.None` (a token whose source never cancels) is provided for callers that do not want cancellation.

By convention, async methods that should be cancellable accept a `CancellationToken` parameter and pass it to inner async calls. Cancellation typically happens at `await` points: the inner call observes the request and completes with `OperationCanceledException`.

<a id="sec-exception-handling-in-async-methods"></a>
### 30.9 Exception handling in async methods

- An exception thrown in the synchronous prefix of an async method (before any `await`) faults the returned task, not the immediate caller.
- An exception thrown after an `await` faults the task.
- Awaiting a faulted task re-raises the exception with the original stack trace preserved.
- `try`/`catch` inside an async method catches exceptions normally, including those raised by `await`.

`await` has no syntactic restrictions on its placement inside `try`/`catch`/`finally` *bodies*. The state machine handles suspension across exception-handling boundaries correctly.

Catch **guards** are the one exception: a `when` guard on a catch arm ([§19.4](#sec-guards)) is evaluated synchronously during exception dispatch and may not contain `await` (a compile error, in async and sync methods alike). Exception dispatch — deciding *which* arm handles a raised exception — runs to completion without suspension; permitting a suspension point inside dispatch would leave the exception neither caught nor propagating across the suspension. A guard that needs asynchronous information awaits it *before* the `try`, or catches unconditionally and re-raises (`throw;`, [§21.8](#sec-throw)) after an awaited check in the arm body.

```bezel
public async method WithCleanupAsync(): Task
  try
    await DoWorkAsync();
  catch
    var ex: Exception => do
      await LogErrorAsync(ex);
      throw;
    end
  finally
    await ReleaseResourcesAsync();
  end;
end WithCleanupAsync;
```

The `do ... end` block is required here because the arm body has multiple statements. When the body is just a re-raise, the bare `throw` expression makes the arm a one-liner:

```bezel
catch
  var ex: TransientException => Retry(ex),
  var ex: Exception => throw          // re-raise unrecognized exceptions
end;
```

<a id="sec-standard-library-task-helpers"></a>
### 30.10 Standard library task helpers

The standard library provides:

- `Task.FromResult<T>(value: T): Task<T>` — completed task with a value. Restricted to non-class T (value types, `string`, runtime-managed reference types). For class T, see `Task.FromOwnedResult` below; calling `FromResult` with a class-typed T is a compile error pointing to the owned variant.
- `Task.FromOwnedResult<T>(owned value: T): OwnedTask<T> where T: class` — completed Pattern B task whose owned result is `value`. The caller transfers ownership of `value` to the task. Awaiters consume the task under Pattern B ([§30.5](#sec-result-lifetime-and-task-disposal)): the await must appear in an owned-consuming context (`using var`, an `owned` parameter, an `owned` return, etc.), and ownership transfers to that context. Use this when constructing a completed task from a freshly-acquired or freshly-constructed class instance that the awaiter should own.
- `Task.FromException<T>(exception: Exception): Task<T>` and the non-generic `Task` form.
- `Task.FromCancelled<T>(): Task<T>` and the non-generic form.
- `Task.Delay(milliseconds: int): Task` and `Task.Delay(duration: TimeSpan): Task`, plus overloads of each taking a `CancellationToken`.
- `Task.WhenAll(tasks: Array<Task>): Task` — completes when all tasks complete.
- `Task.WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>` — homogeneous result type for non-class T.
- `Task.WhenAllOwned<T>(owned tasks: OwnedArray<OwnedTask<T>>): OwnedTask<OwnedArray<T>> where T: class` — Pattern B aggregator. The caller transfers ownership of the array (and its tasks); the result is an owned array of owned elements. Each task is awaited in turn (sequentially in the implementation, but the underlying operations run concurrently because the tasks were started before being passed in). See std lib [§6.7](Bezel_Standard_Library.md#sec-task-static-helpers) for a worked example.
- `Task.WhenAny(tasks: Array<Task>): Task<Task>` — completes when the first task completes.
- `Task.FireAndForget(owned task: Task)` — registers a continuation that logs or handles any exception the task produces, then disposes the task. The calling code relinquishes ownership; the runtime supervises the task's lifecycle. **`Task.FireAndForget` is the only supported way to discard a `Task` value** — the discard form `_ := someAsyncCall();` is rejected at compile time because `Task` has a destructor, and abandoning a `Task` reference would skip its continuation cleanup. The two legal patterns for "I don't want to await this task" are `Task.FireAndForget(call())` (supervised: exceptions thrown by the task are logged) and `using var t := call();` (unsupervised: the task is disposed at scope end without blocking, and any exceptions it raises are silently swallowed; see [§30.5](#sec-result-lifetime-and-task-disposal) for the disposal-of-unconsumed-task semantics).

These compose with the rest of the async machinery; awaiting them works like awaiting any other task.

<a id="sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet"></a>
### 30.11 `TaskCompletionSource<T>` and `OwnedTaskCompletionSource<T>`

A `TaskCompletionSource<T>` produces a task whose state is controlled imperatively. The standard use is wrapping callback-based or event-based APIs into awaitable form.

The split into two classes — one for non-class T, one for class T with ownership transfer — exists for the same reason as the `FromResult` / `FromOwnedResult` split. For non-class T (value, string, runtime-managed), the produced `Task<T>` carries no ownership across the await boundary; the standard `TaskCompletionSource<T>` is sufficient. For class T, the result must be owned by something — and the natural shape is for the produced task to be Pattern B (`OwnedTask<T>`, [§30.5](#sec-result-lifetime-and-task-disposal)): the source takes ownership of the result on completion, and the eventual awaiter takes ownership through the `using var x := await ...` shape. `OwnedTaskCompletionSource<T> where T: class` produces this shape.

<a id="sec-taskcompletionsourcet-—-for-non-class-t"></a>
#### `TaskCompletionSource<T>` — for non-class T

```bezel
public class TaskCompletionSource<T>
  // Intrinsic restriction: T must be a value type, string, or other
  // runtime-managed reference type. Instantiating with a class T is a
  // compile error pointing to OwnedTaskCompletionSource<T>.
  public constructor();
  public readonly property Task: Task<T>;

  public method SetResult(result: T);
  public method SetException(exception: Exception);
  public method SetCancelled();

  public method TrySetResult(result: T): bool;
  public method TrySetException(exception: Exception): bool;
  public method TrySetCancelled(): bool;
end TaskCompletionSource;
```

A non-generic `TaskCompletionSource` (for `Task` with no result) has the same shape minus the `result` parameter on `SetResult`/`TrySetResult`.

<a id="sec-ownedtaskcompletionsourcet-—-for-class-t"></a>
#### `OwnedTaskCompletionSource<T>` — for class T

```bezel
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
```

The `where T: class` constraint excludes value types, string, and runtime-managed reference types. The caller transfers ownership of `result` to the source via the `owned` parameter; the source forwards ownership into the produced `OwnedTask<T>`. The awaiter consumes the task under Pattern B ([§30.5](#sec-result-lifetime-and-task-disposal)): one consumer takes ownership of the result via `using var x := await source.Task;`. A second await raises `InvalidOperationException`.

If `TrySetResult` returns `false` because the task is already in a terminal state, the implementation disposes `result` before returning. The caller has no further obligation — ownership was transferred at the call, and the implementation is responsible for releasing it on every path. The same applies if `TrySetException` or `TrySetCancelled` returns `false` while a previous `SetResult` already took ownership of a result; the existing result remains owned by the task.

<a id="sec-common-semantics"></a>
#### Common semantics

The `Task` exposed by either source observes the source's state. The `Set*` methods complete the task; `TrySet*` returns `false` instead of throwing when the task is already in a terminal state.

If the source is disposed before the task completes, the task is automatically transitioned to a faulted state with `InvalidOperationException("source disposed before completion")`.

If the source is disposed after `SetResult` was called, the task continues to expose the result; awaiters are unaffected. For `OwnedTaskCompletionSource<T>`, the produced result is now owned by the task, not the source — disposing the source has no effect on the result.

Pattern — wrapping a callback API:

```bezel
public async method WaitForKeyPressAsync(): Task<char>
  using var tcs := new TaskCompletionSource<char>();

  Keyboard.OnKeyPress((key) => do
    tcs.TrySetResult(key);
  end);

  return await tcs.Task;
end WaitForKeyPressAsync;
```

Pattern — wrapping a class-typed result with ownership transfer:

```bezel
public async method LoadResourceAsync(path: string): OwnedTask<Resource>
  using var tcs := new OwnedTaskCompletionSource<Resource>();

  StartLoadInBackground(path, (loaded: owned Resource) => do
    tcs.TrySetResult(loaded);                      // ownership moves into tcs's task
  end);

  return await tcs.Task;                            // moves ownership out of tcs's task
                                                    //   and into the outer Pattern B task's
                                                    //   result slot. tcs is then disposed,
                                                    //   but its task already has no result.
end LoadResourceAsync;
```

The outer async method's return type `OwnedTask<Resource>` and the inner `OwnedTaskCompletionSource<Resource>.Task` (which is `OwnedTask<Resource>`) align: the `await` performs the Pattern B move out of the inner task, and the outer `return` moves the same value into the outer task's result slot. A caller of `LoadResourceAsync` then writes:

```bezel
using var r := await LoadResourceAsync("/etc/foo");
Process(r);
// r disposed at scope end
```

**Closure capture and callback lifetime.** The lambda in the example captures `tcs` (an owned local) and is passed to `StartLoadInBackground`. Per the closure-capture rules ([§28.3](#sec-local-variables-ownership-and-borrows)), capturing an owned local treats the capture as a borrow whose source is the enclosing scope; the closure may not outlive that scope. For this pattern to type-check, the compiler must prove that the callback runs (and any references it holds become unreachable) before `tcs`'s scope ends.

The proof is structural: the `await tcs.Task` blocks the enclosing method's continuation until `tcs.TrySetResult` is called from the callback. The enclosing method's `tcs` local stays in scope until the method completes; the await ensures the method does not return until the callback has either completed (running `TrySetResult`) or failed in a way the task observed. By the time `tcs`'s scope ends, the callback has been invoked (or the task is in a terminal state without invocation, in which case the callback may still hold a borrow but `tcs`'s task is already settled and disposal of `tcs` will fault the task safely).

This reasoning is **specific to the TCS callback-registration pattern** and not a general closure-escape exception. The compiler recognizes the pattern by the shape "register callback that captures TCS; await TCS.Task" and admits it as a structural exception. Other patterns that capture a borrow into a callback that may outlive the registering scope remain rejected by the standard closure-escape rules. Library authors who write callback-registering APIs analogous to `StartLoadInBackground` should document the callback's lifetime guarantee; consumers wrapping such APIs via TCS rely on the same structural guarantee. A future revision is expected to introduce an explicit non-escaping-callback annotation that captures this contract in the type system; until then, the TCS pattern is the documented escape hatch.

<a id="sec-thread-synchronization"></a>
### 30.12 Thread synchronization

Bezel has no language-level synchronization primitives (no `synchronized` keyword, no `lock` statement). The standard library provides:

- `Mutex` — a mutual-exclusion lock. `Acquire()` returns a `Lock` object; releasing the lock is disposal of the `Lock`.
- `Lock` — a scoped guard implementing `IDisposable`. Integrates with `using var`.
- `Semaphore` — a counting semaphore.
- `Atomic<T>` — atomic operations on primitive types.

The `Lock` pattern with `using var`:

```bezel
class BankAccount
  private owned _mutex: Mutex := new Mutex();
  private _balance: decimal := 0m;

  public method Deposit(amount: decimal)
    using var guard := _mutex.Acquire();
    _balance := _balance + amount;
  end Deposit;
end BankAccount;
```

Within a single execution domain ([§30.13](#sec-runtime-model)), continuations never run concurrently, so a program needs no synchronization for state shared between one domain's tasks; interleaving occurs only at suspension points. Nor does spawning additional domains ([§30.14](#sec-multiple-domains)) create synchronization obligations: domains share no mutable state — everything that crosses a boundary is moved or immutable, and the channel types ([§30.15](#sec-channels)) are internally synchronized. The synchronization types above remain necessary only where foreign threads enter through the FFI ([§32](#sec-foreign-function-interface)). They are safe to use from both sync and async code.

<a id="sec-runtime-model"></a>
### 30.13 Runtime model

<a id="sec-execution-domains"></a>
#### Execution domains

All Bezel code executes within an **execution domain**. A domain is a unit of *serial* execution: at any instant, at most one continuation belonging to a domain is running, and a continuation, once started, runs without interruption from the domain's other work until it suspends at an `await`, completes, or propagates an exception (*run-to-completion*). Continuations of a domain's tasks therefore interleave only at suspension points and never overlap.

A program begins in a single domain (the *main domain*), which executes the entry point. A program that never spawns is a single-domain program. Additional domains are created explicitly with `Domain.Spawn` ([§30.14](#sec-multiple-domains)); data crosses a domain boundary only by ownership transfer or immutable sharing — through spawn arguments, join results, and channels ([§30.15](#sec-channels)) — under the *sendable* discipline of [§30.14](#sec-multiple-domains). Domains share no mutable state.

The guarantees that follow from serial execution within a domain are normative:

- **Memory visibility.** The effects of a continuation are visible to every continuation that subsequently runs in the same domain, without synchronization: each continuation *happens-before* the next continuation in its domain, regardless of which threads the runtime uses to run them.
- **No intra-domain data races.** Two continuations of the same domain never access memory concurrently, so unsynchronized access to state shared between a domain's tasks is well-defined. Interleaving at suspension points can still reorder logical operations — an invariant that must hold across an `await` must be re-established before suspending — but partially-written memory is never observed.
- **Soundness of ownership and borrow checking.** The rules of [§28](#sec-resource-management) assume that between two points of a continuation with no intervening suspension, no other code of the program runs. Within a domain this assumption holds by construction: a borrow's source cannot be disposed mid-use by concurrently-running code, and the disposal obligations of [§28.3](#sec-local-variables-ownership-and-borrows) are discharged exactly once under every interleaving.

#### Scheduling

- Calling an async method allocates the state machine and begins execution synchronously, as part of the running continuation, until the first `await` of an incomplete task.
- At an `await` of an incomplete task, the state machine registers a continuation and returns control to the caller.
- When the awaited task completes, the continuation is queued to its domain. The domain runs queued continuations one at a time, in unspecified order.
- A domain is not bound to an OS thread. The runtime may run a domain's successive continuations on different threads — but never on two threads at once — and it preserves the memory-visibility guarantee above across any thread migration. Async methods cannot assume thread affinity; code interacting with thread-affine foreign APIs (UI toolkits) must arrange marshalling explicitly.
- Foreign threads introduced through the FFI ([§32](#sec-foreign-function-interface)) are outside the domain model. Bezel-level state shared with such threads requires the synchronization types of [§30.12](#sec-thread-synchronization).

The runtime does not provide synchronization contexts in this version.

<a id="sec-multiple-domains"></a>
### 30.14 Multiple domains and sendable types

Domains run in parallel: while each domain's continuations are serial ([§30.13](#sec-runtime-model)), continuations of *different* domains may run simultaneously on different threads. The data-race freedom of the single-domain guarantees is preserved across domains by one discipline: **no mutable state is ever reachable from two domains.** Every value that crosses a domain boundary is either *moved* (ownership transfers; the origin domain provably loses access, by the same `move`/owned-transfer machinery as [§28.8](#sec-ownership-transfer)) or *immutable* (`string`, copies of values). The compiler enforces the discipline through the *sendable* property below; the channel types ([§30.15](#sec-channels)) are the only objects two domains may reach simultaneously, and they are compiler-known and internally synchronized.

<a id="sec-sendable-types"></a>
#### Sendable types

A type is **sendable** — a derived type property ([§3.6](#sec-derived-type-properties)) — when a value of the type may cross a domain boundary: as a spawn argument, a spawn result, or a channel element. Sendability is determined by category:

- **Value types** are sendable iff they are not borrow-bearing ([§9.7](#sec-records-and-disposal)) and not move-only ([§15.3](#sec-move-only-value-types)). All primitives and enums are sendable; a record is sendable per its fields, transitively (the borrow-bearing computation already supplies the transitivity). A sendable value crosses as a copy.
- **`string`** is sendable; it crosses as a shared reference, which is safe because strings are immutable (implementation note: the runtime's reclamation bookkeeping for strings must tolerate cross-domain sharing). A **boxed value** is sendable iff the boxed value's type is sendable. **Function-typed values are never sendable**: their captures are borrows of the origin domain's scopes ([§28.9](#sec-runtime-managed-types)).
- **Class types** are sendable iff the class is **transfer-closed**:
  1. The class is `sealed` ([§12.4](#sec-sealing)); or it is `sealed abstract` and every class in its module-closed hierarchy is itself transfer-closed (then the whole closed hierarchy is sendable). Open classes are not sendable — an open class's sendable-looking surface could be extended by a subclass holding origin-domain references, and a base-typed owned field could carry that subclass across.
  2. Every instance field, including inherited fields, is: a sendable value type; `string`-typed; an `owned` field whose declared type is a sendable class type; or an owning-inline-array field (`[N owned]C` / `[N owned]C?`) whose element class is a sendable class type.
  3. The class has no function-typed fields, no `Object`-typed or interface-typed fields (boxed contents and runtime types behind interfaces cannot be statically verified), and no non-`owned` class-typed fields (a back-reference aliases an object owned in the origin domain).
  Mutually-recursive `owned` references among sendable classes are admitted; the property is evaluated coinductively (assume the types under test sendable, then check the rules). A sendable class instance crosses as an **owned transfer** — the entire ownership tree under it moves with it, and rule 2 guarantees that tree contains everything the instance can reach.
- **Interface types** are never sendable (rule 3's reasoning, applied at the root).
- **Compiler-known standard-library types** state their sendability in their stdlib sections, justified by their internal structure: owning collections and `OwnedBuffer<T>` are sendable iff their element type is a sendable class type (their state is wholly owned); borrowing collections are sendable iff their element category ([§28.11](#sec-element-slot-policy)) is sendable-value or `string` (copy and shared-immutable slots; a borrowing collection of class elements carries borrows and is not sendable); `Task`, `Task<T>`, `OwnedTask<T>`, enumerators, `EventSource<T>`, `CancellationTokenSource`, `Shared<T>` and `Weak<T>` ([§28.13](#sec-shared-ownership) — the shared count is not synchronized), and the [§30.12](#sec-thread-synchronization) types are domain-local and not sendable; `Channel<T>` and `ChannelSender<T>` are sendable ([§30.15](#sec-channels)). `CancellationToken` is a borrow-bearing record and therefore not sendable by the value-type rule — cancellation does not cross domains in this revision; the cross-domain cancellation pattern is a message on a channel.

Sendability of a generic instantiation is checked at the instantiation or call site under the rules of [§3.6](#sec-derived-type-properties); diagnostics name the property ("not sendable"), the offending type, and the failing rule.

<a id="sec-spawning-domains"></a>
#### Spawning

`Domain.Spawn` (stdlib [§6.8](Bezel_Standard_Library.md#sec-domain)) creates a new domain, runs an entry function as its first continuation, and returns a join handle in the spawning domain:

```bezel
// Representative forms; the stdlib section declares the full set.
Domain.Spawn(entry: () -> ()): owned Task
Domain.Spawn<R>(entry: () -> owned R): owned OwnedTask<R>            // R: sendable class
Domain.Spawn<A, R>(entry: (owned A) -> owned R, owned arg: A): owned OwnedTask<R>
Domain.Spawn<R>(entry: () -> R): owned Task<R>                       // R: sendable value type
```

The compiler enforces at the call site:

- **The entry must be capture-free**: a non-capturing lambda or a static method reference. A capturing lambda or an instance method reference is rejected — captures and receivers are borrows of the origin domain ([§28.9](#sec-runtime-managed-types)). Data enters the new domain only through the explicit `arg` parameters, which must be of sendable types; class-typed arguments are `owned` and are moved through the spawn into the entry's parameter.
- **Argument and result types must be sendable**, per the rules above.

The join handle is an ordinary task of the spawning domain. Awaiting an `OwnedTask<R>` join follows Pattern B ([§30.5](#sec-result-lifetime-and-task-disposal)): single consumer, ownership of the (sendable) result transfers to the awaiter — the existing single-consumer transfer semantics are exactly the cross-domain join semantics, unchanged. `Task`/`Task<R>` joins follow Pattern A.

**Faults.** An exception that propagates out of the entry does not cross the boundary as an object (exception classes are not in general sendable). The join instead raises `DomainFaultedException` (stdlib [§6.8](Bezel_Standard_Library.md#sec-domain)), whose `Message` and `FaultStackTrace` carry copies of the original exception's message and stack-trace text (`string`s — sendable). Programs that need richer fault data return it as a sendable result or send it over a channel.

**Domain completion.** When the entry's task completes, the join handle completes (with the result or fault). Tasks started within the spawned domain that are still pending continue to run in that domain; their eventual exceptions are handled as `Task.FireAndForget` handles them (logged, not propagated — stdlib [§6.7](Bezel_Standard_Library.md#sec-task-static-helpers)). The domain's resources are reclaimed when its last continuation completes.

<a id="sec-channels"></a>
### 30.15 Channels

A **channel** carries sendable values from one domain to another with ownership transfer at both ends: `Send` consumes the value in the sending domain; `Receive` produces it owned in the receiving domain. Channels are the only mutable objects reachable from more than one domain; they are compiler-known, internally synchronized, and their element type must be sendable (checked at instantiation per [§3.6](#sec-derived-type-properties)).

The shape is multiple-producer, single-consumer. The `Channel<T>` object is the *receiving* end, owned like any class instance (typically by a `using var` or an `owned` field in the receiving domain). Sender endpoints are minted from it and moved to wherever sending happens:

```bezel
using var inbox := new Channel<Report>();          // unbounded; this domain receives
using var worker := Domain.Spawn(ProcessAll, inbox.NewSender());   // sender moves to the new domain

// in this domain:
using var first := await inbox.Receive();          // ownership of one Report transfers in

// in the spawned domain (static method):
public static method ProcessAll(owned tx: ChannelSender<Report>)
  using var sender := tx;
  sender.Send(BuildReport(1));                     // ownership of each Report transfers out
  sender.Send(BuildReport(2));
end ProcessAll;                                    // sender disposed: last sender closes the channel
```

Normative semantics (full API: stdlib [§6.9](Bezel_Standard_Library.md#sec-channel)):

- **`new Channel<T>()`** creates an unbounded channel. **`NewSender(): owned ChannelSender<T>`** mints a sender endpoint; any number may be minted, and each is an independently owned, sendable object. The channel and its senders share an internal core whose synchronization is the implementation's responsibility and invisible to programs.
- **`Send(owned item: T)`** (class-typed T; for sendable value types and `string` the parameter is unmodified) appends the item, transferring ownership into the channel. Unbounded channels never block on send. Sending on a closed channel raises `ChannelClosedException`.
- **`Receive()`** is asynchronous — `OwnedTask<T>` for class-typed T (Pattern B: awaiting transfers ownership of the item to the receiver), `Task<T>` otherwise. If the channel is empty and open, the receive completes when an item arrives; if empty and closed, it raises `ChannelClosedException`. **`TryReceive(out owned item: T?): bool`** is the non-blocking form (Try pattern; `out using var` at the call site); it returns `false` when no item is available — consult **`IsClosed`** to distinguish empty-but-open from closed.
- **Closing.** Disposing the last `ChannelSender<T>` *closes* the channel: items already sent remain receivable (the channel drains), and after the drain, receives raise `ChannelClosedException`. Disposing the `Channel<T>` itself closes it immediately and disposes every undelivered class-typed item in reverse insertion order (the owned column of [§28.11](#sec-element-slot-policy)); subsequent sends raise `ChannelClosedException`.
- **Ordering.** Items from one sender are received in the order sent; items from different senders interleave in an unspecified but consistent total order (each item is received exactly once).

Bounded channels (back-pressure: an async `Send` that awaits capacity) and multi-consumer channels (receiver cloning, which requires shared ownership) are deferred; see the design rationale.

---

<a id="sec-exception-handling"></a>
## 31. Exception Handling

<a id="sec-throwing"></a>
### 31.1 Throwing

```bezel
throw new ArgumentException("Amount must be positive.", nameof(amount));
throw;          // re-throw the current exception (in a catch arm body only)
```

The first form constructs and raises an exception. The second form preserves the original stack trace. The bare re-throw is also available as an expression — `throw` without the trailing semicolon — which composes naturally with arm bodies; see [§31.2.4](#sec-bare-re-raise) and [§17.12](#sec-the-throw-expression).

<a id="sec-exception-ownership"></a>
#### Exception ownership

`new SomeException(...)` produces an owned class instance that the `throw` statement immediately consumes — ownership transfers to the runtime's exception machinery, which disposes the exception after the matching catch arm completes (or after the exception terminates the program if uncaught).

Inside a catch arm, the bound exception variable (`var ex: Exception` in the arm pattern) is a **borrow** of the in-flight exception; its source is the runtime, and its lifetime is the arm body's. The borrow may be passed to other methods (which further borrow for the duration of the call) but may not be assigned to an `owned` field, returned as `owned`, or captured by a closure that may outlive the arm.

To preserve an exception past the arm body — for logging, deferred reporting, or aggregation — copy the relevant data out of it (typically `ex.Message`, `ex.StackTrace.ToString()`, and any domain-specific properties) into separate owned values. Exception instances themselves cannot escape the arm that caught them. The bare re-throw `throw;` (which preserves the original instance) is the one exception to this rule, since it transfers the in-flight exception back to the runtime rather than capturing it.

<a id="sec-try-catch-finally"></a>
### 31.2 `try`/`catch`/`finally`

A `try` statement consists of a protected block, optionally followed by a `catch` block, optionally followed by a `finally` block. At least one of `catch` or `finally` must be present:

```bezel
try
  // protected statements
catch
  // arms (see §31.2.1)
finally
  // cleanup statements
end;
```

The `catch` block contains comma-separated arms whose syntax mirrors `case` arms ([§19](#sec-the-case-construct)). Each arm is a pattern with an optional guard and a body. The arms are tried in order; the first arm whose pattern matches the thrown exception (and whose guard, if any, evaluates to `true`) is selected, its body runs, and exception handling completes. Arms after the matched one are not considered.

If no arm matches, the exception continues propagating to the enclosing handler — exactly as if the `try` statement had not been present for that exception. This is the behavior of an unmatched arm in statement-position pattern dispatch ([§19.6](#sec-exhaustiveness)); `catch` does not warn about non-exhaustiveness because letting non-matching exceptions propagate is the normal case.

`finally` runs whether the protected block completed normally, was exited by an exception (caught or uncaught), or was exited by `return`. `return` inside a `finally` block is a compile error.

If the `try` has only a `finally` (no `catch`), the catch block is omitted entirely:

```bezel
try
  using var fs := new FileStream(path);
  ProcessFile(fs);
finally
  Log("file processing finished");
end;
```

<a id="sec-arm-patterns"></a>
#### 31.2.1 Arm patterns

A catch arm's pattern is one of:

- A bare type: `IOException`. Matches any thrown exception that is an instance of the type, without binding.
- A binding pattern: `var name: Type`. Matches and binds the exception to `name` for use in the arm's guard and body. The binding is in scope only within the arm.
- A multi-type alternative, with or without binding: `Type1, Type2, ...` or `var name: Type1, Type2, ...`. Matches if the thrown exception is an instance of any of the listed types. When binding, `name` has the static type of the **nearest common base** of the listed types (the most specific type that is a base of every listed type; `Exception` if no more specific common base exists).

Each pattern may be followed by an optional `when Expression` guard. The guard is evaluated only if the pattern matches; if the guard returns `false`, the arm is skipped and the next arm is considered. Inside the guard, the bound variable (if any) is in scope.

Arms are evaluated in textual order. Within a pattern, type alternatives are tested in declaration order, but this is not observable when types are disjoint; for non-disjoint alternatives (a base type listed alongside a derived type), the first matching type-arm wins, and the binding's static type is determined statically by the rule above, not by which alternative matched at runtime.

<a id="sec-arm-bodies"></a>
#### 31.2.2 Arm bodies

A catch arm's body uses the same shape as a `case` arm body ([§19.2](#sec-arms)). Three forms are valid (catch arms are always statement-position, so all three apply):

- **An expression**: `=> SomeMethod(args)`. The expression's result is discarded.
- **A single statement**: `=> Log(ex); ` ends with the statement's `;`. The convenient form for the common one-line arm body that is a statement (assignment, `return`, `throw`, etc.).
- **A `do ... end` block**: `=> do statements end`. Used when the body contains multiple statements.

The arm separator is `,`. A trailing comma after the last arm is permitted.

```bezel
try
  DoWork();
catch
  var ex: ArgumentException when ex.ParamName = "amount" => Console.WriteLine("Invalid amount"),
  InvalidOperationException => Console.WriteLine("Invalid state"),
  var ex: IOException, TimeoutException => Retry(ex),
  var ex: Exception => do
    Log(ex);
    throw;
  end
end;
```

The example demonstrates: a binding arm with a guard, a no-binding arm, a multi-type arm with binding (the `ex` parameter has type `Exception`, the nearest common base of `IOException` and `TimeoutException`, and is passed to a method accepting `Exception`), and a final binding arm whose body re-raises after logging.

<a id="sec-catch-ordering-and-exhaustiveness"></a>
#### 31.2.3 Catch ordering and exhaustiveness

Arms are tried in order; the first match wins. The most specific exception types should appear first. A bare `Exception` arm at the end captures any exception not caught by earlier arms; this is the catch-all idiom.

```bezel
try
  DoWork();
catch
  var ex: TimeoutException => RetryWithBackoff(ex),
  var ex: Exception => do          // catch-all
    Log(ex);
    throw;
  end
end;
```

A `catch` block need not be exhaustive. Exceptions not matched by any arm propagate to the enclosing handler. The compiler does not warn on non-exhaustive `catch` blocks — non-exhaustiveness is the normal case for handlers that recover from specific failures while letting others propagate.

<a id="sec-bare-re-raise"></a>
#### 31.2.4 Bare re-raise

The bare re-raise re-throws the currently-handled exception with its stack trace preserved. It is valid only inside a catch arm body, and exists in two equivalent forms:

- As an **expression**: `throw` (without a semicolon). Has the bottom type and may appear anywhere an expression is expected. The natural form for an arm body that does nothing but re-raise:

  ```bezel
  catch
    var ex: TransientException => Retry(ex),
    var ex: Exception => throw            // re-raise unrecognized exceptions
  end;
  ```

- As a **statement**: `throw;` (with the trailing semicolon). Equivalent to the expression form followed by `;`, used inside `do ... end` blocks where the re-raise is one statement among several:

  ```bezel
  catch
    var ex: SqlException => do
      metric.RecordSqlError(ex);
      throw;                              // re-raise after recording
    end
  end;
  ```

Bare `throw` (either form) outside a catch arm body is a compile error.

To re-throw with a different exception or to wrap the current exception, use `throw expr` ([§17.12](#sec-the-throw-expression)), which is an expression in any position:

```bezel
catch
  var ex: SqlException => throw new DataAccessException("query failed", ex),
  var ex: Exception => Log(ex)
end;
```

<a id="sec-deliberate-suppression"></a>
#### 31.2.5 Deliberate suppression

To deliberately suppress all exceptions in a block — a pattern that should be rare and visible — use a catch-all arm whose body is the unit value `()`:

```bezel
try
  BestEffortCleanup();
catch
  Exception => ()
end;
```

The `()` body makes the suppression syntactically explicit. Code review will surface the pattern; this is intentional. Silent suppression of exceptions is a code smell, and the syntax does not hide it.

<a id="sec-no-throws-clause"></a>
### 31.3 No `throws` clause

Methods do not declare which exceptions they may raise. The compiler does not enforce that callers handle particular exception types. Any exception may propagate from any method call.

The conventional way to document expected exceptions is the `<exception>` documentation tag:

```bezel
/// <exception cref="ArgumentException">Thrown when amount is non-positive.</exception>
public method Deposit(amount: decimal) ...
```

<a id="sec-standard-exception-types"></a>
### 31.4 Standard exception types

The following exception types are language-built-in and available without import. The hierarchy is shown as a tree; types not indented under another type derive directly from `Exception`.

```
Exception
├─ ArgumentException                — invalid arguments.
│   └─ ArgumentNullException        — a null argument where non-null was required.
├─ ArithmeticException              — base for arithmetic errors.
│   ├─ OverflowException            — integer or decimal overflow.
│   └─ DivideByZeroException        — division by zero.
├─ InvalidOperationException        — operation not valid in current state.
│   └─ DisposedAccessException      — member access on a disposed instance (checked disposal mode only).
├─ NotImplementedException          — placeholder for unimplemented code.
├─ NullReferenceException           — null encountered where non-null required.
├─ IndexOutOfRangeException         — array or string index out of bounds.
├─ MatchFailureException            — case expression did not match.
├─ OperationCanceledException       — operation cancelled via CancellationToken.
├─ TypeInitializationException      — static initialization failed; wraps the original.
├─ FormatException                  — string does not conform to an expected format.
└─ StackOverflowException           — call stack exhausted; cannot be caught; terminates the program.
```

The inheritance hierarchy is relevant for arm ordering: an arm pattern of `ArithmeticException` matches both `OverflowException` and `DivideByZeroException`, and an arm pattern of `ArgumentException` matches `ArgumentNullException`.

All exceptions in the list above are catchable except `StackOverflowException`. In particular, `NullReferenceException` (raised by `!` on a null value), `MatchFailureException` (raised by a non-exhaustive `case` expression), `OverflowException`, and `IndexOutOfRangeException` are ordinary exceptions that propagate through `try`/`catch` normally.

`DisposedAccessException` is raised only by implementations operating in checked disposal mode ([§28.2](#sec-checked-disposal)); in unchecked mode the same access is undefined behavior. Programs must not rely on it for control flow — it exists to convert a heisenbug into a deterministic fault during development, and a build is free to omit it.

`TypeInitializationException` is the one built-in whose surface extends `Exception`'s: it exposes `public readonly property InnerException: Exception?`, carrying the original exception that escaped the static initializer ([§22.13](#sec-static-initialization-order); stdlib [§2.17](Bezel_Standard_Library.md#sec-built-in-exceptions-cross-reference)).

The principle behind the built-in list: an exception is built-in iff its only producer is a built-in operation (an array or string indexer, an integer arithmetic operation, the `case` construct, an async cancellation). Exceptions whose producer requires explicit `import` live alongside their producer in the standard library and require the same import.

Domain-specific exception types defined in the standard library include `KeyNotFoundException` (raised by `Dictionary<K, V>`'s indexer; in `System.Collections.Generic`), `FileNotFoundException`, `IOException`, and others. Each requires explicit `import` of its containing module.

<a id="sec-throw-as-an-expression"></a>
### 31.5 `throw` as an expression

`throw` may appear in any expression position. Its operand is optional:

- `throw expr` — raises the supplied exception. Valid in any expression position.
- `throw` — bare re-raise. Valid only inside a catch arm body.

Both forms have the bottom type. See [§17.12](#sec-the-throw-expression) for full semantics and [§31.2.4](#sec-bare-re-raise) for the bare re-raise's interaction with arm bodies.


---

<a id="sec-foreign-function-interface"></a>
## 32. Foreign Function Interface

A Bezel program may call functions defined in native libraries (`.so`, `.dylib`, `.dll`). The mechanism is declaration-based: the Bezel side describes the native function's signature, and the compiler emits the appropriate call.

The boundary between Bezel and native code is **trusted but unverified**. The compiler accepts the programmer's declaration of a native function's signature; if the declaration is wrong, the call is undefined behavior. This is unlike calls within Bezel, which are end-to-end checked.

<a id="sec-the-extern-block"></a>
### 32.1 The `extern` block

`extern` declarations always appear inside a block:

```bezel
extern "libc"
  public method puts(s: CString): int;
  public method strlen(s: CString): IntPtr;
  public method strdup(s: CString): CString?;
end;
```

The block opens with `extern` followed by a string-literal library name, optionally followed by a `convention` clause ([§32.4](#sec-calling-conventions)). The block ends with `end;`. Inside, each method declaration uses ordinary Bezel syntax — visibility, name, parameters, return type — followed by `;`. There is no body and no per-method `end`.

`extern` declarations may be `public` or `internal`, following the rules for module-level declarations. They may not carry `static`, `virtual`, `override`, `abstract`, or `sealed`. They are always module-level — never members of classes.

Empty `extern` blocks are a compile error.

<a id="sec-scoping"></a>
### 32.2 Scoping

`extern` methods belong to their declaring module. From within the same module, they are called by name without qualification. From outside, a `public extern` method is qualified with the module name:

```bezel
import FFIExample;

FFIExample.strdup(cname);
```

`internal extern` methods are invisible outside their declaring module.

<a id="sec-library-specification"></a>
### 32.3 Library specification

The library name is a bare identifier without extension. The runtime resolves it according to platform conventions:

- Linux: `libc` resolves to `libc.so.6`.
- macOS: `libc` resolves to `libc.dylib`.
- Windows: `libc` would resolve to `libc.dll` (typically `msvcrt`).

For libraries with non-conventional names, a full path may be used as the library name; this bypasses runtime resolution and loads the file directly:

```bezel
extern "/opt/myapp/lib/libcustom.so.2.1"
  public method DoWork(): int;
end;
```

Bare names are preferred for portability.

<a id="sec-calling-conventions"></a>
### 32.4 Calling conventions

The default calling convention is the platform-standard C convention (`cdecl` on x86-32 and x86-64, the standard ABI on other architectures). For non-default conventions, the `convention` clause specifies which:

```bezel
extern "user32" convention "stdcall"
  public method MessageBoxW(hwnd: IntPtr, text: WString, caption: WString, style: uint): int;
end;
```

`cdecl` and `stdcall` are universally accepted. Other conventions may be platform-specific. A mismatch between declared and actual conventions is undefined behavior.

<a id="sec-ffi-compatible-types"></a>
### 32.5 FFI-compatible types

Only the following types may appear in `extern` method signatures:

- All integer types and `float`, `double`.
- `bool` — passed as a 1-byte value. `false` is represented as the byte value 0; `true` is represented as the byte value 1. When receiving a `bool` from native code, any non-zero byte value is treated as `true`. C `_Bool` is implementation-defined width on most platforms but is conventionally 1 byte; the Bezel FFI fixes this at 1 byte for portability. For C ABIs that disagree, declare the parameter as `byte` or `int` and convert on the Bezel side.
- `IntPtr`, `UIntPtr` — pointer-sized integers (32 or 64 bits, platform-dependent). Opaque; no operations dereference them.
- `CString` — null-terminated UTF-8 byte sequence.
- `WString` — null-terminated UTF-16 sequence.
- `RawBytes` — pointer-and-length pair representing arbitrary binary data.
- **Inline arrays `[N]T`** where the element type T is an FFI-compatible primitive (integer types, `float`, `double`, `bool`). Inline arrays have C-equivalent layout: N elements packed contiguously, no header. The Bezel `[N]T` parameter or return slot maps to C `T arr[N]` (or equivalently `T*` with known length N). Inline arrays of class-typed, interface-typed, record-typed, runtime-managed, or pointer-typed elements are not admitted. Owning inline arrays (`[N owned]T?`, `[N owned]T`) are not admitted — their element type is by definition class-or-interface ([§15.1.1](#sec-owning-inline-arrays)). Inline arrays of inline arrays are not admitted in v0.98 because multi-dimensional inline arrays are deferred ([§15.1](#sec-inline-arrays)).
- The unit type `()` — valid as a return type only, mapped to C `void`. An FFI declaration with no `:` clause produces a unit-returning method; an explicit `: ()` is also accepted and means the same thing.
- Function types `(...) -> T` whose parameters and return type are themselves FFI-compatible.

Not allowed: Bezel `string` (use `CString` or `WString`), `char` (use `int` or `byte` and convert), classes, records, interfaces, generic type parameters, `Array<T>`, `OwnedArray<T>`, inline arrays whose element type is not FFI-compatible.

`char` is excluded because Bezel's `char` is a 32-bit Unicode scalar value, while C `char` is conventionally an 8-bit byte and platform character types vary further (Windows `wchar_t` is 16 bits, Unix `wchar_t` is 32 bits but with different encoding assumptions). No single mapping is correct across the relevant ABIs; passing `int` (for full code points) or `byte` (for ASCII / opaque bytes) and converting on the Bezel side is unambiguous.

<a id="sec-string-marshalling"></a>
### 32.6 String marshalling

`CString`, `WString`, and `RawBytes` are FFI-boundary types. They cannot be constructed or manipulated directly in ordinary Bezel code; the standard library provides explicit conversion functions.

The `CString` class is for **caller-owned** strings: those that the Bezel side allocated and is responsible for disposing. The pattern is `CString.From(s)` to allocate, with `using var` to bind for disposal:

```bezel
public static class FileOps
  public static method OpenForReading(path: string): int
    using var cpath := CString.From(path);   // owned CString; disposed by using var
    return open(cpath, O_RDONLY);
  end OpenForReading;
end FileOps;
```

For native functions returning a string pointer that the caller does **not** own — pointers into static memory, into a heap buffer owned by the library, or into another structure — the FFI declaration's return type is `IntPtr`, not `CString`. The Bezel side reads through the pointer with `string.FromCStringPointer(p: IntPtr): string`, which copies into a fresh Bezel `string` without taking ownership of the original buffer:

```bezel
extern "libc"
  public method getenv(name: CString): IntPtr;     // returns a borrowed pointer
end;

public static class Env
  public static method GetVariable(name: string): string?
    using var key := CString.From(name);
    var resultPtr := getenv(key);
    if resultPtr = IntPtr.Zero then
      return null;
    end;
    return string.FromCStringPointer(resultPtr);   // copy; getenv buffer untouched
  end GetVariable;
end Env;
```

Native functions returning a buffer the caller **must free** (a heap allocation owned by the caller after return) declare their return type as `CString`; ownership transfers to the Bezel side and `using var` discharges it.

The choice between `CString` and `IntPtr` at the FFI declaration site is the contract: a `CString` return promises caller ownership; an `IntPtr` return makes no ownership claim. Native libraries with mixed conventions across functions need separate declarations matching each function's documented contract.

`string.From(c)` reads from a caller-owned `CString` and produces a Bezel `string`; the original is unaffected and is still subject to the caller's disposal obligation. `string.FromCStringPointer(p)` is the same operation on a raw `IntPtr`.

<a id="sec-pointers-and-intptr"></a>
### 32.7 Pointers and `IntPtr`

`IntPtr` is a compiler-intrinsic value type representing a pointer-sized integer. `UIntPtr` is the unsigned counterpart, useful for `size_t`.

Members:

- `IntPtr.Zero` — the null pointer (integer 0).
- `=` and `<>` for null tests.
- Explicit conversions to and from `long` and `ulong`.

```bezel
extern "libdl"
  public method dlopen(filename: CString, flags: int): IntPtr;
  public method dlclose(handle: IntPtr): int;
end;

public static class DynamicLoader
  public static method LoadAndClose(name: string)
    using var libname := CString.From(name);
    var handle := dlopen(libname, RTLD_NOW);
    if handle = IntPtr.Zero then
      throw new IOException("Library failed to load");
    end;
    var result := dlclose(handle);
  end LoadAndClose;
end DynamicLoader;
```

`IntPtr` is opaque. Bezel provides no language-level operation to dereference it; that is the native side's job.

<a id="sec-callbacks"></a>
### 32.8 Callbacks

Native APIs that accept function pointers are supported via Bezel's function-type syntax:

```bezel
extern "libc"
  public method qsort(
    base: IntPtr,
    nmemb: UIntPtr,
    size: UIntPtr,
    compar: (IntPtr, IntPtr) -> int): ();
end;

public static class IntSorter
  public static method CompareInts(a: IntPtr, b: IntPtr): int
    // ...
  end CompareInts;

  public static method SortInPlace(arrayPtr: IntPtr, count: UIntPtr, sizeOfInt: UIntPtr)
    qsort(arrayPtr, count, sizeOfInt, IntSorter.CompareInts);
  end SortInPlace;
end IntSorter;
```

Constraints:

- The callback must be a **method reference** — a named instance method, a static method, or a property reference. Lambdas, including non-capturing lambdas, are not accepted: the compiler does not promise to recognize a non-capturing lambda as equivalent to a function pointer, so a single rule (no lambdas) avoids relying on a heuristic. To pass a lambda-shaped operation, extract it into a named static method.
- Callbacks are themselves trusted but unverified.
- Exceptions thrown by a Bezel callback do not propagate to native callers. The FFI boundary catches and silently swallows them, or aborts the program, depending on platform. Callbacks should report errors via return codes.

<a id="sec-error-handling"></a>
### 32.9 Error handling

Native functions do not throw Bezel exceptions. They report errors via return codes, `errno`, output parameters, or other native conventions. The FFI does not automatically translate these into Bezel exceptions:

```bezel
public static class FileOps
  public static method OpenChecked(path: CString): int
    var fd := open(path, O_RDONLY);
    if fd < 0 then
      throw new IOException($"open failed: errno={errno()}");
    end;
    return fd;
  end OpenChecked;
end FileOps;
```

Higher-level wrappers around native libraries typically hide the raw error-checking pattern.

<a id="sec-boundary-contract"></a>
### 32.10 Boundary contract

The Bezel compiler guarantees:

- Arguments passed to an `extern` method are of the declared types.
- The return value, if any, is treated as the declared type.
- The calling convention used matches the `convention` clause (or default).
- Function pointers passed across the boundary are valid for at least the duration of the call.

The programmer is responsible for:

- The native function having the declared signature.
- The calling convention matching.
- Pointer values being valid for their intended use.
- Memory allocated by native code being freed by native code (or by Bezel calling the appropriate native deallocation function).
- Native code not corrupting Bezel objects or scribbling over the heap.

When the contract is violated, the consequences are unspecified.

<a id="sec-reserved-for-future-revisions"></a>
### 32.11 Reserved for future revisions

Exporting Bezel methods to native callers, dynamic library loading at runtime, typed pointer wrappers, and structured-data marshalling are not part of this version of the FFI.

---

<a id="sec-program-entry-point"></a>
## 33. Program Entry Point

<a id="sec-the-main-method"></a>
### 33.1 The `Main` method

Every Bezel executable has exactly one entry point: a `public static method Main` declared on any class in the program's main module. The runtime locates this method and invokes it to start the program.

<a id="sec-accepted-signatures"></a>
### 33.2 Accepted signatures

```bezel
public static method Main()
public static method Main(args: Array<string>)
public static method Main(): int
public static method Main(args: Array<string>): int
public static async method Main(): Task
public static async method Main(args: Array<string>): Task
public static async method Main(): Task<int>
public static async method Main(args: Array<string>): Task<int>
```

A signature outside this set does not qualify as an entry point; a method named `Main` with a non-qualifying signature is an ordinary method.

`Main` must be `public` and `static`. An instance method named `Main` does not qualify.

<a id="sec-exit-code"></a>
### 33.3 Exit code

When `Main` returns `int` or `Task<int>`, the returned value becomes the process exit code. Otherwise, normal completion produces an exit code of 0.

<a id="sec-command-line-arguments"></a>
### 33.4 Command-line arguments

The `args` parameter, when present, contains the command-line arguments. The program's own executable path is not included; `args[0]` is the first user-supplied argument.

`args` is typed `Array<string>` (the heap-allocated form, [§15.2](#sec-heap-arrays)). This matches the rest of the standard library, where collection-typed parameters are uniformly `Array<T>` or an `IList<T>`/`IEnumerable<T>` interface. The runtime constructs `args` once at program entry and does not mutate it; users who want fixed-size discipline simply do not call resize methods on it.

Programs that want to forward args to a method expecting `IEnumerable<string>` or `IList<string>` can pass `args` directly. Programs that need a specifically-sized inline buffer (for FFI marshalling, for example) construct a fresh inline array from the args' contents.

<a id="sec-program-entry-point-resolution"></a>
### 33.5 Resolution

A program is ill-formed if more than one method qualifies as the entry point. This applies both across classes (multiple classes each declaring a qualifying `Main`) and within a single class (one class declaring more than one of the eight signature shapes from [§33.2](#sec-accepted-signatures)).

For the cross-class case, the build tool must accept a `--main-class FullyQualifiedName` flag (or its equivalent in tool-specific configuration); the named class is selected as the entry point and a successful build ensues. Without disambiguation, the compiler produces an error listing every qualifying `Main` it found.

For the within-class case, no flag-based disambiguation is provided: a class may declare at most one qualifying `Main`. A class that overloads `Main` with multiple qualifying signatures is rejected at compile time with a clear error pointing to the ambiguous overloads. (A class may still declare a non-qualifying `Main` alongside a qualifying one — for example, an instance method named `Main` does not qualify, so it does not conflict.)

The exact CLI surface is implementation-defined — different tools may use `--main`, `--entry-point`, project-file properties, or other surfaces — but every conforming implementation must expose a mechanism semantically equivalent to "name the fully-qualified class whose `Main` to use." The flag name `--main-class` and its accepted argument shape (a fully-qualified type name) are normative for tools that use a command-line interface.

<a id="sec-example"></a>
### 33.6 Example

```bezel
module HelloWorld;

class Program
  public static method Main(args: Array<string>)
    if args.Count > 0 then
      Console.WriteLine($"Hello, {args[0]}!");
    else
      Console.WriteLine("Hello, Bezel!");
    end;
  end Main;
end Program;
```

A library module has no entry point; the compiler produces a shared library artifact.

---

<a id="sec-comments-and-documentation"></a>
## 34. Comments and Documentation

Three comment forms ([§1.5](#sec-comments)):

- `// ...` line comment.
- `/* ... */` block comment, does not nest.
- `/// ...` documentation comment.

Documentation comments use XML element syntax. Recognized tags include `<summary>`, `<param>`, `<returns>`, `<exception>`, `<remarks>`, `<example>`, `<see>`, `<seealso>`. The compiler preserves them in output metadata for documentation generators; it does not interpret tag content beyond preservation.

Documentation comments are meaningful only immediately before a declaration.

```bezel
/// <summary>
/// Represents a bank account with deposit and withdrawal functionality.
/// </summary>
public class BankAccount: IBankAccount
  /// <summary>
  /// Deposits the specified amount into the account.
  /// </summary>
  /// <param name="amount">The amount to deposit. Must be positive.</param>
  /// <exception cref="ArgumentException">Thrown when amount is non-positive.</exception>
  public method Deposit(amount: decimal)
    if amount <= 0m then
      throw new ArgumentException("Amount must be positive.", nameof(amount));
    end;
    Balance := Balance + amount;
  end Deposit;
end BankAccount;
```


---

<a id="sec-appendix-a"></a>
## Appendix A: Grammar

This appendix gives an EBNF grammar for Bezel. The grammar is a guide to the language's syntax; it is not strictly LL or LR. Terminal symbols are quoted; nonterminals are written `LikeThis`. `?` denotes optional, `*` denotes zero-or-more, `+` denotes one-or-more, `|` denotes alternation, parentheses group.

Whitespace and comments are recognized by the lexer and may appear between any two tokens; they are not shown in the grammar.

<a id="sec-lexical"></a>
### A.1 Lexical

```
Identifier      ::= IdentStart IdentCont*
IdentStart      ::= Letter | "_"
IdentCont       ::= IdentStart | Digit
Letter          ::= /any Unicode letter/
Digit           ::= "0".."9"
HexDigit        ::= Digit | "a".."f" | "A".."F"

IntLiteral      ::= DecLiteral | HexLiteral | BinLiteral
DecLiteral      ::= Digit ("_"* Digit)* IntSuffix?
HexLiteral      ::= "0x" HexDigit ("_"* HexDigit)* IntSuffix?
BinLiteral      ::= "0b" ("0" | "1") ("_"* ("0" | "1"))* IntSuffix?
IntSuffix       ::= "L" | "U" | "UL" | "LU"   /* case-insensitive */

FloatLiteral    ::= DigitGroup "." DigitGroup Exponent? FloatSuffix?
                  | DigitGroup Exponent FloatSuffix?
                  | DigitGroup FloatSuffix
DigitGroup      ::= Digit ("_"* Digit)*
Exponent        ::= ("e" | "E") ("+" | "-")? DigitGroup
FloatSuffix     ::= "f" | "F" | "m" | "M"     /* m -> decimal */

CharLiteral     ::= "'" CharContent "'"
StringLiteral   ::= '"' StringContent* '"'
                  | '@"' VerbatimContent* '"'
InterpString    ::= '$"' (InterpPart | StringContent)* '"'
                  | '$@"' (InterpPart | VerbatimContent)* '"'
InterpPart      ::= "{" Expression ("," "-"? IntLiteral)? (":" FormatSpec)? "}"
```

`CharContent`, `StringContent`, `VerbatimContent`, and `FormatSpec` are not given productions; their content is described in [§18.3](#sec-character-literals), [§18.4](#sec-string-literals), [§18.5](#sec-interpolated-strings), [§18.6](#sec-verbatim-strings), and [§18.8](#sec-escape-sequences-non-verbatim) of the main text. Briefly: `CharContent` is one Unicode scalar value or one escape sequence; `StringContent` is any sequence of code points or escapes other than an unescaped `"`; `VerbatimContent` is any sequence of code points other than `"`, with `""` denoting a single `"`; `FormatSpec` is a sequence of format characters meaningful to the value's formatter.

<a id="sec-source-file"></a>
### A.2 Source file

```
SourceFile      ::= ModuleDecl Import* TopDecl*
ModuleDecl      ::= "module" QualifiedName ";"
Import          ::= "import" QualifiedName ";"
QualifiedName   ::= Identifier ("." Identifier)*

TopDecl         ::= ClassDecl | InterfaceDecl | RecordDecl | EnumDecl
                  | ExtensionDecl | TypeAliasDecl | ExternBlock
TypeAliasDecl   ::= "type" Identifier GenericParams? "=" Type
                    GenericConstraints* ";"
```

<a id="sec-type-declarations"></a>
### A.3 Type declarations

```
ClassDecl       ::= Visibility? Modifier* "class" Identifier GenericParams?
                    PrimaryCtorParams?
                    SerializedClassClause?
                    Inheritance?
                    GenericConstraints*
                    ClassBody
SerializedClassClause ::= "serialized" ( "tree" | "graph" | "cyclic" ) PolymorphicClause?
PolymorphicClause ::= "polymorphic" "(" PolyCase ("," PolyCase)* ")"
PolyCase ::= StringLiteral ":" TypeName
                              /* Optional polymorphic-dispatch registry on a
                                 base class that also carries a graph-shape
                                 assertion. Each PolyCase maps a wire
                                 discriminator string to a permitted concrete
                                 derived type. The set is closed and explicitly
                                 registered: there is no reflective discovery of
                                 subtypes (rationale §5.18, §5.19). The
                                 discriminator must be a non-empty literal and
                                 unique within the clause; each TypeName must
                                 name a type assignable to the declaring class
                                 and itself serialization-eligible under its own
                                 graph-shape clause. A base class with a
                                 PolymorphicClause may itself be concrete (it
                                 registers its own discriminator like any other
                                 case) or abstract (no self case).
                                 `polymorphic` is a contextual word here, not a
                                 reserved keyword. */
                              /* Class-level opt-in enabling serialization
                                 synthesis for the class (§9.8). Two assertions:
                                 `serialized tree` asserts the serialized graph
                                 is a tree (no two serialized edges alias the
                                 same object, no cycles); `serialized graph`
                                 asserts it is a directed acyclic graph (edges
                                 may alias a shared object, but there are no
                                 cycles); `serialized cyclic` asserts a general
                                 graph that may contain cycles. `tree` and
                                 `graph` are enforced dynamically: `tree` raises
                                 on any repeated object identity; `graph`
                                 preserves the first occurrence and binds later
                                 references to it, but raises on a cycle (a
                                 reference encountered while its own target is
                                 still being constructed). `cyclic` permits such
                                 a reference: it is deserialized via a two-phase
                                 construction (§9.8) and additionally requires
                                 every participating class's *construction* to
                                 be *self-contained* (§9.8 — the rule spans the
                                 constructor body, transitively chained
                                 `this(...)`/`base(...)` constructors, and
                                 field/property initializers), a
                                 statically-checked eligibility restriction; a
                                 class in a `cyclic` group whose construction is
                                 not self-contained is a compile error, not a
                                 runtime fault. `tree`, `graph`, and `cyclic`
                                 are contextual words here, not reserved
                                 keywords. Distinct from the member-level
                                 `SerializedClause` (§9.8 field-name
                                 customization) and from the deserializer's
                                 operation-scoped unknown-field policy (stdlib
                                 §2.15): graph shape is an intrinsic property of
                                 the type's serialization contract, so it is
                                 type-scoped, by the placement principle of
                                 rationale §5.19. */
                              /* The grammar admits modifier combinations the
                                 type system rejects: `static class C(args)`
                                 parses (Modifier* allows `static`,
                                 PrimaryCtorParams? is optional and present)
                                 but is rejected semantically — static classes
                                 cannot be instantiated and so cannot carry
                                 primary-ctor parameters (§11.7). */
ClassBody       ::= Member* "end" Identifier? ";"
Modifier        ::= "abstract" | "sealed" | "static"
                              /* Modifier* permits any combination; the
                                 semantically valid combinations are described
                                 in §11.7. `sealed abstract` is permitted
                                 (closed abstract class); other combinations
                                 with `static` are rejected. */
PrimaryCtorParams ::= "(" ParamList? ")"
Inheritance     ::= ":" InheritItem ("," InheritItem)*
InheritItem     ::= Type ("(" ArgList? ")")?
                              /* The grammar accepts the optional argument list
                                 on every InheritItem; semantically, the
                                 argument list is permitted only on the base
                                 class (the first item), not on interfaces. */

InterfaceDecl   ::= Visibility? "interface" Identifier GenericParams?
                    Inheritance?
                    GenericConstraints*
                    InterfaceMember* "end" Identifier? ";"

RecordDecl      ::= Visibility? "record" Identifier GenericParams?
                    PositionalFields?
                    Inheritance?
                    GenericConstraints*
                    ( ";" | RecordBody )
PositionalFields ::= "(" PositionalField ("," PositionalField)* ")"
PositionalField ::= Identifier ":" Type SerializedClause?
SerializedClause ::= "serialized" ( "skip" | SerializedName )
SerializedName   ::= StringLiteral? "optional"?   /* at least one of the two
                                                     must be present */
                              /* Compiler-recognized field-name customization
                                 for serialization synthesis (§9.8). NOT an
                                 attribute system: a closed vocabulary, no
                                 user extensibility, no runtime introspection.
                                 `serialized "name"` renames; `serialized
                                 optional` makes the field tolerant
                                 (omit-if-null on write, default-if-missing on
                                 read); `serialized "name" optional` combines
                                 the two; `serialized skip` excludes the field
                                 and is mutually exclusive with a name or
                                 `optional`. `skip` and `optional` are
                                 contextual words here, not reserved keywords;
                                 the `SerializedName` form requires at least
                                 one of the string or `optional` (a bare
                                 `serialized` is a syntax error). */
RecordBody      ::= Member* "end" Identifier? ";"

EnumDecl        ::= Visibility? "enum" Identifier EnumInheritance?
                    EnumConstants
                    EnumMember* "end" Identifier? ";"
EnumInheritance ::= ":" IntegerType ("," InheritItem)*
                  | ":" InheritItem ("," InheritItem)*
                              /* The first form is `enum E: int, IComparable<E>` —
                                 explicit underlying type followed by interfaces.
                                 The second form is `enum E: IComparable<E>` —
                                 default underlying type (int), interfaces only.
                                 The grammar disambiguates by whether the first
                                 token after `:` is an IntegerType keyword. */
EnumConstants   ::= EnumConstant ("," EnumConstant)* ";"
EnumConstant    ::= Identifier (":=" ConstExpr)? SerializedClause?
                              /* `serialized "name"` on an enum constant
                                 customizes that member's serialized name
                                 (§9.8). `serialized skip` and `serialized
                                 optional` are not admitted on enum constants
                                 — an enum value is a single member, not a
                                 field that can be omitted or defaulted; using
                                 them here is a compile error. */

ExtensionDecl   ::= Visibility? "extension" Type GenericConstraints*
                    ExtensionMember* "end" ";"

ExternBlock     ::= "extern" StringLiteral ("convention" StringLiteral)?
                    ExternDecl+ "end" ";"
ExternDecl      ::= Visibility? "method" Identifier "(" ParamList? ")"
                    (":" "owned"? Type)? ";"
                              /* `owned` on the return is permitted for
                                 declarations whose foreign function returns
                                 a class instance the Bezel caller takes
                                 ownership of; see §32.5 / §32.7. */
```

<a id="sec-members"></a>
### A.4 Members

```
Member          ::= FieldDecl | MethodDecl | PropertyDecl | IndexerDecl
                  | EventDecl | ConstructorDecl | DestructorDecl
                  | OperatorDecl | NestedTypeDecl | ConstDecl
ExtensionMember ::= MethodDecl | ExpressionPropertyDecl

InterfaceMember ::= IfaceMethod | IfaceProperty | IfaceIndexer | IfaceEvent
IfaceMethod     ::= "method" Identifier GenericParams?
                    "(" ParamList? ")" (":" "owned"? Type)? ";"
                              /* `owned` on the return type follows the same
                                 rule as in MethodDecl. An interface declaring
                                 an `owned` return requires implementations to
                                 declare an `owned` return (§11.2's exact-match
                                 rule). */
IfaceProperty   ::= "readonly"? "property" Identifier ":" Type ";"
IfaceIndexer    ::= "readonly"? "property" "[" ParamList "]" ":" Type ";"
IfaceEvent      ::= "async"? "event" Identifier (":" Type)? ";"

FieldDecl       ::= Visibility "static"? "owned"? "readonly"?
                    Identifier ":" Type (":=" Expression)? ";"
ConstDecl       ::= Visibility "static"? "const" Identifier (":" Type)?
                    ":=" ConstExpr ";"

MethodDecl      ::= Visibility "static"? "async"? VirtualKind?
                    "method" Identifier GenericParams?
                    "(" ParamList? ")" (":" "owned"? Type)?
                    GenericConstraints*
                    MethodBody
                              /* The optional `owned` modifier on the return
                                 type marks methods whose result transfers
                                 ownership to the caller (§28.8). It applies
                                 to class-typed returns; for non-class types
                                 it is rejected semantically. */
VirtualKind     ::= "abstract" | "virtual" | "sealed"? "override"
MethodBody      ::= "=>" Expression ";"
                  | Statement* "end" Identifier? ";"
                  | ";"                /* body-less method: requires
                                          VirtualKind = "abstract"; enforced
                                          semantically. Interface methods use
                                          IfaceMethod, not MethodDecl, so the
                                          ";" body here is exclusively for
                                          abstract members. */

PropertyDecl    ::= Visibility "static"? VirtualKind?
                    "readonly"? "property" Identifier ":" Type
                    PropertyBody
PropertyBody    ::= "=>" Expression ";"
                  | AutoAccessorVis? (":=" Expression)? SerializedClause? ";"   /* auto-property */
                  | Accessor* "end" Identifier? ";"
AutoAccessorVis ::= Visibility ("get" | "set")   /* e.g. `private set` */
Accessor        ::= Visibility? "get" AccessorBody
                  | Visibility? "set" SetterParamSpec? GenericConstraints? AccessorBody
SetterParamSpec ::= "(" ParamMod? Identifier? ")"
                              /* Explicit setter parameter spec, used when
                                 the parameter modifier needs to be declared
                                 (typically `owned` for owned-element-collection
                                 indexer setters). The parameter's type is
                                 always the property's declared type, so it
                                 is not repeated here. When the parameter
                                 name is omitted (only the modifier given,
                                 as in `set(owned)`), the implicit name
                                 remains `value`. When the SetterParamSpec
                                 is omitted entirely (`set` alone), the
                                 implicit `value` parameter has no modifier
                                 (borrow-taking per §14.4 Rule 2 after
                                 substitution). */
ParamMod        ::= "owned" | "ref" | "out" | "out" "owned"
AccessorBody    ::= "=>" (Expression | "do" Statement* "end") ";"
                  | ";"                                  /* abstract / interface body */

IndexerDecl     ::= Visibility "static"? VirtualKind?
                    "readonly"? "property" "[" ParamList "]" ":" Type
                    PropertyBody
                              /* IndexerDecl reuses PropertyBody, whose block
                                 form is `Accessor* "end" Identifier? ";"`.
                                 The optional Identifier is for property
                                 declarations (matching the property's name);
                                 indexers have no name, so the only valid
                                 form for an indexer's block-form `end` is
                                 plain `end;`. Enforced semantically. */

EventDecl       ::= Visibility "static"? "async"? "event" Identifier (":" Type)? ";"

ConstructorDecl ::= Visibility "constructor" "(" ParamList? ")"
                    CtorChain? Statement* "end" "constructor"? ";"
                  | "static" "constructor" "(" ")"
                    Statement* "end" "constructor"? ";"
CtorChain       ::= ("base" | "this") "(" ArgList? ")" ";"

DestructorDecl  ::= "destructor" Statement* "end" "destructor"? ";"

OperatorDecl    ::= Visibility "static" "method" "operator" OpToken
                    "(" ParamList ")" ":" "owned"? Type OperatorBody
                              /* `owned` on an operator's return type is
                                 permitted for class-typed operators that
                                 allocate. Most operator overloads return
                                 records or value types and do not need it. */
OperatorBody    ::= "=>" Expression ";"
                  | Statement* "end" ("operator" OpToken)? ";"
OpToken         ::= "+" | "-" | "*" | "/" | "mod" | "&" | "|" | "^"
                  | "<<" | ">>" | "<" | ">" | "<=" | ">="
                  | "=" | "<>" | "~" | "++" | "--"

NestedTypeDecl  ::= ClassDecl | InterfaceDecl | RecordDecl | EnumDecl

ParamList       ::= Param ("," Param)*
Param           ::= ParamModifiers? Identifier ":" Type (":=" Expression)?
ParamModifiers  ::= "ref"
                  | "out" "owned"?
                  | "owned"
ArgList         ::= Argument ("," Argument)*
Argument        ::= ArgModifiers? (Identifier ":=")? ArgExpression
ArgModifiers    ::= "ref"
                  | "out" "var"?
                  | "out" "using" "var"
                  | "out" "owned"
                  | "owned"
ArgExpression   ::= Expression | "_"
                              /* The "_" alternative is admitted by the grammar
                                 in any argument position; it is semantically
                                 valid only when the corresponding parameter
                                 is `out` and the caller chooses to ignore the
                                 produced value (§22.4). The compiler rejects
                                 "_" in non-out positions. */

EnumMember      ::= MethodDecl | ExpressionPropertyDecl
                  | StaticReadonlyDecl | StaticConstDecl
ExpressionPropertyDecl ::= Visibility "readonly"? "property" Identifier ":" Type "=>" Expression ";"
                  /* "readonly" is redundant on expression-bodied properties (no setter exists);
                     it is permitted for consistency with block-form property syntax. */
StaticReadonlyDecl     ::= Visibility "static" "readonly" Identifier ":" Type ":=" Expression ";"
StaticConstDecl        ::= Visibility "const" Identifier (":" Type)? ":=" ConstExpr ";"

Visibility      ::= "public" | "internal" | "protected" | "private"

GenericParams   ::= "<" GenericParam ("," GenericParam)* ">"
GenericParam    ::= Variance? Identifier
Variance        ::= "in" | "out"
GenericConstraints ::= "where" Identifier ":" Constraint ("," Constraint)*
                  | "where" Identifier "is" ModifierConstraint
                              /* The `is`-form constraint conditions on the
                                 modifier of T's argument rather than on a
                                 type bound. Used in stdlib declarations
                                 to condition methods on `owned T` vs
                                 unmodified T. See §14.4. */
Constraint      ::= "class" | "value" | "notnull" | "new" "(" ")" | Type
                              /* "new" "(" ")" is the parameterless-constructor
                                 constraint, three tokens with whitespace
                                 permitted (lexer-level) — `new()` and
                                 `new ()` are equivalent. */
ModifierConstraint ::= "owned" | "not" "owned"
                              /* `is owned` matches when T's argument carries
                                 the `owned` modifier; `is not owned` matches
                                 when it does not. */
```

<a id="sec-types"></a>
### A.5 Types

```
Type            ::= TypeNoNullable "?"?
TypeNoNullable  ::= QualifiedName GenericArgs?
                  | InlineArrayType
                  | FunctionType
                  | "(" ")"                    /* unit type */
InlineArrayType ::= "[" ConstIntExpr "owned"? "]" Type
                              /* Inline array. Size must be a compile-time
                                 integer constant > 0. The optional `owned`
                                 marker (between size and closing bracket)
                                 marks slot ownership; the array becomes
                                 a move-only value type. See §15.1, §15.3.
                                 The inner Type is parsed greedily — `[3]int?`
                                 means an inline array of nullable int, not
                                 a nullable inline array. Nullable inline-array
                                 types are not provided in this revision. */
ConstIntExpr    ::= /* a compile-time integer expression — integer literals,
                       const declarations referring to integer values,
                       and arithmetic on these. See §15.1 "Size". */
GenericArgs     ::= "<" GenericArg ("," GenericArg)* ">"
GenericArg      ::= Type
                              /* In v0.97 and later, `owned` is not admitted on
                                 generic type arguments; the owned-bearing
                                 collection and task types are separate named
                                 classes (`OwnedArray<T>`, `OwnedTask<T>`, etc.).
                                 See §14.4. */
FunctionType    ::= "(" (FuncParamType ("," FuncParamType)*)? ")" "->" "owned"? Type
                              /* The optional `owned` on the return marks
                                 a function type whose result transfers
                                 ownership to the caller. See §28.1. The
                                 `owned` is part of the function type's
                                 identity: `() -> Connection` and
                                 `() -> owned Connection` are distinct types. */
FuncParamType   ::= FuncParamMods? (Identifier ":")? Type
FuncParamMods   ::= "ref"
                  | "out" "owned"?
                  | "owned"
                              /* Same set as the method-declaration ParamModifiers
                                 (§A.4); function types and method declarations
                                 share modifier syntax exactly. The `out owned`
                                 combination represents an out-parameter that
                                 produces an owned value. */
IntegerType     ::= "byte" | "sbyte" | "short" | "ushort"
                  | "int" | "uint" | "long" | "ulong"
```

<a id="sec-appendix-a-statements"></a>
### A.6 Statements

```
Statement       ::= LocalDecl | LocalConstDecl | AssignmentStmt | ExpressionStmt
                  | DiscardStmt
                  | IfStmt | WhileStmt | ForStmt | DoEndBlock
                  | ReturnStmt | ThrowStmt | BreakStmt | ContinueStmt
                  | TryStmt | YieldStmt
                  | IncrementStmt

LocalDecl       ::= "var" Identifier (":" Type)? (":=" Expression)? ";"
                  | "using" "var" Identifier (":" Type)? ":=" Expression ";"
                  | "await" "using" "var" Identifier (":" Type)? ":=" Expression ";"
                              /* The `await using var` form is admitted by the
                                 grammar at any LocalDecl position; it is
                                 valid only inside `async` methods and `async`
                                 lambdas, enforced semantically (§28.12,
                                 Appendix C.6). */

LocalConstDecl  ::= "const" Identifier (":" Type)? ":=" ConstExpr ";"
                                /* local const; no visibility or static modifier */

AssignmentStmt  ::= LValue AssignOp Expression ";"
AssignOp        ::= ":=" | "+=" | "-=" | "*=" | "/=" | "&=" | "|=" | "^="
                  | "<<=" | ">>=" | "??="
LValue          ::= Identifier | "this"
                  | LValue "." Identifier
                  | LValue "[" ArgList "]"
                              /* LValues have a restricted shape: the base must
                                 be `Identifier` or `this`, and only `.field`
                                 and `[index]` accesses extend the chain.
                                 Method-call results cannot serve as the base
                                 of an assignment; `getCache().items[0] := v`
                                 does not parse. Such cases require extracting
                                 the call result into a local variable first:
                                 `var c := getCache(); c.items[0] := v;`. */

DiscardStmt     ::= "_" ":=" Expression ";"            /* explicit discard */

IncrementStmt   ::= LValue ("++" | "--") ";"
ExpressionStmt  ::= Expression ";"

IfStmt          ::= "if" Expression "then" Statement*
                    ("elif" Expression "then" Statement*)*
                    ("else" Statement*)?
                    "end" ";"

WhileStmt       ::= "while" Expression "do" Statement* "end" ";"

ForStmt         ::= "for" "var" Identifier (":" Type)? "in" Expression
                    "do" Statement* "end" ";"
                  | "await" "for" "var" Identifier (":" Type)? "in" Expression
                    ("with" Expression)? "do" Statement* "end" ";"

DoEndBlock      ::= "do" Statement* "end" ";"

ReturnStmt      ::= "return" Expression? ";"
ThrowStmt       ::= "throw" Expression? ";"
BreakStmt       ::= "break" ";"
ContinueStmt    ::= "continue" ";"

TryStmt         ::= "try" Statement* CatchBlock FinallyClause? "end" ";"
                  | "try" Statement* FinallyClause "end" ";"
CatchBlock      ::= "catch" CatchArm ("," CatchArm)* ","?
CatchArm        ::= CatchPattern ("when" Expression)? "=>" ArmBody
CatchPattern    ::= Type ("," Type)*                          /* no binding */
                  | "var" Identifier ":" Type ("," Type)*      /* binding */
FinallyClause   ::= "finally" Statement*

YieldStmt       ::= "yield" "return" Expression ";"
                  | "yield" "break" ";"
```

<a id="sec-expressions"></a>
### A.7 Expressions

The grammar shown here is informal: it lists the alternatives at each precedence level rather than expanding every binary level into recursive productions. Operator precedence is given in [§17.1](#sec-precedence).

```
Expression      ::= ConditionalExpr
ConditionalExpr ::= LogicalOr ("if" Expression "else" Expression)?
                  | CaseExpr
                  | ThrowExpr
ThrowExpr       ::= "throw" Expression?
                              /* Bare "throw" (no operand) is valid only inside a
                                 catch arm body; outside a catch arm body, the
                                 operand is required. Enforced semantically. */
LogicalOr       ::= LogicalAnd ("or" LogicalAnd)*
LogicalAnd      ::= NullCoalesce ("and" NullCoalesce)*
NullCoalesce    ::= Equality ("??" NullCoalesce)?
Equality        ::= Relational (("=" | "<>") Relational)*
Relational      ::= BitwiseOr (RelTail)*
RelTail         ::= RelOp BitwiseOr
                  | TypeRel TypeOperand
RelOp           ::= "<" | ">" | "<=" | ">=" | "in"
TypeRel         ::= "is" "not"? | "as"
TypeOperand     ::= Type | "null"
                              /* "is", "is not", and "as" take a Type on the
                                 right so that generic type arguments parse
                                 correctly: `x is Array<int>` parses with
                                 `Array<int>` as a Type, not as the bitwise
                                 expression `Array < int >`. The `null`
                                 alternative covers `is null` and `is not null`
                                 (§17.4); `as null` is a type error caught
                                 semantically. The other relational operators
                                 (`<`, `>`, `<=`, `>=`, `in`) take ordinary
                                 expressions and use RelOp. */
BitwiseOr       ::= BitwiseXor ("|" BitwiseXor)*
BitwiseXor      ::= BitwiseAnd ("^" BitwiseAnd)*
BitwiseAnd      ::= RangeExpr ("&" RangeExpr)*
RangeExpr       ::= Shift                                    /* no range */
                  | Shift (".." | "..<") Shift ("step" Shift)?
                                                             /* closed: a..b, a..<b,
                                                                with optional step */
                  | Shift ".."                               /* low-only: a..  */
                  | (".." | "..<") Shift                     /* high-only: ..b, ..<b */
                  | ".."                                     /* fully open: ..  */
                              /* Range bounds parse as Shift expressions, so
                                 `0..n+1` and `1<<2..1<<10` parse correctly,
                                 while `score in 60..100` and `r = 0..10`
                                 parse with the range as the right operand
                                 of the looser `in`/`=` operator. The open-
                                 ended forms support pattern matching (`..<0`
                                 matches values less than 0, `31..` matches
                                 values at or above 31; §19.3) and array
                                 slicing (`arr[..5]`, `arr[10..]`, `arr[..]`;
                                 §16.2). `step` is admitted only on closed
                                 ranges; open-ended ranges have no enumerable
                                 step semantics. */
Shift           ::= Additive (("<<" | ">>") Additive)*
Additive        ::= Multiplicative (("+" | "-") Multiplicative)*
Multiplicative  ::= UnaryExpr (("*" | "/" | "mod") UnaryExpr)*
UnaryExpr       ::= ("+" | "-" | "not" | "~")? PrimaryExpr
                  | "await" UnaryExpr      /* unary-precedence; right-recursive
                                              so `await await x` is permitted */
PrimaryExpr     ::= PrimaryAtom PrimarySuffix*
PrimarySuffix   ::= "." Identifier GenericArgs?
                  | "?." Identifier GenericArgs?
                              /* The optional GenericArgs supplies explicit
                                 method type arguments at a call site:
                                 `obj.Method<int>(args)`. The GenericArgs
                                 belong to the member name; a following
                                 `"(" ArgList? ")"` suffix is the call.
                                 Same "generic arguments in an expression"
                                 family as the PrimaryAtom `Identifier
                                 GenericArgs?` form below. See §22.12. */
                  | "[" ArgList "]"
                  | "(" ArgList? ")"
                  | "?(" ArgList? ")"
                  | "!"
                  | "with" InitAssign ("," InitAssign)* ","? "end"
                              /* The `with` suffix produces either a copy-with-
                                 overrides (when the source is a record) or
                                 setter-mutation on a freshly-constructed class
                                 (when the source is `new C(...)` — object
                                 initializer, §22.11). The form is syntactically
                                 unified; the type checker resolves the semantic
                                 difference from the source expression's type.
                                 Placing `with` at suffix level eliminates the
                                 left-recursive ambiguity that an earlier
                                 grammar had with a standalone WithExpr at
                                 PrimaryAtom level. */

PrimaryAtom     ::= Literal
                  | Identifier GenericArgs?
                              /* The optional GenericArgs lets a generic type
                                 name appear in receiver/expression position so
                                 its static members can be accessed with the type
                                 arguments supplied — `Registry<int>.Register()`,
                                 `Cache<string>.Default`. Without a trailing
                                 member-access (or call) suffix, a bare
                                 `Identifier GenericArgs` is not a complete
                                 expression. The `<` that opens GenericArgs here
                                 is the well-known generics-vs-less-than ambiguity;
                                 the parser admits the GenericArgs reading only
                                 when the `<…>` is balanced and followed by a
                                 token that can continue a primary expression
                                 (`.`, `?.`, `(`, `[`), otherwise `<` is the
                                 relational operator. See §22.12. */
                  | "(" Expression ")"
                  | NewExpr
                  | MoveExpr
                  | LambdaExpr
                  | CaseExpr
                  | "this" | "base"
                  | NameOfExpr | TypeOfExpr | DefaultExpr
                  | CollectionLiteral
                  | "(" ")"                             /* unit value */

MoveExpr        ::= "move" PrimaryExpr
                              /* The PrimaryExpr operand must syntactically
                                 resolve to either: (a) a field access on a
                                 class instance — `Identifier "." Identifier`
                                 (this.f abbreviated as f) or
                                 `PrimaryExpr "." Identifier` (obj.f), where
                                 the named field is `owned`; or (b) an
                                 indexer access `PrimaryExpr "[" Expression "]"`
                                 where the receiver is an owning collection
                                 admitting slot-level move (`OwnedArray<T>`,
                                 `OwnedDictionary<K, V>`, `OwnedBuffer<T>`
                                 for class-typed T, `Span<T>` over class-typed
                                 `OwnedBuffer<T>`, or an owning inline array
                                 `[N owned]…`). Both rejections are compile
                                 errors. See §28.8.1 and §28.11. */

NewExpr         ::= "new" Type "(" ArgList? ")"
                  | "new" "(" ArgList? ")"              /* shorthand */
                              /* The object-initializer form `new T() with X := 5 end`
                                 is parsed as `NewExpr` followed by a `with`
                                 PrimarySuffix; there is no separate ObjectInit
                                 production. */
InitAssign      ::= Identifier ":=" Expression

LambdaExpr      ::= "async"? "(" LambdaParams? ")" "=>" LambdaBody
LambdaParams    ::= LambdaParam ("," LambdaParam)*
LambdaParam     ::= FuncParamMods? Identifier (":" Type)?
LambdaBody      ::= Expression
                  | "do" Statement* "end"

CaseExpr        ::= "case" Expression? CaseArm ("," CaseArm)* ","? "end"
CaseArm         ::= Pattern ("when" Expression)? "=>" ArmBody
ArmBody         ::= Expression
                  | Statement                  /* valid only in statement-position case
                                                  or in a catch arm; reads as a single
                                                  statement whose terminating `;` ends the
                                                  arm body, the same as `do Statement end` */
                  | "do" Statement* "end"     /* type (); valid only in statement-position case
                                                  or in a catch arm */
Pattern         ::= "_"                       /* discard */
                  | "null"                     /* null literal pattern */
                  | "var" Identifier (":" Type)?      /* binding */
                  | Constant ("," Constant)*          /* literal alternatives */
                  | RangeExpr                         /* closed, half-open, or
                                                         open-ended range —
                                                         see RangeExpr productions
                                                         in §A.7's expression
                                                         chain. The grammar's
                                                         RangeExpr admits an
                                                         optional `step`; in
                                                         pattern position the
                                                         `step` modifier is
                                                         rejected semantically
                                                         (§16.4: patterns test
                                                         membership, and stride
                                                         has no membership
                                                         meaning). */
                  | RelOpPattern Expression           /* relational; restricted
                                                         operator set, see below */
                  | Type                              /* type test */
                  | QualifiedName "(" PositionalPatternList ")" /* destructuring */
                              /* The grammar does not syntactically distinguish
                                 a single-Identifier `Constant` (an enum
                                 constant or named const reference) from a
                                 single-Identifier `Type` (a type test): both
                                 productions match `Dog`. Disambiguation is
                                 by name resolution: if `Dog` resolves to a
                                 type, the pattern is a type test; otherwise
                                 it is a constant pattern. The same rule
                                 applies to QualifiedNames — `Color.Red` is a
                                 constant pattern; `System.IO.Stream` is a
                                 type-test pattern. */
PositionalPatternList ::= PositionalPattern ("," PositionalPattern)*
PositionalPattern     ::= Pattern
                        | Identifier              /* naked binding,
                                                     valid only inside
                                                     positional destructuring */

RelOpPattern    ::= "<" | ">" | "<=" | ">="
                              /* Pattern-form relational operators are restricted
                                 to the four ordering operators. `is`, `is not`,
                                 and `as` are not pattern relops because they
                                 belong to TypeRel and take a Type rather than
                                 a value on the right. `in` is also excluded:
                                 a membership test against a collection or range
                                 is expressed with a Range pattern (`0..10`) or
                                 a Constant pattern with alternatives, not as
                                 a relational pattern. */
                              /* The Expression on the right of a RelOpPattern
                                 must have the same type as (or be implicitly
                                 convertible to) the scrutinee's type; the type
                                 check is semantic. The grammar admits more
                                 expression forms than the rule allows — for
                                 example, `< 60..100` parses but is rejected
                                 semantically because `<` between a value and
                                 a range is not defined. */

CollectionLiteral ::= "[" (Expression ("," Expression)*)? "]"
                    | "[" DictEntry ("," DictEntry)* "]"                   /* dictionary */
DictEntry         ::= Expression ":" Expression

NameOfExpr      ::= "nameof" "(" QualifiedName ")"
TypeOfExpr      ::= "typeof" "(" Type ")"
DefaultExpr     ::= "default" "(" Type ")"
```

<a id="sec-constants-and-literals"></a>
### A.8 Constants and literals

```
Literal         ::= IntLiteral | FloatLiteral | CharLiteral
                  | StringLiteral | InterpString
                  | "true" | "false" | "null"
ConstExpr       ::= Expression           /* must be evaluable at compile time */
Constant        ::= IntLiteral | FloatLiteral | CharLiteral
                  | StringLiteral | "true" | "false" | "null"
                  | QualifiedName        /* enum constant or const reference */
```

`Literal` is the form referenced by `PrimaryAtom`; it includes interpolated strings since they are expressions. `Constant` is the more restrictive form used in `case` patterns and `const` initializers; it omits `InterpString` (interpolated strings are not compile-time constants in general) and adds `QualifiedName` for enum constants and named constant references.

---

<a id="sec-appendix-b"></a>
## Appendix B: Reserved Words

<a id="sec-reserved-keywords"></a>
### B.1 Reserved keywords

The following identifiers are keywords. Most are reserved unconditionally and may not be used as user-defined names. The **contextual keywords** (`convention`, `get`, `notnull`, `operator`, `serialized`, `set`, `step`, `value`) have keyword meaning only in specific syntactic positions and may be used as identifiers elsewhere; see [§B.2](#sec-contextual-keywords).

**Unconditionally reserved:**

`abstract`, `and`, `as`, `async`, `await`, `base`, `bool`, `break`, `byte`, `case`, `catch`, `char`, `class`, `const`, `constructor`, `continue`, `decimal`, `default`, `destructor`, `do`, `double`, `elif`, `else`, `end`, `enum`, `event`, `extension`, `extern`, `false`, `finally`, `float`, `for`, `if`, `import`, `in`, `int`, `interface`, `internal`, `is`, `long`, `method`, `mod`, `module`, `move`, `nameof`, `new`, `not`, `null`, `or`, `out`, `owned`, `override`, `private`, `property`, `protected`, `public`, `readonly`, `record`, `ref`, `return`, `sbyte`, `sealed`, `short`, `static`, `string`, `then`, `this`, `throw`, `true`, `try`, `type`, `typeof`, `uint`, `ulong`, `ushort`, `using`, `var`, `virtual`, `when`, `where`, `while`, `with`, `yield`.

**Contextual (see [§B.2](#sec-contextual-keywords)):**

`convention`, `get`, `notnull`, `operator`, `serialized`, `set`, `step`, `value`.

All keywords are lowercase. Type identifiers in PascalCase (`Object`, `Array`, `Type`, `Exception`, `IDisposable`, `Task`, etc.) are not keywords; they are names defined by the standard library or by compiler intrinsics.

<a id="sec-contextual-keywords"></a>
### B.2 Contextual keywords

The following keywords from [§B.1](#sec-reserved-keywords) carry meaning only in specific syntactic positions and may also appear as identifiers in other positions. The compiler distinguishes by context.

| Keyword | Reserved context | May be used as identifier |
|---|---|---|
| `value` | The `where T: value` constraint; the implicit setter parameter | Anywhere else: as a property name, a parameter name, etc. |
| `notnull` | The `where T: notnull` constraint | Anywhere else: as an identifier |
| `step` | After a range expression in `for` and lambda contexts | Anywhere else |
| `convention` | After the library name in an `extern` block header | Anywhere else |
| `get`, `set` | Property and indexer accessor declarations (block form) and auto-property accessor visibility shorthand | Anywhere else |
| `operator` | After `static method` in an operator overload declaration | Anywhere else |
| `serialized` | The `serialized` clause on a positional field, auto-property, or enum constant (field-name customization, [§9.8](#sec-serialization-synthesis)), and the class-header `serialized tree` / `serialized graph` / `serialized cyclic` clause, optionally with a `polymorphic` registry (class synthesis, [§9.8](#sec-serialization-synthesis)). Within these clauses, `skip`, `optional`, `tree`, `graph`, `cyclic`, and `polymorphic` are likewise contextual and are not reserved | Anywhere else: as an identifier |

The setter parameter is named `value` by convention; it is not a true keyword in that role and may be shadowed inside the setter body (though doing so is poor style).

`when`, `where`, `with`, and `yield` are reserved everywhere — although they appear only in specific syntactic positions in practice, the grammar treats them as full keywords for clarity.

<a id="sec-not-vs-notnull"></a>
#### `not` vs `notnull`

`not` (boolean negation, [§6.5](#sec-bitwise-operations)) is unconditionally reserved; it cannot appear as an identifier in any position. `notnull` (the generic constraint, [§14.2](#sec-constraints)) is contextual; it is reserved only in the `where T: notnull` position and may otherwise be used as an identifier (a parameter name, a field name, etc.). The two are distinct tokens — there is no relationship between their lexical statuses despite the visual similarity.

The two-token sequence `is not` ([§17.4](#sec-the-is-is-not-and-as-operators), type test combined with negation) is recognized by the parser specifically in the relational position; it does not require `not` to be contextual. `is not` parses as a single relational operator wherever a relational operator is expected, and `not` outside that two-token sequence remains the unary boolean operator. So `x is not Foo` and `not (x is Foo)` are both legal expressions with the same truth value but different narrowing behavior ([§17.4](#sec-the-is-is-not-and-as-operators)).

<a id="sec-future-reservations"></a>
### B.3 Future reservations

This version of the language does not reserve any identifiers for future use. New keywords introduced in future revisions will be drawn from new compound forms or from words not currently used as identifiers in the standard library.

---

<a id="sec-appendix-c"></a>
## Appendix C: Compiler-Enforced Invariants

This appendix consolidates the invariants the compiler enforces. It is a checklist for implementers and a reference for programmers wanting to know which classes of error are caught at compile time.

<a id="sec-appendix-c-type-system"></a>
### C.1 Type system

- Every variable, field, parameter, and return value has a declared or inferred type.
- Implicit conversions occur only as listed in [§5.1](#sec-implicit-conversions).
- When more than one overload is applicable to a call only because arguments match through different implicit conversions ([§5.1](#sec-implicit-conversions)), the call resolves to the candidate that is unambiguously better — exact on every argument where another candidate converts, and never worse. An exact match is preferred over any implicit conversion; built-in implicit conversions are not ranked against one another. If no candidate is unambiguously better, the call is ambiguous and is a compile error citing the candidates ([§22.6](#sec-implicit-conversion-ranking)); the programmer disambiguates with an explicit cast. This mirrors overloaded method-reference resolution ([§27.6](#sec-method-references-to-overloaded-methods)).
- A non-nullable reference may not be assigned `null`.
- A nullable value may not be dereferenced without a null check or `!`.
- Every non-nullable field is assigned in every constructor or has a field initializer.
- Every non-nullable local is assigned before its first read.
- Definite-assignment analysis applies uniformly to instance and static non-nullable reference fields. A static constructor body's `try`/`catch` is subject to the same flow analysis as any constructor body: every path through the static initializer (including catch-and-recover paths) must definitely-assign every non-nullable reference static field.
- A type's `Equals` override requires a matching `GetHashCode` override.
- A class with one or more `abstract` members must itself be `abstract`.
- An abstract class cannot be instantiated with `new`.
- Member hiding is forbidden: a derived class cannot reuse a base member's name unless overriding a virtual member.
- An override must match the base member's visibility, return type, and parameter types exactly.
- An inheritance cycle is rejected: no type may, directly or transitively, list itself among its base classes or extended interfaces ([§12.10](#sec-inheritance-cycles)).
- A type may not declare two members that are indistinguishable: two fields, properties, events, or nested types of the same name, or two methods, constructors, or operator overloads of the same name that are not distinguishable as overloads ([§22.6](#sec-overloading)), are a compile error. Legal overloads are unaffected ([§11.3](#sec-member-body)).
- Variant generic parameters (`in`, `out`) appear only in the appropriate position.
- A variant assignment whose source has a value-type type argument is rejected; the type itself (e.g. `IEnumerable<int>`) is fully usable, only the variant conversion is forbidden ([§12.9](#sec-variance)).
- Boxing a borrow-bearing record or a borrow-carrying inline array is subject to the same borrow-lifetime constraints as the unboxed value; the boxed value may not escape the source scope ([§8.6](#sec-boxing)). Move-only inline arrays (owning forms) cannot be boxed.
- `default(T)` is rejected for non-nullable reference types, for records whose declared fields include non-nullable references, and for inline arrays whose element type is a non-nullable reference type (including `[N owned]T` non-nullable owning forms).
- `default(T)` on an enum type with no declared zero-value constant produces a compiler warning.
- A collection literal targeting a reference type (`Array<T>`, `OwnedArray<T>`, `Set<T>`, `Dictionary<K, V>`, etc., or a user-defined type via the `FromCollectionLiteral` factory of [§15.4.1](#sec-the-user-defined-sequence-literal-protocol)) produces an owned value; the result must reach an owned location ([§28.3](#sec-local-variables-ownership-and-borrows)). For the user-defined case the owned value is the factory's `owned C<T>` return. A collection literal targeting an inline-array type (`[N]T`) produces a value, with the usual value-type rules; ownership obligations apply only when the array has `[N owned]…` slots ([§15.3](#sec-move-only-value-types)).
- Nullable field access without copying to a local is rejected; only local variables are narrowed ([§7.4](#sec-member-access-on-nullable-values)).
- An `end`-label that is supplied — an identifier for named constructs, or a construct keyword (with operator token where applicable) for keyword-labelled constructs — must match the construct it closes; a mismatch is a compile error. The unlabelled form `end;` is always valid ([§1.6](#sec-statements-and-termination)).
- A `sealed abstract` class may be subclassed only within its declaring module; external subclasses are rejected.
- An enum implementing an interface must provide matching public members for every interface member.
- Calling `.Equals(Object?)`, `.Equals(T)`, or `.GetHashCode()` directly on a non-auto-equality-bearing body record ([§9.2](#sec-body-records)) is a compile error at the call site; the user must declare custom equality or simplify the record to an auto-equality-bearing form. Calls through an `Object`-typed receiver bypass the static-type check and reach the inherited `Object.Equals`/`GetHashCode` at runtime (reference equality / identity-based hash), but this is rare in practice.

<a id="sec-appendix-c-visibility"></a>
### C.2 Visibility

- Every class member declares its visibility explicitly, except `static constructor` and `destructor` (which carry no visibility keyword).
- Every type appearing in a member's signature — field type, parameter type, return type, property type, indexer parameter and result type, event payload type, types named in `where` constraints — is at least as visible as the member. The rule applies at every visibility level except `private`, which is unconstrained.
- A class's base class and implemented interfaces are at least as visible as the class.
- Top-level types are `public` or `internal` only.
- Accessor visibility is more restrictive than the property's overall visibility.
- An override declares the same visibility as the base member.

<a id="sec-appendix-c-resource-management"></a>
### C.3 Resource management

- `Dispose()` is generated for every non-static class.
- The compiler-generated `Dispose()` is thread-safe with respect to idempotency: concurrent calls execute disposal exactly once ([§28.2](#sec-the-idisposable-contract)).
- Owned fields are restricted to class-typed fields (not `string`, not value types).
- Owned values may not escape: returning an owned field requires an `owned` return type; assigning an owned value to a non-owned field of another object is rejected.
- A `new` expression's result must reach an owned location or be explicitly disposed within the same method; the compiler warns on silent discard for non-trivial-dispose classes and rejects discard with `_ := …` outright ([§17.13](#sec-the-discard-_)). For trivial-dispose classes, no discharge is required. Implicit boxing is not subject to this rule ([§28.8](#sec-ownership-transfer)).
- A static method returning a class type without `owned` and without an identifiable borrow source produces an owned value; the same discharge rules as `new` apply ([§28.3](#sec-local-variables-ownership-and-borrows)).
- `using var` variables may not be reassigned.
- Records may not have `owned` fields, may not have inline-array fields with the `owned` slot marker, may not have destructors, may not implement `IDisposable`.
- At a control-flow merge point, a variable that was owned on one branch and borrowed on the other is rejected ([§28.3](#sec-local-variables-ownership-and-borrows)). The two branches must be equalized before the merge.
- A method reference that captures a borrowed receiver is subject to the same closure-escape rules as a lambda capturing a borrow ([§28.3](#sec-local-variables-ownership-and-borrows), [§28.9](#sec-runtime-managed-types)).
- A borrow-bearing record or a borrow-carrying inline array may be stored in a static field only when every reference-typed field (and every reference-typed slot of every inline-array field) of the value is null (the null-source form, [§9.7](#sec-records-and-disposal)).
- A trivial-dispose class (no destructor, no `owned` fields, no fields of owning-inline-array type, no non-trivial base) requires no ownership discharge: undischarged `new` expressions are not warned about, and `_ := newTrivialClass();` is permitted ([§28.9](#sec-runtime-managed-types)).
- A `_ := expression;` discard statement ([§17.13](#sec-the-discard-_)) is rejected for non-trivial-dispose class types, for `Task` and `Task<T>`, and for interface types unconditionally (regardless of whether the interface extends `IDisposable`, since any conforming implementation — including one added later in another module — may carry a non-trivial destructor).
- A class implementing `IAsyncDisposable` without a destructor receives a compiler warning ([§28.12](#sec-asynchronous-disposal)).
- The `move x.f` expression ([§28.8.1](#sec-the-move-expression)) is permitted only when `f` is an owned instance field; the field is left in moved-from state. Within the surrounding method, every control-flow path from the move site to the method's exit (normal return, throw, fall-through, `yield break`) must execute an assignment `x.f := value;` between the move and the exit — including paths exited by exceptions raised by intervening throwable expressions. Failure to satisfy the rule is a compile error.
- The `move coll[i]` expression ([§28.8.1](#sec-the-move-expression), [§28.11](#sec-collections-do-not-own-elements)) is permitted only when `coll` is an owned-element collection; the slot is left in moved-from state. The same field-restoration rule applies, with constant indices tracked per-slot and non-constant indices tracked conservatively at the whole-collection level.
- Owned-element collections (`OwnedArray<T>`, `OwnedDictionary<K, V>`, etc.) own their elements: `Add` accepts owned arguments, the indexer-set disposes the previous value, `Remove`/`RemoveAt`/`Clear` dispose elements, and `Dispose` cascades to every element. The `Drain` method transfers ownership of elements out of the collection in iteration order. Default iteration produces borrows; ownership is transferred only via `Drain` or the dedicated `Pop*` methods ([§28.11](#sec-collections-do-not-own-elements)).
- Iterator yield ownership is determined by the iterator method's declared return type ([§29.2](#sec-desugaring-of-for) "Iterator yield ownership"). For an iterator returning `IEnumerable<T>` or `IAsyncEnumerable<T>` that yields owned class-typed values, the enumerator owns each yielded value and the consumer's iteration variable is a borrow. For an iterator returning `OwnedEnumerable<T>`, `OwnedEnumerator<T>`, `OwnedAsyncEnumerable<T>`, or `OwnedAsyncEnumerator<T>`, ownership transfers from enumerator to consumer at each `MoveNext` (or `MoveNextAsync`); the consumer's iteration variable is owned and must be discharged inside the loop body. Falling off the body without discharging is a compile error.

<a id="sec-appendix-c-generics"></a>
### C.4 Generics

- `where T: notnull` substitutions are non-nullable types; a nullable type, value or reference, does not satisfy the constraint.
- `where T: class` substitutions are non-nullable reference types: classes (including `Array<T>`, `OwnedArray<T>`), interfaces, `string`, function types.
- `where T: value` substitutions are non-nullable value types: records, enums, primitives, inline arrays (all forms, including owning `[N owned]…` forms — see [§15.3](#sec-move-only-value-types) for restrictions on copy operations in the generic body), the unit type. A nullable value type (e.g. `int?`) does not satisfy the constraint ([§14.2](#sec-constraints)).
- `where T: SomeClass` substitutions are non-nullable subtypes of `SomeClass` — class types only (no interfaces, since interfaces do not extend classes).
- `where T: IFoo` substitutions are non-nullable types conforming to `IFoo` per the [§14.2](#sec-constraints) conformance rule: classes implementing it (directly or by inheritance), records implementing it directly, and interfaces extending it (directly or transitively).
- Constraint combinations are checked for consistency (`class, value` is rejected).
- Inside an unconstrained generic body, `T` is treated conservatively as possibly nullable.
- Type-bound constraints (`where T: class`, `where T: SomeClass`, `where T: IFoo`) and `where T: notnull` make T non-nullable inside the body.
- `owned` is not admitted on generic type arguments ([§14.4](#sec-owned-as-a-generic-argument)). The owning collection and task types are separate named classes (`OwnedArray<T>`, `OwnedTask<T>`, etc., declared in stdlib [§4](Bezel_Standard_Library.md#sec-system-collections-generic) and [§6](Bezel_Standard_Library.md#sec-system-threading)); each carries `where T: class` (or `where V: class` for `OwnedDictionary<K, V>`) as part of its declaration.

<a id="sec-methods-constructors-and-accessors"></a>
### C.5 Methods, constructors, and accessors

- Modifier order matches [§11.5](#sec-modifier-order).
- A constructor's `base(...)` or `this(...)` call appears in the chain position before the body, not as a statement in the body (see [§12.8](#sec-constructor-chaining) and grammar A.4 `CtorChain`).
- A class with a primary constructor permits `base(...)` only on the primary constructor; non-primary constructors must chain with `this(...)`.
- A static constructor takes no parameters and no access modifier.
- A method declared `async` returns `Task`, `Task<T>`, `IAsyncEnumerable<T>`, or `IAsyncEnumerator<T>`.
- A method using `yield return`/`yield break` does not also use plain `return`.
- `yield` does not appear in lambdas, in `try` blocks whose surrounding statement has a `catch`, or in catch arm bodies or `finally` blocks.
- `return` does not appear in `finally` blocks.
- `break` and `continue` appear only inside loops.
- `++` and `--` appear only as statements.
- Bare `throw` (the expression form without an operand, and equivalently the statement form `throw;`) appears only inside a catch arm body.
- A field initializer does not reference `this`, instance members, or virtual methods.

<a id="sec-async"></a>
### C.6 Async

- `async` does not combine with `abstract` on a method.
- `await` appears only inside `async` methods, `async` lambdas, and the synchronous prefix of `await for`.
- `await` does not appear in static field initializers or static constructor bodies.
- `await using` appears only inside async contexts.
- The `with` clause in `await for ... with token` must be of type `CancellationToken`.
- Awaiting a `Task<T>` for class T (Pattern A) requires the awaited task expression to occupy one of the admitted owning locations defined in [§30.5](#sec-result-lifetime-and-task-disposal): a reference to a `using var`/`await using var`-bound local of `Task<T>`, an `owned` parameter, an `owned` field of `this`, an indexer access on an owning collection of `Task<T>`, or an iteration variable from a borrowing iteration over an array of tasks. A producer call whose result is directly consumed by a Pattern A return in the enclosing async method is also admitted. A bare `var x := await Producer();` whose `Producer()` returns `Task<T>` for class T and whose result isn't in one of the admitted positions is rejected — the borrowed result would have no source.
- `await` on a `OwnedTask<T>` (Pattern B) must appear in an owned-consuming context — a `using var` (or `await using var`) right-hand side, an `owned` parameter argument, an `owned` return expression, or any other position the type system requires the resulting value to be discharged. Awaiting a `OwnedTask<T>` more than once for a successful result raises `InvalidOperationException` at runtime; the second `await` cannot be statically prevented because the type system does not track whether a particular task has already been awaited.
- `OwnedTask<T>` is a compile error when T is not a class type. The argument-type check is performed during type checking; the diagnostic identifies the non-class argument and points at [§14.4](#sec-owned-as-a-generic-argument).

<a id="sec-patterns-and-case"></a>
### C.7 Patterns and `case`

- A `_` arm appears at most once and is the last arm.
- A type-pattern `T` does not bind; a `var name: T` pattern binds.
- A type-pattern matches only non-null values; `null` matches only the `null` literal pattern, the `var` pattern, and `_`.
- `step` is not used in patterns; it is a range modifier only.
- A record-destructuring pattern's positional-binding count matches the record's positional-field count.

<a id="sec-appendix-c-serialization"></a>
<a id="sec-appendix-c-serialization-synthesis"></a>
### C.8 Serialization synthesis

- A type declaring `ISerializable`/`IDeserializable` ([§9.8](#sec-serialization-synthesis)) that is not serialization-eligible is a compile error **at the conformance declaration**, naming the first offending member.
- A class enters serialization synthesis only by declaring a graph-shape clause (`serialized tree`, `serialized graph`, or `serialized cyclic`); the clause appears in the header before the inheritance list.
- Declaring a weaker graph shape than the data exhibits (`serialized tree` on an aliasing graph, `serialized graph` on a cyclic graph) is detected and raises `SerializationException` at deserialization — never a silently wrong result.
- For a class in a `serialized cyclic` group, **construction must be self-contained**: the constructor body, every transitively chained `this(...)`/`base(...)` constructor, and every field/property initializer that runs during construction may not read a back-edge field, call an instance method on `this`, let `this` escape, or call a virtual/abstract method ([§9.8](#sec-cyclic-class-synthesis)). Violation is a **compile error at the conformance declaration**, never a runtime fault. (The field/property-initializer portion is largely subsumed by the universal restriction of [§22.8](#sec-field-initializers); the constructor-chain coverage is the load-bearing extension.)
- The synthesized two-phase deserialization of a `serialized cyclic` group is the single compiler-controlled construction mode treated as one unit for definite-assignment ([§7.9](#sec-definite-assignment-for-non-nullable-fields)); it is unavailable to hand-written constructors and does not relax [§7.9](#sec-definite-assignment-for-non-nullable-fields)'s guarantee.
- A `polymorphic` registry is closed: each discriminator is a unique non-empty string literal, each registered type must be assignable to the declaring class and itself serialization-eligible (compile error at the registry otherwise); an unregistered discriminator (read) or unregistered run-time type (write) raises `SerializationException`. No reflective subtype discovery.
- `serialized`, and the in-clause words `skip`, `optional`, `tree`, `graph`, `cyclic`, `polymorphic`, are contextual; they are reserved only within their clauses ([Appendix B.2](#sec-contextual-keywords)).

<a id="sec-appendix-c-ffi"></a>
### C.9 FFI

- `extern` methods appear only inside `extern` blocks.
- `extern` declarations have no body.
- `extern` method signatures use only FFI-compatible types ([§32.5](#sec-ffi-compatible-types)).
- Empty `extern` blocks are rejected.

<a id="sec-operators"></a>
### C.10 Operators

- `=` and `<>` are not overloaded on classes.
- Comparison operators are overloaded as a complete group (`<`, `>`, `<=`, `>=` together; `=` and `<>` together).
- The bitwise complement operator is `~`; the boolean negation operator is `not`. They are not interchangeable. `~` is user-overloadable; `not` is not.
- `mod` has no compound-assignment form; `mod=` is not valid syntax.
- Assignment and compound-assignment operators (`:=`, `+=`, `-=`, `*=`, `/=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, `??=`) are not user-overloadable. The `+=` and `-=` syntactic forms on `EventSource<T>` and `AsyncEventSource<T>` are a fixed compiler desugaring, not operator overloads.

<a id="sec-static-initialization"></a>
### C.11 Static initialization

- Cyclic static initialization across types is rejected.
- A `const` declaration's initializer is a compile-time constant.

<a id="sec-entry-point"></a>
### C.12 Entry point

- A program defines exactly one qualifying `Main` method.
- A class declares at most one qualifying `Main`; multiple qualifying overloads in the same class are rejected at compile time without flag-based override.
- `Main` is `public static`. If `async`, its return type is `Task`, `Task<int>`, or one of the parameterized forms; otherwise it returns `()` or `int`.


---

<a id="appendix-d-conformance-levels"></a>
## Appendix D: Conformance Levels

This specification defines two conformance levels. The partition exists so that an implementation can be *complete and sound for a well-defined inner language* before the outer features are attempted, and so that the test-suite and teaching material can follow the same boundary.

<a id="sec-the-levels"></a>
### D.1 The levels

- **Core conformance.** An implementation conforms at the Core level when it implements every normative rule of this specification *except* the Extended features listed in D.2, and *rejects* — with a diagnostic naming the construct and the feature — any program that uses an Extended feature it does not implement. Silent misbehavior on Extended input is non-conforming; clean rejection is the Core-conforming response.
- **Full conformance.** An implementation conforms at the Full level when it implements the entire specification: Core plus every Extended feature.

An implementation may also conform at "Core plus named features" (e.g., *Core + Events + FFI*), implementing some Extended features and rejecting the rest; a conformance claim of this form names the features implemented.

<a id="sec-extended-features"></a>
### D.2 The Extended features

| Feature | Specification sections | Standard-library sections |
|---|---|---|
| **Events** | [§24](#sec-events) | `EventSource<T>`, `IEvent<T>`, `AsyncEventSource<T>`, `IAsyncEvent<T>`, `Subscription` (stdlib [§2.11](Bezel_Standard_Library.md#sec-subscription)) |
| **Asynchronous programming** | [§30](#sec-asynchronous-programming) entire — including domains, spawning, and channels ([§30.13](#sec-runtime-model)–[§30.15](#sec-channels)); [§28.12](#sec-asynchronous-disposal) (`await using`, `IAsyncDisposable`); the async-iterator forms of [§29](#sec-iteration-2) (`IAsyncEnumerable<T>`, `IAsyncEnumerator<T>`, `OwnedAsyncEnumerable<T>`, `OwnedAsyncEnumerator<T>`) | `Task`, `Task<T>`, `OwnedTask<T>`, completion sources, `CancellationToken`/`CancellationTokenSource`, `Mutex`/`Semaphore`/`Atomic<T>`, `Domain`, `Channel<T>`/`ChannelSender<T>`, the async forms of IO and sequence operations |
| **Serialization synthesis** | [§9.8](#sec-serialization-synthesis) | stdlib §2.14–2.15 |
| **Foreign Function Interface** | [§32](#sec-foreign-function-interface); the FFI clauses of [§15.1.3](#sec-inline-array-ffi) | — |
| **Buffers and spans** | the buffer/span clauses of [§28.11](#sec-collection-element-ownership) (`move` on `OwnedBuffer`/`Span` slots) | stdlib §3.3–3.4, §4.14–4.16 |
| **Sequence operations (LINQ-style)** | — | stdlib §5 entire |
| **Shared ownership** | [§28.13](#sec-shared-ownership) | `Shared<T>`, `Weak<T>` (stdlib §2.16) |

Everything not listed is **Core**: lexical structure through generics, records, enums, classes, interfaces, inheritance, nullability, patterns and `case`, expressions and statements, operators and overloading, properties and indexers, the entire ownership system of [§28](#sec-resource-management) (except §28.12 and §28.13), synchronous iteration ([§29](#sec-iteration-2)'s non-async forms, including iterator methods and `OwnedEnumerable<T>`), exceptions ([§31](#sec-exception-handling)), the entry point, and the corresponding standard-library sections.

<a id="sec-feature-dependencies"></a>
### D.3 Dependencies and closure

- Extended features depend on Core; Core depends on no Extended feature. The Core sections' references *into* Extended sections (e.g., [§28.3](#sec-local-variables-ownership-and-borrows)'s notes on async lambdas, [§9.7](#sec-records-and-disposal)'s span examples) are normative only for implementations of the referenced feature.
- The async-iterator forms belong to **Asynchronous programming** and additionally require synchronous iteration (Core). **Buffers and spans**, **Sequence operations**, and **Shared ownership** depend only on Core. **Events**' async event source requires **Asynchronous programming**; the synchronous `EventSource<T>` does not.
- A Core implementation's standard library omits the types of unimplemented Extended features entirely (they are absent, not stubbed); programs referencing them fail name resolution with the feature-naming diagnostic of D.1.

<a id="sec-conformance-and-the-runtime-model"></a>
### D.4 Conformance and the runtime model

The execution-domain guarantees of [§30.13](#sec-runtime-model) are stated within the Asynchronous-programming feature, but their soundness consequences for [§28](#sec-resource-management) hold trivially in a Core implementation: with no `async`/`await`, every program is a single continuation in a single domain. A Core implementation therefore satisfies the domain guarantees by construction.
