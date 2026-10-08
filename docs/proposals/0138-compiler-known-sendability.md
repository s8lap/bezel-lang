# Proposal 0138 — The sendability of the compiler-known classes: close the open-class seam, then decide the cluster on it

| | |
|---|---|
| **Status** | graduated-v0.327 — superseded — graduated v0.327 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues"; critique round 1 applied, 2 blocking and 11 non-blocking findings resolved; landed with the v0.327 changelog entry; §13's question on `Type`'s statics-and-sharing facts is left open, and `FrozenArray<Type>` stays as it is) |
| **Tracking issue** | [#5613](https://github.com/s8lap/bezel-lang/issues/5613) (spec-defect, the seam), [#5614](https://github.com/s8lap/bezel-lang/issues/5614) (the copy-state ground), [#5578](https://github.com/s8lap/bezel-lang/issues/5578) (`OwningBuffer<T>` at a value element), [#5615](https://github.com/s8lap/bezel-lang/issues/5615) (`Type`), [#5605](https://github.com/s8lap/bezel-lang/issues/5605) (a compiler-known base's inherited state), [#5604](https://github.com/s8lap/bezel-lang/issues/5604) (spec-defect, the retained comparer) |
| **Target module(s)** | **spec** — §12.4 (one paragraph), §9.7 (two example lists), §30.14 (the forms bullet, the `string` bullet, rule 2, the indirect-alias paragraph, *Faults*), §30.15 (`Send`), §28.3 (K4.3 rule 3's recorded clauses; the trust-admission report's register), §31.4 (one clause), §32.10 (the cross-domain bullet), Appendix C.1 (one row); **stdlib** — §1 (an *Extensibility* row; the *Sendability statements* paragraph), §2.7, §2.13, §3.1, §8.1 (sendability statements), §3.2, §7.1, §7.4 (the opened classes), §4 (the statement; *A retained comparer is a borrow*), §4.8, §4.14, §4.20, §6.8, §6.9, §6.11 (one phrase each); **rationale** — §12.6 and §12.13 (one v0.327 addendum each); **code-generation reference** — four index lines, the buffers lead, one listing comment, two §7.6 pairs; **`DECLINED.md`** — three `rejected` and two `deferred` rows, one row annotated taken; **compiler** — `src/sema/TypeResolution_Names.cpp` (base-type resolution), `src/sema/Sendability.cpp` (the builtin arm, the inherited-storage walk), the trust-admission report. |
| **Written against** | spec **v0.325** at `b27728a` (main; the stdlib companion, rationale, code-generation reference and guides at the same commit). The v0.326 guide re-trace lands underneath; it touches the guides and §28.3's *Bare-type-parameter returns* clauses, none of which this proposal edits. |
| **Origin** | The v0.323 presentation move (owner decision Q5(b)) made the stdlib the single carrier of each compiler-known type's sendability and surfaced #5578; v0.324's ruling on the unstated default (#5593) surfaced #5604 and #5605; v0.325's critic overturned a lane that had stated three more open-listed classes sendable, and filed #5613, #5614 and #5615. The six are one cluster: every decision below rests on the first. |

*Reference convention: unprefixed §-references are language-spec references at `b27728a` (v0.325); K tags are §28.3's kernel ids; rationale, stdlib, code-generation-reference and guide references are marked. Quotations are verbatim. ▲ marks proposed text. The programs below use only members the stdlib companion declares; none invents an API. The language is referred to by its current name only.*

---

## 0. Thesis

Rationale §12.6 records why a sendable declared class must be `sealed`: "sendability is checked on static types, but an owned field typed at a sendable open class could hold, at runtime, a subclass instance carrying a back-reference into the origin domain — a data race smuggled past the static check." Spec §30.14 rule 1 enforces that for declared classes. Nothing enforces it for the compiler-known ones. Stdlib §1 keeps "the transfer-closed rules for declared classes", rule 1 included, off compiler-known classes; §12.4 seals only a class declared `sealed`; and the library states sendable, unconditionally or conditionally, eight classes whose listings are not `sealed`. So the text admits

```bezel
public class Evil: Mutex
  private _back: Item;                 // a non-`owned` class field: a borrow of an origin-domain object
  public constructor(back: Item) _back := back; end constructor;
end Evil;
```

and then `Domain.Spawn(Work, move m)` at `m: owned Mutex` carries the borrow across. That is #5613, and it is why v0.325 deferred three more statements (#5606) rather than add to a set it could not close.

This proposal closes the seam with one rule (§3): **a compiler-known class is a valid base class only where its text opens it** — today `Encoding`, `Stream`, `TextReader`/`TextWriter` and the exception family — the abstract library classes, whose abstract members exist to be supplied, and the exceptions — and the only ones any program derives from (§2). A concrete class's inherited virtual members (`FileStream`'s `ReadAsync`/`WriteAsync`) or `Object` overrides (`JsonSerializer.ToString`) do not open it. On that footing it decides the rest: a fourth ground for §30.14's **sendable** form, *a holder of copy state only*, with statements for `StringBuilder`, `Random` and `Stopwatch` (§4); `OwningBuffer<T>` sendable at a sendable value element, with §32.10's cross-domain bullet extended to a kept `DangerousPointer()` (§4); `Type` sendable as a shared reference beside `string`, classified by the spec's runtime-managed bullet rather than by a new statement form (§5); a declared class deriving from an opened base not sendable until the base's section states its inherited state transfer-compatible, which no section does (§6); and the retained `IEqualityComparer<K>` of a hash collection recorded as the *comparer-held clause* of §28.3's domain clause, the static bound deferred (§7). Kernel: the seam closure is what makes §30.14 rule 1's static guarantee a derivation of K2.3 for compiler-known types rather than an assumption; the comparer clause is a K4.3 recorded trust clause under rule 3's *recorded trust, or nothing*. Body-locality (proposal 0105): the base-class rule is local (declaration-site); every sendability change is fact-discharged through §3.6's sendable property, as the existing rules are.

---

## 1. Ground truth

### 1.1 The rules today

- **§12.4 (line 2062):** "`sealed` on a class prevents inheritance from that class." It is the only sealing rule; nothing seals a compiler-known class implicitly.
- **§30.14, the forms bullet (line 8864):** compiler-known types "take their sendability from the sendability statement in their own stdlib section … Each statement is justified by the type's internal structure and is an instance of one of four forms: **sendable** — the type is internally synchronized and reached from several domains by purpose, deeply immutable, or a channel endpoint, and crosses as an owned transfer; **sendable iff its elements meet the statement's condition** — a type whose state is wholly owned (an owning collection, `OwningBuffer<T>`) iff its element type is a sendable class type, and a borrowing collection iff its element category is sendable-value or `string` …; **sendable iff its payload is in §28.13's `Shared`-payload synchronized set**; and **not sendable** …. A compiler-known class type that no statement classifies is **not sendable** (stdlib §1)." The `string` bullet: "`string` is sendable; it crosses as a shared reference, which is safe because strings are immutable." Rule 2: "Every instance field, including inherited fields, is: …". *Faults*: "exception classes are not in general sendable".
- **Stdlib §1, *Sendability statements* (line 82):** "The transfer-closed rules for declared classes do not classify a compiler-known class: they read instance fields, and this document is not normative for internal data structures." **Stdlib §4 (line 1539):** "A value-typed `OwningBuffer<T>` (`OwningBuffer<double>`) is not sendable."
- **The listings.** `public class` without `sealed`: `Random` (§2.7), `StringBuilder` (§3.1), `Array<T>` and every collection (§4), `OwningBuffer<T>` (§4.14), `Counter<K>` (§4.20), `Mutex` (§6.2), `Semaphore` (§6.3), `Atomic<T>` (§6.4), `Channel<T>`/`ChannelSender<T>` (§6.9), `Stopwatch` (§8.1). `public sealed class`: `Type` (§2.13), `SharedEventSource<T>` (§6.5). `public abstract class`: `Encoding` (§3.2), `Stream` (§7.1), `TextReader`/`TextWriter` (§7.4). Extensibility is stated in words for `Encoding` only ("User code may subclass `Encoding`"); for `Stream` and the text pair it is implied by `abstract` and the override-points prose; for the exception family by §31.4 ("User subclasses chain by calling the base constructor").
- **§28.9 (line 7237):** the runtime-managed tier is `string`, `Type`, boxed values and function-typed values; "`owned` over `Type` is inadmissible". §28.11's slot policy already classes `Type` with `string` as a *shared reference* element; §15.1 names `Type` among the runtime-managed element types that make an inline array not borrow-bearing.
- **The comparer.** `Dictionary<K, V>`, `OwningDictionary<K, V>`, `Set<T>`, `OwningSet<T>` each declare `constructor(comparer: IEqualityComparer<K>)` (stdlib §4.8–§4.10): a plain parameter, so a borrow (§28.3); §4.9's reference implementation stores it (`private _comparer`), and every lookup consults it, so the retention is observable behaviour (rationale §17.14). §28.3's domain clause (the field-store bullet, clauses 4–5, line 6715) binds the *source* not to cross; §30.14 rule 3 forbids an interface-typed field in a declared sendable class; nothing binds a comparer-built collection, which the §4 statement classifies by type.
- **Recorded declines in the area** (`DECLINED.md`): *Runtime send-check for sendable open classes* (`deferred`, rationale §12.6); *Subclass-must-be-sendable inheritance rules* (`rejected`, §12.6); *Sendability requirement on throwable types* (`rejected`, §12.6); the v0.324 row on reading the transfer-closed rules over a compiler-known section (`rejected`); the v0.325 row deferring the three statements until #5613 and #5614 are ruled (`deferred`); the v0.323 presentation row (`rejected`). None of the decisions below contradicts one; §10 says how each is met.

### 1.2 The compiler today (`src/` at `b27728a`; this environment has no build, so the base-class behaviour is read, not measured)

- **Deriving from a compiler-known class.** `src/sema/TypeResolution_Names.cpp:1197–1213` records a base that resolves to no user `TypeSymbol` in `nonSymbolBaseRefs` ("every compiler-provided interface" — the rule is structural, and a builtin *class* base lands there too); `:1255–1283` rejects deriving from a `sealed`/`closed` *declared* class; `src/sema/SymbolCollection.cpp:1094` (`builtinExceptionBaseOf`) gives a builtin exception base its layout and surface. **No rejection of a non-exception builtin base was found**, so `class E: Mutex end E;` probably resolves; what codegen does with it is unknown. The probe that settles it: `class E: Mutex end E;`, `class E: StringBuilder end E;`, `class E: Array<int> end E;`.
- **Sendability.** `src/sema/Sendability.cpp:274–400` is the builtin-name arm: `OwningArray`/`OwningStack`/`OwningQueue`/`OwningSet`/`OwningLinkedList` (`:331–338`) and `OwningDictionary` (`:340–346`) recurse on their arguments; `Array`/`Stack`/`Queue`/`Set`/`LinkedList` (`:353–371`) admit `string`, primitives and non-borrow-bearing records; `Channel`/`ChannelSender`, `Atomic`/`AtomicInteger`, `Mutex`, `Semaphore` return true; everything else — `StringBuilder`, `Random`, `Stopwatch`, `Type`, `OwningBuffer` at every element — falls through to `return false` at `:398–400`. A `Type` element in `Array<Type>` is neither `string`, a primitive, a user class nor a user record, so it falls to `return false` at `:371`. A user record with a `Type` field recurses into the fall-through (`:522`). So the compiler says "not sendable" for every statement §4–§5 add.
- **Inherited state.** `forEachInheritedStorageMember` (`:202–225`) skips a base with no class `TypeSymbol`; a builtin base has none, so `sealed class E: Exception` with `string` fields computes **sendable** today — the unsound direction of #5605.
- **Trust-admission report.** No `src/` text mentions it; the mode is unimplemented, so the comparer site rides whatever tracks the mode.
- **Tests.** No gtest, lit or rule-corpus case derives from a compiler-known class other than `FileStream: Stream` in two skipped doc-example listings (`codegen_0031`, `stdlib_0135`); no test declares a `sealed` class deriving from an exception type (`grep -E 'sealed class \w+\s*:\s*\w*Exception'` over `tests/`: 0); no test passes a comparer-built collection to `Domain.Spawn`, a `Channel<T>` or a `Locked<T>`; no test sends `StringBuilder`, `Random`, `Stopwatch`, `Type` or a value-typed `OwningBuffer<T>` across a domain.

---

## 2. Measurement: who derives from what, and what crosses

| Question | Method | Result |
|---|---|---|
| User classes deriving from a compiler-known **non-exception** class | `grep -rnE 'class \w+\s*:\s*(Mutex\|StringBuilder\|…\|Type\|Shared<\|Task)\b'` over `tests/`, `src/`, `docs/` | **0** in tests and guides. `FileStream: Stream` appears only as the stdlib's own listing and its two doc-example mirrors (both `skip:signature-listing`); the proposals folder's `MemoryStream: Stream`, `StreamWriter: TextWriter`, `GzipStream: Stream` are future library types over opened bases. `UrgentTask : Task` in `property_accessor_types_510_test.cpp:58` derives from a *user* class named `Task`. |
| User classes deriving from an exception type | same grep for `\w*Exception` | 518 lines across `tests/` and `docs/`; none `sealed`, none sent across a domain. Under §6 none changes verdict (an open class was never sendable). |
| Comparer-built collections crossing a domain | files mentioning `IEqualityComparer` ∩ files mentioning `Spawn`/`Channel<`/`Locked<` | one file (`builtin_member_population_2998_test.cpp`), where the two never meet. **0** sites; no doc example either. |
| `OwningBuffer<T>` at a value element crossing a domain | `grep` over `tests/` and the two corpora | **0**. The v0.323 changelog lists "Q5: `OwningBuffer<double>` as a `Channel<T>` element (reject)" under *coverage gaps recorded, not written*; that unwritten case's verdict flips to accept (§11). |
| `Type` crossing a domain, or in a record that does | `grep typeof\|GetType` ∩ `Spawn\|Channel` over `tests/` | **0** (`typeof` is rejected in codegen, #487, so none could run). |
| Sites naming `string` as the shared-reference sendable beside the sendable-value category | `grep 'sendable-value or `string`'` and its spellings over `docs/` | 6 normative sites (stdlib §4 ×2, §4.20, §6.11 ×2; spec §30.14), plus the `Send`/`Spawn`/`Channel` row spellings (spec §30.15; stdlib §6.8, §6.9). All are edited (§9). `FrozenArray<T>`'s "sendable value types and `string`" (stdlib §4.19 ×3, Guide 05) is an element restriction of its own and is left alone (§5). |

Every verdict this proposal moves is therefore **spec-ahead-of-impl with zero measured reach** in the repository, in both directions: the newly rejected shapes (§3, §6) have no instances, and the newly admitted ones (§4, §5) have no instances because the compiler rejects them today.

---

## 3. The seam (#5613): a compiler-known class is a valid base class only where its text opens it

### 3.1 The options

| | Reading | Soundness | Simplicity | Consistency | Migration |
|---|---|---|---|---|---|
| (i) **taken** | A compiler-known class admits a user subclass only where the text that declares it says so; every other one is closed as if `sealed`. | Closes the hole for every statement, present and future, in one rule. | One sentence in §12.4, one convention row in stdlib §1; `sealed` on a listing becomes informative. | The opened set — `Encoding`, `Stream`, `TextReader`/`TextWriter`, the exception family — is the abstract library classes plus the exception family; no other library class declares an abstract member, and a concrete class's inherited virtual members (`FileStream` over `Stream`) are its implementation, not an extension point — `Stream` is that point. | 0 sites. |
| (ii) | Each sendable statement seals its class implicitly. | Closes each statement separately; a future statement that forgets is the hole again. | Ties an inheritance verdict to a sendability paragraph a reader of the listing never sees. | Leaves `StringBuilder` and the rest open for no named reason. | 0 sites. |
| (iii) | Keep every library class open; read the dynamic type at the send. | Sound. | The runtime send-check the decline index records `deferred` (rationale §12.6): a per-send cost proportional to the transferred graph, and a compile-time error turned into a runtime one. | — | — |
| status quo | — | The live gap of §0. | — | — | — |

### 3.2 The rule (▲ spec §12.4, after the `sealed`-class modifier paragraph)

> ▲ **Compiler-known classes are closed to inheritance unless their text opens them.** A compiler-known class — a class of the standard library, or `Exception` and the built-in exception types (§31.4) — is a valid base class only where the text that declares it states that user code may derive from it: stdlib §1 (*Extensibility*) lists the library classes so opened, and §31.4 opens the exception family. Every other compiler-known class is closed to inheritance exactly as a `sealed` class is, whether or not its listing writes `sealed`, and a base-type reference to one in a user declaration is a compile error naming this rule. `Object`, every class's implicit base ([§8.1](#sec-definition)), is not a compiler-known class in this sense, and the rule does not reach it. This is what lets a stdlib sendability statement (§30.14) stand for every instance of its type: a user subclass holding origin-domain borrows behind a compiler-known static type would be the hole §30.14's rule 1 closes for declared classes (rationale §12.6).

The membership lives in the stdlib (the *Extensibility* row of §1's conventions table, pointing at each opened section, which gains or keeps its own sentence) and in §31.4 for the exception family — the same single-carrier shape §30.14 uses for sendability ("this section states the forms, not the memberships"). Appendix C.1 gains the invariant row. §30.14's forms bullet gains the sentence that connects the two: "A statement stands for every instance of its type: a compiler-known class admits no user subclass unless its text opens it (§12.4), and a declared class deriving from an opened one is classified by the transfer-closed rules above, rule 2's compiler-known-base clause included."

### 3.3 What it does not do

It does not seal the opened classes' *subclasses*: a user `Stream` subclass is an ordinary declared class and may be derived from on §12.4's ordinary terms. It does not touch interfaces (a compiler-provided interface is implemented, not derived from). It does not change `closed` (§11.7), which no library class uses. And it does not make the opened classes' subclasses sendable — that is §6.

---

## 4. The copy-state ground (#5614) and `OwningBuffer<T>` at a value element (#5578)

### 4.1 The ground

§30.14's **sendable** form names three grounds and v0.325's critic was right that none fits "a mutable, unsynchronized holder of copy state". The options:

| | Reading | Soundness | Simplicity | Consistency | Migration |
|---|---|---|---|---|---|
| status quo | The three stay at stdlib §1's not-sendable default (v0.325's deferral). | Sound. | No change. | Leaves `Array<char>` sendable and a `StringBuilder` holding the same text not, with no structural reason. | 0 sites. |
| (a) **taken** | A fourth ground, stated by member behaviour: every member reads and writes the state by value, retains no borrow past its call, and exposes no address of the storage (the issue's wording). | Each statement must justify all three conditions from the listed members; a type with a retained callback or an exported address cannot be stated on it. | One clause in the closed list; three statements. | The ground already underlies `Array<char>`'s conditional verdict. | 0 sites (newly accepting; the compiler rejects today). |
| (b) | The same ground without the no-address condition, leaving an exported address to §32.10. | Sound only as trust: a future statement could export an untracked address behind a sendable verdict. | One condition shorter. | — | 0 sites. |
| (c) | Stretch "deeply immutable" or "internally synchronized" to cover them. | Unsound reading of words that mean something else. | — | Breaks the existing grounds' meaning. | — |

(a) is taken: ▲ "… a channel endpoint, **or a holder of copy state only (every member reads and writes that state by value and retains no borrow past its call, and no member exposes an address of its storage)**, and crosses as an owned transfer". (b) is declined in the rationale addendum (no row: it is a weaker variant of the taken ground, not a separate design). It is the ground on which `Array<char>` is already sendable under the conditional form; stating it for a non-collection changes nothing about what "crosses as an owned transfer" means.

Per type, the facts each statement records:

- **`StringBuilder` (stdlib §3.1).** Text only. `Append(value: Object)` keeps `value.ToString()`'s text, not `value`; `AppendFormat`'s `args: Array<Object>` is a call-bounded borrow; no member returns a span, a `Ptr<T>` or any view of the storage (`ToString()` copies). A moved builder keeps its contents.
- **`Random` (§2.7).** Seed and position. `NextBytes(buffer: Array<byte>)` is call-bounded. A moved instance continues its sequence; the replay-intercept point (the parameterless constructor) is untouched by a move.
- **`Stopwatch` (§8.1).** A running flag and counters. A moved running stopwatch keeps measuring, because elapsed time is read from the clock the replay-intercept rule names, not from the starting thread, and §30.13 already runs a domain's continuations on any thread.

The three were declined at v0.325 on two grounds; §3 removes the seam, and this section supplies the form. The v0.325 `deferred` row is annotated *taken* and stays as the record (§10).

### 4.2 `OwningBuffer<T>` at a sendable value element

The issue's recommendation is taken: **yes**. The buffer's state is wholly owned and crosses by move, `Array<double>` already carries mutable value bulk data across, and the two readings coincide at class elements. The one thing it has that the three types above do not is `DangerousPointer(): Ptr<T>` — "valid until the buffer is disposed", tracked by no borrow, on the value-typed presentation only, admitted only at the FFI-pointee set (stdlib §4.14). That pointer is an FFI-band value *whether or not the buffer moves*: using it after a local disposal is already the programmer's untracked obligation under §32.10. A transfer changes only who disposes. `OwningBuffer<T>` is classified by the conditional form, not by §4.1's ground, so the ground keeps its no-address condition and the address is §32.10's business: its cross-domain bullet — which already covers "a `Ptr<T>` that has crossed a domain boundary" — now also covers ▲ "an address of Bezel-owned storage whose owner has crossed a domain boundary — an `OwningBuffer<T>.DangerousPointer()` result the origin domain keeps after moving the buffer: the pointer is valid until the buffer is disposed, by whichever domain then holds it, and no borrow tracks it."

The conditional form reads ▲ "an owning collection iff its element type is a sendable class type, `OwningBuffer<T>` iff its element type is a sendable class type or a sendable value type (its storage is part of the instance and crosses with it; an address `DangerousPointer()` has returned stays with the origin domain under §32.10's cross-domain bullet)". Stdlib §4's statement flips its one sentence and names the example both ways (`OwningBuffer<double>` sendable; a buffer of a borrow-bearing record not). `Span<T>`/`ReadOnlySpan<T>` are borrow-bearing records and stay not sendable. `Locked<OwningBuffer<double>>` becomes an admitted interior (§6.11's "sendable class type" condition, unchanged in text).

---

## 5. `Type` (#5615): sendable, crossing as a shared reference, classified where `string` is

`Type` is runtime-managed (§28.9), immutable, one interned instance per type for the life of the process, reachable from every domain through `typeof`; its listing is `sealed`, and `owned Type` is inadmissible. The issue's three obstacles and their answers:

1. *Every sendable form crosses "as an owned transfer".* True, and that is the wrong carrier. §28.9's runtime-managed tier is `string`, `Type`, boxed values and function values; §30.14's second bullet classifies three of the four directly and omits `Type` alone. So `Type` joins that bullet — ▲ "`string` and `Type` are sendable; each crosses as a shared reference, which is safe because both are immutable — a `Type` value is the process's one interned instance of its type (§28.9), which every domain already reaches through `typeof`" — and the sentence that keeps compiler-known value types under the spec's rule gains its runtime-managed twin: ▲ "a compiler-known runtime-managed reference type (`string`, `Type`) by the `string` bullet above, likewise". Stdlib §2.13 restates the verdict, as stdlib §1 now says the runtime-managed sections do. The issue's fifth statement form is declined (§10): the forms classify the library's own classes, and `string`, which the stdlib also lists as a class, is the precedent for the spec classifying a runtime-managed type itself.
2. *The borrowing collections' condition is "sendable-value or `string`".* It becomes **sendable-value, `string` or `Type`** at every normative site (§2's table), so `Array<Type>`, `Dictionary<Type, int>`, `Counter<Type>` and an `OwningDictionary<Type, V>` at a sendable `V` are sendable. §28.11 already classes `Type` as a shared-reference slot, so no slot semantics change. The `Spawn` and `Channel` rows that drop the `owned` modifier for "value-typed `T` and `string`" name `Type` too; it binds exactly as `string` does, and `owned` at a `Type` substitution is erased by §28.1 as at any runtime-managed substitution.
3. *The lazily materialized `EnumNames`/`EnumValues` views.* §2.13 gains the tolerance note: each view is materialized once per `Type` instance, and the runtime's materialization and reads tolerate concurrent access from several domains — the `string` implementation note's shape, carried into §30.14's parenthetical.

Consequences stated: a record with a `Type` field (`record Msg(Kind: Type, N: int)`) is sendable (§9.7 already makes a runtime-managed field borrow-neutral; the per-component condition now holds). So is a transfer-closed declared class with a `Type` field: rule 2 reads ▲ "`string`- or `Type`-typed" and rule 3's non-`owned`-class-field bar ▲ excepts `Type`-typed fields — without the two words a `sealed class Msg` with a `Type` field could never be sendable (`owned Type` is inadmissible, §28.5), while the record carrying the same field is; and the value bullet's remark that a sendable-class-typed field "is still a borrow" ▲ excepts the runtime-managed `Type`. Not widened: `FrozenArray<T>`'s compiler-intrinsic element restriction, which is its own rule and has no use at `Type`. Not decided here: whether `Type` should carry the **immutable** fact so a `static readonly` `Type` field is warning-free under §22.12 rule 4 (today it takes the class-type row's *warn*); that is a facts ruling under stdlib §1's discipline and is filed as a question (§13).

---

## 6. A compiler-known base's inherited state (#5605)

With §3 in place the only compiler-known bases are the opened ones, and a `sealed class E: Exception` (or `: Stream`, `: Encoding`, `: TextWriter`) is a declared class under the transfer-closed rules. Rule 2 says "every instance field, including inherited fields" — and the base shows none, because stdlib §1 is not normative for a compiler-known class's internal data structures. The compiler, reading nothing, computes such a class sendable; the issue's recommendation is taken instead: ▲ rule 2 gains "The inherited state of a **compiler-known** class among the class's bases, direct or indirect (§12.4's opened classes), is not read as fields (stdlib §1): it satisfies this rule only where the base's own section states its inherited state transfer-compatible, and fails it otherwise." No section states it in this revision, so **a class deriving from a compiler-known class is not sendable**; stdlib §1 says so in one sentence, and each opened section says it beside its opening sentence. §30.14's *Faults* and stdlib §6.8 tighten "exception classes are not in general sendable" to "no exception class is sendable", which is now simply true (the built-in ones are unstated; a declared one derives from a compiler-known base).

Declined for now, recorded `deferred` (§10): stating `Exception`'s inherited state transfer-compatible. Its surface *is* transfer-compatible by structure (`Message`, `StackTrace`: `string`; `CauseChain`, `SupersededChain`: `Array<ExceptionInfo>` over a three-`string` record), so a sealed user exception with sendable fields could cross. But a caught exception is a borrow (§31.1), so the only owned exception that could cross is one constructed and never thrown, and faults already cross as `DomainFaultedException`; nothing needs it, and it is one sentence in §31.4 when something does. The recorded declines are met, not reopened: *Sendability requirement on throwable types* declined *requiring* throwables to be sendable; *Subclass-must-be-sendable* declined a base's sendability *obligating* its subclasses; this clause only lets a subclass's verdict read its base's state, as rule 2 already does for declared bases.

---

## 7. The retained comparer (#5604): the comparer-held clause

The text fixes every premise (the parameter is a borrow; the retention is observable; the statement classifies the type) and states an unsound verdict; no reading repairs it. Of the issue's options:

- (i) a statement for the comparerless constructors only — construction is not in the type; not a type-level rule (`rejected`).
- (ii) an `owned` comparer of a sendable class type — not checkable as worded (an `owned IEqualityComparer<K>` is interface-typed, never transfer-compatible); the checkable form is a comparer type parameter (`Dictionary<K, V, C>`, or a constructor constrained `where C: IEqualityComparer<K>, sendable`), an API redesign across four types (`deferred`, with the bar "a program whose comparer-built collection must cross").
- (iii) **taken**: a trust obligation in §28.3 clause 4's style. K4.3 rule 3 — *recorded trust, or nothing* — is explicit that an admission the domain clause reaches must either reject statically or carry "an individually recorded trust clause", and names the recorded set as enumerable; this adds the **comparer-held clause** to that set, beside field-held, collection-held and frame-held borrows, and to the trust-admission report's register (the constructor call is the site).
- (iv) a capture-free or stateless comparer — fails: a stateless comparer is still an object its origin domain disposes, so the cross-domain borrow dangles whatever its state (`rejected`).

The clause's home is stdlib §4 (▲ *A retained comparer is a borrow*): "while it retains the comparer it is not transferred to another domain, and not consumed into a construction other domains reach (a `Locked<T>` or `RwLocked<T>` interior, a `Shared<T>` payload). A violation is the data race the clause names; checked disposal mode cannot catch it, and the trust-admission report names the constructor call. The comparerless constructors retain nothing." §30.14's indirect-alias paragraph names it among the pre-transfer aliases the static guarantee deliberately does not cover; §4.8's *Stability while stored* paragraph points at it so a reader of the constructor finds it. `SortedDictionary`/`SortedSet` take no comparer and are unaffected. No verdict changes: this is the defect fixed by recording what the forms' justification already assumed.

---

## 8. Composition

- §3 is the premise of §4 (three more statements add nothing to an unsound set once the set is sound) and of §6 (the only compiler-known bases are now the opened ones, so rule 2's clause has a closed domain).
- §4's ground and §5's `Type` touch different carriers: the ground is a stdlib-statement form; `Type` is classified by the spec's runtime-managed bullet, and §1's new sentence says which types go where.
- §5's widening of "sendable-value or `string`" and §4's `OwningBuffer<T>` sentence edit the same stdlib §4 paragraph; the paragraph is rewritten once (`dS#5578 stdlib §4 statement`), with §7's clause appended as a second paragraph.
- §7 adds a trust clause and §3/§6 add rejections; none of the three changes what the other admits. A comparer-built `Dictionary<Type, int>` is sendable by type (§5) and bound by the clause (§7): the two sentences sit in one paragraph.
- v0.326 lands first and re-traces the guides; this proposal edits no guide (the two guide sentences it would change are listed in the lane note for the coordinator) and none of §28.3's *Bare-type-parameter returns* clauses.

---

## 9. The normative edits

The exact `old → new` edits are `scratch/v0327/dS.json` (54 edits across six files after the critic's fixups, `dS_work/build.py`'s *CRITIC FIXUPS* block; built by `scratch/v0327/dS_work/build.py`, which asserts each `old` unique at `b27728a`; dry run 54/54 clean; `check_docs.sh` (full) and `check_proposals.sh` OK on a scratch copy with them applied). By document:

- **Spec** (19): §12.4 (the rule); Appendix C.1 (one row); §31.4 (the exception family opened); §9.7 (two example lists name `Type` among the runtime-managed field types — a clarification §5 relies on); §30.14 — the **sendable** form's fourth ground, the conditional form's `OwningBuffer<T>` and `Type` wording, the "a statement stands for every instance" sentence, the runtime-managed-type sentence, the `string`-and-`Type` bullet, rule 2's compiler-known-base clause, rules 2 and 3 and the value bullet at a `Type` field, *Faults*, the indirect-alias paragraph's comparer; §28.3 K4.3 rule 3's recorded clauses and the trust-admission register; §32.10's cross-domain bullet; §30.15 `Send`.
- **Stdlib** (23): §4.1's reference-implementation sentence ("extend it" means extension blocks, not a subclass); §1 the *Extensibility* row and the *Sendability statements* sentences; §2.7, §3.1, §8.1 the three statements; §2.13 the `Type` statement and the views' tolerance note; §3.2, §7.1, §7.4 the opened classes' sentences; §4 the statement rewrite and *A retained comparer is a borrow*; §4.8 the pointer; §4.14 `DangerousPointer()`; §4.20, §6.11 (×2), §6.8 (×2), §6.9 the `Type` and faults phrases.
- **Rationale** (2): §12.6 and §12.13 v0.327 addenda carrying the decisions and declines.
- **Code-generation reference** (8): `Random`, `StringBuilder`, `Stopwatch`, `Type` index lines; the buffers lead; the `Dictionary` constructor comment; two §7.6 *Looks right, is rejected* lines and the note that follows them. No pin in prose (its pin moves at composition).
- **Proposals folder** (6): the README row; `DECLINED.md` — three `rejected` rows, two `deferred` rows, the v0.325 row annotated taken (the index-size line is recomputed once by the coordinator: +3 `rejected`, +2 `deferred`).

Not edited: the changelog (the coordinator composes the single v0.327 entry from the lane note's draft bullets); the guides (listed for the coordinator); `FrozenArray<T>`; §22.12.

---

## 10. Declined alternatives and their rows

| Alternative | Class | Reopening bar |
|---|---|---|
| (#5613) Sealing implicitly only the classes a sendability statement names | `rejected` | —(none stated). |
| (#5613) The runtime send-check | already `deferred` (rationale §12.6); unchanged | "if sealing proves too narrow". |
| (#5614) The copy-state ground without its no-address condition (option (b)) | declined in the rationale addendum, no row | — |
| (#5578) Keeping `OwningBuffer<T>` at a value element not sendable | declined in the rationale addendum, no row (the v0.323 keep was explicitly provisional) | — |
| (#5615) Keeping `Type` not sendable; a fifth statement form for shared-reference crossing | `rejected` | —(none stated). |
| (#5605) Stating `Exception`'s inherited state transfer-compatible now | `deferred` | a program that needs to send a constructed exception value. |
| (#5604) A statement for the comparerless constructors only; a capture-free or stateless comparer | `rejected` | —(none stated). |
| (#5604) A comparer type parameter as the static bound | `deferred` | a program whose comparer-built collection must cross a domain. |
| (#5606, v0.325) The three statements deferred | the existing `deferred` row, annotated **taken at v0.327**; it stays as the record of the deferral | met. |

---

## 11. Migration (the v0.327 entry's note, this lane's part)

- **Newly rejected (spec-ahead-of-impl).** A user class deriving from a compiler-known class other than `Encoding`, `Stream`, `TextReader`, `TextWriter`, `Exception` or an exception type (`class E: Mutex`, `: StringBuilder`, `: Array<int>`, `: FileStream`; §12.4). A class deriving from one of the opened classes at a sendable position (`sealed class E: Exception` with `string` fields as a spawn argument, a channel element or a `Locked<T>` interior; §30.14 rule 2). Measured reach: 0 sites in `tests/` and `docs/`. The compiler: the first is unverified (the probe is named); the second is admitted today (`Sendability.cpp:202–225`).
- **Newly accepted (spec-ahead-of-impl).** `StringBuilder`, `Random`, `Stopwatch` and `Type` at every sendable position; `OwningBuffer<T>` at a sendable value element; `Array<Type>`, the borrowing maps and sets at `Type` elements, `Counter<Type>`, an `OwningDictionary<Type, V>` at a sendable `V`, a record with a `Type` field, and a transfer-closed class with a `Type` field. The compiler rejects all of them today (`Sendability.cpp:371, :398–400, :522`). The unwritten v0.323 corpus case "`OwningBuffer<double>` as a `Channel<T>` element (reject)" is now an accept.
- **Newly defined.** The comparer-held clause (no verdict change; a recorded obligation, and a trust-admission report site). The opened set, stated once (stdlib §1 *Extensibility*; §31.4).
- **Doc-only.** The rationale addenda; the code-generation reference's lines; `DECLINED.md`'s rows.
- **Unchanged.** `FrozenArray<T>`'s element restriction; §22.12's verdict for a `Type`-typed static; `Span<T>`/`ReadOnlySpan<T>` (borrow-bearing, not sendable); the built-in exception types (unstated, not sendable); `Shared<T>`'s payload set.

---

## 12. Trackers to file at landing (compiler)

1. **Reject a base-type reference to a non-opened compiler-known class** (§12.4, v0.327): probe `class E: Mutex end E;` first; if admitted, the tracker; the diagnostic names §12.4 and the opened set. `TypeResolution_Names.cpp:1197–1213` is where the base is recorded.
2. **Sendability of the new statements** (§30.14, stdlib §2.7/§2.13/§3.1/§8.1/§4, v0.327): `StringBuilder`, `Random`, `Stopwatch`, `Type` sendable; `OwningBuffer<T>` at a sendable value element; the borrowing collections, `Counter<K>` and `OwningDictionary<K, V>` keys at `Type`; a record or inline array with a `Type` component; a `Type` field of a sealed class (the class arm at `Sendability.cpp:615–629` hands a builtin `Type` field to `isSendableType`, so the builtin-arm change carries it). Rows on #5587 (the permissive direction), or one tracker naming each arm of `Sendability.cpp`.
3. **A class deriving from a compiler-known base is not sendable** (§30.14 rule 2, v0.327): `forEachInheritedStorageMember` must fail the walk at a builtin base rather than skip it. The unsound direction — its own tracker, as #5597 was. Test: `sealed class E: Exception` with one `string` field, as a `Domain.Spawn` argument and as a `Channel<E>` element, expecting "not sendable".
4. **The trust-admission report** names a comparer constructor call (§28.3, v0.327): a row on the report mode's tracker (the mode itself is unimplemented).

Issues this landing closes with the citation: #5613, #5614, #5578, #5615, #5605, #5604.

---

## 13. Open questions surfaced (filed, not decided)

- **Filed, not decided:** should `Type` carry statics-and-sharing facts (stdlib §1), so a `static readonly` `Type` field is warning-free under §22.12 rule 4? Two routes exist and they differ, so it is not decided here. (1) Grant **immutable** (deeply immutable; the enum views are a tolerance note, not observable state) together with **self-distributing** (every domain reaches the interned instance through `typeof`, the `SharedEventSource<T>` ground) — rule 4's warning-suppression set gains `Type`, the `Shared`-payload set does not (it could not take an `owned Type` payload anyway), and the derived-set parentheticals at spec §22.12 rule 4, Appendix C and §28.13 plus `check_docs.sh` 3b's roster move. (2) List `Type` beside `string` in §22.12 rule 3's *immutable content*, matching this proposal's placement of `Type` with `string`, with rule 4 then excepting it from "class type". The critic recommends (1): the facts are the library's single carrier for a class's statics treatment, and a facts ruling must confront each derived set, which (2) skips. Low priority: `typeof(X)` can be written wherever such a static would be read.
- **Decided (status quo, under the delegation):** `FrozenArray<T>` does not admit `Type` elements. Its element restriction is its own compiler-intrinsic rule, untouched by this proposal; no program needs it (`Array<Type>` is sendable, and `Shared<FrozenArray<T>>` serves bulk zero-copy reads at its admitted elements). No row: no alternative was argued.
- Does the compiler admit `class E: Mutex` today (tracker 1's probe)? If codegen lays such a class out, the rejection is also a behaviour flip for any program that compiled by accident; none is in the repository.

---

## 14. Self-critique (adversarial pass)

1. **Is (i) a design change dressed as a defect fix?** Yes, and it is written as one: the rule changes what compiles. The owner's delegation authorizes it through this proposal (process §2, §6, §8). The alternative that is *not* a design change — leaving the seam — contradicts rationale §12.6's recorded reason for sealing, which is the stronger recorded decision.
2. **Does (i) break the stdlib's own future?** The proposals folder derives `MemoryStream`, `StreamReader`/`StreamWriter`, `GzipStream`, `NetworkStream` from opened bases only. A future library class that wants user subclasses opens itself with one sentence; nothing forecloses that.
3. **Could an opened base reintroduce the seam?** Only if a subclass of an opened base were sendable with inherited state the rules cannot see. §6 closes exactly that: a class deriving from a compiler-known base fails rule 2 until the base's section states its inherited state transfer-compatible, and no section does.
4. **Is the copy-state ground a loophole for a future type that retains a borrow?** The ground is stated by member behaviour — "retains no borrow past its call" — and each statement must justify it from the type's members (stdlib §1: "justified by the type's internal structure"). A type with a callback or a retained reference does not meet it. `ScopeGuard` and `Subscription` are the nearest unstated types; neither qualifies (a retained callback; a registration) and neither is given a statement.
5. **Is `OwningBuffer<T>` at a value element really the same shape as `Array<double>`?** Both are wholly-owned copy state crossing by move; the difference is `DangerousPointer()`, whose hazard — a kept address outliving the storage's owner — exists without any transfer and is §32.10's recorded trust in both cases. The extension is stated in §32.10, not hidden in the ground.
6. **Does `Type` as a shared reference add sharing?** No: every domain already reaches the interned instance through `typeof`; what changes is that a *value* holding one may now cross. The enum-view materialization is the one runtime state the sharing touches, and it gets the implementation note `string` has.
7. **Does the comparer clause change a verdict silently?** No verdict changes; the admission existed and was unrecorded, which K4.3 rule 3 forbids. The clause is recorded at the stdlib statement, in §30.14's alias paragraph, in K4.3 rule 3's enumerated set and in the report's register — the four places the other recorded clauses live.
8. **Absence claims (process §5 item 3b).** "No test derives from …", "no test sends …", "no section states its inherited state transfer-compatible" were each checked by grep over `tests/`, `src/` and `docs/` (§2). "The compiler does not reject `class E: Mutex`" is *not* asserted; it is recorded as unverified with its probe.
9. **History narration (process §7).** The normative text states current rules only; the story is in the rationale addenda and the changelog. `DECLINED.md`'s taken row says "taken at v0.327", which is a pin, not narration.
10. **Citations.** Every `§` in the new text was checked against the heading at `b27728a`: §12.4 *Sealing*; §28.9 *Runtime-managed types*; §30.13 *Runtime model*; §30.14 *Multiple domains and sendable types*; §31.1 *Throwing and catching*; §31.4 *Standard exception types*; §32.10 *Boundary contract*; §32.12 *Foreign memory*; stdlib §2.7, §2.13, §3.1, §3.2, §4.8–§4.10, §4.14, §4.19, §4.20, §6.8, §6.9, §6.11, §7.1, §7.4, §8.1; rationale §12.6, §12.13, §17.14. Every link target is an existing `<a id>`; `check_docs.sh --quick` on a scratch copy with the edits applied reports the result recorded in the lane note.
