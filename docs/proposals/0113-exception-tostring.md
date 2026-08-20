# Proposal 0113 — `Exception.ToString()`: giving §31.4's rendering promise a declaring member

| | |
|---|---|
| **Status** | under-review — draft (2026-08-20) — authored in the v0.266 ruling lane for [#3259](https://github.com/s8lap/bezel-lang/issues/3259), which the same revision records as an open boundary at §31.4 |
| **Tracking issue** | [#3259](https://github.com/s8lap/bezel-lang/issues/3259) |
| **Target module(s)** | **spec** — §31.4 (*The base `Exception` surface*: one member added; the two rendering-promise sentences gain their declaring text), §8.5 (**no edit** — see this proposal's §5, the audit that expected one and found none needed), §33.3 and §32.8 (one clause each, stating that the reports' content is defined independently of `ToString`). **stdlib** — §2.17, at most a mirroring line; the surface is normative in the spec and §2.17 says so. **rationale** — one paragraph in §11's exception record. **guides** — 02 (Errors and Nullability), where logging an exception is taught. No language change: `Object.ToString` is already `public virtual` (§8.1) and every class already carries the slot. |
| **Written against** | spec/stdlib/rationale **v0.265** (HEAD `03c3ffc`). Every existing name cited was re-verified against that tree; every new name is marked **▲**. |
| **Currency** | v0.265 (`03c3ffc`). **Quoted:** spec §8.1, §8.5, §17.7, §22.15, §31.1, §31.2, §31.4, §32.8, §33.3; stdlib §1, §2.17, §7.4. **Cited without quotation:** spec §2.8.6, §9.2, §10.5, §18.5, §22.16, §28.9, §30.9; stdlib §2.13, §2.18. Re-verification before any ruling must cover spec §8.5 and §31.4 (this proposal's whole argument turns on them) and whichever v0.266 text lands the §31.4 open-boundary flag and §22.13's exit-time report. |
| **Origin** | [#3259](https://github.com/s8lap/bezel-lang/issues/3259), surfaced by the v0.259 gate while ruling on `SupersededChain` rendering; re-derived in the v0.266 ruling lane, which found the defect to be a **conflict** with §8.5 rather than an under-declaration, and corrected the issue's routing (the surface is normative in the language spec, not the stdlib). |

*Unprefixed §-references are **language-spec** references at v0.265; **stdlib** and **rationale** references are marked as such. References to this document's own sections are always written "this proposal's §N". ▲ marks proposed surface, landed nowhere. Every quotation is verbatim from the v0.265 snapshot.*

---

## 1. Motivation

**§31.4 promises a rendering that no member performs, and the promise conflicts with §8.5 rather than merely under-specifying it.**

The two promises, verbatim:

> …the cause *instance* is never retained … and **the full diagnostic chain survives into logs and `ToString()`**. (§31.4, cause chaining)

> Like `CauseChain`, `SupersededChain` **survives into logs and `ToString()`: an exception whose chain is non-empty renders it beside its causes.** (§31.4, supersession, v0.259)

The declaring surface, complete and verbatim:

```bezel
public class Exception
  public pure constructor();
  public pure constructor(message: string);
  public constructor(message: string, cause: Exception);   // snapshot chaining, below
  public readonly property Message: string;
  public readonly property StackTrace: string;             // formatted; "" when unavailable
  public readonly property CauseChain: Array<ExceptionInfo>;   // empty when no cause
  public readonly property SupersededChain: Array<ExceptionInfo>;   // v0.259 — empty when nothing superseded
end Exception;
```

There is no `ToString`. `Exception` therefore carries §8.5's compiler-generated class default:

> - For classes: the type's **qualified name** … The default and `FullName` are one rendering contract, not two.

**That is not latitude — it is a contract.** An implementation that rendered the chains from `Exception.ToString()` today would violate §8.5. So the two §31.4 sentences and §8.5 cannot both be satisfied: the defect is a **conflict**, not a gap, and it cannot be closed by an implementation choosing generously.

**What a user actually gets.** stdlib §7.4 fixes `TextWriter.Write(value: Object)` as *"`Write(value.ToString())`"*, so:

```bezel
try
  DoWork();
catch
  var ex: Exception => Console.WriteLine(ex);      // prints: System.ArgumentException
end;
```

— the message is gone, both chains are gone, and the program that follows §31.4's promise to the letter logs nothing diagnostic. §31.1 already routes around this, tellingly without naming `ToString`:

> To preserve an exception past the arm body — for logging, deferred reporting, or aggregation — copy the relevant data out of it (typically `ex.Message`, `ex.StackTrace` … the `CauseChain` and `SupersededChain` snapshots, and any domain-specific properties) into separate owned values.

**The chains are reachable — just not there.** Two runtime sites render them, both by reading the properties directly:

> after the runtime prints the exception's type, message, stack trace, `CauseChain`, and `SupersededChain` ([§31.4]) to standard error (§33.3, uncaught)

> the runtime prints the exception's type, message, stack trace, `CauseChain`, and `SupersededChain` ([§31.4]) to standard error (§32.8, FFI-boundary abort)

So *"logs"* is true by hand and *"`ToString()`"* is false outright. **The absence is a claim and it is discharged**: `ToString` was grepped across all four versioned documents (≈60 hits — `Convert`, the temporal types, `Guid`, `Decimal`, `StringBuilder`, `ReadOnlyStringSpan`, `JsonSerializer`, the primitives, and §8.5/§9.2 generated-member references). The only `override method ToString` spelled anywhere in the normative documents is `string`'s (stdlib §9.6, *"public pure override method ToString(): string // returns this"*), which establishes the spelling and nothing else.

**Why this is worth a proposal and not a one-line ruling.** The edit is small and the consequence is not: whatever this decides is what *every* Bezel exception log line looks like, permanently, in every program. The content set is a real fork on which the two closest languages disagree — C#'s `Exception.ToString()` carries the stack trace and the inner-exception chain; Java's carries neither the stack trace nor the suppressed list, which `printStackTrace` renders instead. And declaring the member interacts with §17.7's fatal-allocation rule on the termination path (this proposal's §5). One of the two candidate fixes is additionally a **design change by process §2's own test** — re-homing retracts v0.259's recorded *"renders it beside its causes"* — so both branches route here regardless.

## 2. The design ▲

### D1 — Declare the override on `Exception`; do not re-home the promise

▲ `Exception` declares `public override method ToString(): string`, replacing §8.5's class default for `Exception` and every type deriving from it.

The alternative — strike *"and `ToString()`"* from both sentences and point readers at §33.3/§32.8 — is **declined**, on three grounds, in strength order:

1. **It contradicts a recorded decision, which process §2 classifies as a design change on its own.** v0.259's sentence does not merely mention `ToString()`; it specifies an output shape — *"an exception whose chain is non-empty **renders it beside its causes**"*. That is a design, deliberately written, eleven months of revisions after the first promise. Retracting it is not a wording fix.
2. **It leaves the footgun.** `Console.WriteLine(ex)` and `$"{ex}"` are what users write; under re-homing they keep printing a bare type name forever, and the language has no member that renders an exception. Every log site must hand-assemble four properties.
3. **It is not smaller.** Re-homing must also amend §31.1 (which teaches the hand-copy as *the* mechanism), the guides, and any prose implying an exception renders itself — versus one declared member plus its rendering text.

**What D1 does not cost, audited rather than assumed** (this proposal's §4): no new mechanism, no vtable slot, no ABI delta, no purity reclassification, no change to any derived type's conformance.

### D2 — Content: type name, message, cause chain, superseded chain. The stack trace is **excluded**

▲ The rendering contains, for the receiver:

1. its **qualified type name** (stdlib §2.13's `FullName` rendering — the same string §8.5's default returns);
2. its **`Message`**, when non-empty;
3. every entry of its **`CauseChain`**, in the chain's own order (causal depth, outermost cause first — §31.4);
4. every entry of its **`SupersededChain`**, in the chain's own order (chronological, earliest-superseded first — §31.4), rendered **beside** the causes rather than merged with them, since §31.4 states the two chains are independent and *"neither subsumes the other"*.

Each chain entry renders its `ExceptionInfo`'s `TypeName` and `Message`.

**The stack trace is excluded, from the receiver and from the entries alike.** Four reasons:

1. **The promise does not ask for it.** Both §31.4 sentences promise the *chains*. `StackTrace` is a separate property and is never named in either.
2. **It is unspecified text.** §31.4: *"`StackTrace: string; // formatted; "" when unavailable"*. No format is stated anywhere, and *"when unavailable"* means a conforming implementation may return `""`. Putting it inside a rendering makes the rendering's usefulness implementation-dependent in a way the other three items are not.
3. **It makes the common case unusable.** A single-line `Log(ex.ToString())` becomes a multi-hundred-line dump; users then stop calling it, and the promise is functionally unfulfilled a second way.
4. **Java is the precedent that matches Bezel's shape.** Java's `toString()` is type + message; the stack trace belongs to `printStackTrace`. Bezel's structural equivalent of `printStackTrace` already exists as the runtime reports (§33.3, §32.8, and v0.266's §22.13 exit-time report), all three of which *do* print the stack trace. The split is already there; D2 makes `ToString` the short half rather than duplicating the long one.

The C# posture (stack trace included) is the recorded alternative and is **Q2** for the owner.

### D3 — Format posture: content normative, arrangement implementation-defined — and the reports are defined independently

▲ The **content** of D2 is normative; the **arrangement** (separators, ordering of the two chains relative to each other, line breaks, indentation) is implementation-defined, with two hard requirements:

- the qualified type name is always present, so the rendering is never *less* informative than the §8.5 default it replaces; and
- a non-empty `CauseChain` and a non-empty `SupersededChain` are distinguishable from each other in the output, since §31.4 makes their orderings mean different things.

This matches the posture the language already takes for exception text everywhere else: §33.3 and §32.8 fix *what* is printed and never *how*, and `StackTrace` is *"formatted"* with no format.

▲ **The reports' content is defined independently of `ToString()`.** §33.3's uncaught report, §32.8's FFI-boundary abort, and v0.266's §22.13 exit-time disposal report each enumerate their own five-item content and are **not** defined as calling `ToString()`. Stating this is not pedantry — it forecloses two live hazards:

- **double printing**, where an implementation composes `ToString()` (now carrying message and chains) with its own separate property reads. A shipped implementation is in exactly this shape today: `bz_unhandled_exception` calls vtable slot 3 for the *type name* and then reads `Message` and `CauseChain` by field offset;
- **a fatal allocation on the termination path** — see this proposal's §5.

### D4 — Purity: unmarked, deliberately

▲ The member carries **no** `pure` marker. §31.4 marks only the `()` and `(message)` constructors `pure` (the §22.15 throw-carve-out enablers). `ToString` reads `StackTrace` and both chains — all runtime-populated, exactly as §31.4 says of `SupersededChain` (*"the chain is runtime-populated exactly as `StackTrace` is"*) — and it allocates a fresh `string`. A `pure` marker would state a contract §22.15's calls-to-pure-members rule could not admit, and no constructor's existing purity classification changes either way.

### D5 — `ExceptionInfo` is untouched

▲ No change to `public record ExceptionInfo(TypeName: string, Message: string, StackTrace: string)`. §31.4 already records one deliberate loss here (*"a superseded exception's own `CauseChain` is not carried … widening a shipped public record for that case was declined"*); D2 renders the two fields it needs and does not reopen that.

### D6 — The proposed surface ▲

```bezel
public class Exception
  public pure constructor();
  public pure constructor(message: string);
  public constructor(message: string, cause: Exception);   // snapshot chaining, below
  public readonly property Message: string;
  public readonly property StackTrace: string;             // formatted; "" when unavailable
  public readonly property CauseChain: Array<ExceptionInfo>;   // empty when no cause
  public readonly property SupersededChain: Array<ExceptionInfo>;   // v0.259 — empty when nothing superseded
  public override method ToString(): string;               // ▲ 0113 — qualified type name, Message, and both chains
end Exception;
```

One member. The accompanying normative sentence, ▲, to replace the false half of each promise and stand as the declaring text:

> ▲ **`ToString()` is the declaring member of the rendering promise.** `Exception` overrides [§8.1]'s `Object.ToString`, replacing [§8.5]'s compiler-generated class default for `Exception` and every type deriving from it. The rendering contains the receiver's qualified type name (stdlib [§2.13]'s `FullName` rendering), its `Message` when non-empty, and every entry of its `CauseChain` and of its `SupersededChain` — each entry by its `TypeName` and `Message`, each chain in its own order, and the two chains distinguishable from each other, since their orderings mean different things. The receiver's `StackTrace` and the entries' are **not** rendered: the runtime reports that print a stack trace ([§33.3], [§32.8], [§22.13]) each define their own content and are not defined in terms of this member. The arrangement is implementation-defined; the content above and the type name's presence are not. A derived class may override further, as any class may.

## 3. What is **not** proposed

1. **No change to §8.5's class default.** It remains the qualified name for every class that does not override, `Exception` included had it not overridden. §8.5 needs **no edit at all** — this proposal's §5 records the audit that expected one.
2. **No new member on `Object`, and no change to `Object.ToString`'s virtuality.** It is already `public virtual` (§8.1).
3. **No `InnerException`-style live reference**, and no widening of `ExceptionInfo` (D5). §31.4's two declines stand.
4. **No stack-trace format.** D2 excludes the stack trace precisely so this proposal does not have to specify one; §31.4's *"formatted"* stays as unspecified as it is.
5. **No change to the runtime reports' content.** D3 states their independence from `ToString`; it does not alter what any of the three prints.
6. **No `IFormattable` conformance** and no `ToString(format: string)` overload for exceptions. stdlib §2.18's specifier vocabulary is for the intrinsic and user-formattable types; a format-specified exception hole has no motivating use in the repo.
7. **No compile-time rule of any kind.** Nothing new is checked; body-locality is vacuous (0105 §1).

## 4. Costs

**Behavioral, and universal.** Every `Console.WriteLine(ex)`, `$"{ex}"`, and `Log(ex.ToString())` in every program changes output — from one token to one line plus chain entries. No program changes *shape*; the changelog category is *"Meaning changed, without acceptance changing"*. A program that today parses `ex.ToString()` expecting exactly a qualified type name breaks; that program is relying on the §8.5 default of a type whose spec text has promised the opposite since v0.162, and it should read `ex.GetType().FullName` (stdlib §2.13), which is stable and says what it means.

**Runtime.** One string build per call, on a path that was previously a constant. Exceptions are not a hot path. `SupersededChain` and `CauseChain` are empty in the common case and §31.4 already promises the empty case *"allocates nothing"*, so the common rendering is type name + message.

**Implementation.** `bezelc` must emit an `Exception.ToString` body into vtable slot 3 for the whole built-in hierarchy, and `bz_unhandled_exception` must stop calling slot 3 for the type name (D3's double-print hazard; the report already reads `Message`/`CauseChain` by offset and would simply add a `FullName` read). One spec-ahead-of-impl issue at graduation.

**Normative surface.** One member, one paragraph, two one-clause additions at §33.3/§32.8. Nothing is removed.

**§2.8.6 delta classification: identical.** No signature any consumer compiles against changes shape; a member is added to a class whose layout is the runtime's. No consumer recompiles.

## 5. Interactions

- **§8.5 — audited, and it needs no edit.** The expected companion edit does not exist: §8.5's bullet is introduced as *"The compiler-generated **default** returns:"*, and §8.5 already describes two replacement mechanisms — an enum *"may replace its default by declaring a plain `ToString(): string` member"*, and a body record's *"`override ToString` replaces only the formatting"*. `Exception` overriding is the ordinary class case of the same thing. Recorded because concluding otherwise would have doubled this proposal's edit set.
- **§17.7 — fatal allocation, and why D3's independence clause is load-bearing.** *"When a runtime allocation cannot obtain memory … the program **terminates**"*, with exactly one carve-out in the language: §31.2's supersession-snapshot append. `ToString()` allocates. If a runtime report were *defined* as calling `ToString()`, an out-of-memory condition during the report would abort the process mid-report — on the very path (§33.3, §32.8, §22.13) whose purpose is to explain a failure. D3 keeps the reports reading properties directly, which is what the shipped implementation already does and what §33.3's own wording already says. This proposal deliberately does **not** request a second §17.7 carve-out.
- **§22.15/§22.16 — purity.** D4. No classification changes; no contract clause can call the new member, which is correct.
- **§31.1 — the copy-out instruction.** It stays exactly as written; copying properties out remains the way to preserve an exception past its arm, and `ToString()` is a rendering, not a preservation mechanism. One sentence may be added there naming `ToString()` as the shorthand for the *logging* case specifically.
- **§30.9 / §31.4 — the two owner-owned instances.** A task-owned fault and the cached `TypeInitializationException` render like any other exception. `TypeInitializationException.InnerException` is a **live reference**, not an `ExceptionInfo`; D2 does not render it (it is neither chain), which keeps `ToString` free of any live-instance traversal and of the arm-lifetime questions §31.4 closes.
- **v0.266's §31.4 open-boundary flag.** Graduation removes it, exactly as v0.266 removes §22.13's #3258 flag.
- **Guide 02.** Its §278 paragraph teaches supersession; a graduating train should show `ex.ToString()` at a log site, which is currently spelled nowhere in the guides.
- **Kernel (v0.198): no contact.** §31.4 and §8.5 are not rules of §28 or §30.5; exception ownership (§31.1) is untouched, and all built-ins remain trivial-dispose (§28.9 keys that on destructors and `owned` fields, not on member count).
- **Body-locality (0105 §1): vacuous** — no compile-time rule is added or altered. Were one wanted, override resolution is boundary 1's declaration-signature case and would be **local**.

## 6. Questions for the owner

- **Q1 — D1: declare, or re-home?** This proposal recommends declaring, on the ground that re-homing retracts v0.259's recorded *"renders it beside its causes"* and leaves `Console.WriteLine(ex)` printing a bare type name forever. Re-homing is smaller in the spec and larger in the guides, and it makes §31.1's hand-copy the only mechanism the language offers.
- **Q2 — D2: is the stack trace in or out?** Out, per this proposal, on the promise's own scope, `StackTrace`'s unspecified format, one-line-log usability, and Java's split. C# includes it. This is the fork with the largest day-to-day consequence and the one on which the two obvious precedents disagree.
- **Q3 — D3: is "content normative, arrangement implementation-defined" the right posture**, or should a `ToString` rendering be exactly specified the way §8.5's default is (*"one rendering contract, not two"*)? Exact specification buys cross-implementation log parity and costs a normative multi-line format for two variable-length chains.
- **Q4 — should D3's independence clause be stated at §33.3/§32.8/§22.13, or only here?** Stating it at each report site is three clauses and forecloses the double-print and fatal-allocation hazards in the text; stating it only in this proposal leaves them to implementer judgement, and one shipped implementation is already in the shape that goes wrong.

---

# Landing summary

| Item | Class | Where | Blocked on |
|---|---|---|---|
| #3258 — the whole ruling (S1–S7) | **Ruling** | spec §22.13 L5254, §28.4 L6853, §33.3 L9900 + new paragraph | nothing — landable in v0.266 |
| #3258 — S6's exit-code fork | owner fork inside the ruling | one sentence in Edit 1 + Edit 4 | owner word, if they prefer exit 70 |
| #3259 — open-boundary flag | **Clarification** | spec §31.4, new paragraph after L9179 | must land **with** Edit 1 (cross-reference) |
| #3259 — the substantive fix | **Design change → proposal 0113** | spec §31.4 surface + §33.3/§32.8 clauses | owner ruling on Q1–Q4 |
| New issue: shutdown × abandoned-domain init deadlock (R3) | fresh `spec-defect` | — | file at landing |
| Spec-ahead-of-impl: exit-time report (R1); uncaught-path `_exit(70)` skipping the pass (R2) | tracking issues | — | file at landing |

**Composition note for the v0.266 gate.** Edits 1 and 5 are the only pair with a seam: Edit 5's §31.4 flag cross-references *"[§22.13]'s exit-time report"*, which Edit 1 creates. They must land together or Edit 5's reference dangles and `check_docs.sh` check 3 flags it. Verified in the other direction too: Edit 1's report sentence names the five-item bundle by content and does not route it through `ToString()`, so it is unaffected by whichever way 0113 goes.

---

