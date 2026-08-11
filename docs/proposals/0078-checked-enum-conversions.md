# Proposal 0078 — Checked enum conversions: `E(n)` faults on undeclared values, with a nullable-return tier

| | |
|---|---|
| **Status** | under-review — drafted 2026-08-11 (owner-directed, from the elegance review) |
| **Tracking issue** | [#3044](https://github.com/s8lap/bezel-lang/issues/3044) |
| **Target module(s)** | **spec** — §5.4 (the rule change; both subsections), §5.3 (one composing sentence), §10.4 (the defensive-`_` paragraph deletes), §17.11 (the Q3 rider), §17.14 (no change — enum casts are already non-constant; the §5.4 constant-operand error extends), §19.6 (one sentence stating the restored guarantee), §21.7 (the statement-`case` totality rule's enum disqualifier — its recorded ground is exactly the premise this proposal deletes; Q5), Appendix C.1/C.7 (invariant rows). **stdlib** — §9-adjacent: the compiler-provided per-enum static member (declared in spec §10.4's static-surface terms, documented beside §1's tier conventions). **rationale** — a new decision-record subsection (the v0.72 entry documented the C#-inherited behavior; nothing ever weighed it). **guides** — Guide 3 wording only (the raw-value caveat retires). |
| **Written against** | spec/stdlib v0.241 |
| **Origin** | The 2026-08-11 elegance review (front-half critic, finding 1, confidence high; coordinator Tier 1 item 1), owner-accepted. The finding: `E(n)` is the one unchecked conversion in a checked language. |

*Reference convention: unprefixed §-references are language spec references; stdlib, rationale, and guide references are marked.*

---

## 0. Thesis

Every other conversion in Bezel is checked or rejected: integer narrowing raises `OverflowException` (§5.2), constant narrowing out of range is a compile error (§5.1), `bool(n)` does not exist (§5.2), float-to-integer checks (§5.2), and even the enum cast's *range* half is checked — `E(n)` outside the underlying type's range is a compile error for constants and `OverflowException` at runtime (§5.4). The one exception is the enum cast's *declaredness* half: `E(n)` for an in-range `n` matching no declared constant "succeeds and produces a 'raw' enum value that is not equal to any declared constant" (§5.4), with the burden pushed to the call site ("the call site is responsible for validating").

The cost is not hypothetical; the documents pay it three times:

- **§10.4 defuses it inline.** The spec's own `Name` property example carries a paragraph explaining that a raw value reaching the property raises `MatchFailureException` and advising a defensive `_ => "Unknown"` arm — advice that directly contradicts Guide 3 §2's teaching ("A `_` arm silences the compiler's help when the type grows", Guide 3 line 66).
- **§9.8 rejects raw values on both directions.** The synthesized enum `Serialize` raises `SerializationException` for a value that is not a declared constant, and `Deserialize` accepts only declared names. The project's own serialization design refutes §5.4's stated use case for raw values ("useful when round-tripping through serialization").
- **§19.6's enum-exhaustiveness proof is unsound at runtime.** Coverage is "over the set of distinct declared *values*" — a proof over a value space the cast can escape. A statically-exhaustive `case` over an enum can raise `MatchFailureException` today.

**This proposal makes `E(n)` checked**: a value matching no declared constant faults — compile error when the operand is a §17.14 constant, runtime fault otherwise — and adds the branch-shaped form as a compiler-provided static member per enum, following stdlib §1's access-tier conventions. The investigation of the flags question (§4) found **no flags-enum idiom exists** in the language or library, so the checked rule is total, with no unchecked side door; a named unchecked entry point is recorded liftable behind evidence.

The payoff is two invariants restored by deleting a carve-out: *conversions are checked* becomes true with no exception, and *enum exhaustiveness is real* — every runtime value of an enum type is a declared value, end to end (with one rider, Q3, closing the last producer). The spec gets shorter: §5.4's validate-it-yourself paragraph, §10.4's defusing paragraph, and Guide 3's caveat all delete.

## 1. The current state, precisely

- **§5.4 (v0.241):** "Casting an integer that does not correspond to a declared constant produces a value not equal to any declared constant; the call site is responsible for validating." The out-of-range subsection: constants are compile errors; runtime out-of-range raises `OverflowException`; in-range-undeclared "succeeds and produces a 'raw' enum value … useful when round-tripping through serialization, dangerous when used as if it were a declared constant."
- **§10.2:** an enum value *is* its underlying integer; same-valued constants denote one value.
- **§19.6 (enum bullet):** exhaustiveness "is over the set of distinct declared *values*."
- **§9.8:** synthesized enum serialization writes names, and "a value that is not a declared constant (a 'raw' enum value, §10) raises `SerializationException` on write and on read."
- **Rationale:** silent. The v0.72 changelog entry *documents* the behavior as a gap-fill ("within-range values that don't correspond to a declared constant produce raw enum values that the consumer must validate"); no rationale section weighs it against the checked posture. This proposal's rationale subsection is that missing record.

## 2. The proposed rule

**§5.4 replacement text (sketch, ready for amendment):**

> An integer-to-enum cast `E(n)` is **checked against the declared constants**: it succeeds exactly when `n` equals the underlying value of some declared constant of `E`, and yields that value. When `n` is a compile-time constant (§17.14) equal to no declared constant, the cast is a **compile error at the cast site** — the same posture as the out-of-range constant error above, and as §5.1's constant-narrowing range error. When `n` is not a constant, a value equal to no declared constant raises `OverflowException` at runtime, the diagnostic naming the enum type and the value. There is no value of an enum type that is not equal to a declared constant. (Under duplicate constant values, §10.2, the declared-value set is the set of distinct values; `E(1)` where `Legacy := 1, Current := 1` yields that one value.)

Notes on the pieces:

- **The range check subsumes.** In-range-vs-out-of-range stops being a user-visible distinction: both are now "not a declared value," and both raise `OverflowException` (constants: compile error). The §5.4 out-of-range subsection folds into the single rule; its two-step `int(p)` widening text for non-`int` underlying types is unaffected (that direction, enum-to-integer, is total and stays total).
- **§5.3 composes unchanged.** `E?(x)` already inherits "the corresponding non-nullable conversion's own semantics, including its runtime checks (`OverflowException` on a non-fitting present value)" — the checked rule rides that sentence as written; one cross-reference is added.
- **Fault type: `OverflowException` (Q1).** Argued in §5 below.
- **Enum-to-integer (`int(p)`) is untouched.** With the cast checked, `int(p)` can only ever see declared values, so its contract is unchanged and its §5.4 subsection survives verbatim.
- **Operator overloads on enums are unaffected at compile time.** §10.4 admits operator overloads; an overload body that constructs its result via `E(...)` now carries the checked semantics inside the body (see §4 — this is the one flags-shaped corpus artifact).

## 3. The branch-shaped tier: `FromUnderlyingOrNull`

Boundary code (wire parsers, FFI ingestion — enums are not FFI-compatible types, §32.5: the boundary declares the underlying integer and casts, so the cast *is* the ingestion point) needs a non-throwing form. Proposed: a **compiler-provided static member on every enum type** (joining §10.4's static surface the way §11.8's compiler-provided conformances join primitives — no declaration written, no user override):

```bezel
// PROPOSED — compiler-provided on every enum E with underlying type U (§10.3):
public static method FromUnderlyingOrNull(value: U): E?;
    // The declared constant whose underlying value equals `value`, or null
    // when there is none. Total — never raises. Under duplicate constant
    // values (§10.2) the result is the shared value (all aliases denote it).
    // The parameter type is the underlying type U exactly: a wider integer
    // in hand is narrowed explicitly first (§5.2, checked), keeping the two
    // failure modes — out of U's range vs. not a declared value — separately
    // visible, per the checked posture.
```

**Why `…OrNull`, not `TryFromUnderlying` (a recorded deviation from the review's sketch).** The critic (and #3044) sketched `TryFromUnderlying(n): E?`. Mirroring stdlib §1's conventions *exactly* points the other way: a nullable return with no out parameter is the **`*OrNull` convenience tier** (v0.231 — `ParseOrNull`, `GetOrNull`), band 2's nullable-return value shape; the `TryX(...): T?` spelling (shape (b)) is "reserved for acquisition-shaped members whose payload is statically non-nullable (`TryAcquire`, `TryUpgrade`)" — and a conversion is not acquisition-shaped. An enum payload is a non-nullable value type, so `E?` carries the whole signal with no §7.10 conflation caveat; the OrNull tier fits with nothing lost. The three access tiers then read exactly as §1 teaches them: `E(n)` is the throwing form ("absence is a bug here"), `E.FromUnderlyingOrNull(n)` the branch form; no Try shape is needed because there is no outcome a collapsed `E?` cannot distinguish. Q2 puts the spelling to the owner with this recommendation.

**Call shapes, before and after** (a protocol parser reading a `byte`-backed `Priority` off the wire):

```bezel
// TODAY — the §5.4 validate-it-yourself contract, usually not written:
var p := Priority(byteFromWire);          // silently mints an unnameable value
case p of Low => ..., Medium => ..., High => ... end;   // statically exhaustive (§19.6),
                                                        // MatchFailureException at runtime

// AFTER — the boundary states its policy in one line each way:
var p := Priority.FromUnderlyingOrNull(byteFromWire)
         ?? throw new ProtocolException($"bad priority {byteFromWire}");   // reject
// or tolerate-with-default:
var p2 := Priority.FromUnderlyingOrNull(byteFromWire) ?? Priority.Low;
// or let the checked cast be the assert (failure is a bug):
var p3 := Priority(trustedByte);          // OverflowException on corruption — loud, at the source
```

Traced: `FromUnderlyingOrNull` returns `Priority?`; `??` with a `Priority` right operand yields `Priority` (§7.7); the `throw` right operand is §31.5's expression form. No stdlib API beyond the proposed member is used (`ProtocolException` stands for a user exception class).

## 4. The flags question, investigated

**Does Bezel have a flags-enum idiom today? No.** Checked against §10 and the stdlib:

- §10 defines no flags mechanism — no attribute (Bezel has none), no modifier, no `|`-on-enums intrinsic (§6.5's bitwise operators are "not valid on `bool`, `char`, or floating-point types" and operate on integers; enums are not integers without a cast).
- The stdlib contains **zero** flags-style enums: `grep -in flags Bezel_Standard_Library.md` → 0 hits; no stdlib enum declares a `|`/`&` operator overload (0 hits); the one .NET-flags-shaped surface, `FileAccess` (stdlib §7.2), spells `ReadWrite` as a **distinct declared constant** (`Read, Write, ReadWrite` — values 0, 1, 2 by auto-increment), not a bitwise union.
- The corpus contains exactly **one** flags-shaped artifact: `tests/lit/sema/compound_assign_named_target_2162__enum_overload_accepted.bzl`, a #2162 over-rejection guard whose test enum declares `operator |(a, b) => Priority(int(a) | int(b))`. It is a sema-only test (compiles, does not execute), and its two compositions — `p |= Priority.High` (`Low | High` = 0|2 = 2, the declared `High`) and `p += 0` through its declared `operator +` (0+0 = 0, the declared `Low`) — both land on declared values, so it compiles and behaves identically under the checked rule. But the *pattern* it exemplifies — hand-rolled flag unions through a raw cast — is exactly what the checked rule forecloses at runtime for undeclared unions.

**Decision: the checked rule is total; no unchecked entry point ships.** Reasons: (a) no library or corpus surface needs one; (b) a flags *set* is honestly an integer with named masks — `const` masks over `int`/`byte` express it today with full bitwise support and no carve-out, and §17.14's own example is precisely that shape (`const Flags: long := (1 << 10) | (1 << 3);`); (c) small closed combination sets are declared constants (`ReadWrite` is the stdlib's own precedent); (d) an unchecked side door added *now*, against zero demand, would be the §18-style speculative surface this project declines. **Recorded liftable:** if a real flags workload arrives, the shape to lift is an explicitly-named `E.FromUnderlyingUnchecked(value: U): E` — loud at the call site, greppable, never the default spelling — or a first-class flags declaration form; that choice is deferred with the evidence. Q4 confirms this posture.

## 5. The fault type (Q1)

For the non-constant fault, two candidates were weighed against §31.4's taxonomy and §6.1's precedent:

- **`OverflowException` — recommended.** §5.2 already uses it for every "value not representable in the destination type" conversion failure, and §5.4's own range half uses it today; the checked rule reads naturally as extending "the destination type's value set" from *range of U* to *declared values of E*. §5.3's nullable-cast composition sentence already names it. One conversion-fault family, no new type, and an existing `ArithmeticException`/`OverflowException` arm at a boundary catches it.
- **A dedicated subclass** (e.g. `InvalidEnumValueException: ArithmeticException`). §6.1's precedent cuts both ways: `DivideByZeroException` shows that an *operand-domain* error of a built-in operation can earn its own type. Declined for economy: no plausible handler branches on enum-cast-vs-overflow (both are "the input was bad" at a boundary), and §31.4's built-in list grows only for faults with distinct handling.
- **`ArgumentException`** was rejected outright: §31.4 and stdlib §1 band 1 make `Argument*` the defect surface of *method-call* boundaries (`ParamName` has no referent at a cast expression), and no §5 conversion uses it.

## 6. What this restores, and the one rider (Q3)

With casts checked and §9.8 already rejecting raw values on both directions, the producers of enum values are: constant references, checked casts, `FromUnderlyingOrNull`, deserialization — and **`default(E)`**. §17.11 today produces "the value with underlying integer 0 (which need not be a declared constant)" with a **warning** when no zero constant exists — the last remaining mint of an unnameable value (reached also through `where T: new()`'s value arm, §14.2, and default initialization of enum-typed storage, §7.9/§15.1).

**Q3 rider (recommended: adopt):** escalate §17.11's no-zero-constant warning to a **compile error** — `default(E)`, `new T()` at such a substitution, and default-initialized enum storage are rejected for an enum with no declared zero-valued constant; the remedy is declaring one (one line, and almost every practical enum already starts at 0). With the rider, §19.6's enum bullet gains the sentence this proposal exists for: *"Because every producer of an enum value yields a declared value (§5.4, §9.8, §17.11), a statically exhaustive enum `case` cannot raise `MatchFailureException`."* Without the rider, the sentence carries a `default(E)` asterisk and §10.4's defusing paragraph shrinks rather than deletes. The rider is separable; the owner can take the cast rule alone.

**The §21.7 consequence (Q5).** The restored §19.6 sentence collides with §21.7's statement-`case` totality rule, which today records the exact opposite, keyed on exactly the premise this proposal deletes: *"The **enum** constants-coverage proof never grants totality: a non-constant cast `E(n)` produces a raw value matching no declared constant ([§5.4]), so the no-match fall-through of an enum `case` statement is reachable and a trailing exit remains required"* (§21.7, v0.241). With casts checked (and the Q3 rider closing the `default(E)` producer), that sentence's stated ground is repealed. Whether the constants-coverage proof therefore joins §21.7's condition-(i) list — a statically exhaustive enum `case` statement counting as exiting on every path, no trailing `return` — or §21.7 keeps its trailing-exit requirement on some other ground is put to the owner as Q5 (recommended: grant totality — the recorded disqualifier was the raw value, and the raw value is gone).

## 7. Corpus evidence and migration

Greps at v0.241:

- **In-range-undeclared casts: zero.** No test, conformance case, or doc example constructs an in-range undeclared enum value and relies on its acceptance. The `enum_cast_const_range_859` lit family pins the *range* half only (`Color(300)` on `byte` rejected; the accepted case `Color(2)` names declared `Blue := 2` — survives). The conformance mirror `spec_0005.bzl`'s `Priority(1)` is declared `Medium` — survives. The `enum_cast_arg_type_1171` family (operand-type checks) is orthogonal — survives.
- **The one flags-shaped artifact** (§4): compiles and passes unchanged; its pattern is foreclosed at runtime for undeclared unions.
- **`TryFromUnderlying`/`FromUnderlyingOrNull`: zero occurrences** anywhere (fresh name, no collision).
- **`default(E)`-with-no-zero-constant (Q3):** the §17.11 warning's own tests aside, no corpus enum omits a zero constant deliberately (stdlib enums all start at 0 or declare explicit values covering 0 — `FileMode`, `FileAccess`, `Priority` examples included; `HttpStatus`-style enums with no zero constant appear only in cast-error tests).

**Changelog categories.** Newly **rejected**: constant `E(n)` equal to no declared constant (previously accepted, minted a raw value); under Q3, `default(E)`/default-initialization for enums with no zero constant (previously warned). Newly **defined**: runtime `E(n)` on an undeclared value raises `OverflowException` (previously succeeded); the compiler-provided `FromUnderlyingOrNull` member. Newly **removed text**: §5.4's validate-yourself paragraph, §10.4's defensive-`_` paragraph, Guide 3's raw-value caveat. Doc-only: the rationale decision record; §19.6's restored-guarantee sentence.

## 8. Alternatives considered

1. **Status quo, recorded.** Write the missing rationale entry defending raw values. Declined: the defense does not exist — the stated use case (serialization round-trip) is refuted by §9.8's own design, exhaustiveness stays false, and the spec keeps paying the §10.4 teaching tax.
2. **Keep raw values; make `case` total over the underlying type.** Forces a `_` arm on every enum match — regresses the language's best exhaustiveness story to C#'s. Declined.
3. **Validate at *use* instead of at the cast** (raw values flow; comparisons/`case` fault on them). Moves the fault away from its cause, makes `=` partial, and costs a check per use instead of one per cast. Declined.
4. **A `bool`+`out` Try shape instead of the OrNull tier.** Shape (a) exists for payloads where null is a legitimate payload value; an enum payload can never be null, so the bool adds ceremony and no information — stdlib §1's own reasoning for shape (b)/OrNull. Declined; see §3.
5. **An unchecked named entry point now.** Declined for zero demand (§4); recorded liftable.

## 9. Implementation sketch

- **sema** (`src/sema/ExprTypecheck*`, `ConstFold.cpp`): the enum-cast path gains the declared-value check for constant operands (the declared-value set is already in the enum symbol for §19.6/§10.2 duplicate handling); diagnostic text: `` enum cast `Priority(7)` — 7 is not the underlying value of any declared constant of Priority ``. The Q3 rider extends the existing §17.11 warning site to an error. Small: the constant-range check (#859) is the template.
- **codegen** (`src/codegen/`): non-constant casts emit a declared-set membership test before the existing range check — a range test for dense value sets (the common 0..k-1 case: two compares), a small switch for sparse sets — branching to the existing throw path. `FromUnderlyingOrNull` lowers as the same test yielding `null` instead of throwing (compiler-known member, the §15.4.1/`Parse<T>`-style dispensation).
- **tests**: spec-cited regression tests per `tests/README.md` — constant undeclared cast rejected (sema/lit, citing §5.4 vX); runtime undeclared cast raises `OverflowException` (codegen, `runAot`); declared and duplicate-constant casts accepted; `FromUnderlyingOrNull` hit/miss/duplicate; §9.8 round-trip unchanged; Q3's `default(E)` rejection; and the restored §19.6 guarantee (exhaustive enum `case` over a wire-ingested value cannot mismatch).
- **Size**: small-to-moderate — two checked paths in codegen, one sema check, one compiler-known member; no grammar change (Appendix A untouched; the cast is ordinary call syntax, the member ordinary static-member syntax).

## 10. Questions for the owner

- **Q1** — adopt the checked cast with **`OverflowException`** as the runtime fault (recommended), or a dedicated `ArithmeticException` subclass (§5's declined alternative)?
- **Q2** — the tier member's spelling: **`FromUnderlyingOrNull(value: U): E?`** (recommended — the stdlib §1 OrNull tier's exact shape; a recorded deviation from the review's `TryFromUnderlying` sketch, reasons in §3), or `TryFromUnderlying` as sketched?
- **Q3** — the `default(E)` rider: escalate the §17.11 no-zero-constant warning to an error, completing the every-value-is-declared guarantee (recommended), or keep the warning and state the guarantee with the `default(E)` asterisk?
- **Q4** — confirm the no-unchecked-entry posture (recommended; `FromUnderlyingUnchecked` recorded liftable behind flags-workload evidence)?
- **Q5** — §21.7's statement-`case` totality rule disqualifies the enum constants-coverage proof on the ground that a non-constant `E(n)` can mint a raw value (§6). With casts checked and the Q3 rider taken, does the constants-coverage proof now grant statement-`case` totality (recommended: yes — its recorded disqualifier is repealed, and §21.7's enum sentence rewrites to say so), or does §21.7 keep its trailing-exit requirement on other grounds?

## 11. Interactions

- **#2244 (`Enum.Parse<E>`, routed to its own proposal):** the string→enum tier. Its design should mirror this proposal's integer tier (`Parse` throwing / `ParseOrNull`); §9.8's name matching is the shared substrate. No dependency either way.
- **Proposal 0089 (generic-leaves study, #3055):** independent, but both serve §19.6 — this proposal makes the *enum* bullet's proof sound at runtime; 0089 questions the *class-hierarchy* bullet's generic machinery. A joint changelog entry could state §19.6's guarantees per scrutinee kind in one place.
- **Proposal 0086 item 5 (§10.6 prefix-omission record, #3052):** touches the same §10 chapter text; no rule overlap.
- **Proposal 0080 (`Exception`-derived throwables, #3046):** the §31-side non-`Exception`-throwable work; it shares §31.4's taxonomy discussion with Q1 here — coordinate the fault-type vocabulary at graduation if both land in one revision.
- **Proposal 0079 (shift-count domain, #3045):** its Q2 fault-type choice and Q1 here are one ruling — 0079 §4 records that the choice "should be made consistently with 0078's Q1 — the two proposals should land with one answer" (and 0086 item 1's range-step fault is a third site of the same ruling).
