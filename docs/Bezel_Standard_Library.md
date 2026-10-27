# Bezel Standard Library Specification

**Companion to the Bezel Language Specification, v0.345 — October 2026**

This document specifies the standard library types referenced by the language specification. The language spec assumes these types exist and have certain shapes; this document defines those shapes.

The library is organized into modules. Some are auto-imported in every file; others require explicit `import`. See [§2.3](Bezel_Language_Spec.md#sec-always-available-names) of the language spec for the auto-import list.

<a id="sec-contents"></a>
## Contents

1. [Conventions](#1-conventions)
2. [`System` (auto-imported)](#2-system-auto-imported)
3. [`System.Text` (auto-imported)](#3-systemtext-auto-imported)
4. [`System.Collections.Generic` (auto-imported)](#4-systemcollectionsgeneric-auto-imported)
5. [`System.Collections.Linq` (auto-imported)](#5-systemcollectionslinq-auto-imported)
6. [`System.Threading`](#6-systemthreading)
7. [`System.IO`](#7-systemio)
8. [`System.Diagnostics`](#8-systemdiagnostics)
9. [Primitive type members](#9-primitive-type-members)
10. [FFI-boundary types](#10-ffi-boundary-types)
11. [Modules deferred to future revisions](#11-modules-deferred-to-future-revisions)

---

<a id="sec-conventions"></a>
## 1. Conventions

The conventions below hold throughout this document. Each row states one convention; *Owned by* names the language-spec section whose rule the convention applies, or *this document* where the convention is the library's own. The library's own longer texts follow the table as prose: the Try convention, the failure-signal rule (with the no-dual rule and error-payload governance), the statics-and-sharing facts, and the sendability statements.

| Convention | Rule | Owned by |
|---|---|---|
| **Signatures** | Method signatures use Bezel syntax, with modifier order per [§11.5](Bezel_Language_Spec.md#sec-modifier-order) of the language spec; a method without a `:` clause returns `()`; every listed member is `public` unless otherwise noted. | spec §11.5, §22.1 |
| **Generic constraints** | Constraints are noted where relevant; absence of a constraint clause means "any T." | spec §14.2 |
| **Conditional members** | A member admitted at some instantiations of its enclosing type only — a **conditionally-admitted member**, spec [§14.2](Bezel_Language_Spec.md#sec-conditionally-admitted-members) — states its condition at its declaration, and this document declares exactly these: `Array<T>.Sort()`, `Array<T>.Sorted()` and `OwningArray<T>.Sort()` (`where T: IComparable<T>`; [§4.4](#sec-arrayt), [§4.5](#sec-ownedarrayt); the remedy is the comparer-taking overload); `OwningBuffer<T>.DangerousPointer()` (`T` in the FFI-pointee set of spec [§32.12](Bezel_Language_Spec.md#sec-foreign-memory), on the value-typed presentation; [§4.14](#sec-owned-buffers-and-spans); `OwningBuffer<IntPtr>` for pointer elements); `Result<T, E>.Map`, `MapError`, `AndThen` and `UnwrapOr` (the receiver and the result instantiation both value-like, spec [§35.4](Bezel_Language_Spec.md#sec-choice-classification) rule 3; [§2.20](#sec-result); the owning match otherwise); the equality search — `ICollection<T>.Contains` and `Remove(item)` with their `Array<T>` and `LinkedList<T>` implementations, `Stack<T>.Contains`, `Queue<T>.Contains`, `Array<T>.IndexOf` and `LastIndexOf`, `LinkedList<T>.Find`, and the span family's `IndexOf`, `Contains` and `SequenceEqual` (the element type, nullability stripped, has a member for spec [§9.2](Bezel_Language_Spec.md#sec-equality-through-a-type-parameter)'s first kind; [§4](#sec-system-collections-generic); declare equality on the element type, or search by a predicate); and every member whose signature forms the nullable companion of its receiver's element or value type as a result, `out` slot or property — the `*OrNull` tier over `IEnumerable<T>` (`FirstOrNull`, `LastOrNull`, `SingleOrNull`, `ElementAtOrNull`), the `Try*(out item: T?)` family (`Stack<T>.TryPop` and `TryPeek`, `Queue<T>.TryDequeue` and `TryPeek`, `Channel<T>.TryReceive`), `LinkedList<T>.First` and `Last`, and `Dictionary<K, V>`'s and `SortedDictionary<K, V>`'s `GetOrNull` and `TryGetValue` at `V` (that type argument is not an inline array, spec [§15.1](Bezel_Language_Spec.md#sec-inline-arrays); a count or index test, the non-nullable member, or an element type that wraps the array in a record). No other member is conditional; the set grows only by recorded ruling. | spec §14.2 |
| **Normative scope** | This document is normative for the names, signatures, and observable behavior listed; it is not normative for performance characteristics, internal data structures, or the choice of algorithm, and implementations are free to optimize. Implementations may add internal helper methods and private members; anything documented here is part of the public contract. | this document |
| **Disposal and the `destructor;` shorthand** | Every class implicitly implements `IDisposable`. A class whose disposal does meaningful work beyond the owned-field cascade (releases a native resource, removes a registration, fires pending callbacks) is shown with a `destructor;` declaration followed by a one-line comment describing the disposal effect, the body being provided by the runtime or implementation; this form is a **documentation shorthand** used in this document only, and user code requires a destructor body per language spec [§28.4](Bezel_Language_Spec.md#sec-the-destructor) (`destructor /* statements */ end;`). The shorthand marks only disposal work *beyond* the owned-field cascade: a class whose `Dispose` is a no-op (memory-only state, no registry, no native handle, and no `owned` fields) shows no destructor, the no-op being inferred from the absence, and a class shown without a destructor may still dispose non-trivially **through its owned fields**, by the compiler-generated default `Dispose`'s cascade through them (language spec [§28.5](Bezel_Language_Spec.md#sec-owned-fields)) — the owning collections' reference implementations and the §2.15 serializers (which own internal buffers) are that case. A trivial-dispose class's instances are compiler-freed at their owner's release point, no runtime lifetime mechanism being involved; the composite trivial-dispose types (`Array<T>` and the borrowing collections' backing, the sorted forms of [§4.17](#sec-sorted-dictionary)/[§4.18](#sec-sorted-set), `Counter<K>` of [§4.20](#sec-counterk)) hold their backing state as an **intrinsic part of the instance's single allocation**, released by that one free — a compiler-known layout device whose user-expressible spelling is `owned` fields of trivial-dispose type. | spec §28.4, §28.5, §28.9 |
| **`owned` returns** | A method returning a freshly-allocated class instance annotates its return type `owned`, the caller's disposal obligation; a method returning `string` or another runtime-managed type ([§28.9](Bezel_Language_Spec.md#sec-runtime-managed-types)) does not, those types being exempt from manual disposal. Two families are deliberately unmarked, the declared return type itself being the transfer announcement — type-announced, and minting at every callee kind under one rule (calls mint, language spec [§28.3](Bezel_Language_Spec.md#sec-producers)): **`Task`-family returns** (every call whose declared return type is `Task`/`Task<T>`/`OwningTask<T>` mints a caller-owned task; [§6.7](#sec-task-static-helpers)) and **iterable-family returns** (every call whose declared return type is an iterable or enumerator interface or a lazy query mints a caller-owned frame — the lazy [§5](#sec-system-collections-linq) operators, `Drain()`, `ReceiveAll()` included). A *stored* task or collection view is re-exposed through a property, field, or slot, whose reads are borrows — never through an unmarked method return of a family type. Where a signature does spell `owned` on such a return (`GetEnumerator`, `Domain.Spawn`), it states the same contract with emphasis. | spec §28.3, §28.8 |
| **Borrowing/owning duals** | Where one operation has a borrowing and an owning argument-side form, the two are a same-name overload pair where their parameter types already differ (the merged `Task.WhenAll`/`WhenEach`, §6.7: ordinary applicability selects, with spec [§22.3](Bezel_Language_Spec.md#sec-parameter-modifiers)'s call-site marker spelling the consuming form), and take distinct names where they would differ only by `owned` (`TryGetValue`/`TryPopValue`, `TryPeek`/`TryPop`), which spec [§22.6](Bezel_Language_Spec.md#sec-owned-and-overload-resolution) does not let overloads do. The surviving `Owned` names each carry a recorded reason (rationale [§3.10](Bezel_Design_Rationale.md#sec-call-site-transfer-marker)/[§3.9](Bezel_Design_Rationale.md#sec-owning-prefix)): the *return-side* `ParseOwned` family (`Json.ParseOwned<T>`, `Binary.ParseOwned<T>`), the return position carrying no call-site marker; and `Task.FromOwnedResult`, whose would-be pair with `FromResult` would differ only by `owned`. | spec §22.6 |
| **Optional trailing argument** | An optional trailing argument with a nameable default — a literal, a constant, `default(T)` (spec [§22.5](Bezel_Language_Spec.md#sec-optional-parameters-and-named-arguments)'s permitted default-value expressions) — is **one** signature carrying a `:=` default, not an overload pair (`Task.Delay(milliseconds, token := CancellationToken.None)`, `Directory.Delete(path, recursive := false)`, `Debug.Assert(condition, message := "")`), and the parameter's name is API surface exactly as at any other position. A same-name pair is declared where the two forms differ by parameter *type* or by `owned` (the duals above); where the shorter form is an interface-required member whose signature must match exactly (spec [§11.2](Bezel_Language_Spec.md#sec-interface-declaration)) — `String.CompareTo`/`Equals` beside their `ignoreCase` forms ([§9.6](#sec-string)); or where the omitted argument has **no nameable default** — the *to-the-end* window forms (`Slice`, `AsSpan`, `AsReadOnlySpan`, `Substring`, `String.Remove`), whose shorter form means the remainder of the sequence rather than a default value, and the [§2.15](#sec-concrete-serialization-formats) JSON facade's `options: JsonOptions`, which names no default constant (`DECLINED.md`, *One-shot serialization facade alternatives*). | spec §22.5 |
| **Compiler-provided constants** | A compiler-intrinsic type (primitive wrappers, `Range<T>`) may declare `const` members whose values the compiler provides rather than a source-level initializer expression: a `const` field shown without `:= value` (`public static const MaxValue: double;`) is such a constant. User code may not omit a `const` initializer (language spec [§22.12](Bezel_Language_Spec.md#sec-static-members)). | spec §22.12 |
| **Elided interface-required members** | The members an implemented interface requires are part of the class's public contract whether the declaration shows them or elides them with a marker comment (`// plus standard ICollection<T> operations`, `// ICollection<T> operations plus:`) — the two forms are equivalent — with signatures determined by the interface declaration plus the implementation's type substitution (the [§11.2](Bezel_Language_Spec.md#sec-interface-declaration) exact-match rule). Where the substitution produces a member with the same name as, but a different parameter type from, another method on the class, both are public members and both are reachable — `Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>`'s `Add(item: DictionaryEntry<K, V>)` beside `Add(key: K, value: V)`. | spec §11.2 |
| **Extensibility** | A class of this library admits a user-declared subclass only where its section says so: `Encoding` ([§3.2](#sec-encoding)), `Stream` ([§7.1](#sec-stream)), `TextReader` and `TextWriter` ([§7.4](#sec-textreader-textwriter-directory-path)), and the exception types — the built-in family (language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)) and this document's producer-local exceptions ([§2.17](#sec-built-in-exceptions-cross-reference)). Every other class is closed to inheritance as a `sealed` class is, whether or not its listing writes `sealed`; `sealed` on a listing (`Type`, `SharedEventSource<T>`) is informative. | spec §12.4 |
| **Block-by-name** | An instance member blocks a same-named extension, document-wide: extension resolution commits to instance members **by name** — any arity, applicability, or accessibility, inherited ones included (spec [§26.5](Bezel_Language_Spec.md#sec-resolution) rule 1) — so wherever a type of this document declares or inherits an instance member named `Count`, `Contains`, `First`, `Last`, `Reverse`, or `ToArray`, a call on that receiver resolves to the member and the same-named [§5](#sec-system-collections-linq) operator is unreachable there by name (`arr.Count(pred)` does not resolve to LINQ's predicate count on an `Array<T>`, because `Array<T>` declares the `Count` property). The semantics are usually equivalent where both exist (`Array<T>.Contains` and LINQ's `Contains` agree); the differences, where they exist, are the member's own documented behavior, and the composed spelling — `coll.Where(pred).Count()`; `Where` is shadowed nowhere in this document — reaches the operator pipeline. A per-name workaround tier (`CountWhere`) is declined: rationale [§15.z](Bezel_Design_Rationale.md#sec-instance-member-shadows-extension). | spec §26.5 |
| **Exception safety when a mutator re-enters user code** | Where a mutating member calls back into user code while the receiver is mid-mutation — a comparator (`Sort`, `Sorted`, and the sorted collections' insertion path), `Equals`/`GetHashCode` (the hashed containers' `Add`, `Remove`, `Contains`, and indexer-set), or a caller-supplied predicate, selector, or key function on any member that takes one — and that code raises, **the receiver remains valid and usable**: every invariant its own type states still holds, every element it still holds is intact, and no element is left half-transferred (on an **owning** container the dispose-exactly-once obligation is preserved for every element, either still held by the container or already transferred to the caller, never both and never neither). **The receiver's contents are otherwise unspecified** — a partially-applied ordering, a partially-completed insertion or removal, or a partially-drained sequence is admitted — and no member promises the strong (all-or-nothing) guarantee unless its own text says so; a caller that needs all-or-nothing operates on a copy and installs the result. This is the basic guarantee, a convention of this document that the language imposes on no user type: the strong guarantee would require a shadow copy, the value copy [§4.5](#sec-ownedarrayt) says the owning family cannot have, while spec [§28.4](Bezel_Language_Spec.md#sec-the-destructor)'s multi-value-cleanup rule and [§28.7](Bezel_Language_Spec.md#sec-reassigning-owned-fields)'s drop-on-overwrite make the exactly-once accounting statable on an exception edge. The [§4.4](#sec-arrayt) sorting contract spells the per-member obligation for `Sort`; its per-member clauses are this convention's instances and remain the stronger, member-specific statements. | this document |
| **Negative magnitudes fault (#1396)** | Every constructor parameter that states a magnitude — `capacity`, `size`, `length` — raises `ArgumentOutOfRangeException` when passed a negative value: `StringBuilder(capacity)`, `Array<T>(capacity)`, `Array<T>(size, fill)`, `OwningArray<T>(capacity)`, the `Dictionary`/`OwningDictionary`/`Set`/`OwningSet` capacity forms, and `OwningBuffer<T>(length)` with its `init` overloads. Zero is valid (an empty container or zero-length buffer). A negative magnitude is a programming error, not a request — the defined-fault stance of `DateTime`'s component validation (§2.5), chosen over a silent empty fallback. `Channel<T>(capacity)` is stricter under its own contract: `capacity >= 1`, `ArgumentException` otherwise. | this document |
| **The `Async` suffix** | The suffix marks the asynchronous member of a sync/async pair (`Acquire`/`AcquireAsync`, `Send`/`SendAsync`, `ReadAll`/`ReadAllAsync`, `Lock`/`LockAsync`); an operation that exists only in asynchronous form carries no suffix (`Channel<T>.Receive`, `TaskScope.Join`, `Task.Delay`). `Receive` beside `SendAsync` on the §6.9 endpoint pair is this rule at work, not an inconsistency. | this document |
| **Acronyms and widths (#1359)** | Acronyms in Bezel-coined names are PascalCase (`IeeeEquals`); names inherited verbatim from .NET keep the .NET spelling (`IEEERemainder`, `Encoding.UTF8`, `ToInt64`) — so `Math.IEEERemainder` and `Double.IeeeEquals` are both *correct* under one rule. Where a bit-width is the point of the name, the width-bearing .NET type words are used (`Int32`, `Int64` — e.g. `Random.NextInt64`, `BitConverter`'s family). The owning enumeration interfaces carry the `I-` prefix (#1372). | this document |
| **`Count` vs `Length`** | `Length` measures a fixed or contiguous extent — text (`String`, `StringBuilder`, `ReadOnlyStringSpan`), buffers and spans (§4.14–§4.15), `FrozenArray<T>`, `Stream`, and the §10 FFI carriers; `Count` counts the elements of a collection whose membership grows and shrinks (the `Add`/`Remove`/`Push`/`Enqueue` families). A type declares one or the other, never both, and new types follow the same line — which is why the resizable `Array<T>` says `Count` where .NET's fixed array says `Length`. | this document |
| **Key membership is `Contains(key)`** | A map-like type spells key membership `Contains(key)`, so that `key in m` resolves (spec [§17.5](Bezel_Language_Spec.md#sec-the-in-operator)'s structural `Contains` lookup); `ContainsKey` on `Dictionary<K, V>`, `OwningDictionary<K, V>` and `SortedDictionary<K, V>` is the inherited .NET alias (rationale [§15.x](Bezel_Design_Rationale.md#sec-convert-is-a-familiarity-alias)'s familiarity ground) and is not minted on new types — `Counter<K>` ([§4.20](#sec-counterk)) declares `Contains` alone. | spec §17.5 |
| **Window parameters** | A parameter pair naming a sub-range of a sequence is spelled **`(start, length)`** — `startIndex` stands as the string family's established spelling of the start position — and every new windowed signature follows it; parameter names are API surface (named arguments, spec §22.5). The one recorded exception is the **`Stream` byte-window family** (`Read`/`Write(buffer, offset, count)`, §7.1), which keeps `(offset, count)` deliberately: those are the signatures C-family muscle memory types unprompted. The convention names are `String.From(buffer, start, length)` and `String.Remove(startIndex, length)`; `String.From` keeps its window because `Array<char>` has no span-shaped route to `string` — a span twin, if one is wanted, retires the window with it. **Slice first:** the reads and writes of the span accessors ([§4.14](#sec-owned-buffers-and-spans)'s byte and bit tiers, and their [§4.15](#sec-arrayspant-readonlyarrayspant) members) and the span-taking overloads (`Stream`'s span `Read`/`Write`, [§7.1](#sec-stream); the [§3.2](#sec-encoding) span-source `GetString`) take no offset parameter — each operates from the start of its span (bit 0 of a bit view), and an offset is reached by slicing first, with the one-argument `Slice(start)` every span type declares (`Slice(bitStart)` on a bit view): `header.Slice(8).ReadInt32LittleEndian()`. A window over an `Array<byte>` is likewise not a parameter pair but a slice — `Encoding.GetString(bytes.AsReadOnlySpan(start, length))`. (`destOffset` on `RawBytes.CopyTo` names a write *position*, not a window, and is untouched.) | this document |
| **Replay intercept points** | The members of this library that consult the world outside the process — the clock and elapsed-time reads (§2.5, §8.1), entropy (§2.6, and §2.7's parameterless constructor), environment, argument, and process information (§2.4), console input (§2.1), and the file system (§7) — together with the scheduling and synchronized-object operations the language spec enumerates, are **intercept points**: a conforming runtime may record their results and later substitute the recorded values, and no program may observe the substitution except through the values themselves. Everything not so designated is pure computation and is deliberately not interceptable. A future addition to this library that reads the world must declare itself an intercept point in its own section at the time it is added. | spec [§30.13](Bezel_Language_Spec.md#sec-determinism) (*Determinism*) |

**The Try convention (#1336).** Four `Try` shapes are sanctioned — three for payload extraction and one payload-free — and the table is the rule:

| Shape | Signature | False path | Members |
|---|---|---|---|
| **(a)** `bool` + nullable out | `TryX(..., out item: T? when true): bool` (`owned` on the out where the payload transfers) | assigns `null` | keyed/positional extraction: `TryGetValue`, `TryPop`/`TryPeek`/`TryDequeue`, `TryPopFirst`/`TryPopLast`, `Channel<T>.TryReceive`, `OwningSet<T>.TryPop`, `OwningDictionary<K, V>.TryPopValue`, and `IParsable<T>.TryParse` through the constraint |
| **(b)** nullable return | `TryX(...): owned T?` | returns `null` | acquisition-shaped members whose payload is statically non-nullable at every instantiation: `Mutex.TryAcquire` (simpler call sites), `Weak<T>.TryUpgrade` (a bool would carry no information) |
| **(c)** `bool` + non-nullable value-typed out | `TryX(..., out result: T): bool` | assigns `default(T)` (spec §22.3) | a **concrete value type** payload: the §9 primitive `TryParse` family, `Convert.TryTo*` (§2.3), the `DateTime`/`DateOnly`/`TimeOnly`/`Guid` `TryParse` members (§2.5/§2.6), the §4.14/§4.15 `TryRead*` accessors |
| **(d)** bool-only attempt | `TryX(...): bool` — no out; the payload, if any, was passed *in* | — | succeeded/declined operations: `TryAdd` (§4.8–§4.10, §4.17/§4.18), the `TrySet*` completion family (§6.6), the span `TryWrite*` accessors (§4.14/§4.15) |

Why the shapes split as they do. In (a) the bool distinguishes the outcome from a legitimately-null payload, and for a **generic or class-typed** payload the out parameter is *nullable in every form*, because a class payload has no `default` to assign on the false path (spec §17.11) and a generic payload may be instantiated by a class; §22.3 requires every out assigned on every return path (throw paths carry no obligation), and the false path assigns `null`. (b) is for members where null alone is the signal. In (c) the `when true` postcondition does not apply — the out is never null, so `default(T)` (a real value distinguished only by the `bool`) is what the false path assigns. (d) carries no nullability postcondition: there is no out to narrow. **Every shape-(a) signature in this document carries the `when true` nullability postcondition** on its out parameter (language spec §22.4) — `TryX(..., out item: T? when true): bool` — so in a region where the call is known `true`, the recipient is narrowed non-null and no `!` is needed; the clause is elided from the per-type listings below for brevity but is normative for **every** shape-(a) member in this document (the table's (a) row names them). **The `*OrNull` convenience tier.** Beside the Try shapes, a surface may carry a nullable-return **convenience form**, named with the `OrNull` suffix (`ParseOrNull`, `GetOrNull`, `ReadKeyOrNull`, §5's `*OrNull` element accessors — `ElementAtOrNull` completing that set) — a recorded tier in the sense of the no-dual rule below, and band 2's nullable-return value shape in the failure-signal rule below. The three access tiers, named: the **throwing form** (`Parse`, the `[key]` indexer) treats absence as a bug, the **`*OrNull` form** returns `T?` for direct `??`/`?.` composition where the payload can carry the signal (where it cannot — a nullable payload type — the surface carries the §7.10 conflation caveat: §4.8, §4.17, §5), and the **`Try` form** remains the only shape that distinguishes the outcomes a collapsed `T?` cannot (spec §7.10). Shape (b)'s `TryX` spelling stays reserved for acquisition-shaped members whose payload is statically non-nullable (`TryAcquire`, `TryUpgrade`). Neither a shape-(a) member nor an `*OrNull` member is admissible where the type argument its `T?` is formed at — the element type, or a dictionary's value type — is an inline array: an inline array has no nullable form (language spec [§15.1](Bezel_Language_Spec.md#sec-inline-arrays)), so each is a conditionally-admitted member there (language spec [§14.2](Bezel_Language_Spec.md#sec-conditionally-admitted-members); the *Conditional members* row above), a compile error at the call, or at the formation of a method reference over the member (language spec [§27.6](Bezel_Language_Spec.md#sec-method-references)) — `Channel<[3]int>.TryReceive` and `Array<[3]int>.FirstOrNull()` among them — and the throwing form, the non-nullable member or a count or index test is the access.

<a id="sec-failure-signal-rule"></a>
**The failure-signal rule.** Every fault **condition** a member of this library declares is classified by the first matching band below, and the band fixes the condition's one primary mechanism. One member may declare conditions in different bands — an absence condition and a corruption condition on the same operation classify separately — but never two mechanisms for one condition. The rule binds declarations, not history: it governs fault conditions declared or re-ruled from this revision forward; an existing spelling stands until its surface is re-ruled, and a landed spelling the rule would place in a different band is a recorded misfit — flagged where it stands, moved only by its own graduation ruling — never a conformance error.

1. **Caller bug.** The condition is something a correct caller already guarantees (a bad argument, protocol misuse, a state violation) → a `requires` clause where a contract is possible (language spec [§22.16](Bezel_Language_Spec.md#sec-contract-clauses)); the residual defect exceptions (such as `Argument*` and `InvalidOperationException`) at pre-contract boundaries. A defect surface is never a recovery surface: its exception exists to report a bug, not to be branched on.
2. **Expected absence, no information.** The condition is the absence of a **queried value** — a lookup, extraction, or acquisition whose target may legitimately not be there — expected, and carrying no information beyond its occurrence → the value shapes of this section: a nullable return where null alone is the signal, or a Try shape per the Try convention above. An operation that could not be *performed* is not an absence, however expected the failure — it classifies under the bands below.
3. **Expected failure with information, branch-consumed, value-like payload.** The failure is expected, carries information the caller branches on at or near the call site, and the error payload is spellable as a **value-like** type (language spec [§35.4](Bezel_Language_Spec.md#sec-choice-classification) — a record or choice over values and strings; a plain inline array of value elements serves bounded aggregation) → a `choice` return (`Result<T, E>`, [§2.20](#sec-result), or a domain choice) — the positive half of the language spec's "never with `requires`" boundary ([§22.16](Bezel_Language_Spec.md#sec-contract-clauses)). The payload type is governed by the error-payload governance rule below.
4. **Boundary-consumed fault (the residual band).** The failure's consumption site is a boundary, supervision, or retry site — the fault is logged, converted to a protocol answer, retried, or reported, rather than branched on for program logic — **or** the condition is band-3-shaped (expected, information-bearing, branch-consumed) but its payload cannot be value-like (language spec [§35.4](Bezel_Language_Spec.md#sec-choice-classification) — an owned payload makes the instantiation owning: consumable only by owning match, language spec [§35.5](Bezel_Language_Spec.md#sec-choice-matching), and never directly sendable, language spec [§35.12](Bezel_Language_Spec.md#sec-choice-sendability)) → a typed exception, producer-local per language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types), **regardless of how much information it carries**, with the classification and its reason recorded beside the surface. The typical inhabitant is the environmental fault (I/O, network, OS state); environmental origin describes the band, it is not a conjunct of the test. Information content never forces band 3: a fault may carry rich data and still sit here, because the data is consumed at a boundary that reads it once — the `catch` copy-out model (language spec [§31.1](Bezel_Language_Spec.md#sec-throwing-and-catching)) — not dispatched on downstream.
5. **Fatal.** The process cannot meaningfully continue → termination: allocation failure and `StackOverflowException` (language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)), `Debug.Assert` ([§8.2](#sec-debug)).

The band-3/band-4 boundary, stated once: what distinguishes them is **where consumption happens** (call-site branching vs. boundary handling) **and whether the payload can be value-like** — never whether information exists. A condition satisfying band 3's first two conjuncts whose payload cannot be value-like takes band 4's mechanism, and the recorded reason keeps the classification revisitable. One carve: a condition whose value-shaped spelling would have to hand ownership of a value *back* to the caller — the give-back class ([§6.9](#sec-channel)'s send family, whose failure path would need to hand the owned item back) — is neither band's material: its spelling is an ownership-protocol design under the language spec's ownership rules, and the no-dual rule below applies to whatever spelling that design fixes.

**The no-dual rule.** One primary mechanism per fault condition. A member family may additionally offer recorded **tiers** — the assert-shaped throwing access ("failure is a bug at this call site") beside the branch-shaped value access ("failure is expected here"), the [§9.1](#sec-integer-types) `Parse`/`TryParse` pair being the model — because tiers serve different call-site *stances* toward one operation, not the same problem twice. Anything else — two peer spellings of one stance, a `Result` dual beside a Try dual for the same branch-shaped stance, an exception and a choice both offered for branching on the same condition — is a violation, and the newer spelling is the one that yields.

**Error-payload governance.** An error-payload type — the `E` of a `Result<T, E>` return, or a domain choice declared to carry a producer's failures — lives with its producer: it is declared alongside the producer whose failures it describes and carries the producer's import (the producer-locality discipline of language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types), transposed from the exception taxonomy). Error-payload types are value-like (language spec [§35.4](Bezel_Language_Spec.md#sec-choice-classification)) unless a recorded ruling states otherwise. One producer family, one payload type: a second module extends by wrapping, never by widening the producer's type. Additions to a shipped payload type's case set are gated by recorded ruling, exactly as growth of the exception taxonomy is — and with sharper consequence here, because a choice is a closed sum: every case addition breaks every exhaustive consumer (language spec [§35.6](Bezel_Language_Spec.md#sec-choice-exhaustiveness)).

<a id="sec-statics-and-sharing-facts"></a>
**Statics-and-sharing facts (proposal 0087).** Certain library types carry up to four normative per-type **facts**, each declared on a **Facts** line in the type's own section: **synchronized** — the type's operations are internally synchronized *and* an instance is safe to reach from every domain (a conjunction: `Channel<T>` declares no fact however synchronized its internals — its single-consumer receiving end is kept domain-confined by recorded policy, the ground on §6.9's fact line); **immutable** — deeply immutable after construction, so cross-domain reach needs no machinery; **self-distributing** — the type already has its own sanctioned distribution channel, so `Shared<T>` wrapping is excluded (it would add a second); and **displacement-unbounded** — the value a mutable static's overwrite would strand (language spec [§22.12](Bezel_Language_Spec.md#sec-static-members) rule 2's overwrite-never-disposes store semantics) is of unbounded, payload-scaled size **and that cost was ruled unacceptable for mutable statics of the type** — contrast the `Task?` cache's bounded acceptance (language spec §22.12 rule 2's store semantics) and `SharedEventSource<T>`'s accepted subscriber-list leak ([§6.5](#sec-events-ievent-eventsource)), neither of which carries the fact. The language spec **derives** its three statics-and-sharing sets from these facts by three normative formulas: [§22.12](Bezel_Language_Spec.md#sec-static-members) rule 2's **form-(i)** set = synchronized ∧ ¬displacement-unbounded; [§22.12](Bezel_Language_Spec.md#sec-static-members) rule 4's **warning-suppression set** = synchronized ∨ immutable; [§28.13](Bezel_Language_Spec.md#sec-shared-ownership)'s **`Shared`-payload synchronized set** = (synchronized ∨ immutable) ∧ ¬self-distributing. The fact lines are the single normative carrier of the memberships; the spec's membership parentheticals beside each formula are informative and derived (`scripts/check_docs.sh` check 3b recomputes them from the fact lines — and cross-checks these formulas and the fact-line roster — failing on divergence). A fact is granted or withdrawn only by **recorded ruling** — the ruling names the facts it grants and confronts each derived-set consequence in its record — and a type declaring no facts is in none of the derived sets.

<a id="sec-sendability-statements"></a>
**Sendability statements.** A compiler-known class type's sendability (language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)) is stated in its own section — in a **Sendability** paragraph where the section has one — in one of the four forms §30.14 defines: **sendable**; **sendable iff** its element types meet the stated condition (the collections, `OwningBuffer<T>`, `OwningSlotMap<T>`); **sendable iff** its payload is in the `Shared`-payload synchronized set (`Shared<T>`); or **not sendable** (domain-local). The statements are the single normative carrier of these classifications; the spec states the forms and the rules for every other type, and lists no per-type memberships. A compiler-known class type whose section states no sendability (a type of this library, or a built-in exception type of language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)) is **not sendable**. Such a type becomes sendable only by a statement in its own section. A statement classifies every instance of its type, because a class of this library admits no user subclass unless its section opens it (*Extensibility* above; language spec §12.4). A user class deriving from an opened class is a declared class under §30.14's transfer-closed rules, whose rule 2 reads the base's inherited state as transfer-compatible only where the base's section states so; no section of this document states it, so such a class is not sendable. `string` and `Type`, runtime-managed (language spec §28.9) though this document lists them as classes, are classified by §30.14 directly, as the value types are, and their sections restate the verdict. The transfer-closed rules for declared classes do not classify a compiler-known class: they read instance fields, and this document is not normative for internal data structures (above), the fields of a reference implementation included. A statement is independent of the facts above: `CancellationTokenSource` is **synchronized** and not sendable ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)), and `Channel<T>` carries none of the facts and is sendable ([§6.9](#sec-channel)).

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
  // (#1355): per-type Write/WriteLine overloads for bool and the numeric
  // primitives (same shape as StringBuilder.Append §3.1) — behavior =
  // Write(value.ToString()); no Object boxing on the path. TextWriter (§7.4) mirrors them.

  // Input
  public static method ReadLine(): string?;     // null at EOF
  public static method ReadKey(): char;         // blocks for one key; EndOfStreamException at end of input (see below)
  public static method ReadKeyOrNull(): char?;  // (§1 *OrNull tier): null at end of input; otherwise blocks for one key

  // Streams
  public static readonly property In: TextReader;
  public static readonly property Out: TextWriter;
  public static readonly property Error: TextWriter;

end Console;
```

The `Object` overloads invoke `ToString()`. A value that holds borrows — a buffer- or array-backed span, or a borrow-bearing record — is not passed to them: the parameter is a borrow-erasing position (language spec [§9.7](Bezel_Language_Spec.md#sec-records-and-disposal)), and `x.ToString()` or an interpolation hole prints the same text. For interpolated strings, the compiler resolves the `string` overload directly. A call to an `Object` overload whose argument is statically of primitive type is lowered without boxing (implementation guidance — not observable, and §1's performance disclaimer applies). No overload takes a nullable parameter, so a `null` argument — the literal, or an un-narrowed `string?` or `Object?` — has no applicable candidate and is a compile error (language spec [§5.1](Bezel_Language_Spec.md#sec-implicit-conversions), [§22.6](Bezel_Language_Spec.md#sec-overloading); [#6778](https://github.com/s8lap/bezel-lang/issues/6778)). A possibly-absent value prints through an interpolation hole, which renders an absent value as the four characters `null` with no `ToString()` dispatch (language spec [§18.5](Bezel_Language_Spec.md#sec-interpolated-strings)), or after a narrowing check or `??`.

**Line termination, and the stream properties.** `WriteLine` writes its argument followed by `Environment.NewLine` ([§2.4](#sec-environment)); the no-argument `WriteLine()` writes `Environment.NewLine` alone. `Write` writes its argument with no terminator. `Console.Write` and `Console.WriteLine` **are** `Console.Out.Write` and `Console.Out.WriteLine`: one destination, so output written through the two spellings interleaves in call order. `Console.Error` is a separate destination, ordered against `Out` only as the host orders the two streams.

`ReadKey()` blocks until one key (code point) is available and raises `EndOfStreamException` ([§7.5](#sec-io-exception-types); catching it by name requires `import System.IO;`) at end of input, where no key can exist. `ReadKeyOrNull()` is the [§1](#sec-failure-signal-rule) `*OrNull` tier beside it — `null` at end of input, the key otherwise — resolving the misfit the failure-signal rule: end of input is band 2's expected absence, and `char?` is exactly the `ReadLine` null-at-EOF analog this tier would otherwise lack (`char` is a non-nullable value type, so `null` is unambiguous). `ReadKey` remains the assert-shaped throwing tier — end of input is a bug at its call sites. Whether the key is echoed is implementation-defined.

<a id="sec-math"></a>
### 2.2 `Math`

A static class of mathematical functions and constants. Constants are `double`.

```bezel
public static class Math
  public static const Pi: double := 3.14159265358979323846;
  public static const Tau: double := 6.28318530717958647692;

  public static pure method Abs(x: int): int;
  public static pure method Abs(x: long): long;
  public static pure method Abs(x: double): double;
  public static pure method Abs(x: decimal): decimal;

  public static method Min<T>(a: T, b: T): T where T: IComparable<T>;
  public static method Max<T>(a: T, b: T): T where T: IComparable<T>;
  public static method Clamp<T>(value: T, low: T, high: T): T where T: IComparable<T>;
      // low > high: ArgumentException

  public static pure method Sqrt(x: double): double;
  public static pure method Pow(x: double, y: double): double;
  public static method DivRem(a: int, b: int, out remainder: int): int;    // returns the quotient;
  public static method DivRem(a: long, b: long, out remainder: long): long; //   b = 0 raises DivideByZeroException
                                                                            //   (§6.1, as `/`). Not `pure`: a pure
                                                                            //   member declares no `out` (§22.15);
                                                                            //   the tuple shape 0062 sketched does
                                                                            //   not exist (no tuples).

  public static pure method Floor(x: double): double;
  public static pure method Ceiling(x: double): double;
  public static pure method Round(x: double, digits: int := 0): double;   // banker's rounding
  public static pure method Truncate(x: double): double;

  public static pure method Floor(x: decimal): decimal;    // toward negative infinity;
  public static pure method Ceiling(x: decimal): decimal;  //   toward positive infinity — each to an
  public static pure method Truncate(x: decimal): decimal; //   integral value; exact, never faults
                                                           //   (decimal's range endpoints are integral).
                                                           //   Truncate is toward zero. Round(decimal)
                                                           //   is the instance Decimal.Round (§9.3).

  public static method Sign<T>(x: T): int where T: INumber<T>;   // -1, 0, or 1: two `<` tests against
                                                                  //   T.Zero() (§2.19), so a NaN yields 0.
                                                                  //   Not `pure`: INumber<T>'s operators are
                                                                  //   not pure-declared (spec §22.15's
                                                                  //   transitive rule), as Min/Max/Clamp above

  public static pure method Fmod(x: double, y: double): double;
  public static pure method IEEERemainder(x: double, y: double): double;
end Math;
```

**What `Min`, `Max`, and `Clamp` compute.** Ordering throughout is `T`'s `CompareTo` ([§2.8](#sec-icomparablet)). `Min(a, b)` returns `a` when `a.CompareTo(b) <= 0` and `b` otherwise; `Max(a, b)` returns `a` when `a.CompareTo(b) >= 0` and `b` otherwise — so a tie returns **`a`** in both, which is what fixes *which object* the call yields at a class or interface substitution, where the two arguments are distinguishable. `Clamp(value, low, high)` returns `low` when `value` compares less than `low`, `high` when it compares greater than `high`, and `value` otherwise.

**Ownership at class-typed substitutions.** `Min`, `Max`, and `Clamp` return one of their arguments. Under the bare-type-parameter rule (language spec [§28.3](Bezel_Language_Spec.md#sec-bare-type-parameter-returns)), at a class or interface substitution of `T` the call produces a **borrow** whose sources are the arguments: the caller takes no disposal obligation, and the result is usable only while every argument's source is live. At value and runtime-managed substitutions (the common numeric cases; `string`) no ownership attaches.

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
  public static method ToString(value: long): string;
  public static method ToString(value: float): string;
  public static method ToString(value: double): string;
  public static method ToString(value: decimal): string;
  public static method ToString(value: bool): string;
  public static method ToString(value: int, toBase: int): string;              // bases 2, 8, 10, 16; ArgumentException otherwise
  public static method ToString(value: long, toBase: int): string;
end Convert;
```

The `To*` methods raise `FormatException` when the string is not a well-formed representation of the target type and `OverflowException` when the value does not fit — the same contract as the §9 per-type `Parse` family these conversions mirror; `ToBoolean` accepts exactly `"true"`/`"false"`, case-insensitively (`Boolean.Parse`'s contract, [§9.4](#sec-boolean)). `Try` forms return `false` in exactly the cases the throwing forms would raise; the `out` parameter receives the default value.

Each one-argument `ToString` returns the text the value it receives renders as under its own no-format `ToString()` ([§9](#sec-primitive-type-members); [§2.18](#sec-iformattable) for the round-trip and exact forms and the non-finite spellings), and `ToString(value, toBase)` returns the text `value.ToString(toBase)` returns ([§9.1](#sec-integer-types)): `Convert.ToString(true)` is `"True"`, `Convert.ToString(0.1f)` is `"0.1"` and `Convert.ToString(1.0 / 0.0)` is `"Infinity"`. An argument of any other type is resolved against these parameter types by overload resolution (language spec [§22.6](Bezel_Language_Spec.md#sec-overloading)), and where that selects an overload the text is the converted value's. A `char` and the integer types other than `int` and `long` each convert to two or more of these parameter types and match none exactly, so the call is ambiguous and a compile error (§22.6); the remedy is an explicit cast to one parameter type, or the value's own `ToString()`, which renders it in its own type.

<a id="sec-environment"></a>
### 2.4 `Environment`

Process- and OS-level information.

> ⚠ **Spec-ahead-of-implementation ([#5579](https://github.com/s8lap/bezel-lang/issues/5579)):** `UserName` and `CommandLineArgs` are not implemented: no sema or codegen arm names them.

```bezel
public static class Environment
  public static readonly property NewLine: string;
  public static readonly property MachineName: string;
  public static readonly property UserName: string;
  public static readonly property OSVersion: string;
  public static readonly property ProcessorCount: int;
  public static readonly property CommandLineArgs: IReadOnlyList<string>;   // read-only view of cached process-lived storage (§4.1); dedicated view object, not an Array — §2.13's downcast rule (#1805)

  public static method Get(name: string): string?;    // null when the variable is absent
  public static method GetInt(name: string): int?;    // convenience tier: null when the variable is
  public static method GetBool(name: string): bool?;  //   absent OR malformed — deliberately conflated
                                                      //   (recorded): callers that must distinguish keep the
                                                      //   two-step idiom (Get + TryParse).
                                                      //   GetBool accepts exactly Boolean.Parse's forms
                                                      //   ("true"/"false", case-insensitive); "1"/"yes" are null.
  public static method Set(name: string, value: string?);
      // value = null removes the variable; an empty name, or one containing "=",
      //   raises ArgumentException
  public static method Variables(): owned Dictionary<string, string>;

  public static method Exit(exitCode: int);        // never returns; unwinds no frame; inside §22.13's exit pass
                                                   //   it ends the pass, exitCode winning (spec §33.3)
end Environment;
```

`NewLine` is the host platform's line terminator: the one character U+000A on Unix-family platforms, the two characters U+000D U+000A on Windows. Its value is fixed for the process and does not vary with the destination written to; it is the terminator `WriteLine` appends ([§2.1](#sec-console), [§7.4](#sec-textreader-textwriter-directory-path)) and the one `StringBuilder.AppendLine` appends ([§3.1](#sec-stringbuilder)).

`Variables()` returns a freshly-allocated `owned Dictionary<string, string>` (per the §1 convention for fresh-instance returns).

<a id="sec-datetime-timespan-dateonly-timeonly"></a>
### 2.5 `DateTime`, `TimeSpan`, `DateOnly`, `TimeOnly`

Value types for representing points in time and durations. All are records.

```bezel
public record DateTime: IComparable<DateTime>, IEquatable<DateTime>
  // (#1357): ordering + construction. CompareTo = tick order (Kind not
  // normalized, matching .NET). Ordering is CompareTo only: DateTime declares no
  // <,>,<=,>= overloads, and records auto-generate equality but never ordering (spec §9.2).
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
  public readonly property Date: DateOnly;        // the date components
  public readonly property TimeOfDay: TimeOnly;   // the time-of-day components

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
  public static method ParseOrNull(s: string): DateTime?;   // (§1 *OrNull tier): null exactly
                                                            //   where Parse(s) raises; otherwise the
                                                            //   parsed value

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
  // (#1357): CompareTo = tick order. Ordering is CompareTo only — no
  //   <,>,<=,>= overloads are declared (spec §9.2, §25.2).
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

  public static readonly property Zero: TimeSpan;          // FromTicks(0)
  public static readonly property MinValue: TimeSpan;      // FromTicks(long.MinValue)
  public static readonly property MaxValue: TimeSpan;      // FromTicks(long.MaxValue)

  public method ToString(): string;                // constant ("c") form (§2.18)
  public method ToString(format: string): string;  // format vocabulary: §2.18

  public static method operator +(a: TimeSpan, b: TimeSpan): TimeSpan;
  public static method operator -(a: TimeSpan, b: TimeSpan): TimeSpan;
  public static method operator *(a: TimeSpan, factor: double): TimeSpan;
                                     // computes over Ticks in double; OverflowException on a
                                     //   product outside the tick range or a NaN operand
                                     //   (pinned)
  public static method operator /(a: TimeSpan, divisor: double): TimeSpan;
                                     // same computation model and OverflowException rule
                                     //   as *; DivideByZeroException on
                                     //   divisor = 0.0
  public static method operator -(t: TimeSpan): TimeSpan;
                                     // unary negation; OverflowException on MinValue
end TimeSpan;
```

`DateOnly` and `TimeOnly` (#1357):

```bezel
public record DateOnly: IComparable<DateOnly>, IEquatable<DateOnly>
  public constructor(year: int, month: int, day: int);   // ArgumentOutOfRangeException on invalid
  public readonly property Year: int;
  public readonly property Month: int;
  public readonly property Day: int;
  public method CompareTo(other: DateOnly): int;          // chronological; ordering is CompareTo only
  public method ToDateTime(time: TimeOnly): DateTime;     // Kind = Unspecified
  public static method Parse(s: string): DateOnly;        // FormatException on invalid
  public static method TryParse(s: string, out result: DateOnly): bool;
  public static method ParseOrNull(s: string): DateOnly?; // null exactly where Parse(s)
                                                          //   raises; otherwise the parsed value
  public method ToString(): string;                       // "yyyy-MM-dd"
  public method ToString(format: string): string;         // format vocabulary: §2.18
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
  public static method ParseOrNull(s: string): TimeOnly?; // null exactly where Parse(s)
                                                          //   raises; otherwise the parsed value
  public method ToString(): string;                       // "HH:mm:ss"
  public method ToString(format: string): string;         // format vocabulary: §2.18
end TimeOnly;
```

`DateTime.Date: DateOnly` and `DateTime.TimeOfDay: TimeOnly` accessors decompose a `DateTime`.

**The tick, and the `From`/`Total` pairs.** A **tick** is 100 nanoseconds — 10,000,000 to the second, 864,000,000,000 to the day. It is the unit of `Ticks` on both types and the resolution of every value either can hold; the `c` format's `.fffffff` tail ([§2.18](#sec-iformattable)) is that unit spelled out. `TimeSpan.From<Unit>(n)` is the span of `n` of that unit — `FromHours(1.5)` is ninety minutes — computed over ticks in `double` and truncated toward zero to a whole number of ticks, raising `OverflowException` when the result falls outside the tick range or the argument is `NaN` (the `*` operator's contract above). `Total<Unit>` is the whole span measured in that unit, fractional part included, so `Total<Unit>` of `From<Unit>(n)` is `n` up to that truncation; the integer components (`Days`, `Hours`, `Minutes`, `Seconds`, `Milliseconds`) are the span's decomposition, each within its stated range and carrying the span's sign. `(TimeSpan.FromDays(1) - TimeSpan.FromHours(1)).TotalHours` is therefore `23.0`, and that span's `Hours` is `23`.

The `ArgumentOutOfRangeException` these constructors raise is declared here, deriving from `ArgumentException` so that an `ArgumentException` catch arm covers the whole argument-validation family (language spec [§31.2.1](Bezel_Language_Spec.md#sec-arm-patterns)); it lives in the auto-imported `System` alongside its producers:

```bezel
public class ArgumentOutOfRangeException: ArgumentException
  public constructor(message: string);
  public constructor(message: string, paramName: string);
end ArgumentOutOfRangeException;
```

The no-argument `ToString()` renders a `DateTime` in the round-trippable ISO 8601 form `yyyy-MM-ddTHH:mm:ss` (e.g. `2024-03-15T00:00:00`); it is the value also used when a `DateTime` is interpolated into a string with no format specifier. `ToString(format)` selects an explicit format.

**Parse and range contracts.** `DateTime.Parse` accepts exactly the type's own rendered forms — the round-trip form `yyyy-MM-ddTHH:mm:ss`, optionally extended with a fractional-seconds tail `.fff`, and the date-only form `yyyy-MM-dd` (time components zero); the result's `Kind` is `Unspecified` (the rendered forms carry no zone), and any other input raises `FormatException`, the `DateOnly.Parse` stance. `TimeOnly.Parse` accepts `HH:mm:ss` with the same optional `.fff` tail. Every `TryParse` returns `false`, and every `ParseOrNull` returns `null` (the §1 `*OrNull` tier), exactly where its `Parse` would raise. The `Add*` family, `Add`, `Subtract(span: TimeSpan)`, and `DateTime.FromTicks` raise `OverflowException` when the result falls outside `MinValue..MaxValue`: the condition is a data-dependent overflow of the tick domain — language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)'s `OverflowException` class, the fault `TimeSpan`'s arithmetic above already raises — a band-4 catchable fault, not a band-1 argument defect; the constructors' component validation keeps `ArgumentOutOfRangeException`. The `Add*` family is complete over the units, one member per calendar and clock unit from `AddYears` to `AddMilliseconds`, so an offset is spelled the same way whatever its unit; a member of it is not removed for low use. `ToUnixTimeSeconds` converts a `Kind = Local` value via `ToUtc()` first and treats `Unspecified` as already UTC (deterministic — no hidden machine-zone read); `FromUnixTimeSeconds` produces `Kind = Utc`.

<a id="sec-guid"></a>
### 2.6 `Guid`

A 128-bit globally unique identifier.

```bezel
public record Guid: IComparable<Guid>, IEquatable<Guid>
  // (#1357): CompareTo = lexicographic over the 16 canonical big-endian
  // bytes (stated explicitly — .NET's field-group order is a known trap);
  // ordering is CompareTo only — no <,>,<=,>= overloads are declared.
  public method CompareTo(other: Guid): int;
  public static method FromByteArray(bytes: Array<byte>): Guid;   // borrows; raises ArgumentException unless Count = 16
  public static readonly property Empty: Guid;

  public static method NewGuid(): Guid;
      // pin: a version-4 (random) UUID — the version and variant bits are
      //   fixed (0100 / 10), the remaining 122 bits drawn from the runtime's
      //   entropy source (the §1 replay-intercept point declared for this
      //   section). Uniqueness is probabilistic, not checked: no dedup, no
      //   cross-process coordination. The entropy source is NOT specified as
      //   cryptographically strong — §2.7's recorded posture: cryptographic
      //   randomness belongs to the future System.Security.Cryptography module —
      //   so NewGuid values must not serve as secrets (session tokens,
      //   capability strings).
  public static method Parse(s: string): Guid;      // accepts the "D" and "N" renderings (§2.18); FormatException otherwise
  public static method TryParse(s: string, out result: Guid): bool;
  public static method ParseOrNull(s: string): Guid?;   // (§1 *OrNull tier): null exactly where
                                                        //   Parse(s) raises; otherwise the parsed value

  public method ToString(): string;          // canonical form
  public method ToString(format: string): string;   // "N", "D" — the §2.18 vocabulary; any other specifier raises FormatException
  public method ToByteArray(): owned Array<byte>;
end Guid;
```

`Guid` declares no constructor, so `new Guid(...)` is a compile error at any argument list (language spec [§17.7](Bezel_Language_Spec.md#sec-the-new-operator)'s closed-constructor-surface rule); construct via `Empty`, `NewGuid()`, `FromByteArray`, `Parse`/`TryParse`/`ParseOrNull`, or `default(Guid)` (`Empty` is the all-zero UUID — the same value `default(Guid)` produces).

<a id="sec-random"></a>
### 2.7 `Random`

Pseudorandom number generation. Seeded; a default-constructed `Random` uses a system-time-based seed.

`Random` is a class with a no-op `Dispose` (its only state is its internal seed; there is nothing to release). Using `using var` on a `Random` is permitted but is purely cosmetic.

```bezel
public class Random
  public constructor();
  public constructor(seed: long);

  public method Next(): int;                    // 0..int.MaxValue
  public method Next(maxValue: int): int;       // 0..<maxValue; maxValue < 0: ArgumentOutOfRangeException
  public method Next(minValue: int, maxValue: int): int;   // minValue > maxValue: ArgumentOutOfRangeException
  public method NextInt64(): long;
  public method NextDouble(): double;           // [0.0, 1.0)
  public method NextBytes(buffer: Array<byte>);
  public method NextBoolean(): bool;
end Random;
```

**Seeded generation is deterministic.** A `Random` constructed with an explicit `seed` reads nothing outside the process: the values it produces are a function of that seed and of the sequence of calls made on the instance, so two instances constructed with the same seed and driven with the same calls produce the same values. The parameterless constructor is the one [§1](#sec-conventions) designates a replay-intercept point. The generating algorithm is **not** specified: the sequence a seed names is reproducible within one implementation and is not guaranteed across implementations or revisions, so a program that must reproduce an exact sequence carries its own generator.

For cryptographic randomness, see `System.Security.Cryptography` (a future module).

**Sendability** ([§1](#sec-sendability-statements); language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)). `Random` is **sendable**, on the sendable form's copy-state ground: its only state is its seed and position, every member reads and writes that state by value, and no member retains a borrow past its call (`NextBytes`'s `buffer` is call-bounded) or exposes an address of its storage. A moved instance continues its sequence in the receiving domain; the replay-intercept point is the parameterless constructor, which a move does not touch.

<a id="sec-icomparablet"></a>
### 2.8 `IComparable<T>`

```bezel
public interface IComparable<T>
  method CompareTo(other: T): int;
end IComparable;
```

`a.CompareTo(b)` returns a negative number if `a < b`, zero if equal, a positive number if `a > b`.

The stdlib's primitives and `string` conform to `IComparable<T>`/`IEquatable<T>` at their own type only (`Int32: IComparable<int>`, `String: IComparable<string>`, …); both interfaces are invariant (language spec [§12.9](Bezel_Language_Spec.md#sec-variance)), so no `IComparable<Object>`/`IEquatable<Object>` view of them exists — object-keyed equality is `Object.Equals` (language spec [§8.3](Bezel_Language_Spec.md#sec-equals-and-gethashcode)).

<a id="sec-iequatablet"></a>
### 2.9 `IEquatable<T>`

```bezel
public interface IEquatable<T>
  method Equals(other: T): bool;
end IEquatable;
```

A typed alternative to `Object.Equals(Object?)`. Records implement this implicitly; classes may implement it for type-safe equality without boxing. A class's `Equals(other: T)` is its `Object` slot's second spelling (spec [§8.3](Bezel_Language_Spec.md#sec-equals-and-gethashcode), [§12.2](Bezel_Language_Spec.md#sec-hiding-is-not-allowed)): one virtual member, declared at `T` or `T?`, reached by `a.Equals(b)` and `a = b` (spec §8.4), by a `Dictionary<T, V>` or `Set<T>` constructed without a comparer (a `T`-typed use, spec §9.2 *Through a type parameter*, hashed by `GetHashCode()`), by a deriving class that inherits or overrides it, and through an `Object`-typed receiver or argument, where a non-`T` or `null` argument compares unequal — as `string`'s does ([§9.6](#sec-string)). Such a class overrides `GetHashCode()` to agree with it (spec §8.3's compiler-enforced pairing) and declares no `override Equals(Object?)` beside it. As for `IComparable<T>` ([§2.8](#sec-icomparablet)), the stdlib's primitives and `string` conform at their own type only and the interface is invariant — no `IEquatable<Object>` view of them exists.

<a id="sec-idisposable-and-iasyncdisposable"></a>
### 2.10 `IDisposable` and `IAsyncDisposable`

```bezel
public interface IDisposable
  method Dispose();
end IDisposable;

public interface IAsyncDisposable: IDisposable
  method DisposeAsync(): Task;
end IAsyncDisposable;
```

Every class implicitly implements `IDisposable` (language spec [§28.2](Bezel_Language_Spec.md#sec-the-idisposable-contract)); user code does not implement it directly — a class or record declaring a member named `Dispose`, at any parameter list, is a compile error at that declaration (language spec [§28.2](Bezel_Language_Spec.md#sec-the-idisposable-contract)). The interface is declared here so that generic constraints (`where T: IDisposable`) and explicit interface variables (`var d: IDisposable := someInstance;`) refer to a named declaration in the standard library rather than a language built-in.

`IAsyncDisposable` is implemented explicitly by classes that need asynchronous cleanup (language spec [§28.12](Bezel_Language_Spec.md#sec-asynchronous-disposal)). It extends `IDisposable`: the synchronous `Dispose()` every class has (language spec [§28.2](Bezel_Language_Spec.md#sec-the-idisposable-contract)) is the inherited member's implementation, a value held at `IAsyncDisposable` exposes both, and the constraint paragraph below follows from the extension. Implementations declare a method matching `DisposeAsync(): Task` and add `async` to the implementation; `async` is an implementation-side modifier and never appears on the interface declaration. A class that implements `IAsyncDisposable` should also declare a `destructor` for the synchronous fallback path, at the level that declares `DisposeAsync`: a destructor the class only inherits is the base's cleanup and does not stand in (language spec [§28.12](Bezel_Language_Spec.md#sec-asynchronous-disposal)). A direct `DisposeAsync()` call takes language spec [§28.6](Bezel_Language_Spec.md#sec-who-may-dispose)'s receiver rule, as `Dispose()` does: only the owner calls it, and `base.DisposeAsync()` inside a `DisposeAsync` override is the one admitted `this`-rooted call.

The constraint `where T: IDisposable` admits every class (every class implicitly implements `IDisposable`), no records (records cannot implement `IDisposable` per language spec [§9.7](Bezel_Language_Spec.md#sec-records-and-disposal)), and the interfaces that extend `IDisposable` directly or transitively — including `IAsyncDisposable`, `IEnumerator<T>`, `IAsyncEnumerator<T>`, and user-defined interfaces that list `IDisposable` in their extension clause. See language spec [§14.2](Bezel_Language_Spec.md#sec-constraints) for the general conformance rule.

Both interfaces are auto-imported.

For cleanup that must run only on the *failure* path — the errdefer idiom, an undo dismissed on success — see `ScopeGuard` ([§2.21](#sec-scopeguard)).

<a id="sec-subscription"></a>
### 2.11 `Subscription`

```bezel
public class Subscription
  // No public constructor; obtained from a registration call.
  destructor;       // removes the registration
end Subscription;
```

`Subscription` declares no constructor, so `new Subscription(...)` is a compile error at any argument list (language spec [§17.7](Bezel_Language_Spec.md#sec-the-new-operator)'s closed-constructor-surface rule); an instance is obtained only from a registration call.

`Subscription` is returned from `IEvent<T>.Subscribe` ([§6.5](#sec-events-ievent-eventsource)), from `IAsyncEvent<T>.Subscribe` ([§6.5](#sec-events-ievent-eventsource)), from `CancellationToken.Register` ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)), and from analogous register-and-unregister APIs. Methods returning a `Subscription` annotate the return type as `owned Subscription` so that the caller's leak-prevention obligation is explicit.

`Dispose()` is idempotent (per the language-wide rule, language spec [§28.2](Bezel_Language_Spec.md#sec-the-idisposable-contract)): the first call removes the registration; subsequent calls are no-ops. This makes it safe to dispose a subscription whether or not the underlying source has already removed it for other reasons (the source was disposed, the event was unhooked elsewhere, the cancellation already fired).

`Subscription` lives in `System` (not `System.Threading`) and is auto-imported: every register-and-unregister API across the standard library returns it, including `CancellationToken.Register` whose declaring class is itself auto-imported, so placing it beside those APIs — rather than behind an explicit `import System.Threading;` — matches the type's actual usage. See language spec [§2.3](Bezel_Language_Spec.md#sec-always-available-names) for the auto-import list.

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
      // Pseudo-signature — compiler intrinsic: TFrom stands for the argument's
      //   inferred type, not a declared type parameter (not legal user Bezel as
      //   displayed). The primitive-numeric gate keeps every admissible
      //   instantiation value-typed, so the signature sits outside spec §28.3's
      //   bare-type-parameter rule.
end BitConverter;
```

`FloatToInt32Bits` and `DoubleToInt64Bits` return the raw IEEE 754 bit pattern of the floating-point value as a signed integer of matching width. The integer return types are `int` (32 bits) and `long` (64 bits) — Bezel's standard width-named primitives, equivalent to the boxed record types `Int32` and `Int64` ([§9.1](#sec-integer-types)) that appear in the method names. `Int32BitsToFloat` and `Int64BitsToDouble` perform the reverse — interpreting the integer's bit pattern as a float.

These are pure bit-reinterpretation operations: the bit patterns are preserved exactly. For `NaN` values, the specific bit pattern (signaling vs quiet, payload bits) is preserved, which is what bit-level tools (serialization, hashing, exact comparison) need.

The method names use the sized record-type names (`Int32`, `Int64`) consistently with the byte-buffer accessors of [§4.14](#sec-owned-buffers-and-spans). The `Float`/`Double` portion uses the primitive keyword names (Bezel has no separate `Single` record type; `float` is the only 32-bit floating-point type). No unsigned *named* forms (e.g. `FloatToUInt32Bits`) are provided — the bit-pattern integer is treated as opaque by callers most of the time, and the signed form interoperates with the standard bitwise operators identically; when a same-width signedness view is genuinely needed, the generic `Reinterpret<TTo>` below supplies it.

`Reinterpret<TTo>(value)` is the general same-width bit-reinterpretation operation. The single type argument names the *target* type; the source type is inferred from the argument, so the call site writes only the target — `BitConverter.Reinterpret<int>(someUint)`. The source and target must be primitive numeric types of equal bit width (the 8-, 16-, 32-, and 64-bit groups — e.g. `int`, `uint`, and `float` are mutually reinterpretable at 32 bits; `long`, `ulong`, and `double` at 64 bits); a target of a different width is a compile error. `Reinterpret` copies the value's bits unchanged — it never raises and never alters the bit pattern. The four named `…Bits…` methods above are the float/double special cases of `Reinterpret`, retained for readability and continuity. `Reinterpret` is the bit-level counterpart to the *checked* same-width signed↔unsigned cast of language spec [§5.2](Bezel_Language_Spec.md#sec-explicit-conversions): the cast performs a *value* conversion that raises `OverflowException` if the value does not fit, while `Reinterpret` performs a *bit* reinterpretation that always succeeds. Convert a value with the cast; view the same bits under a different type with `Reinterpret`.

<a id="sec-the-type-class"></a>
### 2.13 The `Type` class

`Type` is the runtime type-identity token. A `Type` value is obtained from the `typeof(T)` operator (language spec [§17.10](Bezel_Language_Spec.md#sec-the-typeof-operator)) for a statically named type, or from `value.GetType()` (language spec [§8.2](Bezel_Language_Spec.md#sec-gettype)) for the runtime type of a value. The runtime guarantees exactly one `Type` instance per distinct type, so reference equality and `=` coincide; `Type` is a valid `Dictionary` key. `Type` is **runtime-managed** (language spec [§28.9](Bezel_Language_Spec.md#sec-runtime-managed-types)): a `Type` value carries no ownership classification, a `Type` has no `Dispose()` member (language spec [§8.7](Bezel_Language_Spec.md#sec-dispose-is-not-on-object)), `owned Type` is inadmissible in every position, and a `Type`-returning member — instance or `static` — needs no ownership marker. `Type` is **sendable** ([§1](#sec-sendability-statements)): it crosses a domain boundary as a shared reference, as `string` does, because the one interned instance is immutable and every domain already reaches it through `typeof` (language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)); `Array<Type>`, `Dictionary<Type, V>` at a sendable `V`, a record with a `Type` field, and a transfer-closed class with a `Type` field are sendable on the same terms.

**Facts** ([§1](#sec-statics-and-sharing-facts)): `Type` — **immutable** (deeply immutable: the one interned instance per type is fixed for the life of the process, no member writes it, and the views it materializes are an implementation tolerance — language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)'s implementation note — not observable state), **self-distributing** (every domain already reaches the interned instance through `typeof` and `GetType()` — language spec [§28.9](Bezel_Language_Spec.md#sec-runtime-managed-types) — so a spec-§28.13 handle around one would add a second distribution mechanism; and `owned Type` is inadmissible in every position, so no `Shared<Type>` could be constructed). *Displacement-unbounded* is not granted: the instance is never reclaimed, so an overwrite strands nothing. Consequences, per §1's discipline: `Type` joins language spec [§22.12](Bezel_Language_Spec.md#sec-static-members) rule 4's warning-suppression set — a `static readonly` field or auto-property of type `Type` is warning-free — and neither rule 2's form-(i) set (not synchronized: a mutable `static` of `Type` stays rejected by rule 2's closing sentence) nor [§28.13](Bezel_Language_Spec.md#sec-shared-ownership)'s `Shared`-payload set (self-distributing). Provenance: the v0.328 ruling, changelog v0.328.

> ⚠ **Spec-ahead-of-implementation ([#3819](https://github.com/s8lap/bezel-lang/issues/3819)):** the lattice members below — `BaseType`, `IsAssignableFrom`, the kind predicates (`IsClass`, `IsEnum`, and their siblings), `EnumNames`, `EnumValues`, and `FullName` — have no compiler support: no sema arm and no codegen arm carries any of their names, and the runtime `Type` token itself is not yet emitted (`typeof(T)` is rejected in codegen, #487; `GetType()` in sema, #511), so no member of this class is reachable from a running program today. The `Name`/`FullName` rendering rules are exercised only through language spec §8.5's default `ToString`. The surface below is the normative contract the implementation will meet.

```bezel
public sealed class Type
  public readonly property Name: string;            // unqualified: "BankAccount"
  public readonly property FullName: string;        // qualified: "BankingApp.BankAccount"

  // Kind predicates. Exactly one of IsClass, IsInterface, IsRecord, IsEnum,
  // IsChoice, IsPrimitive, IsInlineArray, IsFunctionType is true for any Type.
  public readonly property IsClass: bool;
  public readonly property IsInterface: bool;
  public readonly property IsRecord: bool;
  public readonly property IsEnum: bool;
  public readonly property IsChoice: bool;          // any §35 choice, at every
                                                    //   §35.4 classification
  public readonly property IsPrimitive: bool;       // int, bool, char, float, ...
  public readonly property IsInlineArray: bool;     // [N]T, [N owned]T?, ...
  public readonly property IsFunctionType: bool;    // (A, B) -> R
  public readonly property IsValueType: bool;       // = IsPrimitive or IsEnum
                                                    //   or IsRecord or IsChoice
                                                    //   or IsInlineArray

  // Lattice queries.
  public readonly property BaseType: Type?;         // direct superclass; null for
                                                    //   Object, interfaces, value
                                                    //   types, function types
  public method IsAssignableFrom(other: Type): bool;// true if a value of type
                                                    //   `other` is assignable to a
                                                    //   location of this type

  // Enum metadata. Valid only when IsEnum; on any other Type these raise
  // InvalidOperationException.
  public readonly property EnumNames: IReadOnlyList<string>;  // member names, declaration
                                                              //   order; read-only view (§4.1)
  public readonly property EnumValues: IReadOnlyList<long>;   // member values, same order
                                                      //   (widened to long; ulong-underlying
                                                      //   enums bit-reinterpret — see below)
end Type;
```

**Kind predicates.** For any `Type`, exactly one of `IsClass`, `IsInterface`, `IsRecord`, `IsEnum`, `IsChoice`, `IsPrimitive`, `IsInlineArray`, `IsFunctionType` holds. These map the type kinds of language spec [§3.1](Bezel_Language_Spec.md#sec-type-kinds) onto a runtime predicate surface, with two deliberate flattenings: `string` and the heap array types (`Array<T>`, `OwningArray<T>`) report `IsClass` (they are reference types — language spec [§3.1](Bezel_Language_Spec.md#sec-type-kinds) lists `string` and heap arrays as reference types reaching `Object` directly, and treating them as classes here is consistent with how the language and standard library describe them elsewhere); the unit type `()` reports `IsValueType` with none of the eight primary predicates true (it is a degenerate value type and the rare `typeof(())` is not expected in practice, but the surface does not claim a primary kind for it). **`IsChoice` is the predicate for language spec [§35](Bezel_Language_Spec.md#sec-choices) choices**: it holds for a choice type whatever its [§35.4](Bezel_Language_Spec.md#sec-choice-classification) classification — value-like, borrow-bearing, or owning — because the classification governs copy/move/dispose semantics and borrow obligations, never type *kind*, exactly as `IsInlineArray` holds alike for `[N]T` and for the move-only `[N owned]T`. `IsValueType` is a derived convenience equal to `IsPrimitive or IsEnum or IsRecord or IsChoice or IsInlineArray` (and additionally true for the unit type); it does not add a primary category. Its choice disjunct is required by language spec [§3.2](Bezel_Language_Spec.md#sec-reference-vs-value-types), which classifies every choice — owning ones included, as move-only value types ([§15.3](Bezel_Language_Spec.md#sec-move-only-value-types)) — as a value type. The "exactly one of eight" guarantee therefore holds for every type a program can name with `typeof` or obtain from `GetType` except the unit type, which no realistic program reifies.

**`Name` and `FullName` rendering (normative, #1812).** Both properties render the type's canonical source-level type expression (except FullName's enclosing generic segment under rule 1, which has no source spelling); the produced strings are exact — they are what diagnostics print and registries key on. `Name` renders named components unqualified; `FullName` renders every named component qualified by its declaring module path and, for a nested type, its enclosing-type path (`"BankingApp.BankAccount"`, `"BankingApp.Ledger.Entry"`); the distinction is carried recursively. Rules:

1. **A named non-generic type** renders its declared identifier (Name) / its module-qualified path (FullName). The root and this class, both declared in `System` (language spec [§8.1](Bezel_Language_Spec.md#sec-definition), [§2.3](Bezel_Language_Spec.md#sec-always-available-names)), render `"Object"`/`"System.Object"` and `"Type"`/`"System.Type"`, and as a type argument under FullName `"System.Object"` (`"System.Collections.Generic.Array<System.Object>"`).
   - **A nested type** (language spec [§11.6](Bezel_Language_Spec.md#sec-nested-types)): Name is the nested identifier alone (`"Inner"`); FullName is the declaring module, then every enclosing type outermost first, then the identifier (`"M.Outer.Inner"`).
   - **An enclosing generic type** in that path renders its identifier followed by `<`, one `_` per declared type parameter joined by `", "`, and `>`, because a nested type is one type whatever the enclosing instantiation and has no enclosing arguments to render (language spec [§11.6](Bezel_Language_Spec.md#sec-nested-types)): `"M.Tree<_>.Node<int>"`, `"M.Pair<_, _>.Entry"`. This form is a rendering, not a type expression.
   - The enclosing-type path is part of the type's identity (language spec [§2.8.3](Bezel_Language_Spec.md#sec-transitive-closure-and-type-identity) rule 3), so two unrelated nested types of one name render distinct FullNames and the same Name (the nested `Inner`s of the arity twins `Outer` and `Outer<T>` render `"M.Outer.Inner"` and `"M.Outer<_>.Inner"`).
2. **The keyword-aliased types** (language spec [§3.4](Bezel_Language_Spec.md#sec-keyword-aliases-for-built-in-types)) render as their lowercase keyword in *both* properties (`typeof(int).FullName = "int"`; `typeof(String)` renders `"string"`) — the keyword is the canonical name, a recorded exception to qualification.
3. **A generic instantiation** renders the head followed by `<`, its arguments joined by `", "`, and `>`, each argument rendered recursively in the same property's form (`"Dictionary<string, Array<int>>"`; FullName: `"System.Collections.Generic.Dictionary<string, System.Collections.Generic.Array<int>>"`) — no arity markers; a nullable argument renders with a trailing `?` (`"Array<int?>"`, a distinct `Type` from `"Array<int>"`).
4. **An inline array** renders its canonical type expression `"[N]T"` / `"[N owned]T?"` (language spec §15), the element recursing.
5. **A function type** renders its canonical type expression `"(A, B) -> R"` (parameters `", "`-joined, one space around `->`, modifiers as written, unit as `"()"`), components recursing — a function type has no module of its own, so Name and FullName differ only through the components.
6. **The unit type** renders `"()"`.

**Nullability is erased at reification**: no `Type` exists for a top-level nullable form — a `null` has no runtime type and `GetType` requires a narrowed receiver ([§8.2](Bezel_Language_Spec.md#sec-gettype)) — and `typeof(T)` at a substitution `U?` returns `typeof(U)`; `typeof(int?)` is a compile error ([§17.10](Bezel_Language_Spec.md#sec-the-typeof-operator)'s accepted forms). The language-spec side of this contract — substitution distinctness scoped up to top-level nullability, and the `U?` reification — is stated normatively in [§17.10](Bezel_Language_Spec.md#sec-the-typeof-operator)'s identity paragraph (as amended in the same revision, #1812). Nullability inside a composite's identity (rules 3–5) is part of that identity and renders.

**`BaseType`.** Returns the direct superclass of a class type. It is `null` for `Object` (the root has no base), for interface types (interfaces have no base *class*; conformance is queried with `IsAssignableFrom`), for all value types, and for function types. Walking the chain to the root is `t.BaseType`, then `that!.BaseType`, terminating at the `Object` `Type` whose `BaseType` is `null`. For most subtype questions `IsAssignableFrom` is the better tool; `BaseType` is the lower-level primitive, useful mainly for diagnostics that print an inheritance chain.

**`IsAssignableFrom`.** `a.IsAssignableFrom(b)` is the reflective form of the `is` operator (language spec [§12.7](Bezel_Language_Spec.md#sec-type-tests-is-is-not-as)): it returns whether a value whose runtime type is `b` may be assigned to a location of static type `a` — true when `a = b`, when `a` is a base class of `b`, or when `a` is an interface that `b` implements; interface conformance is therefore queried as `iface.IsAssignableFrom(t)`, and there is no second spelling for it. It operates on `Type` tokens with no value in hand, which is what distinguishes it from `is`/`as`.

**Enum metadata.** When `IsEnum`, `EnumNames` and `EnumValues` are parallel arrays (same length, same order — declaration order of the enum members) giving each member's source name and its integer value widened to `long` — except for an enum whose underlying type is `ulong` (spec §10.3 admits it), whose values are **bit-reinterpreted** into `long` (`BitConverter.Reinterpret` semantics, [§2.12](#sec-bitconverter): the bit pattern is preserved; callers of such enums reinterpret back to `ulong`), since values above `long.MaxValue` have no widening. They are the building blocks for enumerating all members for UI or validation — a help text, a validation list — and they read the same declared-identifier table the enum's own compiler-provided `E.NameOf`/`E.FromNameOrNull` ([§9.9](#sec-enum-compiler-provided-members)) and `nameof` (language spec [§17.9](Bezel_Language_Spec.md#sec-the-nameof-operator)) read, so a list built from `EnumNames` matches the parse surface exactly; a `serialized "<name>"` rename (language spec [§9.8](Bezel_Language_Spec.md#sec-field-name-customization)) changes the encoded form only and appears in none of them. On any non-enum `Type`, accessing either property raises `InvalidOperationException` (the property is meaningless, and a silent empty array would mask a programming error). The properties are **read-only views** over cached, process-lived storage (`IReadOnlyList<T>`, [§4.1](#sec-borrowing-collection-interfaces)), handed out as borrows (a property getter may not mint — spec §28.6) — the [§4.8](#sec-dictionaryk-v) `Keys`/`Values` shape, **including its backing**: each view is a dedicated view object owned by the `Type` instance, whose class is not nameable in user code and is in particular not `Array<T>`. There is no mutation surface, declared or recoverable: the view type has no mutating members, so the hazard of mutating process-wide metadata through the borrow is closed at compile time, and a type-test downcast (spec [§12.7](Bezel_Language_Spec.md#sec-type-tests-is-is-not-as)) cannot reach one — `typeof(E).EnumNames as Array<string>` is `null` (#1805). The same backing rule governs [§2.4](#sec-environment)'s `CommandLineArgs`. Each view is materialized once per `Type` instance, and the runtime's materialization and reads tolerate concurrent access from several domains (language spec §30.14's implementation note: a `Type` crosses as a shared reference). `IReadOnlyList` lives in `System.Collections.Generic`; touching the view's members needs no import (language spec [§2.8.3](Bezel_Language_Spec.md#sec-transitive-closure-and-type-identity) rule 2) — naming the interface in source does, as for [§2.4](#sec-environment)'s `Variables` result. Example:

```bezel
public method DescribeEnum(t: Type)
  if not t.IsEnum then return; end;
  var names := t.EnumNames;
  var values := t.EnumValues;
  for var i in 0..<names.Count do
    Console.WriteLine($"{names[i]} = {values[i]}");
  end;
end DescribeEnum;
```

**What `Type` deliberately does not provide.** There is no field, property, or method enumeration; no by-name member access; no attribute reading; no dynamic instantiation. `Type` is type identity plus lattice metadata, not member-access reflection. The reasoning is in rationale [§5.18](Bezel_Design_Rationale.md#sec-type-identity-not-reflection): a reflectively-accessed `owned` member would have no static call site to carry its ownership contract, and member reflection is the largest possible breach of the "no hidden magic" principle the language is built on. Programs that would reach for member reflection in other languages use compile-time generics, explicit registration instead of reflective discovery, and source-defined serialization in Bezel.

<a id="sec-serialization-vocabulary"></a>
### 2.14 Serialization vocabulary

Bezel's serialization model separates two concerns that runtime-reflection-based serializers conflate: *what a type's shape is* (the type's concern, statically known) and *how a format encodes primitives and structure* (the format's concern). This separation is the architectural keystone — it is what lets serialization work without reflection, and it is the one part that cannot be retrofitted later. The interface vocabulary is established here; the writing direction is usable immediately by hand, and the design is forward-compatible with the compiler-synthesized conformance planned for a later revision (rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection)).

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

**Why `IDeserializable` has no method in this version.** Serialization is an *instance* operation (an existing value writes itself); deserialization is a *static* one (a format reader produces a new value — there is no instance yet). A static entry point could not be expressed as an interface method until static interface members existed (language spec [§11.8](Bezel_Language_Spec.md#sec-static-interface-members)); this by-name protocol predates them. Rather than block deserialization on that keystone, the protocol recognizes the deserialization entry point **by name**, as the user-defined collection-literal protocol recognized `FromCollectionLiteral` before its promotion to the [§2.19](#sec-static-abstractions) target pair (language spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol)): a type that declares `: IDeserializable` and provides a static method of the shape

```bezel
public static method Deserialize(d: IDeserializer): T;          // value-type
                                                                //   target (record,
                                                                //   choice,
                                                                //   enum): no `owned`
public static method Deserialize(d: IDeserializer): owned T;     // class target:
                                                                //   `owned` return
public static method Deserialize(d: IDeserializer): owned Shared<C>;  // co-owned entry:
                                                                //   the class is itself
                                                                //   a shared node
// T = the declaring type; C = the declaring shared-node class; both written concretely.
```

is deserializable. The recognized factory's return type is the declaring type itself, written concretely. The `owned` modifier appears exactly when the declaring type is a class (a class instance is an owned heap value transferred to the caller — language spec [§28.8](Bezel_Language_Spec.md#sec-ownership-transfer)); for a value-type target (a record, enum, or value-like choice — the synthesis-eligible common case, language spec [§9.8](Bezel_Language_Spec.md#sec-choice-serialization) admitting choices on the same terms as records and enums) the return is a plain by-value `T` with no `owned` modifier, because `owned` on a record, enum, or choice return is a compile error (language spec [§28.1](Bezel_Language_Spec.md#sec-where-the-owned-modifier-can-appear)'s type requirement — the modifier admits class types, interface types, and bare type parameters, and is admitted-but-redundant on move-only value types). A class that is itself a *shared node* in its own serialized graph uses the `owned Shared<C>` form — it participates by identity and round-trips as a co-owned handle, and `Shared<C>` is a class so the `owned` modifier applies; the choice is locally decidable — a class that merely *holds* `Shared<T>`/`Weak<T>` fields returns `owned C`, a class that *is* the shared node returns `owned Shared<C>` (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis), rationale [§5.22](Bezel_Design_Rationale.md#sec-type-driven-serialization)). Bezel has no `Self` type (a self-type keyword is declined — spec §11.8; proposals `DECLINED.md`), so the return type is spelled with the concrete type name. `IDeserializable` is a *marker* whose presence says "this type provides the recognized `Deserialize` factory," in the same way `: IEquatable<T>` opts a record into synthesized equality without the user writing the comparison. The hand-written form today, for the record (value-type) case:

```bezel
public record Point(X: int, Y: int): ISerializable, IDeserializable
  public method Serialize(s: ISerializer)
    s.BeginObject(2);
    s.WriteFieldName("X"); s.WriteInt(X);
    s.WriteFieldName("Y"); s.WriteInt(Y);
    s.EndObject();
  end Serialize;

  public static method Deserialize(d: IDeserializer): Point
    _ := d.BeginObject();   // field count, or -1 for JSON; unused here
    var x := 0;  var y := 0;
    // field-name dispatch omitted for brevity; a real implementation
    // loops over ReadFieldName() and fills the locals
    d.EndObject();
    return new Point(x, y);
  end Deserialize;
end Point;
```

Promoting `Deserialize` to a static interface member is a recorded follow-up (#1507; rationale [§18](Bezel_Design_Rationale.md#sec-things-deferred)): it takes the type-parameter form of spec §11.8 — a self-type keyword is declined (proposals `DECLINED.md`) — with the member's name and signature unchanged and the conformance declaration becoming `: IDeserializable<T>`; the same discipline promoted `FromCollectionLiteral` ([§2.19](#sec-static-abstractions)).

**Synthesis and what remains.** A record or enum that declares `: ISerializable` and/or `: IDeserializable` and satisfies the synthesis-eligibility rule has the bodies **generated by the compiler** — the hand-written forms above are unnecessary for the common case. The rule, the recursive component check, and the generated-body semantics are defined in language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis); the design mirrors the existing auto-equality synthesis ([§9.2](Bezel_Language_Spec.md#sec-body-records)). The hand-written `Point` above becomes:

```bezel
public record Point(X: int, Y: int): ISerializable, IDeserializable;
// compiler synthesizes Serialize(s: ISerializer) and the recognized
// static Deserialize(d: IDeserializer): Point — no body needed
```

Hand-writing either member suppresses synthesis of that direction (exactly as a manual `Equals` suppresses auto-equality), so the explicit forms shown earlier remain valid and take precedence when present. A `serialized` clause customizes the synthesized field names without disabling synthesis:

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
  public property Id: int => id;
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
  public property Number: string => number;
end Invoice;
```

The synthesized `Deserialize` for a class returns `owned Invoice` (a class instance is an owned heap value transferred to the caller). It reconstructs the owning spanning tree (the `owned` edges) in owner-before-borrower order; each plain borrowing back-edge (here `AccountManager`) is supplied **as a constructor argument**, resolved against an identity table that already holds the target because the owning tree admits such an order. Because every reference field bound through the constructor is set before any constructor body runs, no constructed object ever has an unbound non-nullable reference — definite assignment (language spec [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields)) holds with no nullable relaxation. A `Shared<T>` participant is reconstructed once at its first occurrence and `Clone()`d at each alias; a `Weak<T>` back-edge is `Downgrade()`d from its reconstructed `Shared<T>`. Identity is preserved, never duplicated — Bezel refuses the silent-duplication failure mode of reference-unaware serializers. The one read-side check the field types cannot decide is that a `Weak<T>`'s target is strongly reached somewhere in the graph: a `Weak` reference to a never-strongly-reached target raises `SerializationException`.

A base class whose components may hold derived instances declares an explicit `polymorphic` registry as a class-body member:

```bezel
public abstract class Shape: ISerializable, IDeserializable
  polymorphic ("circle": Circle, "square": Square);
end Shape;

public class Circle(radius: double): Shape, ISerializable, IDeserializable
  public property Radius: double => radius;
end Circle;
```

The registry is a closed, explicitly written map from a wire discriminator to a permitted concrete type — **no reflective subtype discovery** (the design principle of rationale [§5.18](Bezel_Design_Rationale.md#sec-type-identity-not-reflection); this is what prevents a serialized discriminator from becoming an arbitrary-type construction primitive, the defect behind reflective-`$type` remote-code-execution vulnerabilities elsewhere). The synthesized serializer writes the concrete type's discriminator as a leading `$type` field; the synthesized deserializer reads it **first**, looks it up in the registry, and delegates to that type's synthesized `Deserialize`. An unregistered discriminator (read) or an unregistered run-time type (write) raises `SerializationException` — the closed set enforced on both directions. Polymorphic dispatch composes uniformly with the type-driven model: each registered type reconstructs under whatever shape its own field types describe.

A class whose serialized graph may contain **cycles** uses `Shared<T>` for the shared spine and a nullable `Weak<T>?` for each cycle-closing back-edge — both ordinary owned handles, so the cycle is constructed in ownership-valid Bezel and round-trips through the ownership checker:

```bezel
public class Node(label: string): ISerializable, IDeserializable
  public owned Next: Shared<Node>?;  // owned handle to the shared spine (identity
                                     //   participant); nullable to terminate the chain
  public owned Back: Weak<Node>?;    // back-edge: a weak observer that may close a cycle
  public property Label: string => label;
end Node;
```

A cycle is built by constructing the `Shared<Node>` spine, then wiring each `Weak<Node>?` back-edge with `Downgrade()` once its target exists — ordinary mutation of a nullable field, so definite assignment (language spec [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields)) is satisfied by the null initializer and no two-phase construction is required. The synthesized `Deserialize` reconstructs the spine and binds each `Weak` `$ref` by `Downgrade()`ing the already-reconstructed `Shared<Node>` its identity tag resolves to. A `Weak<T>` reference whose target is never strongly reached in the graph raises `SerializationException` (the design is rationale [§5.22](Bezel_Design_Rationale.md#sec-type-driven-serialization)).

This is the type-driven serialization model: a class declares `: ISerializable`/`: IDeserializable` and its serialized shape — tree, DAG, or cyclic — follows from its field types (`owned` → owning edge, `Shared<T>` → identity participant, `Weak<T>`/borrow → back-reference), with polymorphic edges resolved through an explicit class-body `polymorphic` registry. The concrete format implementations are in [§2.15](#sec-concrete-serialization-formats); the design record for the type-driven model is rationale [§5.22](Bezel_Design_Rationale.md#sec-type-driven-serialization).

<a id="sec-concrete-serialization-formats"></a>
### 2.15 Concrete serialization formats

The standard library provides two concrete implementations of the [§2.14](#sec-serialization-vocabulary) agent interfaces: a JSON pair (`JsonSerializer`/`JsonDeserializer`) and a compact binary pair (`BinarySerializer`/`BinaryDeserializer`). Both are ordinary classes implementing `ISerializer`/`IDeserializer`; they introduce no language feature. They live in `System.Serialization` and are *not* auto-imported (unlike the vocabulary interfaces) — a program that performs serialization names the format explicitly with `import System.Serialization`. The module's one-shot facades `Json` and `Binary` (below) are the whole-value front door over these agents.

Both serializers write to, and both deserializers read from, an `OwningBuffer<byte>`-backed stream. The serializer accumulates bytes and exposes the result; the deserializer is constructed over a `ReadOnlySpan<byte>` of the encoded form.

```bezel
public class JsonSerializer: ISerializer
  public constructor();
  public constructor(options: JsonOptions);
  // ISerializer surface (§2.14) implemented for JSON text.
  public method ToBytes(): owned OwningBuffer<byte>;   // UTF-8 encoded result
  public override method ToString(): string;          // convenience: UTF-8 → string
end JsonSerializer;

public class JsonDeserializer: IDeserializer
  public constructor(input: ReadOnlySpan<byte>);         // UTF-8 JSON
  public constructor(input: ReadOnlyArraySpan<byte>);    // (#1358): Array-world twin;
                                                          //   borrowed identically for the deserializer's lifetime
  public constructor(input: string);                  // convenience
  public constructor(input: ReadOnlySpan<byte>, options: JsonOptions);
  public constructor(input: ReadOnlyArraySpan<byte>, options: JsonOptions);  // options parity
  public constructor(input: string, options: JsonOptions);                   //   for every input form
  // IDeserializer surface (§2.14) implemented for JSON text.
end JsonDeserializer;

public record JsonOptions(
  Indented: bool,                 // pretty-print (serializer); ignored on read
  IndentWidth: int,               // spaces per level when Indented; 2 when no options are passed
  AllowTrailingCommas: bool,      // deserializer leniency; false when no options are passed
  IgnoreUnknownFields: bool,      // deserializer: skip input fields that match
                                  //   no declared component instead of raising;
                                  //   false (strict) when no options are passed
  MaxDepth: int                   // nesting guard; 64 when no options are passed
);
// All five components are required: a positional record admits no default values
// (spec A.3 PositionalField), so `new JsonOptions(...)` supplies every one. The
// values above are what the no-options constructors behave as.

public class BinarySerializer: ISerializer
  public constructor();
  // ISerializer surface (§2.14) implemented for the compact binary format.
  public method ToBytes(): owned OwningBuffer<byte>;
end BinarySerializer;

public class BinaryDeserializer: IDeserializer
  public constructor(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool := false);        // strict by default
  public constructor(input: ReadOnlyArraySpan<byte>, ignoreUnknownFields: bool := false);   // (#1358): Array-world twin;
                                                          //   borrowed identically for the deserializer's lifetime
  // IDeserializer surface (§2.14) implemented for the compact binary format.
end BinaryDeserializer;
```

**JSON mapping.** The `ISerializer` token stream maps to JSON as follows.

| Token | JSON form |
|---|---|
| `WriteInt`/`WriteUInt`; `ReadInt`/`ReadUInt` | A JSON number with no fraction and no exponent. `ReadInt` and `ReadUInt` read only such a number, so one with a fraction or an exponent (`5.0`, `1e3`, `5.9`) is a type mismatch and raises `SerializationException` located at the number, whatever its value; a number read by `ReadInt` whose value lies outside `long`'s range, or by `ReadUInt` outside `ulong`'s (`9223372036854775808` and `-1` respectively), raises `SerializationException` located at the number. An integer component narrower than the read checks its own range as language spec [§9.8](Bezel_Language_Spec.md#sec-synthesized-deserialize) states. |
| `WriteFloat`; `ReadFloat` | `WriteFloat` produces a JSON number, with non-finite values (`NaN`, `±Infinity`) raising `SerializationException` (JSON has no representation for them — a binary format is the correct choice when non-finite floats must round-trip). `ReadFloat` reads a JSON number of any length, its value rounded once to `double` as [§9.2](#sec-double-and-float) rounds `double.Parse`'s input; a number whose value, so rounded, is not finite (`1e999`, `-1e999`) raises `SerializationException` located at the number, so a document never reads back a non-finite value `WriteFloat` could not have written. A number that rounds to zero or to a subnormal is in range: `1e-400` reads as `0`, and `-0` as negative zero. A `float` component converts that `double` as language spec [§9.8](Bezel_Language_Spec.md#sec-synthesized-deserialize) states. |
| `WriteDecimal` | A JSON number written without precision loss. |
| `WriteBool` | `true`/`false`. |
| `WriteString` | A quoted, escaped JSON string. |
| `WriteChar` | A one-character JSON string. |
| `WriteNull`; `TryReadNull` | `null`. `TryReadNull()` consumes and returns `true` iff the next value is `null`. |
| `BeginObject`/`WriteFieldName`/`EndObject` | A JSON object. The `fieldCount` argument to `BeginObject` is advisory (JSON objects are not length-prefixed) and is ignored by `JsonSerializer`. On the read side `JsonDeserializer.BeginObject()` returns `-1` (JSON does not prefix counts), so consumers use `ReadFieldName()` driven by the object's textual extent. |
| `BeginArray`/`EndArray` | A JSON array; the `elementCount` argument is likewise advisory. On the read side `JsonDeserializer.BeginArray()` returns `-1`, so consumers use `HasMoreElements()`. |
| **Identity tokens** — the graph-identity operations of the synthesis (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis): first-occurrence tags for `Shared<T>` identity participants, `$ref` back-references for `Weak<T>` and borrowing fields) | An object carrying an identity tag is written with a reserved first field `"$id": n` (a positive JSON number, unique per stream); a back-reference is written as the one-field object `{"$ref": n}` in place of the value. On the read side these forms are recognized positionally: an object whose first field is `"$id"` registers the tag; an object whose only field is `"$ref"` resolves it. |

Field names beginning with `$` are reserved for this mapping (`$id`, `$ref`, `$type`, and the choice case discriminator `$case`, language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis)): a `serialized "<name>"` rename whose name begins with `$` is a compile error at the conformance declaration. The binary format's tag encoding is internal to it (§9.8's semantics; this table is the JSON wire form only).

**Binary mapping.** The compact binary format is length-prefixed and self-describing enough to be read back without the schema, but it is *not* a stable interchange format across Bezel versions — it is intended for same-version persistence and IPC, where compactness and speed matter and the producer and consumer share a build. Integers use a variable-length encoding; `BeginObject`/`BeginArray` write the count, so `BinaryDeserializer.BeginObject()` and `BeginArray()` return the actual element count and `HasMoreElements()` is answered from it. Non-finite floats round-trip exactly (the format stores IEEE 754 bit patterns via `BitConverter`, [§2.12](#sec-bitconverter)). Programs needing a documented, version-stable wire format use JSON or an explicit hand-written format; the binary format trades interchange stability for size and speed.

<a id="sec-one-shot-serialization-facades"></a>
**The one-shot facades: `Json` and `Binary`.** The most common serialization operation — whole value in, encoded form out, and back — needs none of the agent ceremony. Two static classes, declared in `System.Serialization` beside the agents they wrap, provide it; the agents remain the extension point (streaming and incremental writing, multi-value streams, custom `ISerializer`/`IDeserializer` formats, hand-driven [§2.14](#sec-serialization-vocabulary) conformances) — the [§7.3](#sec-file) `File`-over-`FileStream` shape.

> ⚠ **Spec-ahead-of-implementation ([#2349](https://github.com/s8lap/bezel-lang/issues/2349)):** the `Json` and `Binary` facades are not implemented; the agent spelling above (`JsonSerializer`/`JsonDeserializer`, `BinarySerializer`/`BinaryDeserializer`) is.

```bezel
// One-shot facades over the agent pairs. The write members are ordinary
// constrained generics; Parse/ParseOwned are compiler-known (see below).
public static class Json
  // Write: value → encoded form. `value` is borrowed for the call; the
  // options overloads thread to the JsonSerializer constructor.
  public static method ToText<T>(value: T): string where T: ISerializable;
  public static method ToText<T>(value: T, options: JsonOptions): string where T: ISerializable;
  public static method ToBytes<T>(value: T): owned OwningBuffer<byte> where T: ISerializable;
  public static method ToBytes<T>(value: T, options: JsonOptions): owned OwningBuffer<byte> where T: ISerializable;

  // Read: encoded form → value, for value-type targets (records, enums,
  // value-like choices — spec §9.8); a class-typed T is a compile-time error
  // directing to ParseOwned<T>. The input forms mirror the JsonDeserializer
  // constructor set above one-for-one.
  public static method Parse<T>(text: string): T where T: IDeserializable;
  public static method Parse<T>(text: string, options: JsonOptions): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlySpan<byte>): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlySpan<byte>, options: JsonOptions): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlyArraySpan<byte>): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlyArraySpan<byte>, options: JsonOptions): T where T: IDeserializable;

  // Read, class targets: the recognized factory's owned T transfers to the
  // caller. Same six input forms as Parse<T>.
  public static method ParseOwned<T>(text: string): owned T where T: class, IDeserializable;
  public static method ParseOwned<T>(text: string, options: JsonOptions): owned T where T: class, IDeserializable;
  public static method ParseOwned<T>(input: ReadOnlySpan<byte>): owned T where T: class, IDeserializable;
  public static method ParseOwned<T>(input: ReadOnlySpan<byte>, options: JsonOptions): owned T where T: class, IDeserializable;
  public static method ParseOwned<T>(input: ReadOnlyArraySpan<byte>): owned T where T: class, IDeserializable;
  public static method ParseOwned<T>(input: ReadOnlyArraySpan<byte>, options: JsonOptions): owned T where T: class, IDeserializable;
end Json;

public static class Binary
  // The binary format is bytes-only: no text form. It has no options record;
  // its one knob is the BinaryDeserializer constructor bool, mirrored here.
  public static method ToBytes<T>(value: T): owned OwningBuffer<byte> where T: ISerializable;

  public static method Parse<T>(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool := false): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlyArraySpan<byte>, ignoreUnknownFields: bool := false): T where T: IDeserializable;

  public static method ParseOwned<T>(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool := false): owned T where T: class, IDeserializable;
  public static method ParseOwned<T>(input: ReadOnlyArraySpan<byte>, ignoreUnknownFields: bool := false): owned T where T: class, IDeserializable;
end Binary;
```

Each facade member is defined — normatively, not illustratively — as the corresponding agent sequence, with the same options and the same fault surface, and **byte-identical output**: a facade write and a hand-driven agent write of the same value under the same options produce identical bytes, and a facade read accepts exactly the inputs, and raises exactly the exceptions, of the corresponding agent read **driven over a whole document** — which means the agent sequence plus the document-exhaustion assertion (**Facade faults** below). Each member constructs its format's agent with the identically-spelled arguments, invokes `value.Serialize(s)` or the recognized `T.Deserialize(d)` factory — a read then asserting document exhaustion (**Facade faults** below) — and returns the agent's corresponding result accessor (`ToString()` for `ToText`, `ToBytes()` for `ToBytes`). The scope-bound `SavePoint`/`LoadPoint` bodies under **Ownership and lifetime** below are the definitional expansions of two of the forms (the read body plus the exhaustion assertion, which no [§2.14](#sec-serialization-vocabulary) vocabulary member spells):

```bezel
import System.Serialization;
import System.IO;

// The SavePoint/LoadPoint pair, restated through the facade (Point: the §2.14
// example record). The agent-driven bodies below are what these calls mean.
public method SavePoint(p: Point): owned OwningBuffer<byte>
  return Json.ToBytes(p);
end SavePoint;

public method LoadPoint(bytes: ReadOnlySpan<byte>): Point
  return Json.Parse<Point>(bytes);
end LoadPoint;

// A class target arrives owned (Order: the §2.14 example class); binary bytes
// compose with the §7.3 statics — WriteAllBytes' span overload is the
// zero-copy serializer-output path.
public method Reencode(text: string, path: string)
  using var order := Json.ParseOwned<Order>(text);
  using var blob  := Binary.ToBytes(order);
  File.WriteAllBytes(path, blob.AsReadOnlySpan());
end Reencode;
```

**The write family is ordinary generic code; the read family is compiler-known.** `ISerializable` has a real instance method, so `ToText`/`ToBytes` reach `value.Serialize(s)` through their constraint and are user-writable Bezel. The read family is not: `IDeserializable` is a marker interface with no members ([§2.14](#sec-serialization-vocabulary)), so no generic body can reach the recognized `T.Deserialize(d)` factory through a constraint, and `Parse<T>`/`ParseOwned<T>` (both formats) are **compiler-known generics** — the same mechanism as `Task.FromResult<T>`'s compiler-special-cased class-`T` rejection ([§6.7](#sec-task-static-helpers)). `Parse<T>` requires an `IDeserializable`-conforming **value-type** target — a record, enum, or value-like choice (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis)) — and rejects a class-typed `T` at compile time with a diagnostic directing to `ParseOwned<T>`; `ParseOwned<T>` requires an `IDeserializable`-conforming class whose recognized factory returns `owned T`. A class whose factory has the third [§2.14](#sec-serialization-vocabulary) shape — `Deserialize(d): owned Shared<C>`, the shared-node entry — has no facade form and is read through the agents; `ParseOwned<C>` over such a class is a compile-time error. Were the recognized factory promoted to a static interface member (the recorded follow-up of rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection), itself gated on the `Self` return type [§2.14](#sec-serialization-vocabulary) separately defers), all four become expressible as ordinary constrained generics with no signature change.

**Ownership at the facade.** `value` is an unmarked parameter — a borrow for the duration of the call at class substitutions (language spec [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)), a value at record/enum substitutions; the caller keeps ownership and the disposal obligation. The results are fresh productions under [§1](#sec-conventions)'s conventions: `ToText` returns a runtime-managed `string`, `ToBytes` returns `owned OwningBuffer<byte>` ([§4.14](#sec-owned-buffers-and-spans)), and `ParseOwned<T>` returns `owned T` (language spec [§28.8](Bezel_Language_Spec.md#sec-ownership-transfer)), discharged by `using var` or an owned field. Inside a facade read the deserializer's input-span borrow is bounded by the call itself, so the caller's obligation to keep the backing bytes alive ends when the call returns — unlike the agent path, where the span must outlive the `using var d` scope.

**Facade faults.** Every facade member faults, in every delegated condition, exactly as its defining agent sequence faults — malformed input, type mismatches, depth violations, unknown fields under strict policy, and the JSON write path's non-finite floats — raising `SerializationException` with the same `Offset`/`Line`/`Column` surface, because it is the same code raising it. One condition is the facade's own: after the root value completes, remaining non-whitespace input raises `SerializationException` from the facade read itself — the **document-exhaustion assertion** that defines *driven over a whole document* above. The agent path deliberately carries no such fault: its cursor may rest mid-stream for a subsequent read (the multi-value idiom), and deserializer disposal never raises. Under the failure-signal rule ([§1](#sec-failure-signal-rule)) the read-side conditions (the exhaustion condition included) classify with the agents' own, in band 4 under the rule's second clause: they are band-3-shaped — expected at the decode boundary, information-bearing — but no value-like error-payload type yet exists to carry them (the R3/R2 sequencing recorded), so the typed exception is the mechanism, that reason is recorded here, and the classification stays revisitable at the decode-cluster pilot. The write path's non-finite-float condition is a separate condition with no input to locate (all three location properties are `null` there, above): a caller-chose-an-unrepresentable-format condition inherited from the agents, whose existing `SerializationException` spelling stands under [§1](#sec-failure-signal-rule)'s scoping sentence until re-ruled. The throwing form is the surface's one spelling (no value-shaped dual is declared, and no error-payload type). The binary format's interchange caveat (*Binary mapping* above) applies verbatim at the facade: `Binary.ToBytes`/`Parse`/`ParseOwned` produce and consume the compact same-version form — persistence and IPC between builds of one version, not a version-stable wire format.

**Ownership and lifetime.** All four agent classes are `IDisposable`; the serializers own an internal growable byte buffer, the deserializers own parser state. The idiom is scope-bound:

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

**Errors.** Malformed input, type mismatches (a `ReadInt` when the next token is a string), and depth-limit violations raise `SerializationException` (declared in `System.Serialization`). Trailing data is the **whole-document** read forms' condition: the facades' reads assert document exhaustion and raise it (above), while the agent path deliberately leaves remaining input unread and never faults on it — the cursor may rest mid-stream for a subsequent read, which is exactly how a multi-value stream is read — and deserializer disposal never raises. `SerializationException` locates the fault through its declared surface — `Offset` for the binary format, `Line`/`Column` for JSON; the other format's properties are `null` on each instance, and all three are `null` on an instance raised by the write side (e.g. the JSON non-finite-float rule above), where there is no input to locate:

```bezel
public class SerializationException: Exception
  public readonly property Offset: long?;   // byte offset of the fault in the input (binary; null for JSON)
  public readonly property Line: int?;      // line of the fault in the input text (JSON; null for binary)
  public readonly property Column: int?;    // column of the fault in the input text (JSON; null for binary)
end SerializationException;
```

The three properties are value-typed and copy out of a `catch` arm like any other snapshot state (language spec [§31.1](Bezel_Language_Spec.md#sec-throwing-and-catching)). The deserializer interfaces' `Try*`-shaped members (`TryReadNull`) do not throw; every other read member throws on mismatch rather than returning a sentinel, because a deserialization mismatch is a structural error in the input, not an expected control-flow branch.

**Unknown-field policy.** A field present in the input that matches no declared component of the target type is, by default, a `SerializationException` — the strict default established when synthesis shipped, because silently dropping unrecognized input masks producer/consumer schema drift and field-name typos. This behavior is a property of the deserialization *operation*, not of the target type: the same type may legitimately be read strictly when consuming a trusted internal message and leniently when consuming a third-party producer that adds fields freely, a decision that belongs to the caller performing the read, not to the type's serialization contract (rationale [§5.19](Bezel_Design_Rationale.md#sec-serialization-without-reflection)). It is therefore configured on the deserializer: `JsonOptions.IgnoreUnknownFields` (default `false`) for JSON, the `ignoreUnknownFields` parameter (default `false`) of the `BinaryDeserializer` constructors and of the `Binary.Parse<T>`/`ParseOwned<T>` facade. When set, the deserializer skips an unrecognized field's value (via `IDeserializer.SkipValue`, [§2.14](#sec-serialization-vocabulary), which consumes the value including any nested object/array structure) and continues rather than raising. The synthesized `Deserialize` (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis)) is identical regardless of policy — it always delegates an unrecognized field to the deserializer; strictness lives entirely in the deserializer's configuration. This policy governs only *unknown* fields; missing required fields, duplicates, and type mismatches remain unconditional errors regardless of the setting, and a *missing* declared field is tolerated only by the per-member `serialized optional` clause (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis)) — the two leniencies are orthogonal and compose.

`JsonSerializer`, `JsonDeserializer`, `BinarySerializer`, `BinaryDeserializer`, `JsonOptions`, and `SerializationException` are declared in `System.Serialization` and require an explicit `import System.Serialization`, and the one-shot facades `Json` and `Binary` are declared beside them under the same import. The vocabulary interfaces the agent classes implement (`ISerializer`/`IDeserializer`/`ISerializable`/`IDeserializable`, [§2.14](#sec-serialization-vocabulary)) remain auto-imported, so a type's `: ISerializable` conformance never requires the import — only the code that picks a concrete format does.

<a id="sec-shared-and-weak"></a>
### 2.16 `Shared<T>` and `Weak<T>`

Scoped shared ownership (language spec [§28.13](Bezel_Language_Spec.md#sec-shared-ownership)): N handles jointly own one underlying value; the last handle's disposal disposes it. Both types are compiler-known; the shared count and last-one-disposes behavior are not expressible in user Bezel. Both are auto-imported with `System`. `Shared<T>` is sendable iff `T` is in the `Shared`-payload synchronized set of language spec [§28.13](Bezel_Language_Spec.md#sec-shared-ownership) (derived there from the [§1](#sec-statics-and-sharing-facts) statics-and-sharing facts — currently `Mutex`, `Atomic<T>`, `AtomicInteger<T>`, `Semaphore`, `Locked<T>`, `RwLocked<T>`, `FrozenArray<T>`, `CancellationTokenSource`; the count is maintained with synchronized operations); `Weak<T>` and `Shared<T>` of any other `T` remain domain-local ([§30.14](Bezel_Language_Spec.md#sec-sendable-types)).

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
  public method TryUpgrade(): owned Shared<T>?;              // a new strong handle if the value
                                                              //   is still alive; null otherwise
                                                              //   (§1 Try shape (b))
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
- `TryUpgrade` follows Try shape (b) (§1 — null alone is the signal: `Shared<T>` is a proper class type at every instantiation, so the handle itself carries the outcome; the `Mutex.TryAcquire` acquisition shape): `using var s := w.TryUpgrade(); if s <> null then ... end;` — on non-null the caller owns a fresh handle, discharged by the `using var`; a null binding's scope-end disposal is a no-op. The strong count reaches zero before the underlying value's disposal begins, so a `TryUpgrade()` that runs during that disposal — from the value's own destructor, or from another disposer of the same cleanup pass — returns `null`.

<a id="sec-built-in-exceptions-cross-reference"></a>
### 2.17 Built-in exception types (cross-reference)

*(`ContractViolationException` — a `requires`/`ensures`/`invariant` clause evaluated `false`; language spec [§22.16](Bezel_Language_Spec.md#sec-contract-clauses), [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types).)*

The built-in exception hierarchy — `Exception` and the types available without `import`, whose module is `System` — is normative in the language spec ([§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)); this document does not repeat it. Exceptions whose producers live in this document require the producer's `import` and are declared alongside it (e.g. `KeyNotFoundException` in [§4.8](#sec-dictionaryk-v), the IO exceptions in [§7.5](#sec-io-exception-types)). Their **constructor surface** is the language spec's rule: a type declared here with no constructor shown carries `Exception`'s `()`, `(message)`, and `(message, cause)`; one that shows constructors carries exactly those.

The exception types that carry declared surface beyond `Exception`'s are enumerated by reference rather than counted: the built-ins `ArgumentException` (`ParamName` — language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)) and `TypeInitializationException` (below), and the producer-local `SerializationException` (`Offset`/`Line`/`Column` — [§2.15](#sec-concrete-serialization-formats)) and `DomainFaultedException` (`FaultStackTrace` — [§6.8](#sec-domain)). Of these, `TypeInitializationException` is recorded here for completeness:

```bezel
public class TypeInitializationException: Exception
  public readonly property InnerException: Exception?;   // the original initializer exception (null for the spec §22.13 cycle backstop)
end TypeInitializationException;
```

Raised on access to a static member of a type whose static initialization failed; `InnerException` carries the original exception — `null` for the instance the runtime synthesizes at the static-init cycle backstop, where no original exists (language spec [§22.13](Bezel_Language_Spec.md#sec-static-initialization-order), [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)).

---

<a id="sec-iformattable"></a>
### 2.18 `IFormattable` and format specifiers (#812/#1339)

```bezel
public interface IFormattable
  method ToString(format: string): string;
end IFormattable;
```

Auto-imported. String interpolation dispatches a hole's format spec here (language spec §18.5): `$"{money:C2}"` calls `money.ToString("C2")` when `money`'s type implements `IFormattable`. A **user-defined** `IFormattable` implementation defines its own specifier vocabulary — the table below governs only the intrinsic types, with the strict-fault rule applying within each vocabulary (`money` above is such a user type defining its own `"C2"`; the intrinsic vocabulary deliberately has no `C`). The primitive and temporal types are **intrinsically formattable** (they behave as conforming implementations without declaring the interface); a format spec on any other non-conforming operand is a compile error.

**The specifier vocabulary is invariant-culture only** — Bezel has no culture machinery, and formatted output is identical on every platform. An unrecognized specifier raises `FormatException` (strict-fault, as everywhere).

| Kind | Specifiers | Notes |
|---|---|---|
| Integers (`int`, `long`, `byte`, …) | `D`/`Dn` decimal (n = min digits, zero-padded) · `X`/`x` hex (upper/lower, `Xn` pads) · `B` binary | no grouping separators (no culture) |
| Floating (`double`, `float`) | `F`/`Fn` fixed-point (n decimals, default 2) · `E`/`En` scientific · `G`/`Gn` general · `P`/`Pn` percent (×100, `%` suffix) · `R` round-trip | `R` and the **no-format `ToString()`** both produce the shortest string that parses back to the identical value — of several such, the one nearest it (round-trip guarantee) |
| `decimal` | `F`/`Fn`, `E`/`En`, `G`, `P`/`Pn` | exact decimal semantics; `G` and the **no-format `ToString()`** both produce the exact shortest form ([§9.3](#sec-decimal)) |
| `DateTime`, `DateOnly`, `TimeOnly` | pattern strings of `yyyy` `MM` `dd` `HH` `mm` `ss` `fff` plus literal separators | e.g. `yyyy-MM-dd HH:mm:ss`; no-format `ToString()` is ISO 8601 |
| `TimeSpan` | `c` (constant: `[-][d.]HH:mm:ss[.fffffff]`) | no-format = `c`; each bracketed part appears only under its condition (*The `TimeSpan` constant form*, below) |
| `Guid` | `D` (hyphenated, lowercase), `N` (no hyphens) | no-format = `D` |
| `bool`, `char`, `string` | *(none)* | a format spec on these is a compile error |

`C` (currency) deliberately does not exist — currency formatting is culture by definition; format money explicitly (`$"{amount:F2} EUR"`).

**`decimal` midpoint rule.** For a `decimal` operand, digit reduction under `F`/`Fn`, `P`/`Pn`, and `E`/`En` rounds a midpoint **half-to-even** (banker's rounding), agreeing with `Decimal.Round` ([§9.3](#sec-decimal)): `0.125m` under `F2` renders `"0.12"`, and `P`/`E` apply the same rule to their scaled digit strings — the format specifiers and `Round` are two spellings of one rounding operation and never disagree at a midpoint.

**Non-finite `double` and `float` values.** The no-format `ToString()` and every floating specifier (`F`, `E`, `G` and `P`, with or without a digit count, and `R`) render a NaN as `NaN`, positive infinity as `Infinity` and negative infinity as `-Infinity` — the names of the `NaN`, `PositiveInfinity` and `NegativeInfinity` constants ([§9.2](#sec-double-and-float)) — with no digits, no exponent and no `%` suffix: `$"{1.0 / 0.0}"` and `(1.0 / 0.0).ToString("P2")` both render `"Infinity"`. Every NaN renders `NaN`, whatever its sign bit and payload. `Parse` reads each of the three back ([§9.2](#sec-double-and-float)); for a NaN the round trip yields `NaN`, equal to the original under `=` (language spec [§4.3.1](Bezel_Language_Spec.md#sec-equality-semantics-for-floating-point-types)), and does not carry its sign or payload bits, which `BitConverter` ([§2.12](#sec-bitconverter)) carries.

**Temporal patterns: non-specifier characters are literals.** In a temporal pattern (`DateTime`, `DateOnly`, `TimeOnly`), every character that does not begin one of the specifiers above is a **literal**, copied to the output verbatim — `T`, `Z`, `-`, `:`, space, and any other non-specifier character alike; the unrecognized-specifier fault governs malformed runs of the specifier letters (`y` `M` `d` `H` `m` `s` `f`), not literal characters. The temporal specifier vocabulary grows only by recorded ruling, and never in a way that reinterprets a character that passes through as a literal — a new specifier letter would be a breaking change for every pattern using it as a literal, and this section's default answer to such a proposal is no.

**Temporal field widths.** Each of the seven specifiers renders a fixed-width, zero-padded field of exactly its own letter count: `yyyy` four digits (year 42 renders `0042`), `MM`, `dd`, `HH`, `mm`, and `ss` two each (day 5 renders `05`, hour 0 renders `00`), and `fff` three — the millisecond component. A field is never widened, never trimmed, and never grouped; a run of one of the specifier letters at any other length is the malformed run the paragraph above faults.

**The `TimeSpan` constant form.** `c` renders the span's magnitude behind at most one sign: whole days, hours, minutes and seconds, decomposed as the integer components are ([§2.5](#sec-datetime-timespan-dateonly-timeonly)) but unsigned, and the sub-second remainder in ticks. Each bracketed part of `[-][d.]HH:mm:ss[.fffffff]` appears exactly when its condition holds: `-` when the span is negative; `d.`, the whole days in decimal with no padding and then a `.`, when the magnitude is at least one day; and `.fffffff`, a `.` and then the sub-second remainder in ticks as exactly seven zero-padded digits, never trimmed, when that remainder is non-zero. `HH:mm:ss` always appears, each field two zero-padded digits, `HH` running from `00` to `23`. So `TimeSpan.FromMinutes(90.5)` renders `01:30:30`, `TimeSpan.FromDays(1.5)` renders `1.12:00:00`, `TimeSpan.FromMilliseconds(1.5)` renders `00:00:00.0015000`, `-TimeSpan.FromSeconds(90.0)` renders `-00:01:30`, `TimeSpan.Zero` renders `00:00:00`, and `TimeSpan.MinValue` renders `-10675199.02:48:05.4775808`. The no-format `ToString()`, `ToString("c")` and an interpolation hole with or without `:c` produce this one rendering.

<a id="sec-static-abstractions"></a>
### 2.19 `INumber<T>`, `ISteppable<T>`, `IParsable<T>`, and the collection-literal target pair (#807)

The static-interface abstractions (language spec [§11.8](Bezel_Language_Spec.md#sec-static-interface-members)). The primitive numeric tower conforms to the first three, and `char` to `ISteppable<char>` and `IParsable<char>` only, via compiler-provided conformances (the list below; `char` is not an `INumber<T>` conformer, [#6956](https://github.com/s8lap/bezel-lang/issues/6956)); user types conform by declaring and supplying the statics. A conformer's `<` brings language spec [§25.2](Bezel_Language_Spec.md#sec-operator-overloading-restrictions)'s complete group with it — the type declares `>`, `<=`, and `>=` as well, as every type declaring a relational operator does — and that obligation is what entitles a `where T: INumber<T>` or `where T: ISteppable<T>` body to all four comparisons (§25.2, *Type-parameter operands*). The collection-literal target pair at the end of this section is conformed to by declaration only — by the ten built-in sequence collections in their own declarations ([§4.3](#sec-how-the-borrowing-and-owning-hierarchies-relate)) and by user classes.

```bezel
public interface INumber<T> where T: INumber<T>
  static method Zero(): owned T;               // producers spell owned: a fresh value at
  static method One(): owned T;                //   class/interface conformers; erased at value ones (§28.1)
  static operator +(a: T, b: T): owned T;
  static operator -(a: T, b: T): owned T;
  static operator *(a: T, b: T): owned T;
  static operator /(a: T, b: T): owned T;      // raises per the operand type's division rules
  static operator <(a: T, b: T): bool;
end INumber;

public interface ISteppable<T> where T: ISteppable<T>
  static method Step(value: T, by: long): owned T;   // advance in T's space — a fresh value,
                                                     //   checked as T's own arithmetic is (see Step below)
  static operator <(a: T, b: T): bool;
end ISteppable;

public interface IParsable<T> where T: IParsable<T>
  static method Parse(s: string): owned T;                       // FormatException on bad input
  static method TryParse(s: string, out owned value: T?): bool;  // the §1 Try convention
end IParsable;
```

Conformances: `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` conform to all three; `char` to `ISteppable<char>` and `IParsable<char>`. (`Range<T>` admits only the integer conformers and `char`: conformance is necessary, not sufficient, there — [§9.8](#sec-ranget), language spec §16.3.) `string` conforms to none (concatenation is not addition; parsing a string to a string is meaningless), and `bool` conforms to none — deliberately (#1792): its `Parse`/`TryParse` ([§9.4](#sec-boolean)) are plain static members, not witnesses; the constraint's clients are generic scalar-parsing paths, a caller that means bool spells `bool.TryParse` directly, and a later admission would be a compatible widening if a use case arrives. `Parse` is witnessed by the existing per-type members unchanged — for `char`, the one-code-point members of [§9.5](#sec-char). For `TryParse`, the per-type members keep their non-nullable value-typed outs (the shape-(c) convention, [§1](#sec-conventions)); the **compiler-provided conformance bridges the out-nullability difference**, so `T.TryParse(s, out using var v)` **through the constraint** has the declared `out owned value: T?` shape (the marker erased at value substitutions, language spec [§28.1](Bezel_Language_Spec.md#sec-where-the-owned-modifier-can-appear)) while a **direct** call keeps `out result: T` (language spec [§11.8](Bezel_Language_Spec.md#sec-static-interface-members)).

**`Step`.** `T.Step(value, by)` is `value` advanced by `by` in `T`'s own arithmetic, and it reports leaving `T`'s range as that arithmetic does ([#7275](https://github.com/s8lap/bezel-lang/issues/7275)). At an integer `T` the result is the mathematical `value + by`, whatever `by`'s sign, and a result not representable in `T` raises `OverflowException` (language spec [§6.1](Bezel_Language_Spec.md#sec-integer-arithmetic)). At `decimal` it is `value + by` under language spec [§6.4](Bezel_Language_Spec.md#sec-decimal-arithmetic), raising `OverflowException` on overflow; at `float` and `double` it is `value + by` under [§6.3](Bezel_Language_Spec.md#sec-floating-point-arithmetic)'s IEEE 754 arithmetic, `by` converting by language spec [§5.1](Bezel_Language_Spec.md#sec-implicit-conversions) (which may round), and nothing is raised. At `char` it is the code-point advance language spec [§6.6](Bezel_Language_Spec.md#sec-compound-assignments) gives `c += n`, `char(long(value) + by)`: a result outside U+0000–U+10FFFF or in the surrogate block raises `OverflowException` ([§5.2](Bezel_Language_Spec.md#sec-explicit-conversions)). It does not step across the surrogate gap as `Range<char>` iteration does ([§16.3](Bezel_Language_Spec.md#sec-iteration-over-rangechar)): that successor function is an iteration's, not this member's. A user conformer's `Step` is its declared body. So `T.Step(1, 4_000_000_000L)` at `T = int` raises, `T.Step('a', 1)` at `T = char` is `'b'`, and `T.Step('\uD7FF', 1)` raises.

**Producer markers.** Every §2.19 member that produces a value spells the production: `owned` returns on `Zero`, `One`, the four arithmetic operators, `Step`, `Parse`, and both `FromCollectionLiteral` members, and the `out owned` slot on `TryParse` — arithmetic mints a *new* number, never one of its operands, so at a class or interface conformer each witness transfers a fresh value on every path (language spec [§28.8](Bezel_Language_Spec.md#sec-ownership-transfer)), which is what makes the [§11.8](Bezel_Language_Spec.md#sec-static-interface-members) `Sum<T>` accumulator sound there (each iteration's displaced total is disposed by drop-on-overwrite, language spec [§28.7](Bezel_Language_Spec.md#sec-reassigning-owned-fields)). The `<` operators return `bool` and carry no marker, and the free selectors `Math.Min`/`Max`/`Clamp` ([§2.2](#sec-math)) deliberately keep their bare-`T` borrow signatures — a selector transfers nothing. At every **value or runtime-managed** conformer the markers are erased by the substitution (language spec [§28.1](Bezel_Language_Spec.md#sec-where-the-owned-modifier-can-appear)): the primitive tower's intrinsic witnesses and any user record or enum conformer are declared — and remain — unmarked, matching the erased signatures exactly; only class and interface conformers spell the markers.

**The collection-literal target pair.** The two static interfaces behind the sequence-literal protocol (language spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol)): a target conforms at `TSelf` = itself and supplies the one factory; exactly one of the two per class, declared on the class itself. The ten built-in sequence collections declare the conformance and list the factory in their own sections ([§4.3](#sec-how-the-borrowing-and-owning-hierarchies-relate)). Neither interface is variant (language spec [§11.8](Bezel_Language_Spec.md#sec-static-interface-members), consequence 2).

```bezel
public interface ICollectionLiteralTarget<TSelf, T>
    where TSelf: ICollectionLiteralTarget<TSelf, T>
  static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned TSelf;   // the borrowing form: elements
end ICollectionLiteralTarget;                                                           //   are borrows (§4.15 view)

public interface IOwningCollectionLiteralTarget<TSelf, T>
    where TSelf: IOwningCollectionLiteralTarget<TSelf, T>
    where T: class
  static method FromCollectionLiteral(owned elements: OwningArray<T>): owned TSelf;   // the owning form: the factory
end IOwningCollectionLiteralTarget;                                                    //   drains the array (§4.5)
```

<a id="sec-result"></a>
### 2.20 `Result<T, E>`

The outcome type for band 3 of the failure-signal rule ([§1](#sec-failure-signal-rule)) — expected failure, branched on at or near the call site, with a value-like error payload (language spec [§35](Bezel_Language_Spec.md#sec-choices)): a closed sum of a success payload and an error payload. Auto-imported. The stdlib ships exactly this one choice; `Option<T>` is deliberately absent — `T?` is Bezel's option type (proposals `DECLINED.md` D-0004).

```bezel
public choice Result<T, E>
  Ok(owned value: T),
  Error(owned error: E);

  public method IsOk(): bool => (case this of
    Ok(_)    => true,
    Error(_) => false);

  public method IsError(): bool => not IsOk();

  // Compiler-provided (spec §35.2): admissible only where the receiver AND the
  // member's result instantiation are value-like; a call at (or producing) a
  // borrow-bearing or owning instantiation is rejected at the use site (spec
  // §3.6 rule 2), the diagnostic naming the classification and the deciding
  // payload.
  public method Map<U>(f: (T) -> U): Result<U, E>;
  public method MapError<F>(f: (E) -> F): Result<T, F>;
  public method AndThen<U>(f: (T) -> Result<U, E>): Result<U, E>;
  public method UnwrapOr(fallback: T): T;
end Result;
```

`T` and `E` are unconstrained; the `owned` payload markers follow the per-instantiation erasure of language spec [§28.1](Bezel_Language_Spec.md#sec-where-the-owned-modifier-can-appear): `Result<int, string>` is value-like (Core; copyable; auto-hashable when the payloads are admissible), while `Result<Connection, IoError>` is owning per instantiation (Extended — the move-only row of Appendix D.2). `Result` declares no interface conformances, keeping every instantiation classification-clean. There is no `Unwrap` — a method receives `this` as a borrow (spec [§35.2](Bezel_Language_Spec.md#sec-choice-members)), so an owned-payload extractor is unspellable as a method, correctly: extraction is consumption, and consumption is the owning match's job, exhaustiveness-checked where `Unwrap` would fault. (`UnwrapOr` below is not an extractor and does not disturb this record: it is total — a value-in/value-out fold with no fault path — and exists only at the value-like instantiations, where nothing is consumed.)

**The value-payload combinators.** `Map`, `MapError`, `AndThen`, and `UnwrapOr` are **conditionally-admitted members** (language spec [§14.2](Bezel_Language_Spec.md#sec-conditionally-admitted-members), *Conditionally-admitted members*; the choice-specific clauses, and the compiler-provided bodies, at spec [§35.2](Bezel_Language_Spec.md#sec-choice-members)): admissible only where the receiver instantiation **and the member's result instantiation** (`Result<U, E>` for `Map`/`AndThen`, `Result<T, F>` for `MapError`) are both **value-like** (spec [§35.4](Bezel_Language_Spec.md#sec-choice-classification) rule 3 — every payload a value, `string`, runtime-managed type, or transitively value-like record or choice; no payload `owned`, move-only, or borrow-bearing; function-typed payloads are classification-neutral and ride), and rejected at the use site otherwise — a borrow-bearing or owning receiver, or an inferred `U`/`F` that would make the result borrow-bearing or owning (spec [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties) rule 2), the diagnostic naming the classification, the deciding payload, and the owning-match remedy.

| Member | Semantics |
|---|---|
| `Map(f)` | maps `Ok(v)` to `Ok(f(v))` and passes `Error(e)` through unchanged |
| `MapError(f)` | maps `Error(e)` to `Error(f(e))` and passes `Ok(v)` through |
| `AndThen(f)` | returns `f(v)` on `Ok(v)` and passes `Error(e)` through |
| `UnwrapOr(fallback)` | returns `v` on `Ok(v)` and `fallback` on `Error(_)` |

`fallback` is an ordinary **value** argument, not a deferred computation (there is no lazy `UnwrapOrElse`): the argument expression is evaluated at the **call site, before the call**, on the `Ok` path exactly as on the `Error` path, under spec [§22.5](Bezel_Language_Spec.md#sec-optional-parameters-and-named-arguments)'s normative argument-evaluation order, so a side-effecting or throwing fallback expression has its effect on either case — where the fallback must be computed only on failure, compute it in the `Error` arm of a `case` (spec [§35.5](Bezel_Language_Spec.md#sec-choice-matching)). The parameter name is API surface under spec [§22.5](Bezel_Language_Spec.md#sec-optional-parameters-and-named-arguments). The function parameters are plain, hence call-bounded (spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters)) — invoked during the call, never retained; call-boundedness fixes when the function *value* is invoked, not when the argument expression producing it is evaluated, which is the call site's own spec [§22.5](Bezel_Language_Spec.md#sec-optional-parameters-and-named-arguments) order (`r.Map(MakeMapper())` evaluates `MakeMapper()` before the call, on either case) — and `U`/`F` infer at the call site (spec [§14.5](Bezel_Language_Spec.md#sec-type-argument-inference) phase 2). None of the four is `pure`: invoking a function-typed value is on the pure body's prohibited list (spec [§22.15](Bezel_Language_Spec.md#sec-pure-members)), and `UnwrapOr` — which invokes nothing and would be pure-eligible — stays plain `method` so the four-member family carries one purity posture rather than a lone `pure` sibling (a uniformity choice, recorded liftable; unlike the `Contains` ruling, which turned on invoking user equality). The receiver is a borrow; at the admissible instantiations the internal match binds copies (value payloads) or shared references (`string`/runtime-managed payloads), and every result is a fresh copyable value — no disposal obligation arises anywhere in a combinator call, and the combinators declare no fault condition (all four are total; the [§1](#sec-failure-signal-rule) failure-signal rule is engaged vacuously). **Owning instantiations carry none of these members**: an owning `Map` would have to consume the payload through a receiver a method only ever borrows, and its `f` would need per-instantiation ownership polymorphism — the retired-and-deferred territory of spec [§14.4](Bezel_Language_Spec.md#sec-owned-and-generic-type-arguments); the owning idiom remains the owning match (`case move x of …`; adopt with `using var` in the arm — the connect-or-explain example below).

```bezel
// Value-like instantiation (Core): parse outcome with information.
public method ParsePort(text: string): Result<int, string>
  if not int.TryParse(text, out var value) then
    return Error($"not a number: {text}");    // declared-result position (spec §35.3)
  end;
  if value < 1 or value > 65535 then
    return Error($"out of range: {value}");
  end;
  return Ok(value);
end ParsePort;

case ParsePort(arg) of
  Ok(var p)    => Listen(p),
  Error(var m) => Console.WriteLine($"bad port: {m}")
end;                                  // exhaustive; no _ needed
```

```bezel
// Value-payload pipelines over the combinators (value-like instantiations).
var port := ParsePort(raw).UnwrapOr(8080);

var label := ParsePort(raw)
    .Map(lambda(p) => $"listening on {p}")
    .UnwrapOr($"bad port: {raw}");

// Re-code the error at a boundary (MapError):
public record ConfigError(Key: string, Message: string);

public method PortSetting(raw: string): Result<int, ConfigError>
  return ParsePort(raw).MapError(lambda(m) => new ConfigError("port", m));
end PortSetting;

// Sequence two fallible steps without an intermediate case (AndThen); the
// ascribed lambda's returns are declared-result positions (spec §35.3):
public method WellKnownPort(text: string): Result<int, string>
  return ParsePort(text).AndThen(lambda(n): Result<int, string> => do
    if n > 1023 then return Error($"not a well-known port: {n}"); end;
    return Ok(n);
  end);
end WellKnownPort;
```

```bezel
// Owning instantiation (Extended): connect-or-explain.
public method Connect(host: string): Result<Connection, string>
  var conn := TryOpen(host);          // illustrative TryOpen(host): owned Connection?
  if conn = null then
    return Error($"unreachable: {host}");
  end;
  return Ok(move conn);                 // owned payload slot: marker consumes (spec §35.3)
end Connect;

case Connect(host) of                 // owning match over the temporary
  Ok(var c) => do
    using var conn := move c;         // adopt; disposed at block end
    conn.Send(payload);
  end,
  Error(var m) => Console.WriteLine($"giving up: {m}")
end;
```

The Try shapes of §1 remain the right tool for value extraction with a boring failure; the `Result` return is where the caller branches on the failure and its payload is value-like (the failure-signal rule's band 3, [§1](#sec-failure-signal-rule)) — the two idioms compose (`ParsePort` uses `TryParse` internally and returns a choice).

<a id="sec-scopeguard"></a>
### 2.21 `ScopeGuard`

> ⚠ **Spec-ahead-of-implementation ([#4669](https://github.com/s8lap/bezel-lang/issues/4669)):** `ScopeGuard` is not implemented: the compiler does not know the type, so a program naming it does not compile. The surface below is the normative contract the implementation will meet.

Failure-only cleanup, packaged: perform a side effect (register something, mutate shared state, hand a handle to a foreign library, begin a transaction), arm a guard whose action undoes it, and `Dismiss()` on success. On any throwing edge the guard's disposal runs the undo; after `Dismiss()` it is a no-op. This is the commit-flag `try`/`finally` idiom — correct and composing today — as one declaration plus one visible commit point, and it requires **no compiler support**: `ScopeGuard` is an ordinary class using `scoped`, `destructor`, and `using var` exactly as declared (language spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters), [§28.4](Bezel_Language_Spec.md#sec-the-destructor), [§28.10](Bezel_Language_Spec.md#sec-using-var)). The language-level `defer`/`errdefer` statements a reader may expect instead are a recorded decline (rationale [§17.11](Bezel_Design_Rationale.md#sec-defer-errdefer)); this type is the sanctioned spelling of the failure half. Auto-imported, in the [§2.10](#sec-idisposable-and-iasyncdisposable)/[§2.11](#sec-subscription) cleanup family.

```bezel
public class ScopeGuard
  public constructor(scoped action: () -> ());     // runs at disposal unless dismissed
  public method Dismiss();                          // disarms; idempotent
  public readonly property IsDismissed: bool;
  destructor;                                       // invokes action unless dismissed
end ScopeGuard;
```

**Facts** ([§1](#sec-statics-and-sharing-facts)): `ScopeGuard` — none (domain-local and mutable, holding a caller-supplied callback whose captures are bounded to the guard's own scope; it belongs to none of the three derived sets — the §6.9 `Channel<T>` precedent of recording a deliberate absence with its ground).

Every element of the declaration is load-bearing under spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters):

- **`scoped` is required, not stylistic.** The guard *stores* the callback and invokes it after the constructing call has returned; §28.14's call-bounded default forbids storing a plain function-typed parameter in any field. `scoped` is the storage-bounded mode, and the guard's action field is exactly the admitted shape: a `scoped` field of the bounding object itself.
- **The spelling is mandatory: explicit constructor, not primary.** Spec [§22.10](Bezel_Language_Spec.md#sec-methods-and-constructors-primary-constructors) rejects a function-typed `scoped` primary-constructor parameter, and `action` is function-typed, so the explicit-constructor form above is the only admitted shape.
- **The bounding object resolves by form 1** (a constructor call → the constructed object), so `using var g := new ScopeGuard(…)` admits a **capture-bearing** lambda whenever every captured local source is declared **before** `g` — the natural order: acquire the thing, then arm the guard that undoes the acquisition. `using var` is the spelling; a guard bound any other way — any binding other than a `using var`/`await using var` local — resolves no admitted bound and accepts capture-free callbacks only (form 1 wins at every `new ScopeGuard(…)`, so the binding form is the whole question). (§28.14's verified argument forms apply: a lambda literal or a method reference.)
- **Quiescence is satisfied by construction.** §28.14 requires that disposal of the bounding object guarantee no stored callback runs after disposal completes; the guard invokes its action **in its own destructor** — during disposal, never after — so the obligation is discharged by the type's definition. This section is the normative statement §28.14 asks of compiler-known types.
- **The freeze is the admission's price, taught before it bites.** Until `g`'s scope exits, the captured local sources are frozen (spec §28.14, K5.7): they may not be moved from, reassigned, or disposed — and, symmetrically, they are **unfrozen once `g`'s scope has exited** (spec §28.14: `g` is disposed at that scope end, and the guard's action runs *in* its destructor, so nothing can observe the source afterwards). The build-guard-commit-*transfer-out* shape (`return conn;` while `g` is live) therefore does not compile as written; the remedy is `do … end` (spec [§21.6](Bezel_Language_Spec.md#sec-do-end-blocks)) bounding the guard tighter than the method, a method reference on `this` (whose capture contribution is the receiver), or keeping the resource in a field, where the freeze does not reach — trading the static bound for spec [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)'s field-held residual-trust regime, with checked disposal (spec [§28.2](Bezel_Language_Spec.md#sec-checked-disposal)) as the backstop. Guide 1's teaching section works the rejected and repaired forms.
- **`Dismiss()` is idempotent**, matching the language-wide idempotent-disposal rule's spirit and [§2.11](#sec-subscription)'s precedent; `IsDismissed` exists for assertions and tests and is not needed by the idiom (dismissal neither extracts a payload nor can fail, so no [§1](#sec-conventions) Try shape applies). The name `Dismiss` is deliberate: `Cancel` means cooperative cancellation in this library ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)), and `Disarm`/`Release` are taken (`Semaphore.Release`; *release* is ownership vocabulary). `ScopeGuard` declares no member colliding with a §5 operator name ([§1](#sec-conventions)'s block-by-name note).

```bezel
public method Register(id: int)
  _registry.Add(id);
  using var g := new ScopeGuard(lambda() => Unregister(id));  // the class's unit-returning
                                                              //   inverse: the action is () -> ()
                                                              //   by construction
  Validate(id);            // may throw
  Publish(id);             // may throw
  g.Dismiss();             // success: keep the registration
end Register;
```

Deliberate absences: no constructor takes a dismissed initial state (the caller spells `g.Dismiss()` on the next line), and no `owned`-taking form exists — a guard owning a *value* is what `using var` on the resource itself already is; the guard's job is the **action**, an effect with no owner. The nearest sibling is [§2.11](#sec-subscription)'s `Subscription` — an owned handle whose disposal undoes a registration — and the difference is worth stating: a `Subscription` is *handed to* you by the registrar and always undoes; a `ScopeGuard` is *built by* you and undoes only when not dismissed. `AsyncScopeGuard` is deliberately not provided: an `await`-ing cleanup would need `IAsyncDisposable`/`await using` and a `() -> Task` action whose invocation from a synchronous disposal path spec §28.14 rejects — recorded as out of scope, not overlooked. If the guard's action itself throws during an exception-path disposal, spec [§28.4](Bezel_Language_Spec.md#sec-the-destructor)'s multi-value cleanup rule governs: the action's exception supersedes the in-flight one, and the superseded exception is recorded into the superseding one's `SupersededChain` before the drop (spec [§31.2](Bezel_Language_Spec.md#sec-try-catch-finally)) — the original failure stays readable in the report.

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
  public pure readonly property Capacity: int;

  public method Append(value: string): StringBuilder;
  public method Append(value: char): StringBuilder;
  public method Append(value: Object): StringBuilder;
  // (#1355): per-type overloads for bool and the numeric primitives —
  // observable behavior = Append(value.ToString()); no Object boxing on the path.
  public method Append(value: bool): StringBuilder;
  public method Append(value: int): StringBuilder;      // + byte/sbyte/short/ushort/uint/long/ulong/float/double/decimal, same shape
  public method Append(value: ReadOnlyStringSpan): StringBuilder;   // (#1810): copies the span's
                                                                    //   code points; = Append(value.ToString())
                                                                    //   without the intermediate string
  public method AppendLine(): StringBuilder;
  public method AppendLine(value: string): StringBuilder;
  public method AppendFormat(format: string, args: Array<Object>): StringBuilder;

  public method Insert(index: int, value: string): StringBuilder;
  public method Remove(startIndex: int, length: int): StringBuilder;
  public method Replace(oldValue: string, newValue: string): StringBuilder;
  public method Clear(): StringBuilder;

  public override method ToString(): string;
end StringBuilder;
```

Methods return `this` to allow chaining.

**Sendability** ([§1](#sec-sendability-statements); language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)). `StringBuilder` is **sendable**, on the sendable form's copy-state ground: it holds text only; every member reads and writes that text by value — `Append(value: Object)` keeps `value.ToString()`'s text, not `value`, and `AppendFormat`'s `args` is a call-bounded borrow — and no member exposes a span or an address of its storage. A moved builder keeps its contents.

`AppendLine(value)` appends `value` followed by `Environment.NewLine` ([§2.4](#sec-environment)); `AppendLine()` appends the terminator alone. No `Append` overload appends a terminator.

**Contracts.** `AppendFormat`'s format string contains literal text and holes `{n}` — `n` a zero-based index into `args` — each hole optionally carrying a format spec after a colon (`{0:F2}`), interpreted per the §2.18 vocabulary against the argument's runtime type; `{{` and `}}` escape literal braces. A malformed format string, a hole index outside `args`, or a spec against a type that accepts none raises `FormatException`. `Insert` and `Remove` raise `IndexOutOfRangeException` when the index or the `(startIndex, length)` window falls outside `0..Length`; `Replace` raises `ArgumentException` when `oldValue` is empty.

<a id="sec-encoding"></a>
### 3.2 `Encoding`

```bezel
public abstract class Encoding
  public abstract method GetBytes(s: string): owned Array<byte>;
  public abstract method GetString(bytes: Array<byte>): string;
  public abstract method GetString(bytes: ReadOnlySpan<byte>): string;       // (#1809): buffer-backed source
  public abstract method GetString(bytes: ReadOnlyArraySpan<byte>): string;  // (#1809): Array-backed twin (#1358)
  public abstract method GetByteCount(s: string): int;                       // (#1355)
  public abstract method GetBytes(s: string, destination: Span<byte>): int;  // returns bytes written;
                                                                             //   IndexOutOfRangeException if too short
  public abstract method GetByteCount(s: ReadOnlyStringSpan): int;           // span-source encode:
  public abstract method GetBytes(s: ReadOnlyStringSpan,                     //   identical contract to the string
                                  destination: Span<byte>): int;             //   forms over the span's range
  public abstract method GetBytes(s: ReadOnlyStringSpan): owned Array<byte>; //   one-shot allocating form
  public abstract readonly property Name: string;

  public static readonly property UTF8: Encoding;
  public static readonly property UTF16: Encoding;
  public static readonly property UTF32: Encoding;
  public static readonly property ASCII: Encoding;
end Encoding;
```

The static properties (`UTF8`, `UTF16`, `UTF32`, `ASCII`) return singleton instances of implementation-provided concrete subclasses. The concrete subclass types are not part of the public API; callers access them through the `Encoding` base type. The singleton instances are program-lifetime objects with no resources to release; their `Dispose()` is a no-op. They are not marked `static owned` — they are exempt from the ownership-discharge requirement by the same reasoning as non-`owned` static class-typed fields ([§22.13](Bezel_Language_Spec.md#sec-static-initialization-order) of the language spec).

`Encoding` is `abstract` to permit implementation-provided concrete subclasses. User code may subclass `Encoding` for custom encodings; user subclasses are subject to normal class disposal rules (they may have destructors and owned fields) and are not sendable (language spec §30.14 rule 2's compiler-known-base clause; [§1](#sec-conventions), *Extensibility*).

**Semantics of the standard encodings.** All four standard encodings follow one error model: malformed or unrepresentable input is a **defined fault**, never a silent substitution. `GetString` raises `FormatException` when the input bytes are not a well-formed sequence in the encoding; `GetBytes` raises `FormatException` when the string contains a code point the encoding cannot represent (possible only for `ASCII`). No replacement characters are produced. (The lenient decode-with-replacement mode was considered and declined — [`Bezel_Changelog.md`](Bezel_Changelog.md), v0.136.)

- **`UTF8`** (`Name` = `"utf-8"`): bytes are the UTF-8 form of the string's code points. `GetBytes` never emits a byte-order mark; `GetString` does not treat a leading `EF BB BF` specially (it decodes to U+FEFF). Ill-formed sequences — overlong forms, unpaired-surrogate encodings, truncated tails — raise `FormatException`.
- **`UTF16`** (`Name` = `"utf-16le"`): little-endian, no BOM emitted. `GetString` requires an even byte count, consumes one leading little-endian BOM (`FF FE`) if present, and raises `FormatException` on an odd count, a big-endian BOM (`FE FF`), or an unpaired surrogate. Big-endian UTF-16 is not provided; a program that needs it byte-swaps explicitly.
- **`UTF32`** (`Name` = `"utf-32le"`): little-endian, no BOM emitted. `GetString` requires a byte count divisible by four, consumes one leading little-endian BOM if present, and raises `FormatException` on any unit that is not a Unicode scalar value (greater than `U+10FFFF` or in the surrogate range).
- **`ASCII`** (`Name` = `"us-ascii"`): strict seven-bit. `GetBytes` raises `FormatException` on any code point above `U+007F`; `GetString` raises `FormatException` on any byte above `0x7F`.

The span-source decode overloads (#1809) are the same operation over a caller-selected range spelled as a span — a window over an `Array<byte>` is `bytes.AsReadOnlySpan(start, length)` ([§4.4](#sec-arrayt)), per [§1](#sec-conventions)'s slice-first rule (*Window parameters*) — identical fault model, BOM consumed only at the start of the span, the empty span decoding to `""`; the parameter is a call-bounded borrow and the returned `string` retains nothing of the source. They complete the span-destination encode pair (`GetByteCount`/`GetBytes(s, destination)`) in the decode direction, on both byte-span families per the #1358 mirroring precedent. The span-**source** encode overloads are the mirror on the input side: identical contract to the string forms over the span's code-point range (`GetByteCount` and `GetBytes` agree; the empty span encodes to zero bytes; the same strict fault model, `FormatException` possible only for `ASCII`), equivalent to calling the string overload on `s.ToString()` minus the materialization — the produced bytes retain nothing of the source, and `ReadOnlyStringSpan` is not borrow-bearing ([§3.3](#sec-readonlystringspan)), so the parameter imposes no lifetime restriction.

<a id="sec-readonlystringspan"></a>
### 3.3 `ReadOnlyStringSpan`

A read-only view over a contiguous range of code points in a `string`. The natural companion to `string.AsSpan()` for zero-allocation substring operations — tokenization, parsing, prefix/suffix checks — without materializing intermediate string instances.

```bezel
public record ReadOnlyStringSpan(Source: string, Start: int, Length: int)

  public property [i: int]: char
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Source[Start + i];
    end;
  end;

  public pure property IsEmpty: bool => Length = 0;

  // Range construction
  public pure method Slice(start: int, length: int): ReadOnlyStringSpan =>
    new ReadOnlyStringSpan(Source, this.Start + start, length);
  public pure method Slice(start: int): ReadOnlyStringSpan =>       // from offset to end
    Slice(start, Length - start);
  public pure method Slice(r: Range<int>): ReadOnlyStringSpan;      // — Range form: normalized
                                                                    //   against Length (spec §16.3), then
                                                                    //   equivalent to Slice(start, length);
                                                                    //   §4.14's normalization contract

  // Search by char
  public pure method IndexOf(value: char): int;
  public pure method Contains(value: char): bool;

  // Search by substring (span and string forms)
  public pure method IndexOf(value: ReadOnlyStringSpan): int;
  public pure method IndexOf(value: string): int;
  public pure method Contains(value: ReadOnlyStringSpan): bool;
  public pure method Contains(value: string): bool;

  // Prefix / suffix (span and string forms)
  public pure method StartsWith(prefix: ReadOnlyStringSpan): bool;
  public pure method StartsWith(prefix: string): bool;
  public pure method EndsWith(suffix: ReadOnlyStringSpan): bool;
  public pure method EndsWith(suffix: string): bool;

  // Equality (span and string forms)
  public pure method SequenceEqual(other: ReadOnlyStringSpan): bool;
  public pure method SequenceEqual(other: string): bool;
  public pure method SequenceEqual(other: ReadOnlySpan<char>): bool;      // — the buffer-backed char span (§4.14)
  public pure method SequenceEqual(other: ReadOnlyArraySpan<char>): bool; // — the array-backed char span (§4.15)

  // Bulk copy
  public method CopyTo(destination: Span<char>);    // copy chars to destination;
                                                     //   lengths must match, else
                                                     //   ArgumentException (the §4.14 rule)

  // Materialization
  public pure method ToString(): string;
end ReadOnlyStringSpan;
```

A `ReadOnlyStringSpan` is an **ordinary record, not a borrow-bearing one**: its only reference-typed field is a `string`, which is runtime-managed, and language spec [§9.7](Bezel_Language_Spec.md#sec-records-and-disposal) classifies a record whose only reference-typed fields are runtime-managed as *not* borrow-bearing. None of the borrow-lifetime restrictions apply — the span may sit in fields, cross `await`, and outlive any scope, because the `Source` reference itself keeps the string alive ([§4.6](Bezel_Language_Spec.md#sec-string)): as long as anything reachable from the program holds the string, the span remains valid.

`ReadOnlyStringSpan` is purely user-level Bezel — no compiler intrinsic. The record machinery, borrow lifetime tracking, and indexer dispatch are all standard language features.

**Why a separate type from `ReadOnlySpan<char>`.** `ReadOnlySpan<char>` (stdlib [§4.14](#sec-owned-buffers-and-spans)) views into an `OwningBuffer<char>` — a contiguous char buffer with each slot occupying `sizeof(char)` bytes. A `string`'s internal storage is adaptive (1, 2, or 4 bytes per code point per language spec [§4.6](Bezel_Language_Spec.md#sec-string)), so a string-backed view cannot share storage with a buffer-backed view. Two source types would force `ReadOnlySpan<T>`'s `Buffer` field to be polymorphic over source type, which would either change the existing record's shape — a breaking change — or require a general source-polymorphism mechanism that is not introduced. The separate `ReadOnlyStringSpan` type leaves `ReadOnlySpan<char>` unchanged and makes the source visible at API boundaries ("does this method want a buffer-backed view or a string-backed view?").

**Indexer access.** `span[i]` returns the code point at position `Start + i` in the source string, by delegating to the string's indexer (which dispatches on the string's internal storage width). The per-access overhead is small — one range check, one width-dispatched read — and the call site never sees the dispatch. For parsing work where the per-element cost is dominated by compare/branch logic, the overhead is negligible.

**Searches.** `IndexOf(value: char)` walks the span's range, returning the first index at which the code point matches `value`, or `-1` if none. `Contains(value: char)` returns `IndexOf(value) >= 0`.

`IndexOf(value: ReadOnlyStringSpan)` searches for the first occurrence of `value` as a contiguous substring. The algorithm is left to the implementation — naïve scan, Knuth-Morris-Pratt, Boyer-Moore are all admissible. The return is the start index of the match in the receiver, or `-1` if not found. `Contains(value: ReadOnlyStringSpan)` returns `IndexOf(value) >= 0`.

For convenience when comparing against string literals, `IndexOf(value: string)` and `Contains(value: string)` are also provided. They are equivalent to calling the `ReadOnlyStringSpan` overload after `value.AsSpan()` — `span.Contains("Content-Type:")` parses naturally without the user writing the explicit `.AsSpan()`. No allocation happens for the conversion; it is record construction only.

**Prefix and suffix.** `StartsWith(prefix)` returns `true` if the prefix matches the leading portion of the span; `EndsWith(suffix)` is the trailing analog. Both compare code-point-by-code-point — no Unicode normalization, no case folding. For locale-aware or Unicode-equivalent comparison, users go through the dedicated string-comparison APIs ([§9.6](#sec-string)). Each method has both a `ReadOnlyStringSpan` and a `string` overload, mirroring the search methods above.

**Equality.** `SequenceEqual(other)` returns `true` if the two spans have the same length and matching code points at every position. The comparison is pairwise; no normalization or case folding. A `SequenceEqual(other: string)` overload is provided for convenience.

**Empty needles and empty receivers.** The empty span or string is a valid needle, with the same vacuous-match conventions as the string family ([§9.6](#sec-string)): `IndexOf` of an empty needle returns `0`; `Contains`, `StartsWith`, and `EndsWith` of an empty needle return `true`; `SequenceEqual` against an empty operand is `true` iff the receiver is itself empty (the length rule above). A **zero-length receiver** is likewise well-defined: it reads no source code point, every non-empty needle is not found (`IndexOf` returns `-1`; `Contains`/`StartsWith`/`EndsWith` return `false`), and the empty needle matches per the conventions above.

**Materialization.** `ToString()` returns a `string` containing the span's code points. If the span covers the entire source string (`Start = 0` and `Length = Source.Length`), implementations may return the source string directly (no copy); otherwise materialization is required (a fresh string is allocated). This is the way to "escape" a span into a plain string when the caller needs to retain the chars beyond the source's lifetime.

**Iteration.** `for var c in span do ... end` is admitted. The desugaring is a direct indexed loop over the span's range, parallel to the buffer-backed span rule of language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for). The iteration variable's type is `char` and the loop body sees a copy of each code point.

**Construction.** Callers construct `ReadOnlyStringSpan` via the `AsSpan()` family on `string` ([§9.6](#sec-string)) rather than via the constructor directly. The direct constructor is admitted but is rarely needed — `s.AsSpan()` and its overloads cover the common cases.

<a id="sec-readonlyasciibytespan"></a>
### 3.4 `ReadOnlyAsciiByteSpan` (retired)

Retired. There is no `ReadOnlyAsciiByteSpan`, and `string` has no `AsAsciiByteSpan()`/`TryAsAsciiByteSpan(out)` surface. For one-shot byte access to a string's contents, `Encoding.UTF8.GetBytes(s)` is the always-correct allocating form; the zero-per-call-allocation form — for **all** strings, ASCII or not — is `Encoding.GetByteCount(s)` plus `GetBytes(s, destination: Span<byte>)` into a caller-reused `OwningBuffer<byte>` ([§3.2](#sec-encoding)); byte-span-consuming APIs are fed with the array's or buffer's own span (`bytes.AsReadOnlySpan()`, `buf.AsReadOnlySpan()`). At the FFI boundary the carriers for string bytes are `CString`/`RawBytes` ([§10](#sec-ffi-boundary-types)) — a borrow-bearing record has no C-compatible form (language spec [§32.5](Bezel_Language_Spec.md#sec-ffi-compatible-types) admits only the §32.13 C-compatible records, which spans are not). The section number is retained so later-section citations stay stable; the prior definition and the decision record are in the v0.188 changelog entry ([`Bezel_Changelog.md`](Bezel_Changelog.md)).

---

<a id="sec-system-collections-generic"></a>
## 4. `System.Collections.Generic` (auto-imported)

The collection types. **Auto-imported:** this module is on language spec [§2.3](Bezel_Language_Spec.md#sec-always-available-names)'s auto-import list, so every name below resolves in every file with no `import` — the same standing as `System`, `System.Text`, and [§5](#sec-system-collections-linq). §2.3's row states the bare-name cost in full.

The language separates **borrowing** collections (store references the caller continues to own; element disposal is the caller's responsibility) from **owning** collections (own their elements; element disposal is the collection's responsibility). Each kind has its own concrete class and its own interfaces; the two kinds share read-only iteration interfaces (`IEnumerable<T>`, `IReadOnlyCollection<T>`, `IReadOnlyList<T>`) because reading produces borrows in either case.

The naming convention (#1372): borrowing collection types use their plain names (`Array<T>`, `LinkedList<T>`, etc.); owning collection types are prefixed `Owning…` (`OwningArray<T>`, `OwningLinkedList<T>`, etc.) — the prefix names the **element** axis ("a collection that *owns its elements*"), lexically distinct from the `owned` qualifier, which names the **container** axis ("a value *you* own"). `owned OwningArray<Dog>` therefore reads exactly as intended: an owning array that you own; a bare `OwningArray<Dog>` is a borrow of someone else's owning array. Interface members of the owning family carry the standard `I` prefix (`IOwningList`, `IOwningCollection`, `IOwningEnumerable`, `IOwningEnumerator`, and the async pair). The owning types constrain `where T: class` (and, for `OwningDictionary<K, V>`, `where V: class`) because ownership only makes sense for class-typed elements — with one recorded exception: the storage primitive `OwningBuffer<T>` admits value-typed `T` with plain value slots ([§4.14](#sec-owned-buffers-and-spans); spec §14.4 names it), its `Owning` naming the storage axis.

**Sendability** ([§1](#sec-sendability-statements); language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)). A **borrowing** collection (`Array<T>`, `LinkedList<T>`, `Set<T>`, `Stack<T>`, `Queue<T>`, `Dictionary<K, V>`, `SortedDictionary<K, V>`, `SortedSet<T>`) is sendable iff its element category (spec §28.11) is sendable-value, `string` or `Type` — copy and shared-immutable slots; for the maps, the key's and the value's categories both. At class elements a borrowing collection carries borrows and is not sendable. An **owning** collection is sendable iff its element type is a sendable class type — its state is wholly owned. `OwningDictionary<K, V>`'s element type is `V`. Its keys are held as a borrowing map's are (spec §28.11), so the borrowing condition applies to them as well: `K`'s category is sendable-value, `string` or `Type`. `OwningDictionary<string, Session>` is sendable at a sendable `Session`. At a class- or interface-typed `K` the map carries borrows and is not sendable, whatever `K`'s own sendability. `OwningBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)) is sendable iff its element type is a sendable class type or a sendable value type: its storage is part of the instance and crosses with it (`OwningBuffer<double>` is sendable; a buffer of a borrow-bearing record is not), and an address `DangerousPointer()` has returned stays with the origin domain under language spec [§32.10](Bezel_Language_Spec.md#sec-boundary-contract)'s cross-domain bullet. `Dictionary<string, int>` and `Set<Guid>` are therefore sendable classes, admissible as `Locked<T>` interiors ([§6.11](#sec-locked-and-rwlocked)); `Array<Session>` (class elements) is not.

**A retained comparer is a borrow.** The `comparer` constructors of `Dictionary<K, V>`, `OwningDictionary<K, V>`, `Set<T>` and `OwningSet<T>` ([§4.8](#sec-dictionaryk-v)–[§4.10](#sec-sett-and-ownedsett)) take a plain parameter and retain it: every later lookup consults it, so the collection holds a borrow of the comparer for its life. The statements above classify the type; a collection constructed with a comparer carries language spec [§28.3](Bezel_Language_Spec.md#sec-borrow-lifetime-rules)'s domain clause as recorded trust, the **comparer-held clause**: while it retains the comparer it is not transferred to another domain, and not consumed into a construction other domains reach (a `Locked<T>` or `RwLocked<T>` interior, a `Shared<T>` payload). A violation is the data race the clause names; checked disposal mode cannot catch it, and the trust-admission report (language spec §28.3) names the constructor call. The comparerless constructors retain nothing.

<a id="sec-value-copy-members"></a>
**Value-copy members are stores.** A member that places element values taken from its receiver or from an argument into a fresh collection it returns, or into a collection, buffer or array the caller already holds, is a **value-copy member**, and a call to one is judged as a store of each placed value into that destination, at the call and over the declared signature. Which members are value-copy members is read off their signatures by language spec [§28.11](Bezel_Language_Spec.md#sec-borrowing-collections)'s *Value-copy members* — three shapes over this section's collections, spans and buffers and over [§5](#sec-system-collections-linq)'s operators: an `owned` collection result, a borrowing-family or span receiver taking `IEnumerable<T>`, a read-only span or a `T` with no return, and a written span argument — which also names the four members a shape reads that copy nothing: `Set<T>.IntersectWith` and `ExceptWith` ([§4.10](#sec-sett-and-ownedsett)), which only remove, and `Span<T>`'s class-typed `MoveFrom` and `MoveTo` ([§4.14](#sec-owned-buffers-and-spans)), which move. No declaration carries a marker: the signature is the register. The single-element stores — `Add`, `Insert`, `LinkedList<T>`'s `AddFirst`, `AddLast`, `AddBefore` and `AddAfter`, `Push`, `Enqueue`, `TryAdd`, `GetOrAdd`, `Counter<K>.Increment` and an indexer-set, a span view's included, which writes the buffer or array the span views — are stores in their own right under the slot policy's store row (language spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy)), whatever they return.

<a id="sec-collection-equality-search"></a>
**Equality search at an element type with no equality.** The members that compare an argument with stored elements by `Equals` are the equality search: `Contains` and `Remove(item)` of `ICollection<T>` and of its implementations here at an unconstrained element type (`Array<T>`, `LinkedList<T>`), `Contains` of `Stack<T>` and `Queue<T>`, `Array<T>.IndexOf` and `LastIndexOf`, `LinkedList<T>.Find`, and the span family's `IndexOf`, `Contains` and `SequenceEqual` ([§4.14](#sec-owned-buffers-and-spans), [§4.15](#sec-arrayspant-readonlyarrayspant)). Their signatures are unconstrained, and they are conditionally-admitted members (language spec [§14.2](Bezel_Language_Spec.md#sec-conditionally-admitted-members)): a call is admitted only where the element type, nullability stripped, has a member for the comparison language spec [§9.2](Bezel_Language_Spec.md#sec-equality-through-a-type-parameter) calls the first kind, `Equals` with an argument of that type. It has none exactly where the same comparison between two values of the type draws §9.2's, §35.7's or §15.1.2's use-site error, as for a record or choice that is neither auto-hashable nor declares an `Equals` of its own, or an inline array of one, or is spec [§22.6](Bezel_Language_Spec.md#sec-overloading)'s ambiguity, as for a class declaring `Equals(other: I)` for an interface `I` it implements but no `Equals` at its own type (spec §9.2, §12.2). There the call is a compile error, `x in xs` (language spec [§17.5](Bezel_Language_Spec.md#sec-the-in-operator)) and a method reference formed over the member (language spec [§27.6](Bezel_Language_Spec.md#sec-method-references)) included, and the collection type is unaffected: `Array<R>` is well-formed at such an `R`, and only its equality search is refused. Inside a generic body whose element type involves a type parameter, the call is the body's own comparison, read at each instantiation of the declaration that declares the parameter exactly as the same `Equals` written on that element type would be (language spec §14.2 rule 2, §9.2, [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties) rule 2); a user `ICollection<T>` implementation called through its own type is its own declaration, under §3.6 rule 2, and only a call through `ICollection<T>` takes this rule. A value kind's member for the first kind is its own-typed `Equals`, declared or generated, its `Object` slot, and no value kind declares an `Equals` at `Object?` (spec §9.2), so the search never boxes a value that holds borrows. The remedy is to declare equality on the element type, which makes it **hashable**, or to search with a predicate (`xs.Any(lambda(x) => x.Id = id)`, [§5](#sec-system-collections-linq)). The owning collections' members of the same names fire the rule only at that ambiguity, since their `where T: class` element type otherwise always has the member; `SortedSet<T>` compares by `CompareTo` ([§4.18](#sec-sorted-set)), and `Set<T>`, `Dictionary<K, V>` and §5's hashable block require **hashable**: `Set<T>` and `Dictionary<K, V>` by their structural check at every naming of the instantiation ([§4.8](#sec-dictionaryk-v), [§4.10](#sec-sett-and-ownedsett)), §5's block by constraint.

<a id="sec-ireadonlycollectiont-ireadonlylistt-and-ilistt"></a>
<a id="sec-borrowing-collection-interfaces"></a>
### 4.1 Borrowing collection interfaces

```bezel
public interface IReadOnlyCollection<out T>: IEnumerable<T>
  pure readonly property Count: int;
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
end ICollection;

public interface IList<T>: IReadOnlyList<T>, ICollection<T>
  property [i: int]: T;
  method Insert(index: int, item: T);
end IList;
```

The four interfaces form the borrowing collection family. `IReadOnlyCollection<T>` and `IReadOnlyList<T>` are covariant in T (`out T`); `ICollection<T>` and `IList<T>` are invariant because they admit mutation through the type parameter.

`IsReadOnly` is `false` for every standard collection implementing `ICollection<T>` (`Array<T>`, `LinkedList<T>`, `Set<T>`, `Dictionary<K, V>`; `Stack<T>`/`Queue<T>` conform only to `IReadOnlyCollection<T>` (#1354) and have no `IsReadOnly` member). The standard library currently ships no read-only `ICollection<T>` implementation; the member exists for user-defined read-only implementations, which return `true` and raise `InvalidOperationException` from the mutating methods.

**`Remove`'s result** (stated here once, for both families). Every `Remove` that returns `bool` — `ICollection<T>.Remove(item)` and `IOwningCollection<T>.Remove(item)` with their implementations, the keyed `Remove(key)` of `Dictionary<K, V>`, `OwningDictionary<K, V>`, `SortedDictionary<K, V>` and `Counter<K>`, `SortedSet<T>.Remove(item)`, `OwningSlotMap<T>.Remove(handle)`, and the entry form `Remove(entry)` of [§4.8](#sec-dictionaryk-v) — returns `true` iff it removed an element, and `false`, leaving the collection unchanged, when nothing matched: no equal element, an absent key, a stale or vacated handle. An equality search removes the **first** equal element in iteration order where the collection admits duplicates (`Array<T>`, `OwningArray<T>`, `LinkedList<T>`, `OwningLinkedList<T>`), under the type's standard equality as `Contains` and `IndexOf` read it; an owning collection disposes the element it removed and disposes nothing on `false`. A `// false if absent` comment on a declaration restates this rule.

None of the four interfaces extends `IDisposable`, so `Dispose()` is not a member of a value typed at one of them: `il.Dispose()` and the reference `il.Dispose` over `il: IList<T>` are compile errors (spec §8.7, §27.6), and an owned value held at such a type is disposed at its discharge points — scope end, `using var`, the discard `_ := il;` — by dispatch to the runtime type (spec §17.13). Element disposal is never performed by or through a borrowing collection (spec §28.6): a holder that owns its elements uses the owning family, whose `Dispose()` cascades; selective cleanup takes elements out (`Drain()`, `Pop*`, `move coll[i]`) into owned bindings first.

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

The `where T: class` constraint expresses that owning collections are meaningful only for elements carrying a disposal obligation, and the language spec ([§14.4](Bezel_Language_Spec.md#sec-owned-as-a-generic-argument)) states the enforced rule: element types are **class and interface types** (nullable-underlying admitted). **Value-typed elements are rejected at the instantiation site**: a value element has no disposal obligation, so `OwningArray<int>` says nothing `Array<int>` doesn't; use `Array<T>` or `OwningBuffer<T>` for value elements. Runtime-managed reference types (`string`, function values) are likewise rejected. Code that wants a borrowing collection uses the unprefixed name (`Array<…>`).

<a id="sec-borrowing-and-owning-collections-relate-via-iiterator-interfaces"></a>
<a id="sec-how-the-borrowing-and-owning-hierarchies-relate"></a>
### 4.3 How the borrowing and owning hierarchies relate

The two hierarchies share `IEnumerable<T>`, `IReadOnlyCollection<T>`, and `IReadOnlyList<T>` as common ancestors. A function that wants to read elements without taking ownership accepts one of these interfaces; both borrowing and owning collections of T conform. The sequence collections are also collection-literal targets, and each **declares** it (language spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol); the pair is at [§2.19](#sec-static-abstractions)): each conforms at itself — `Array<T>`, `LinkedList<T>`, `Set<T>`, `Stack<T>`, and `Queue<T>` to `ICollectionLiteralTarget` (`Array<T>` to `ICollectionLiteralTarget<Array<T>, T>`, and so on), and `OwningArray<T>`, `OwningLinkedList<T>`, `OwningSet<T>`, `OwningStack<T>`, and `OwningQueue<T>` to `IOwningCollectionLiteralTarget` (`OwningArray<T>` to `IOwningCollectionLiteralTarget<OwningArray<T>, T>`, and so on) — the conformance is in each class's declaration below and the factory is a listed member of each, whose body is the literal's own lowering (language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)). Language spec §15.4's target list is derived from these declarations. The dictionaries take the dictionary-literal form only and conform to neither.

```bezel
public method ReportEach<T>(items: IReadOnlyList<T>) where T: class
  for var c in items do Console.WriteLine(c); end;   // Object overload -> ToString();
                                                     //   `where T: class` exposes only Object's surface (spec §14.2)
end ReportEach;

// Accepts both:
ReportEach(myArray);          // Array<Connection> → IReadOnlyList<Connection> ✓
ReportEach(myPool);            // OwningArray<Connection> → IReadOnlyList<Connection> ✓
```

The two hierarchies do *not* share mutating interfaces. `IList<T>` (borrowing) and `IOwningList<T>` (owning) are unrelated types with no subtyping; code that abstracts over "any mutable list" must pick one. This is intentional — the semantic of `Add` differs between the two (borrow-taking vs. owned-taking), so there is no useful single abstraction over both.

For draining iteration that transfers ownership, the owning collections expose `Drain()` returning `IOwningEnumerable<T>` (declared in [§4.13](#sec-owning-enumeration-interfaces)).

Every collection's element semantics — what `Add`, `Remove`, `Clear`, the indexer, iteration, and `Dispose` do with the elements — derive from the element slot policy of language spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy): the collection's family and the element type's category determine the slot kind, and the slot kind determines every element operation. The per-type sections below present that table for each concrete type; they introduce no element semantics of their own. The storage primitive embodying the owning column is `OwningBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)).

**Empty-collection egress.** On an empty collection, `Pop`, `PopFirst`, `PopLast`, `Dequeue`, and `Peek` raise **`InvalidOperationException`** — that is what *raises if empty* means wherever the per-type sections below write it ([§4.5](#sec-ownedarrayt), [§4.7](#sec-ownedlinkedlistt), [§4.11](#sec-stackt-ownedstackt-queuet-and-ownedqueuet)), and it is the type [§4.17](#sec-sorted-dictionary)'s `FirstKey`/`LastKey`, [§4.18](#sec-sorted-set)'s `First`/`Last`, and [§5](#sec-system-collections-linq)'s `First`/`Last`/`Single` already name for the same condition. Taking from an empty collection is a state violation, [§1](#sec-failure-signal-rule)'s band 1; the `Try` twins (`TryPop`, `TryPopFirst`, `TryPopLast`, `TryDequeue`, `TryPeek`) are the branch-shaped tier and return `false` instead. **`PopAt(index)` is governed by its index rather than by emptiness**: an `index` outside `0..<Count` raises `IndexOutOfRangeException`, the document-wide bounds fault, which on an empty receiver is every index — so the `= PopAt(0)` and `= PopAt(Count - 1)` glosses on `PopFirst`/`PopLast` name the position each selects, not the fault it raises.

**`Clear()`'s disposal order.** An owning collection's `Clear()` disposes the elements it holds in the order its own `Dispose()` does — reverse storage order for the ordered families (`OwningArray`, `OwningLinkedList`, `OwningStack`, `OwningQueue`, and — slot order being its storage order — `OwningSlotMap`, [§4.21](#sec-owning-slot-map); spec [§28.11](Bezel_Language_Spec.md#sec-collection-element-ownership) states it per family), implementation-defined for the unordered (`OwningDictionary`, `OwningSet`) — and keeps the emptied collection (language spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy)). `Remove`, `RemoveAt`, and an indexer-set dispose one element and raise no ordering question. A borrowing collection's `Clear()` disposes nothing ([§4.1](#sec-borrowing-collection-interfaces)).

<a id="sec-arrayt"></a>
### 4.4 `Array<T>`

Borrowing resizable array. Stores references that the caller continues to own (or value-typed elements directly).

```bezel
public class Array<T>: IList<T>, ICollectionLiteralTarget<Array<T>, T>
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Array<T>;   // §2.19 witness; the literal's
                                                                                                //   own lowering (spec §15.4)
  public constructor();
  public constructor(capacity: int);                     // empty (Count = 0), storage reserved;
                                                          //   capacity < 0: ArgumentOutOfRangeException (#1396)
  public constructor(size: int, fill: T);                // Count = size, every slot = fill (#1375);
                                                          //   size < 0: ArgumentOutOfRangeException (#1396)

  public pure readonly property Count: int;
  public pure readonly property Capacity: int;
  public property [i: int]: T;

  public method Add(item: T);
  public method AddRange(items: IEnumerable<T>);
  public method Insert(index: int, item: T);
  public method RemoveAt(index: int);
  public method Remove(item: T): bool;
  public method Clear();
  public method Contains(item: T): bool;
  public method IndexOf(item: T, startIndex: int := 0): int;
                                                // first occurrence at or after startIndex, or -1
                                                //   0 <= startIndex <= Count required,
                                                //   IndexOutOfRangeException otherwise;
                                                //   startIndex = Count is legal and returns -1
  public method LastIndexOf(item: T): int;      // last occurrence, or -1. Equality is
                                                //   Equals for both, as IndexOf

  public method Sort(comparer: (T, T) -> int);  // in place
  public method Reverse();                       // in place

  public method Sorted(comparer: (T, T) -> int): owned Array<T>;  // fresh sorted copy
  public method Reversed(): owned Array<T>;                       // fresh reversed copy

  public method ToArray(): owned Array<T>;      // fresh heap-array copy

  // Span views
  public method AsSpan(): ArraySpan<T>;                          // full array, writable
  public method AsSpan(start: int): ArraySpan<T>;                // from offset to end
  public method AsSpan(start: int, length: int): ArraySpan<T>;
  public method AsSpan(r: Range<int>): ArraySpan<T>;             // parallels Range slicing

  public method AsReadOnlySpan(): ReadOnlyArraySpan<T>;
  public method AsReadOnlySpan(start: int): ReadOnlyArraySpan<T>;
  public method AsReadOnlySpan(start: int, length: int): ReadOnlyArraySpan<T>;
  public method AsReadOnlySpan(r: Range<int>): ReadOnlyArraySpan<T>;

  // Range slicing (language spec §15.6): `arr[r]` — arr[a..b], arr[..b], arr[a..], arr[..] —
  // is the compiler-recognized equivalent of this method:
  public method Slice(r: Range<int>): owned Array<T>;   // fresh array; the range's values, or copies of the element borrows

  // Natural order: conditionally-admitted members (language spec §14.2) — the clause
  // names the array's own `T` and is the member's condition.
  public method Sort() where T: IComparable<T>;                    // in place
  public method Sorted(): owned Array<T> where T: IComparable<T>;  // fresh sorted copy
end Array;

// Natural-order sorting is declared on `Array<T>` itself, NOT as an extension:
// spec §26.5 rule 1 commits instance resolution by *name* — of any arity — so an
// extension member spelled `Sort`/`Sorted` would never be consulted on an `Array<T>`
// receiver. These are arity-0 overloads of the names above (§22.6), and CONDITIONALLY
// ADMITTED members in the sense of language spec §14.2: the `where` clause on the
// enclosing type's `T` is the condition, decided after substitution from the
// receiver's type argument and reported at the use (the call, or the formation of a
// method reference over it, spec §27.6), never at the declaration. §1's *Conditional
// members* row lists every such member of this document. `OwningArray<T>` declares
// the same parameterless `Sort()` in its own block below (the compiler's owning-side
// gate landed with #3325).
```

A collection literal `[1, 2, 3]` constructs an `Array<T>` when the target type is `Array<T>`; it constructs an inline array `[N]T` when the target is an inline array. See language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals) for the contextual-typing rules.

`Array<T>` stores borrows: when the element type is a class, the elements are references whose ownership remains with whoever passed them in. The array's `Dispose()` is a no-op — `Array<T>` is trivial-dispose (language spec [§28.9](Bezel_Language_Spec.md#sec-runtime-managed-types)): the backing storage is part of the instance's single allocation, released with it by the compiler-inserted free at the array's owner's release point — and the elements are not disposed. To have elements disposed, use `OwningArray<T>` ([§4.5](#sec-ownedarrayt)) — element disposal never happens through the borrowing type (spec §28.6).

`Reverse`/`Sort` versus `Reversed`/`Sorted` follow a uniform naming convention: the imperative-verb form (`Reverse`, `Sort`) mutates the receiver in place and returns nothing; the past-participle form (`Reversed`, `Sorted`) leaves the receiver unchanged and returns a fresh `owned Array<T>` with the elements in the new order. The result's ownership mirrors `ToArray(): owned Array<T>` — it is a borrowing array (a fresh backing buffer holding the same element borrows), so the caller is responsible for disposing the array itself, not its elements. `Sorted` comes in two forms on `Array<T>`: the comparer-taking method, and the parameterless natural-order overload declared above, admitted where `T: IComparable<T>` and checked at the call's instantiation. Neither can be supplied by an **extension**: [§26.5](Bezel_Language_Spec.md#sec-resolution) rule 1 commits instance resolution by *name* — of any arity — so an extension member spelled `Sort` or `Sorted` is never consulted on an `Array<T>` receiver. For an element type that is not `IComparable<T>`, pass a comparer; for a concrete element type the natural-order comparer is that type's own `CompareTo` (for example `int.CompareTo`). `OwningArray<T>` **declares** no copy-forms of its own — no `Reversed`, no `Sorted`, no `ToArray` — because no *owning* copy is expressible: an owning copy would either double-own each element (two arrays claiming the same owned instances) or drain the source (leaving it empty). That is a statement about the type's **declared surface**, not about what is reachable on a receiver of it. `Reversed`/`Sorted` are declared only on `Array<T>` (above) and on nothing `OwningArray<T>` conforms to, so neither resolves on an owning array. `ToArray` is different: [§5](#sec-system-collections-linq) declares it as an extension on `IEnumerable<T>`, which `OwningArray<T>` implements ([§4.13](#sec-owning-enumeration-interfaces)), and [§26.5](Bezel_Language_Spec.md#sec-resolution) rule 1 blocks an extension only where the receiver's type *declares* the name — so `pool.ToArray()` resolves (the module is auto-imported, [§5](#sec-system-collections-linq)) and is sound, returning a **borrowing** snapshot rather than an owning copy ([§4.5](#sec-ownedarrayt) states the form and its lifetime obligation). To reorder an `OwningArray<T>`, use the in-place `Reverse()`/`Sort()`; to move its elements into a new owning array, drain it (`Drain()`), which transfers ownership and empties the original. ([#3286](https://github.com/s8lap/bezel-lang/issues/3286).)

**Sorting contract.** (1) **Stability.** `Array<T>`/`OwningArray<T>` `Sort`/`Sorted` (both the comparer and comparerless forms) are **not guaranteed stable** — equal elements may be reordered. `OrderBy`, `OrderByDescending`, `ThenBy`, and `ThenByDescending` ([§5](#sec-system-collections-linq)) **are stable** — equal keys retain source order (`ThenBy`'s compositional semantics require it). (2) **Comparer contract.** A comparer (or `CompareTo`) must induce a **total order**; under an inconsistent comparer the result is an **unspecified permutation of the original elements** — memory-safe, no element lost or duplicated. (3) **A comparer that throws mid-sort** propagates the exception and leaves the collection holding **exactly the original elements in unspecified order** (none lost, none duplicated). For `OwningArray<T>` this is the Appendix C disposed-exactly-once guarantee: every owned slot remains present and singly-held on the throw edge. (4) **Re-entrancy.** Structurally mutating a collection from inside its own sort comparer raises `InvalidOperationException` (the loud-failure discipline of language spec [§29.12](Bezel_Language_Spec.md#sec-concurrent-modification), extended from enumerator invalidation to sort callbacks).

`AsSpan()` and its overloads return an `ArraySpan<T>` (writable view) over a contiguous range of the array; `AsReadOnlySpan()` returns a `ReadOnlyArraySpan<T>`. These views are non-allocating record constructions and admit `Slice`, search, equality, and bulk-mutation methods. See [§4.15](#sec-arrayspant-readonlyarrayspant) for the full description; note that the mutation surface differs from `Span<T>`'s — it is uniform `Fill`/`CopyFrom`/`Reverse` for all T, without the class/value split that `Span<T>` requires, because `Array<T>` slots store borrows rather than owned values. `AsSpan` returns the writable form by default (matching `Array<T>`'s mutable nature); callers wanting read-only access either call `AsReadOnlySpan` directly or call `.AsReadOnly()` on the writable form. The four overload signatures of each form accept range parameters in the same style as `string.AsSpan()`: no-arg (full used range, `0..<Count`), `(start)` (from offset to end), `(start, length)`, or `(r: Range<int>)`. Bounds-check semantics follow the standard Bezel convention: `IndexOutOfRangeException` if `start` or `length` is out of range against the array's `Count`.

<a id="sec-ownedarrayt"></a>
### 4.5 `OwningArray<T>`

Owning resizable array. Owns its elements; disposal cascades through them.

```bezel
public class OwningArray<T>: IOwningList<T>, IOwningEnumerable<T>, IOwningCollectionLiteralTarget<OwningArray<T>, T> where T: class
                                     // IOwningEnumerable: stated — §4.13/spec §29.2's both-implemented rule presupposed it
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningArray<T>;   // §2.19 witness: the
                                                                                                        //   transferred array is the result
  public constructor();
  public constructor(capacity: int);

  public pure readonly property Count: int;
  public pure readonly property Capacity: int;

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
  public method Clear();                                  // disposes every element: a spec §28.4 multi-value pass —
                                                          //   a throwing disposer does not stop it; the array is left empty
  public method Contains(item: T): bool;
  public method IndexOf(item: T): int;

  public method Sort(comparer: (T, T) -> int);          // comparer takes borrows
  public method Sort() where T: IComparable<T>;         // natural order; a conditionally-
                                                        //   admitted member (language spec
                                                        //   §14.2), as Array<T>.Sort() is
  public method Reverse();

  // Drain methods — transfer ownership of elements out of the collection
  public method PopAt(index: int): owned T;             // removes and returns owned
  public method PopFirst(): owned T;                     // = PopAt(0); raises if empty
  public method PopLast(): owned T;                      // = PopAt(Count - 1); raises if empty
  public method TryPopFirst(out owned item: T?): bool;   // (#1336): the false path assigns null (§22.3)
  public method TryPopLast(out owned item: T?): bool;
  public method Drain(): IOwningEnumerable<T>;             // iteration drains the source
end OwningArray;

// No `OwningArray<T>` extension supplies a default-comparer `Sort`. Spec §26.5
// rule 1 commits instance resolution by *name*, and `OwningArray<T>` declares
// `Sort` above, so an extension member of that name is never consulted. Callers
// pass a comparer, or call the parameterless `Sort()` declared above, admitted
// where `T: IComparable<T>`.
```

`OwningArray<T>` owns each element. `Add`, `Insert`, and the indexer setter take ownership; `RemoveAt`, `Remove`, `Clear`, and the array's own `Dispose()` dispose the affected elements. `Sort`'s comparer parameter takes borrows of T (the comparer doesn't consume; it inspects).

The indexer getter produces a borrow of the slot's owned element; the borrow is valid until the slot is mutated (by indexer set, RemoveAt, Clear, or any operation that disposes the slot's contents). A use of that borrow after the mutation is a mid-scope kill under the checked-disposal backstop (language spec [§28.2](Bezel_Language_Spec.md#sec-checked-disposal)), not statically tracked per slot (language spec [§28.11](Bezel_Language_Spec.md#sec-owning-collections)); re-take the borrow after the mutation, or take the element out first with `PopAt(i)`. To take an element out, use `PopAt(i)` (returns owned T and removes the slot) or `move arr[i] with fresh` (returns owned T and stores `fresh` in the slot in the same step; a bare `move arr[i]` is rejected, the slot being non-nullable — see language spec [§28.8.1](Bezel_Language_Spec.md#sec-the-move-expression)).

**The `TryPopAt` gap, recorded.** The `Pop*` surface is asymmetric against [§1](#sec-conventions)'s access tiers: the positional egress has a throwing form (`PopAt`) and no `Try` form, while its two convenience specializations (`PopFirst`/`PopLast`) have both. A coherent reading exists — an out-of-range index is the assert-shaped tier, a bug like the throwing indexer's — but the same argument applies to `PopFirst`/`PopLast` on an empty collection, and those *did* get `Try` forms. The surface currently asserts neither resolution; closing the gap means either declaring `TryPopAt(index: int, out owned item: T?): bool` (shape (a), the `when true` postcondition riding it per §1) or writing the band sentence that says positional egress is deliberately assert-only. Recorded as a gap with the reasoning on both sides, not closed.

`Drain()` returns an `IOwningEnumerable<T>` ([§4.13](#sec-owning-enumeration-interfaces)) whose iteration transfers ownership of each element to the consumer. `OwningArray<T>` itself implements `IOwningEnumerable<T>` — the both-implemented case whose disambiguation rule ([§4.13](#sec-owning-enumeration-interfaces), spec §29.2) exists for it: default `for` iteration stays borrowing, and the owning path is taken only at static type `IOwningEnumerable<T>`. `Drain()` mints a **fresh drain-view frame** over the array — a small caller-owned enumerable, never the array itself (superseding the return-the-receiver form: a `Drain()` call is an ordinary minting iterable-family production, spec [§28.3](Bezel_Language_Spec.md#sec-producers), and the `for` desugaring's `using var __source` disposes the minted frame at loop end — disposing a frame that *was* the collection would dispose the collection). Disposing the drain view does not dispose the array; the observable drain semantics are unchanged — elements leave the collection as they are yielded, and partial iteration leaves the remainder in place. A `for var x in arr.Drain() do ... end` loop has owned iteration variables and consumes the array's elements; the spec §28.3 discharge rules apply per iteration (an undischarged element auto-drops at the body's end with the must-use warning).

`OwningArray<T>` declares no `ToArray()`, and no **owning** copy of an owning array exists: such a copy would manufacture duplicate ownership of each element. To produce a fresh `OwningArray<T>` with the same elements, the source array must be drained — the original ceases to own its elements.

What *is* reachable, and is not an owning copy, is [§5](#sec-system-collections-linq)'s `ToArray()` extension on `IEnumerable<T>`: it resolves on an `OwningArray<T>` receiver because the type implements `IEnumerable<T>` ([§4.13](#sec-owning-enumeration-interfaces)) and declares no member of that name ([§26.5](Bezel_Language_Spec.md#sec-resolution) rule 1 blocks by declared name, and resolves on the receiver's compile-time type — an upcast to `IReadOnlyList<T>` or `IEnumerable<T>` reaches it identically). It returns an `owned Array<T>` — a **borrowing snapshot**: at a class element type `Array<T>` is a borrowing collection, so its slots hold borrows of the elements, sourced from this array, which continues to own them (language spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy)'s owned-slot row, *"borrow out (source: the collection)"*; [§5](#sec-system-collections-linq)'s materialization paragraph). No duplicate ownership is manufactured and no element is disposed twice; the caller's obligation is the array itself, never its elements. The obligation the snapshot carries is a **lifetime** one: this array must outlive every use of the snapshot, because disposing it — or `Clear`, `Remove`, `RemoveAt`, or an indexer-set on the slot — disposes the element a snapshot slot points at, after which a read through that slot faults in checked disposal mode ([§28.2](Bezel_Language_Spec.md#sec-checked-disposal)) and is undefined behavior unchecked. The same reachability and the same obligation hold for `ToSet()`, `ToDictionary(…)`, and every other [§5](#sec-system-collections-linq) operator applied to an owning receiver; the parallel ownership-preserving surface declared on `IOwningEnumerable<T>` is deferred (rationale [§18](Bezel_Design_Rationale.md#sec-things-deferred), *LINQ over owned-element collections*, which records that the borrowing form works today). ([#3286](https://github.com/s8lap/bezel-lang/issues/3286).)

Disposing an `OwningArray<T>` automatically disposes every element — the cascade is the type's contract.

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

`OwningArray<T>` composes over `OwningBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)) in pure Bezel; the standard library provides this implementation directly, and user code is welcome to inspect it and to extend it with extension blocks (language spec [§26](Bezel_Language_Spec.md#sec-extension-blocks)); like every class of this library that [§1](#sec-conventions)'s *Extensibility* row does not open, it admits no user subclass.

```bezel
public class OwningArray<T>: IOwningList<T>, IOwningEnumerable<T>, IOwningCollectionLiteralTarget<OwningArray<T>, T> where T: class
  private owned _storage: OwningBuffer<T>;
  private _count: int := 0;

  public constructor()
    this(8);
  end constructor;

  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningArray<T>
    return elements;                                   // the literal's temporary is the result (spec §15.4.1, owning form)
  end FromCollectionLiteral;

  public constructor(capacity: int)
    if capacity < 0 then
      throw new ArgumentOutOfRangeException("capacity");
    end;
    _storage := new OwningBuffer<T>((if capacity > 0 then capacity else 1));
  end constructor;

  public pure property Count: int => _count;   // `pure` is mandatory: §4.1's
                                               //   IReadOnlyCollection<T>.Count is
                                               //   pure-declared, and spec §22.15
                                               //   dispatch rule 1 makes dropping it
                                               //   at an implementation a compile error
  public pure property Capacity: int => _storage.Length;   // `pure` is mandatory: the
                                                //   declared surface above pure-declares
                                                //   Capacity, and the body's only call —
                                                //   OwningBuffer<T>.Length (§4.14) — is
                                                //   pure-declared, so spec §22.15 rule 6
                                                //   admits it

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
    var taken := move _storage[index];             // taken: T? (owned); the slot holds null (§28.8.1)
    for var j in index..<(_count - 1) do
      _storage[j] := move _storage[j + 1];          // shift down: take slot j+1 (it holds null afterwards),
                                                     //   store into slot j (null: nothing to dispose)
    end;
    _count := _count - 1;
    return taken!;                                   // i < _count guaranteed non-null
  end PopAt;

  public method RemoveAt(index: int)
    using var _ := PopAt(index);                  // dispose at scope end
  end RemoveAt;

  public method Clear()
    for var i in 0..<_count do
      var slot := _count - 1 - i;                  // reverse index order
      var item := move _storage[slot];             // item: T? (owned); the slot holds null
      _ := item;                                    // disposes it (slot < _count: non-null);
                                                     //   `T: class` has no `Dispose()` (spec §8.7)
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
      fresh[i] := move _storage[i];                  // take from the old slot (it holds null afterwards),
                                                      //   store into the fresh one
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

The implementation is straightforward Bezel: a class with an owned `OwningBuffer<T>` field, a count, and the standard ownership-transferring operations. Every slot take is the replacing `move` of [§28.8.1](Bezel_Language_Spec.md#sec-the-replacing-move) of the language spec: on the buffer's nullable slots the bare `move _storage[i]` takes the element and stores `null` in the same step, so no slot is ever observed empty-but-owning and nothing is left to restore.

`GrowTo` is the resize operation. It allocates a fresh buffer, moves every live element from the old buffer to the new one (each `move` leaving `null` behind), then disposes the old buffer. The old buffer's destructor sees only null slots and does no further work.

`Drain()`, `PopFirst()`, `PopLast()`, and the rest follow the same pattern: take from a slot via `move`, adjust `_count`. None of the operations require compiler-intrinsic knowledge of `OwningArray<T>` itself — the intrinsic surface is confined to `OwningBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)).

The implementation example above omits some methods for brevity; the production standard library version provides the full surface listed in the class declaration above, all built over the same `OwningBuffer<T>` primitive.

<a id="sec-linkedlistt"></a>
### 4.6 `LinkedList<T>`

Borrowing doubly-linked list. Use when frequent insertion/removal in the middle is required. (The name `List<T>` is reserved and unused: it commonly denotes a dynamic array — which `Array<T>` is here — so a linked structure under that name would mislead; `LinkedList<T>` makes the linked representation an explicit choice.)

```bezel
public class LinkedList<T>: ICollection<T>, ICollectionLiteralTarget<LinkedList<T>, T>
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned LinkedList<T>;   // §2.19 witness (spec §15.4)
  public constructor();
  public constructor(source: IEnumerable<T>);

  public pure readonly property Count: int;
  public pure readonly property FirstNode: LinkedListNode<T>?;
  public pure readonly property LastNode: LinkedListNode<T>?;
  public pure readonly property First: T?;
  public pure readonly property Last: T?;

  public method AddFirst(item: T): LinkedListNode<T>;
  public method AddLast(item: T): LinkedListNode<T>;
  public method AddBefore(node: LinkedListNode<T>, item: T): LinkedListNode<T>;
  public method AddAfter(node: LinkedListNode<T>, item: T): LinkedListNode<T>;
  public method RemoveFirst();
  public method RemoveLast();
  public method RemoveNode(node: LinkedListNode<T>);
  public method Find(item: T): LinkedListNode<T>?;
  // Plus the standard ICollection<T> operations: Add (appends at the tail,
  // = AddLast), Clear, Contains, Remove, IsReadOnly.
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
public class OwningLinkedList<T>: IOwningCollection<T>, IOwningCollectionLiteralTarget<OwningLinkedList<T>, T> where T: class
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningLinkedList<T>;   // §2.19 witness
  public constructor();

  public pure readonly property Count: int;
  public pure readonly property FirstNode: LinkedListNode<T>?;       // borrow if non-null
  public pure readonly property LastNode: LinkedListNode<T>?;
  public pure readonly property First: T?;                           // borrow
  public pure readonly property Last: T?;

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
  public method TryPopFirst(out owned item: T?): bool;   // (#1336)
  public method TryPopLast(out owned item: T?): bool;
  public method Drain(): IOwningEnumerable<T>;
  // Plus the remaining IOwningCollection<T> operations, elided per §1's marker
  //   convention: Add(owned item) — appends at the tail, = AddLast —
  //   and IsReadOnly.
end OwningLinkedList;
```

`LinkedListNode<T>` (declared in [§4.6](#sec-linkedlistt)) is reused. On `OwningLinkedList<T>`, the node's `Value` property returns a borrow of the node's element; the node still owns the element until the node is removed. To take an element out, use `RemoveNode` (which disposes), `PopFirst` / `PopLast` (transfer ownership to caller), or `Drain()`.

The `move` operator does not extend to linked-list nodes in this version — removing a value from a specific node would leave the node in an inconsistent state, and the surrounding-node-pointer integrity makes a slot exchange impractical. Future revisions may add a node-level move form.

**There is no source constructor.** `OwningLinkedList<T>` declares `constructor()` and no populate-from-source form, deliberately, and the absence is the answer rather than a gap. A borrowing source has no elements to give: a borrowing collection never owns its elements and never disposes them ([§4.1](#sec-borrowing-collection-interfaces); spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy)), so copying out of an `IEnumerable<T>` into an owning list would mint a second owner for every element — the duplicate ownership [§4.5](#sec-ownedarrayt) refuses for `OwningArray<T>.ToArray()`, for the same reason. A source that *does* own its elements is **drained**, not copied, and the original ceases to own them; the spelling already exists. Build an owning list from owned elements with a collection literal (`using var l: OwningLinkedList<Connection> := [Connect("a"), Connect("b")];` — [§4.6](#sec-linkedlistt)), or add them one at a time, taking them out of the source as you go: `for var c in src.Drain() do dst.AddLast(move c); end;` (`Drain()` above; the call-site transfer marker is spec [§22.3](Bezel_Language_Spec.md#sec-parameter-modifiers)). `LinkedList<T>`'s `constructor(source: IEnumerable<T>)` ([§4.6](#sec-linkedlistt)) therefore has no owning twin.

<a id="sec-dictionaryk-v"></a>
### 4.8 `Dictionary<K, V>`

Borrowing hash-based map. `K` must be **hashable** (spec §3.6/§8.3) — enforced at the instantiation site per §3.6 rule 2: a `Dictionary<K, V>` (or `OwningDictionary<K, V>`) whose `K` is a record that is neither auto-hashable nor declares user equality is rejected with a diagnostic naming the property. The requirement is the key type's, read at every naming of the instantiation (spec [§14.1](Bezel_Language_Spec.md#sec-generic-declarations)), the comparer constructors included: a comparer chooses the equality the map consults and does not stand in for a key type that has none.

**Stability while stored.** While a value is stored as a **key** of a `Dictionary`/`OwningDictionary` or a `SortedDictionary`, or an **element** of a `Set`/`OwningSet` or `SortedSet`, the program must not mutate any state consulted by its `Equals`/`GetHashCode` / the collection's `IEqualityComparer` (hash forms) or `CompareTo` (ordered forms). Violation leaves the collection's observable behavior **unspecified** — entries may become unfindable by lookup/`Remove` while still visible to iteration and `Count` — but **memory-safe**: no slot is lost or double-held, and an owning collection still disposes every owned element exactly once at collection disposal/`Clear` (the Appendix C guarantee is unconditional). A class-typed key/element is held by **borrow** (the [§28.11](Bezel_Language_Spec.md#sec-borrowing-collections) borrowing column), so its owner may still mutate it while it is stored — this rule is the caller's obligation, not a compile-time guarantee. A `comparer` the constructor receives is retained for the collection's life as a borrow, and a collection holding one does not cross a domain boundary ([§4](#sec-system-collections-generic), *A retained comparer is a borrow*).

```bezel
public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<K>);      // relocates identity whole — see §4.17's
                                                           //   single-order note (rationale §17.14)
  public constructor(capacity: int, comparer: IEqualityComparer<K>);

  public pure readonly property Count: int;
  public readonly property Keys: IReadOnlyCollection<K>;      // (#1354): read-only views, matching §4.9
  public readonly property Values: IReadOnlyCollection<V>;

  public property [key: K]: V;

  public method Add(key: K, value: V);
  public method TryAdd(key: K, value: V): bool;    // (§1 shape (d)): false and no
                                                   //   mutation if the key is present
  public method ContainsKey(key: K): bool;
  public method Contains(key: K): bool;            // alias for ContainsKey;
                                                   //   enables `key in dict`
  public method Remove(key: K): bool;
  public method TryGetValue(key: K, out value: V?): bool;   // (#1336): false path assigns null; value-typed V reads as V? at the call site
  public method GetOrNull(key: K): V?;             // (§1 *OrNull tier): absent -> null; present -> the
                                                   //   value — read semantics and caveat in the prose below
  public method GetOrAdd(key: K, factory: (K) -> V): V;

  public method Clear();                            // drops every entry; disposes neither
                                                    //   keys and values
end Dictionary;

public record DictionaryEntry<K, V>(Key: K, Value: V);

public interface IEqualityComparer<T>
  method Equals(a: T, b: T): bool;
  method GetHashCode(value: T): int;
end IEqualityComparer;
```

Indexer `dict[key]` raises `KeyNotFoundException` if the key is not present. Use `GetOrNull` (the §1 `*OrNull` tier) or `TryGetValue` for safe lookup. The exception is declared here, in `System.Collections.Generic` per the producer-locality rule (language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)); it derives directly from `Exception` — deliberately not from `InvalidOperationException`, so that a state-fault catch arm does not silently swallow missing-key lookups:

```bezel
public class KeyNotFoundException: Exception
  public constructor();
  public constructor(message: string);
end KeyNotFoundException;
```

**`GetOrNull`.** The §1 `*OrNull` convenience tier's keyed-lookup form, beside the throwing indexer and the distinguishing `TryGetValue`: `null` when the key is absent, otherwise the value under the family's read semantics — a copy for value-typed `V` and `string`, a borrow of the stored value (source: the receiver) for class-typed `V`. (When the value type is itself nullable, `?` is idempotent under substitution — spec §7.10 — so a `GetOrNull` result cannot distinguish "absent" from "a present `null`"; use `ContainsKey` or the `Try` shape when the distinction matters.) At class-typed `V` the borrowed result does not meet an owned fallback through `??` (spec §7.7's `if`/`else`-join meet rule): a borrowed fallback composes, and a fresh fallback binds the result first and null-tests — the direct `??` composition is for value-typed and `string` `V` (`counts.GetOrNull(word) ?? 0`, `config.GetOrNull(key) ?? "default"`).

The write surface is the indexer and `Add`: `dict[key] := value` inserts the key or updates an existing one (add-or-update), while `Add(key, value)` inserts and raises `ArgumentException` if the key is already present. There is no separate `Set` method — neither here nor on any other collection (`OwningDictionary` in [§4.9](#sec-owneddictionaryk-v) writes through its owned-taking indexer-set `dict[key] := value`, and the array/list/stack/queue types write through their own indexer or `Add`/`Push`/`Enqueue`). A call to a `Set` method on any collection is rejected as an unknown member. (A rule about the collections' write surface only, not about the name: a `Set` member elsewhere in the library — e.g. `Environment.Set`, [§2.4](#sec-environment) — is unrelated.)

**Counting and update-in-place.** `dict[key] += 1` desugars to a getter plus a setter (spec §6.6/§17.6), and the getter raises `KeyNotFoundException` on an absent key — the compound form updates, it never inserts. The counting idiom initializes explicitly:

```bezel
if not counts.ContainsKey(w) then counts[w] := 0; end;
counts[w] += 1;
```

(or `TryGetValue` plus an indexer-set). `GetOrAdd(key, factory)` is a *read* path, not an update path: it returns the stored value — a copy for value-typed `V` — so incrementing through its result mutates nothing. It is also a store, checked at every call whether or not the key is present: of `key` into a key slot and of the factory's result into a value slot, which carries the factory's sources and `key`'s; both are checked against the dictionary under language spec [§28.11](Bezel_Language_Spec.md#sec-borrowing-collections)'s store rule. No single-lookup upsert member exists on `Dictionary`; for **tallying**, `Counter<K>` ([§4.20](#sec-counterk)) is the dedicated form, and the two-lookup idiom remains the supported form for general non-`int` accumulation.

Iteration over a `Dictionary<K, V>` — `for var entry in dict do ... end` — yields `DictionaryEntry<K, V>` values (the element type of the `ICollection<DictionaryEntry<K, V>>` conformance), one per live entry in a single pass, in insertion order. `entry.Key` and `entry.Value` read the entry; for class-typed values, `entry.Value` is a *borrow* of the stored value — the dictionary retains ownership (the borrowing column of language spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy)). Removing a key and re-adding it places it at the end of the iteration order; an indexer-set on a present key updates the entry in place and keeps its position. `Clear()` resets the order. The `Keys` and `Values` views enumerate in this order, so over one state of the dictionary the n-th key and the n-th value are those of the n-th entry ([#7040](https://github.com/s8lap/bezel-lang/issues/7040)). `Contains(key)` is an alias for `ContainsKey(key)`, enabling the `in` operator (language spec [§17.5](Bezel_Language_Spec.md#sec-the-in-operator)) to test key membership: `key in dict` is equivalent to `dict.ContainsKey(key)`.

The entry-shaped `ICollection<DictionaryEntry<K, V>>` forms (reachable per §1's elision rule) behave as follows: `Add(entry)` is `Add(entry.Key, entry.Value)` — `ArgumentException` on a present key; `Contains(entry)` is `true` iff the key is present **and** the stored value equals `entry.Value` under the type's standard equality; `Remove(entry)` removes only when both match, else returns `false` with no change. `GetOrAdd`'s factory is call-bounded, and structurally mutating the dictionary from inside its own factory raises `InvalidOperationException` — the §29.12 loud-failure discipline, exactly as for sort comparers ([§4.4](#sec-arrayt)); the same rule applies to `OwningDictionary<K, V>.GetOrAdd` ([§4.9](#sec-owneddictionaryk-v)).

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

  public pure readonly property Count: int;
  public readonly property Keys: IReadOnlyCollection<K>;       // borrowing view
  public readonly property Values: IReadOnlyCollection<V>;     // borrowing view

  public property [key: K]: V
    get;                                       // borrow of stored value
    set(owned);                                // owned-taking; disposes previous
  end;

  public method Add(key: K, owned value: V);   // ArgumentException if the key is present; the passed
                                                //   value was consumed at the call site and is disposed
                                                //   on that path — pre-check ContainsKey when
                                                //   the caller must keep it; TryAdd (below) when it
                                                //   only needs the outcome
  public method TryAdd(key: K, owned value: V): bool;   // (§1 shape (d)): false on duplicate, and the
                                                        //   passed value is disposed on that path — the
                                                        //   OwningSet<T>.TryAdd contract (§4.10) verbatim
  public method ContainsKey(key: K): bool;
  public method Contains(key: K): bool;
  public method Remove(key: K): bool;          // disposes the removed value
  public method TryGetValue(key: K, out value: V?): bool;      // out is borrow; (#1336)
  public method GetOrNull(key: K): V?;                         // (§1 *OrNull tier): absent -> null;
                                                               //   present -> a borrow of the stored value, the
                                                               //   indexer's get posture — ownership stays with
                                                               //   the dictionary: a look, not a take (the take
                                                               //   forms remain PopValue/TryPopValue). where V:
                                                               //   class makes V statically non-nullable at
                                                               //   every instantiation (spec §7.10), so null
                                                               //   means absent, unambiguously. The §4.8 ??
                                                               //   limit (spec §7.7) applies at every
                                                               //   instantiation here: an owned fallback never
                                                               //   meets this borrow
  public method GetOrAdd(key: K, factory: (K) -> owned V): V;  // returns borrow

  // Drain
  public method PopValue(key: K): owned V;     // raises if not present. The `Value` suffix carries
                                               //   information and stays through the egress
                                               //   rename (§4.10): the dictionary's egress transfers
                                               //   the *value* and consumes the *key* as a query, so
                                               //   `PopValue(key)` says which of the two the caller
                                               //   gets, where a bare `Pop(key)` would not — a
                                               //   naming-carries-information ground, not an
                                               //   exception to the rename
  public method TryPopValue(key: K, out owned value: V?): bool;   // (#1336)
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
    using var e := move entry;                 // entry is owned; adopt to discharge (transfer marker)
    Process(e.Key, e.Value);                   // e.Value is the owned Connection
  end;
  // dict is now empty; each entry (and its Value) disposed at loop-body end
end DrainAndProcess2;
```

`Drain()` returns `IOwningEnumerable<DrainedEntry<K, V>>` ([§4.13](#sec-owning-enumeration-interfaces)). Iterating it transfers ownership of each `DrainedEntry<K, V>` to the consumer; the entry is a class that owns its `Value`, so adopting the iteration variable with `using var e := move entry;` (the binding-position transfer marker, language spec [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)) — or otherwise discharging it — disposes both the entry shell and, via the standard owned-field cascade (language spec [§28.5](Bezel_Language_Spec.md#sec-owned-fields)), the contained value. Partial iteration drains only the entries yielded; the rest remain in the dictionary. This is the single-call drain; no language feature beyond the existing owning-class and `IOwningEnumerable<T>` machinery is required — `DrainedEntry<K, V>` owns its `Value` exactly as any class owns an `owned` field. The `Key` is carried by value (or as a borrow for class-typed K, following the standard field rules).

`GetOrAdd(key, factory)`'s factory produces an `owned V`; the dictionary stores the value (taking ownership) and the call returns a borrow of the now-stored value. Its `key` is stored as `Add`'s is, under language spec [§28.11](Bezel_Language_Spec.md#sec-borrowing-collections)'s store rule.

The dictionary's `Dispose()` cascade handles every value disposal.

The `Keys` and `Values` properties return read-only borrowing views; their type is `IReadOnlyCollection<K>` and `IReadOnlyCollection<V>` (the unmodified types, since reading produces borrows). The view objects are owned by the collection and handed out as receiver-sourced borrows (a property getter may not mint — spec §28.6): they stay valid while the collection lives and reflect subsequent mutations; the same identity holds for `Dictionary<K, V>.Keys`/`Values` ([§4.8](#sec-dictionaryk-v)). Here the two views enumerate in the one unspecified but deterministic order the *Note on iteration order* below describes, so over one state of the dictionary the n-th key and the n-th value are those of one entry, as at §4.8 ([#7040](https://github.com/s8lap/bezel-lang/issues/7040)). Code that needs to mutate the dictionary calls `Add`, `Remove`, etc. directly.

**Construction via collection literal.** An `OwningDictionary<K, V>` may be constructed with the dictionary-literal form `[k1: v1, ...]`; each value expression must produce an `owned V`, and ownership transfers into the dictionary:

```bezel
using var pool: OwningDictionary<string, Connection> := [
  "primary": Connect("p"),
  "backup":  Connect("b")
];
```

If a literal contains duplicate keys that are not all compile-time constants, the last value wins and each displaced earlier value is disposed at construction (consistent with the runtime last-write-wins rule). The empty owned dictionary is `[]` with an `OwningDictionary<K, V>`-typed target.

`OwningDictionary` does **not** support owned keys (`OwningDictionary<owned K, V>` and similar forms are not provided). Realistic key types are value-like (string, int, records, etc.); cases for owned class-typed keys are rare to nonexistent in practice.

<a id="sec-owneddictionaryk-v-reference-implementation"></a>
#### Reference implementation

`OwningDictionary<K, V>` composes over `OwningBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)) in pure Bezel. The implementation is more involved than `OwningArray<T>`'s because mutation through a chain of borrowed pointers is incompatible with Bezel's borrow-lifetime tracker (see "Note on chain mutation" below); the implementation works in terms of *drain-and-rebuild* on each bucket.

The structure: each bucket of an `OwningBuffer<Entry<K, V>>` holds an owned `Entry<K, V>` (or null), where each entry has an `owned Next: Entry<K, V>?` field forming a chain of synonym-hash entries. The `Value` field is declared nullable (`owned Value: V?`) so a bare `move chain.Value` can take it, leaving `null` ([§28.8.1](Bezel_Language_Spec.md#sec-the-replacing-move) of the language spec); in committed state every entry's `Value` is non-null.

```bezel
// Internal entry type.
private class Entry<K, V> where V: class
  public Key: K;
  public owned Value: V?;          // nullable so a bare `move` takes it (§28.8.1);
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
    var chain := move _buckets[idx];   // the bucket holds null while the chain is rebuilt
    // A comparer that throws mid-walk unwinds over `chain` and `rebuilt`: both are
    //   disposed, so the bucket's entries are lost (none is disposed twice) and `_count`
    //   still counts them; this sketch does not rebuild the bucket on unwind.
    var rebuilt: Entry<K, V>? := null;
    var foundDup := false;
    while chain <> null do
      if not foundDup and _comparer.Equals(chain.Key, key) then
        foundDup := true;
      end;
      var rest := move chain.Next with rebuilt;   // one step: take the tail, link the rebuilt head
      rebuilt := move chain;           // ownership transfers in (transfer marker)
      chain := move rest;              // (transfer marker)
    end;
    if foundDup then
      _buckets[idx] := rebuilt;       // put the unchanged chain back
      throw new ArgumentException("duplicate key");
      // The owned `value` parameter is auto-disposed on throw per the
      // [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) exception-path discharge rule.
    end;
    _buckets[idx] := new Entry<K, V>(key, move value, move rebuilt);   // call-site markers (spec §22.3)
    _count := _count + 1;
    MaybeGrow();
  end Add;

  public method PopValue(key: K): owned V
    var idx := BucketIndex(key);
    var chain := move _buckets[idx];   // the bucket holds null while the chain is rebuilt
    // As in `Add`: a comparer that throws mid-walk unwinds over `chain` and
    //   `rebuilt`, and the bucket's entries are lost, each disposed exactly once (`_count` unchanged).
    var rebuilt: Entry<K, V>? := null;
    var taken: V? := null;
    while chain <> null do
      var isTarget := taken = null and _comparer.Equals(chain.Key, key);
      var rest := move chain.Next;     // the tail; `chain.Next` holds null
      if isTarget then
        taken := move chain.Value;     // `chain.Value` holds null
        chain.Dispose();                  // value and next are both null;
                                          //   destructor's auto-dispose passes are no-ops
      else
        chain.Next := rebuilt;            // link the rebuilt-chain head (a store into a null field)
        rebuilt := move chain;           // (transfer marker)
      end;
      chain := move rest;                // (transfer marker)
    end;
    _buckets[idx] := rebuilt;             // put the rebuilt chain back
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

The structural insight: **every bucket mutation is expressed as taking the chain head with `move`, walking the owned chain as local state, and storing a rebuilt chain back into the bucket**. No method mutates a chain through a borrow into shared storage. Every `move buf[i]` and `move entry.X` is the replacing `move` of [§28.8.1](Bezel_Language_Spec.md#sec-the-replacing-move): it leaves `null` (the bucket and the entry fields are nullable) or the replacement named by `with` in the same step, so no slot or field is ever observed empty-but-owning.

**Note on chain mutation.** A naïve implementation might walk the chain with a borrow `probe: Entry<K, V>?` and surgically modify links (`prev.Next := probe.Next`, etc.). Bezel's borrow-lifetime tracker rejects this pattern: `probe`'s borrow source is rooted in `_buckets[idx]`, and mutating any link along the chain — or mutating the slot itself — invalidates the borrow chain, after which subsequent reads of `probe.X` are borrow-after-mutation errors. The drain-and-rebuild pattern sidesteps the issue by walking the chain as owned local state. The cost is `O(chain length)` work per mutation rather than `O(1)`. In practice, chain lengths are bounded by the dictionary's load factor (typically ≤ 8), so the constant factor is small; for use cases where this cost is unacceptable, an open-addressing implementation can be substituted without changing the public API.

**Note on iteration order.** The drain-and-rebuild pattern reverses chain order on every bucket mutation. Iteration over `OwningDictionary<K, V>` therefore yields entries in an order that depends on insertion-and-mutation history rather than purely on insertion order. The standard library makes no insertion-order guarantee for `OwningDictionary` (in contrast to `Dictionary<K, V>` at [§4.8](#sec-dictionaryk-v), which does); programs that need ordered iteration use a different collection. The order, while unspecified, is **deterministic**: it is a pure function of the sequence of mutating operations performed and the key hash values involved — two instances built by identical operation sequences over keys with identical hash values iterate identically. This is a determinism statement, not an order statement; programs must still not rely on any particular order.

**Why no compiler intrinsic for `OwningDictionary`.** None of the operations above require compiler-intrinsic knowledge of `OwningDictionary<K, V>`. Every `move` is the replacing `move` the compiler lowers for `move buf[i]` and `move entry.X` alike. The intrinsic surface of the owning-collection family is once again confined to `OwningBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)).

<a id="sec-sett"></a>
<a id="sec-sett-and-ownedsett"></a>
### 4.10 `Set<T>` and `OwningSet<T>`

Borrowing and owning hash-based sets. `T` must be **hashable** (spec §3.6/§8.3), enforced at the instantiation site per §3.6 rule 2 — same rule as `Dictionary<K, V>` keys (§4.8), at every naming and whatever constructor runs, a comparer included.

**Iteration order.** Iteration over a `Set<T>` yields elements in **insertion order** — removing and re-adding an element places it at the end — the same guarantee as `Dictionary<K, V>` ([§4.8](#sec-dictionaryk-v)). `OwningSet<T>` makes **no** iteration-order guarantee, for [§4.9](#sec-owneddictionaryk-v)'s drain-and-rebuild reason; programs needing ordered iteration use `SortedSet<T>` or an `Array`.

```bezel
public class Set<T>: ICollection<T>, ICollectionLiteralTarget<Set<T>, T>
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Set<T>;   // §2.19 witness; duplicates
                                                                                              //   collapse (spec §15.4)
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<T>);
  public constructor(capacity: int, comparer: IEqualityComparer<T>);   // the §4.8 combined form

  // ICollection<T> operations (including Add(item: T) returning ()) plus:
  public method TryAdd(item: T): bool;                 // returns whether added
                                                       //   (false if duplicate)
  public method UnionWith(other: IEnumerable<T>);
  public method IntersectWith(other: IEnumerable<T>);
  public method ExceptWith(other: IEnumerable<T>);
  public method IsSubsetOf(other: IEnumerable<T>): bool;
  public method IsSupersetOf(other: IEnumerable<T>): bool;
end Set;

public class OwningSet<T>: IOwningCollection<T>, IOwningCollectionLiteralTarget<OwningSet<T>, T> where T: class
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningSet<T>;   // §2.19 witness
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<T>);
  public constructor(capacity: int, comparer: IEqualityComparer<T>);

  // IOwningCollection<T> operations plus:
  public method TryAdd(owned item: T): bool;          // returns whether added
                                                       //   (false if duplicate;
                                                       //   passed item is disposed)
  // Drain
  public method Pop(query: T): owned T;                // borrow-taking query;
                                                       //   returns owned matched
                                                       //   (renamed from
                                                       //   Take/TryTake — below)
  public method TryPop(query: T, out owned removed: T?): bool;
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
  pool.Add(move candidate);    // ownership transfers (call-site marker)
end;
// On the duplicate path, candidate retains ownership.
```

`Pop(query: T)` finds the equal element in the set, removes it, and returns ownership. The query and the stored element are equal by the set's `Equals` rule but may be distinct instances. `TryPop` is the no-throw variant.

A `Set<T>` may be constructed with a sequence literal (language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)); duplicates (per `T`'s equality contract) are silently collapsed, the first textual occurrence retained: `using var s: Set<int> := [1, 2, 2, 3];` yields `{1, 2, 3}`. For `OwningSet<T>`, each later duplicate's owned element is disposed at construction. The empty set is `[]` with a `Set<T>`-typed (or `OwningSet<T>`-typed) target.

<a id="sec-stackt-and-queuet"></a>
<a id="sec-stackt-ownedstackt-queuet-and-ownedqueuet"></a>
### 4.11 `Stack<T>`, `OwningStack<T>`, `Queue<T>`, and `OwningQueue<T>`

LIFO and FIFO collections.

```bezel
public class Stack<T>: IReadOnlyCollection<T>, ICollectionLiteralTarget<Stack<T>, T>   // (#1354)
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Stack<T>;   // §2.19 witness; first element
                                                                                                //   pushed first (spec §15.4)
  public constructor();
  public method Push(item: T);
  public method Contains(item: T): bool;        // explicit (declaration restored)
  public method Pop(): T;             // raises if empty
  public pure method Peek(): T;
  public method TryPop(out item: T?): bool;    // (#1336)
  public method TryPeek(out item: T?): bool;
  public method Clear();                        // (#1811): drops all elements; disposes nothing (§28.6)
end Stack;

public class OwningStack<T>: IReadOnlyCollection<T>, IOwningCollectionLiteralTarget<OwningStack<T>, T> where T: class   // (#1811): demoted, completing #1354
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningStack<T>;   // §2.19 witness
  public constructor();
  public method Push(owned item: T);
  public method Contains(item: T): bool;        // explicit (#1811)
  public method Pop(): owned T;                   // raises if empty; transfers out
  public pure method Peek(): T;                         // borrow of top element
  public method TryPop(out owned item: T?): bool;   // (#1336)
  public method TryPeek(out item: T?): bool;       // out is borrow
  public method Clear();                           // disposes every element
  public method Drain(): IOwningEnumerable<T>;
end OwningStack;

public class Queue<T>: IReadOnlyCollection<T>, ICollectionLiteralTarget<Queue<T>, T>   // (#1354)
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Queue<T>;   // §2.19 witness; first element
                                                                                                //   at the front (spec §15.4)
  public constructor();
  public method Enqueue(item: T);
  public method Dequeue(): T;
  public pure method Peek(): T;
  public method Contains(item: T): bool;        // explicit
  public method TryDequeue(out item: T?): bool;   // (#1336)
  public method TryPeek(out item: T?): bool;
  public method Clear();                        // (#1811): drops all elements; disposes nothing (§28.6)
end Queue;

public class OwningQueue<T>: IReadOnlyCollection<T>, IOwningCollectionLiteralTarget<OwningQueue<T>, T> where T: class   // (#1811)
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningQueue<T>;   // §2.19 witness
  public constructor();
  public method Enqueue(owned item: T);
  public method Contains(item: T): bool;        // explicit (#1811)
  public method Dequeue(): owned T;
  public pure method Peek(): T;                         // borrow of front element
  public method TryDequeue(out owned item: T?): bool;   // (#1336)
  public method TryPeek(out item: T?): bool;       // out is borrow
  public method Clear();                           // disposes every element
  public method Drain(): IOwningEnumerable<T>;
end OwningQueue;
```

For class T on the borrowing forms, `Pop`, `Peek`, and `Dequeue` produce borrows whose **compile-time source is the collection** (spec §28.3's bare-type-parameter rule: the result is statically usable while the receiver is live). The collection is a transit point only for the *underlying obligation*: that the element's original owner outlives the use is the caller-side residual of §28.11's borrowing-collection store rule, which the static bound does not re-check. Storing the popped value in a long-lived field of another object remains a borrow-lifetime violation per language spec [§28.6](Bezel_Language_Spec.md#sec-leak-prevention).

On the owning forms, `Pop` and `Dequeue` return ownership; the collection no longer holds the element. This resolves the borrow-source story cleanly: when the stack owns its elements, the popped element transfers ownership to the caller with the standard discharge obligation.

**Construction via collection literal.** All four types accept a sequence literal (language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)). The ordering convention matters and differs between stack and queue:

- `Queue<T>` / `OwningQueue<T>`: the first literal element is at the front (dequeued first). `using var q: Queue<int> := [1, 2, 3];` dequeues `1`, `2`, `3` in that order.
- `Stack<T>` / `OwningStack<T>`: the literal lists elements bottom-to-top — the first element is pushed first (ends up at the bottom), the last element is on top (popped first). `using var s: Stack<int> := [1, 2, 3];` pops `3`, `2`, `1` in that order.

The bottom-to-top reading for stacks was chosen so that the literal's left-to-right order matches the push order; a reader can trace `[1, 2, 3]` as "push 1, push 2, push 3." For owning forms, element expressions must produce owned values. The empty collection is `[]` with the corresponding typed target.

**Iteration order.** All four types iterate (via `IReadOnlyCollection<T>`) in take order — a stack top-to-bottom (pop order), a queue front-to-back (dequeue order); the owning forms iterate identically, yielding borrows.

**Surface coherence (#1811).** The owning forms, like the borrowing forms, conform to `IReadOnlyCollection<T>` only: `Add`, `Remove`, and `IsReadOnly` have no stated LIFO/FIFO semantics, and on the owning family `Remove`'s dispose-the-removed-element contract (§4.2) would make an unpositioned middle-removal irreversible. Element ingress is `Push`/`Enqueue`, egress is `Pop`/`Dequeue`/`Try*`/`Drain`, bulk disposal is `Clear` — the declared members are the whole mutation story. The borrowing forms also provide `Clear()`: it drops every element reference and disposes nothing, per the borrowing-family rule (§4.1; spec §28.6), matching `Array<T>` and the owning twins.

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

The standard iteration interfaces. `IEnumerable<T>` is covariant in T. Neither interface, nor its async variant below, admits a **move-only value type** (language spec [§15.3](Bezel_Language_Spec.md#sec-move-only-value-types) — an owning inline array or an owning choice) as `T`: an instantiation at one is a compile error at the instantiation site, through any chain of user declarations (language spec [§28.11](Bezel_Language_Spec.md#sec-collection-element-ownership), [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties) rule 2). `IEnumerator<T>.Current` is non-destructive — it returns a borrow of the most-recently-yielded value, which the enumerator continues to own (if it has ownership; if T is a borrowing element type, Current returns a re-borrow).

These interfaces are auto-imported with the rest of this module (language spec [§2.3](Bezel_Language_Spec.md#sec-always-available-names)); they are the interfaces the for-loop desugaring names. `IEnumerable<T>` does not extend `IDisposable`, so `Dispose()` is not a member of a value typed at it (spec §8.7): an owned `IEnumerable<T>` local — a minted frame, spec §28.3 — is disposed at scope end, by `using var` or by the discard `_ := xs;`. `IEnumerator<T>` extends it and is disposed explicitly as any owned `IDisposable` value is (spec §29.1).

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
  method TakeCurrent(): owned T;        // transfers ownership
  method MoveNext(): bool;
end IOwningEnumerator;
```

The owning iteration interfaces. `IOwningEnumerator<T>.TakeCurrent()` transfers ownership of the most-recently-yielded value to the caller after a successful `MoveNext`; a second take without an intervening advance, or a take before the first advance/after exhaustion, raises `InvalidOperationException` (spec §29.1). Between a successful advance and `TakeCurrent()` the **enumerator owns the pending value**: a `Dispose()` after an advance whose value was never taken disposes it — an advanced-but-untaken element neither leaks nor reaches the consumer (spec §29.1). The for-loop desugaring at language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) calls `TakeCurrent()` exactly once per iteration.

A `for var x in src do ... end` where `src` is an `IOwningEnumerable<T>` produces owned iteration variables. Each `x` carries the standard owned-local obligation on every normal-exit edge (fall-through, `break`, `continue`, `return`): the body may discharge it explicitly — transfer it on, or dispose it — and where it does not, the affine auto-drop of language spec [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) disposes it at the iteration's scope end with the plain-`var` must-use warning. Either way it is disposed exactly once (the same rules the async form states below). On exception edges the iteration variable is auto-disposed by the implicit `try`/`finally` rule of [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) "Exception-path discharge."

These interfaces are auto-imported alongside the borrowing iteration family.

The async variants:

```bezel
public interface IOwningAsyncEnumerable<T>: IAsyncDisposable where T: class
  method GetOwningAsyncEnumerator(token: CancellationToken := CancellationToken.None): owned IOwningAsyncEnumerator<T>;
end IOwningAsyncEnumerable;

public interface IOwningAsyncEnumerator<T>: IAsyncDisposable where T: class
  method TakeCurrent(): owned T;        // transfers ownership
  method MoveNextAsync(): Task<bool>;
end IOwningAsyncEnumerator;
```

`IOwningAsyncEnumerable<T>` is the return type for async iterator methods that yield owned values (asynchronously-produced owned resources, async drain operations on owning collections). The `TakeCurrent()` rule applies to `IOwningAsyncEnumerator<T>` the same way (relative to `MoveNextAsync`), including the pending-value ownership — `DisposeAsync()` after an advance whose value was never taken disposes it; `await for var x in src do ... end` produces owned iteration variables; the spec §28.3 discharge rules (auto-drop with must-use warning) apply in the body.

A type may implement both `IEnumerable<T>` and `IOwningEnumerable<T>`. `OwningArray<T>` does so: default iteration via `IEnumerable<T>` yields borrows of T (the standard read-only-iteration semantics); drain iteration via `IOwningEnumerable<T>` yields owned T values that the consumer must discharge. The for-loop desugaring picks based on the static type of the source expression: `for var x in arr do` desugars via `IEnumerable<T>` (borrowing); `for var x in arr.Drain() do` desugars via `IOwningEnumerable<T>` (owning). Family-wide: every owning collection's `Drain()` mints a **fresh drain-view frame** — a caller-owned enumerable over the collection, never the collection re-exposed (spec [§28.3](Bezel_Language_Spec.md#sec-producers)'s calls-mint rule; [§4.5](#sec-ownedarrayt) states the `OwningArray<T>` case) — so the frame's disposal at loop end never disposes the collection, and partial iteration leaves the undrained remainder in place. A drain view is **not single-pass**: it is an ordinary owned frame, not a spec-[§29.11](Bezel_Language_Spec.md#sec-multiple-enumeration) lazy query (the affine filing retired), and each `GetOwningEnumerator()` call on a live view yields a fresh enumerator over the collection's *current* remainder — [§4.5](#sec-ownedarrayt)'s partial-iteration contract means a re-enumeration continues draining what remains, never replaying what already left.

<a id="sec-owned-buffers-and-spans"></a>
<a id="sec-ownedbuffert-spant-readonlyspant"></a>
### 4.14 `OwningBuffer<T>`, `Span<T>`, `ReadOnlySpan<T>`

A runtime-sized storage primitive and two non-owning view types built over it. Together they are the foundation over which the resizable owning collections (`OwningArray<T>`, `OwningDictionary<K, V>`, etc.) compose, and they give user code a safe way to express "a fixed-but-runtime-sized buffer of slots" without any `unsafe` mechanism.

The slot semantics dispatch on T's category:

- **Class-typed T** (classes, interfaces): slots hold `T?` (nullable references) and the buffer owns each non-null slot's referent. Setter takes `owned T?` and disposes the previous value before storing the new one. `move buf[i]` is admitted — the replacing `move` of [§28.8.1](Bezel_Language_Spec.md#sec-the-replacing-move): bare, it takes the element and stores `null`; `move buf[i] with e` stores `e`. The buffer's destructor cascades through slots, disposing every non-null one.
- **Value-typed T** (primitives, records, enums, **value-like and borrow-bearing choices** ([§35.4](Bezel_Language_Spec.md#sec-choice-classification)), non-move-only inline arrays — language spec [§15.3](Bezel_Language_Spec.md#sec-move-only-value-types)'s copyable set, whose complement is the exclusion list below): slots hold `T` directly (no nullability wrapper). Setter takes `T` and overwrites the slot. `move buf[i]` is *not* admitted (values have no ownership to transfer; reading is sufficient). The buffer's destructor releases its backing storage but performs no per-slot disposal. A **value-like** choice slot is an ordinary copyable value ([§35.4](Bezel_Language_Spec.md#sec-choice-classification) rule 3) and needs nothing further; a **borrow-bearing** choice slot carries the borrow obligations of *Borrow-bearing slots* below. The **owning** classification is a move-only value type and is not admitted (the exclusion list below).

The two categories share the same `Length`, indexer-access, `AsSpan`/`AsReadOnlySpan` factories, and other category-agnostic surface; they differ in the slot type, the setter modifier, and the admissibility of `move`. (`Slice` is a member of the span *views*, not of the buffer — a buffer is range-restricted by taking a span of it.)

**What is not admitted.** Runtime-managed reference types (`string`, function types) are not admitted as T — they have no disposal obligation, so the "owning" semantics add nothing, and they are stored more naturally in borrowing collections (`Array<string>`, `Array<(int) -> int>`). Move-only value types (both kinds of [§15.3](Bezel_Language_Spec.md#sec-move-only-value-types) of the language spec — the owning inline arrays `[N owned]T` and `[N owned]T?`, and the owning-classified choices of [§35.4](Bezel_Language_Spec.md#sec-choice-classification)) are also not admitted — the slot semantics for buffers of move-only types require additional design work and are deferred (see [§18](Bezel_Design_Rationale.md#sec-things-deferred) in the rationale).

<a id="sec-ownedbuffert"></a>
#### `OwningBuffer<T>`

The declaration's surface depends on T's category. For class-typed T:

```bezel
public class OwningBuffer<T>: IEnumerable<T?>            // class T only
  public constructor(length: int);                       // all slots null
  public constructor(length: int, init: (int) -> owned T);

  public pure readonly property Length: int;    // stored extent, immutable once
                                                //   allocated; pure per spec §22.15
                                                //   *Intrinsic purity*

  public property [i: int]: T?
    get;                                                  // borrow of slot
    set(owned);                                           // takes owned T?; disposes previous
  end;

  // move buf[i] admitted (returns owned T? and stores null in the same step;
  //   `move buf[i] with e` stores e — language spec §28.8.1)

  // Span views
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

  public pure readonly property Length: int;    // stored extent, immutable once
                                                //   allocated; pure per spec §22.15
                                                //   *Intrinsic purity*

  public property [i: int]: T
    get;                                                  // borrow of slot (value copy
                                                          //   for primitives at the use site)
    set;                                                  // overwrites slot
  end;

  // move buf[i] NOT admitted (values have no ownership; use plain read)

  // Foreign-memory access (language spec §32.12) — admitted only for
  //   T in the language spec §32.12 FFI-pointee set; see prose. The result type
  //   lives in System.Interop: naming Ptr<T> requires that import, using the
  //   returned value does not (language spec §2.8.3 rule 2).
  public method DangerousPointer(): Ptr<T>;

  // Span views
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

**Class-typed T.** `OwningBuffer<T>` is a class with fixed-at-construction storage of N slots, each holding `T?` (a nullable reference). Once allocated, `Length` is immutable. Non-null slots are owned by the buffer; the buffer's destructor disposes every non-null slot in implementation-defined order. The single-argument constructor produces a buffer with all slots initialized to `null`. The two-argument constructor calls `init(i)` for each `i` in `0..<length`, taking ownership of each returned value. The indexer's setter takes ownership of the assigned value and disposes whatever the slot previously held. `move buf[i]` is admitted and produces an owned `T?` value, storing `null` into the slot in the same step; `move buf[i] with e` stores `e` instead ([§28.8.1](Bezel_Language_Spec.md#sec-the-replacing-move)). No slot is ever left empty-but-owning.

**Value-typed T.** `OwningBuffer<T>` is a class with fixed-at-construction storage of N slots, each holding `T` directly (no nullability wrapper). Once allocated, `Length` is immutable. The single-argument constructor requires T to be **defaultable** — an instantiation-site rule, read at a concrete argument and, where a generic body names `OwningBuffer<T>` at a type argument that is or contains its own type parameter, at each instantiation substituting the argument (language spec [§14.2](Bezel_Language_Spec.md#sec-conditionally-admitted-members), the two-presentation paragraph; [§17.11](Bezel_Language_Spec.md#sec-the-default-operator)'s "any other site that supplies a `default(T)`") — note that **no choice type is defaultable** (language spec [§17.11](Bezel_Language_Spec.md#sec-the-default-operator), [§35.10](Bezel_Language_Spec.md#sec-choice-default-with): *"`default(C)` is a compile error for every choice type"*), so a choice-element buffer is always built with the two-argument init-function constructor below — (language spec [§17.11](Bezel_Language_Spec.md#sec-the-default-operator)) — every primitive is; a record is when every declared field is and the record is not constructor-only (language spec [§9.9](Bezel_Language_Spec.md#sec-constructor-only-records)); an enum is when it declares a zero-valued constant; any type §17.11 does not admit has no default. It initializes every slot to `default(T)`. For types without a default, only the two-argument constructor with an explicit init function is admitted. The two-argument constructor calls `init(i)` for each `i` in `0..<length`, copying the returned value into the slot. The indexer's setter overwrites the slot's previous value with the new one — no disposal is performed (values are not owned). `move buf[i]` is not admitted; values are read with the plain indexer and copied at the use site.

The "Owning" in the name refers to the buffer-class's ownership of *its allocated storage* — and, for class-typed T, of its slot contents (the semantics that motivated the name). For value-typed T, the buffer owns only its allocation; the slot contents are just data. In both cases the buffer is `IDisposable`: its destructor releases the allocation.

**Span views.** `AsSpan()` and its overloads return a `Span<T>` (writable view) over a contiguous range of the buffer; `AsReadOnlySpan()` returns a `ReadOnlySpan<T>`. These views are non-allocating record constructions and admit the standard span surface (`Slice`, indexer, search, equality, bulk mutation per T's category). The methods are pure ergonomics — `buf.AsSpan(0, 100)` reads more naturally than `new Span<T>(buf, 0, 100)` and matches the established pattern from `string.AsSpan()` and `Array<T>.AsSpan()`. `AsSpan` returns the writable form by default (matching the buffer's mutability); callers wanting read-only access either call `AsReadOnlySpan` directly or call `.AsReadOnly()` on the writable form. The four overload signatures of each form accept range parameters: no-arg (full buffer, `0..<Length`), `(start)` (from offset to end), `(start, length)`, or `(r: Range<int>)`. Bounds-check semantics follow the standard Bezel convention: `IndexOutOfRangeException` if `start` or `length` is out of range against the buffer's `Length`.

> ⚠ **Spec-ahead-of-implementation ([#1717](https://github.com/s8lap/bezel-lang/issues/1717)):** `DangerousPointer()` is not implemented: a call to it is rejected with a diagnostic stating so.

**`DangerousPointer()`.** Returns the address of slot 0 as a `Ptr<T>` (language spec [§32.12](Bezel_Language_Spec.md#sec-foreign-memory)), valid until the buffer is disposed — by whichever domain then holds the buffer: a value-typed `OwningBuffer<T>` is sendable ([§4](#sec-system-collections-generic)), and a pointer the origin domain keeps after moving the buffer is governed by language spec [§32.10](Bezel_Language_Spec.md#sec-boundary-contract)'s cross-domain bullet. Declared on the value-typed presentation only, and admitted only where `T` is in the **FFI-pointee set** — a **conditionally-admitted member** in the sense of language spec [§14.2](Bezel_Language_Spec.md#sec-conditionally-admitted-members), its condition stated here and not yet implemented by the compiler (#1717) — `Ptr<T>`'s own pointee gate (language spec [§32.12](Bezel_Language_Spec.md#sec-foreign-memory)): integer types, `float`, `double`, `bool`, `IntPtr`, `UIntPtr`, and C-compatible records (language spec [§32.13](Bezel_Language_Spec.md#sec-c-compatible-records)). The result is a `Ptr<T>`, so the gate is the same gate: every other `T` — classes (the class-typed presentation never satisfies the gate, so the member does not appear there), `string`, and `Ptr<U>` elements (whose slot address would be the declined `Ptr<Ptr<U>>`) — is rejected at the call's instantiation, or at the formation of a method reference over the member (language spec [§27.6](Bezel_Language_Spec.md#sec-method-references)), with a diagnostic naming the set. Pointer arrays are `OwningBuffer<IntPtr>` — exactly the argv shape. The `Dangerous` prefix marks the one accessor family whose result aliases mutable Bezel-owned slots; native retention past the synchronous call is the language spec [§32.10](Bezel_Language_Spec.md#sec-boundary-contract) obligation, and the recorded mitigation is the wrapper class that owns buffer and registration together.

**Iteration.** For class-typed T, `OwningBuffer<T>` implements `IEnumerable<T?>`; iteration yields borrows of `T?` (slot contents, possibly null). The `for var x in buf do ... end` form works both via the enumerator protocol and via the direct-indexed-loop optimization that [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) permits for concretely-typed sources. For value-typed T, `OwningBuffer<T>` does *not* implement `IEnumerable<T>` (the type-parameter-category dispatch would require conditional interface implementation, which Bezel does not currently provide as a general mechanism). `for var x in buf do ... end` over a value-typed OwningBuffer desugars to a direct indexed loop per the [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) extension; the iteration variable has type T and is a copy of the slot. Users who need IEnumerable<T> conformance on a value-typed buffer can wrap or convert.

**Why this is a compiler-known primitive.** The `OwningBuffer<T>` class needs intrinsic-level support primarily for one operation: **allocation of N slots where N is a runtime constructor parameter**. There is no user-level Bezel syntax for "a class field holding a runtime-determined-size array of slots"; inline arrays `[N owned]T?` and `[N]T` require compile-time N. The remaining surface (the indexer's interaction with the `move` expression form for class T, the destructor's per-slot disposal pass for class T, the iteration support) reuses mechanisms that `OwningArray<T>` and similar collections already exercise, just operating on the runtime-sized backing. The intrinsic boundary of the owning-collection family shrinks to this one class.

**Disposal.** For class-typed T, the buffer's destructor disposes every non-null slot. Slot-disposal order within a single buffer is implementation-defined. For value-typed T, the destructor releases the backing storage but performs no per-slot work. Both cases satisfy the `IDisposable` contract; both are non-trivial-dispose ([§28.9](Bezel_Language_Spec.md#sec-runtime-managed-types) of the language spec) in the sense that the user must discharge the buffer (typically via `using var` or by storing it as an `owned` field of a longer-lived class).

**Borrow-bearing slots.** When T is a class type whose instances themselves carry borrows from external sources (a class with a borrow-bearing record field, an interface backed by such a class, or any class whose fields include reachable borrows), the buffer participates in the same borrow-lifetime tracking as any other class-typed storage: the buffer may not outlive any source from which a slot borrows. The compiler tracks slot-borrow sources element-wise where possible and falls back to whole-buffer tracking when slot-borrow sources cannot be statically separated. For value-typed T whose values may themselves carry borrows — records with borrow-bearing fields, and **choices that hold borrows** (language spec [§35.4](Bezel_Language_Spec.md#sec-choice-classification)'s holds-borrows property; within the admitted set this is exactly the borrow-bearing classification, an owning choice being inadmissible as T) — the same rule applies to the values stored in slots. The value-tier constructor's `init` results are among them: each slot carries what the invocation `init(i)` carries, the function value's source set (language spec [§28.3](Bezel_Language_Spec.md#sec-bare-type-parameter-returns), *Bare-type-parameter returns*, caller-side clause 3), so the buffer may not outlive what `init` captures. For a choice the obligations are language spec [§9.7](Bezel_Language_Spec.md#sec-records-and-disposal)'s borrow-lifetime bullets, which that section applies to choices verbatim *"with "record" read as "record or choice" throughout"*; nothing choice-specific is added here.

<a id="sec-spant"></a>
#### `Span<T>`

The declaration's surface, like `OwningBuffer<T>`'s, depends on T's category.

For class-typed T:

```bezel
public record Span<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)
  // class T

  // Indexer (bounds-checked against span Length).
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

  // Range query.
  public property IsEmpty: bool => Length = 0;

  // Search.
  public method IndexOf(value: T?): int;          // -1 if not found
  public method Contains(value: T?): bool;        // IndexOf(value) >= 0

  // Equality.
  public method SequenceEqual(other: Span<T>): bool;
  public method SequenceEqual(other: ReadOnlySpan<T>): bool;      // — the read-only span of this backing
  public method SequenceEqual(other: ReadOnlyArraySpan<T>): bool; // — the read-only span of the other backing (§4.15)

  // Range construction.
  public method Slice(start: int, length: int): Span<T> =>
    new Span<T>(Buffer, this.Start + start, length);
  public method Slice(start: int): Span<T> => Slice(start, Length - start);
  public method Slice(r: Range<int>): Span<T>;   // — Range form: normalized against
                                                  //   Length (spec §16.3), then equivalent to
                                                  //   Slice(start, length); see prose

  public method AsReadOnly(): ReadOnlySpan<T> =>
    new ReadOnlySpan<T>(Buffer, Start, Length);

  // Bulk mutation.
  public method Clear();                          // set every slot to null
                                                   //   (disposes previous contents)
  public method MoveFrom(source: Span<T>);        // move slot-by-slot from source;
                                                   //   source slots become null;
                                                   //   destination's previous
                                                   //   contents disposed;
                                                   //   lengths must match
  public method MoveTo(destination: Span<T>);     // — symmetric with MoveFrom;
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
public record Span<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)
  // value T (primitive, record, enum, value-like or borrow-bearing choice,
  //   non-move-only inline array — spec §15.3's copyable set)

  // Indexer (bounds-checked against span Length).
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
  public property IsEmpty: bool => Length = 0;

  // Search.
  public method IndexOf(value: T): int;
  public method Contains(value: T): bool;

  // Equality.
  public method SequenceEqual(other: Span<T>): bool;
  public method SequenceEqual(other: ReadOnlySpan<T>): bool;      // — the read-only span of this backing
  public method SequenceEqual(other: ReadOnlyArraySpan<T>): bool; // — the read-only span of the other backing (§4.15)

  // Range construction.
  public method Slice(start: int, length: int): Span<T> =>
    new Span<T>(Buffer, this.Start + start, length);
  public method Slice(start: int): Span<T> => Slice(start, Length - start);
  public method Slice(r: Range<int>): Span<T>;   // — Range form: normalized against
                                                  //   Length (spec §16.3), then equivalent to
                                                  //   Slice(start, length); see prose

  public method AsReadOnly(): ReadOnlySpan<T> =>
    new ReadOnlySpan<T>(Buffer, Start, Length);

  // Bulk mutation (value-typed forms).
  public method Fill(value: T);                   // copy `value` into every slot
  public method CopyFrom(source: ReadOnlySpan<T>); // copy slot-by-slot from source
                                                    //   lengths must match
  public method CopyTo(destination: Span<T>);      // — copy slot-by-slot to destination
                                                    //   lengths must match
  public method Reverse();
end Span;
```

A `Span<T>` is a borrow-bearing record ([§9.7](Bezel_Language_Spec.md#sec-records-and-disposal) of the language spec) viewing into a contiguous range of an `OwningBuffer<T>`. The `Buffer` field carries a borrow of an `OwningBuffer<T>`; per the borrow-bearing-record rule, the span's lifetime is bounded by the borrow's source, so a span cannot outlive the buffer it views.

`Span<T>` is purely user-level Bezel — no compiler intrinsic. The record machinery, borrow lifetime tracking, and indexer dispatch are all standard language features. So is the disposal backstop: a read or write through the span reaches the buffer through `OwningBuffer<T>`'s own indexer, and on a logically-disposed buffer it faults as any member access on a disposed instance does (language spec [§28.2](Bezel_Language_Spec.md#sec-checked-disposal), checked disposal mode); the span's own components (`Start`, `Length`, `IsEmpty`, `Slice`) touch no storage. That includes the generated record members of language spec [§8.5](Bezel_Language_Spec.md#sec-tostring)/[§9.1](Bezel_Language_Spec.md#sec-positional-records): the span records carry them by the ordinary record rules (the positional-record `ToString` among them) — the listings in this section are a lower bound on the surface, and a member's absence from a body listing is not an exclusion.

**Read access.** For class T: `span[i]` returns a borrow of the slot's value (which is `T?` because the underlying buffer's slots are nullable). For value T: `span[i]` returns a borrow of the slot (`T`, non-nullable). The borrow's source is the underlying `OwningBuffer<T>` (transitively reached through the span's `Buffer` field). For class T with potentially-null slots, programs that view into a buffer known to have non-null contents at every slot can use `span[i]!` for the null-forgiving access, raising `NullReferenceException` if the slot is null.

The indexer bounds-checks against the span's `Length` (not just the underlying buffer's length). If `i < 0` or `i >= Length`, `IndexOutOfRangeException` is thrown. The check fires even when `Start + i` would be in the underlying buffer's range — the point of the span's range restriction is to enforce that callers cannot read outside the slice the span advertises.

**Write access.** For class T: `span[i] := value` takes ownership of the assigned value (per the `set(owned)` setter) and stores it in the underlying buffer slot. The underlying buffer's owned-setter disposes whatever the slot previously held; the new value replaces it. `move span[i]` is also admitted: it takes ownership of the underlying slot's current contents (an owned `T?`) and stores `null` into that slot in the same step, or the replacement named by `with` ([§28.8.1](Bezel_Language_Spec.md#sec-the-replacing-move)). The setter and `move` form bounds-check against the span's `Length` (same rule as the getter).

For value T: `span[i] := value` overwrites the slot with the new value. No ownership transfer happens; no previous value is disposed. `move span[i]` is *not* admitted — values are read with the plain indexer and copied at the use site. Writing through a value-typed span is mechanically the same as writing through a value-typed buffer directly.

A span carries the full read/write capability of the underlying buffer for its viewed range; what distinguishes it from holding the whole buffer is *range restriction*, not access mode. Programs that want a read-only view use `ReadOnlySpan<T>` instead.

`Slice(start, length)` produces a sub-span that views into the same buffer. The slice's lifetime is the same as the original span's — both are borrows of the same source. The slice has the same access mode as its parent (a `Span<T>` slices to a `Span<T>`; a `ReadOnlySpan<T>` slices to a `ReadOnlySpan<T>`).

The **Range overload** `Slice(r: Range<int>)` (#2662) is the view counterpart of the `AsSpan(r: Range<int>)` factories above: the range is normalized against the span's `Length` under the standard `Range<int>` endpoint rules — closed (`a..b`), half-open (`a..<b`), and open-ended (`a..`, `..b`, `..`) forms per language spec [§16.3](Bezel_Language_Spec.md#sec-the-ranget-type) — and normalization **validates eagerly**, like the `AsSpan(r)` factories and unlike the direct constructors and the `(start, length)` overload (the no-eager-check rule, below): out-of-range endpoints raise `IndexOutOfRangeException`, and an empty range produces an empty span. After normalization the result is the `Slice(start, length)` view: the same receiver-typed view over the same source, same lifetime, same access mode. The overload is declared uniformly on all five span-shaped types — `Span<T>` and `ReadOnlySpan<T>` here, `ArraySpan<T>`/`ReadOnlyArraySpan<T>` ([§4.15](#sec-arrayspant-readonlyarrayspant)), and `ReadOnlyStringSpan` ([§3.3](#sec-readonlystringspan)) — with this same contract. (Contrast `Array<T>.Slice(r)`, [§4.4](#sec-arrayt): a different receiver with its own declared contract — it materializes a fresh owned array, where a span's `Slice` never copies.)

`AsReadOnly()` returns a `ReadOnlySpan<T>` viewing the same buffer range. Bezel has no implicit record-to-record conversions, so passing a `Span<T>` where a `ReadOnlySpan<T>` is expected requires the explicit call. This is friction the C# precedent does not have (C# admits an implicit conversion from `Span<T>` to `ReadOnlySpan<T>`); Bezel's choice trades that convenience for the broader principle that record-type identity is nominal and conversions between records are always explicit. The `AsReadOnly()` call is cheap (record construction, no allocation) and makes the API-boundary intent visible at the call site.

**Iteration.** `for var x in span do ... end` is admitted for any T. The desugaring is a direct indexed loop over the span's range, parallel to the inline-array rule of [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) of the language spec: spans do not implement `IEnumerable<T?>` (the enumerator that `IEnumerable<T>.GetEnumerator()` returns is a heap object that would hold the span's borrow across its own, unbounded lifetime, which the borrow tracker cannot bound — a *different* situation from the **call-bounded** box of a span as the receiver of a member call, which the language spec [§8.6](Bezel_Language_Spec.md#sec-boxing-of-borrow-bearing-records) permits because it cannot escape the borrow's source scope), so the standard enumerator path does not apply. The iteration variable's type follows the slot type: `T?` for class T, `T` for value T. For class T, the loop body sees a borrow of each slot's value; for value T, a copy of the slot's value carrying the slot's borrows (for primitive T, simply the value) — a member store through the variable updates the copy, never the slot (spec §29.2, note 2). To consume slot contents inside the loop body for class T, use `move span[i]`, which stores `null` into the underlying slot in the same step (language spec §28.8.1).

**Search and equality.** `IndexOf` walks the span's range and returns the first index whose slot's value equals the target. The comparison uses `Equals` (records, classes, primitives), null-safe for class T, and a call at an element type with no equality is rejected ([§4](#sec-collection-equality-search)). `Contains` is `IndexOf(value) >= 0`. `SequenceEqual` compares two spans element-by-element; lengths differ → false; otherwise pairwise `Equals`. Every span type declares it for its own type and for the read-only span of each heap backing — here `Span<T>`, `ReadOnlySpan<T>` and `ReadOnlyArraySpan<T>` ([§4.15](#sec-arrayspant-readonlyarrayspant)) — so a comparison across backings needs no common type, and a writable span passed as the argument converts by `.AsReadOnly()`. The overloads are declared at the receiver's own `T`: at a class `T` the buffer span's `T?` slots compare with an array span's `T` slots, `Equals` null-safe on the nullable side, so a null slot equals no element. The span types implement no common interface ([§4.16](#sec-ireadonlyspan)); a member that accepts a read-only span of either heap backing declares one overload per span type, each the direct-dispatch path.

**Bulk mutation — class T.** `Clear()` assigns `null` to every slot in the span's range. Each assignment goes through the underlying buffer's owned-setter, which disposes the slot's previous contents. After `Clear()`, every slot in the range is null; slots outside the span's range are unaffected.

`MoveFrom(source: Span<T>)` requires `source.Length` to equal `this.Length` (else `ArgumentException` is raised). It transfers ownership slot-by-slot: for each `i`, `move source[i]` takes the source slot's contents, then `this[i] := value` stores those contents in the destination (disposing the destination's previous contents). After `MoveFrom`, every slot in the source span is null; every slot in the destination span holds what the source previously held.

`MoveTo(destination: Span<T>)` is the symmetric form. It is equivalent to `destination.MoveFrom(this)` and provided for the ergonomic case where the caller holds the source and writes left-to-right: `source.MoveTo(dest)` reads naturally where `dest.MoveFrom(source)` would require introducing the destination first. Same length requirement, same `ArgumentException` on mismatch, same per-slot move semantics.

**Overlap behavior.** When the source and destination spans view overlapping ranges of the same underlying buffer (which is admitted by the borrow tracker — multiple spans into the same buffer are normal), the bulk-copy and bulk-move operations process slots in implementation-defined order. For non-overlapping ranges (the typical case) the order is invisible to programs. For overlapping ranges, programs that depend on a particular processing order are wrong; if the source and destination overlap, callers should either copy via an intermediate buffer or use slot-by-slot loops to control the order explicitly. This applies to `CopyFrom`, `CopyTo`, `MoveFrom`, and `MoveTo` on all span types.

`Reverse()` reverses the slot ordering within the span's range, in place. Implementation pattern: pairwise swap via `move` and re-assign, working from both ends toward the middle.

**Bulk mutation — value T.** `Fill(value: T)` copies `value` into every slot. Because values are not owned references, the operation is a plain copy — no contradictions with ownership semantics arise.

`CopyFrom(source: ReadOnlySpan<T>)` requires `source.Length` to equal `this.Length` (else `ArgumentException` is raised). It copies each value from the source span to the destination span, slot by slot. No move semantics involved (values are copied, not transferred).

`CopyTo(destination: Span<T>)` is the symmetric form. Equivalent to `destination.CopyFrom(this)` and provided for the same ergonomic reason as `MoveTo`. Same length requirement and same per-slot copy semantics. The `ReadOnlySpan<T>` (value T) form also gets `CopyTo(destination: Span<T>)` — the source side is read-only but the destination side is writable; this is the common "I have a read-only view, copy it into a writable buffer" pattern.

`Reverse()` reverses the slot ordering within the span's range, in place. Implementation pattern: pairwise swap via plain reads and writes (no `move` for value-typed spans).

**Why the mutation methods differ by category.** For class T, the slots own their contents; bulk-assigning a single value would either share a class reference into N slots (forbidden by Bezel's ownership model — one owner per reference) or require synthesizing N independent class instances (Bezel does not provide implicit cloning). The only bulk-transfer that works is move-from-another-span (one ownership root per slot, transferring intact). For value T, none of these constraints apply — values are copies by default, so `Fill(value)` (broadcast) and `CopyFrom(source)` (per-slot copy) are both well-defined. The category-dependent mutation API is a direct consequence of the ownership-vs-value-semantics distinction.

<a id="sec-readonlyspant"></a>
#### `ReadOnlySpan<T>`

The declaration's surface mirrors `Span<T>`'s but exposes only the read-side operations.

For class-typed T:

```bezel
public record ReadOnlySpan<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)
  // class T

  public property [i: int]: T?
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Buffer[Start + i];
    end;
  end;

  public property IsEmpty: bool => Length = 0;

  public method IndexOf(value: T?): int;
  public method Contains(value: T?): bool;

  public method SequenceEqual(other: ReadOnlySpan<T>): bool;
  public method SequenceEqual(other: ReadOnlyArraySpan<T>): bool; // — the read-only span of the other backing (§4.15)

  public method Slice(start: int, length: int): ReadOnlySpan<T> =>
    new ReadOnlySpan<T>(Buffer, this.Start + start, length);
  public method Slice(start: int): ReadOnlySpan<T> => Slice(start, Length - start);
  public method Slice(r: Range<int>): ReadOnlySpan<T>;   // — Range form: normalized against
                                                          //   Length (spec §16.3), then equivalent to
                                                          //   Slice(start, length); see prose
end ReadOnlySpan;
```

For value-typed T:

```bezel
public record ReadOnlySpan<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)
  // value T

  public property [i: int]: T
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Buffer[Start + i];
    end;
  end;

  public property IsEmpty: bool => Length = 0;

  public method IndexOf(value: T): int;
  public method Contains(value: T): bool;

  public method SequenceEqual(other: ReadOnlySpan<T>): bool;
  public method SequenceEqual(other: ReadOnlyArraySpan<T>): bool; // — the read-only span of the other backing (§4.15)

  public method Slice(start: int, length: int): ReadOnlySpan<T> =>
    new ReadOnlySpan<T>(Buffer, this.Start + start, length);
  public method Slice(start: int): ReadOnlySpan<T> => Slice(start, Length - start);
  public method Slice(r: Range<int>): ReadOnlySpan<T>;   // — Range form: normalized against
                                                          //   Length (spec §16.3), then equivalent to
                                                          //   Slice(start, length); see prose

  public method CopyTo(destination: Span<T>);     // — copy slot-by-slot to destination;
                                                   //   lengths must match
end ReadOnlySpan;
```

A read-only counterpart to `Span<T>`. Mechanically, `ReadOnlySpan<T>` is the same shape as `Span<T>` but exposes only the indexer getter — no setter, no `move`-from-slot (class T), no bulk mutation. The type distinction is real and present-day, not a name-squat: an API accepting `ReadOnlySpan<T>` cannot mutate the underlying buffer through the span, even if it happens to be backed by the same buffer that another span (`Span<T>`-typed) is writing to. Callers can construct a `ReadOnlySpan<T>` from any source that produces a `Span<T>` view — including from a span obtained via owner-internal access — and pass it to code that should not gain write capability. Code that needs read-only access takes `ReadOnlySpan<T>`; code that needs write access takes `Span<T>`. The C# precedent is the same split; the names line up for readers familiar with that ecosystem.

`Slice` returns `ReadOnlySpan<T>` (read-only slice of a read-only span). There is no widening operation from `ReadOnlySpan<T>` to `Span<T>` — once read-only, always read-only through that handle. To regain write access, the code path must be reached through a span constructed against the underlying buffer directly.

**Iteration.** `for var x in roSpan do ... end` is admitted, with the same direct-indexed-loop desugaring as `Span<T>`. See the iteration paragraph under `Span<T>` above.

**Search and equality.** `IndexOf`, `Contains`, and `SequenceEqual` mirror `Span<T>`'s read-only methods. `SequenceEqual(other: ReadOnlySpan<T>)` compares two read-only spans element-by-element. `SequenceEqual(other: ReadOnlyArraySpan<T>)` compares against the array-backed read-only span ([§4.15](#sec-arrayspant-readonlyarrayspant)); to compare against a writable span of either backing, the caller converts it via `.AsReadOnly()`.

**No materialization on this family.** `ReadOnlySpan<T>` declares no `ToArray(): owned Array<T>`, and the compiler never admitted the call: the span family's member surface is a closed allow-list ending in a *no member* diagnostic that enumerates the surface and omits the name, so for the fifty-five revisions the declaration stood this document asserted a call that does not compile — and no call site of it exists in any document, guide, or test. The absence is now uniform across the two span families: [§4.15](#sec-arrayspant-readonlyarrayspant) records the array-backed one, whose source `Array<T>` carries its own `ToArray()` ([§4.4](#sec-arrayt)). It is a recorded decision rather than a gap. To move a buffer-backed span's contents into the collection world, copy with a loop or an explicit `Array<T>`. For class-typed `T` no materializer was ever admissible in the first place — the buffer owns the class slots, and materializing them would require duplicating ownership or implicit cloning, both foreclosed — so consume those with `move span[i]` or through the owning collections' transfer surface. The role `ToString()` plays on `ReadOnlyStringSpan` ([§3.3](#sec-readonlystringspan)) has no counterpart here. **Recorded liftable:** a materializer for *both* span families, specified together and implemented rather than only declared, is a standard-library pass on its own merits.

**Why not stack-allocated like C#'s `Span<T>`?** C#'s `Span<T>` is a `ref struct` — stack-only, cannot escape to the heap. This gives it the strongest possible lifetime guarantees but constrains where it can appear (no `Span<T>` fields in classes, no `Span<T>` in async state machines, no `Span<T>` returned across await points). Bezel's `Span<T>` is an ordinary record subject to borrow-bearing-record rules; it lives wherever the borrow-lifetime rules admit a value that holds borrows — a local, a parameter, a return over a receiver-sourced buffer (*Construction patterns* below), a component of another such value, a slot of a method-local collection its source outlives — never a class instance field or a static (language spec [§9.7](Bezel_Language_Spec.md#sec-records-and-disposal)'s instance-field and static-field bullets: a span declares its `Buffer` non-nullable, so it has no null-source form), and, for a span *local* whose source is co-resident in the same async frame (an owned local, `using var`, or `owned` parameter of that method), across `await` points in that method. A span may **not** be an async method's parameter or result — a caller-side source cannot be shown to outlive the frame (spec §30.1, #1438). Programs whose buffer lifetimes cannot be statically proven sufficient are rejected by the borrow tracker. The Bezel choice trades the unconditional stack-only safety of C# for a much smaller language footprint — no new "ref struct" type kind is introduced. The lifetime tracker handles the common cases (parser scratch buffers, byte-buffer reads, slice-and-walk patterns, async pipelines whose buffer outlives the awaits) without a special type kind.

**Construction patterns.** A buffer's owner produces spans by handing out `new Span<T>(buf, start, length)` or `new ReadOnlySpan<T>(buf, start, length)` directly, or by exposing helper methods. The direct constructors of the five span records do **not** range-check `(Start, Length)` against the source: a mis-constructed span faults at each out-of-range *access* instead — the span-level check passes and the source's own bounds check throws `IndexOutOfRangeException` (the §4.15 caveat describes the same composition). The `AsSpan`/`AsReadOnlySpan` factories validate eagerly and are the recommended construction path:

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

In either case, the returned span borrows the `FrameBuffer` instance (transitively, through the `_pixels` field). Callers cannot retain the span past the `FrameBuffer`'s lifetime — the borrow tracker rejects code that would. A replacement of `_pixels` inside the `FrameBuffer` while a caller still holds a span over it — a `Reset()` that assigns the field — disposes the viewed buffer under that span: a field slot is not frozen (language spec [§28.3](Bezel_Language_Spec.md#sec-consume-site-freeze), K5.6), and the caller's next read through the span is checked disposal mode's `DisposedAccessException` (language spec [§28.2](Bezel_Language_Spec.md#sec-checked-disposal)), the posture every borrow of a slot's current value carries.

<a id="sec-byte-buffer-accessors"></a>
#### Byte-buffer accessors

When T is `byte`, the spans are commonly used for binary I/O — file formats, network protocols, image headers, serialization. The endianness-aware accessors below read and write multi-byte numeric types without bit-shuffling at the call site. The **read tier** is declared on the read-only span of each heap backing: `extension ReadOnlySpan<byte>` below, and `extension ReadOnlyArraySpan<byte>` ([§4.15](#sec-arrayspant-readonlyarrayspant)) with the same members, name for name and with identical semantics. A writable byte span — `Span<byte>`, or §4.15's `ArraySpan<byte>` — reads through `AsReadOnly()` (`sp.AsReadOnly().ReadInt32LittleEndian()`), a record construction with no allocation; an accessor body reads only `this.Length` and `this[i]` and returns a value. The **write tier** is declared per writable span.

```bezel
public extension ReadOnlySpan<byte>                        // public: spec §26.1 defaults extensions to internal; §4.15 declares the same block on ReadOnlyArraySpan<byte>
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

public extension Span<byte>
  // Reads and TryReads: through AsReadOnly() — the ReadOnlySpan<byte> block above;
  // nothing is re-declared here.

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

**No offset parameter** ([§1](#sec-conventions), *Window parameters*). To read at an offset, slice first: `header.Slice(8).ReadInt32LittleEndian()`. `Slice` is cheap (record construction, no allocation). The alternative (offset as a parameter) would split the API into two access patterns and add one parameter per method, both costs the slice-then-read form avoids.

**Bounds-check semantics.** Read methods throw `IndexOutOfRangeException` if the span is shorter than the type being read (e.g. `ReadInt32LittleEndian` on a 3-byte span). Write methods throw the same exception if the span is too short to hold the value being written.

`TryRead*` variants return `false` instead of throwing and assign the `out` parameter to `default(T)` on the false path (per the language spec [§22.3](Bezel_Language_Spec.md#sec-parameter-modifiers) requirement that every `out` parameter be assigned on every return path; throw paths carry no obligation). They suit incremental parsers that accept short inputs as end-of-stream rather than as errors.

`TryWrite*` variants return `false` instead of throwing on a too-short destination span. On the false path the span is **not modified** — the write is atomic (either every byte of the value is written, or none is). This matters for code that uses a single shared buffer for multiple writes and falls back to a different output path on overflow; the unmodified state is the invariant the fallback path can rely on.

**Float bit handling.** `ReadFloatLittleEndian` reads four bytes into an int value, then invokes `BitConverter.Int32BitsToFloat` ([§2.12](#sec-bitconverter)) to reinterpret the bits as a `float`. The double-precision form uses `BitConverter.Int64BitsToDouble`. Both preserve NaN bit patterns exactly. Users with explicit bit-pattern needs (custom encodings, byte-twiddling outside the standard formats) can call `BitConverter` directly.

**Example implementation (one Read, one Write).** The remaining methods follow the same pattern.

```bezel
public extension ReadOnlySpan<byte>
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

public extension Span<byte>
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

<a id="sec-bit-granular-accessors"></a>
#### Bit-granular accessors

A tier below the byte accessors, for formats that pack fields at sub-byte positions — a protocol header whose flag, opcode, and code fields share a byte; a stream of variable-width codes. `BitReader` and `BitWriter` are records carrying a span plus a `(BitStart, BitLength)` window counted in **bits**; every read and write covers a run of 1 to 64 bits and needs no mask-and-shift ladder at the call site.

> ⚠ **Spec-ahead-of-implementation ([#3283](https://github.com/s8lap/bezel-lang/issues/3283)):** the bit tier is not implemented — `BitReader`, `BitWriter`, their reads, writes, `TryRead*`/`TryWrite*` forms, slices and entry points have no sema or codegen arm. The byte tier's `TryWrite*` forms are implemented. The `TryWrite*` forms' design contract is the byte tier's (its recorded write-side reason covers both; [#3558](https://github.com/s8lap/bezel-lang/issues/3558)).

```bezel
public record BitReader(Source: ReadOnlySpan<byte>, BitStart: int, BitLength: int)

  public pure property IsEmpty: bool => BitLength = 0;
  public pure property IsByteAligned: bool => BitStart mod 8 = 0;

  // Reads — from bit 0 of the view; count in 1..64
  public method ReadBitsBigEndianMsbFirst(count: int): ulong;
  public method ReadBitsLittleEndianLsbFirst(count: int): ulong;
  public method ReadBitMsbFirst(): bool;
  public method ReadBitLsbFirst(): bool;

  // Try variants — return false instead of throwing on a short view
  public method TryReadBitsBigEndianMsbFirst(count: int, out value: ulong): bool;
  public method TryReadBitsLittleEndianLsbFirst(count: int, out value: ulong): bool;
  public method TryReadBitMsbFirst(out value: bool): bool;
  public method TryReadBitLsbFirst(out value: bool): bool;

  // Repositioning — the §4.14 Slice family, counted in bits
  public pure method Slice(bitStart: int): BitReader;
  public pure method Slice(bitStart: int, bitLength: int): BitReader;
  public pure method Slice(r: Range<int>): BitReader; // Range form: normalized against
                                                       //   BitLength (spec §16.3), then
                                                       //   equivalent to Slice(start, length)
  public pure method AlignToByte(): BitReader;        // advance to the next byte boundary

  // The byte-aligned remainder — an ordinary sub-span, no bit machinery
  public method AlignedTail(): ReadOnlySpan<byte>;
end BitReader;

public record BitWriter(Source: Span<byte>, BitStart: int, BitLength: int)

  public pure property IsEmpty: bool => BitLength = 0;
  public pure property IsByteAligned: bool => BitStart mod 8 = 0;

  public method WriteBitsBigEndianMsbFirst(value: ulong, count: int);
  public method WriteBitsLittleEndianLsbFirst(value: ulong, count: int);
  public method WriteBitMsbFirst(value: bool);
  public method WriteBitLsbFirst(value: bool);

  // Try variants — false on a short view; the destination is not modified
  public method TryWriteBitsBigEndianMsbFirst(value: ulong, count: int): bool;
  public method TryWriteBitsLittleEndianLsbFirst(value: ulong, count: int): bool;
  public method TryWriteBitMsbFirst(value: bool): bool;
  public method TryWriteBitLsbFirst(value: bool): bool;

  public pure method Slice(bitStart: int): BitWriter;
  public pure method Slice(bitStart: int, bitLength: int): BitWriter;
  public pure method Slice(r: Range<int>): BitWriter;
  public pure method AlignToByte(): BitWriter;

  public method AlignedTail(): Span<byte>;
  public method AsReadOnly(): BitReader;
end BitWriter;

// Entry points, parallel to AsSpan / AsReadOnlySpan
public extension ReadOnlySpan<byte>
  public pure method AsBitReader(): BitReader;
end;

public extension Span<byte>
  public method AsBitReader(): BitReader;             // delegates via AsReadOnly()
                                                       //   (unmarked), so not `pure`
  public pure method AsBitWriter(): BitWriter;
end;
```

**Purity.** A member carries `pure` (language spec [§22.15](Bezel_Language_Spec.md#sec-pure-members)) exactly when it needs nothing but reads of the view's own components and the construction of a record: `IsEmpty`, `IsByteAligned`, the three `Slice` overloads, and `AlignToByte` on every type of this tier, plus the two entry points that construct without delegating — `ReadOnlySpan<byte>.AsBitReader()` and `Span<byte>.AsBitWriter()` (and their §4.15 mirrors), each of which reads only the source span's `Length`, a positional component whose auto-property getter is pure by classification (§22.15's *Members classified by rule*), and constructs a record through a compiler-synthesized positional constructor. That set is what a contract clause can call ([§22.16](Bezel_Language_Spec.md#sec-contract-clauses)), and it is stated at declaration time because purity cannot be retrofitted by inference. The rest carry no marker, each for a stated reason. **The writes store** — the four `TryWrite*` forms included: a declined write stores nothing, but the succeeding one does, and purity is a property of the declaration, not of a path. **Each `TryRead*` form additionally declares an `out` parameter**, which §22.15's declaration rules exclude outright (the `TryWrite*` forms declare none — they are stdlib [§1](#sec-conventions)'s bool-only shape (d) — so the `out` rule is not what stops them). And **the reads, `AlignedTail`, `AsReadOnly`, and the *delegating* entry point** — `Span<byte>.AsBitReader()`, and its §4.15 mirror `ArraySpan<byte>.AsBitReader()`, which reach their read-only source through the span's own unmarked `AsReadOnly()`, there being no implicit record-to-record conversion (§4.14, *`Span<T>`*) — reach **through the source span**: its indexer, its `Slice`, its `AsReadOnly`, none of which the §4.14/§4.15 span families mark, so a marker here would state a contract §22.15's calls-to-pure-members rule could not admit. (This is the one structural difference from `ReadOnlyStringSpan` ([§3.3](#sec-readonlystringspan)), whose **source-reading members** — its search, comparison, and `ToString` forms — do carry `pure`, because the type it reads through, `string`, has a `pure` indexer ([§9.6](#sec-string)); this tier's sources carry no such marker, so the same members here could not take one. The difference is *availability of the marker*, not a claim that every non-mutating member there is marked: `ReadOnlyStringSpan`'s own indexer is unmarked, as every span indexer in this document is. A purity pass over the span families — deferred, `DECLINED.md` — would let the remaining members of this tier take the marker as a widening.)

**Bit order and byte order in the method name.** Each multi-bit method names both axes, exactly as the byte accessors name endianness: there is no bit-order parameter, no constructor argument, and no default. A constructor argument would be worse than the enum parameter the byte tier already rejects — it moves the decision further from the call site that depends on it.

**Bit numbering.** Let *k* be the absolute bit index `BitStart + i` for view bit *i*. Under **MsbFirst**, bit *k* is bit `7 - (k mod 8)` of byte `k / 8`, and the first bit read or written is the **most** significant bit of the value; under **LsbFirst**, bit *k* is bit `k mod 8` of byte `k / 8`, and the first bit is the **least** significant.

**Two families, and why these two.** The two axes are independent, so four combinations are nameable; two are declared, because those two obey an agreement law with the accessors above. At a byte-aligned position, for `count` in {16, 32, 64}, `ReadBitsBigEndianMsbFirst(count)` yields exactly what `ReadUInt16BigEndian` / `ReadUInt32BigEndian` / `ReadUInt64BigEndian` yields there, and `ReadBitsLittleEndianLsbFirst(count)` exactly what the matching `…LittleEndian` accessor yields; the write side agrees the same way. Each declared family is therefore the bit-granular generalization of a byte accessor, and its name says which. The mixed combinations (big-endian with LSB-first, little-endian with MSB-first) are **not declared**: they generalize no accessor of the byte tier, so a compound name would promise a correspondence there is nothing to check against. A field packed in a mixed order is read with the per-bit members — a code packed most-significant-bit-first into an LSB-first stream is `ReadBitLsbFirst` accumulated most-significant-bit-first by the caller, which is the loop a variable-width-code decoder walks anyway. The naming scheme is additive if a named mixed family is ever warranted.

**No offset parameter; slice to advance** ([§1](#sec-conventions), *Window parameters*, at bit granularity). To walk a sequence of fields, slice between operations:

```bezel
var r      := header.AsBitReader();
var id     := ushort(r.ReadBitsBigEndianMsbFirst(16));  r := r.Slice(16);
var qr     := r.ReadBitMsbFirst();                      r := r.Slice(1);
var opcode := byte(r.ReadBitsBigEndianMsbFirst(4));     r := r.Slice(4);
```

`Slice` is record construction, not allocation, and a peek costs nothing (read without slicing). Its bounds model is the byte tier's, counted in bits — **including the byte tier's split between the two overload shapes**, which is the whole of the correspondence and is stated rather than approximated. The `(bitStart)` and `(bitStart, bitLength)` forms do **not** range-check their arguments against the view, exactly as the span records' direct constructors and `(start, length)` `Slice` do not (the no-eager-check rule, *Construction patterns* above): a window outside the view is not a fault at the slice, it is a fault at the first access that leaves the source, where the source span's own bounds check raises `IndexOutOfRangeException` — the same composition the byte tier documents, one layer deeper. The `Range<int>` form **does** validate eagerly, again as the byte tier's does (§4.14's contract): the range is normalized against `BitLength` under language spec [§16.3](Bezel_Language_Spec.md#sec-the-ranget-type)'s endpoint rules, out-of-range endpoints raise `IndexOutOfRangeException` at the call, and an empty range produces an empty view. The asymmetry is inherited deliberately rather than repaired here: repairing it in the bit tier alone would make one tier's `Slice` eager and the neighbouring tier's lazy under the same name — precisely the divergence a mirrored surface exists to prevent — and the lazy shape is the one this tier's own totality convention already assumes — `AlignToByte()` likewise declines to fault at the reposition and lets the over-run surface at the next read. A reader does not advance itself: in an instance member of a value kind `this` is a borrow of the receiver, so a store into the receiver's own storage is a compile error (language spec [§9.4](Bezel_Language_Spec.md#sec-value-receivers-are-borrows)) — the position is part of the value, and each read is total on the value it is given. The cost of that shape is stated rather than hidden: each width appears twice, in the read and in the slice, and a mismatch between them is silent — it reads the neighbouring bits rather than faulting. Both numbers sit on one line, and an over-run of the view faults (below) rather than reading past it.

**One result type.** Every multi-bit read returns `ulong` and every multi-bit write takes `ulong`, for every width in 1..64; the byte tier next door is the typed tier, serving every byte-aligned width under a name that carries it. Narrowing at the call site is safe by construction: Bezel's narrowing casts are checked (language spec [§5.2](Bezel_Language_Spec.md#sec-explicit-conversions)), and a `count`-bit value is representable in every primitive of at least `count` bits, so `ushort(r.ReadBitsBigEndianMsbFirst(16))` cannot overflow. There is no signed form — a two's-complement field is one sign-extension expression on the `ulong`.

**Bounds-check semantics.** Read and write methods throw `IndexOutOfRangeException` if the view holds fewer than `count` bits. A `count` outside 1..64 throws `ArgumentOutOfRangeException`. `AlignedTail()` on a view whose `BitStart` is not byte-aligned throws `InvalidOperationException`.

`TryRead*` variants return `false` instead of throwing and assign the `out` parameter to `default(T)` on the false path, as the byte tier's do. One consequence is worth stating at the surface: `TryReadBitMsbFirst(out value: bool)` assigns `false` on the false path, which is indistinguishable from a legitimately-read zero bit — callers branch on the return value, never on the out.

`TryWrite*` variants return `false` on a view too short to hold the run. On the false path the destination is **not modified**, the byte tier's atomicity guarantee at bit granularity.

**Partial bytes.** A write that begins or ends inside a byte preserves that byte's other bits (read-modify-write of the two edge bytes). `AlignToByte()` returns the view repositioned at the next byte boundary — the same view when it is already aligned, and the **empty view at the end** (`BitLength = 0`) when the next boundary lies past the view's end, so the operation is total and the over-run is observed at the next read rather than at the alignment. `AlignedTail()` returns the whole-byte remainder `Source.Slice(BitStart / 8, BitLength / 8)`; a trailing partial byte (`BitLength mod 8 <> 0`) is excluded rather than silently folded in, and the composition with the byte accessors is direct: `r.AlignedTail().ReadUInt32BigEndian()`.

**Borrows and lifetimes.** Both records are **borrow-bearing** (language spec [§9.7](Bezel_Language_Spec.md#sec-records-and-disposal)) by transitivity: the `Source` component is a span, itself borrow-bearing, so a bit view's lifetime is bounded by its source's exactly as the span's is by the buffer's. Every consequence is the span family's, unchanged — no instance field of a class, no static field, no escaping capture, no construction from a temporary; not an `async` method's parameter or result, while a view over a span whose source is co-resident in the same async frame (an owned local, `using var`, or `owned` parameter of that method) may live across `await` points in that method. Nothing here owns anything: records carry no `owned` fields and no destructors.

Like the span records, both are purely user-level Bezel — no compiler intrinsic. The implementation is mask-and-shift over the borrowed bytes, written once:

```bezel
public record BitReader(Source: ReadOnlySpan<byte>, BitStart: int, BitLength: int)
  public method ReadBitMsbFirst(): bool
    if BitLength < 1 then
      throw new IndexOutOfRangeException();
    end;
    var k := BitStart;
    return (int(Source[k / 8]) & (1 << (7 - (k mod 8)))) <> 0;
  end ReadBitMsbFirst;
end BitReader;
```

<a id="sec-arrayspant-readonlyarrayspant"></a>
### 4.15 `ArraySpan<T>`, `ReadOnlyArraySpan<T>`

Records carrying borrows of an `Array<T>` ([§4.4](#sec-arrayt)) plus a `(Start, Length)` range. The natural companion to `Array<T>.AsSpan()` and `Array<T>.AsReadOnlySpan()` for passing a sub-range of an array to a method without copying.

`ArraySpan<T>` is the writable form; `ReadOnlyArraySpan<T>` is the read-only form. The split parallels `Span<T>` / `ReadOnlySpan<T>` ([§4.14](#sec-owned-buffers-and-spans)) but with `Array<T>` as the source instead of `OwningBuffer<T>`. The two span families coexist; they are not interchangeable (a method that takes `Span<T>` does not accept `ArraySpan<T>` and vice versa) because the underlying storage models differ — `OwningBuffer<T>` owns its slots for class T, while `Array<T>` borrows them. Both types declare the three `Slice` overloads of the §4.14 family, including the `Slice(r: Range<int>)` Range form under [§4.14](#sec-owned-buffers-and-spans)'s normalization contract.

**Byte-buffer accessors (#1358):** the §4.14 endianness-aware read tier — every `Read*{Little,Big}Endian`/`TryRead*` form — is declared again as `extension ReadOnlyArraySpan<byte>`, member for member and with identical semantics (`ArraySpan<byte>` reads through `AsReadOnly()`, as `Span<byte>` does); the `Write*`/`TryWrite*` forms are mirrored onto `ArraySpan<byte>` with identical semantics (no offset parameters: [§1](#sec-conventions), *Window parameters*). This closes the file→binary-parse path: `File.ReadAllBytes(p).AsReadOnlySpan().ReadUInt32LittleEndian()` is direct and zero-copy. The chained form is admitted although `File.ReadAllBytes` returns `owned Array<byte>` unbound: §4.14's *no construction from a temporary* is language spec [§9.7](Bezel_Language_Spec.md#sec-records-and-disposal)'s syntactic rule on a `new`-expression's **construction argument**, and the factory route supplies `this` — a named location — instead; the array is an unbound owned temporary whose borrowing uses, receiver position included, run before its end-of-full-expression disposal (spec [§28.3](Bezel_Language_Spec.md#sec-unbound-owned-temporaries)). **Binding** the view is the rejected shape: `var s := File.ReadAllBytes(p).AsReadOnlySpan();` leaves `s` borrowing a source disposed at that statement's end. Bind the array, not the span — `using var bytes := File.ReadAllBytes(p); var s := bytes.AsReadOnlySpan();`.

**Bit-granular accessors:** the §4.14 bit tier is mirrored the same way, as `ArrayBitReader` and `ArrayBitWriter` — see [below](#sec-array-bit-granular-accessors).

<a id="sec-arrayspant"></a>
#### `ArraySpan<T>`

```bezel
public record ArraySpan<T>(Source: Array<T>, Start: int, Length: int)

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

  public property IsEmpty: bool => Length = 0;

  // Range construction
  public method Slice(start: int, length: int): ArraySpan<T> =>
    new ArraySpan<T>(Source, this.Start + start, length);
  public method Slice(start: int): ArraySpan<T> => Slice(start, Length - start);
  public method Slice(r: Range<int>): ArraySpan<T>;   // — Range form: normalized against
                                                       //   Length (spec §16.3), then equivalent to
                                                       //   Slice(start, length); §4.14's contract

  public method AsReadOnly(): ReadOnlyArraySpan<T> =>
    new ReadOnlyArraySpan<T>(Source, Start, Length);

  // Search
  public method IndexOf(value: T): int;
  public method Contains(value: T): bool;

  // Equality
  public method SequenceEqual(other: ArraySpan<T>): bool;
  public method SequenceEqual(other: ReadOnlyArraySpan<T>): bool; // — the read-only span of this backing
  public method SequenceEqual(other: ReadOnlySpan<T>): bool;      // — the read-only span of the other backing (§4.14)

  // Bulk mutation
  public method Fill(value: T);                              // copy `value` into every slot
  public method CopyFrom(source: ReadOnlyArraySpan<T>);      // copy slot-by-slot; lengths must match
  public method CopyTo(destination: ArraySpan<T>);           // — copy slot-by-slot to destination;
                                                              //   lengths must match
  public method Reverse();                                    // reverse slot ordering in place
end ArraySpan;
```

An `ArraySpan<T>` is a borrow-bearing record ([§9.7](Bezel_Language_Spec.md#sec-records-and-disposal) of the language spec) viewing into a contiguous range of an `Array<T>`. The `Source` field carries a borrow of the array; per the borrow-bearing-record rule, the span's lifetime is bounded by the source's lifetime.

`ArraySpan<T>` is purely user-level Bezel — no compiler intrinsic. The record machinery, borrow lifetime tracking, and indexer dispatch are all standard language features. So is the disposal backstop: a read or write through the span reaches the array through `Array<T>`'s own indexer, and on a logically-disposed array it faults as any member access on a disposed instance does (language spec [§28.2](Bezel_Language_Spec.md#sec-checked-disposal), checked disposal mode); the span's own components (`Start`, `Length`, `IsEmpty`, `Slice`) touch no storage.

**Read access.** `span[i]` returns the value at position `Start + i` in the source array. The return type follows the array's element type — `T` is whatever the array was instantiated with (a class type for `Array<MyClass>`, a nullable class type for `Array<MyClass?>`, a value type for `Array<int>`). Unlike `Span<T>` over `OwningBuffer<T>`, there is no `T?` wrapping; the source array does not store nullable wrappers for class T (the array stores borrows directly, and a non-nullable `Array<MyClass>` has non-null borrow slots).

**Write access.** `span[i] := value` stores `value` into the source array's slot at `Start + i`. For class T (whether nullable or not), the assigned value is a borrow — no ownership transfer; the previous slot contents are not disposed (they were borrows, not owned values). For value T, the value is copied into the slot. The setter takes the value with the default parameter convention (no `owned` modifier); this differs from `Span<T>`'s class-T setter, which takes `owned T?` because the underlying `OwningBuffer<T>` owns its slots.

`move arrSpan[i]` is **not admitted** for any T. Borrowed slots have no ownership to transfer; the plain indexer-get is the only read form. This differs from class-typed `Span<T>` (which admits `move` because `OwningBuffer<T>` slots are owned).

**Bounds-check semantics.** The indexer bounds-checks against the span's `Length` (not just the underlying array's `Count`); `IndexOutOfRangeException` is thrown if `i < 0` or `i >= Length`. The same rule applies to the setter. This matches the §4.14/§3.3 bounds-check rule.

**Iteration.** `for var x in arrSpan do ... end` is admitted. The desugaring is a direct indexed loop over the span's range, parallel to the OwningBuffer-backed span rule of language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for). The iteration variable's type is T and the loop body sees a borrow of each slot (the borrow's source is the underlying array, reached through the span's `Source` field).

**Search and equality.** `IndexOf` walks the span's range, returning the first index whose slot equals `value`, or `-1` if none. `Contains` returns `IndexOf(value) >= 0`. The comparison uses `Equals` per the standard convention, and a call at an element type with no equality is rejected ([§4](#sec-collection-equality-search)). `SequenceEqual(other: ArraySpan<T>)` compares two writable array spans pairwise; `SequenceEqual(other: ReadOnlyArraySpan<T>)` and `SequenceEqual(other: ReadOnlySpan<T>)` compare against the read-only span of either backing — the rule [§4.14](#sec-owned-buffers-and-spans) states for `Span<T>`, a writable argument converting by `.AsReadOnly()`.

**Bulk mutation.** Unlike `Span<T>` (whose mutation API splits by T's category — `Clear`/`MoveFrom`/`MoveTo` for class T owning slots, `Fill`/`CopyFrom`/`CopyTo` for value T), `ArraySpan<T>` provides a single uniform mutation surface for all T: `Fill(value)` assigns `value` to every slot; `CopyFrom(source)` copies slot-by-slot from a source span (lengths must match, else `ArgumentException`); `CopyTo(destination)` copies slot-by-slot to a destination span (lengths must match); `Reverse()` reverses the slot ordering in place. The uniform API is admissible because `Array<T>` slots do not own their contents — there is no ownership transfer to worry about, no dispose-previous semantics, no need for the category-dependent split. For class T, the assigned-into slot stores a borrow (`Fill(myClass)` makes every slot borrow `myClass`); for value T, the slot stores a copy. Both fit naturally under `Fill`.

**No in-place modification of value slots.** Bezel has no ref returns ([§22.14](Bezel_Language_Spec.md#sec-no-params-no-ref-returns)), so `arrSpan[i].Field := x` (modifying a value-type slot in place through the span) is not available. The pattern is `var v := arrSpan[i]; v.Field := x; arrSpan[i] := v;` — read-modify-write through assignment. This limitation applies equally to `Array<T>` itself; the span inherits it.

**Construction.** Callers construct `ArraySpan<T>` via the `AsSpan()` family on `Array<T>` ([§4.4](#sec-arrayt)) rather than via the constructor directly. The direct constructor is admitted but is rarely needed.

**Caveat: array mutation during span lifetime.** Unlike `OwningBuffer<T>` (which has a fixed `Length`), `Array<T>`'s `Count` can change during the array's lifetime via `Add`, `Insert`, `Remove`, `RemoveAt`, and `Clear`. A span's `Length` is fixed at construction; subsequent mutation of the underlying array does not update the span. If the array shrinks below `Start + Length` after the span was constructed, accesses through the span (which pass the span's bounds check but then index into the array beyond its new `Count`) will throw `IndexOutOfRangeException` from the array's own bounds check. If the array grows or its contents shift via `Insert` or `RemoveAt`, the span's `(Start, Length)` continues to point at the same logical positions in the array — which may now hold different elements than the span originally viewed. This is a genuine hazard and a structural difference from the `OwningBuffer<T>`-backed spans (whose underlying buffer cannot resize). The intended usage pattern is short-lived spans: construct the span, pass it to a method that reads or writes through it, return — without resizing the source array in between. Programs that need a stable view across mutations should copy the relevant range into a fresh `OwningBuffer<T>` or `Array<T>` instead.

<a id="sec-readonlyarrayspant"></a>
#### `ReadOnlyArraySpan<T>`

```bezel
public record ReadOnlyArraySpan<T>(Source: Array<T>, Start: int, Length: int)

  public property [i: int]: T
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Source[Start + i];
    end;
  end;

  public property IsEmpty: bool => Length = 0;

  public method Slice(start: int, length: int): ReadOnlyArraySpan<T> =>
    new ReadOnlyArraySpan<T>(Source, this.Start + start, length);
  public method Slice(start: int): ReadOnlyArraySpan<T> => Slice(start, Length - start);
  public method Slice(r: Range<int>): ReadOnlyArraySpan<T>;   // — Range form: normalized against
                                                               //   Length (spec §16.3), then equivalent to
                                                               //   Slice(start, length); §4.14's contract

  public method IndexOf(value: T): int;
  public method Contains(value: T): bool;

  public method SequenceEqual(other: ReadOnlyArraySpan<T>): bool;
  public method SequenceEqual(other: ReadOnlySpan<T>): bool;      // — the read-only span of the other backing (§4.14)

  public method CopyTo(destination: ArraySpan<T>);            // — copy slot-by-slot to destination;
                                                               //   lengths must match
end ReadOnlyArraySpan;
```

A read-only counterpart to `ArraySpan<T>`. Mechanically the same shape but exposes only the indexer getter — no setter, and no bulk mutation *of the viewed range* (the declared `CopyTo(destination: ArraySpan<T>)` writes the destination, not the receiver — §4.14's read-only-source, writable-destination shape). The type distinction is real and present-day, not a name-squat: an API accepting `ReadOnlyArraySpan<T>` cannot mutate the underlying array through the span, even if it happens to be backed by the same array that another span (`ArraySpan<T>`-typed) is writing to. Callers construct a `ReadOnlyArraySpan<T>` from the source array directly (via `Array<T>.AsReadOnlySpan()`) or by calling `.AsReadOnly()` on an `ArraySpan<T>`.

`Slice` returns `ReadOnlyArraySpan<T>` (read-only slice of a read-only span). There is no widening operation from `ReadOnlyArraySpan<T>` to `ArraySpan<T>` — once read-only through that handle, write access requires reaching the underlying array through a different path.

The borrow-of-`Array<T>` lifetime story is the same as `ArraySpan<T>`'s.

**No `ToArray()` on either span family (a recorded decision).** Neither family materializes: [§4.14](#sec-owned-buffers-and-spans)'s *No materialization on this family* states it for the buffer-backed side, and this family declares no counterpart. (`DECLINED.md` carries the row, with the member's history.) The absence still costs least here, and that is still a decision rather than an oversight — a view on this side is over an `Array<T>`, which already carries `ToArray()` and `Slice(r: Range<int>): owned Array<T>` ([§4.4](#sec-arrayt)), so the materialization is reachable through the source. The residual is real and named: from a `ReadOnlyArraySpan<T>` handle alone the source array is reachable only through the `Source` component, so a caller given only the view materializes a sub-range with an explicit indexed copy — which is now the buffer-backed family's answer too. Recorded liftable **for both families together**; a one-sided mirror is declined on the price of the resulting asymmetry.

<a id="sec-array-bit-granular-accessors"></a>
#### Bit-granular accessors

> ⚠ **Spec-ahead-of-implementation ([#3283](https://github.com/s8lap/bezel-lang/issues/3283)):** `ArrayBitReader` and `ArrayBitWriter` are not implemented, as §4.14's bit tier is not.

The [§4.14](#sec-bit-granular-accessors) bit tier is mirrored onto this family as `ArrayBitReader` and `ArrayBitWriter`, member-for-member identical to `BitReader` and `BitWriter` with `ReadOnlyArraySpan<byte>` and `ArraySpan<byte>` substituted for the source type — the same device, and for the same reason, as the byte accessors' mirror above: the two span families are not interchangeable, and the dominant byte sources sit on opposite sides. `Stream.Read(buffer: Span<byte>)` ([§7.1](#sec-stream)) fills the buffer-backed family, while `File.ReadAllBytes` and `Stream.ReadAll()` return `owned Array<byte>`, whose `AsReadOnlySpan()` is the array-backed one. Without the mirror the file path could not reach the bit tier at all.

Every rule of §4.14's bit tier holds here verbatim: the two named families and their agreement law, bit numbering, the 1..64 `ulong` result, no offset parameter (slice to advance), the `IndexOutOfRangeException` / `ArgumentOutOfRangeException` / `InvalidOperationException` faults, the `Slice` bounds split (the `(bitStart[, bitLength])` forms do not range-check; the `Range<int>` form validates eagerly), the Try shapes and the `TryWrite*` atomicity, the partial-byte rules, and the borrow-bearing lifetime story. The purity convention carries over with the same membership, read against this family's sources: `IsEmpty`, `IsByteAligned`, the three `Slice` overloads, and `AlignToByte` carry `pure`, as does the constructing entry point on each side — `ReadOnlyArraySpan<byte>.AsBitReader()` and `ArraySpan<byte>.AsBitWriter()`, which read only `Length` and construct; nothing else does, `ArraySpan<byte>.AsBitReader()` included, since it too must delegate through the unmarked `AsReadOnly()`.

```bezel
public record ArrayBitReader(Source: ReadOnlyArraySpan<byte>, BitStart: int, BitLength: int)
  public pure property IsEmpty: bool => BitLength = 0;
  public pure property IsByteAligned: bool => BitStart mod 8 = 0;

  public pure method Slice(bitStart: int): ArrayBitReader;
  public method ReadBitsBigEndianMsbFirst(count: int): ulong;
  // ... (the full 15-member surface of BitReader, with ArrayBitReader for
  //      BitReader and ReadOnlyArraySpan<byte> for ReadOnlySpan<byte>
  //      throughout; omitted here for brevity)
  public method AlignedTail(): ReadOnlyArraySpan<byte>;
end ArrayBitReader;

public record ArrayBitWriter(Source: ArraySpan<byte>, BitStart: int, BitLength: int)
  public pure property IsEmpty: bool => BitLength = 0;
  public pure property IsByteAligned: bool => BitStart mod 8 = 0;

  public pure method Slice(bitStart: int): ArrayBitWriter;
  public method WriteBitsBigEndianMsbFirst(value: ulong, count: int);
  // ... (the full 16-member surface of BitWriter, with ArrayBitWriter for
  //      BitWriter and ArraySpan<byte> for Span<byte> throughout; omitted
  //      here for brevity)
  public method AlignedTail(): ArraySpan<byte>;
  public method AsReadOnly(): ArrayBitReader;
end ArrayBitWriter;

public extension ReadOnlyArraySpan<byte>
  public pure method AsBitReader(): ArrayBitReader;
end;

public extension ArraySpan<byte>
  public method AsBitReader(): ArrayBitReader;        // delegates via AsReadOnly()
                                                       //   (unmarked), so not `pure`
  public pure method AsBitWriter(): ArrayBitWriter;
end;
```

<a id="sec-ireadonlyspan"></a>
<a id="sec-ireadonlyspantelement"></a>
### 4.16 `IReadOnlySpan<TElement>` (retired)

Retired. There is no `IReadOnlySpan<TElement>`. The span types are four borrow-bearing records — `Span<T>` and `ReadOnlySpan<T>` over an `OwningBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)), `ArraySpan<T>` and `ReadOnlyArraySpan<T>` over an `Array<T>` ([§4.15](#sec-arrayspant-readonlyarrayspant)) — and the ordinary record `ReadOnlyStringSpan` over a `string` ([§3.3](#sec-readonlystringspan)). They implement no common interface, and every rule of the language spec reads them as the records they are (language spec [§9.7](Bezel_Language_Spec.md#sec-records-and-disposal)): no reference type is read as holding borrows, and a value that holds borrows converts to `Object` or an interface only as the receiver of a member call (language spec [§8.6](Bezel_Language_Spec.md#sec-boxing-of-borrow-bearing-records)). A member that accepts a read-only span of either heap backing declares two overloads, `(ReadOnlySpan<T>)` and `(ReadOnlyArraySpan<T>)`, each the direct-dispatch path; a writable span passes `.AsReadOnly()`. `SequenceEqual` is declared on each span type for its own type and for the read-only span of each heap backing (§4.14, §4.15; §3.3 for `char`), and the endianness-aware byte read tier is declared on `ReadOnlySpan<byte>` and on `ReadOnlyArraySpan<byte>` with identical member lists (§4.14, *Byte-buffer accessors*; §4.15). The section number is retained so later-section citations stay stable; the prior definition and the decision record are in the v0.336 changelog entry ([`Bezel_Changelog.md`](Bezel_Changelog.md)) and in [proposal 0175](proposals/0175-retire-the-read-only-span-interface.md).

<a id="sec-sorted-dictionary"></a>
### 4.17 `SortedDictionary<K, V>`

A comparison-ordered associative collection: iteration visits entries in ascending key order. The borrowing/non-owning counterpart of `Dictionary<K, V>` ([§4.8](#sec-dictionaryk-v)) with an ordering guarantee.

```bezel
public class SortedDictionary<K, V>: IReadOnlyCollection<DictionaryEntry<K, V>> where K: IComparable<K>   // conformance added (#1354): makes for-in/LINQ legal per spec §29; explicit IDisposable dropped (§1 convention — implicit, trivial-dispose)
  public constructor();

  public pure readonly property Count: int;

  public property [key: K]: V;                  // get: KeyNotFoundException if absent
                                                // set: insert or replace
  public method Add(key: K, value: V);          // ArgumentException if the key is present —
                                                //   the §4.8 duplicate-is-a-bug tier, restored so the
                                                //   sorted map carries the same three insert stances
                                                //   as Dictionary (§4.8)
  public method TryAdd(key: K, value: V): bool; // (§1 shape (d)): false and no mutation if present
  public method ContainsKey(key: K): bool;
  public method Contains(key: K): bool;         // alias for ContainsKey; enables `key in dict` (§4.8 convention)
  public method TryGetValue(key: K, out value: V?): bool;   // (#1336): nullable out, matching §4.8's revised uniform Try shape
  public method GetOrNull(key: K): V?;          // (§1 *OrNull tier): absent -> null; present -> the
                                                //   value under §4.8's read semantics — caveat in the prose
                                                //   below
  public method Remove(key: K): bool;           // false if absent
  public method Clear();

  public pure readonly property FirstKey: K;         // least key; InvalidOperationException if empty
  public pure readonly property LastKey: K;          // greatest key; InvalidOperationException if empty
end SortedDictionary;
```

Ordering is defined by `K`'s `IComparable<K>.CompareTo` — there is no separate comparer parameter; a type that needs a different order wraps its key in a record with the desired `CompareTo` (the single-order rule keeps equality, ordering, and hashing in one place, on the type; why the hash family's `IEqualityComparer<K>` constructors are *not* the same case is recorded at rationale §17.14). `Keys`/`Values` views are deliberately absent (recorded): iteration already yields entries in ascending key order, so a one-line loop substitutes; the views would earn their place only with a use the entry iteration cannot serve. Two keys are the same key exactly when `CompareTo` returns `0`. Point operations (`[k]`, `ContainsKey`, `TryGetValue`, `GetOrNull`, `Remove`) are **O(log n)**; iteration is **O(n)** in ascending key order and is the single-pass borrowing form of [§4.8](#sec-dictionaryk-v) (`for var e in sd` yields entries with `e.Key`/`e.Value` borrows, one live pass at a time, structural modification during iteration raises `InvalidOperationException`).

Value semantics match `Dictionary<K, V>`: values are copied in and out for value types and `string`, borrowed for class types. `GetOrNull` follows the same read semantics. (When the value type is itself nullable, `?` is idempotent under substitution — spec §7.10 — so a `GetOrNull` result cannot distinguish "absent" from "a present `null`"; use `ContainsKey` or the `Try` shape when the distinction matters.) The tree backing is intrinsic — part of the instance's single allocation, released by the instance's one compiler-inserted free (spec §28.9; the [§1](#sec-conventions) intrinsic-backing device) — which is what the class comment's trivial-dispose note records. There is no owning `OwningSortedDictionary` in this revision — owning sorted collections ride the self-hosted-collections work (changelog).

<a id="sec-sorted-set"></a>
### 4.18 `SortedSet<T>`

A comparison-ordered set: membership by `CompareTo`-equality, iteration in ascending order.

```bezel
public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>   // conformance added (#1354); explicit IDisposable dropped
  public constructor();

  public pure readonly property Count: int;

  public method Add(item: T);                   // (#1354): unit-returning, matching §4.10's convention
  public method TryAdd(item: T): bool;          // false if an equal item is present
  public method Contains(item: T): bool;
  public method Remove(item: T): bool;          // false if absent
  public method Clear();

  public pure readonly property First: T;            // least; InvalidOperationException if empty
  public pure readonly property Last: T;             // greatest; InvalidOperationException if empty
end SortedSet;
```

The same single-order rule, complexity guarantees (point ops O(log n), in-order iteration O(n)), iteration form, value semantics, and intrinsic-backing note as [§4.17](#sec-sorted-dictionary) apply. As with `Set<T>` ([§4.10](#sec-sett-and-ownedsett)), the element type supplies the identity; for `SortedSet<T>` that identity is `CompareTo`-equality, and adding an equal element leaves the set unmodified — `Add` returns `()` and discards silently, `TryAdd` returns `false` (the split the listing above declares).

<a id="sec-frozen-array"></a>
### 4.19 `FrozenArray<T>`

A **deeply immutable, fixed-size array** whose contents can never change after construction — the standard library's
zero-copy shared-read primitive (language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types),
[§28.13](Bezel_Language_Spec.md#sec-shared-ownership)). `FrozenArray<T>` is sendable, and it is a member of the
[§28.13](Bezel_Language_Spec.md#sec-shared-ownership) `Shared`-payload synchronized set — the set's first member
admitted by **deep immutability** rather than synchronization machinery (the ruling below) — so
`Shared<FrozenArray<T>>` is sendable: clone one handle per worker domain, move the clones, and N domains read one
buffer with zero bytes copied. A `static owned readonly` `FrozenArray<T>` is a sound all-domains singleton and draws
no [§22.12](Bezel_Language_Spec.md#sec-static-members) rule-4 warning (rule 4's warning-suppression set); mutable statics of it remain rejected (rule 2 — it is deliberately not in the form-(i) set).

**Facts** ([§1](#sec-statics-and-sharing-facts)): `FrozenArray<T>` — **immutable** (deeply immutable after construction — the construction obligations below), **displacement-unbounded** (a stranded overwrite-displaced value is the whole array; and a frozen array has no mutability at all, so nothing demands static reassignment. Provenance: the ruling above).

```bezel
/// Compiler-intrinsic; T restricted to sendable value types and `string`.
public class FrozenArray<T>: IEnumerable<T>
  // Construction consumes or copies its source — after either, no writer can exist.
  public static method From(owned buffer: OwningBuffer<T>): owned FrozenArray<T>;
      // VALUE-TYPED T ONLY: O(1) — steals the storage; the buffer is consumed
      //   (the call-site transfer marker `move`; the proposal-0006 owned-vs-copy construction shape).
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

**Why no writer can exist — checked rules plus one named obligation.** `FrozenArray<T>` has no mutating
member, and both `From` forms sever their source: the owned overload consumes the buffer, the borrowing overload
copies. The severance is delivered by named rules, not by assumption: the consume-site freeze (language spec
[§28.3](Bezel_Language_Spec.md#sec-consume-site-freeze)) rejects `From(move buf)` while any local borrow of `buf`
survives — **including a live span local over it**, which §28.3 states counts as such a borrow — and
[§9.7](Bezel_Language_Spec.md#sec-records-and-disposal)'s escape bullets keep spans out of fields, statics, escaping
closures, and async frames, so no span over the buffer outlives the freezing body invisibly. **The residual is the
domain clause** (language spec [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows), extended to exactly this construction): a borrow of the source held in a *field, a collection slot, or a pending async
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
    FrozenArray<double>.From(move raw));                         // O(1) freeze
using var t1 := Domain.Spawn(Program.ScanLow,  input.Clone());   // Shared<FrozenArray<double>>: sendable;
                                                                 //   ScanLow(owned h: Shared<FrozenArray<double>>): double — a handle takes the owned row (§6.8)
using var t2 := Domain.Spawn(Program.ScanHigh, input.Clone());
var answer := await t1 + await t2;                               // zero bytes copied
```

**Iteration** yields element copies (value semantics; `string` elements as shared references). The
[§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) direct-indexed-loop optimization applies (concretely-typed
source). Because class-typed `T` is not admitted, the `IEnumerable<T>` conformance is unconditional — the
category-conditional-conformance problem that keeps value-typed `OwningBuffer<T>` out of `IEnumerable` ([§4.14](#sec-owned-buffers-and-spans))
does not arise.

**Recorded pressure (2026-08-18 review programme; v0.260, proposal 0102 §6; no change now).** `FrozenArray<T>` freezes a *buffer*; there is no form that freezes a **class tree** — a build-once object graph whose deep immutability would admit it to the same [§28.13](Bezel_Language_Spec.md#sec-shared-ownership) set on the same ground (the class-element restriction above names it: *"frozen ownership trees are a designed future extension"*). That is the likely **first user pressure** on this deferral: the values services actually want to share across domains read-only are configurations, parsed documents, routing tables, and compiled schemas — object graphs, not arrays — and today each must be either re-synthesised per domain, wrapped in a lock ([§6.11](#sec-locked-and-rwlocked)'s *Taking data out* names this exact gap), or flattened into arrays of value records. Nothing changes here; the note exists so the next person to meet the gap finds it recorded rather than rediscovering it, and so that a proposal starts from the two questions the shape actually raises: **what makes a tree provably deeply immutable** (a `frozen`/`immutable` type property that propagates to every field type, checkable by the [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties) derived-property machinery, versus a construction-phase-then-seal protocol that is a runtime state), and **how the borrow rules read a frozen tree's interior** (a borrow of a frozen node has no source-liveness constraint against mutation, but still has one against disposal — which is exactly `FrozenArray<T>`'s own `static owned readonly` singleton answer, scaled up). The rationale's MOVE-and-FREEZE record ([§12.9](Bezel_Design_Rationale.md#sec-data-parallelism)) carries the cross-reference.

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

<a id="sec-counterk"></a>
### 4.20 `Counter<K>`

> ⚠ **Spec-ahead-of-implementation ([#3848](https://github.com/s8lap/bezel-lang/issues/3848)):** `Counter<K>` is not implemented: no member has a sema or codegen arm. The surface below is the normative contract the implementation will meet.

A tally: distinct keys mapped to `int` counts. The first stdlib surface to state its key requirement with the `where K: hashable` constraint ([§14.2](Bezel_Language_Spec.md#sec-constraints)) — an inadmissible key diagnoses at the *user's* instantiation site. (Recorded: `Dictionary`/`Set` keep their §3.6-rule-2 structural checks — the two mechanisms decide identically at a concrete key type, and re-spelling the existing containers is churn without payoff. At a key that is a type parameter of the naming declaration they decide at different sites: `Counter<V>` is admitted only where `V`'s own constraints guarantee `hashable` (language spec [§14.2](Bezel_Language_Spec.md#sec-constraints) *A type parameter as the type argument*), while a `Dictionary<V, int>` key check is carried to that declaration's instantiations (language spec [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties) rule 2). Also recorded: a constrained-generic `extension Dictionary<K, V> where …` upsert was viable; the dedicated type was chosen for the tally-specific surface — `Total`, distinct-`Count`, creation-at-delta — not out of impossibility.)

```bezel
public class Counter<K>: IEnumerable<DictionaryEntry<K, int>> where K: hashable
  public constructor();

  public method Increment(key: K, delta: int := 1);   // absent key is created at `delta`
  public method CountOf(key: K): int;                 // 0 when absent — not `pure`: compares
                                                      //   via K's equality (§22.15's line;
                                                      //   the same reason Contains is not pure)
  public method Contains(key: K): bool;               // distinguishes present-at-zero from absent;
                                                      //   enables `key in tally` (spec §17.5)
  public method Remove(key: K): bool;
  public method Clear();

  public pure readonly property Count: int;           // distinct keys currently stored
  public pure readonly property Total: int;           // sum of all counts (may be negative)
end Counter;
```

`Counter<K>` implements `IEnumerable<DictionaryEntry<K, int>>`; iteration is **insertion-ordered**, exactly as `Dictionary` ([§4.8](#sec-dictionaryk-v)): `Increment` on an existing key updates in place and preserves its position, and on an absent key stores the key — a store into the tally under language spec [§28.11](Bezel_Language_Spec.md#sec-borrowing-collections)'s store rule, as `Dictionary<K, V>.Add`'s is (*Value-copy members* there reads it off the signature). Negative `delta` is admitted; a key decremented to zero or below is **not** removed — it keeps its position, counts toward `Count`, and `CountOf` returns its (possibly zero or negative) value; only `Remove` deletes it. `CountOf` therefore returns `0` for both an absent key and a present-at-zero key — `Contains` distinguishes them. Trivial-dispose — the tally's backing state is intrinsic, part of the instance's single allocation released by its one compiler-inserted free (spec §28.9; the [§1](#sec-conventions) intrinsic-backing device); the key regime is `Dictionary`'s (class-typed keys are borrows).

**Sendability** ([§1](#sec-sendability-statements); language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)). `Counter<K>` is sendable iff `K`'s element category (language spec §28.11) is sendable-value, `string` or `Type`, the borrowing maps' key condition ([§4](#sec-system-collections-generic)); its counts are `int`. `Counter<string>` is sendable; a `Counter<K>` at a class- or interface-typed `K` carries borrows and is not.

The counting idiom, replacing §4.8's two-lookup dance for tallies:

```bezel
using var tally := new Counter<string>();
for var word in words do
  tally.Increment(word);
end;
```

<a id="sec-owning-slot-map"></a>
### 4.21 `OwningSlotMap<T>` and `SlotHandle`

> ⚠ **Spec-ahead-of-implementation ([#4604](https://github.com/s8lap/bezel-lang/issues/4604), [#4777](https://github.com/s8lap/bezel-lang/issues/4777)):** `OwningSlotMap<T>` and `SlotHandle` are not implemented: no member has a sema or codegen arm, and the runtime does not provide the map. The surface below is the normative contract the implementation will meet.

An owning collection **keyed by handles it mints itself**: `Insert` takes ownership of a value and returns a `SlotHandle`; every read, take and removal is keyed by that handle; a removed slot's handle reads as absent for the rest of the map's life. It is the library form of the handle-and-table pattern (rationale [§5.4](Bezel_Design_Rationale.md#sec-no-shared-ownership)): a program that must refer to an object from many places — a graph's edges, a cache's index, a registry's entries — stores the object's `SlotHandle`, a value, where it would otherwise store a borrow, and so carries no [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) trust clause and no domain clause for the reference.

```bezel
/// Compiler-intrinsic value record. Sendable (spec §30.14); hashable (spec §3.6) — usable as a
/// Dictionary<K, V> key or Set<T> element. Declares neither ISerializable nor IDeserializable: no
/// serialization is synthesized for it, and it is not an admissible serialized component of any
/// other type (spec §9.8).
public record SlotHandle
  public readonly property Index: int;         // the slot's position in its map; stable for the handle's life
  public readonly property Generation: int;    // the slot's generation when the handle was minted
  // =, <>, Equals, GetHashCode and ToString are compiler-provided. Two handles are equal iff
  // they were minted by the same map for the same slot at the same generation; a third,
  // non-public component identifies the minting map and participates in equality and hashing.
  // No public constructor: handles are minted by OwningSlotMap<T>.Insert alone. default(SlotHandle)
  // (spec §17.11) is the one handle no map minted: it denotes a slot of no map (below).
  // ToString renders `SlotHandle(Index: i, Generation: g)` — the auto-hashable body-record rendering
  // of spec §8.5/§9.2 over the two public auto-properties; the hidden component is not rendered, so
  // two handles may print alike and compare unequal.
end SlotHandle;

public class OwningSlotMap<T>: IEnumerable<T>, IOwningEnumerable<T> where T: class
  public constructor();
  public constructor(capacity: int);                      // ArgumentOutOfRangeException if negative (§1)

  public pure readonly property Count: int;               // live slots

  public method Insert(owned value: T): SlotHandle;       // takes ownership; mints the handle
  public method Contains(handle: SlotHandle): bool;       // live under this handle; enables `h in map` (spec §17.5)
  public method Get(handle: SlotHandle): T;               // borrow of the stored value; KeyNotFoundException if absent
  public method GetOrNull(handle: SlotHandle): T?;        // §1 *OrNull tier: absent -> null; present -> a borrow
  public method TryGetValue(handle: SlotHandle, out value: T?): bool;   // §1 shape (a); the out is a borrow
  public method Remove(handle: SlotHandle): bool;         // disposes the value; false if absent
  public method PopValue(handle: SlotHandle): owned T;    // vacates the slot; KeyNotFoundException if absent
  public method TryPopValue(handle: SlotHandle, out owned value: T?): bool;   // §1 shape (a)
  public method Handles(): owned Array<SlotHandle>;       // a snapshot of every live handle, ascending Index
  public method Drain(): IOwningEnumerable<T>;            // iteration drains the map, ascending Index
  public method Clear();                                  // disposes every value, descending Index
end OwningSlotMap;
```

**Slots and generations.** The map is a table of slots addressed by index. Every slot carries a **generation**, `0` when the slot is first used and advanced by one each time the slot is **vacated** — by `Remove`, `PopValue`, `TryPopValue`, a `Drain()` yield, or `Clear()`. `Insert` places the value in a vacant slot and mints a handle carrying that slot's index and current generation. A handle is **live** iff its slot holds a value and the slot's generation equals the handle's `Generation`; otherwise the handle reads as **absent** — `Contains` is `false`, `GetOrNull` is `null`, `TryGetValue` and `TryPopValue` return `false`, `Remove` returns `false`, and `Get` and `PopValue` raise `KeyNotFoundException`. A handle therefore never reads a later occupant of its slot.

**Reuse and retirement.** `Insert` reuses the **most recently vacated** slot that is not retired, and otherwise takes the next never-used index; `Index` values are consequently dense, which is what makes them usable as indices into a side table kept beside the map. A slot whose generation would exceed `int.MaxValue` is **retired** — never reused — so generations never wrap and a stale handle can never become live again.

**A handle from another map is a fault.** Every handle-taking member raises `ArgumentException` when the handle was minted by a different `OwningSlotMap<T>` instance ([§1](#sec-failure-signal-rule)'s band 1: a caller bug, not an expected absence). A `default(SlotHandle)` (spec [§17.11](Bezel_Language_Spec.md#sec-the-default-operator) — the record is defaultable, so `var h: SlotHandle;`, a default-initialized field and a default-initialized `Array<SlotHandle>` slot all hold it) denotes a slot of no map and takes the same fault: no map ever mints it.

**Ownership faces** (spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy)'s owned column). `Insert` takes `owned`: a fresh producer or a `move`d binding, never an unmarked read of an owned binding (spec §28.3 K3.3). `Get`, `GetOrNull`, `TryGetValue`'s out, and default iteration produce **borrows sourced from the map**, valid until the slot is mutated — a `Remove`, `PopValue` or `Clear` on the slot is a mid-scope kill under the checked-disposal backstop (spec [§28.2](Bezel_Language_Spec.md#sec-checked-disposal)). `Remove` and `Clear` **dispose**; `PopValue`, `TryPopValue` and `Drain()` **transfer**. Disposing through a borrow obtained from `Get` or `GetOrNull` is spec [§28.6](Bezel_Language_Spec.md#sec-who-may-dispose)'s compile error.

**Order.** Default iteration (`IEnumerable<T>`), `Handles()` and `Drain()` visit live slots in **ascending `Index`**. `Clear()` and the map's own `Dispose()` dispose the values in **descending `Index`** — the ordered families' reverse-storage-order rule of spec [§28.11](Bezel_Language_Spec.md#sec-owning-collections), storage order here being slot order. `Clear()` keeps the emptied map; `Dispose()` then releases the storage.

**`Handles()` is a snapshot, not a view.** It returns a fresh `owned Array<SlotHandle>` — trivial-dispose at this element type, so `for var h in map.Handles() do … end;` carries no ceremony — because the dominant loop over a registry removes as it goes, and a mutation through a live view is spec [§29.12](Bezel_Language_Spec.md#sec-mutation-during-iteration)'s `InvalidOperationException`. `Insert`, `Remove`, `PopValue`, `Drain` and `Clear` are structural modifications under that section and invalidate an active enumerator.

```bezel
using var m := new OwningSlotMap<Session>();
var h := m.Insert(new Session("alice"));       // a value; store it anywhere
_edges.Add(new Edge(h, other));                 // record Edge(From: SlotHandle, To: SlotHandle): no borrow, no trust clause
var s := m.GetOrNull(h);                        // a borrow sourced from m, or null if the slot was vacated
if s <> null then s.Touch(); end if;
for var stale in m.Handles() do                 // remove while walking: the snapshot is not a view
  if m.Get(stale).Expired then _ := m.Remove(stale); end if;   // Remove disposes; the handle reads absent afterwards
end for;
```

**Interface conformance.** `IEnumerable<T>` (borrowing default iteration) and `IOwningEnumerable<T>` (the `Drain()` path) — the both-implemented shape of [§4.5](#sec-ownedarrayt), with [§4.13](#sec-owning-enumeration-interfaces)'s static-type rule selecting the path. Not `IOwningCollection<T>`: that interface's `Add(owned item: T)` returns `()` where this type's ingress returns the handle, and its `Contains(item: T)`/`Remove(item: T)` are keyed by element where this type's are keyed by handle. Not a collection-literal target (spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol)): a literal could not hand back the handles it would mint. **There is no borrowing twin**: handles exist to replace stored borrows, and a borrow-storing slot map would carry spec [§28.11](Bezel_Language_Spec.md#sec-borrowing-collections)'s store residual at every slot. A program that must refer to an object owned elsewhere stores that object's handle from the map that owns it; where the *holder* must keep the object alive, `Shared<T>`/`Weak<T>` ([§2.16](#sec-shared-and-weak)) remains the form.

**Derived properties.** Not trivial-dispose at any instantiation (spec [§28.9](Bezel_Language_Spec.md#sec-trivial-dispose-for-generic-class-instantiations)). Sendable iff `T` is a sendable class type (spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)'s owning-collection rule: the map's state is wholly owned; handles left behind in the origin domain denote a map that domain can no longer reach). `SlotHandle` is sendable, hashable and defaultable (spec [§17.11](Bezel_Language_Spec.md#sec-the-default-operator); its default is the no-map handle above), and declares no serialization conformance. `OwningSlotMap<T>` declares no statics-and-sharing facts ([§1](#sec-statics-and-sharing-facts)); a `static owned readonly` map draws spec §22.12 rule 4's warning like every other owning collection, and the `Locked<T>` registry idiom ([§6.11](#sec-locked-and-rwlocked)) is the process-global spelling. `T` is restricted to class and interface types at the instantiation site (spec [§14.4](Bezel_Language_Spec.md#sec-owned-as-a-generic-argument)).

<a id="sec-system-collections-linq"></a>
## 5. `System.Collections.Linq` (auto-imported)

**Auto-imported:** this module is on language spec [§2.3](Bezel_Language_Spec.md#sec-always-available-names)'s auto-import list, so the operators below resolve in every file with no `import`, exactly as the `IEnumerable<T>` they extend is nameable in every file. LINQ-style extension methods on `IEnumerable<T>`. All are lazy unless documented otherwise. Each deferred operator call mints a **caller-owned query frame** — the unmarked extension declarations below are the ordinary minting iterable-family spelling (calls mint, language spec [§28.3](Bezel_Language_Spec.md#sec-producers); the per-callee-kind matrix is the spec's announcement table, [§28.3](Bezel_Language_Spec.md#sec-announcement-table)). A lazy query produced by these operators is **single-consumption** — it may be enumerated at most once (a re-enumeration is a compile error, or `InvalidOperationException` where only dynamically reachable); the single consumption point spends the frame's **enumeration right**, layered over — not identified with — the standard owned-value obligation, which the caller discharges under the ordinary rules (`using var`, transfer, dispose, or auto-drop). Call `ToArray()` to materialize an owned, re-enumerable `Array<T>`. See language spec [§29.11](Bezel_Language_Spec.md#sec-multiple-enumeration).

A bound lazy query is single-consumption with the normative consumption-point list of spec §29.11 (enumeration/terminal, composing a further operator, passing as `IEnumerable<T>`); inside a callee a plain `IEnumerable<T>` parameter is statically untracked and guarded at runtime.

**Ownership at class-typed substitutions (#1666).** Under the bare-type-parameter rule (language spec [§28.3](Bezel_Language_Spec.md#sec-bare-type-parameter-returns)), at a class or interface element type the results below are **borrows** — the caller takes no disposal obligation — and at value and runtime-managed substitutions no ownership attaches (§2.2's note, applied to this module); at a substitution that is a value type that holds borrows (language spec [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)) — `DictionaryEntry<K, V>` at a class `V` among them — the result is a value whose carried borrows have the sources the table names (language spec [§28.3](Bezel_Language_Spec.md#sec-bare-type-parameter-returns), *Bare-type-parameter returns*, caller-side clause 3):

| Members | The result is a borrow of | Usable while |
|---|---|---|
| `First`/`Last`/`Single`/`ElementAt`, with the `*OrNull` forms and the predicate forms where they exist (`ElementAt`/`ElementAtOrNull` carry none); `Min()`/`Max()` and their no-selector `MinOrNull()`/`MaxOrNull()` forms ([§5.1](#sec-no-selector-aggregation-overloads)) | the element, sourced from the receiver | the element's backing is live — for a collection-backed sequence, while the collection's own source lives |
| `Min<U>`/`Max<U>(selector)` | the projected value | the same liveness, followed transitively through the element |
| `Aggregate<TAccum>(seed, fold)` at class `TAccum` | {receiver, seed} — an empty source returns the seed itself | both are live (the one §5 classification this rule tightens) |

These accessors are compiler-known members: at class element types no in-language body over arbitrary `IEnumerable<T>` can produce the element borrow, because an iteration-variable borrow may not escape the loop (language spec §28.3/§29.2). Over an **enumerator-owned-element source** — an iterator whose enumerator's own frame owns the yielded element, by **minting** it or by **adopting** it into a frame-resident owned binding (language spec [§29.2](Bezel_Language_Spec.md#sec-iterator-yield-ownership)), or an operator chain over one — the element does not survive the terminal's internal enumerator, so a later use of the result faults deterministically in checked mode (the K4.1 backstop, language spec [§28.2](Bezel_Language_Spec.md#sec-checked-disposal)); **materializing does not remedy this (#2557)** — `ToArray()`/`ToSet()`/`ToDictionary(…)` at a class or interface element type return **borrowing** collections ([§4.4](#sec-arrayt)) whose slots dangle exactly as the accessor result does. The remedies are to consume each element inside the loop body that borrows it (the borrow is valid until the next advance, language spec [§29.2](Bezel_Language_Spec.md#sec-iterator-yield-ownership)), or to give the elements an owner that outlives the use — the producer declares an owning enumeration return type (`IOwningEnumerable<T>`, language spec [§29.3](Bezel_Language_Spec.md#sec-iterator-methods)) and the consumer takes each element through owning iteration ([§4.13](#sec-owning-enumeration-interfaces)) into an owning collection, to which the accessor is then applied; where the source is an owning collection, `Drain()` is that path ([§4.5](#sec-ownedarrayt)). Where the producer is a **borrowing** minting iterator the consumer cannot change, no retention remedy exists at a class element type — the borrowing protocol has no transfer-out (language spec [§29.1](Bezel_Language_Spec.md#sec-current-semantics-on-borrowing-and-owning-enumerators)) — and the element must be used before the next advance. The two arms and the compiler's **must-warn** subset of this hazard at the no-selector forms of these accessors — the member set, the predicate, the pass-through nodes, the exhaustive silent shapes with their recorded liftables, and the over-warned shape — are normative at language spec [§29.11](Bezel_Language_Spec.md#sec-multiple-enumeration) (#3047, [#3285](https://github.com/s8lap/bezel-lang/issues/3285)), whose wording governs.

**Callback retention (the callback-taking lazy operators store their callbacks).** A predicate/selector/key/combiner passed to a **lazy** operator that takes one — `Where`, `Select`, `SelectMany`, `TakeWhile`, `SkipWhile`, `OrderBy`, `OrderByDescending`, `ThenBy`, `ThenByDescending`, `GroupBy`, `Zip` — is **retained by the returned query value until enumeration**, not just for the duration of the call. These signatures are the normative instance of the language spec's deferred-activation frame-residence rule (spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters) *Call-bounded × deferred activation*): the callback resides in the returned **query value** — the deferred-activation frame — its captured borrows' sources are carried sources of that frame (spec [§28.3](Bezel_Language_Spec.md#sec-unbound-owned-temporaries)'s capture-set contribution), and the retention ends at the query's [§29.11](Bezel_Language_Spec.md#sec-multiple-enumeration) single-consumption point. A user-authored callback-taking iterator method follows the identical rule — a general language rule, not a stdlib certification. The callback-free lazy operators (`Take`, `Skip`, `Reverse`, `Distinct`, `Concat`, `Union`, `Intersect`, `Except`) hold no callback and carry no such obligation. The **eager** operators (`First(pred)`, `FirstOrNull(pred)`, `Last(pred)`, `LastOrNull(pred)`, `Single(pred)`, `SingleOrNull(pred)`, `Count(pred)`, `Any(pred)`, `All(pred)`, `Sum`, `Min`, `Max`, `Average`, `Aggregate`, `ToArray`, `ToDictionary`, `ToSet`) are **call-bounded** as their unmarked signatures state — they invoke the callback during the call and retain nothing.

```bezel
public extension IEnumerable<T>
  // Filtering
  public method Where(predicate: (T) -> bool): IEnumerable<T>;

  // Projection
  public method Select<U>(selector: (T) -> U): IEnumerable<U>;
  public method SelectMany<U>(selector: (T) -> IEnumerable<U>): IEnumerable<U>;

  // Quantifiers
  public method Any(): bool;
  public method Any(predicate: (T) -> bool): bool;
  public method All(predicate: (T) -> bool): bool;
  public method Count(): int;
  public method Count(predicate: (T) -> bool): int;

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
  public method ElementAtOrNull(index: int): T?;      // — null when index is out of range

  // Aggregation
  public method Sum<U>(selector: (T) -> U): owned U where U: INumber<U>;
                                               // — the §2.19 constrained generic (spec §11.8's
                                               //   accumulator shape); replaces the seven per-type
                                               //   overloads; `owned` erased at value U (spec §28.1)
  public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
  public method Max<U>(selector: (T) -> U): U where U: IComparable<U>;
  public method Average(selector: (T) -> int): double;
  public method Average(selector: (T) -> long): double;
  public method Average(selector: (T) -> uint): double;
  public method Average(selector: (T) -> ulong): double;
  public method Average(selector: (T) -> float): float;
  public method Average(selector: (T) -> double): double;
  public method Average(selector: (T) -> decimal): decimal;
                                               // Min/Max/Average, selector and no-selector forms alike:
                                               //   InvalidOperationException on an empty source (§5's
                                               //   empty-source rule); Sum returns zero, Aggregate seed
  public method Aggregate<TAccum>(seed: TAccum, fold: (TAccum, T) -> TAccum): TAccum;

  // Ordering
  public method OrderBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T> where TKey: IComparable<TKey>;
  public method OrderByDescending<TKey>(key: (T) -> TKey): IOrderedEnumerable<T> where TKey: IComparable<TKey>;
  public method Reverse(): IEnumerable<T>;

  // Slicing
  public method Take(count: int): IEnumerable<T>;
  public method TakeWhile(predicate: (T) -> bool): IEnumerable<T>;
  public method Skip(count: int): IEnumerable<T>;
  public method SkipWhile(predicate: (T) -> bool): IEnumerable<T>;

  // Combination
  public method Concat(other: IEnumerable<T>): IEnumerable<T>;
  public method Zip<U, R>(other: IEnumerable<U>, combine: (T, U) -> R): IEnumerable<R>;

  // Grouping
  public method GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>> where TKey: hashable;

  // Materialization (eager)
  public method ToArray(): owned Array<T>;
  public method ToDictionary<TKey>(key: (T) -> TKey): owned Dictionary<TKey, T> where TKey: hashable;
  public method ToDictionary<TKey, TValue>(key: (T) -> TKey, value: (T) -> TValue): owned Dictionary<TKey, TValue> where TKey: hashable;
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

The six operators that require **hashable** `T` are declared in their own constrained block. The requirement is an ordinary constraint (language spec [§14.2](Bezel_Language_Spec.md#sec-constraints)) on a [§26.4](Bezel_Language_Spec.md#sec-generic-extensions) extension block, not a conditionally-admitted member; [§26.6](Bezel_Language_Spec.md#sec-multiple-blocks-for-the-same-type) merges the block with the one above, so no call site changes and no member moves module. §5.1's `where T: IComparable<T>` and `where T: INumber<T>` blocks are the same device on the same type.

```bezel
public extension IEnumerable<T> where T: hashable
  // Quantifiers
  public method Contains(value: T): bool;

  // Set-like
  public method Distinct(): IEnumerable<T>;
  public method Union(other: IEnumerable<T>): IEnumerable<T>;
  public method Intersect(other: IEnumerable<T>): IEnumerable<T>;
  public method Except(other: IEnumerable<T>): IEnumerable<T>;

  // Materialization (eager)
  public method ToSet(): owned Set<T>;
end;
```

`First`/`Last`/`Single`/`ElementAt` throw `InvalidOperationException` (or `IndexOutOfRangeException` for `ElementAt`) on missing-element; the `*OrNull` variants return `null` instead (`ElementAtOrNull`, declared, completes the tier the surrounding texts had always named — no predicate form exists for it, an index having no predicate analogue). On an **empty source**: `Sum` (both forms, §5.1's no-selector included) returns the zero of its result type (`U.Zero()`/`T.Zero()`, [§2.19](#sec-static-abstractions)); `Min`/`Max` — selector and no-selector forms alike — and `Average` throw `InvalidOperationException`; `Aggregate` returns `seed`. There are deliberately no `MinOrNull`/`MaxOrNull` selector overloads — compose `Select(selector).MinOrNull()` for the null-signaled form. (When the element type is itself nullable, `?` is idempotent under substitution — spec §7.10 — so an `*OrNull` result cannot distinguish "empty" from "a present `null`"; use `Count` or the `Try` shape when the distinction matters.)

The predicate overloads of `First`, `Last`, and `Single` (and their `*OrNull` forms) restrict to the elements satisfying the predicate: `First(pred)` and `Last(pred)` yield the first and last matching element, and `Single(pred)` the sole matching element. Each is equivalent to the corresponding no-argument method applied after `Where(pred)` — `xs.First(pred)` is `xs.Where(pred).First()` — and inherits its missing-element behaviour: `First(pred)`/`Last(pred)`/`Single(pred)` throw `InvalidOperationException` when no element matches (and `Single(pred)` also when more than one matches), while `FirstOrNull(pred)`/`LastOrNull(pred)`/`SingleOrNull(pred)` return `null` when none matches (`SingleOrNull(pred)` still throws when more than one matches).

`ToArray()` returns a fresh `Array<T>` containing the materialized sequence. The return is `owned` — the caller is responsible for disposing it. **What materialization copies (#2557).** `owned` here is the container axis only ([§4](#sec-system-collections-generic)'s naming note): at a class or interface element type `Array<T>` is a **borrowing** collection, so the result's slots are borrows of the enumerated elements and the caller is responsible for the array itself, not its elements ([§4.4](#sec-arrayt); language spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy)) — the same holds of `ToSet()` and `ToDictionary(…)`. Materialization gives the result a new **container**, never a new **owner** for the elements; over a source whose elements do not outlive the call it therefore produces a collection of dead borrows (the class-element paragraph above). Where the element type's values carry a borrow, the copy is a **store** judged at the call (language spec [§9.7](Bezel_Language_Spec.md#sec-records-and-disposal)'s collection bullet for a value type that holds borrows, [§28.11](Bezel_Language_Spec.md#sec-borrowing-collections)'s *Storing a borrow* for a class or interface element): each copied slot is sourced as an element read from the receiver is, so `var ys := xs.ToArray();` is admitted where `ys.Add(xs[0])` would be, and `_keep := xs.ToArray();` or `return xs.ToArray();` over a local `xs` is rejected where that `Add` into a field-held or escaping collection would be. Every value-copy member ([§4](#sec-value-copy-members); language spec [§28.11](Bezel_Language_Spec.md#sec-borrowing-collections)'s *Value-copy members*, which reads them off their signatures) is read the same way. There is no `ToList()` because Bezel does not have a dynamic-array type named `List<T>` (`Array<T>` is the resizable form; `LinkedList<T>` is the linked form; use `new LinkedList<T>(source)` if a linked list is wanted).

**Key and equality contracts.** `ToDictionary` (both overloads) raises `ArgumentException` on a duplicate key — `Dictionary.Add` semantics; silent last-wins would mask a defective key selector. `Distinct`, `Union`, `Intersect`, `Except`, `Contains`, and `ToSet` require **hashable** `T` and are declared in the constrained `extension IEnumerable<T> where T: hashable` block above; `GroupBy` and both `ToDictionary` overloads state the same requirement on `TKey` with an ordinary member-level `where` clause. Both are the ordinary constraint form of language spec [§14.2](Bezel_Language_Spec.md#sec-constraints) — on a [§26.4](Bezel_Language_Spec.md#sec-generic-extensions) extension block in the first case, on the member's own type parameter in the second — and neither is a **conditionally-admitted member** (language spec [§14.2](Bezel_Language_Spec.md#sec-conditionally-admitted-members), whose rule 1 says so in terms). The requirement itself is unchanged, and is the one `Set<T>`/`Dictionary<K, V>` state (§4.8/§4.10; spec §3.6 rule 2). What changes is where a violation is reported: at a non-hashable `T` the constrained block is inapplicable, so the call is reported as having no applicable extension (spec [§26.5](Bezel_Language_Spec.md#sec-resolution)) rather than with a `hashable`-specific message. That is recorded as a worse diagnostic for the same program, liftable through the general no-applicable-extension diagnostic rather than here — §26.5 rule 4 already obliges an extension-ambiguity diagnostic to name a block's declared head including its `where` clause, so the machinery to name a constraint in a diagnostic exists.

**Element-level semantics.** `Zip` ends when either input ends. `Take`/`Skip` **clamp**: a negative count behaves as `0`, and a count beyond the source takes everything / skips everything — they are range operations, not magnitude constructors, so §1's negative-magnitude fault rule deliberately does not apply. `Distinct` and `Union` yield first occurrences in encounter order (`Union`: first input, then the second); `Intersect` and `Except` yield the retained elements, made distinct, in first-input order.

**`GroupBy` semantics (single-consumption scoping).** `GroupBy` is lazy until its first enumeration, then **materializes fully** — no complete group can exist before the source is exhausted, so the operator buffers; its §29.11 consumption of the source happens at that point. Groups appear in first-occurrence-of-key order; elements within a group in source order. Single-consumption (spec §29.11) binds to the **outer** `IEnumerable<IGrouping<TKey, T>>` sequence only — the lazy query here. Each `IGrouping<TKey, T>` is a materialized, ordinarily **re-enumerable** collection whose elements are the grouping's own buffer, and it remains valid after the outer sequence advances past it: `g.Count()` followed by `g.Average(…)` is defined and reads the buffer twice. At a class or interface element type that buffer holds **borrows** of the source's elements, as every borrowing collection does (language spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy)): the *grouping* survives the outer sequence, its *elements* only as long as their own owner does, so a grouping over an enumerator-owned-element source carries dead borrows exactly as a materializer does (the class-element paragraph above), and — the grouping's own buffer being filled by exhausting the source — **every** element borrow in **every** group is dead before the first group is handed out, not only those from groups the outer sequence has advanced past; a class-element terminal accessor over such a group draws language spec [§29.11](Bezel_Language_Spec.md#sec-multiple-enumeration)'s terminal-accessor provenance warning where the grouping's own source traces to a same-unit enumerator-owned-yield iterator, the grouping being a pass-through node of that trace and not a root. When a group feeds many aggregations, materializing it once — `for var g in logs.GroupBy(lambda(r) => r.Level) do using var items := g.ToArray(); … end;` — remains the advised shape; it is advice, not a rule.

**`Reverse` (naming note).** §5 keeps the .NET-heritage operator names, so the lazy `Reverse()` deliberately sits outside §4.4's imperative-mutates/participle-copies convention — recorded as a decision, not an accident (renaming to `Reversed()` was declined: §4.4's participle promises a fresh owned *copy*, which a lazy sequence is not). No call-site ambiguity arises: on an `Array<T>` receiver, instance resolution commits to the in-place `Reverse()` before extensions are consulted (spec §26.5 rule 1); the lazy operator applies only where the receiver's static type has no instance `Reverse`.

**`Sum` is the `INumber<U>` generic; `Average` stays per-type, blocked with its reasons recorded.** The per-type overloads predated the [§2.19](#sec-static-abstractions) abstraction, and their recorded justification — "no generic numeric constraint in this version" — has been false since §2.19 landed. `Sum` collected the migration: its 14 overloads (7 selector, 7 no-selector) collapsed to the two constrained generics (above and [§5.1](#sec-no-selector-aggregation-overloads)) — an accumulator over `U.Zero()` and `+`, exactly spec §11.8's own `Sum<T>` example, sound at every substitution including user class conformers (spec §28.3's resolved note). `Average` **does not migrate**, on two obstructions. Primary: **the result type is not the element type** — five of its seven forms deliberately widen (the integer families average to `double`) while `float` and `decimal` keep their own types, and no constraint expresses that mapping: a single `Average<U>(): U` would truncate integer averages silently, a single `(): double` would lose `decimal`'s exactness and `float`'s type. Secondary: the divide-by-count is expressible only at the price of a second constraint — `INumber<U>` declares `/` over `(U, U)` with no `int`→`U` conversion, so the count enters `U`'s space only via `ISteppable<U>.Step(U.Zero(), count)` ([§2.19](#sec-static-abstractions)) — an ordinal-advance operation pressed into service as a conversion, demanded of every user conformer wanting an arithmetic aggregation. `Average` therefore awaits a result-type facility on `INumber<T>` (an associated result type — the same associated-types gap rationale §18 records for traits); its 14 overloads stand. User-defined numeric types: `Sum` now applies directly at any `INumber<T>` conformer; averaging one is done via `Aggregate(seed, fold)`. One consequence of the migration worth stating: `Sum` now returns the element's own type, so a narrow integer element type accumulates in that type and is overflow-checked in it (spec [§6.1](Bezel_Language_Spec.md#sec-integer-arithmetic)) — `bytes.Sum()` over an `Array<byte>` raises `OverflowException` past 255. Widen in the selector when the total will not fit: `xs.Sum(lambda(x) => long(x))`.

<a id="sec-no-selector-aggregation-overloads"></a>
### 5.1 No-selector aggregation overloads

`Min` and `Max` without a selector are generic over comparable types:

```bezel
public extension IEnumerable<T> where T: IComparable<T>
  public method Min(): T;              // InvalidOperationException if empty
  public method Max(): T;              // InvalidOperationException if empty
  public method MinOrNull(): T?;       // null if empty
  public method MaxOrNull(): T?;       // null if empty
end;
```

`Sum` without a selector is the constrained generic (replacing the seven per-type extensions, of which four were declared and three prose-stated):

```bezel
public extension IEnumerable<T> where T: INumber<T>
  public method Sum(): owned T;        // T.Zero()/`+` over §2.19; `owned` erased at value T (spec §28.1)
end;
```

`Average` without a selector stays per-type, for [§5](#sec-system-collections-linq)'s recorded result-type-mapping obstruction:

```bezel
public extension IEnumerable<int>
  public method Average(): double;   // InvalidOperationException if empty (every Average form)
end;

public extension IEnumerable<long>
  public method Average(): double;
end;

public extension IEnumerable<double>
  public method Average(): double;
end;

public extension IEnumerable<decimal>
  public method Average(): decimal;
end;
```

Analogous `Average` extensions exist for `uint`, `ulong`, and `float` — `Average(): double` for the integer forms and `float` for `float`, matching the selector overloads. The per-type pattern preserves the deliberate result-type mapping (`decimal` exact, `float` in `float`, the integer families widening to `double`) that no `INumber<U>` signature can express — the blocking record at [§5](#sec-system-collections-linq)'s aggregation note.

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

  // (#810): a source cancelled when ANY parent token cancels (or by its
  // own Cancel/CancelAfter). Two-token form for the everyday case (caller +
  // deadline); array form for N parents.
  public static method CreateLinked(a: CancellationToken, b: CancellationToken): owned CancellationTokenSource;
  public static method CreateLinkedAll(parents: Array<CancellationToken>): owned CancellationTokenSource;

  public readonly property Token: CancellationToken;
  public readonly property IsCancellationRequested: bool;

  public method Cancel();
  public method CancelAfter(delay: TimeSpan);

  destructor;       // signals cancellation to any tokens still observing,
                           // unregisters from linked parents, then
                           // releases the registered-callback list
end CancellationTokenSource;
```

**Facts** ([§1](#sec-statics-and-sharing-facts)): `CancellationTokenSource` — **synchronized** (the cancelled-state flag and the registration list are internally synchronized, and the state read is atomic and cross-thread-visible by the guarantee language spec [§30.8](Bezel_Language_Spec.md#sec-cancellation) already carries, so an instance is safe to reach from every domain across its **state-and-signal surface** — `Cancel`, `CancelAfter`, `IsCancellationRequested`, `Token`, the token's `IsCancellationRequested` and `ThrowIfCancellationRequested`, and the linking factories. The surface excluded is `Register`, and it is excluded by a runtime check rather than left to the programmer: a *published* source runs no user callbacks at all, below), **displacement-unbounded** (the value a mutable static's overwrite would strand is the source together with its whole registration list and its registrations in every parent source; and the stranded source can no longer be signalled, so every token still observing it waits on a cancellation that cannot arrive — a worse residue than a payload-scaled leak, and no use case demands static reassignment of a cancellation source. Provenance: the ruling recorded immediately below).

**Cross-domain cancellation: the published source is signal-only.** The **synchronized** fact puts `CancellationTokenSource` in language spec [§28.13](Bezel_Language_Spec.md#sec-shared-ownership)'s `Shared`-payload synchronized set, so **`Shared<CancellationTokenSource>` is sendable**: a cloned handle may cross a domain boundary as a spawn argument or a channel element, and the source is disposed once, by whichever domain drops the last handle. Much of the machinery is already promised — language spec §30.8 guarantees the state read is atomic and observes another domain's `Cancel()` without additional synchronization.

**What is new here, and why it needs a rule of its own.** `CancellationTokenSource` is the **first member of the `Shared`-payload set that is not itself sendable.** The other members (`Mutex`, `Semaphore`, `Atomic<T>`, `AtomicInteger<T>`, `Locked<T>`, `RwLocked<T>`, `FrozenArray<T>`) each cross on their own as well as behind a handle, so for them "disposed by whichever domain drops the last handle" disposes a value that domain could legitimately have held, running only that value's own destructor — and `Locked<T>`'s interior, the nearest shape, must be a *sendable class*. A cancellation source is different in kind: its disposal runs **registered callbacks**, which are function values, and language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types) states that *"function-typed values are never sendable: their captures are borrows of the origin domain's scopes"*. Set membership keys on the type's facts, not on its sendability, and this is the first case where the two come apart. Everything below follows from that one novelty.

**The rule: a published source carries no registrations.** A source is **published** — reachable from more than one domain — at three points, each of which sets the property permanently:

1. **`new Shared<CancellationTokenSource>(owned source)`** publishes the source it consumes.
2. **A `static readonly` or `static owned readonly` field of the type** publishes the source it holds: a static is process-global and therefore reachable from every domain (language spec [§22.12](Bezel_Language_Spec.md#sec-static-members) rule 4's own words).
3. **A linked child of a published parent** is published: `CreateLinked`/`CreateLinkedAll` produce a published child whenever any parent source is published. Publication does **not** propagate from child to parent, cancellation propagating only parent-to-child.

On a published source, **`Register` raises `InvalidOperationException`**, and **publishing a source that already holds live registrations raises `InvalidOperationException`** at the publishing operation — the second half is what makes the order immaterial, since registering first and sharing afterwards fails at the share rather than succeeding into a hazard. A published source is therefore **signal-only**: `Cancel`, `CancelAfter`, the deadline set by `WithCancelAfter`, cancel-on-dispose, and propagation from a linked parent all still work and are safe from any domain, because none of them has a user callback to run. This closes every path structurally rather than by enumerating fire sites — the registration list of a published source is empty, so *no* fire path can run a foreign closure, including the ones an enumeration would miss.

**Why the rule is enforced and not merely stated.** A stated obligation would not have held. Four paths run a callback in a domain that did not write it, and only the first is the one a reader looks for: a foreign domain calling `Cancel()`; the **last handle drop**, which performs a full `Cancel()` on the disposing domain's thread and is a runtime race no obedient program can steer; a **linked child**, whose own callbacks fire transitively when a published parent is cancelled from anywhere; and a **process-global static**, on which any domain may call `Token` and then `Register`. Each is a data race on a domain's mutable state, the thing language spec §30.14's *"no mutable state is ever reachable from two domains"* exists to prevent, and none of them has a checked-disposal backstop. The check is one runtime layer of the two-layer shape language spec [§30.12](Bezel_Language_Spec.md#sec-thread-synchronization) uses for the blocking acquisitions; the lexical layer of that shape is deliberately **not** mirrored, because publication is a property of the receiver's history rather than of the call site's syntax, so there is no lexical form for a compiler to reject.

**The recorded cost, stated rather than discovered later.** A program may today hold a `static readonly CancellationTokenSource` and `Register` on it, and in a single-domain program nothing is wrong with that; under this rule it raises. The over-rejection is deliberate: a static source is reachable from every domain by construction, the property cannot be narrowed to "reachable from a *second* domain" without observing the whole program, and the alternative is a hazard that appears only when a program later spawns its first domain. The remedy is one of two lines — poll `IsCancellationRequested` / `ThrowIfCancellationRequested`, which is the dominant idiom and exactly what "signal-only" means, or hold the source in ordinary domain-local storage instead of a static and register there.

**The alternatives, declined with reasons.** *Leaving the confinement to the programmer as a stated obligation* is what the four paths above refute: two of them (last-handle drop, transitive linked firing) are not visible at the site that would have to honour the obligation. *Queueing a foreign callback to the domain that registered it* would break the guarantee `Register` exists to give: disposal performs a full `Cancel()` whose callbacks run **before** the registration list is severed, which is language spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters)'s quiescence — no stored callback is invoked after disposal completes — and a callback posted to another domain's queue cannot be known to have run when disposal returns, or ever, that domain being possibly complete already (language spec §33.3). *Rejecting the registration statically instead of at runtime* would require the compiler to know, at a `Register` call site, whether the receiver's source is published: `Register` is declared on `CancellationToken`, a freely-copyable record that is passed, stored in async frames, and copied without ceremony, so the analysis would be a whole-program value-provenance analysis over token values with no existing carrier to ride — Bezel's per-value analyses (ownership, borrow liveness, definite assignment, narrowing) are all method-local, and this one is not. It is declined on that cost, not on principle.

**The source-outlives-token obligation now spans domains, and the handle discharges it.** A `CancellationToken` is a borrow-bearing record whose borrow source is the source; under sharing, a receiving domain's token borrows through that domain's own handle, and the handle — not the borrow — is what keeps the source alive ([§28.13](Bezel_Language_Spec.md#sec-shared-ownership)). So the obligation is met mechanically as long as the observing domain holds a live handle, which is stronger than the intra-domain case, where it rests on the program's scope ordering. Two shapes follow: a domain must clone its handle before it sends one on, and cancel-on-dispose fires when the **last** handle drops — so `using var` over a handle scopes that handle, not the cancellation, and the cancellation may be signalled from a domain other than the one that created the source.

**The other two derived sets.** A `static readonly` (or `static owned readonly`) `CancellationTokenSource` is admitted **warning-free**: the **synchronized** fact puts the type in language spec [§22.12](Bezel_Language_Spec.md#sec-static-members) rule 4's warning-suppression set. Suppressing that warning is right even though a residual member-level rule exists, and the reason is that the rule is *checked*: rule 4's warning asks the programmer to confine the instance to one domain or ensure its own internal synchronization, the synchronized fact discharges the second disjunct for the whole state-and-signal surface, and the one part that confinement was still protecting — registrations — is now excluded by a runtime raise rather than by an unenforced reminder. A **mutable** static of the type stays rejected: the **displacement-unbounded** fact keeps the type out of rule 2's form-(i) set, and overwrite-never-disposes is exactly the wrong store semantics for a source whose displaced value can no longer signal the tokens still observing it.

**Linked sources (#810).** `CreateLinked`/`CreateLinkedAll` complete the composition story: *"cancel if the caller cancels OR my local timeout fires"* binds **both** sources so the timeout source outlives the linked source:

```bezel
using var timeoutSrc := CancellationTokenSource.WithCancelAfter(timeout);
using var cts := CancellationTokenSource.CreateLinked(callerToken, timeoutSrc.Token);
```

— no hand-wired `Register` plumbing. A `WithCancelAfter` result **must be bound** — an obligation on the program, not a rule the compiler enforces: left unbound the expression is admitted, and the resulting unbound owned temporary is disposed at the end of the full expression that produced it (spec [§28.3](Bezel_Language_Spec.md#sec-unbound-owned-temporaries)), which cancels the source (cancel-on-dispose, below) and severs the just-created link, so `cts` is born cancelled. The consequence is entirely a runtime one; no diagnostic marks the unbound form. Reverse-order disposal (`cts` before `timeoutSrc`) satisfies the source-outlives-token rule. Semantics: the linked source enters the cancelled state when **any** parent token cancels or its own `Cancel()`/`CancelAfter()` fires, whichever happens first; a parent already cancelled at creation cancels the linked source before `CreateLinked` returns (the same race-free synchronous policy as `Register` on a cancelled token, below). Disposing the linked source **unregisters from its parents first** — no callback can fire into a disposed source. Lifetime follows the existing source-outlives-token rule ([§28.13](Bezel_Language_Spec.md#sec-shared-ownership) posture, spec §30.8): the parent *sources* must outlive the linked source, exactly as they must outlive any other holder of their tokens. Linking crosses domains where the parent source does: a domain holding a handle from a `Shared<CancellationTokenSource>` may link a child against a token borrowed through it. The link's own edge is runtime machinery touching only the parent's synchronized state, but the child it feeds is not: cancelling a published parent from any domain drives the edge, the child enters the cancelled state, and the child's **own** registered callbacks would then run on the cancelling domain's thread. That is why publication propagates to the linked child (above), which is what keeps the transitive path closed. Where no source is published, cross-domain cancellation remains a message on a channel (rationale §12.6).

**Timeout-as-a-scope needs no new type.** The RFC's `CancellationScope` sketch is already expressible: the source's destructor *cancels on dispose*, so `using var cts := CancellationTokenSource.WithCancelAfter(delay);` **is** a cancellation scope — auto-cancelled at scope exit or on timeout, whichever first. Disposal performs a **full `Cancel()`**: registered callbacks that have not yet run are invoked synchronously on the disposing thread *before* the registration list is severed — `Register`-based cleanup therefore fires at scope exit, and §28.14's quiescence (no callback after disposal completes) holds. Recorded as the idiom rather than a wrapper type (see the v0.173 rationale entry).

**`ThrowIfCancellationRequested()`.** Raises `OperationCanceledException` (language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)) when cancellation has been requested on the token's source, and returns with no effect otherwise — the polling half of the signal-only idiom above, asking the same question `IsCancellationRequested` answers as a value. `CancellationToken.None` has no source and is never in the cancelled state, so it never raises. It is the same raise a compiler-generated async iterator's `MoveNextAsync()` performs implicitly on the enumerator's token (language spec [§30.7](Bezel_Language_Spec.md#sec-async-iterators)).

`Register` invokes the callback when cancellation is requested; the returned `Subscription` removes the registration when disposed. The `callback` parameter is `scoped` (spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters)): the registration stores the callback bounded by the returned `Subscription` (the owned-return bounding form — `using var reg := token.Register(...)`), and disposal severs the registration, which is the quiescence guarantee.

If the token is **already in the cancelled state** at the moment `Register` is called, the callback is invoked synchronously on the calling thread before `Register` returns, and the returned `Subscription` is one whose `Dispose()` is a no-op (the registration was never installed because there was nothing to install — cancellation has already happened). Code receiving the `Subscription` should not assume the callback has not yet run.

This matches C#'s `CancellationToken.Register` behavior. The synchronous-fire policy is the only race-free choice: any policy that schedules the callback for later, or that ignores the registration on an already-cancelled token, would create a window in which the caller could observe an inconsistent state ("I registered a callback but it never fired, even though the token is cancelled").

`WithCancelAfter(delay)` is a static factory that constructs a `CancellationTokenSource` and immediately schedules it to cancel after `delay`. It is the recommended form for "create a source that auto-cancels after a fixed time" (a constructor's only job stays initialization — the language-wide preference against side-effect-bearing constructors). To set or extend the delay on an existing source, use `CancelAfter(delay)`.

`CancellationTokenSource.IsCancellationRequested` and `source.Token.IsCancellationRequested` always return the same value: the source's property is a convenience for callers that hold the source directly, equivalent to `source.Token.IsCancellationRequested`. The duplication on the API surface matches C#'s analogous shape and exists for ergonomics — code that already has the source on hand shouldn't have to traverse to the token to ask the question.

<a id="sec-timeoutexception"></a>
#### `TimeoutException`

```bezel
public class TimeoutException: Exception
  public constructor();
  public constructor(message: string);
  public constructor(message: string, inner: Exception);   // borrows inner; snapshot copied into CauseChain (spec §31.4) — no reference retained
end TimeoutException;
```

Raised by operations that exceed a deadline. Distinct from `OperationCanceledException`, which signals cancellation by an explicit `CancellationToken`; `TimeoutException` is for time-budget exhaustion where no token is involved. The standard library's own timeout-taking operations signal deadline by returning `null` (Try shape (b), §1 — `TryAcquire(timeout)`, `TryLock`/`TryRead`/`TryWrite` and their `Async` forms) and do **not** raise this type; it is provided for user libraries and future modules (e.g. `System.Net`) whose operations have no nullable failure shape. Declared in `System.Threading`; available once that module is imported (it is not in the auto-imported set).

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

**Sendability.** `Mutex` is *sendable* (language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)) and internally synchronized — like `Channel<T>` ([§6.9](#sec-channel)), it may be reached by multiple domains simultaneously; that is its purpose. `Lock` is **not** sendable: a held guard is bound to the acquiring domain and is released there (moving a held lock between domains would decouple acquisition from release and break the `using var` discipline that makes the guard idiom sound).

**Visibility (language spec [§30.13](Bezel_Language_Spec.md#sec-cross-thread-visibility)).** Releasing a `Lock` — disposing it — *publishes* the holder's writes: the release happens-before the next acquisition of the same `Mutex`, whichever domain or thread (including an FFI foreign thread) acquires next. `Semaphore`/`SemaphoreToken` ([§6.3](#sec-semaphore)) carry the same edge.

**Facts** ([§1](#sec-statics-and-sharing-facts)): `Mutex` — **synchronized** (the statement above: internally synchronized, reachable from every domain by design). `Lock` declares no facts (a held guard is domain-bound).

`AcquireAsync` and `TryAcquireAsync` are the async counterparts. `AcquireAsync` returns a `OwningTask<Lock>` (Pattern B, single-consumer); `TryAcquireAsync` returns `OwningTask<Lock?>` — the same nullable shape inside a Pattern B task. Consumers write:

```bezel
using var guard := await mutex.AcquireAsync();
// critical section; guard disposed at scope end.

using var maybe := await mutex.TryAcquireAsync(TimeSpan.FromSeconds(5));
if maybe <> null then
  // critical section
end;
```

`null` is itself the failure signal (Try shape (b), §1), producing simple call sites with no out-parameter contortion.

**Blocking discipline (spec §30.12).** `Acquire()` and `TryAcquire(timeout)` block the calling thread. They are rejected at compile time inside `async` methods and `async` lambdas (use `AcquireAsync`/`TryAcquireAsync`); where a non-`async` helper reached from a domain continuation would actually park in them, the runtime raises `InvalidOperationException` instead. They remain fully legal on the startup thread and on FFI foreign threads (outside any domain), and when the mutex is immediately available.

**Non-reentrant (language spec [§30.12](Bezel_Language_Spec.md#sec-thread-synchronization)).** A `Mutex` does not track its holder: a holder that acquires it again **waits on itself**, in every acquisition form — the blocking forms park (or hit the runtime guard above where a domain continuation would park), the `Async` forms never complete, and `TryAcquire`/`TryAcquireAsync` return `null` at the timeout. A nested critical section acquires once and passes the guard; it never re-acquires. `Locked<T>` and `RwLocked<T>` ([§6.11](#sec-locked-and-rwlocked)) carry the same posture.

**Cancellation of async acquisition (the §30.8 convention made concrete).** Cancelling the token of a pending `AcquireAsync`/`TryAcquireAsync` faults the returned task with `OperationCanceledException`, and a cancelled acquisition **never holds the mutex**: an acquisition racing cancellation is released internally before the fault surfaces — the give-back shape of §6.9's receive abandonment — so `using var g := await m.AcquireAsync(tok);` cannot leak a held lock on the cancellation path. In the `Try*Async` forms, timeout completes the task with `null` and cancellation faults it; a wait that both timed out and was cancelled reports whichever the implementation observed first. The same contract applies verbatim to `Semaphore` ([§6.3](#sec-semaphore)) and to the `Locked<T>`/`RwLocked<T>` acquisition families ([§6.11](#sec-locked-and-rwlocked)).

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

`Acquire()` and `TryAcquire(timeout)` follow the same blocking discipline as `Mutex`'s (spec §30.12): compile error inside `async` bodies, runtime `InvalidOperationException` where a domain continuation would park in them, legal outside domains. The async forms follow §6.2's cancellation contract: fault with `OperationCanceledException`, a cancelled acquisition never holds the permit, null-on-timeout in the `Try` forms.

**Permits (language spec [§30.12](Bezel_Language_Spec.md#sec-thread-synchronization)).** A `Semaphore` holds a count of **permits**, `initialCount` of them at construction. Each successful acquisition consumes one and yields the `SemaphoreToken` that holds it; disposing the token returns it. An acquisition attempted while no permit remains waits — `Acquire`/`AcquireAsync` until one is returned, `TryAcquire`/`TryAcquireAsync` until then or until the timeout, whichever comes first. A holder acquiring again is **not** re-entrancy and is not a deadlock: it consumes a second permit and is legal while permits remain (contrast `Mutex`, [§6.2](#sec-mutex-and-lock)).

**`maxCount`.** The two-argument constructor's `maxCount` is the semaphore's **permit ceiling**, and its whole reachable effect is at construction: `new Semaphore(initial, max)` raises `ArgumentException` when `max < 1` or `initial > max`, and the one-argument form has no ceiling. The ceiling is self-maintaining thereafter and needs no run-time check, which is the fact worth stating rather than leaving a reader to wonder at: a permit is returned **only** by disposing a `SemaphoreToken`, and a token exists only because an acquisition consumed a permit, so the count can never rise above where it started. There is no release-more-than-you-took path to cap — the shape a ceiling exists to guard in semaphore APIs that release by count. Passing `maxCount` therefore validates the initial count and documents the author's intent; it does not gate any later operation.

**Sendability (language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)).** `Semaphore` is **sendable** and internally synchronized — like `Mutex` ([§6.2](#sec-mutex-and-lock)) it may be reached by multiple domains simultaneously; that is its purpose (`Shared<Semaphore>` is the cross-domain sharing idiom, language spec §28.13). `SemaphoreToken` is **not** sendable: a held permit is bound to the acquiring domain and released there, like `Lock` ([§6.2](#sec-mutex-and-lock)) — moving a held permit between domains would decouple acquisition from release.

**Facts** ([§1](#sec-statics-and-sharing-facts)): `Semaphore` — **synchronized** (the *Sendability* statement above: internally synchronized and reached by multiple domains simultaneously by purpose — the §6.2 mutex genus; spec §30.14). `SemaphoreToken` declares no facts (a held permit is domain-bound).

`AcquireAsync` returns `OwningTask<SemaphoreToken>`; awaiters consume the task under Pattern B (language spec [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)) — the await must appear in an owned-consuming context. The natural call site:

```bezel
using var token := await sem.AcquireAsync();
DoCriticalWork();
// token disposed at scope end -> semaphore released.
```

<a id="sec-atomict-and-atomicintegert"></a>
### 6.4 `Atomic<T>` and `AtomicInteger<T>`

Atomic operations on primitive types. The split into two classes — one for the operations that make sense on any cell-sized primitive, one for the integer-arithmetic extensions — makes the intrinsic restriction visible in the type system: `Increment`, `Decrement`, and `Add` are members of `AtomicInteger<T>` and unavailable on `Atomic<T>`, so a typo like `var b := new Atomic<bool>(false); b.Increment();` is a name-resolution error rather than an opaque intrinsic-restriction error.

**Facts** ([§1](#sec-statics-and-sharing-facts)): `Atomic<T>`, `AtomicInteger<T>` — **synchronized** (sequentially-consistent intrinsic operations — the §6.4 ordering statement below; sendable and safe to reach from every domain, spec §30.14).

<a id="sec-atomict-load-store-exchange-compare-and-swap"></a>
#### `Atomic<T>`: load, store, exchange, compare-and-swap

`T` is restricted by the compiler to the primitive types `bool`, `char`, `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `IntPtr`, and `UIntPtr`. This restriction is stricter than the `where T: value` constraint expressed in the declaration and is enforced as a compiler-intrinsic rule. Instantiating `Atomic<T>` with any type outside that list — a user-defined record, an enum, a floating-point type, or any other value type (the admitted `IntPtr`/`UIntPtr` are themselves intrinsic records, §10.0 — hence "outside that list", not "records") — is a compile error: *"Atomic<T> requires a cell-sized primitive type; T is not eligible. For floating-point atomics, use Atomic<int> with a bit-cast; for record-typed atomics, use a Mutex."*

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

`Value`'s read and write are atomic with respect to other `Value` reads, `Value` writes, and the `Exchange`/`CompareExchange` methods on the same instance. All §6.4 atomic operations are **sequentially consistent**: they participate in a single global order; no weaker ordering is offered. (Relaxed/acquire-release variants were considered and declined — see the changelog, v0.140; the surface stays minimal until profiles demand otherwise.) `Atomic<T>` and `AtomicInteger<T>` are **sendable** (language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)) and internally synchronized, and participate in the `Shared<T>` cross-domain rule of language-spec §28.13. The accessors are compiler-intrinsic; they are not auto-property storage.

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

An **event** is a named publication point on a class: the class controls when the event fires; external code subscribes but cannot fire the event or inspect the subscriber list. Events are not a member kind (language spec [§24](Bezel_Language_Spec.md#sec-events), rationale §13.6); an event is the following **pattern** over the types of this section, and this section is the normative home of event semantics. The generic parameter `T` is the event's **payload type** — the type of value passed to handlers when the event fires. The payload type may be any type, except that `AsyncEventSource<T>` admits no value type that holds borrows (the `RaiseAsync` bullet below states that rule). Three interactions are worth knowing; the first two are not rules of this section, and the third carries the one rule:

- **An async handler *written as an `async` lambda* cannot take a borrow-bearing payload**, because [§30.1](Bezel_Language_Spec.md#sec-async-methods) rejects such a parameter there; a plain lambda or a method reference returning an already-completed task is unaffected.
- **A move-only payload is not admitted at the source classes.** `EventSource<T>` and `AsyncEventSource<T>` admit no move-only value type (language spec [§15.3](Bezel_Language_Spec.md#sec-move-only-value-types) — an owning inline array or an owning choice) as `T`: a compile error at the instantiation site, through any chain of user declarations (language spec [§28.11](Bezel_Language_Spec.md#sec-collection-element-ownership), [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties) rule 2). The ground is the interaction itself: a move-only payload is passed by value-move, leaving the argument moved-from after the first handler's call, which does not compose with more than one subscriber. `IEvent<T>` and `IAsyncEvent<T>` carry no such rule: a user implementation's own raise is checked where it reads the payload.
- **`RaiseAsync` retains its payload until the returned task settles** — it awaits each handler's task before invoking the next (*Raising*, below), so the payload outlives `RaiseAsync`'s synchronous return exactly as an `async` method's parameters outlive theirs ([§30.1](Bezel_Language_Spec.md#sec-async-methods)); the absence of an `async` marker on the declaration is a fact about signatures ([§28.12](Bezel_Language_Spec.md#sec-asynchronous-disposal)), not about the frame. That retention is normative, and so is the obligation it places on the **raiser**: every borrow the payload carries must outlive the settlement of the task `RaiseAsync` returns, and the borrowed object must not be transferred to another domain before that settlement — [§30.1](Bezel_Language_Spec.md#sec-async-methods)'s held-until-settlement obligation, in its entirety, discharged trivially by `await`ing the raise in the scope that owns the borrow sources and carried live only by the abandonment shapes ([§30.10](Bezel_Language_Spec.md#sec-standard-library-task-helpers)). The payload's carried borrows are class- or interface-typed, and the obligation has the ordinary checked-mode backstop ([§28.2](Bezel_Language_Spec.md#sec-the-idisposable-contract)): **`AsyncEventSource<T>` admits no payload type that is a value type that holds borrows** (language spec [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)'s holds-borrows property: a borrow-bearing record — a span ([§4.14](#sec-owned-buffers-and-spans), [§4.15](#sec-arrayspant-readonlyarrayspant)) among them — a borrow-bearing inline array, or a choice that holds borrows). `AsyncEventSource<Pair>`, `AsyncEventSource<ReadOnlySpan<byte>>` and `AsyncEventSource<[2]Item>` are **compile errors at the instantiation**, wherever the type is formed — explicit, inferred, or through a user generic's substitution (`class Fan<T>` holding an `AsyncEventSource<T>` is rejected at `Fan<Pair>` wherever it is named: an instantiation-site rule in language spec [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties) rule 2's sense). `CancellationToken` is excepted on [§30.1](Bezel_Language_Spec.md#sec-async-methods)'s certified footing (axiom A10), its source obligation [§30.8](Bezel_Language_Spec.md#sec-cancellation)'s. The ground is §30.1's for an `async` parameter: the raise frame holds the payload past the synchronous call, a position whose lifetime the payload's carried sources do not provably bound, and a value type that holds borrows is stored past its bound at no position ([§9.7](Bezel_Language_Spec.md#sec-records-and-disposal)'s instance-field bullet; an `async` frame, [§30.1](Bezel_Language_Spec.md#sec-async-methods); a task result, [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)). The remedy is a class payload — the buffer or collection object itself, or a class holding it with an offset and a length — raised under the obligation above, or borrow-free data; a synchronous `EventSource<T>`, whose `Raise` is call-bounded, takes any payload type ([proposal 0154](proposals/0154-no-async-event-payload-that-holds-borrows.md)).

Whether the first should become a restriction on `EventSource<T>`/`AsyncEventSource<T>` instantiation is an open question, not settled here; the reopening bar recorded in [`DECLINED.md`](proposals/DECLINED.md) stands for it. The second is the source classes' instantiation-site rule above. The third is `AsyncEventSource<T>`'s rule above, stated at the type as language spec [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)'s `Task<T>` row is ([proposal 0154](proposals/0154-no-async-event-payload-that-holds-borrows.md)). For a synchronous event the handler type is `(T) -> ()`; for an asynchronous event, `(T) -> Task`.

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
using var sub := button.Clicked.Subscribe(lambda(e) => UpdateStatus(e.X, e.Y));
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

  public method Add(scoped handler: (T) -> ());         // scoped: Add stores — spec §28.14
  public method Remove(handler: (T) -> ());              // call-bounded: Remove only compares
  destructor;    // severs the subscriber list before reclamation (spec §28.14 quiescence)
end EventSource;

// T: not a value type that holds borrows (CancellationToken excepted) — the payload is held until settlement (above).
public class AsyncEventSource<T>: IAsyncEvent<T>
  public constructor();
  public method Subscribe(scoped handler: (T) -> Task): owned Subscription;
  public method RaiseAsync(payload: T): Task;   // `async` is implementation-side and does not
                                                //   appear in a signature (spec §28.12)

  public method Add(scoped handler: (T) -> Task);        // scoped: Add stores — spec §28.14
  public method Remove(handler: (T) -> Task);             // call-bounded: Remove only compares
  destructor;    // severs the subscriber list before reclamation (spec §28.14 quiescence)
end AsyncEventSource;
```

The constructors are public: the pattern constructs its own sources, and standalone construction threatens nothing — a third party constructing a *fresh* `EventSource<T>` gains capability over its own event only.

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
    _sub := ticks.Subscribe(lambda(e) => do OnTick(e); end);   // captures `this` — admitted:
  end constructor;                                        //   bounding object is `this` (§28.14 form 3)

  private method OnTick(e: TickEvent)
    _count := _count + 1;
  end OnTick;
end TickObserver;
```

A captured constructor parameter other than `this` is not admitted **where it contributes a borrow source** (spec [§28.3](Bezel_Language_Spec.md#sec-borrow-lifetime-rules)) — a class-, interface-, or plain function-typed parameter, or an `owned` one: such a parameter is a borrow of a caller local (or a callback the call bounds) and does not provably outlive the constructed object. A parameter contributing **no** source — a value-typed one that holds no borrows, or a runtime-managed `string` (spec [§28.9](Bezel_Language_Spec.md#sec-runtime-managed-types)) — carries no lifetime hazard and *is* admitted, while a value-typed parameter that holds borrows (a span, a borrow-bearing record, a `CancellationToken`) contributes the sources of the borrows it carries (spec [§28.3](Bezel_Language_Spec.md#sec-borrow-lifetime-rules)'s provenance clause 1) and is not: the admission is keyed on borrow sources, not on the capture's being a parameter (spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters), *v1 admission bounds* item 1). A raise cannot reach the handler before the constructor completes unless the constructor itself — directly or transitively — raises the subscribed event: instance sources are domain-serial (below) and no other continuation runs during construction. A constructor that does so observes `this` partially initialized — the construction-time-observation residual of spec §22.7, with the runtime bound recorded there. For handlers that must not capture `this`, `using var sub := …` remains the scope-bound alternative (spec §28.14 form 2).

**Raising.** `Raise` and `RaiseAsync` invoke handlers in subscription order, over a **snapshot of the subscriber list taken when the raise begins**: a handler that subscribes or unsubscribes during the raise affects the *next* raise, never the in-flight pass — snapshot rather than fault, because self-unsubscribing one-shot listeners are a legitimate idiom (spec §29.12's loud-mutation discipline is the contrasting response to mutation mid-traversal; the correction of the former spec §24.5 "same posture" comparison is recorded in rationale §13.6). With zero subscribers, both methods are no-ops — no null check, no `?.`, no `Invoke` indirection. Handlers execute **on the raising domain's thread** (normative): for an instance source this is invisible (all access is domain-serial); for a shared source (below) a handler subscribed from one domain runs on whichever domain raises — which is why shared-source handlers must be capture-free. The synchronous `Raise` returns `()` and runs each handler to completion before invoking the next. `RaiseAsync` returns a `Task` that completes when every handler's task completes; **each handler's task is awaited before the next is invoked.**

**Handler exceptions — first error wins (rationale §13.5).** If a synchronous handler throws, subsequent handlers are not invoked and the exception propagates out of `Raise` to the raiser. If an asynchronous handler's task faults, subsequent handlers are not invoked and the exception propagates from the awaited `RaiseAsync`. If an asynchronous handler's task is **cancelled**, the behavior is the same as a fault: subsequent handlers are not invoked and `OperationCanceledException` propagates from the awaited `RaiseAsync`. To run all handlers regardless of failures and aggregate exceptions, the raiser must implement that policy explicitly — `Raise`/`RaiseAsync` do not aggregate. Handlers that must run independently catch their own exceptions (fragment context: `_messageReceived` is the connection's own source field, so the async lambda's bare `ProcessPayloadAsync`/`Log` calls capture `this` and are admitted by spec §28.14's subscription admission):

```bezel
// The payload here is a `string`-only record, so it is not borrow-bearing (spec §9.7)
// and is admissible as an `async` lambda's parameter (spec §30.1):
//   record MessageReceivedEvent(Channel: string, Body: string);
_messageReceived += async lambda(e) => do
  try
    await ProcessMessageAsync(e.Body);
  catch
    var ex: Exception => Log($"Handler failed: {ex.Message}")
  end;
end;
```

**Thread safety, per type.** An `EventSource<T>`/`AsyncEventSource<T>` subscriber list is **not synchronized**, and within a single execution domain (spec §30.13) it does not need to be: continuations never run concurrently, so subscribe, unsubscribe, and raise — from sync or async code — are serialized by construction (the reasoning is recorded). When handlers are async, the subscriber list may still change between a raise's suspension points; that is interleaving, not a data race. External synchronization (`Mutex`, [§6.2](#sec-mutex-and-lock)) is required only where event operations are reachable from foreign threads through the FFI (spec §32). A `SharedEventSource<T>` (below) is the internally-synchronized exception. Quiescence is normative for every source type of this section: the source's destructor severs the subscriber list before reclamation (spec §28.14), so a publisher's disposal is a deterministic unsubscribe-all.

**The type fence.** `Add`, `Remove`, and `Raise` are `public` rather than `internal` because the producer-side restriction is carried by **types**: the producer surface of a given event is fenced by the **field's** visibility. An `EventSource<T>` reference for someone else's event is reachable only if their class exposes it — or if a holder of the interface face downcasts it (`face as EventSource<T>`), which is the recorded, not-defended-against breach: a type-driven fence is exactly as strong as the type system's static view, and downcast is that view's built-in escape. Code that smuggles a source out of its class — through a public member or through the FFI — gains `Raise`, as a deliberate breach of the pattern's contract that the language does not attempt to defend against. The conventional design is to never expose the `EventSource<T>` itself; the public surface is always the interface face. Third-party construction of a *fresh* source (the public constructor) grants no capability over anyone else's events.

**Variance.** `IEvent<T>` and `IAsyncEvent<T>` are covariant in T (`out T`): `IEvent<Dog>` is a subtype of `IEvent<Animal>`, the same direction as `IEnumerable<out T>` — sound because T appears once, in the parameter position of `(T) -> ()` (contravariant), itself a method parameter (contravariant), composing to covariant overall (spec §12.9). The source classes are invariant — `Raise(payload: T)` places T in input position. The conventional public surface is the interface, so user-facing variance is expected through `IEvent<out T>`/`IAsyncEvent<out T>` (and `ISharedEvent<out T>`); an exposed source is invariant, as its signature dictates. Covariant exposure is directly declarable by the pattern: an `EventSource<Dog>` field behind a `property Sightings: IEvent<Animal>` face.

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

- **Synchronization.** The subscriber list is internally synchronized: subscribe, raise, and `Subscription.Dispose` may arrive from any domain. A raise takes the snapshot; handlers execute on the raising domain; first-error-wins applies unchanged. The destructor severs the list under the same lock as subscribe/raise — a concurrent subscribe either lands before the severance and is severed, or observes the disposed source; program-exit disposal of the recommended `static owned readonly` field runs per spec §22.13 on the shutdown path with no other Bezel code running, which makes exit-time severance race-free.
- **Capture-freedom at every subscribe position.** Every subscribe position of the pair (`Subscribe` on the interface and the class, `Add`, and the `+=` desugaring, spec §17.2) admits only **capture-free** handlers — a non-capturing lambda or a static method reference — as a stdlib-stated entry in spec §28.14's capture-free register (`Domain.Spawn`'s admission), keyed on the **resolved callee**: the check applies wherever a call resolves to a subscribe position declared on the pair, a constrained generic receiver included. Two companion rules make the admission alias-proof: `SharedEventSource<T>` deliberately does **not** implement `IEvent<T>` (an upcast would launder the requirement through `IEvent<T>.Subscribe`'s capture-admitting `scoped` signature), and forming a method reference over a capture-free-register position is a compile error (spec §28.14 — the formed plain function type would erase the admission). The requirement is therefore enforced on every route through which a subscription can be written. A payload never needs to be sendable: under capture-freedom plus raising-domain execution, each payload is constructed and consumed within one domain.
- **The severed `IEvent<T>` composition, and its bridge.** A shared source does not compose with `IEvent<T>`-consuming APIs (the `TickObserver` shape above). The bridge for that use case is a domain-local relay: subscribe a capture-free forwarder that routes payloads through a process-global `ChannelSender<T>` static (spec §22.12 rule 2 form (i)); the consuming domain owns the `Channel<T>`, receives, and re-raises on a local instance `EventSource<T>`, whose `IEvent<T>` face composes with anything. Payloads crossing the bridge must be sendable (spec §30.14) — inherent to any cross-domain delivery.

  The bridge, complete. The one non-obvious move: the static must be a **mutable** `ChannelSender<T>?` (spec §22.12 rule 2 form (i)) wired at startup, because the sender is minted from a channel the *consuming* domain owns — a `static readonly Channel<T>` is the wrong end and draws rule 4's warning; overwriting a wired sender never disposes the displaced one (rule 2's store semantics).

  ```bezel
  public record PriceTick(Symbol: string, Price: decimal);      // sendable payload

  public static class TickBus
    private static owned readonly _feed: SharedEventSource<PriceTick> := new();
    public static property Feed: ISharedEvent<PriceTick> => _feed;
    public static method Publish(t: PriceTick) _feed.Raise(t); end Publish;

    public static Tap: ChannelSender<PriceTick>?;   // MUTABLE static, form (i); nullable = starts unwired
    public static method Forward(t: PriceTick) Tap?.Send(t); end Forward;   // static ⇒ capture-free
  end TickBus;

  // In the consuming domain (its startup continuation):
  using var inbox := new Channel<PriceTick>();
  TickBus.Tap := inbox.NewSender();                          // wire: minted FROM this domain's channel
  using var fwd := TickBus.Feed.Subscribe(TickBus.Forward);  // capture-free handler ✓
  using var local := new EventSource<PriceTick>();           // local re-raise point; IEvent<T> face
  using var sub := local.Subscribe(lambda(t) => UpdatePanel(t));   // ordinary §28.14 capture rules apply here
  await for var t in inbox.ReceiveAll() do
    local.Raise(t);                                          // domain-local delivery
  end;
  ```
- **User implementations of `ISharedEvent<T>`** are admissible but gain no retention right: a user-authored `Subscribe` body is verified under spec §28.14's ordinary call-bounded regime, so it can neither store its handler parameter nor forward it into a register position. **User classes cannot be cross-domain publication points in v1** (recorded; the lift path is §28.14's deferred unbounded tier, #1427). `SharedEventSource<T>` is `sealed`, so no derived class inherits the trusted position.
- **Statics.** `SharedEventSource<T>` is in spec §22.12 rule 2's form-(i) synchronized set: the recommended shape `private static owned readonly _exiting: SharedEventSource<()> := new();` is admitted warning-free, with program-exit disposal (spec §22.13). The recommended public face is a static expression-bodied property of the interface face, as in the idiom below — a bare read of a `static` `owned` field returned as a borrow of process-lived static storage (spec §28.3/§28.6). A *mutable* static of the type is admissible under rule 2's store semantics; note that overwrite-never-disposes means a displaced source leaks with its subscriber list intact until process exit — prefer `readonly`. The type is **not sendable** (spec §30.14): static reachability, not transfer, is its distribution mechanism — and it is deliberately not in spec §28.13's `Shared`-payload set (`Shared<SharedEventSource<T>>` would add a second distribution mechanism — the self-distributing fact, [§1](#sec-statics-and-sharing-facts); the Facts line below).
- **The shared async twin is deferred.** `ISharedAsyncEvent<T>`/`SharedAsyncEventSource<T>` (a synchronized `RaiseAsync` awaiting each handler's task on the raising domain) ships when demand appears; until then a cross-domain async publication point is not spellable, and the channel bridge above is the alternative.

**Facts** ([§1](#sec-statics-and-sharing-facts)): `SharedEventSource<T>` — **synchronized**, **self-distributing** (distributed by static reachability, spec §22.12 rule 5; wrapping it in a spec-§28.13 handle would add a second distribution mechanism). *Displacement-unbounded* was considered and declined: a displaced source leaks with its subscriber list intact (the *Statics* bullet above), an accepted cost — mutable statics of the type stay admitted. The instance sources `EventSource<T>`/`AsyncEventSource<T>` declare no facts (unsynchronized, domain-serial — above).

**Sendability** ([§1](#sec-sendability-statements); language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)). `EventSource<T>` and `AsyncEventSource<T>` are **not** sendable: the subscriber list is unsynchronized and domain-serial (*Thread safety, per type*, above). `SharedEventSource<T>` is **not** sendable either, although it is internally synchronized: static reachability, not transfer, is its distribution mechanism (language spec §22.12 rule 5).

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

All six event types live in `System.Threading` and are always available without import (language spec [§2.3](Bezel_Language_Spec.md#sec-always-available-names) — the `CancellationToken` precedent). `Subscription` (the return type of `Subscribe`) is declared in [§2.11](#sec-subscription) and lives in the auto-imported `System` module ([§2.11](#sec-subscription) records why).

<a id="sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet"></a>
### 6.6 `TaskCompletionSource<T>` and `OwningTaskCompletionSource<T>`

See [§30.11](Bezel_Language_Spec.md#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet) of the language spec.

```bezel
// For non-class T (copyable value types, string, runtime-managed reference types).
// Instantiating with a class T is a compile error pointing to OwningTaskCompletionSource<T>;
// a move-only value T, or a value type that holds borrows (CancellationToken excepted),
// is rejected per spec §30.5 (its Task<T> is itself ill-formed).
public class TaskCompletionSource<T>
  public constructor();
  public readonly property Task: Task<T>;

  public method SetResult(result: T);
  public method SetException(owned exception: Exception);   // the task owns its fault (spec §30.9)
  public method SetCancelled();

  public method TrySetResult(result: T): bool;
  public method TrySetException(owned exception: Exception): bool;
  public method TrySetCancelled(): bool;
  destructor;    // disposes the produced task, faulting it first if the source
                 //   is disposed before completion (spec §30.11)
end TaskCompletionSource;

// For class T. Result is transferred via `owned` parameter; produced task is Pattern B.
public class OwningTaskCompletionSource<T> where T: class
  public constructor();
  public readonly property Task: OwningTask<T>;

  public method SetResult(owned result: T);
  public method SetException(owned exception: Exception);   // the task owns its fault (spec §30.9)
  public method SetCancelled();

  public method TrySetResult(owned result: T): bool;
  public method TrySetException(owned exception: Exception): bool;
  public method TrySetCancelled(): bool;
  destructor;    // disposes the produced task, faulting it first if the source
                 //   is disposed before completion (spec §30.11)
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
                 //   is disposed before completion (spec §30.11)
end TaskCompletionSource;
```

**Fault-path ownership.** `SetException`/`TrySetException` (all forms) take the exception **`owned`**: the produced task becomes the single owner of its fault exception (spec §30.9 — awaiters borrow it on re-raise; it is disposed exactly once, at task disposal; a `TrySetException` returning `false` disposes the passed exception). A caller that holds only a caught borrow (spec §31.1) constructs a fresh cause-chained exception instead: `tcs.SetException(new Exception(ex.Message, ex));` — the `(message, cause)` constructor snapshots the chain (spec §31.4).

<a id="sec-task-static-helpers"></a>
### 6.7 `Task` static helpers

Language spec [§30.10](Bezel_Language_Spec.md#sec-standard-library-task-helpers) shows the helpers its rules read; the full set is declared below. They live on `Task`.

**Sendability** ([§1](#sec-sendability-statements); language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)). `Task`, `Task<T>` and `OwningTask<T>` are **not** sendable: a task is domain-local, its continuations and result buffer belonging to the domain that awaits it (language spec §30.13). Values cross a domain boundary as `Domain.Spawn` arguments and results ([§6.8](#sec-domain)) or as channel elements ([§6.9](#sec-channel)).

**Task-family returns are caller-owned productions.** Every method in this library whose declared return type is `Task`, `Task<T>`, or `OwningTask<T>` — the async acquisitions (`AcquireAsync`/`TryAcquireAsync`, §6.2–§6.3), `Locked<T>`/`RwLocked<T>`'s `LockAsync`/`ReadAsync`/`WriteAsync` families (§6.11), `Channel<T>.Receive`/`SendAsync` (§6.9), stream and file I/O (`ReadAsync`/`WriteAsync`/`ReadAllAsync`, §7.1–§7.2; `ReadAllTextAsync`/`WriteAllTextAsync`, §7.3), `RaiseAsync` (§6.5), `TaskScope.Join` (§6.10), `IAsyncDisposable.DisposeAsync` and `MoveNextAsync` (§2.10, §4.12–§4.13), and the static helpers below — mints a **fresh task owned by the caller** at every call site (language spec [§28.3](Bezel_Language_Spec.md#sec-producers)). The signatures carry no `owned` marker: the `Task`-family return type is itself the transfer announcement (type-announced). Where a signature does spell it out — `Domain.Spawn(...): owned Task` (§6.8), the `Parallel` combinators (§6.12) — the marker states the same contract explicitly and is retained for emphasis at the domain boundary; it is not a semantic difference from the unmarked async forms. Stored tasks are exposed as **properties** (`TaskCompletionSource<T>.Task`, `OwningTaskCompletionSource<T>.Task`, §6.6), whose reads are borrows of the source's storage (spec [§30.11](Bezel_Language_Spec.md#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet)).

```bezel
public class Task
  // For non-class T (copyable value types, string, runtime-managed reference types).
  // For class T, use FromOwnedResult instead; a move-only value T, or a value
  // type that holds borrows, is rejected per spec §30.5 (its Task<T> is ill-formed).
  public static method FromResult<T>(value: T): Task<T>;

  // For class T. Produces a Pattern B task; awaiters consume under `using var x := await ...`.
  public static method FromOwnedResult<T>(owned value: T): OwningTask<T> where T: class;

  // For any T over which Task<T> is well-formed (spec §30.5 — a move-only value T,
  // or a value type that holds borrows, is not): a faulted task never produces its result, so the Pattern A vs
  // Pattern B distinction does not apply. `FromException<MyClass>(ex)` is correctly
  // typed as Task<MyClass> even when MyClass is a class type — there is no value
  // to transfer ownership of, only an exception to re-raise on await.
  public static method FromException<T>(owned exception: Exception): Task<T>;   // the task owns its fault (spec §30.9)
  public static method FromCancelled<T>(): Task<T>;
  // Non-generic forms (spec §30.10). A call without a type-argument list selects
  // these whatever the target (spec §22.6's non-generic preference), so where a
  // Task<int> is wanted write FromException<int>(ex) or FromCancelled<int>().
  public static method FromException(owned exception: Exception): Task;   // fault ownership, as above
  public static method FromCancelled(): Task;

  public static method Delay(milliseconds: int, token: CancellationToken := CancellationToken.None): Task;
  public static method Delay(duration: TimeSpan, token: CancellationToken := CancellationToken.None): Task;

  public static method WhenAll(tasks: Array<Task>): Task;

  // For non-class T (value types, string, runtime-managed reference types).
  public static method WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>;

  // For class T: the owning WhenAll overload (the parameter type
  // distinguishes it; the call site is Task.WhenAll(move tasks)).
  // Aggregates OwningTask<T> results into an owned array of owned elements.
  // The caller transfers ownership of the input array (and its tasks);
  // the returned task produces an owned OwningArray<T>.
  public static method WhenAll<T>(
    owned tasks: OwningArray<OwningTask<T>>
  ): OwningTask<OwningArray<T>> where T: class;

  public static method WhenAny(tasks: Array<Task>): Task<int>;   // index of the first task to SETTLE — success, fault,
                                                                 //   or cancellation; never re-raises the winner's fault
                                                                 //   (inspect tasks[i]); empty array: ArgumentException
                                                                 //   synchronously (spec §30.10)
  public static method FireAndForget(owned task: Task);
end Task;
```

`FireAndForget` registers a continuation that logs any exception the task produces through the implementation-defined diagnostic channel (the closing note below is the single statement of mechanism), then disposes the task. The calling code relinquishes ownership (the parameter is `owned`); the runtime supervises the task's lifecycle.

The parameter type `Task` accepts any subtype: `Task`, `Task<T>`, and `OwningTask<T>` all satisfy the parameter. For `OwningTask<T>` (Pattern B), the runtime's task disposal cascades through any unconsumed owned result — `FireAndForget(someOwningTask)` is semantically "let it run to completion; if it produces a result, dispose it; if it produces an exception, log it." This is the right pattern when the caller wants the underlying operation to complete its side effects (e.g., an async write whose `OwningTask<WriteHandle>` would normally yield a handle, but the caller doesn't need the handle).

**`FireAndForget` is the only supported way to discard a `Task`.** The discard form `_ := someAsyncCall();` is rejected at compile time because disposing a task abandons a running operation — and so is the anonymous binding `using var _ := someAsyncCall();` (and `await using var _ := …;`), which nothing can await and which is the same abandonment one scope later (language spec [§17.13](Bezel_Language_Spec.md#sec-the-discard-_)). The two legal patterns — the second of them **named** — are:

```bezel
Task.FireAndForget(DoTelemetryAsync());      // supervised; exceptions logged
using var t := DoTelemetryAsync();           // unsupervised; disposed at scope end
```

`FireAndForget` is the right choice when the task should run to completion and any exceptions it raises should be logged. The `using var` form binds the task to a scope; at scope end, the task's `Dispose()` runs but does not block waiting for completion ([§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)). The underlying operation continues to run in the background; the caller no longer observes its completion or exceptions. (In checked disposal mode, a `DisposedAccessException` raised in such an abandoned continuation is reported through the diagnostic channel before being swallowed — spec §30.10.) Use `using var` only when "fire and forget without supervision" is exactly what's wanted; for tasks whose failures should be surfaced, use `Task.FireAndForget` or hold the task and `await` it explicitly.

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

An `OwningTaskCompletionSource<T>` cannot express this shape as a return value (an earlier revision showed `return src.Task;` out of the `using var src` scope, which the source-owns-task contract of spec [§30.11](Bezel_Language_Spec.md#sec-common-semantics) rejects as a borrow escaping its source; the same example also passed an exception of unstated provenance where `SetException` requires a visibly-owned argument — a caught borrow additionally needs a fresh cause-chained instance, [§6.6](#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet)). The TCS remains the tool for **imperative** completion — callback bridging inside a live scope — not for minting detached faulted tasks.

<a id="sec-whenall-for-class-t-whenallowned"></a>
#### `WhenAll` for class T: the owning overload

The generic `WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>` aggregates results from non-class T (value types, `string`, runtime-managed reference types). For class T held by `OwningTask<T>`, use the **owning overload** — same name, its parameter type already distinct, its consuming form spelled by the call-site marker (`Task.WhenAll(move tasks)`; spec [§22.6](Bezel_Language_Spec.md#sec-owned-and-overload-resolution) — the merge that retired the former `Owned`-suffix dual name):

```bezel
public async method LoadAllConfigs(paths: Array<string>): OwningTask<OwningArray<Config>>
  var tasks := new OwningArray<OwningTask<Config>>();
  for var path in paths do
    tasks.Add(LoadConfigAsync(path));    // each task is OwningTask<Config>; transfers in
  end;
  return await Task.WhenAll(move tasks);      // tasks consumed; returned array owns the configs
end LoadAllConfigs;
```

The caller's pattern matches the borrowing form: build an array of tasks, await `WhenAll`, receive an array of results. The differences are in the type and ownership flow:

- The input array is `OwningArray<OwningTask<Config>>`, not `Array<OwningTask<Config>>`. Each element is itself owned (OwningTask<T> for class T is single-consumer per language spec [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal); the array owns its tasks for the duration).
- The input parameter is `owned OwningArray<...>`: the caller transfers ownership of the array (and its contents) to the aggregator — spelled at the call site by the transfer marker. After the call, the caller no longer holds the array.
- The result is `OwningTask<OwningArray<Config>>`: a Pattern B task whose result is an owned array of owned configs. The caller awaits with `using var`, gaining scope-bound disposal of the whole structure.

Implementation sketch (the *behavior* shown — await in input order, first-fault re-raise, disposal of already-produced and laggard results — is now contract, spec §30.10 "Aggregation contracts"; the code itself remains illustrative):

```bezel
public static async method WhenAll<T>(
  owned tasks: OwningArray<OwningTask<T>>
): OwningTask<OwningArray<T>> where T: class
  using var taskArray := move tasks;                  // owned param adopted (transfer marker); disposes at end
  var results := new OwningArray<T>(capacity := taskArray.Count);
  for var task in taskArray.Drain() do                // drain yields owned OwningTask<T>
    results.Add(await task);                          // await produces owned T (Pattern B);
                                                      //   Add(owned item) is an owned-consuming
                                                      //   context, satisfying §30.5
  end;
  return results;                                     // ownership transfers to caller
end WhenAll;
```

The implementation uses `OwningArray<T>.Drain()` to take ownership of each task out of the input array, awaits each (Pattern B), and transfers the awaited result into the result array via `Add(owned item)`. The pattern works because every collection slot in the chain is an owned-element slot, and because `Add(owned item)` is an owned-consuming context that admits `await` directly per the loosened Pattern B rule (language spec [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)). The for-loop's iteration variable `task` is owned (per the new rule for iterators returning `IOwningEnumerable<T>`, language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for) — "Iterator yield ownership"), and is consumed by the await on each iteration.

For genuine concurrency (the underlying tasks running in parallel), the caller should start the tasks *before* passing the array to the aggregator. Each task's underlying operation begins when the task is constructed; awaiting blocks for completion. The owning `WhenAll` body awaits sequentially, but the tasks themselves run in parallel because they were started earlier.

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
    using var f := move file;  // discharge on entry — covers all exit edges (transfer marker)
    await ProcessAsync(f);
  end;
end ProcessAll;
```

The `using var f := move file;` shape on entry to the body — the `move` marker is the binding-position transfer spelling (language spec [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows); unmarked, the binding is rejected, language spec [§28.10](Bezel_Language_Spec.md#sec-using-var)) — is the canonical way to discharge the iteration variable, exactly as for synchronous owned-element iteration (language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for)).

The async-iterator form is preferable when the consumer processes each result before requesting the next; the owning `WhenAll` overload is preferable when the consumer needs all results at once.

#### `WhenEach`: streaming fan-in (graduated from proposal 0023)

Where `WhenAll` collects every result before returning, `WhenEach` streams each task **as it completes** — the as-completed loop that otherwise requires a hand-built merge channel:

```bezel
// No-result form.
public static method WhenEach(tasks: Array<Task>): IAsyncEnumerable<Task>;

// Value-typed, string, or runtime-managed result (Pattern A). Each element is an
// already-completed Task<T>; await it to observe the result or re-raise its fault.
public static method WhenEach<T>(tasks: Array<Task<T>>): IAsyncEnumerable<Task<T>>;

// Owning overload for class T (Pattern B — same name, the parameter type
// distinguishes it, the call site is Task.WhenEach(move tasks)). Takes ownership
// of the array and every task in it; yields each completed OwningTask<T> as an
// owned iteration variable.
public static method WhenEach<T>(
    owned tasks: OwningArray<OwningTask<T>>
): IOwningAsyncEnumerable<OwningTask<T>> where T: class;
```

- **Completion order.** The first element yielded is whichever task completed first; ties break in unspecified order. An empty input produces an enumerator that completes immediately.
- **Faulted and cancelled tasks are yielded, not skipped**: the fault or `OperationCanceledException` surfaces when the caller awaits the yielded element — the caller decides per task, unlike `WhenAll`'s first-fault re-raise.
- **Observer, not owner.** The borrowing forms of `WhenEach` borrow their input: the returned iterable is a deferred-activation frame carrying the array's borrow (spec §28.3 carriage), so the compiler holds the array alive for the frame's bound; enumeration neither cancels nor settles the tasks. The owning overload is the ownership-transferring form: tasks not consumed before enumeration ends are disposed by the enumerator. Supervision (fault ⇒ cancel siblings) remains `TaskScope`'s job (§6.10; rationale §12.12) — `WhenEach` only reports.
- **Building the borrowed input.** Under spec §28.3's calls-mint rule a fresh `GetXAsync(…)` result is an *owned* task, and a borrowing `Array<Task<T>>` slot takes no ownership — so `tasks.Add(GetXAsync(url))` abandons the freshly-minted task at end of statement (the #1813 seam, shared with `WhenAll`'s input). Build the borrowed array over **named** task bindings (`var t1 := GetXAsync(a); … var tasks: Array<Task<string>> := [t1, t2];`), or use the owning overloads (`WhenEach(move tasks)`, `WhenAll(move tasks)`) for dynamically-sized fan-in — these two spellings are the complete input story (#1813, ruled 2026-07-24): dynamic fan-in over freshly-minted tasks is an ownership-transfer scenario by construction, so the owning forms are its spelling, and no borrowing-family dynamic-build facility is planned. (Should the give-back protocol (proposal 0060) later yield a general conditional-consume form, revisiting is a new proposal, not a reserved seam.)
- **Single consumption (affine).** Both returned sequences follow spec §29.11: a second `GetAsyncEnumerator` raises `InvalidOperationException`. The `CancellationToken` accepted by `GetAsyncEnumerator(token)` (the standard §30.7 shape) fires at `await MoveNextAsync()` boundaries and stops **enumeration only** — the tasks keep running and remain the caller's to settle.

```bezel
await for var t in Task.WhenEach(move tasks) do         // t: owned OwningTask<Report>
  using var report := await t;                          // Pattern B: result transfers
  Publish(report);
end;
```

<a id="sec-domain"></a>
### 6.8 `Domain`

Static class for spawning execution domains (language spec [§30.14](Bezel_Language_Spec.md#sec-multiple-domains)). `Spawn`'s explicit `owned Task`/`owned OwningTask<R>` returns state the calls-mint contract with emphasis — the owned-marked mint-announcing row of the spec's announcement table ([§28.3](Bezel_Language_Spec.md#sec-announcement-table)). The compiler enforces the call-site rules of §30.14: the entry must be a non-capturing lambda or a static method reference, and the argument and result types must be sendable (the derived type property of language spec [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)/[§30.14](Bezel_Language_Spec.md#sec-sendable-types)).

```bezel
public static class Domain
  // No result.
  public static method Spawn(entry: () -> ()): owned Task;
  public static method Spawn<A>(entry: (owned A) -> (), owned arg: A): owned Task
      where A: sendable, class;
  public static method Spawn<A>(entry: (A) -> (), arg: A): owned Task
      where A: sendable, notnull;
      // every plain `arg: A` and plain `R` row: a sendable value type, string or Type
      // only — a class A or R is a compile error at the call site (below)

  // Class-typed result: ownership transfers to the awaiter (Pattern B).
  public static method Spawn<R>(entry: () -> owned R): owned OwningTask<R>
      where R: sendable, class;
  public static method Spawn<A, R>(entry: (owned A) -> owned R, owned arg: A): owned OwningTask<R>
      where A: sendable, class where R: sendable, class;
  public static method Spawn<A, R>(entry: (A) -> owned R, arg: A): owned OwningTask<R>
      where A: sendable, notnull where R: sendable, class;

  // Value-typed, string or Type result (Pattern A).
  public static method Spawn<R>(entry: () -> R): owned Task<R>
      where R: sendable, notnull;
  public static method Spawn<A, R>(entry: (owned A) -> R, owned arg: A): owned Task<R>
      where A: sendable, class where R: sendable, notnull;
  public static method Spawn<A, R>(entry: (A) -> R, arg: A): owned Task<R>
      where A: sendable, notnull where R: sendable, notnull;

  // Async entries (spec §30.14 "Async entries") — nine rows elided: every
  // synchronous row above has an async twin whose `entry` returns `Task` /
  // `OwningTask<R>` / `Task<R>` in place of `()` / `owned R` / `R`, with the same
  // parameters, the same `where` clauses and the same return type; the elided rows
  // are declared members on exactly those terms. The runtime awaits the returned
  // task inside the spawned domain; the join handle tracks that task.
end Domain;
```

`sendable` is checked per instantiation by the §30.14 computation (language spec §14.2's derived-property constraints), diagnosed at the caller's site naming the failing component. The kind constraints (`class`/`notnull`) select each row's ownership shape, which sendability alone does not determine, and the plain rows carry one requirement their `where` clauses do not spell: **a plain `arg: A` row and a plain `R` result row admit no class type other than `string` and `Type`.** At those positions a `notnull` argument is a sendable value type, `string` or `Type` — spec §30.5's non-class category — and an instantiation at any other class type, explicit, inferred, or reached through a user generic's substitution (`class Fan<T> where T: sendable, notnull` forwarding `x: T` to `Domain.Spawn<T>(Use, x)` is rejected at `Fan<Counter>` wherever it is named, spec §3.6 rule 2), is a **compile error at the call site** naming the `owned` row as the remedy: a class instance crosses a domain only as an owned transfer (spec §30.14), and the plain row would carry a borrow of an object the origin domain keeps using. A `Shared<T>` handle is a class and takes the `owned` row too — clone, then move the clone. For more than one argument, pass a sendable class aggregating them, an `OwningArray<T>` of sendable elements, or a `ChannelSender<T>` ([§6.9](#sec-channel)) and send the rest.

**Async entries.** The nine async rows — elided from the listing above, each the twin of a synchronous row with a `Task`-family entry return, on the terms the marker comment states — mirror the nine synchronous rows. The entry is a capture-free `async` lambda or a static `async` method reference; the runtime runs it as the spawned domain's first continuation and awaits the returned task **within that domain** (the `async Main` analog, spec §33.2). The join handle completes when that task settles; a fault anywhere in the entry's async chain surfaces at the join as `DomainFaultedException`. The entry's task object never crosses the boundary; for the `OwningTask<R>` rows the runtime is the single Pattern B consumer, and only the sendable result crosses. Overload resolution prefers the async rows for any entry whose return type is in the `Task` family; the synchronous generic rows never bind `R` to a task type (spec §30.14).

The join handle completes when the entry's task completes. A fault in the entry surfaces at the join as `DomainFaultedException`:

```bezel
public class DomainFaultedException: Exception
  public readonly property FaultStackTrace: string;   // stack trace text of the original exception
  // Message carries the original exception's message text.
end DomainFaultedException;
```

The original exception object never crosses the boundary (no exception class is sendable — [§1](#sec-sendability-statements)); `Message` and `FaultStackTrace` are copied strings. `DomainFaultedException` requires `import System.Threading`, like most of this module (the exceptions — `CancellationToken` and the six §6.5 event types — are enumerated in spec §2.3).

A `CancellationToken` never crosses a domain boundary itself: it is a borrow-bearing record and not sendable. The cross-domain currency is the **source**, behind a handle — `Shared<CancellationTokenSource>` is sendable ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)), so a spawning domain clones a handle, sends the clone, and the spawned domain reads its own token through it and polls at its safe points. A source that crosses this way is **signal-only**: it carries no registrations, and `Register` on it raises ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)) — polling is the cross-domain idiom. Where no source is shared, the pattern is a dedicated channel carrying a cancel message, polled by the spawned domain at its safe points.

<a id="sec-channel"></a>
### 6.9 `Channel<T>` and `ChannelSender<T>`

Multiple-producer, single-consumer channel carrying sendable values between domains with ownership transfer at both ends (language spec [§30.15](Bezel_Language_Spec.md#sec-channels)). Both types are compiler-known and internally synchronized; `ChannelSender<T>`, together with `Mutex`, `Semaphore`, the atomics (`Atomic<T>`/`AtomicInteger<T>`), and the data-owning locks (`Locked<T>`/`RwLocked<T>`) — stdlib [§6.2](#sec-mutex-and-lock)–[§6.4](#sec-atomict-and-atomicintegert), [§6.11](#sec-locked-and-rwlocked) — and, by deep immutability rather than machinery, `FrozenArray<T>` behind `Shared` handles or as a `static readonly` singleton ([§4.19](#sec-frozen-array)), are the standard types two domains may reach simultaneously (the sendable, internally-synchronized set; language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)); the `Channel<T>` receiving end is single-consumer by ownership and stays domain-confined. T must be sendable; the instantiation is rejected otherwise (language spec [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)). A **mutable static** `Channel<T>` field is rejected by language spec [§22.12](Bezel_Language_Spec.md#sec-static-members) rule 2, and a `static readonly Channel<T>` draws rule 4's warning: the channel object is the single-consumer *receiving* end and must stay confined to one consuming domain — when producers across many domains need a process-global send endpoint, publish a `ChannelSender<T>` static instead (rule 2's form-(i) set).

**Facts** ([§1](#sec-statics-and-sharing-facts)): `ChannelSender<T>` — **synchronized**, **self-distributing** (senders are minted per producer from the channel, so a spec-§28.13 handle around one would add a second distribution mechanism).

**Facts** ([§1](#sec-statics-and-sharing-facts)): `Channel<T>` — none (a recorded policy, not a property failure: the receiving end is single-consumer by ownership, and a process-global static would readmit the multi-consumer receive this section explicitly defers — the recorded ground of language spec §22.12 rule 2(i); every statics/sharing exclusion above falls out of that policy).

The signature surface dispatches on T's category, as with `OwningBuffer<T>` ([§4.14](#sec-owned-buffers-and-spans)). For class-typed (sendable) T:

```bezel
public class Channel<T> where T: sendable                  // the receiving end; class T form (the constraint states §6.9's element requirement)
  public constructor();                                    // unbounded
  public constructor(capacity: int);                       // BOUNDED (#809): back-pressure;
                                                           //   capacity >= 1, ArgumentException otherwise

  public readonly property Count: int;                     // items currently buffered
  public readonly property IsClosed: bool;                 // true once closed (see below)

  public method NewSender(): owned ChannelSender<T>;       // mint a sender endpoint
  public method Close();                                   // (#1807): receiver-initiated graceful
                                                           //   close — same closed state as last-sender
                                                           //   dispose; buffered items remain receivable;
                                                           //   idempotent (no-op when already closed)

  public method Receive(): OwningTask<T>;                   // await transfers ownership of one item
  public method TryReceive(out owned item: T?): bool;      // non-blocking; false when none available
  public method ReceiveAll(): IOwningAsyncEnumerable<T>;  // drain form — see below
  destructor;    // closes the channel; disposes every undelivered class-typed
                 //   item in reverse insertion order (language spec §30.15)
end Channel;

public class ChannelSender<T> where T: sendable            // class T form
  public method Send(owned item: T);                       // transfers ownership into the channel;
                                                           //   never blocks; on a FULL bounded channel
                                                           //   raises ChannelFullException;
                                                           //   ChannelClosedException if closed
  public async method SendAsync(owned item: T): Task;      // (#809): awaits capacity on a
                                                           //   bounded channel (immediate when space
                                                           //   or unbounded); ChannelClosedException
                                                           //   if the channel closes while waiting
  destructor;    // disposing the last sender closes the channel (language spec §30.15)
end ChannelSender;
```

For sendable value-typed T, `string` and `Type`, the ownership modifiers drop out: `Receive(): Task<T>`, `TryReceive(out item: T?): bool`, `ReceiveAll(): IAsyncEnumerable<T>`, `Send(item: T)`, `SendAsync(item: T): Task`. At an inline-array `T`, `TryReceive` is not admissible: its `T?` has no form there (language spec [§15.1](Bezel_Language_Spec.md#sec-inline-arrays)), so it is a conditionally-admitted member (language spec [§14.2](Bezel_Language_Spec.md#sec-conditionally-admitted-members)), and `await Receive()` is the receive.

Semantics (normative statement: language spec [§30.15](Bezel_Language_Spec.md#sec-channels)):

- `Channel<T>` and `ChannelSender<T>` are themselves sendable: each endpoint is an independently owned object and may be moved to another domain (as a spawn argument or through another channel). Typical use keeps the channel in the receiving domain and moves senders out.
- **Closing.** Disposing the last sender closes the channel; buffered items remain receivable, and after the drain `Receive` raises `ChannelClosedException`. **`Close()` (#1807) closes the channel from the receiving end with the same graceful semantics** — buffered items remain receivable, an active `ReceiveAll` completes normally after the tail, subsequent `Send`/`SendAsync` raise `ChannelClosedException` (disposing the offered item first), and the item of a `SendAsync` pending at the instant of close is disposed by the channel and its task faulted with `ChannelClosedException`, exactly as on close-while-waiting. `Close()` is idempotent: on an already-closed channel it is a no-op, and later sender disposals remain no-ops with respect to the closed state. Disposing the `Channel<T>` closes it immediately and disposes every undelivered class-typed item in reverse insertion order; subsequent `Send` raises `ChannelClosedException`. `TryReceive` returns `false` for both empty-open and drained-closed; consult `IsClosed` to distinguish.
- **Ordering.** Per-sender FIFO; cross-sender interleaving is an unspecified consistent total order; each item is received exactly once.
- **`ReceiveAll`.** The non-exceptional drain form: `await for var item in inbox.ReceiveAll() do … end;` yields each item with `Receive`'s exact transfer semantics (ownership transfers for class T), suspends while empty-and-open, and completes the enumeration **normally** on drained-and-closed — `ChannelClosedException` stops being control flow for the *normal* exit. At most one consumption may be in progress: beginning a `ReceiveAll` while another is active or while a `Receive` task is pending, and calling `Receive`/`TryReceive` while one is active, raise `InvalidOperationException` (the channel is single-consumer; this names the obligation). **Frame identity (stated positively):** `ReceiveAll()` mints a **fresh drain enumerable over the channel** — an ordinary caller-owned iterable-family production (calls mint, language spec [§28.3](Bezel_Language_Spec.md#sec-producers)), never the channel re-exposed — so disposing the enumerable ends the enumeration, not the channel; the at-most-one-consumption rule above is the *channel's* single-consumer invariant, runtime-enforced, not a frame-identity fact. The enumeration is single-pass like every externally-backed source (language spec [§29.11](Bezel_Language_Spec.md#sec-multiple-enumeration)). Zero new checking rules — a restatement of `Receive` over the [§4.13](#sec-owning-enumeration-interfaces) async-iteration interfaces (normative statement: language spec [§30.15](Bezel_Language_Spec.md#sec-channels)).
- **Receive abandonment.** Disposing a pending `Receive` task deregisters the waiter **without consuming an item** (normative statement: language spec §30.15); an item being handed off at the instant of disposal is returned to the channel's head. Applies to both the `OwningTask<T>` and `Task<T>` forms of `Receive`.

```bezel
public class ChannelClosedException: Exception
end ChannelClosedException;

public class ChannelFullException: Exception        // sync Send on a full bounded channel
end ChannelFullException;
```

**Bounded channels (#809).** `new Channel<T>(capacity)` creates a channel that buffers at most `capacity` items. `SendAsync` **awaits capacity**: the returned task completes when the item has been accepted (immediately when space is free, and always immediately on an unbounded channel). The synchronous `Send` keeps its never-blocks contract on every channel — on a *full* bounded channel it raises **`ChannelFullException`** (a defined fault, §30.3's no-blocking discipline: a producer that wants to wait says so with `await`). If the channel closes while a `SendAsync` is waiting, the pending item is **disposed by the channel** (exactly like buffered undelivered items) and the task faults with `ChannelClosedException`. Ordering and exactly-once delivery are unchanged. A faulting send — `ChannelFullException`, or `ChannelClosedException` from either send form — disposes the class-typed item before the exception surfaces; a channel never retains an item it did not accept (language spec [§30.15](Bezel_Language_Spec.md#sec-channels)).

Deferred, each with its reason recorded in the v0.174 rationale entry: **capacity-zero rendezvous** (couples producer/consumer scheduling; addable later as `capacity: 0` without breaking), **`TrySend`** (the failure path would need to hand the owned item *back* — an ownership-protocol design of its own), **cancellation of a pending send** (same give-back protocol, plus it rides #1413's foundations), **cancellable and multi-source receive** (`Receive(token)`, select/`ReceiveAny` — the dispose-deregistration rule defines *abandonment*; a first-class primitive needs the same hand-the-owned-item-back protocol and is deferred with the send-side family), and **multi-consumer receiver cloning** (drags `Shared<T>`'s §28.13 domain-locality questions; worker pools compose today from N single-consumer channels). The give-back-protocol family (TrySend, send cancellation, cancellable/multi-source receive) is consolidated as one design in proposal 0060.

**The N-channel worker pool, worked.** The deferral list's "worker pools compose today from N single-consumer channels", assembled from documented members only — one bounded inbox per worker (back-pressure), caller-side round-robin, one results-sender minted per worker, shutdown by disposing the job-senders:

```bezel
public sealed class Job                          // sendable: sealed, value fields (spec §30.14)
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public record JobResult(Id: int, Ok: bool);      // sendable value record

public sealed class WorkerCtx                    // Domain.Spawn takes ONE argument (§6.8) → aggregate
  public owned Inbox: Channel<Job>;
  public owned Results: ChannelSender<JobResult>;
  public constructor(owned inbox: Channel<Job>, owned results: ChannelSender<JobResult>)
    Inbox := inbox; Results := results;
  end constructor;
end WorkerCtx;

class Pool
  private static async method WorkerMain(owned ctx: WorkerCtx): Task
    using var c := move ctx;                               // adopt (transfer marker)
    await for var job in c.Inbox.ReceiveAll() do           // drain form: completes normally on close
      using var j := move job;                             // owned iteration variable discharged
      c.Results.Send(new JobResult(j.Id, true));           // value-form Send (record payload)
    end;
  end WorkerMain;                                          // ctx disposal releases the results-sender

  public static async method Main(): Task
    var workers := 4;
    using var results := new Channel<JobResult>();         // boss = the single consumer
    using var senders := new OwningArray<ChannelSender<Job>>();
    using var joins   := new OwningArray<Task>();
    for var i in 0..<workers do
      var inbox := new Channel<Job>(capacity := 8);        // bounded: back-pressure
      senders.Add(inbox.NewSender());
      joins.Add(Domain.Spawn(WorkerMain,
                new WorkerCtx(move inbox, results.NewSender())));   // async-entry row (§6.8)
    end;
    for var n in 0..<100 do
      await senders[n mod workers].SendAsync(new Job(n));  // awaits capacity
    end;
    senders.Clear();                                       // dispose all job-senders → every inbox closes
    var done := 0;
    await for var r in results.ReceiveAll() do done += 1; end;
    for var t in joins.Drain() do
      using var jt := move t;
      await jt;                                            // join; faults surface as DomainFaultedException
    end;
    Console.WriteLine($"done: {done}");
  end Main;
end Pool;
```

Shutdown traces entirely from documented rules: `senders.Clear()` disposes every job-sender (§4.5) ⇒ each inbox closes (the closing rule above) ⇒ each worker's `ReceiveAll` yields the tail and completes normally ⇒ the worker's `using var c` disposes its context, cascading into its results-sender ⇒ after all N dispose, the results channel closes ⇒ the boss's `ReceiveAll` completes ⇒ the joins are awaited (§6.8).

---

<a id="sec-taskscope"></a>
### 6.10 `TaskScope` (#808)

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

- **Consumption is `await using` only, and that binding is the scope's sole owner** — a `TaskScope` is minted only as the initializer of an `await using var` binding whose static type is `TaskScope` (`new TaskScope()` in that position is the one admitted production), and the binding owns it until scope exit (a `using var` is never moved out of, language spec [§28.10](Bezel_Language_Spec.md#sec-using-var)). Every other owning position is a compile error naming this rule, at the declaration that spells the position or at the expression that would place a `TaskScope` in one, `TaskScope?` reading as `TaskScope` throughout: a plain `var`, `using var` or `const` binding, or dropping the scope undisposed (an unbound `new TaskScope()`, a discard); any position language spec [§28.1](Bezel_Language_Spec.md#sec-where-the-owned-modifier-can-appear) admits `owned` at — a parameter (`out` and lambda parameters included), a return slot, a field, a choice payload, a function-type slot, an owning inline-array slot; an owning-collection element or a `Shared<T>` payload — `TaskScope` is admissible as no type argument at all, explicit or inferred, at any instantiation site (language spec [§14.1](Bezel_Language_Spec.md#sec-generic-declarations)), which also keeps `new T()` under `where T: new()` from minting one; and an owning position of a wider static type reached by a language spec [§5.1](Bezel_Language_Spec.md#sec-reference-conversions) reference conversion (an `owned Object` return, an `owned IAsyncDisposable` parameter, an `await using var` declared at `Object` or at an interface). Borrows are otherwise unrestricted — a plain parameter at `TaskScope`, at `Object` or at an interface, a pattern binding, a child body's self-capture (below); a borrow passed to a generic parameter is the type-argument case above. No borrow reaches a disposal path: no implicit discharge runs on a borrow, and `Dispose()` or a direct `DisposeAsync()` through one is a compile error (language spec [§28.6](Bezel_Language_Spec.md#sec-who-may-dispose)). The ground: a synchronous destructor cannot await the drain, and live children hold scope-bounded borrows (§28.14), so no synchronous escape is sound — and every owning position above discharges on language spec [§28.12](Bezel_Language_Spec.md#sec-asynchronous-disposal)'s implicit synchronous path, which skips the drain.
- **Not live across a `yield` (spec [§30.7](Bezel_Language_Spec.md#sec-asynchronous-programming-async-iterators)):** inside an async iterator, a `TaskScope` may not be held across a `yield return` — consumer abandonment at that suspension would take [§28.12](Bezel_Language_Spec.md#sec-asynchronous-disposal)'s synchronous-fallback path, and `TaskScope` has no destructor by design (no synchronous escape is sound), so children would be neither cancelled nor joined and the quiescence guarantee above would be silently voided. Fan out inside a non-yielding `async` helper called between yields instead. (A degraded synchronous destructor — cancel plus detached drain, the #1422 shape — was considered and is recorded as the fallback only if this static rule proves too restrictive.)
- **`await Join()` is mandatory on the normal path** (compile error where provable, `InvalidOperationException` from `DisposeAsync` otherwise): the join-and-re-raise happens at a source-visible `await`, never at an unmarked `end`. `DisposeAsync` (the exception/early-exit edge) cancels the scope's token, drains remaining children, and **always swallows** their faults — `Join` is the sole re-raise site. The drain may suspend unboundedly (the §28.12 close-edge note); bound it with `CancelAfter`.
- **Fault policy**: first fault wins and cancels siblings via the scope's token (cooperative, §30.8); later faults are swallowed (the recorded §13.5 no-aggregation posture). **"First" is first in real time** — the fault that triggered the cancellation — and never spawn order, which fixes nothing about a set of concurrently-running children. Where two children fault concurrently, **which one re-raises is unspecified** and an implementation may choose either. What holds at every arity is the part a program can rely on: `await Join()` re-raises **exactly one** child fault, never an aggregate, and every sibling is cancelled. A test pins that guarantee with a single faulting child; a two-fault selection is a race, and an expectation written over it would be a flake rather than a stronger check.
- **`Spawn` after close** (after `Join` returned or disposal began) raises `InvalidOperationException` — deterministic, never a silent extension of the drain.
- **Quiescence (normative, §28.14)**: `DisposeAsync` completes only after every child task has completed and been reclaimed; no `scoped` callback runs after it returns.
- **Self-capture**: a child body may capture the scope itself and values borrow-derived from it — `scope.Token` inline, or the bound intermediate `var tok := scope.Token;` — admitted by §28.14's self-capture rule. This is how children observe cooperative cancellation and how re-registration (`scope.Spawn` from inside a child) is expressed.
- Intra-domain: `TaskScope` is not sendable; cross-domain fan-out remains channels + `Domain.Spawn`.

<a id="sec-locked-and-rwlocked"></a>
### 6.11 `Locked<T>` and `RwLocked<T>`

> ⚠ **Spec-ahead-of-implementation ([#1721](https://github.com/s8lap/bezel-lang/issues/1721)):** `Locked<T>`, `RwLocked<T>` and their guards are not implemented: no member has a sema or codegen arm. The surface below is the normative contract the implementation will meet.

Data-owning locks (language spec [§30.12 "Data-owning locks"](Bezel_Language_Spec.md#sec-data-owning-locks) — the
normative statement of the interior admission, guard borrow-sourcing, the guard-liveness checks, blocking discipline,
the read-guard write-restriction, the lock-disposal posture, visibility, and set memberships; design rationale
[§12.8](Bezel_Design_Rationale.md#sec-locked-rwlocked)). The lock **owns its
interior**: construction consumes an owned `T`; access is only through a scoped guard whose `Value` is a borrow
sourced from the guard, so the `using var` scope *is* the critical section and the borrow-escape rules enforce the
lock discipline — including against site-announced kills (a guard binding cannot be explicitly `Dispose()`d or discarded
while a borrow from its `Value` is live, and a body-local lock binding cannot be killed while one of its guards is
live; language spec §30.12/§28.3); the destructor disposes the interior. `T` must be a **sendable class type**
(checked at instantiation): owning collections the §4 statement makes sendable (`OwningSet<T>` at a sendable `T`, `OwningDictionary<K, V>` at a sendable `V` and a sendable-value, `string` or `Type` key, …) and classes
declared with wholly-owned state (language spec §30.14's transfer-closed rules) qualify, and so do **borrowing collections at sendable-value, `string` or `Type` element
categories** — `Locked<Dictionary<string, int>>` is admitted (the §4 sendability statement); borrowing
collections of **class** elements do not (`Locked<Dictionary<string, Session>>` is rejected — own the values:
`Locked<OwningDictionary<string, Session>>`).

**Facts** ([§1](#sec-statics-and-sharing-facts)): `Locked<T>`, `RwLocked<T>` — **synchronized**, **displacement-unbounded** (a stranded overwrite-displaced value is the lock plus its entire interior — a payload-scaled cost the spec-§22.12 `Task?` cache's bounded acceptance does not cover; and no use case demands static reassignment of a lock, whose interior mutability lives behind its guards. Provenance: the v0.186 ruling, rationale [§12.8](Bezel_Design_Rationale.md#sec-locked-rwlocked)). The guard types declare no facts (a held guard is domain-bound).

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

public class LockGuard<T> where T: class
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

public class ReadGuard<T> where T: class
  public readonly property Value: T;           // borrow; WRITE-RESTRICTED (language spec §30.12)
  destructor;                                  // releases the shared hold
end ReadGuard;

public class WriteGuard<T> where T: class
  public readonly property Value: T;           // borrow; full access
  destructor;                                  // releases the exclusive hold
end WriteGuard;
```

The canonical process-global registry — the `static owned readonly` idiom (rule-4 warning suppressed by
[§22.12](Bezel_Language_Spec.md#sec-static-members) rule 4's warning-suppression set, the ruling above; `owned` enrolls program-exit
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

**Taking data out.** A guard borrow cannot leave the `using var` scope (by design — the scope *is* the critical section), so a read that must escape uses one of: **(a) copy value data out** — return a record/`string`/primitive built from the interior (a `TryGetValue`d record escapes as a copy); **(b) compute under the guard** — do the work in the scope and let only value-shaped results leave; **(c) `PopValue`/`Pop`** — transfer an entry's ownership out, removing it. A registry that must hand out live *references* to class-typed entries needs the entry type itself to be synchronized or deeply immutable (`Shared<T>` payloads are spec §28.13's derived set — the [§1](#sec-statics-and-sharing-facts) facts; frozen class trees are the recorded §4.19 deferral) — a recorded gap, not an idiom this section can supply.

Semantics and obligations (normative statement: language spec §30.12):

- **Acquisition mirrors `Mutex` ([§6.2](#sec-mutex-and-lock))**: blocking forms are rejected lexically inside `async`
  bodies and hit the runtime `InvalidOperationException` guard where a domain continuation would actually park; the
  `Async` forms are Pattern B (`using var g := await x.LockAsync();`). `TryLock(timeout)`/`TryRead`/`TryWrite` return
  `null` on timeout (Try shape (b), [§1](#sec-conventions)); cancellation follows §6.2's contract: a
  cancelled pending acquisition faults with `OperationCanceledException` and never holds the lock. **Non-reentrant**: a holder re-acquiring waits on itself
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
### 6.12 `Parallel`

Compiler-known data-parallel combinators over a lazy, process-wide team of worker domains (normative checking and
runtime semantics: language spec [§30.14 "Data-parallel combinators"](Bezel_Language_Spec.md#sec-data-parallel-combinators)).

> ⚠ **Spec-ahead-of-implementation ([#3557](https://github.com/s8lap/bezel-lang/issues/3557)):** this entire class is unimplemented — no sema arm, no codegen arm, no runtime team. The surface below is the normative contract the implementation will meet.

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
      where A: sendable, notnull where R: sendable, notnull;   // Pattern A result: the task owns the
                                                 //   array, awaiters borrow (the §6.7 WhenAll<T> shape)

  public static method Reduce<A, R>(items: Array<A>, seed: R,
      selector: (A) -> R,                        // spawn position
      combiner: (R, R) -> R,                     // spawn position; MUST be associative
      degree: int := 0): owned Task<R>
      where A: sendable, notnull where R: sendable, notnull;

  public static method ForEach<A>(items: Array<A>, body: (A) -> (),
      degree: int := 0): owned Task
      where A: sendable, notnull;

  // ---- class elements: ownership partitions in, results partition back ----
  public static method Map<A, R>(owned items: OwningArray<A>, body: (owned A) -> owned R,
      degree: int := 0): owned OwningTask<OwningArray<R>>
      where A: sendable, class where R: sendable, class;

  public static method ForEach<A>(owned items: OwningArray<A>, body: (owned A) -> (),
      degree: int := 0): owned Task
      where A: sendable, class;

  // ---- frozen elements (§4.19): zero-copy — one Shared handle cloned per worker ----
  public static method Map<A, R>(items: Shared<FrozenArray<A>>, body: (A) -> R,
      degree: int := 0): owned Task<Array<R>>
      where A: sendable, notnull where R: sendable, notnull;
  public static method Reduce<A, R>(items: Shared<FrozenArray<A>>, seed: R,
      selector: (A) -> R, combiner: (R, R) -> R,
      degree: int := 0): owned Task<R>
      where A: sendable, notnull where R: sendable, notnull;
  public static method ForEach<A>(items: Shared<FrozenArray<A>>, body: (A) -> (),
      degree: int := 0): owned Task
      where A: sendable, notnull;
end Parallel;
```

As with `Domain.Spawn` (§6.8), `sendable` is checked per instantiation by the §30.14 computation (language
spec §14.2's derived-property constraints), diagnosed at the call site naming the failing rule, and the kind
constraints (`class`/`notnull`) select each row's ownership shape. §6.8's instantiation-site rule applies
unchanged: the value-element rows (`items: Array<A>`, `items: Shared<FrozenArray<A>>`, and every plain `R`)
admit no class type other than `string` and `Type` at `A` or `R` — a class element or result crosses only through the owned rows — and the
rejection is re-read per instantiation through a user generic's substitution (spec §3.6 rule 2). (Return types spell the
`owned` marker out, as `Domain.Spawn` does — emphasis at the domain boundary; the same contract as the
unmarked async forms, §6.7.)

- **Snapshot-at-call** (normative, spec §30.14): borrowed `Array<A>` inputs are chunk-copied synchronously by the
  call itself; owned inputs are consumed and partitioned; `Shared<FrozenArray<A>>` inputs clone one handle per
  participating worker (the borrow of the caller's handle is call-scoped; the caller keeps its handle) and copy no
  element.
- **Results in input order**; first fault wins, surfaced at the `await` as `DomainFaultedException` (implementations
  are encouraged to name the failing element index in `Message`); later faults swallowed; moved-in unprocessed items
  disposed by the runtime.
- **`Reduce` seed semantics (stated plainly).** Each worker folds its chunk starting from `seed`; per-worker
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
  public method Read(buffer: Span<byte>): int;             // span carries its own range
  public method Write(buffer: ReadOnlySpan<byte>);          //   (slice first: §1, Window parameters)
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

**Windowed-form contract (#1375):** in `Read`/`Write(buffer, offset, count)` the window `[offset, offset+count)` ranges over the buffer's **`Count`** (like every other bounds rule in this library); out-of-range raises `IndexOutOfRangeException`. `Read` writes into *existing* elements and never grows the array — the canonical read loop allocates its buffer with the sized constructor (`new Array<byte>(64 * 1024, 0)`, §4.4) and passes `count := buffer.Count`. The **synchronous** span overloads carry their range in the value and are the preferred forms for synchronous I/O; the asynchronous path uses the `Array<byte>` windowed form exclusively (the async span overloads were removed — see the changelog below, #1438).

**Read/write contracts.** `Read` returns the number of bytes read into the window: between `1` and `count` when data remains — it blocks until at least one byte is available, and **may return fewer than `count`** (a short read is not end-of-stream) — and `0` **iff** the stream is at end (a zero-length window returns `0` immediately, the one carve-out). The canonical read loop passes the full window and treats `0` as termination. `ReadAsync` has the same contract with the wait asynchronous; the span overload has it over its own `Length`. `Write`/`WriteAsync` write the entire window or raise. `Stream` itself never raises `EndOfStreamException` — that type ([§7.5](#sec-io-exception-types)) is reserved for exact-read consumers.

**Override points.** `Read`/`Write(buffer, offset, count)`, `Flush`, `Seek`, `Length`, `Position`, and the `CanRead`/`CanWrite`/`CanSeek` properties are `abstract` — every concrete stream supplies them. `ReadAsync`/`WriteAsync` are **`virtual`** with a **base default that delegates to the synchronous `Read`/`Write`**; the default **blocks the calling domain for the I/O's duration** — legal only where blocking is legal, and a [§30.12](Bezel_Language_Spec.md#sec-thread-synchronization) pool-starvation hazard (a blocked continuation pins one pool worker) with **no compiler/runtime backstop for I/O**. A stream performing real I/O (`FileStream`, [§7.2](#sec-filestream)) **overrides** them with genuinely asynchronous implementations; a memory-backed stream whose synchronous ops never block correctly inherits the default. The span overloads `Read(Span<byte>)`/`Write(ReadOnlySpan<byte>)` and the `ReadAll`/`ReadAllAsync` conveniences are **fixed base implementations** over the windowed/async forms — not override points; a subclass that overrides the windowed/async forms gets the correct behavior through them.

User code may derive from `Stream` ([§1](#sec-conventions), *Extensibility*); a subclass is not sendable (language spec §30.14 rule 2's compiler-known-base clause). `Stream` is abstract and declares no `Dispose` behavior of its own; each concrete subclass defines its disposal (e.g. `FileStream` flushes and closes, [§7.2](#sec-filestream)). The `destructor;`-absence convention of [§1](#sec-conventions) does **not** imply that a `Stream` subclass's disposal is a no-op.

```bezel
public enum SeekOrigin: byte
  Begin, Current, End;
end SeekOrigin;
```

> **No async span overloads (#1438).** Spans are borrow-bearing records and barred as `async` parameters (spec §30.1's borrow-bearing-parameter prohibition — the state-machine frame would hold the buffer's borrow past the caller's obligation). Asynchronous byte I/O uses the heap-owned windowed form — `ReadAsync`/`WriteAsync(buffer: Array<byte>, offset, count, token)` — or `ReadAllAsync(): OwningTask<Array<byte>>`; the **synchronous** span overloads are unaffected (spec §8.6). Callers holding a span window over an `Array<byte>` pass the backing array with the window's `Start`/`Length` as `offset`/`count`; windows over an `OwningBuffer<byte>` copy into an `Array<byte>` for the async call, or use the synchronous overload. Full record: the v0.179 changelog.

<a id="sec-filestream"></a>
### 7.2 `FileStream`

```bezel
public class FileStream: Stream
  public constructor(path: string, mode: FileMode, access: FileAccess := FileAccess.ReadWrite);
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

**Constructor contract.** `access` defaults to **`FileAccess.ReadWrite`** (spec §22.5): `new FileStream(path, mode)` is `new FileStream(path, mode, FileAccess.ReadWrite)`. A construction that cannot open the file raises the [§7.5](#sec-io-exception-types) types: `FileNotFoundException` when `FileMode.Open` names a file that does not exist, `DirectoryNotFoundException` when a directory component of `path` does not exist, and `IOException` otherwise (an existing file under `FileMode.CreateNew`, a permission failure, and every other OS-level open failure — where the OS reports a too-long path, as [§7.5](#sec-io-exception-types)'s `IOException`-derived `PathTooLongException`).

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
  public static method WriteAllBytes(path: string, contents: ReadOnlySpan<byte>);   // (#1358): serializer-output path, zero-copy
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

**Text contract.** All `File` text methods encode and decode with `Encoding.UTF8` under the §3.2 defined-fault model: ill-formed bytes on read raise `FormatException`; no byte-order mark is written, and a leading BOM is not specially consumed (it decodes to U+FEFF, §3.2). `ReadAllLines` splits on `\n`, treating an immediately preceding `\r` as part of the terminator (so `\r\n` and `\n` both delimit); a trailing terminator produces no final empty element. `WriteAllLines` terminates every line, including the last, with `\n`. `ReadAllText`/`WriteAllText` transfer contents verbatim — no terminator normalization. The async surface is deliberately text-only in this revision: async byte/line conveniences (`ReadAllBytesAsync`, `ReadAllLinesAsync`, …) are deferred with the proposal-0006 stream family — use `FileStream.ReadAllAsync` (§7.1–§7.2) meanwhile (recorded deferral).

<a id="sec-textreader-textwriter-directory-path"></a>
### 7.4 `TextReader`, `TextWriter`, `Directory`, `Path`

```bezel
public abstract class TextReader
  public abstract method Read(): int;             // next char as int, or -1 at EOF.
                                                  //   Recorded misfit (§1's failure-signal rule):
                                                  //   band 2 would spell this `char?`; the sentinel
                                                  //   stands, moved only by its own graduation
                                                  //   ruling — compare Console.ReadKeyOrNull (§2.1)
  public abstract method ReadLine(): string?;      // null at EOF
  public method ReadToEnd(): string;
  destructor;       // releases the underlying resource
end TextReader;

public abstract class TextWriter
  public abstract method Write(value: string);
  public abstract method Write(value: char);
  public method Write(value: Object);            // fixed base impl: Write(value.ToString()) (#1355)
  public method WriteLine();
  public method WriteLine(value: string);
  public method WriteLine(value: Object);
  // (#1355): per-type Write/WriteLine overloads for bool and the numeric
  // primitives — the §2.1/§3.1 family; behavior = Write(value.ToString()); fixed
  // base implementations over the abstract Write(string).
  public abstract method Flush();
  destructor;       // flushes and releases the underlying resource
end TextWriter;
```

`TextWriter`'s `WriteLine` members write the argument followed by `Environment.NewLine` ([§2.4](#sec-environment)), the no-argument form writing the terminator alone, and its `Write` members write no terminator — [§2.1](#sec-console)'s rule, of which `Console.Out` and `Console.Error` are instances; a `null` argument has no applicable `Write`/`WriteLine` overload, as there.

`TextReader`'s reads and `TextWriter`'s writes are **synchronous** and may block the calling domain for the I/O's duration — the §30.12 pool-starvation hazard, with no compiler/runtime backstop for I/O (the §7.1 classification, applied to the text surface). The types offer no async counterparts; asynchronous text I/O is deferred to the concrete stream family (proposal 0006). User code may derive from `TextReader` and `TextWriter` ([§1](#sec-conventions), *Extensibility*); a subclass is not sendable (language spec §30.14 rule 2's compiler-known-base clause). `TextReader` has no concrete subclass in this library: its one instance is `Console.In` ([§2.1](#sec-console)), and its members, `ReadToEnd` among them, are the base contract the concrete readers of accepted proposal 0006 ([#1433](https://github.com/s8lap/bezel-lang/issues/1433)) implement.

`Directory` and `Path` (#1356):

```bezel
public static class Directory
  public static method Exists(path: string): bool;
  public static method CreateDirectory(path: string);
  public static method Delete(path: string, recursive: bool := false);   // IOException if non-empty and not recursive
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

`IOException`, `FileNotFoundException`, `DirectoryNotFoundException`, `EndOfStreamException`, `PathTooLongException`. All derive from `IOException`, which derives from `Exception`. None are auto-imported; users `import System.IO;` to get them. `EndOfStreamException`'s documented producers are the exact-read consumers — `Console.ReadKey` (§2.1) and the deferred proposal-0006 reader family; `Stream.Read` signals end-of-stream with a `0` return, never with this exception.

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
  public method Reset();
end Stopwatch;
```

`Reset()` stops the stopwatch and zeroes `Elapsed`; a restart from zero is `sw.Reset(); sw.Start();`, and `StartNew()` is the fresh-instance form.

**Sendability** ([§1](#sec-sendability-statements); language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)). `Stopwatch` is **sendable**, on the sendable form's copy-state ground: its state is a running flag and elapsed-time counters, read and written by value, and no member retains a borrow or exposes an address. A moved running stopwatch keeps measuring: elapsed time is read from the clock the replay-intercept rule names ([§1](#sec-conventions)), not from the thread or domain that started it (language spec §30.13 already runs a domain's continuations on any thread).

<a id="sec-debug-and-trace"></a>
<a id="sec-debug"></a>
### 8.2 `Debug`

Conditional logging facilities.

```bezel
public static class Debug
  public static method Assert(condition: bool, message: string := "");
  public static method WriteLine(message: string);
end Debug;
```

Calls to `Debug.*` may be elided in release builds, depending on the compiler's `--debug` flag. Side effects in `Debug.Assert` arguments are unspecified. A failed assertion — `condition` false where the call is not elided — reports the condition's source location and the message (an empty `message` is reported as no message) through the diagnostic channel and **terminates the process**: assertion failure is a programming-error report, not a recoverable fault, so `Assert` never throws.

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

  public static pure method Parse(s: string): int;
  public static method TryParse(s: string, out result: int): bool;
  public static pure method ParseOrNull(s: string): int?;             // (§1 *OrNull tier): null exactly
                                                                      //   where Parse(s) raises (the
                                                                      //   FormatException/OverflowException
                                                                      //   contract); otherwise the parsed
                                                                      //   value. Same strict input grammar as
                                                                      //   Parse — no whitespace tolerance
                                                                      //   string form only
  public static pure method Parse(s: string, radix: int): int;        // radix in {2, 8, 10, 16}; any other
                                                                      //   radix raises ArgumentException — a
                                                                      //   caller bug, not input failure (pinned)
  public static method TryParse(s: string, radix: int, out result: int): bool;
                                                                      // shape (c): false (out = 0)
                                                                      //   exactly where Parse(s, radix) raises
                                                                      //   FormatException/OverflowException;
                                                                      //   an invalid radix raises
                                                                      //   ArgumentException from both forms
  public static pure method Parse(s: ReadOnlyStringSpan): int;                     // (#1810): identical contract
  public static method TryParse(s: ReadOnlyStringSpan, out result: int): bool;     //   to the string forms; shape (c)

  public pure method ToString(): string;
  public pure method ToString(format: string): string;
  public pure method ToString(radix: int): string;

  public pure method CompareTo(other: int): int;
  // Equals(other: int): bool — auto-generated from IEquatable<int>
  //   per language spec §9.2; not shown explicitly.
end Int32;
```

Records implement `IEquatable<T>` implicitly through the auto-generated equality (language spec [§9.2](Bezel_Language_Spec.md#sec-body-records)): `Equals(other: int): bool` exists on `Int32` and is invoked when calling `someInt.Equals(otherInt)` directly or when an `IEquatable<int>`-constrained generic uses `Equals` through the constraint. The same convention applies to every primitive declaration in [§9.1](#sec-integer-types)–[§9.5](#sec-char).

**The radix alphabet.** The digits of radix *r* are the first *r* of `0`–`9` then `A`–`Z`; for the admitted radices that is `0`/`1`, `0`–`7`, `0`–`9`, and `0`–`9` with `A`–`F`. `Parse(s, radix)` and `TryParse(s, radix, …)` accept **either case** of the letter digits — `int.Parse("ff", 16)` and `int.Parse("FF", 16)` are both `255` — under the input grammar the string forms carry throughout: an optional leading `+` or `-`, then one or more digits of that radix, and nothing else; no whitespace, no `0x`/`0b` prefix, no digit separators, and any other input raises `FormatException`, a value outside the type's range raising `OverflowException`. `ToString(radix)` emits the **lower-case** digits — a `-` for a negative value followed by the magnitude's digits most significant first, a single `0` for zero, never a leading zero otherwise — and takes the same radix set, raising `ArgumentException` for any other. The two are therefore inverse: `T.Parse(n.ToString(radix), radix)` is `n` for every value of every integer type and every admitted radix. `Convert.ToString(value, toBase)` ([§2.3](#sec-convert)) emits the same text as `ToString(radix)`. The no-format `ToString()`, which an interpolation hole without a format spec uses (language spec [§18.5](Bezel_Language_Spec.md#sec-interpolated-strings)), emits the text `ToString(10)` emits: `(-42).ToString()` is `"-42"`.

The span overloads (#1810) accept exactly the inputs the string forms accept — same `FormatException`/`OverflowException` contract, no whitespace tolerance — over a `ReadOnlyStringSpan` window without materializing a substring; `TryParse` is shape (c) of the §1 Try convention. `IParsable<T>` (§2.19) is unchanged — the span forms are per-type members, not constraint surface. The `ParseOrNull` members are the §1 `*OrNull` convenience tier's parse form — string input only, no radix or span duals — and share the span forms' per-type, non-constraint status.

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

  public static pure method IsNaN(value: double): bool;
  public static pure method IsInfinity(value: double): bool;
  public static pure method IsPositiveInfinity(value: double): bool;
  public static pure method IsNegativeInfinity(value: double): bool;
  public static pure method IsFinite(value: double): bool;

  public static pure method Parse(s: string): double;
  public static method TryParse(s: string, out result: double): bool;
  public static pure method ParseOrNull(s: string): double?;          // null exactly where Parse(s)
                                                                      //   raises; otherwise the parsed value
  public static pure method Parse(s: ReadOnlyStringSpan): double;                  // (#1810): span form
  public static method TryParse(s: ReadOnlyStringSpan, out result: double): bool;

  // Equality variants
  public static pure method IeeeEquals(a: double, b: double): bool;       // strict IEEE
  public static pure method BitwiseEquals(a: double, b: double): bool;    // bit-pattern equal

  public pure method ToString(): string;
  public pure method ToString(format: string): string;
  public pure method CompareTo(other: double): int;
end Double;
```

**The classification predicates.** Each `Is*` member reports the operand's IEEE 754 class and is **total** — every value gets an answer, and none of them raises. `IsNaN(v)` is `true` exactly when `v` is a NaN, of either sign and any payload, signaling or quiet; `IsPositiveInfinity(v)` and `IsNegativeInfinity(v)` exactly at `+∞` and `−∞`; `IsInfinity(v)` at either; and `IsFinite(v)` exactly when `v` is neither a NaN nor an infinity — every finite value, subnormals and both zeros included. The five are classifications of the operand alone and never consult the `=` operator.

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

  public static pure method IsNaN(value: float): bool;
  public static pure method IsInfinity(value: float): bool;
  public static pure method IsPositiveInfinity(value: float): bool;
  public static pure method IsNegativeInfinity(value: float): bool;
  public static pure method IsFinite(value: float): bool;

  public static pure method Parse(s: string): float;
  public static method TryParse(s: string, out result: float): bool;
  public static pure method ParseOrNull(s: string): float?;           // null exactly where Parse(s)
                                                                      //   raises; otherwise the parsed value
  public static pure method Parse(s: ReadOnlyStringSpan): float;                   // (#1810): span form
  public static method TryParse(s: ReadOnlyStringSpan, out result: float): bool;

  // Equality variants
  public static pure method IeeeEquals(a: float, b: float): bool;       // strict IEEE
  public static pure method BitwiseEquals(a: float, b: float): bool;    // bit-pattern equal

  public pure method ToString(): string;
  public pure method ToString(format: string): string;
  public pure method CompareTo(other: float): int;
end Float;
```

`Float`'s `MinValue`, `MaxValue`, `Epsilon`, `PositiveInfinity`, `NegativeInfinity`, and `NaN` are the IEEE 754 binary32 standard values. The same `IeeeEquals`/`BitwiseEquals` distinction described for `Double` applies to `Float` with the appropriate bit-pattern width. `Epsilon` is the **smallest positive subnormal** value (the .NET meaning: ≈4.94e-324 for `double`, ≈1.40e-45 for `float`) — *not* the machine epsilon of numerical analysis (2⁻⁵² ≈ 2.22e-16 for `double`); tolerance comparisons use an explicit, domain-chosen tolerance, never `Epsilon`.

**Input grammar.** `Parse` accepts an optional sign (`+` or `-`), then one or more of the digits `0`–`9`, then optionally a `.` followed by one or more digits, then optionally an exponent — `e` or `E`, an optional sign, and one or more digits — and nothing else: `"5"`, `"+5"`, `"-0.25"`, `"007.5"` and `"6.02E+23"` are admitted. The shape is a `double` literal's (language spec [Appendix A](Bezel_Language_Spec.md#sec-lexical)'s `FloatLiteral`) with a sign admitted before it and an integer spelling admitted, and without the literal's digit separators and suffix. There is no surrounding whitespace, no digit separator, no hexadecimal form and no bare point, so `" 5"`, `"5 "`, `"1_000"`, `"0x1p3"`, `".5"` and `"5."` raise `FormatException`, as does any other input apart from exactly `"NaN"`, `"Infinity"` and `"-Infinity"`. Those three stand outside the grammar: they are the renderings of the non-finite values ([§2.18](#sec-iformattable)), and `Parse` reads them back as `NaN`, `PositiveInfinity` and `NegativeInfinity`, as §2.18's round trip requires. They are matched exactly, so `"nan"`, `"inf"`, `"infinity"`, `"INFINITY"`, `"+Infinity"` and `"-NaN"` raise `FormatException`. As for the integer types ([§9.1](#sec-integer-types)) and `decimal` ([§9.3](#sec-decimal)), a caller that means to accept padded or grouped input trims or strips it explicitly (the strict-fault model, [§3.2](#sec-encoding)). An input the grammar does not admit raises `FormatException` whatever number a part of it spells: `"1_000e999"` raises `FormatException`, not the `OverflowException` below.

**Parsing and range.** `Parse` yields the value its input spells, rounded once to the type as language spec [§4.3](Bezel_Language_Spec.md#sec-floating-point-types) rounds a literal of that type. An input that spells a finite number whose value, so rounded, is not finite — §4.3's out-of-range boundary for a literal of that type — raises `OverflowException`: the type cannot represent the number the input spells, and unrepresentable input is a defined fault, never a silent substitution (the strict-fault model of `Encoding`, [§3.2](#sec-encoding)). So `double.Parse("1.7976931348623158e308")` is `double.MaxValue` and `float.Parse("3.4028235e38")` is `float.MaxValue`, while `double.Parse("1e999")`, `double.Parse("-1e999")` and `float.Parse("3.5e38")` raise. A value that rounds to zero or to a subnormal is in range: `double.Parse("1e-400")` is `0`. `TryParse` returns `false` in every case where `Parse` would raise, and the span forms carry the string forms' contract.

<a id="sec-decimal"></a>
### 9.3 `Decimal`

```bezel
/// Compiler-intrinsic; aliased by the keyword `decimal`.
public record Decimal: IComparable<decimal>, IEquatable<decimal>
  public static const MinValue: decimal;
  public static const MaxValue: decimal;
  public static const Zero: decimal;
  public static const One: decimal;

  public static pure method Parse(s: string): decimal;
  public static method TryParse(s: string, out result: decimal): bool;
  public static pure method ParseOrNull(s: string): decimal?;         // null exactly where Parse(s)
                                                                      //   raises; otherwise the parsed value
  public static pure method Parse(s: ReadOnlyStringSpan): decimal;                 // (#1810): span form
  public static method TryParse(s: ReadOnlyStringSpan, out result: decimal): bool;

  public pure method ToString(): string;
  public pure method ToString(format: string): string;
  public pure method CompareTo(other: decimal): int;
  public pure method Round(digits: int): decimal;    // banker's rounding (round-half-to-even), matching
                                                //   Math.Round (§2.2); digits outside 0..28:
                                                //   ArgumentOutOfRangeException
end Decimal;
```

**Rendering and range.** The no-format `ToString()` — the rendering an interpolation hole with no specifier uses, and what the `G` specifier produces ([§2.18](#sec-iformattable)) — is the value's exact decimal form and nothing more: a `-` when the value is negative, the integer digits (a single `0` when the magnitude is below one), and, when the value has a fractional part, a `.` followed by the fractional digits. There is no exponent, no grouping separator, and no trailing-zero padding, because **scale is not part of a `decimal`'s value**: `1.50m` and `1.5m` are one value and both render `1.5`, `10m` renders `10`, and `0m` renders `0` — which is what makes `=` on `decimal` plain value equality (language spec [§4.3.1](Bezel_Language_Spec.md#sec-equality-semantics-for-floating-point-types)). `MaxValue` is `79228162514264337593543950335` and `MinValue` is `-79228162514264337593543950335` — the range language spec [§4.4](Bezel_Language_Spec.md#sec-decimal)'s 28–29 significant decimal digits describe; `Zero` is `0m` and `One` is `1m`.

`Parse` accepts an optional leading sign (`+` or `-`), then one or more of the digits `0`–`9`, then optionally a `.` followed by one or more digits, and nothing else: [§9.2](#sec-double-and-float)'s input grammar without its exponent, which a `decimal` literal admits (language spec [Appendix A](Bezel_Language_Spec.md#sec-lexical)'s `FloatLiteral`) and the no-format `ToString()` never writes. `"5"`, `"+5"`, `"-0.25"` and `"007.50"` are admitted; `".5"`, `"5."`, `"."`, `"1.2.3"`, `"1e3"` and `"1_000"` raise `FormatException`, and every rendering the no-format `ToString()` produces lies inside the grammar; **surrounding whitespace is not accepted** and raises `FormatException` (the strict-fault model of `Encoding` ([§3.2](#sec-encoding)) applies: silent tolerance hides data problems; a caller that means to accept padded input trims explicitly). An in-range value with more digits than a `decimal` holds is rounded as language spec [§4.4](Bezel_Language_Spec.md#sec-decimal) states (`"0.12345678901234567890123456785"` parses to `0.1234567890123456789012345678`); a value outside `decimal`'s range — one whose rounding does not fit at scale 0, as language spec [§4.4](Bezel_Language_Spec.md#sec-decimal) states, so `"79228162514264337593543950335.4"` parses to `MaxValue` and `"79228162514264337593543950335.5"` raises — raises `OverflowException`. `TryParse` returns `false` in every case where `Parse` would raise.

<a id="sec-boolean"></a>
### 9.4 `Boolean`

```bezel
/// Compiler-intrinsic; aliased by the keyword `bool`.
public record Boolean: IEquatable<bool>
  public static pure method Parse(s: string): bool;            // "true"/"false", case-insensitive
  public static method TryParse(s: string, out result: bool): bool;
  public static pure method ParseOrNull(s: string): bool?;     // (§1 *OrNull tier): null exactly
                                                               //   where Parse(s) raises; otherwise the
                                                               //   parsed value

  public pure method ToString(): string;                       // "True" or "False"
end Boolean;
```

<a id="sec-char"></a>
### 9.5 `Char`

```bezel
/// Compiler-intrinsic; aliased by the keyword `char`.
public record Char: IComparable<char>, IEquatable<char>
  public static const MinValue: char;             // U+0000
  public static const MaxValue: char;             // U+10FFFF (excluding surrogates)

  public static pure method IsDigit(c: char): bool;
  public static pure method IsLetter(c: char): bool;
  public static pure method IsLetterOrDigit(c: char): bool;
  public static pure method IsWhiteSpace(c: char): bool;
  public static pure method IsUpper(c: char): bool;
  public static pure method IsLower(c: char): bool;
  public static pure method IsControl(c: char): bool;
  public static pure method IsPunctuation(c: char): bool;

  public static pure method ToUpper(c: char): char;
  public static pure method ToLower(c: char): char;

  public static pure method Parse(s: string): char;                 // exactly one code point
  public static method TryParse(s: string, out result: char): bool;
  public static pure method ParseOrNull(s: string): char?;          // null exactly where Parse(s)
                                                                    //   raises; otherwise the parsed value

  public pure method ToString(): string;
  public pure method CompareTo(other: char): int;
end Char;
```

**The classification predicates.** Each `Is*` member tests the argument's Unicode character properties; every one is **total** — each `char` gets an answer, and none of them raises. `IsDigit(c)` is `c`'s general category being `Nd` (decimal digit number); `IsLetter(c)` is one of `Lu`, `Ll`, `Lt`, `Lm`, `Lo`; `IsLetterOrDigit(c)` is `IsLetter(c) or IsDigit(c)`; `IsUpper(c)` is `Lu` and `IsLower(c)` is `Ll`; `IsControl(c)` is `Cc`; `IsPunctuation(c)` is one of `Pc`, `Pd`, `Ps`, `Pe`, `Pi`, `Pf`, `Po`; and `IsWhiteSpace(c)` is `c` carrying the Unicode `White_Space` property. `ToUpper`/`ToLower` apply the **simple** case mapping — one code point to one, unconditional, no locale tailoring; a sibling of, not identical to, the simple case *folding* [§9.6](#sec-string)'s `ignoreCase` comparisons apply — and return the argument unchanged where it has none. `IsDigit` is **not** the alphabet the parses read: [§9.1](#sec-integer-types)'s and [§9.3](#sec-decimal)'s `Parse` families accept the ASCII digits `0`–`9` and nothing else.

`Parse` accepts a string containing **exactly one code point** (`s.Length = 1`, [§9.6](#sec-string)'s code-point count) and returns that code point; any other input — the empty string or a multi-code-point string — raises `FormatException`. There is no whitespace tolerance and no recognition of escape sequences: escapes are source-lexical ([language spec §1](Bezel_Language_Spec.md)), so `char.Parse("\u0041")` receives the one-character string `"A"`, while a six-character `\u0041` sequence in runtime data is a six-code-point string and raises. (The strict-fault model of [§3.2](#sec-encoding) and `Decimal.Parse`, [§9.3](#sec-decimal).) `TryParse` returns `false` in every case where `Parse` would raise; the out slot gets `'\0'` — `default(char)`, language spec [§17.11](Bezel_Language_Spec.md#sec-the-default-operator) (the shape-(c) `default(T)`, [§1](#sec-conventions)). These are the members [§2.19](#sec-static-abstractions)'s `IParsable<char>` conformance witnesses. (#1792.)

<a id="sec-string"></a>
### 9.6 `String`

The most extensive primitive type. `String` is written with the `string` keyword and indexed below in the class form; it is a reference type that is not a class (language spec [§14.2](Bezel_Language_Spec.md#sec-constraints)): it implements no `IDisposable` and has no `Dispose()` member — `s.Dispose()` is a compile error (language spec [§8.7](Bezel_Language_Spec.md#sec-dispose-is-not-on-object), [§28.9](Bezel_Language_Spec.md#sec-runtime-managed-types)) — and its lifetime is the runtime's (language spec [§4.6](Bezel_Language_Spec.md#sec-string)). It implements `IEnumerable<char>`, so strings may be iterated with `for` (language spec [§4.6](Bezel_Language_Spec.md#sec-string)).

```bezel
public class String: IEnumerable<char>, IComparable<string>, IEquatable<string>     // alias: string
  public pure readonly property Length: int;            // number of code points (O(1))
  public pure readonly property IsEmpty: bool;          // Length = 0 — §3.3's spelling (O(1))
  public pure readonly property IsAscii: bool;          // true iff every code point < 128 (O(1))
  public pure readonly property [i: int]: char;                  // O(1)

  // Construction
  public static pure method From(value: char): string;
  public static pure method From(buffer: Array<char>, start: int, length: int): string;   // params follow the §1 window convention
  public static method From(value: CString): string;                  // FFI-boundary, owned-instance source
  public static method From(value: WString): string;                  // FFI-boundary, owned-instance source
  public static method FromCStringPointer(p: IntPtr): string;         // FFI-boundary, raw-pointer source
  public static method FromWStringPointer(p: IntPtr): string;         // FFI-boundary, raw-pointer source
  public static pure method Repeat(c: char, count: int): string;      // count < 0 raises
                                                                      //   ArgumentOutOfRangeException; count = 0 is ""
  public static pure method Repeat(unit: string, count: int): string; // same count contract;
                                                                      //   Repeat("", n) = ""
  public static method Join(separator: string, items: IEnumerable<string>): string;

  // Inspection
  public static pure method IsNullOrWhiteSpace(value: string?): bool;
  public pure method Contains(value: string): bool;
  public pure method Contains(value: char): bool;
  public pure method StartsWith(value: string): bool;
  public pure method EndsWith(value: string): bool;
                              // Empty needle: Contains("") = true; StartsWith("") = true;
                              //   EndsWith("") = true — the vacuous-match convention of IndexOf("") = 0;
                              //   the §3.3 span family mirrors the whole set.
  public pure method IndexOf(value: char, startIndex: int := 0): int;     // code-point index of the first occurrence
  public pure method IndexOf(value: string, startIndex: int := 0): int;   //   at or after startIndex, or -1;
                                                                          //   0 <= startIndex <= Length
                                                                          //   (IndexOutOfRangeException otherwise);
                                                                          //   IndexOf("", start) = start, so
                                                                          //   IndexOf("") = 0
  public pure method LastIndexOf(value: char): int;   // last occurrence, or -1
  public pure method LastIndexOf(value: string): int; // -1 when absent; LastIndexOf("") = Length

  // Substring
  public pure method Substring(startIndex: int): string;
  public pure method Substring(startIndex: int, length: int): string;
  public pure readonly property [r: Range<int>]: string;       // slicing form (§4.6 of the language spec)

  // Span views
  public pure method AsSpan(): ReadOnlyStringSpan;             // full string as a span
  public pure method AsSpan(start: int): ReadOnlyStringSpan;   // from offset to end
  public pure method AsSpan(start: int, length: int): ReadOnlyStringSpan;
  public pure method AsSpan(r: Range<int>): ReadOnlyStringSpan; // parallels [r: Range<int>]: string

  // Modification (returns a new string; original unchanged)
  public pure method Trim(): string;
  public pure method Trim(chars: IEnumerable<char>): string;
  public pure method TrimStart(): string;
  public pure method TrimEnd(): string;
  public pure method ToUpper(): string;
  public pure method ToLower(): string;
  public pure method Replace(oldChar: char, newChar: char): string;
  public pure method Replace(oldValue: string, newValue: string): string;
  public pure method PadLeft(totalWidth: int, paddingChar: char := ' '): string;
  public pure method PadRight(totalWidth: int, paddingChar: char := ' '): string;
  public pure method PadCenter(totalWidth: int, paddingChar: char := ' '): string;
                              // Pad contract (all three Pad members): totalWidth <= Length returns the original string
                              // unchanged; totalWidth < 0 raises ArgumentOutOfRangeException.
                              // PadCenter splits the padding evenly; when it is odd, the extra
                              // character goes to the RIGHT: "ab".PadCenter(5) = " ab  ".
                              // PadLeft prepends and PadRight appends paddingChar until the result's
                              // Length is totalWidth: "ab".PadLeft(5, '*') = "***ab",
                              // "ab".PadRight(5, '*') = "ab***" (#7157).

  public pure method Insert(startIndex: int, value: string): string;
  public pure method Remove(startIndex: int): string;
  public pure method Remove(startIndex: int, length: int): string;   // param follows the §1 window convention

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
  public pure method CompareTo(other: string): int;            // ordinal; satisfies IComparable<string>
  public pure method CompareTo(other: string, ignoreCase: bool): int;   // the case-folded form (note above);
                                                                       //   `other` is non-nullable, as the
                                                                       //   conformance member's is
  public pure method Equals(other: string): bool;              // the Object slot's own-typed spelling
                                                          //   (spec §8.3): content-based; satisfies
                                                          //   IEquatable<string>; a `string?` or
                                                          //   `Object?` argument reaches it through
                                                          //   the slot, null and non-strings unequal
  public pure method Equals(other: string?, ignoreCase: bool): bool;

  // Object overrides (Equals is the own-typed member above)
  public pure override method GetHashCode(): int;              // content-based, agrees with Equals
  public pure override method ToString(): string;              // returns this

  // Conversion
  public method ToCharArray(): owned Array<char>;

  // Iteration
  public method GetEnumerator(): owned IEnumerator<char>;

  // Operators
  public static pure method operator +(left: string, right: string): string;
end String;
```

`Replace`, `Trim`, `ToUpper`, etc. return new `string` values; `string` is immutable. A `StringBuilder` ([§3.1](#sec-stringbuilder)) is appropriate for accumulation.

**`IsNullOrWhiteSpace`.** Returns `true` when `value` is `null`, when it is the empty string, and when every one of its code points satisfies `Char.IsWhiteSpace` ([§9.5](#sec-char)); `false` otherwise. The member is total over `string?` — the nullable parameter admits the null and the member answers it, so no narrowing is needed at the call.

**The window members: `Substring`, `Remove` and `Insert`.** Positions and lengths count code points (`Length`). `Substring(startIndex, length)` yields the `length` code points of the receiver starting at position `startIndex`, and `Substring(startIndex)` the rest of the receiver from there — [§1](#sec-conventions)'s *to-the-end* window, `Substring(startIndex, Length - startIndex)`; `Remove` yields the receiver without the same window, and `Insert(startIndex, value)` yields the receiver with `value`'s code points inserted at position `startIndex`. `startIndex` may equal `Length` and `length` may be `0`: `"abc".Substring(3)` and `"abc".Substring(1, 0)` are `""`, `"abc".Remove(3)` is `"abc"`, and `"abc".Insert(3, "d")` is `"abcd"`. A negative `startIndex` or `length`, a `startIndex` above `Length`, or a window reaching past the end (`startIndex + length > Length`) raises `IndexOutOfRangeException` — the bound `IndexOf` states above and [§3.1](#sec-stringbuilder)'s `StringBuilder.Insert`/`Remove` state, and the exception language spec [§4.6](Bezel_Language_Spec.md#sec-string)'s slice `s[r]` raises: `"abc".Substring(1, 2)` is `"bc"`, `"abc".Remove(1, 1)` is `"ac"`, and `"abc".Substring(2, 2)` raises ([#7161](https://github.com/s8lap/bezel-lang/issues/7161)).

**`Split` contract.** Splitting yields every (possibly empty) segment between separator occurrences: adjacent separators yield an empty element between them, and a leading or trailing separator yields a leading or trailing empty element. A separator that never occurs yields the one-element array `[s]`; splitting the empty string yields `[""]`. There is no remove-empty-entries option — tokenizers filter explicitly: `line.Split(seps).Where(lambda(w) => w.Length > 0)` ([§5](#sec-system-collections-linq), auto-imported). The `separators: Array<char>` form splits at any listed code point.

`Length` and `s[i]` are O(1) for every string (language spec [§4.6](Bezel_Language_Spec.md#sec-string)). The internal representation may be 1, 2, or 4 bytes per code point, selected per-string at construction; the choice is invisible to programmers but enables `IsAscii` for the common ASCII-only case. For char-level non-allocating substring views, use `AsSpan()` to obtain a `ReadOnlyStringSpan` ([§3.3](#sec-readonlystringspan)). `AsSpan(start)` and `AsSpan(start, length)` are [§4.14](#sec-owned-buffers-and-spans)'s eager factories, as every `AsSpan` overload in this library is: the window is checked at the call against `Length` — `start` in `0..Length`, `length` non-negative, `start + length` at most `Length` — and a window outside it raises `IndexOutOfRangeException`, as `AsSpan(r)` and the slice `s[r]` do (language spec [§4.6](Bezel_Language_Spec.md#sec-string)); `start = Length` yields the empty span. `From(buffer, start, length)` reads the same window over the array's `Count` ([§1](#sec-conventions), *Window parameters*; the rule of the [§7.1](#sec-stream) `Stream` window) and raises `IndexOutOfRangeException` outside it; `From(buffer, buffer.Count, 0)` is `""`. For byte access to a string's contents, use `Encoding.UTF8.GetBytes` in `System.Text` ([§3.2](#sec-encoding)): the one-shot form `GetBytes(s)` returns a fresh owned `Array<byte>` and is always correct; the allocation-conscious form — repeated encoding in a loop, I/O paths reusing a buffer — is `GetByteCount(s)` with `GetBytes(s, destination: Span<byte>)`, which encodes into a caller-reused `OwningBuffer<byte>` with zero per-call allocation, for every string, ASCII or not. (The ASCII-only zero-copy byte view `ReadOnlyAsciiByteSpan` is retired — [§3.4](#sec-readonlyasciibytespan).)

The `+` operator is declared explicitly here, not as a compiler intrinsic; the compiler resolves `s + t` to this overload like any other operator.

<a id="sec-arrayt-extension-methods"></a>
### 9.7 `Array<T>` extension methods

In addition to the methods declared on `Array<T>` itself ([§4.4](#sec-arrayt)), the LINQ extensions in [§5](#sec-system-collections-linq) apply to `Array<T>` because it implements `IEnumerable<T>` — **except the four whose names `Array<T>` itself declares** (narrowing a claim that had been false as stated): under spec §26.5 rule 1 and [§1](#sec-conventions)'s block-by-name note, `Contains`, `Count`, `Reverse`, and `ToArray` resolve to the instance members and the same-named §5 operators are unreachable on an `Array<T>` receiver — `arr.Count(pred)` does not resolve to LINQ's predicate count; compose `arr.Where(pred).Count()`.

Multi-dimensional arrays are deferred to a future revision; the current standard library provides only one-dimensional `Array<T>` (heap) and `[N]T` (inline, language spec [§15.1](Bezel_Language_Spec.md#sec-inline-arrays)).

<a id="sec-ranget"></a>
### 9.8 `Range<T>`

The type underlying range expressions (language spec [§16](Bezel_Language_Spec.md#sec-range-expressions)). `Range<T>` is a compiler-intrinsic record. `T` is restricted by the compiler to the integer types (`byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`) and `char`. The nine admitted types are compiler-provided `ISteppable<T>` conformers ([§2.19](#sec-static-abstractions)), but the restriction is not a `where T: ISteppable<T>` clause: `float`, `double`, `decimal`, and user conformers conform to `ISteppable<T>` and are not admitted (language spec [§16.3](Bezel_Language_Spec.md#sec-the-ranget-type)).

```bezel
/// Compiler-intrinsic; T restricted to integer types and char.
public record Range<T>: IEnumerable<T>
  public readonly property Start: T?;              // null if open-ended on the lower side
  public readonly property End: T?;                // null if open-ended on the upper side
  public readonly property Step: long;             // default 1; signed to support descending
  public readonly property IsEndInclusive: bool;   // true for `..`, false for `..<`
  public pure readonly property IsOpenEnded: bool; // true if Start or End is null
  public pure readonly property IsEmpty: bool;     // true if the range contains no values

  public pure method Contains(value: T): bool;
  public method GetEnumerator(): owned IEnumerator<T>;
end Range;
```

All six declared properties are the type's auto-property surface for the language's generated members (spec [§8.5](Bezel_Language_Spec.md#sec-tostring)/[§9.2](Bezel_Language_Spec.md#sec-body-records)): the generated `ToString` renders all six — `IsOpenEnded` and `IsEmpty` recomputed exactly as their member reads compute (spec [§16.3](Bezel_Language_Spec.md#sec-range-operations)) — whatever the intrinsic's storage layout. That surface decides the *generated members*, not purity: `Start`, `End`, `Step` and `IsEndInclusive` read stored slots, so their getters are pure by classification (spec [§22.15](Bezel_Language_Spec.md#sec-pure-classification)) and the marker on them would be redundant, while `IsOpenEnded` and `IsEmpty` are recomputed and therefore carry `pure` as the stated contract of a runtime-provided body — which `Contains` needs, its normative formula reading `IsEmpty` ([§16.3](Bezel_Language_Spec.md#sec-range-operations)).

`Contains(v)` is `true` iff the range is nonempty under iteration and `v` lies within the endpoint extent (exclusive upper bound excluded) — an empty range contains nothing, and `Contains` agrees with range patterns on the stepless forms (language spec [§16.3](Bezel_Language_Spec.md#sec-range-operations)). `Step` affects membership only through direction/emptiness. This is the operation invoked by the `in` operator (spec [§17.5](Bezel_Language_Spec.md#sec-the-in-operator)). `Contains` is **`pure`** — the intrinsic-purity contract of spec [§22.15](Bezel_Language_Spec.md#sec-pure-members), normative for the implementation: the result is a function of the receiver's components and the argument, evaluated by the membership formula above, and the runtime-provided body writes no storage outside its own frame, allocates nothing, and raises nothing. The marker is what admits a range test inside a contract clause: `requires(port in 0..65535)` is a [§22.16](Bezel_Language_Spec.md#sec-contract-clauses) clause expression, whose `in` call §22.15's calls-to-pure-members rule permits. `GetEnumerator` is not `pure` and cannot be: it produces an owned class instance, and §22.15's pure body prohibits calls returning `owned` class instances — whatever the return's declared interface spelling.

`GetEnumerator` produces an enumerator that yields values from `Start` to `End`, incrementing by `Step`. Calling `GetEnumerator` on an open-ended range (`IsOpenEnded` is `true`) raises `InvalidOperationException`. Because `Range<T>` implements `IEnumerable<T>`, ranges compose with LINQ and other IEnumerable-based APIs:

```bezel
var evenSquares := (1..20).Where(lambda(x) => x mod 2 = 0).Select(lambda(x) => x * x);
var total := (1..100).Sum();
```

The compiler may optimize `for var i in rangeExpr` into a direct counted loop that bypasses `GetEnumerator`; see language spec [§16.5](Bezel_Language_Spec.md#sec-iteration) and [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for).

<a id="sec-enum-compiler-provided-members"></a>
### 9.9 Enum types: the compiler-provided `FromUnderlyingOrNull`, `FromNameOrNull` and `NameOf`

Enums are user-declared types, not standard-library ones, but every enum `E` with underlying type `U` carries three **compiler-provided** static members (language spec [§10.4](Bezel_Language_Spec.md#sec-methods-properties-static-members) — no declaration written, no user override):

```bezel
/// Compiler-provided on every enum E with underlying type U:
public static method FromUnderlyingOrNull(value: U): E?;
public static method FromNameOrNull(name: string): E?;
public static method NameOf(value: E): string;
```

It returns the declared constant whose underlying value equals `value`, or `null` when there is none — total, never raises; under duplicate constant values the result is the shared value. It is the [§1](#sec-conventions) `*OrNull` tier beside the throwing cast: `E(n)` is the assert-shaped form (an undeclared value faults — language spec [§5.4](Bezel_Language_Spec.md#sec-enum-casts)), `E.FromUnderlyingOrNull(n)` the branch-shaped form for boundary code, composing directly with `??`/`?.`. An enum payload is a non-nullable value type, so `E?` carries the whole signal — no `Try` shape is needed ([§1](#sec-conventions)'s failure-signal rule; the `TryX(...): T?` spelling stays reserved for acquisition-shaped members). The parameter type is `U` exactly: a wider integer in hand is narrowed explicitly first (spec [§5.2](Bezel_Language_Spec.md#sec-explicit-conversions), checked), keeping out-of-range and not-a-declared-value separately visible.

**The name pair.** `FromNameOrNull` and `NameOf` are the two directions of the enum's declared-identifier table — each constant's own bare identifier, the table `nameof` (language spec [§17.9](Bezel_Language_Spec.md#sec-the-nameof-operator)) reads at compile time and [§2.13](#sec-the-type-class)'s `EnumNames` lists, so `E.NameOf(E.A) = nameof(E.A)` for every written constant. `FromNameOrNull(name)` returns the constant whose identifier equals `name` exactly ([§9.6](#sec-string)'s ordinal equality — case-sensitive, nothing trimmed; a digit string names no constant), or `null`; the parameter is non-nullable, so a `string?` in hand is narrowed first (`s!`, or a null test). `NameOf(value)` returns the constant's identifier — under duplicate values, the first-declared constant's, so the pair round-trips up to aliasing (language spec [§10.4](Bezel_Language_Spec.md#sec-methods-properties-static-members) states both laws). Both are total. A `serialized "<name>"` rename (language spec [§9.8](Bezel_Language_Spec.md#sec-field-name-customization)) is the encoded form only and is invisible to both: a boundary that wants the wire spelling deserializes it ([§2.15](#sec-concrete-serialization-formats)). `FromNameOrNull` is the [§1](#sec-conventions) `*OrNull` tier's band-2 shape and the enum's *only* parse spelling: there is no assert *tier* on the name axis — `E.FromNameOrNull(s) ?? throw …` at the call site, which picks its own exception, is the recorded idiom — and a throwing twin and a shape-(c) `Try` form are recorded declines (`DECLINED.md`, *Enum name-parse tiers*): an enum has no `default(E)` to hand back on a false path unless a zero constant is declared (language spec [§17.11](Bezel_Language_Spec.md#sec-the-default-operator)). `NameOf` is the value→name direction `ToString` does not provide (language spec [§8.5](Bezel_Language_Spec.md#sec-tostring) renders the qualified type name).

---

<a id="sec-ffi-boundary-types"></a>
## 10. FFI-boundary types

`CString`, `WString`, `RawBytes`, `IntPtr`, and `UIntPtr` are FFI-boundary types. The first three are classes with `owned`-returning factory methods and idempotent `Dispose`. `IntPtr` and `UIntPtr` are compiler-intrinsic value types representing pointer-sized integers — they are documented here for completeness, but their members are detailed in language spec [§32.7](Bezel_Language_Spec.md#sec-pointers-and-intptr). The types of §10.0–§10.4 live in the `System` module and are auto-imported (language spec [§2.3](Bezel_Language_Spec.md#sec-always-available-names)). The typed foreign-memory surface of [§10.5](#sec-system-interop-ptr)–[§10.6](#sec-system-interop-interop) lives in **`System.Interop`**, which is *not* auto-imported: naming `Ptr<T>` or `Interop` requires the `import System.Interop;` line, a convention marking a file's typed foreign-memory surface rather than a sound audit marker for foreign-memory access — the §10.0–§10.4 types above are auto-imported and reach foreign memory without it (language spec [§2.3](Bezel_Language_Spec.md#sec-always-available-names)).

<a id="sec-intptr-and-uintptr"></a>
### 10.0 `IntPtr` and `UIntPtr`

```bezel
/// Compiler-intrinsic; pointer-sized integer (32 or 64 bits depending on target).
public record IntPtr
  public static readonly property Zero: IntPtr;

  // §25.2's co-declaration rule, with no intrinsic exemption (the exemption is retired):
  // a record overloading homogeneous =/<> declares Equals(T) and
  // `override GetHashCode` beside them. The homogeneous overload already
  // strips auto-hashable (§9.2's membership list and its third disqualifier,
  // ruled), so these two members ADD the correct address-based equality
  // surface rather than replacing a synthesized one -- ADDING them reclassifies
  // no type, because the operators already did. (A record with an `IntPtr`
  // component is not auto-hashable under that ruling; see the language spec
  // §9.2 recursive component rule and the v0.276 changelog entry's stated cost.)
  // The §10.5 `Ptr<T>` spelling, verbatim.
  public method Equals(other: IntPtr): bool;
  public override method GetHashCode(): int;

  public static method operator =(a: IntPtr, b: IntPtr): bool;
  public static method operator <>(a: IntPtr, b: IntPtr): bool;
end IntPtr;

/// Compiler-intrinsic; unsigned pointer-sized integer.
public record UIntPtr
  public static readonly property Zero: UIntPtr;

  public method Equals(other: UIntPtr): bool;        // §25.2 co-declaration set, as `IntPtr` above
  public override method GetHashCode(): int;

  public static method operator =(a: UIntPtr, b: UIntPtr): bool;
  public static method operator <>(a: UIntPtr, b: UIntPtr): bool;
end UIntPtr;
```

`IntPtr` and `UIntPtr` are opaque pointer-sized integers. They are not dereferenced from Bezel code; their purpose is to flow opaque addresses through FFI calls (the result of `dlopen`, the `void*` argument of a callback, etc.). `Zero` is the null-pointer value. It is a `static readonly` property, not a `const`: language spec [§22.12](Bezel_Language_Spec.md#sec-static-members) restricts `const` to a primitive value type or `string`, and `IntPtr` is a record — the same spelling `Guid.Empty` ([§2.6](#sec-guid)), `DateTime.MinValue` and `TimeSpan.Zero` ([§2.5](#sec-datetime-timespan-dateonly-timeonly)), and `CancellationToken.None` ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)) use for a record-typed sentinel. The value is compiler-provided; the property spelling constrains the surface, not the lowering ([§1](#sec-conventions)).

The `=` and `<>` operators support null tests (`if handle = IntPtr.Zero then ...`). Unlike the primitive value-type wrappers in [§9.1](#sec-integer-types)–[§9.5](#sec-char), `IntPtr` and `UIntPtr` cannot rely on the language-intrinsic comparison rule for primitive types (language spec [§3.5](Bezel_Language_Spec.md#sec-operators-on-primitive-types)): that rule applies to the keyword-aliased primitive types (`int`, `long`, etc.) and to `string`, not to general record types. Nor can the auto-equality machinery (language spec [§9.2](Bezel_Language_Spec.md#sec-body-records)) supply meaningful equality — `IntPtr` and `UIntPtr` have no auto-properties exposing their underlying pointer-sized value, so a synthesized `=` over them could only be the zero-property "every-pair-equal" form, which is wrong for pointer comparison. (No synthesis reaches these types at all: the homogeneous `=` overload below strips them of **auto-hashable** by itself, language spec [§9.2](Bezel_Language_Spec.md#sec-body-records). The zero-property form is what synthesis *would* produce for a version of these records carrying no operators — the reason the operators are declared, not a description of what §9.2 does to them today.) Equality for these types is therefore declared explicitly, as the four members [§10.5](#sec-system-interop-ptr)'s `Ptr<T>` declares: the `=`/`<>` overloads plus the `Equals(T)` and `override GetHashCode` that language spec [§25.2](Bezel_Language_Spec.md#sec-operator-overloading-restrictions)'s co-declaration rule requires beside them (there is no intrinsic exemption; the date-scoped one these types formerly carried was retired). All four implementations are compiler intrinsics that compare or hash the underlying pointer-sized integer representation. The overload strips these records of **auto-hashable** (language spec [§9.2](Bezel_Language_Spec.md#sec-body-records)) on its own, so the co-declared pair adds the correct address-based `Equals`/`GetHashCode` rather than displacing a synthesized one — which is what admits `Dictionary<IntPtr, V>` and `Set<IntPtr>` at all, keyed on the address: without the co-declared pair these records would be neither auto-hashable nor user-equality-declaring, hence outside **hashable** (language spec [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)) and rejected as a key type.

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
  public readonly property Pointer: IntPtr;   // address of the buffer — see prose
  public method ToBytes(): owned Array<byte>;

  destructor;       // releases the underlying buffer when this CString
                    //   allocated it (CString.From / FromBytes); for a CString
                    //   wrapping a native-allocated buffer, the disposal
                    //   behavior is governed by the FFI declaration. See prose.
end CString;
```

`CString.From(s)` allocates a fresh, owned `CString` whose contents are the UTF-8 encoding of `s` followed by a single null byte. The caller owns the result; when disposed, the underlying buffer is released.

`CString.FromBytes(bytes)` **copies** the bytes of `bytes` into a fresh, owned buffer and appends a null terminator; the source array is unaffected and the result's buffer is independent of it (the same copy-in semantics as `RawBytes.From` in [§10.3](#sec-rawbytes), and as the borrow parameter `bytes: Array<byte>` requires — a borrowed argument cannot be retained past the call). `FromBytes` performs **no validation**: the bytes are taken verbatim. Well-formedness is a contract between the caller and the consuming native API, not with `CString` — an interior null byte truncates the C-side view (C-string convention), and ill-formed UTF-8 produces an ill-formed C string. That is deliberate: unlike `From(string)` — which encodes a Bezel string's code points — `FromBytes` is the path for already-encoded or possibly-ill-formed data obtained from I/O or another source.

A `CString` returned from a native function is wrapped without copying; the Bezel side must understand the native API's ownership contract (see language spec [§32.6](Bezel_Language_Spec.md#sec-string-marshalling)). Disposing such a `CString` calls the native deallocator only when constructed via `CString.From` or `FromBytes`. For native-allocated strings, the FFI declaration's documentation must specify how the native allocation should be released; this is beyond the scope of `CString` itself.

**`Pointer`** exposes the underlying buffer's address (the same contract as `RawBytes.Pointer`, [§10.3](#sec-rawbytes): valid until the `CString` is disposed) — the element source for `char*[]` argv-building over `OwningBuffer<IntPtr>` ([§4.14](#sec-owned-buffers-and-spans)). Native retention of the address past the synchronous call is the trusted-lifetime obligation of language spec [§32.10](Bezel_Language_Spec.md#sec-boundary-contract).

<a id="sec-wstring"></a>
### 10.2 `WString`

A null-terminated UTF-16 sequence, suitable for Windows APIs and other UTF-16-using ABIs.

```bezel
public class WString
  // No public constructor; obtained via factory methods.

  public static method From(s: string): owned WString;

  public readonly property Length: int;       // length in UTF-16 code units, excluding the null terminator
  public readonly property Pointer: IntPtr;   // address of the buffer — see prose

  destructor;       // releases the underlying buffer
end WString;
```

`WString.From(s)` allocates a fresh, owned `WString` whose contents are the UTF-16 encoding of `s` followed by a single zero code unit (two zero bytes). Surrogate pairs are produced for code points above the BMP per UTF-16 rules.

**`Pointer`** exposes the underlying buffer's address, valid until the `WString` is disposed — the same contract as `CString.Pointer` ([§10.1](#sec-cstring)) and `RawBytes.Pointer` ([§10.3](#sec-rawbytes)). Native retention past the synchronous call is the trusted-lifetime obligation of language spec [§32.10](Bezel_Language_Spec.md#sec-boundary-contract).

<a id="sec-rawbytes"></a>
### 10.3 `RawBytes`

A pointer-and-length pair representing arbitrary binary data.

```bezel
public class RawBytes
  // No public constructor; obtained via factory methods.

  public static method From(buffer: Array<byte>): owned RawBytes;
  public static method FromForeign(p: IntPtr, length: int,
      freeOnDispose: bool := false): owned RawBytes;    // — see prose

  public readonly property Length: int;
  public readonly property Pointer: IntPtr;

  public method CopyTo(destination: Array<byte>, destOffset: int);
      // copies Length bytes into destination[destOffset .. destOffset+Length);
      //   a window outside destination's Count raises IndexOutOfRangeException
      //   (the §1/§7.1 window rule); on a FromForeign view the read is over
      //   foreign memory — spec §32.10 trust applies

  destructor;       // releases the underlying buffer if owned by this instance
end RawBytes;
```

`RawBytes.From(buffer)` allocates a fresh, owned `RawBytes` containing a copy of the source bytes. Mutations to the source after construction do not affect the `RawBytes`; the buffer is independent.

`Pointer` exposes the underlying memory address as an `IntPtr`, suitable for passing to native APIs that expect `void*`. The pointer is valid until the `RawBytes` is disposed.

**`FromForeign`** wraps `[p, p + length)` **without copying**. With `freeOnDispose := false` (the default) the result is a *view*: `Length`, `Pointer`, and `CopyTo` operate over foreign memory and disposal releases nothing — the destructor's existing "if owned by this instance" contract already carries the vocabulary. With `freeOnDispose := true`, disposal passes `p` to the C runtime's `free(3)` — the "returns a malloc'd buffer the caller must free" contract (POSIX `getline`'s buffer, `realpath(path, NULL)`). Two trust obligations, stated plainly: (a) if the buffer was not allocated by the same C runtime's `malloc` family, disposal is undefined behavior; (b) a custom deallocator (a library's `libfoo_free`) is *not* expressible through this flag — that contract belongs to a wrapper class owning both handle and buffer (language spec [§32.13](Bezel_Language_Spec.md#sec-c-compatible-records)). Cross-domain note: `RawBytes` is a class and not sendable (language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)'s class rule — not `sealed`), so a `FromForeign` view never crosses a domain boundary; stated here so the invariant is explicit.

<a id="sec-conversions-to-bezel-types"></a>
### 10.4 Conversions to Bezel types

Two conversion families exist for reading C-side strings:

- **From a caller-owned `CString` instance:** `string.From(value: CString): string` and `string.From(value: WString): string` copy the contents into a fresh Bezel `string`. The source `CString` or `WString` retains its ownership and disposal obligation.
- **From a raw pointer (no ownership):** `string.FromCStringPointer(p: IntPtr): string` and `string.FromWStringPointer(p: IntPtr): string` read through a pointer the Bezel side does not own and produce a copied Bezel `string`. Use these when the FFI declaration's return type is `IntPtr` (a borrowed pointer; see language spec [§32.6](Bezel_Language_Spec.md#sec-string-marshalling)) rather than `CString`/`WString` (a caller-owned instance).

Both forms are O(n) in the source length and do not retain a reference to the source after return.

Behavior at the boundaries:

- `string.FromCStringPointer(IntPtr.Zero)` and `string.FromWStringPointer(IntPtr.Zero)` raise `ArgumentNullException`. Callers must check for the null pointer before invoking the conversion (the typical pattern is `if p <> IntPtr.Zero then ... end`).
- For non-null pointers, the conversion reads bytes (`FromCStringPointer`) or 16-bit code units (`FromWStringPointer`) starting at the pointer until a null terminator is found. If the pointer does not refer to a properly null-terminated buffer, behavior is undefined — the read may continue past the intended end of the string, returning garbage or causing a segmentation fault. The Bezel side cannot validate; the FFI declaration is trusted to expose only properly-formed strings.

Each FFI-boundary type has a corresponding `string.From` overload ([§9.6](#sec-string)) for `CString` and `WString`, returning a Bezel `string`. For `RawBytes`, callers convert through `Array<byte>` via `CopyTo` and pass the byte array to the appropriate decoder (e.g. `Encoding.UTF8.GetString`).

<a id="sec-system-interop-ptr"></a>
### 10.5 `System.Interop`: `Ptr<T>`

> ⚠ **Spec-ahead-of-implementation ([#5580](https://github.com/s8lap/bezel-lang/issues/5580), [#1765](https://github.com/s8lap/bezel-lang/issues/1765)):** `LoadAt`, `StoreAt`, `CopyFromArray` and `FromIntPtr` are not implemented. `Load()` and `Store()` are not implemented either: no lowering reads or writes the pointee.

```bezel
// Compiler-intrinsic value record; module System.Interop (NOT auto-imported).
// Representation identical to IntPtr. T is gated (per-instantiation, language
// spec §3.6-style diagnostic) to the FFI-POINTEE SET: integer types, float,
// double, bool, IntPtr, UIntPtr — and C-compatible records (language spec
// §32.13). Nested Ptr<Ptr<T>> is NOT admitted (language spec §32.12).
public record Ptr<T>
  public static readonly property Zero: Ptr<T>;        // the null pointer, like IntPtr.Zero

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
  public method Equals(other: Ptr<T>): bool;           // §25.2 co-declaration rule: a record
  public override method GetHashCode(): int;           //   overloading homogeneous =/<> declares
                                                       //   both. §25.2 admits no intrinsic
                                                       //   exemption; §10.0's IntPtr/UIntPtr
                                                       //   declare the same four members.
  public static method operator =(a: Ptr<T>, b: Ptr<T>): bool;
  public static method operator <>(a: Ptr<T>, b: Ptr<T>): bool;
end Ptr;
```

The pointee gate mirrors language spec [§32.12](Bezel_Language_Spec.md#sec-foreign-memory): `T` is drawn from the FFI-pointee set — integer types, `float`, `double`, `bool`, `IntPtr`, `UIntPtr`, and C-compatible records (language spec [§32.13](Bezel_Language_Spec.md#sec-c-compatible-records)); any other instantiation is rejected at the instantiation site with a diagnostic naming the set. `Ptr<Ptr<U>>` is not admitted — `Ptr<U>` is an FFI-compatible scalar (language spec [§32.5](Bezel_Language_Spec.md#sec-ffi-compatible-types)) but is not in this set; pointer-array walking is `Ptr<IntPtr>` plus explicit re-typing.

`CopyToArray(count)` copies `count` elements from foreign memory into a fresh Bezel-owned `Array<T>`; `CopyFromArray(source)` writes `source.Count` elements at `p`. Both are explicit whole-copies — foreign data becomes Bezel data at exactly one visible point.

Equality is declared explicitly rather than left to record synthesis for the same reason as `IntPtr`'s ([§10.0](#sec-intptr-and-uintptr)): `Ptr<T>` has no auto-properties, so the synthesized `=` would be the zero-property "every-pair-equal" form. `Zero` is the null pointer, a `static readonly` property at record type for the reason [§10.0](#sec-intptr-and-uintptr) gives — language spec [§22.12](Bezel_Language_Spec.md#sec-static-members) admits `const` only at a primitive value type or `string`. The value is compiler-provided.

`Ptr<T>` computes **sendable** under language spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)'s value-type rule, with the cross-domain trust bullet of language spec [§32.10](Bezel_Language_Spec.md#sec-boundary-contract): foreign memory reached through a crossed pointer is outside domain isolation, and unsynchronized cross-domain access to the same address is a data race and undefined behavior.

<a id="sec-system-interop-interop"></a>
### 10.6 `System.Interop`: `Interop`

> ⚠ **Spec-ahead-of-implementation ([#5581](https://github.com/s8lap/bezel-lang/issues/5581)):** `Interop` is not implemented: no sema or codegen arm names `LastErrno()` or `CodeAddress<F>`. The surface below is the normative contract the implementation will meet.

```bezel
/// Static intrinsics for the FFI boundary. Module System.Interop (NOT auto-imported).
public static class Interop
  // The calling thread's C errno, read directly. Valid only when read BEFORE the
  // next extern call or runtime operation on this thread — and NEVER across an
  // await: a continuation may resume on a DIFFERENT OS thread (language spec
  // §30.13), so a post-await read is another thread's errno. Read it first,
  // synchronously, in the same continuation as the failing call.
  public static method LastErrno(): int;

  // The process-lifetime code address of a callback admissible under the STATIC
  // rule of language spec §32.8; F gated per-instantiation to FFI-compatible
  // function types. One-way by design: no address→function inverse exists.
  public static method CodeAddress<F>(callback: F): IntPtr;
end Interop;
```

`LastErrno()` reads the calling thread's C `errno` directly. The window discipline is real, not decorative: read it into a local **before** constructing an exception message — interpolation allocates, and allocation is a runtime operation that may clobber the thread-local — and **never across an `await`** (a continuation may resume on a different OS thread, language spec [§30.13](Bezel_Language_Spec.md#sec-runtime-model): a post-await read is another thread's errno). Language spec [§32.9](Bezel_Language_Spec.md#sec-error-handling) shows the canonical use.

`CodeAddress<F>(callback)` returns the process-lifetime code address of a callback admissible under the **static** rule of language spec [§32.8](Bezel_Language_Spec.md#sec-callbacks) — a static method reference or a non-capturing lambda; a capture-bearing argument is rejected with §32.8's diagnostic, and the `env` clause does not apply (there is no bounding call). `F` is gated per-instantiation to FFI-compatible function types. One-way by design: no address→function inverse exists (language spec [§32.14](Bezel_Language_Spec.md#sec-deferred-ffi-surface)).

---

<a id="sec-modules-deferred-to-future-revisions"></a>
## 11. Modules deferred to future revisions

The following standard library modules are anticipated but not specified in this version. Their absence is intentional; the current scope focuses on what the language specification depends on.

- `System.Net` and `System.Net.Http` — networking and HTTP. A real HTTP client will live here.
- `System.Text.Json` and `System.Text.Regex` — a general JSON DOM/streaming API and regular expressions. Note this is distinct from the serialization JSON *format agent* (`JsonSerializer`/`JsonDeserializer` and the one-shot `Json` facade in `System.Serialization`, [§2.15](#sec-concrete-serialization-formats)), which is provided: `System.Serialization` maps the [§2.14](#sec-serialization-vocabulary) token stream to and from JSON for whole-value (de)serialization, whereas a future `System.Text.Json` would offer document-tree navigation and ad-hoc querying of arbitrary JSON not driven by a target type. The two are complementary, as they are in other ecosystems that provide both a serializer and a document API.
- `System.Security.Cryptography` — cryptographic primitives.
- `System.Globalization` — culture-aware formatting and comparison.
- `System.Reflection` — runtime metadata beyond `typeof` and `GetType`. This splits three ways (rationale [§18](Bezel_Design_Rationale.md#sec-things-deferred)): type-lattice metadata is **delivered** on the `Type` class ([§2.13](#sec-the-type-class)); member-access reflection is **declined, not deferred**; generic-instantiation decomposition remains deferred, pending an unbound-generic `typeof`.
- `System.Diagnostics.Process` — child-process spawning and management.

The shape of these modules is open. Their inclusion in a future revision will be additive; nothing in this version will need to change to accommodate them.
