# Proposals currency sweep 2026-09-17 (docs v0.246 → v0.306)

*Supporting record (not a numbered proposal). Written 2026-09-17 at `main` @ `29410e29`, docs at
spec/stdlib v0.306. The successor of [`proposals-currency-2026-08-11.md`](proposals-currency-2026-08-11.md),
which swept the live proposals to v0.246; sixty revisions had landed since, and the live proposals
were pinned between v0.177 and v0.263. Same two-tier discipline as that record: **tier 1** is a
mechanical respell (a retired spelling, a renamed member, a moved section, a dangling structure
reference) applied in place and logged here with its line, old and new form, and the revision that
changed it; **tier 2** is a design premise the range changed, **flagged for the owner and not
fixed** — the proposal's design is not redrawn by a sweep. Every proposal touched carries a dated
`Currency note` in its header; no `Written against` pin was changed, by convention (it names the
revision the design was written against, not the last one it was checked at).*

*Method: one inventory lane read every changelog entry v0.247 → v0.306 and produced the
retired-form inventory in §1 (161 rows in five classes, with a tested quick-scan recipe); three
sweep lanes then applied it to 44 live proposals in parallel (§2–§4 are their reports, verbatim).
Three further live proposals — 0005, 0040 and 0052 — were refreshed in separate commits the same
day as design documents rather than swept, and were scanned with §1's recipe by the coordinator
(all hits English-word false positives). No build was available ([#4451](https://github.com/s8lap/bezel-lang/issues/4451));
every claim is a document or source read.*

## 0. Summary

| Lane | Files | Tier-1 respells | Tier-2 flags | Clean (neither) |
|---|---:|---:|---:|---:|
| A (0001–0016) | 14 | 9 in 5 files | 8 | 6 |
| B (0017–0031, 0047) | 15 | 9 in 6 files | 9 | 6 |
| C (0053–0111) | 15 | 8 in 6 files | 11 | 3 |
| coordinator (0005, 0040, 0052) | 3 | refreshed separately (see their commits) | — | — |
| **Total** | **47** | **26** | **28** | **15** |

**What the mechanical half found.** Small and identifiable, as the inventory's own scan
predicted: three `out owned var` (v0.289), one `operator ++` quotation (v0.289), one
`Task.WhenAllOwned` (v0.250), four "not auto-imported" currency claims false since v0.278/v0.289,
two retired stdlib overload pairs in transcribed surfaces (v0.290), `DateTime` ordering operators
that stdlib §2.5 no longer declares (v0.274), and a handful of spec quotations re-derived from the
landed text (v0.287, v0.299, v0.301). The design half is where the value is — §5.

**Cross-lane items, handled by the coordinator.**
- 0111 §4 cites proposal 0025 as independent corroboration that `System.Collections.Generic` is not
  auto-imported; lane B corrected 0025 (the premise expired at v0.289, 0122 L3) after lane C had
  already flagged 0111's delta as overtaken. 0111's citation now carries a bracketed note pointing
  here; the corroboration is historical.
- 0045 (graduated v0.204, superseded) keeps stdlib §2.4's pre-v0.231 `Environment` member names;
  the v0.231 changelog directs 0011, 0013 and 0045 to "take a currency note at their next
  revalidation, not a retro-edit". Lane A did so for 0011 and 0013; the coordinator added the same
  note to 0045, which as a superseded proposal owes no further revalidation.
- One **spec defect** surfaced by the sweep, filed as [#4457](https://github.com/s8lap/bezel-lang/issues/4457):
  §29.11 (v0.306) still mandates that the consumption diagnostic name "the `.ToArray()`
  materialization remedy" the same section withdrew at v0.265 (#2557). Proposal 0052 quotes the
  sentence and is annotated.

**README.** No live proposal changed status in this sweep, so the per-proposal ledger rows are
unchanged; this record's own row is added. The rows whose *next action* the tier-2 flags bear on
(0006, 0012, 0016, 0047, 0053, 0111) are listed in §5 for the owner to re-ledger when ruled.

## 1. The retired-form inventory (v0.247 → v0.306)

*Produced by the inventory lane from every changelog entry in range; the sweep lanes' single input.
The `scan.sh` recipe in §1F was run against the tree before the lanes started; its hit counts are
what a lane expected to find.*

*Supporting record for a proposals currency sweep. This is the successor of §1 of
`docs/proposals/papers/proposals-currency-2026-08-11.md`
("The retired-form inventory swept", which covered v0.242–v0.246). It covers the **sixty revisions
v0.247 → v0.306** and is the list a sweep lane works from.*

**Derivation.** Every row below was read out of `docs/Bezel_Changelog.md` entries
`### v0.247 changelog` (line 7447) through `### v0.306 changelog` (line 9249), entry by entry, with
the two graduation batches (`0122-language-simplification-batch-2026-09.md` §1 L1–L18,
`0123-stdlib-regularization-batch-2026-09.md` R1–R10) and proposals 0124/0125/0126 read for
the exact old→new spellings. Current spellings were confirmed against
`docs/Bezel_Language_Spec.md` and `docs/Bezel_Standard_Library.md` at **v0.306**.
Line numbers in the *Changelog line* column are `docs/Bezel_Changelog.md` line numbers at the tree
this was written against; they are a reading aid, not a citation — cite **§ + revision**.

**Scope rule inherited from the 2026-08-11 paper.** Forms retired *before* v0.247 are **not**
repeated (`sealed abstract`, *equality-bearing*, `:= owned x` / `case owned x of` / `M(owned x)`,
`var _ : T` pattern bindings, `E(n)` raw minting, non-`Exception` throw operands). Where a later
revision changed one of those again — the *equality-bearing* → *hashable* vocabulary's sibling
`borrow-carrying census` → `holds-borrows` (v0.257), the `owned`-marker overload rules (v0.250) —
the **new** change is listed here.

**Population.** 47 live proposals per `docs/proposals/README.md`'s live table:
0001, 0002, 0004, 0005, 0006, 0007, 0008, 0009, 0010, 0011, 0012, 0013, 0014, 0015, 0016, 0017,
0018, 0019, 0020, 0021, 0022, 0024, 0025, 0026, 0027, 0028, 0029, 0030, 0031, 0040, 0047, 0052,
0053, 0054, 0060, 0068, 0069, 0070, 0071, 0088, 0103, 0104, 0105, 0106, 0108, 0110, 0111.
(0061 left the live table; 0082/0091–0102/0107/0109/0112–0126 graduated in-range — see §E.)

---

### §0 Summary counts

| Class | What it covers | Rows |
|---|---|---|
| **A** | Retired language constructs / keywords (a spelling that no longer parses, or parses and is rejected) | **28** |
| **B** | Renamed / removed / added stdlib members, types and parameter names | **50** |
| **C** | Changed rules a proposal's *design* may rely on (no rename; a premise moved) | **44** |
| **D** | Moved or renumbered spec/stdlib/rationale sections, grammar productions and anchors | **25** |
| **E** | Document-structure changes (marker sweeps, content relocations, derived-document pins, ledgers) | **14** |
| | **Total** | **161** |

Classes A, B and D are mechanical and complete for the range; class C is a completeness-first list of
premise-invalidating rulings, one line each.

---

### §A Retired language constructs and keywords

| # | Retired / old form | Current form | Rev | Changelog line | Grep regex | False-positive notes |
|---|---|---|---|---|---|---|
| A1 | `(when c1 => a, c2 => b, _ => z)` — the `when` **expression atom** | `(if c1 then a elif c2 then b else z)` | v0.289 | 8815 | `\(when[[:space:]]` | Very high: English prose "(when the closure …", "(when `using var h` …". The **arm guard** `case … when g =>` and the `when true` / `when false` postcondition clause both **survive** — do not respell those. |
| A2 | `when c1 => s1, _ => s2 end when?;` — the `when` **statement** | `if c1 then … elif c2 then … else … end;` (a single-statement `=> stmt;` arm becomes a one-statement block) | v0.289 | 8815 | `^[[:space:]]*when[[:space:]]+[^=]*=>` and `end[[:space:]]+when` | `end when` is near-zero FP. The `_`-less form has no corpus instance; if one appears it restructures or raises a **declared** exception in its `else` — never a hand-raised `MatchFailureException` (§31.4 states its producer). |
| A3 | Grammar nonterminals `WhenExpr`, `WhenStmt`, `WhenArm` | deleted from A.6 / A.7 | v0.289 | 8821 | `WhenExpr\|WhenStmt\|WhenArm` | None. Any hit is a quotation of a deleted production. |
| A4 | `out owned var x` at an `out owned` parameter | `out var x` (adopts the callee's announced classification, §28.3's adoption clause); `out using var x` **survives**; a pre-declared adoptable local adopts once | v0.289 | 8815, 8816 | `out[[:space:]]+owned[[:space:]]+var` | None. Do **not** rewrite `out using var`. |
| A5 | `operator ++` / `operator --` **declarations** (a syntax error at the token) | `operator +` / `operator -`, and `x := x + 1;` at the use site | v0.289 | 8815 | `operator[[:space:]]+(\+\+\|--)` | None. |
| A6 | `x++` / `x--` on any operand outside §6.6's `+= 1` set (a user type with or without an overload; an enum) | `x := x + 1;` | v0.289 | 8815 | `[A-Za-z0-9_)\]]\+\+\|\+\+[A-Za-z0-9_]` | Moderate: "C++", diff markers, `**bold**` is not matched but `++` inside code spans is. **`char++` is NOT retired** (§6.6 admits `char`), nor is `n++` on the numeric `+= 1` set. |
| A7 | `requires`, `ensures`, `invariant` used as **identifiers** (`method M(requires: int)`) | rename the identifier — all three are **unconditionally reserved** (B.1), no longer contextual | v0.260 | 7767 | `\b(requires\|ensures\|invariant)[[:space:]]*(:[^=]\|,\|\)\|\])` | Enormous: these are the contract keywords, so nearly every hit is a legitimate clause. Only an **identifier position** is retired. `old` and `result` stay contextual. |
| A8 | Lifted arithmetic / bitwise / shift / relational operators over nullable value types, in every spelling (binary, unary, compound-assignment, `++`/`--`), plus relational and range **patterns** over a nullable scrutinee | unwrap first: `!`, `??`, or §7.4 narrowing. For a mutable nullable field, the short-circuit form `invariant(_p <> null and _p! >= 0)` or declare the field `readonly` | v0.267 | 8004, 8017 | `[a-z]\?[[:space:]]*(\+\|-\|\*\|/\|<<\|>>\|<=\|>=\|&\|\|\|\^)[[:space:]]` | High: `T?` followed by punctuation, `Task?`, markdown. §8.4 equality, `??`, `?.`, `?(args)`, `??=` are **unchanged**. |
| A9 | `using var _ := <Task-family expression>;` and `await using var _ := …;` | `Task.FireAndForget(e)`, or a named `using var t := e;` | v0.288 | 8784 | `using[[:space:]]+var[[:space:]]+_[[:space:]]*:=` | The anonymous owning binding `using var _ := …` over a **non-Task** expression is the v0.244 *kept* form — check the RHS type before editing. |
| A10 | A `when true` / `when false` postcondition restated as a hand-written `static ensures` clause on an override or interface implementation | restate the `when` clause itself (one writable spelling) | v0.288 | 8784 | `static[[:space:]]+ensures` | None. |
| A11 | A **mutable** or `scoped` static field, a `static` property, or a static constructor on an **enum or choice** body | move to a class; an **uninitialized `static readonly`** on an enum/choice body is newly *admitted* | v0.288 | 8784, 8785 | `static[[:space:]]+(var\|owned[[:space:]]+var\|property\|constructor)` + enum/choice context | High — needs the enclosing declaration. Class and record statics are unaffected. |
| A12 | Reading a primary-constructor parameter, or any field / auto-property of `this`, in a `this(...)` / `base(...)` argument | hoist to a local, or pass a literal | v0.288 | 8784 | `:[[:space:]]*(this\|base)\(` | Moderate: `: base(...)` with constant arguments is fine. |
| A13 | A borrow of an `owned` **parameter** stored into a field or a longer-lived collection | keep the borrow call-bounded, or transfer | v0.288 | 8784 | — (semantic; read `owned` parameters' bodies) | Compiler already rejected since #3746. |
| A14 | A **member-level `where` clause naming an enclosing type parameter** (`class Box<T> … static method Make(): int where T: new()`) | give the member its own type parameter; enclosing-parameter conditions are §14.2 *conditionally-admitted members*, stated in prose | v0.277 | 8484 | `where[[:space:]]+[A-Z][A-Za-z0-9_]*[[:space:]]*:` inside a member declaration | High: a member-level `where` over the **member's own** parameters is legal. Compare the clause's identifier against the member's own `<…>` list. |
| A15 | Two type parameters constrained in one `where` clause: `where A: sendable, class, R: sendable, class` | one `where` clause per type parameter (A.4 admits one identifier per clause with `GenericConstraints*` repetition) | v0.274 | 8306 | `where[[:space:]]+[A-Za-z_]+[[:space:]]*:[^,]*,[[:space:]]*[A-Za-z_]+[[:space:]]*:` | Low. The form is *ungrammatical*, not merely stylistic. |
| A16 | `virtual`, `abstract`, `sealed override` on any **record** member; `override` on a record member outside `Equals`/`GetHashCode`/`ToString`; the same on an enum's or choice's **expression-bodied property** | value kinds dispatch statically — drop the modifier, or make the type a class | v0.297 | 9005 | `(virtual\|abstract\|sealed[[:space:]]+override)[[:space:]]+(method\|property\|indexer)` | High: classes use all of these legitimately. Needs the enclosing kind. |
| A17 | A class or record member named `Dispose` **at any parameter list** (widened from the zero-parameter form) | rename the member | v0.300 | 9108 | `method[[:space:]]+Dispose[[:space:]]*\(` | Calls to `x.Dispose()` are fine; an **interface** may declare `Dispose()` (v0.301 carve-out, stated by admission). |
| A18 | `virtual` on a member of a `sealed` class | drop `sealed`, or drop `virtual` | v0.300 | 9108 | `sealed[[:space:]]+class` + `virtual` within the body | Needs two-line context. Zero in-tree occurrences at landing. |
| A19 | `owned Type` at every position §28.1 lists, and a `Type` element slot claimed as owned (`OwningArray<Type>`) | `Type` is runtime-managed — drop the marker and use a borrowing collection | v0.300 | 9108 | `owned[[:space:]]+Type\b\|Owning[A-Za-z]*<[[:space:]]*Type[[:space:]]*>` | Moderate: `Type` is a common metavariable in prose. |
| A20 | A bare `..` in an untyped context (`var r := ..;`) | ascribe the element type, or write a bounded range | v0.300 | 9108 | `:=[[:space:]]*\.\.` | Low. |
| A21 | A choice **case** named `Equals`, `GetHashCode` or `ToString`, on every classification | rename the case | v0.302 | 9179 | `case[[:space:]]+(Equals\|GetHashCode\|ToString)\b` | Low. |
| A22 | An explicit conversion whose operand is a **compile-time constant** the conversion would reject — *including in unreachable code* (`uint(-1)`, `if false then var u := uint(-1); end;`) | write an in-range constant, or convert a non-constant | v0.300 | 9108 | `\b(uint\|int\|byte\|sbyte\|short\|ushort\|long\|ulong\|char)\([[:space:]]*-?[0-9]` | High: in-range constant casts are legal. |
| A23 | The same at an **enum→integer** cast (`int(HttpStatus.Huge)` at a `long`-backed enum) | pick a target that holds the constant | v0.301 | 9127 | `\b(int\|uint\|short\|byte\|sbyte\|ushort\|long\|ulong)\([A-Z][A-Za-z0-9_]*\.[A-Z]` | Moderate. A **local-variable** operand still faults at run time. |
| A24 | `new Subscription(...)` at any argument list; a standard-library **builtin** standing as the substitution for `where T: new()` | construct through the declared factory; substitute a user class | v0.300 | 9108 | `new[[:space:]]+Subscription\(` | Low. |
| A25 | An `abstract` or `closed` class as the substitution for `where T: new()` | substitute a concrete class, or choose a different constraint | v0.302 | 9155 | `where[[:space:]]+[A-Z][A-Za-z0-9_]*[[:space:]]*:[[:space:]]*new\(\)` + the substituted type | Needs the substitution site. §22.7's implicit parameterless constructor is **untouched**; only the constraint excludes them. |
| A26 | A reference to a generic type that omits its argument list (`Box` bare) or supplies the wrong arity | supply exactly the declared arity; there is no raw and no open generic **type** reference (`where T: new()`-style inference is a generic **member call** rule, §14.5) | v0.303 | 9189 | — (semantic; read declaration sites) | Only a *type reference* position. |
| A27 | A bare identifier bound inside a **function type's slots** in an `extension` head (`extension Array<(T) -> T>`) | §26.4's "any nesting depth" means a **constructed generic type's** argument positions only — bind the parameter on the extension block, or restructure | v0.303 | 9191 | `extension[[:space:]]+[A-Za-z0-9_]+<[^>]*\(` | Low. |
| A28 | A write that lands in the box of a §12.7 **value-narrowed** carrier at an `Object`/interface location — `o[0] := 9`, `o.X := 5`, `o[0] += 1`, `ref o[0]` / `out o.X`, and the by-address `extern` inline-array argument | copy out, mutate, re-box: `var b: [3]int := o; b[0] := 9; o := b;` — `o += 1` / `o++` on the **bare binding** stay admitted (§6.6 mints a fresh box) | v0.254 | 7592 | `[[:space:]]o(\[[^]]*\]\|\.[A-Za-z_][A-Za-z0-9_]*)[[:space:]]*(:=\|\+=\|-=\|\+\+)` | High; needs the narrowing context. Narrowing to a **reference** type is untouched. |

**Also in this class, as semantic retirements with no single grep:** self-mutating value-kind member
bodies (a record method or block-form accessor storing into `this` through implicit `this`), and a
value kind conforming to an interface that declares a settable property or indexer — both rejected
at **v0.255** (line 7619); `static readonly` of an **owning-classified choice** type, rejected at
**v0.265** (line 7940, replacement: a `static owned readonly` class field holding the choice).

---

### §B Renamed, removed and added stdlib members, types and parameters

| # | Old form | Current form | Rev | Changelog line | Grep regex | False-positive notes |
|---|---|---|---|---|---|---|
| B1 | `Task.WhenAllOwned` (stdlib §6.7) | `Task.WhenAll` — the owning form keeps its exact signature and merges into an overload; the old name left the stdlib in the same revision | v0.250 | 7515 | `WhenAllOwned` | None. |
| B2 | `Task.WhenEachOwned` | `Task.WhenEach` | v0.250 | 7515 | `WhenEachOwned` | None. `Task.FromOwnedResult` is **not** renamed. |
| B3 | `OwningSet<T>.Take(query)` / `TryTake(query)` (stdlib §4.10) | `Pop(query)` / `TryPop(query)` — hard flip, no alias. Un-shadows LINQ's `Take(count)` on a set receiver | v0.259 | 7733 | `\.(Take\|TryTake)\(` | High: proposals declare their own `Take` (e.g. 0052's `Take(refresh: bool)`). `OwningDictionary.PopValue`/`TryPopValue` keep their names, with the reason now stated at §4.9. |
| B4 | The 14 per-type `Sum` overloads (stdlib §5.1) | `Sum<U>(selector: (T) -> U): owned U where U: INumber<U>` and `Sum(): owned T where T: INumber<T>` | v0.259 | 7734 | `\bSum\(` | `Average`'s 14 overloads **stay** (blocked on result-type mapping; recorded at §5). |
| B5 | `ElementAtOrNull` named in four enumerations but **undeclared** | declared: `ElementAtOrNull(index: int): T?` (stdlib §5); joins §29.11's terminal-accessor warning set; `MinOrNull`/`MaxOrNull` join the same enumeration | v0.259 | 7735 | `ElementAtOrNull` | None — this is an addition, so a proposal naming it is now correct. |
| B6 | *(new)* | `Exception.SupersededChain: Array<ExceptionInfo>` (spec §31.4), with §31.2's record-before-drop rule, chronological earliest-first | v0.259 | 7729 | `SupersededChain` | Addition. |
| B7 | *(new)* | `ScopeGuard` (stdlib §2.21): `constructor(scoped action: () -> ())`, `Dismiss()`, `IsDismissed`, destructor-invoked. The errdefer idiom; language `defer`/`errdefer` **declined** (rationale §17.11) | v0.259 | 7739 | `ScopeGuard\|errdefer\|\bdefer\b` | `defer` in English prose. |
| B8 | `Range<T>.Contains` / `IsOpenEnded` / `IsEmpty` unmarked | `pure` (stdlib §9.8, spec §16.3) — so `requires(port in 0..65535)` now compiles | v0.263 | 7868 | `Range<[^>]*>\.(Contains\|IsEmpty\|IsOpenEnded)` | Addition of a marker. |
| B9 | `CancellationTokenSource` non-sendable; `static readonly` drew a warning | `Shared<CancellationTokenSource>` is **sendable**; `static readonly` / `static owned readonly` admitted warning-free; `Register` on a **published** source raises `InvalidOperationException` (stdlib §6.1) | v0.263 | 7869, 7872 | `CancellationTokenSource` | A **mutable** static source is still rejected; the bare source still may not cross a domain boundary; `CancellationToken` still may not cross at all. |
| B10 | *(new)* | The **bit tier**: `BitReader`, `BitWriter` (stdlib §4.14), `ArrayBitReader`, `ArrayBitWriter` (§4.15) — 68 members, six extension entry points. `TryWrite*` carries a spec-ahead-of-impl marker (v0.284) | v0.264 | 7903 | `BitReader\|BitWriter\|ArrayBit` | Addition. |
| B11 | `ToArray()` recommended as the remedy for the fresh-yield liveness hazard at **class** element types (stdlib §5, v0.235 sentence) | guidance **withdrawn** — the member still exists and is sound as a borrowing snapshot; the two working remedies are named in the same paragraph | v0.265 | 7944 | `\.ToArray\(` | Very high: `ToArray()` exists on every owning collection and is used legitimately. The retired thing is the *claim*, at class element types. |
| B12 | `Result`'s `UnwrapOr` / `Map` / `MapError` / `AndThen` argument deferred into the matched arm | the argument expression is evaluated **at the call site on every path** (stdlib §2.20) — `.UnwrapOr($"bad port: {raw}")` builds the string on the `Ok` path too | v0.265 | 7943 | `UnwrapOr\|MapError\|AndThen` | Acceptance unchanged; observable effects change. |
| B13 | §15.4.1's borrowing-form collection-literal carrier `ReadOnlySpan<T>` (the **owning** family's view) | `ReadOnlyArraySpan<T>` (the borrowing family's view, backed by `Array<T>`); the `may be stack-allocated` permission retires; the no-retain clause (§28.6) is kept verbatim | v0.272 | 8231, 8237 | `(^\|[^A-Za-z])ReadOnlySpan<` | High: `ReadOnlySpan<T>` is a **live** type — only the §15.4.1 carrier moved. `IReadOnlySpan<…>` must not match (use the leading non-letter guard). |
| B14 | `ReadOnlyArraySpan<T>.ToArray()` | **does not exist** — declined with a `DECLINED.md` row and a reopening bar; a factory body spelled `elements.ToArray()` stops compiling | v0.272 | 8239 | `ReadOnlyArraySpan<[^>]*>\.ToArray` | Low. |
| B15 | `Array<T>.Sort()` / `Sorted()` declared as **extension** members (unreachable by §26.5 rule 1) | arity-0 overloads **on `Array<T>` itself**, admitted only where `T: IComparable<T>`, checked and reported at the call's instantiation. (Deleted at v0.274, restored at v0.275 — v0.274's ruling 2 was reversed.) `SortNatural`/`SortedNatural` **never existed** | v0.274 → v0.275 | 8302, 8348 | `\.Sorted?\(\)` | `SortNatural`/`SortedNatural` are an invented API; any hit is stale. |
| B16 | `OwningArray<T>.Sort()` absent | declared at the document level | v0.276 | 8441 | `OwningArray<[^>]*>\.Sort` | Low. |
| B17 | `DateTime` / `TimeSpan` / `DateOnly` / `Guid` claimed a `<`, `>`, `<=`, `>=` operator group agreeing with `CompareTo` | **none of the four operators exists** — the claim was deleted; use `CompareTo` | v0.274 | 8300 | `(deadline\|DateTime\|TimeSpan\|DateOnly\|Guid)[^\n]{0,40}[<>]=?[[:space:]]` | Moderate: generic-argument angle brackets. |
| B18 | `AsyncEventSource<T>.RaiseAsync` declared `public async method` | the `async` marker is deleted (§28.12 makes `async` implementation-side); payload types stay **unrestricted**, and the **retention obligation** — `RaiseAsync` retains the payload until the returned task settles — is now normative | v0.274 → v0.275 | 8310, 8353 | `RaiseAsync` | Low. |
| B19 | `Ptr<T>` overloaded `=`/`<>` with no `Equals`/`GetHashCode` (a §25.2 compile error) | `Ptr<T>` declares `Equals`/`GetHashCode`; `IntPtr`/`UIntPtr` **keep** §25.2's exemption, with §9.2's self-contradiction filed as the blocker | v0.274 → v0.275 | 8312, 8357 | `Ptr<[^>]*>\.(Equals\|GetHashCode)\|IntPtr\|UIntPtr` | Moderate. |
| B20 | `DateTime.Date` / `DateTime.TimeOfDay` promised in prose, absent from the block | declared | v0.274 | 8314 | `\.TimeOfDay\|DateTime[^\n]{0,10}\.Date\b` | Addition. |
| B21 | `Type` had no choice predicate; `IsValueType` disjunction excluded choices | `Type.IsChoice` declared (stdlib §2.13); §4.14 admits **value-like and borrow-bearing** choices, owning choices stay excluded | v0.275 | 8355, 8356 | `IsChoice\|IsValueType` | Addition. |
| B22 | `const Zero` on `IntPtr`, `UIntPtr`, `Ptr<T>` | `static readonly property Zero` (the `Guid.Empty` / `TimeSpan.Zero` / `CancellationToken.None` convention) | v0.275 | 8361 | `const[[:space:]]+Zero` | Low. |
| B23 | `OwningBuffer<T>.Length` unmarked; `Count`/`Capacity` unmarked | `Length` is `pure`; `Count` gains `pure`; **`Capacity` deliberately does not** | v0.274, v0.275 | 8316, 8363 | `OwningBuffer<[^>]*>\.(Length\|Count\|Capacity)` | Marker-only. |
| B24 | `ReadOnlySpan<T>.ToArray(): owned Array<T>` | **removed** at the document level (the compiler already rejected it) | v0.277 | 8484 | `ReadOnlySpan<[^>]*>\.ToArray` | Low; distinguish from `OwningArray<T>.ToArray()`, which exists. |
| B25 | Six `IEnumerable<T>` LINQ declarations spread across extension blocks | moved between blocks §26.6 merges — no call site changes, no member changes module; `GroupBy`/`ToDictionary` gain `where TKey: hashable` | v0.277 | 8485, 8489 | `where[[:space:]]+TKey` | Presentation only. |
| B26 | `Dictionary<K,V>` and `SortedDictionary<K,V>` had no `TryAdd`; `SortedDictionary` was indexer-write-only | `Dictionary<K,V>.TryAdd(key, value): bool`, `SortedDictionary<K,V>.TryAdd(key, value): bool`, `OwningDictionary<K,V>.TryAdd(key, owned value): bool` (disposes on `false`), and `SortedDictionary.Add(key, value)` | v0.282 | 8619 | `TryAdd` | Additions. `SortedDictionary`'s `Keys`/`Values` stay absent, now with a recorded reason. |
| B27 | `String.From(buffer, start, **count**)` | `String.From(buffer, start, **length**)` | v0.282 (#3373) | 8619 | `String\.From\([^)]*,[^)]*,` and `\bcount:` | Named-argument call sites only are affected; positional sites are unchanged. |
| B28 | `String.Remove(startIndex, **count**)` | `String.Remove(startIndex, **length**)` | v0.282 | 8619 | `\.Remove\([^)]*,[^)]*,` / `\bcount:` | Same. `Substring(startIndex, length)` was already conforming. |
| B29 | `Encoding.GetString(bytes, **index**, **count**)` | first renamed `(bytes, start, length)` at v0.282, then the **whole windowed form retired** at v0.290 → `enc.GetString(bytes.AsReadOnlySpan(start, length))` | v0.282 → v0.290 | 8619, 8836 | `GetString\([^)]*,[^)]*,` | Low. The one-argument `GetString(bytes)` survives. |
| B30 | The window-parameter convention was unstated | stdlib §1 records it: windows are **`(start, length)`** (`startIndex` is the string family's spelling of the start); the **`Stream` byte-window family keeps `(offset, count)`** as the one recorded exception; `RawBytes.CopyTo(destination, destOffset)` is a write position, not a window | v0.282 | 8619 | `\b(count\|index\|startIndex):` | High: `index:` and `count:` remain correct on `Stream` and as ordinary prose words. |
| B31 | `Console.Clear()` | **removed** (stdlib §2.1) | v0.284 | 8654 | `Console\.Clear` | Low. |
| B32 | LINQ `OfType<U>()` | **removed** (stdlib §5) | v0.284 | 8655 | `OfType<` | Low. |
| B33 | `Math.Sin`, `Cos`, `Tan`, `Asin`, `Acos`, `Atan`, `Atan2`, `Sinh`, `Cosh`, `Tanh`, `Exp`, `Log` (both), `Log10`, `Log2`, and the constant `Math.E` — 15 documented names | **removed** (stdlib §2.2). The undocumented `Math.Hypot`, `Math.Cbrt`, `Math.PI` are removed from the compiler with them. **Kept:** `Sqrt`, `Pow`, `DivRem`, `Fmod`/`IEEERemainder`, the rounding family | v0.284 | 8656 | `Math\.(Sin\|Cos\|Tan\|Asin\|Acos\|Atan2?\|Sinh\|Cosh\|Tanh\|Exp\|Log10\|Log2\|Log\|Hypot\|Cbrt\|PI\|E)\b` | `Math.E` needs the `\b` or it matches `Math.Exp`/`Math.Equals`. |
| B34 | Nine overload **pairs** | one `:=`-defaulted signature each (stdlib §1, §2.2, §2.15, §4.4, §6.7, §7.2, §7.4, §8.2, §9.6): `Task.Delay(ms\|duration, token := CancellationToken.None)`, `Directory.Delete(path, recursive := false)`, `PadLeft`/`PadRight`/`PadCenter(totalWidth, paddingChar := ' ')`, `FileStream(path, mode, access := FileAccess.ReadWrite)`, `BinaryDeserializer(input, ignoreUnknownFields := false)`, `Binary.Parse<T>`/`ParseOwned<T>(input, ignoreUnknownFields := false)`, `Debug.Assert(condition, message := "")`, `Math.Round(x, digits := 0)`, `String.IndexOf(value, startIndex := 0)`, `Array<T>.IndexOf(item, startIndex := 0)` | v0.290 | 8833 | `Task\.Delay\(\|Directory\.Delete\(\|Pad(Left\|Right\|Center)\(\|new FileStream\(\|BinaryDeserializer\(\|Debug\.Assert\(\|Math\.Round\(\|\.IndexOf\(` | Call sites are unaffected (the defaults are exactly what the dropped shorter overload did). A proposal that *lists the overload pair* is stale. **Three shapes keep a pair:** forms differing by parameter type or `owned`; an interface-required member beside its `ignoreCase` twin; and an omitted argument with **no nameable default** — the to-the-end window forms (`Slice`, `AsSpan`, `AsReadOnlySpan`, `Substring`, `String.Remove`) and the §2.15 `Json` `options: JsonOptions` pairs. |
| B35 | `Counter<K>.ContainsKey(key)` | `Counter<K>.Contains(key)`, and `key in tally` (spec §17.5) | v0.290 | 8833, 8845 | `ContainsKey` | High: the **three dictionaries** keep `ContainsKey` as the inherited alias, and proposals declare their own (`HttpHeaders.ContainsKey`, `JsonObject.ContainsKey`). Only `Counter<K>` moved. Consequence: `tally.Contains(entry)` through §5's `IEnumerable<T>` extension is now **shadowed** by the instance member. |
| B36 | `ArraySpan<T>.SequenceEqual(ReadOnlyArraySpan<T>)` and `ReadOnlyArraySpan<T>.SequenceEqual(ArraySpan<T>)` — the two cross-mode concretes | **retired** — a cross-mode call resolves to the `IReadOnlySpan<T>` overload under §8.6's call-bounded receiver boxing | v0.290 | 8835 | `SequenceEqual` | Moderate: the interface overload and the same-mode concretes survive. |
| B37 | `Math.Sign`'s four per-type `pure` overloads | `Math.Sign<T>(x: T): int where T: INumber<T>` — **not `pure`**; a `char` argument is newly rejected → `Math.Sign(int(c))`; `Math.Sign(double.NaN) = 0` | v0.290 | 8835 | `Math\.Sign` | `Math.Abs` deliberately **stays** per-type (the result-type mapping blocks a generic). |
| B38 | `Type.Implements(iface)` | `iface.IsAssignableFrom(t)` — retired from both documents and from rationale §18's delivered-member list | v0.290 | 8837 | `\.Implements\(` | Low. |
| B39 | `Stopwatch.Restart()` | `Reset(); Start();` — and §8.1 now states what `Reset()` does: stops the stopwatch and zeroes `Elapsed` | v0.290 | 8837 | `\.Restart\(` | Low. `ElapsedMilliseconds` is a recorded **keep**. |
| B40 | static `String.Compare(a, b)` / `String.Compare(a, b, true)` | instance `a.CompareTo(b)` / `a.CompareTo(b, ignoreCase := true)`, beside `Equals(other, ignoreCase)` | v0.290 | 8837 | `[Ss]tring\.Compare\(` | Low. |
| B41 | `String.IsNullOrEmpty(s)` | `s = null or s.IsEmpty`, or `s?.IsEmpty ?? true` | v0.290 | 8837 | `IsNullOrEmpty` | `string.IsNullOrWhiteSpace` **survives** and is the respelling §3.4's static-access example now uses. |
| B42 | 128 per-span `Read*` / `TryRead*` byte accessors declared on four span types | **32 members** on one `extension IReadOnlySpan<byte>` (stdlib §4.14, §4.15, §4.16); the **write** tier stays per writable span; the bit tier's records stay mirrored. The accessors are **not `pure`** | v0.290 | 8838 | `ReadUInt\|ReadInt\|TryRead\|extension[[:space:]]+IReadOnlySpan` | Call sites are unaffected; a proposal that *enumerates the per-type declarations* is stale. |
| B43 | Every enum had **one** compiler-provided static, `FromUnderlyingOrNull` | **three**: `FromUnderlyingOrNull(value: U): E?`, `FromNameOrNull(name: string): E?`, `NameOf(value: E): string` (spec §10.4, stdlib §9.9), unoverridable by a member **or a declared constant**; `E.NameOf(E.A) = nameof(E.A)`; laws hold up to aliasing under duplicate constant values | v0.293 | 8902, 8910 | `FromNameOrNull\|NameOf\|FromUnderlyingOrNull` | `nameof(` is a different operator. A `serialized "<name>"` rename is the **encoded form only** — `NameOf`/`FromNameOrNull` never see it. |
| B44 | `FromCollectionLiteral` recognized **by name** (a `public static` with one of two recognized signatures) | conformance to `ICollectionLiteralTarget<TSelf, T>` (borrowing, `ReadOnlyArraySpan<T>` carrier) or `IOwningCollectionLiteralTarget<TSelf, T>` (owning, `owned OwningArray<T>` carrier), stdlib §2.19; the member's name and signature are unchanged, but the class header needs the clause. The ten built-in sequence collections declare it and list `FromCollectionLiteral` as a member (§4.4–§4.11) | v0.294 | 8924, 8936 | `FromCollectionLiteral\|ICollectionLiteralTarget` | See **C29** — proposals that cite `FromCollectionLiteral` as a *by-name recognition mechanism precedent* (0053 does, three times) are relying on a retired premise, not just a stale spelling. |
| B45 | `Semaphore(initial, max)` — `max` had no stated meaning | `max` is a construction-time permit ceiling: `ArgumentException` when `max < 1` or `initial > max`; no run-time enforcement thereafter. The one-argument form has no ceiling | v0.302 | 9165 | `new[[:space:]]+Semaphore\(` | Low. |
| B46 | `SortedDictionary<K,V>` as a **dictionary-literal target**; the `Single` type name | both **removed** (impl-ahead removals; spec text unchanged) | v0.288 | 8784 | `SortedDictionary[^ ]*[[:space:]]*\[\|\bSingle\b` | Very high on `Single`: the LINQ operator `Single()` and the English word. Only the *type name* `Single` is gone. |
| B47 | `TextReader.Read(): int` | unchanged, but now carries a **recorded-misfit flag** (stdlib §7.4); the `char?` change is `rejected` with the member's own graduation as the bar | v0.290 | 8840 | `TextReader` | Informational. |
| B48 | `Parallel` (stdlib §6.12) and the bit tier's `TryWrite*` | unchanged, but now carry **spec-ahead-of-implementation markers** (#3557, #3558) — the whole `Parallel` class is unimplemented, kept on its recorded design weight | v0.284 | 8658 | `Parallel\.` | Informational; a proposal that *builds on* `Parallel` should say so. |
| B49 | `String.CompareTo(other, ignoreCase)`; `Binary.Parse<T>`/`ParseOwned<T>(ReadOnlyArraySpan<byte>, ignoreUnknownFields)`; `BinaryDeserializer(ReadOnlyArraySpan<byte>, ignoreUnknownFields)`; named arguments at **every** defaulted position | newly defined | v0.290 | 8845 | — | Additions. |
| B50 | `ReadOnlyAsciiByteSpan` (retired v0.188, pre-range) | stdlib §3.4 keeps the **tombstone section** — heading shortened to `(retired)` at v0.306, body citation intact. It tells a reader the type does not exist and what to use instead | v0.306 | 9257 | `ReadOnlyAsciiByteSpan` | A hit is *not* automatically stale — check whether the proposal is citing the tombstone. |

---

### §C Changed rules a proposal's design may rely on

One line each: the rule as it now stands, and the keywords a lane greps a proposal for to detect reliance.

| # | Rule now | Rev | Changelog line | Detect reliance by grepping for |
|---|---|---|---|---|
| C1 | **A closure is a borrow-carrying value, admitted wherever such a value is** — §28.3's closed three-form closure-capture list (`using var`-bound / immediately invoked / call-bounded) is **replaced** by the source-set rule; the storage and return sinks stay closed to any function value with a non-empty source set | v0.298 | 9020, 9027 | `immediately invoked`, `call-bounded`, `three forms`, `closed list`, `closure-capture` |
| C2 | A closure's **source set** has stated provenance: traced local, invocation result (carrying the invoked value's set), field/collection read (empty), otherwise unannounced | v0.298 | 9027 | `source set`, `provenance`, `capture set` |
| C3 | **A call through a function-typed value admits no named argument** (§27.1) — `caller(b := 2, a := 100)` is a compile error, not a positional bind | v0.295 | 8953, 8959 | `named argument`, a `:=` inside a call on a function-typed local/parameter/field |
| C4 | **Calls-mint unification**: a call whose declared, pre-substitution return type is *mint-announcing* — Task-family **or** iterable-family (the eight §29.3/§30.7 heads, or a lazy query) — produces a caller-owned frame at **every** callee kind. Axiom **A2**'s task/iterable split retires to a tombstone | v0.258 | 7695 | `mint-announcing`, `axiom A2`, `announcement table`, `calls mint` |
| C5 | The stored-collection-view idiom respells: `Items(): IEnumerable<Item> => return _items;` → `public readonly property Items: IEnumerable<Item> => _items;` (axiom A4's lend-only accessor guarantee becomes load-bearing) | v0.258 | 7695 | `): IEnumerable<`, `=> return _`, `stored collection view` |
| C6 | The v0.223 **owning-head declaration-site rejection** and its `Drain()` / `ReceiveAll()` §29.11 affine carve-out **retire** | v0.258 | 7695 | `Drain()`, `ReceiveAll()`, `affine carve`, `owning head` |
| C7 | v0.256's #3246 **function-slot iterable fence** is **subsumed and its text removed**, not amended — a bare-return function slot at an iterable-head type, and the six §5 bare-slot members, are no longer fenced | v0.256 → v0.258 | 7649, 7695 | `#3246`, `function-slot`, `iterable-family instantiation` |
| C8 | The §35.4 *borrow-carrying census* is renamed **holds-borrows**; §9.7's field-level *borrow-carrying* predicate and §28.3's frame-descriptor use are deliberately kept as that phrase's only meanings | v0.257 | 7679 | `borrow-carrying census`, `census` |
| C9 | **`defaultable`** is a named §3.6 derived type property; three **announcement senses** (*target-announced*, *site-announced*, *type-announced*) and the kill table's three bits (*obligation-moves*, *flag-set*, *keyword-visible*) are named | v0.257 | 7678 | `defaultable`, `target-announced`, `site-announced`, `type-announced`, `kill table` |
| C10 | Every §9.7-derived borrow obligation keys on the **census**, not the classification label — an *owning* choice can carry borrows and picks up all of them | v0.252 | 7547 | `borrow-bearing choice`, `owning choice`, `§9.7` |
| C11 | A **move-only value payload** in a choice case binds: borrow in a borrowing match; **transfer at arm selection** in an owning match (positional and marked-named alike); the unmarked named slot keeps the borrow. `Ready(move conns = var b)` is admitted | v0.251 | 7528 | `move-only payload`, `owning match`, `named slot` |
| C12 | `old(E)` over a **move-only value operand** is a compile error at the contract clause; remedy is a copyable projection or a borrowing-match projection `old((case c of …))` | v0.253 | 7566 | `old(` |
| C13 | Reads through a §12.7 value-narrowing at an `Object`/interface location read the **box payload** (stated affirmatively); a box payload is fixed at boxing | v0.254 | 7593 | `narrow`, `boxing`, `is [` |
| C14 | **Value receivers are borrows** (spec §9.4, anchor `sec-value-receivers-are-borrows`); a member body may not store into the receiver's own storage | v0.255 | 7619, 7629 | `value receiver`, `mutating`, record accessor setters |
| C15 | `new` is **not** in §28.8.1's throwable inventory; a compound assignment whose desugared form is throwable (`/=`, `mod=`, checked `+=`) **is** an unrestored exception edge | v0.255 | 7619, 7620 | `throwable`, `§28.8.1`, `restoration` |
| C16 | A throwing class's **destructor body does not run on constructor unwind** (§22.9, §7.9 ground); §28.4 states a one-resource-per-owning-wrapper discipline | v0.256 | 7650 | `constructor unwind`, `destructor`, `half-constructed` |
| C17 | A `public` **explicit record field** is a named-slot target (§19.3) | v0.256 | 7651 | `named slot`, `named destructur` |
| C18 | `M(item := move x)` — a **named** transfer-marked argument is legal; `ref`/`out`/`scoped` stay positional-only, with the reason stated | v0.260 | 7770 | `positional only`, `named argument`, `move` at an argument |
| C19 | **Axiom A13 retires**: owning-inline-array whole-value reassignment takes per-slot **drop-on-overwrite** (reverse slot order, RHS-first, moved-from slots skipped). "Those slots leak" is false. Register: **ten live axioms, four tombstones** | v0.260 | 7769 | `A13`, `leak`, `inline array`, `reassignment` |
| C20 | Expanding **in-SCC instantiation edges** are rejected (§14.6, anchor `sec-recursive-instantiation`); a readonly bare-`T` static is classified per instantiation and warns at class-type substitutions (§22.12) | v0.262 | 7839, 7840 | `recursive instantiation`, `§14.6`, `bare T static` |
| C21 | A record or choice with an **`Object`- or interface-typed** field or payload is **borrow-bearing**, picking up §9.7, §30.1's async bars, non-sendability and §28.11's copy-slot obligation | v0.265 | 7940 | `Object?` / interface-typed components in declared records/choices |
| C22 | `byte? += 1` and every other non-`int` nullable compound assignment is accepted (§6.6) | v0.265 | 7942 | `+=` on a nullable |
| C23 | A record, choice or inline array with a **function-typed component**, at any depth, is **not sendable** (§30.14) | v0.266 | 7984 | `sendable`, `() ->` inside a record/choice declaration |
| C24 | Both **null-source admissions** of §9.7 read strictly — *every* reference-typed field null, not merely every borrow-carrying one | v0.266 | 7984 | `null source`, `static readonly` of a borrow-bearing record |
| C25 | A **self-returning enumerator's `return this;`** is admitted by name (§28.3 rows 2–3, axiom A15); an **unmarked mint-announcing return** is carved out of §11.2's marker rule | v0.266 | 7986 | `return this`, `GetEnumerator`, `owned` on a mint-announcing return |
| C26 | **Constructor-only records**: `default(T)` and `with` rejected; positional fields beside a clause-bearing constructor rejected; a `serialized optional` component whose type is **not defaultable** rejected; a failing `requires`/`ensures` during deserialization raises `SerializationException` with `ContractViolationException` in its `CauseChain` | v0.268 | 8051, 8052 | `constructor-only`, `with(`, `default(`, `serialized optional` |
| C27 | `Exception.ToString()` is declared, with a required emission | v0.268 | 8054 | `Exception.ToString` |
| C28 | A write to **another instance's** `readonly` storage from inside a constructor of the declaring type is rejected (the `readonly` context is per-instance); a `readonly` **auto-property** may not be written by both its initializer and a constructor body | v0.270, v0.271 | 8171, 8214 | `readonly`, constructor bodies |
| C29 | The **sequence-literal protocol is interface-based**, not by-name recognition. A proposal that cites `FromCollectionLiteral` as a *name-based recognition mechanism* precedent (0053 §4/§5/§13 does) is relying on a premise retired at v0.294 — the sanctioned forward path is now a **static-member interface** (§11.8) | v0.294 | 8924, 8945 | `FromCollectionLiteral`, `name-based recognition`, `recognized static factory`, `specifically-named public static` |
| C30 | Conformance to either literal-target interface **by a value kind** is rejected; a class declaring both, or one at two element types, is rejected; an **interface** type is never a target; the three retired §15.4.1 conjunct diagnostics fold into §11.2 conformance matching plus one declaration-site diagnostic | v0.294 | 8938, 8939 | `FromCollectionLiteral`, `must be public`, `exactly one may be declared` |
| C31 | **`import System.Collections.Linq;` is no longer required** — §2.3 auto-imports it, under the invariant *where the stdlib declares an extension over an always-available type, its module is auto-imported*. `IGrouping` and `IOrderedEnumerable` become bare-visible | v0.278 | 8496, 8526 | `import System.Collections.Linq`, `not auto-imported`, `requires import` |
| C32 | **`System.Collections.Generic` is auto-imported**; every stdlib §4 name is bare-visible; §2.3 states its cost at twenty-nine names | v0.289 (L3) | 8816 | `import System.Collections.Generic`, `explicit import` |
| C33 | A **homogeneous** `=`/`<>` overload strips **auto-hashable**; a **heterogeneous** one (`T = U`) does not. A body record with a homogeneous overload renders by §8.5's qualified-name default, not `"TypeName(Prop: value)"` | v0.276 | 8393, 8440 | `auto-hashable`, `operator =`, `ToString` |
| C34 | §35.2's boxable-classifications clause scopes **replacement, not declarability** — `Equals`/`GetHashCode`/`ToString` are declarable on **every** choice classification | v0.285 | 8676 | `boxable`, `owning choice`, `§25.2` |
| C35 | **Operator operand positions and the comparison surface's `Equals(T)` parameter bind borrows** of move-only values; both operands live after | v0.286 | 8684 | `operator =`, `Equals(other:`, move-only comparison |
| C36 | `n += 1` / `n++` on a §7.4-**narrowed** nullable local is accepted; `override` on an enum's or choice's `Equals`/`GetHashCode`/`ToString` is accepted; oversized unsuffixed literals are admitted at collection-literal elements, record positional arguments, default arguments and enum constant values; `I(e)` works for every integer target; `enum E: Int32` and siblings are ratified; `FullName` renders `"M.Outer.Inner"` for nested types | v0.288 | 8785 | `narrowed nullable`, `Int32`, `FullName`, oversized literals |
| C37 | A **fresh class-typed key** stored into an `OwningDictionary<K,V>` (`od[new K()] := v`) draws a new warning | v0.288 | 8786 | `OwningDictionary`, `new` inside an indexer store |
| C38 | §22.12's statics rules 2–5 become a **decision table** (rule 3 reworded); §29.2's six "direct indexed loop" paragraphs become one table. No verdict moves, but any quotation of those paragraphs is now unquotable | v0.291 | 8872 | `§22.12`, `§29.2`, `direct indexed loop`, `statics rules` |
| C39 | A generic-method candidate whose constructed parameter type an argument matches at **more than one** implemented instantiation is rejected; a **non-call lambda literal** at a typed local is admitted (§27.3), with an ascription edge (`lambda(x): int => x * 2` at `(int) -> long` is rejected where the unascribed form is admitted); `DateTime` range faults are `OverflowException` | v0.292 | 8892 | `unify`, `lambda literal`, `ascription`, `ArgumentException` on `DateTime` |
| C40 | A **method-reference** argument's exact/converting ranking at an overloaded callee; a **parameter-count mismatch** at a function-typed slot makes a candidate *inapplicable*, not an error; an `async` lambda's wrapper follows a **three-source** rule (§30.6); a result-discarding lambda at a `-> ()` slot ranks *converting*; the **written-declared-type** position family is named once (§17.7) | v0.296 | 8981 | `overload`, `method reference`, `async lambda`, `parameter count`, `new()` at a return |
| C41 | §9.8's **serialized-name uniqueness** reaches enum constants and choice cases (per-case scope, ordinal comparison); §31.1's *may outlive the arm* is §28.3's rule with the arm as the source's scope; §22.7 rule 1 says **declaring type** | v0.297 | 9005 | `serialized "`, `catch` arm closures, `§22.7` |
| C42 | The non-auto-hashable **use-site error is per member**; a `serialized skip` component still binds its constructor parameter (`default(T)`); a nested record/choice serialization component must declare the conformance itself (enums need not); the **trivial-dispose ceremony exemption is over the obligation**, at every holder form, keyed produced-type-where-known and declared-type otherwise — interfaces never exempt; §28.10's borrow-pin drops *"not disposed"*, so `using var pin := w; w.Dispose();` is admitted; definite assignment's **local** half is keyed on **defaultable**, not nullability; a second `Dispose()` on the same binding **warns** | v0.299 | 9065–9068 | `equality skip`, `serialized skip`, `trivial dispose`, `must-use`, `borrow pin`, `unassigned local` |
| C43 | A class declaring **no instance constructor** has an implicit public parameterless one satisfying `where T: new()` (stdlib builtins carved out; **abstract and `closed` classes excluded** at v0.302); reverse **slot** order at all four endings of an owning inline array; `Clear()` disposes in reverse order; an unmarked `using var` over a **move-only** local adopts and disposes at **its own** block's end; an expression-position `case` over a non-nullable record with a total destructure and **no `_` arm** is admitted; `return null;` under `owned T?` is admitted; a record body constructor **need not chain**; a doubled generic conformance (`: IComparable<int>, IComparable<string>`) is admitted; a parameter is an assignment target; an empty method body is admitted | v0.300, v0.302 | 9109–9111, 9155 | `where T: new()`, `using var` over an inline array, `with(`, `_ =>` arms, `Clear()`, `chain` |
| C44 | **Every required parameter must be bound** (an unbound one makes the candidate *inapplicable*, so a sibling overload still resolves); §17.13's Task-discard prohibition **strips nullability** (`_ := someNullableTask;` over a `Task?` is the same error); §28.11's disposal order is reverse **storage** order, per family — *"reverse-insertion"* is **retired** except for `Channel<T>`; §9.9 — a constructor-only record's **local** declared with no initializer is **legal** (§7.9 governs the read, not the declaration) | v0.302 | 9157–9161, 9153 | `reverse-insertion`, `Task?`, `_ :=`, `required parameter`, `constructor-only` |

**Further class-C rulings with no separate row, listed so the range is complete:** §16.3's `Range<T>`
restriction against `ISteppable<T>` and the declined unbounded tier (v0.292); §14.1's exact-arity rule,
§26.4's function-slot exclusion, the **inert `static invariant` must warn** ruling, §28.2's
dominating-kill-warns / conditional-kill-errors ruling, and §14.5's nullability-is-part-of-a-binding
clarification (all v0.303, lines 9187–9197); §9.7/§28.3's **closed three-form** admitted set for a
borrow-bearing record return (receiver-sourced, static-storage-sourced, null-source; a **plain
parameter** is rejected), §19.3/§19.6's named destructuring over a class-typed scrutinee, §29.11's
closed suppression set with its spent-enumeration-right condition, the **unreachable `catch` arm
warning**, the borrow-source-keyed captured-constructor-parameter exclusion, a choice's `ToString`
(value-like auto-hashable → **unqualified** head; every other classification → **no default**, a
use-site error), §21.10's member-signature prohibition binding only enclosing and sibling
declarations, **no must-use warning at an owning inline array's auto-drop**, and the ruling that a
**consuming call that raises leaves the binding moved-from on the exception edge** (all v0.301,
lines 9125–9145).

---

### §D Moved / renumbered sections, grammar productions and anchors

The changelog asserts *"no section numbers move"* at almost every revision in this range, so §D is
short by design. What follows is every recorded move, deletion or addition.

| # | Old | Current | Rev | Changelog line | Stable anchor / note |
|---|---|---|---|---|---|
| D1 | Appendix **A.6** production `WhenStmt` | deleted | v0.289 | 8821 | — |
| D2 | Appendix **A.7** productions `WhenExpr`, `WhenArm` | deleted | v0.289 | 8821 | — |
| D3 | Appendix **A.4** alternative `"out" "owned" "var"` | deleted | v0.289 | 8821 | — |
| D4 | Appendix **A.4** operator tokens `"++" \| "--"` | deleted | v0.289 | 8821 | Appendix **B.1** unchanged — `when` stays reserved |
| D5 | Appendix **A.3** `PolyCase ::= StringLiteral ":" TypeName` | `PolyCase ::= StringLiteral ":" QualifiedName` (so a polymorphic registry cannot name a generic instantiation) | v0.269 | 8101 | — |
| D6 | Appendix **A.4** `ArgModifiers`, `SetterParamSpec`/`ParamMod`, **A.3** `ChoiceMember` restriction-restating productions | replaced by the shared `Member` production (0122 L12; net −5 productions) | v0.287–v0.288 | 8734 | Conformance cases `spec_0320`, `spec_0321` re-transcribe them |
| D7 | Appendix **A.7** `MoveExpr` commentary: *"positional only (§22.5)"* | *"named or positional alike"* | v0.260 | 7770 | Comment inside an unchanged production |
| D8 | Appendix **A** (spec) | gains the **grammar-foreclosed-prohibition index** table | v0.300 | 9116 | New table, no section renumber |
| D9 | Appendix **A** over-admission index | one new row, one amended row (enum/choice, record `Member` with `invariant`) | v0.297, v0.299, v0.303 | 9011, 9073 | — |
| D10 | *(new)* spec §9.4 | anchor **`sec-value-receivers-are-borrows`** | v0.255 | 7629 | Rationale twin: `sec-value-receivers-are-borrows-rationale` (§9.13) |
| D11 | *(new)* spec §14.6 | anchor **`sec-recursive-instantiation`** | v0.262 | 7848 | — |
| D12 | spec §9.2 | gains a **new anchored subsection** for the value-kind dispatch-modifier rule (moved out of the equality subsection at the gate's instruction) | v0.297 | 9011, 9013 | Re-check any `§9.2` sub-citation |
| D13 | anchor **`sec-the-scrutinee-less-form`** | **mis-targeted** — five pinned changelog entries point at it and it no longer names the `when` form (#3846) | v0.289 | 8823 | A proposal citing §19's scrutinee-less form has no live target |
| D14 | *(new)* stdlib §4.14, §4.15 | anchors **`sec-bit-granular-accessors`**, **`sec-array-bit-granular-accessors`** | v0.264 | 7911 | — |
| D15 | stdlib **§2.19** | retitled and reopened — now declares the two static-member collection-literal interfaces | v0.294 | 8944 | — |
| D16 | stdlib **§9.9** heading: *"Enum types: the compiler-provided `FromUnderlyingOrNull`"* | *"Enum types: the compiler-provided `FromUnderlyingOrNull`, `FromNameOrNull` and `NameOf`"* | v0.293 | 8915 | Heading text changed; anchor stable |
| D17 | stdlib **chapter 5** heading and Contents entry | now `System.Collections.Linq (auto-imported)`; the chapter's opening paragraph rewritten | v0.278, v0.299 | 8526, 9073 | — |
| D18 | stdlib **§3.4** heading `ReadOnlyAsciiByteSpan (retired v0.188)` | `ReadOnlyAsciiByteSpan (retired)` — section kept in full as the tombstone | v0.306 | 9257 | — |
| D19 | stdlib **§4.10**'s *"The v0.259 rename (`Take`/`TryTake` → `Pop`/`TryPop`)"* paragraph | **moved to rationale §15.z** | v0.306 | 9256 | A proposal citing "stdlib §4.10's rename record" is dangling |
| D20 | rationale **§17.12** (comparer-injection record, mis-inserted under the contract-carrying-type-aliases anchor) | the comparer record is **rationale §17.14**; §17.12 remains *contract-carrying type aliases*; all citations renumbered | v0.282 | 8624 | Anchor-hijack class; two `DECLINED.md` rows were silently re-pointed and fixed |
| D21 | *(new)* rationale sections in range | **§3.15** (unit type, v0.283), **§5.29** `sec-destructor-body-not-on-ctor-unwind` (v0.256), **§7.6** `sec-equality-operator-strips-auto-hashable` (v0.276), **§9.13** (value receivers, v0.255), **§11.8**/**§17.11** (supersession confrontation; `defer` decline, v0.259), **§12.15** (statics table, v0.291), **§15.y** (overload-pair rule, v0.290), **§15.z** (Pop rename, v0.306), and v0.263's four: `sec-no-bit-syntax-patterns`, `sec-recovery-unit`, `sec-contract-carrying-type-aliases`, `sec-ambient-authority` | v0.255–v0.306 | 7658, 7880, 8393, 8624, 8850, 8872, 9256 | — |
| D22 | Guide 01's Quick reference §10 | **§11** — a new §10 *Cleanup that must not run on the success path* was inserted | v0.259 | 7747 | Zero cross-document references to the old number at landing |
| D23 | Guide 03 §2 | rewritten for the conditional atom (the `when`-ladder teaching is gone) | v0.289 | 8819 | Guide 03's pin is v0.289 |
| D24 | `DECLINED.md` Part 2 register: D-0004's three stacked declined-paragraphs | **D-0005** (value-form try expression), **D-0006** (comprehensions), **D-0007** (delegation-by-…); **D-0009** registered at v0.294 | v0.263, v0.294 | 7874, 8944 | Cite `DECLINED.md` rows **by subject, not by number** (v0.271's own rule) |
| D25 | Archived per-revision filenames `Bezel_Language_Spec_0_NNN.md#sec-…` | resolve only in git history — filenames have been stable since v0.131 | standing | — | Zero hits in the current proposals tree; listed so a lane recognizes one |

---

### §E Document-structure changes

| # | Change | Rev | Changelog line | What can be dangling | Grep |
|---|---|---|---|---|---|
| E1 | The **spec's 238 inline revision markers** are removed — `(v0.289)`, `(v0.265, [#3278](../…))`, `(v0.250 — …)` and a dozen other shapes. Issue links and proposal references sharing a marker's parenthetical are **kept**. Three historical clauses were rewritten rather than stripped (B.2's *"unconditionally reserved since v0.260 — previously contextual"* → *"unconditionally reserved rather than contextual"*; its B.2 twin; §28.7's *"the owning choice has had it since v0.200"*) | v0.304 | 9213–9215 | Any proposal **quoting a spec sentence verbatim** with an inline `(v0.NNN)` no longer matches the live text | `“[^”]*v0\.[0-9]+[^”]*”` over quoted spec text |
| E2 | The spec's **loaded and colloquial vocabulary** is normalized: `footgun` ×4, `blessed`, *"implicit magic"* → *"implicit binding"*, *"the single most dangerous primitive"* → *"the highest-risk primitive"*, *"clearly-defined source"* → *"well-defined source"*, `simply` ×16 deleted, three second-person uses made impersonal, six contractions expanded. **`no-hidden-magic`** (a named principle) and `trivially` ×9 and `just` ×9 deliberately **kept** | v0.304 | 9216–9218 | Verbatim quotations of any of those sentences | `footgun\|blessed\|implicit magic\|most dangerous primitive` |
| E3 | The **stdlib's inline markers 552 → 88**; `footgun` → *genuine hazard*, *"compiler magic"* → *"compiler intrinsic"*, `simply` ×5 deleted. Three classes kept: changelog/rationale **citations**, **referring uses** (*"generalizing the v0.148 ruling"*), and cross-document citations already carrying a section (which lose only the revision) | v0.305 | 9233–9237 | Verbatim stdlib quotations carrying a marker | same as E1, over stdlib quotes |
| E4 | The **stdlib's markers 88 → 10**, and its **design narrative and revision biography move out**: §4.10's `Take`/`Pop` rename argument → **rationale §15.z**; `ReadOnlySpan<T>`'s `ToArray()` biography → a **`DECLINED.md` row** (§4.14/§4.15 now state the current position and point at it); the *"v0.269 rewrote the async examples off a hypothetical `HttpClient`"* note and *"the category was written at v0.102, before choices existed"* **deleted**; headings lose their dates (`### 4.18 SortedSet<T> (added in v0.136)` → `### 4.18 SortedSet<T>`) | v0.306 | 9251–9256 | A proposal citing **"stdlib §X's history"**, a dated stdlib heading, or the §4.10 rename record | `stdlib §[0-9.]+[^.]{0,30}(history\|record\|rationale)`, `added in v0\.`, `retired v0\.` |
| E5 | **v0.269's spec example sweep**: 57 fenced examples edited, ~68 defects fixed, **44 conformance cases invalidated**; `import` comments added wherever a name resolves only through one (§26.1's `ToArray` gained `import System.Collections.Linq;`, §10.4's `FrozenArray` likewise); §9.2's body-record member admission list corrected to *"auto-properties, methods, expression-bodied properties, constructors, and operator overloads"* | v0.269 | 8101, 8102, 8106 | A proposal quoting a pre-v0.269 spec example | — |
| E6 | **v0.274's stdlib + rationale audit** (176 blocks, ~51 findings) and **v0.275's rulings** — `new Stream()` has zero stdlib hits; the FACTS check was structurally blind to the stdlib's duplicate of the synchronized set, fixed | v0.274, v0.275 | 8296, 8344 | Proposals that quote stdlib prose written before v0.274 | — |
| E7 | **`Bezel_For_Code_Generation.md`** is re-pinned nearly every revision and is at **v0.306**. `check_docs.sh`'s PIN check **fails** past three revisions behind, and since v0.302 all **three** of its pins (validation, citation basis, and the §5 stdlib-name-index transcription line) move together or the gate fails | v0.269 → v0.302 | 8104, 9175 | A proposal citing the reference without a revision | `Bezel_For_Code_Generation\|code-generation reference` |
| E8 | `check_docs.sh` gained **PIN** and **FENCE** (v0.269), **TABLE** for §3.6 (v0.257) and for proposals (v0.267), the blank-line clause-region **style check 3d** (v0.260), and **SETS** (v0.275). It now runs **eleven checks** | v0.257–v0.302 | 7687, 7767, 8104, 8368 | Proposals describing the doc gate | `check_docs\|check_proposals` |
| E9 | `DECLINED.md` grows **176 → 464+** rows in range (181 at v0.264, 193 at v0.267, 207 at v0.268, 410 at v0.269, 446 at v0.289, 464 at v0.290) and is renumbered twice. **Cite rows by subject, not number** | v0.263 → v0.290 | 7880, 8105, 8818, 8850 | Any `D:NNN` or "row N" citation | `D:[0-9]+\|DECLINED\.md[^)]{0,20}row` |
| E10 | The **conformance corpus** moved from 571 hand-annotated cases (v0.256) to **2,148** cases with a **rule-keyed** track under `tests/conformance/rules` (landed separately, indexed by #4383, v0.299). Case ids are **positional** and renumber on regeneration | v0.299 → v0.306 | 9041, 9117 | Proposals citing a `spec_NNNN` / `stdlib_NNNN` id | `spec_0[0-9]{3}\|stdlib_0[0-9]{3}\|codegen_0[0-9]{3}` |
| E11 | **Audit and review artifacts** added beside the numbered proposals: `0118-audit-v0.281.md`, `audit-0122-v0.291.md`, `elegance-review-2026-09.md`, `subtraction-review-2026-08.md`, `catchup-run-sequence-2026-08-12.md`, and this range's currency papers. The README's supporting-records table carries them | v0.260 → v0.291 | 8580, 8860 | — | — |
| E12 | **Thirty proposals graduated in range** — 0082 (v0.249), 0091/0092 (v0.250), 0093/0095 (v0.257), 0096 (v0.258), 0097/0098/0099 (v0.259), 0100/0101/0102 (v0.260), 0094 (v0.261), 0109 (v0.264), 0114 (v0.267), 0107/0112/0113 (v0.268), 0115 (v0.272), 0116 (v0.277), 0117 (v0.278), 0118 (v0.281), 0119 (v0.282), 0120 (v0.283), 0121 (v0.284), 0122 (v0.289), 0123 (v0.290), 0124 (v0.293), 0125 Part A (v0.294), 0126 (v0.298); 0103/0104/0105/0106 landed `accepted` with staged content | v0.249–v0.298 | various | A live proposal describing any of these as *pending*, *proposed* or *open* | `proposal[s]? \[?01(0[0-9]\|1[0-9]\|2[0-6])\]?\|008[2]\|009[1-9]` |
| E13 | **0125 Part B is deferred, not landed** — `IDeserializable` stays the marker interface it is; the generic `IDeserializable<TOut>` design sits behind a three-item bar | v0.294 | 8947 | Proposals assuming a generic `IDeserializable<TOut>` | `IDeserializable` |
| E14 | **0122 L17 was withdrawn at graduation**; its three questions stay open as #3821 and were answered editorially at v0.296 (§17.7's written-declared-type family). **L14–L16** landed at v0.291 | v0.289, v0.291, v0.296 | 8822, 8860, 8981 | Proposals citing L17 as pending in 0122 | `L1[4-7]\|#3821` |

---

### §F Quick-scan recipe

Save as `scan.sh` and run from the repository root. `$1` is a file list or glob; it defaults to every
proposal. **Reading and grepping the tree is all this does — it edits nothing.**

```bash
#!/usr/bin/env bash
## currency-scan v0.246 -> v0.306.  Run from the repo root.  $1 = file glob (default: all proposals)
P="${1:-docs/proposals/*.md}"
run(){ printf '%-6s %s\n' "$(grep -nEc "$2" $P 2>/dev/null | awk -F: '{s+=$NF}END{print s+0}')" "$1"; }
run "A1 when-construct"        '\(when[[:space:]]|^[[:space:]]*when[[:space:]]+[^=]*=>|end[[:space:]]+when|WhenExpr|WhenStmt|WhenArm'
run "A2 out owned var"         'out[[:space:]]+owned[[:space:]]+var'
run "A3 ++/-- ops"             'operator[[:space:]]+(\+\+|--)|[A-Za-z0-9_)\]]\+\+|\+\+[A-Za-z0-9_]'
run "A4 reserved ids"          '\b(requires|ensures|invariant)[[:space:]]*(:[^=]|,|\)|\])'
run "A5 nullable lifting"      '[A-Za-z0-9_]\![[:space:]]*$|[a-z]\?[[:space:]]*(\+|-|\*|/|<<|>>|<=|>=|&|\||\^)[[:space:]]'
run "A6 using var _ := task"   'using[[:space:]]+var[[:space:]]+_[[:space:]]*:='
run "A7 value-kind dispatch"   '(virtual|abstract|sealed[[:space:]]+override)[[:space:]]+(method|property|indexer)'
run "A8 multi-param where"     'where[[:space:]]+[A-Za-z_]+[[:space:]]*:[^,]*,[[:space:]]*[A-Za-z_]+[[:space:]]*:'
run "B1 task aggregators"      'WhenAllOwned|WhenEachOwned'
run "B2 set egress"            '\.(Take|TryTake)\(|OwningSet<[^>]*>\.(Take|TryTake)'
run "B3 retired .NET duals"    '\.Restart\(|IsNullOrEmpty|[Ss]tring\.Compare\(|\.Implements\(|Console\.Clear|OfType<'
run "B4 Math surface"          'Math\.(Sin|Cos|Tan|Asin|Acos|Atan2?|Sinh|Cosh|Tanh|Exp|Log10|Log2|Log|Hypot|Cbrt|PI|E)\b|Math\.Sign'
run "B5 Counter membership"    'ContainsKey'
run "B6 window params"         '(count|index|startIndex)[[:space:]]*:|GetString\([^)]*,[^)]*,|String\.(From|Remove)\([^)]*,[^)]*,'
run "B7 span carrier/ToArray"  '(^|[^A-Za-z])ReadOnlySpan<|ReadOnlyAsciiByteSpan|\.ToArray\('
run "B8 aggregation/accessors" '\bSum\(|\bAverage\(|ElementAt|SequenceEqual'
run "B9 misc removed"          'const[[:space:]]+Zero|DangerousPointer|SortedDictionary|\bSingle\b|Type\.Implements'
run "C1 closure form list"     'immediately[[:space:]]+invoked|call-bounded|three[[:space:]]+forms|closed[[:space:]]+(three-form[[:space:]]+)?list'
run "C2 census vocabulary"     'borrow-carrying|\bcensus\b|equality-bearing|reverse-insertion|FFI-pointee'
run "C3 mint/announcement"     'mint-announcing|announcement[[:space:]]+table|Drain\(\)|ReceiveAll\(\)|axiom[[:space:]]+A(2|13)\b'
run "C4 literal protocol"      'FromCollectionLiteral|ICollectionLiteralTarget|collection[- ]literal'
run "C5 named args / imports"  'named[[:space:]]+argument|import[[:space:]]+System\.Collections\.(Linq|Generic)|not[[:space:]]+auto-imported'
run "C6 new()/dispatch/order"  'where[[:space:]]+[A-Z][A-Za-z0-9_]*[[:space:]]*:[[:space:]]*new\(\)|reverse[- ]index|auto-hashable|defaultable'
run "D1 archived filenames"    'Bezel_(Language_Spec|Standard_Library|Design_Rationale)_0_[0-9]'
run "D2 moved anchors/§§"      'sec-the-scrutinee-less-form|§19\.[1-7]|§22\.4\b|§15\.4\.1|stdlib §(3\.4|4\.10|5\.1|9\.9)|rationale §17\.1[23]|A\.(4|6|7)'
run "E1 derived doc pins"      'Bezel_For_Code_Generation|code-generation reference'
run "E2 stdlib history cites"  'stdlib §[0-9.]+[^.]{0,30}(history|record|rationale)|see the changelog|changelog entry'
run "E3 DECLINED row numbers"  'DECLINED\.md[^)]{0,20}(row|#)[[:space:]]*[0-9]|D:[0-9]+'
```

**Measured hit counts** (run against this tree; matching *lines*, so a line with two hits counts once
per pattern). Column 1 is all **159** files in `docs/proposals/`; column 2 is the **47 live** files
only — which is the number a sweep lane should budget against.

| Pattern | all 159 | live 47 |
|---|---:|---:|
| A1 when-construct | 26 | **0** |
| A2 out owned var | 22 | **3** |
| A3 ++/-- ops | 11 | **1** |
| A4 reserved ids | 56 | 12 |
| A5 nullable lifting | 84 | 7 |
| A6 using var _ := task | 17 | 1 |
| A7 value-kind dispatch | 29 | 5 |
| A8 multi-param where | 5 | **0** |
| B1 task aggregators | 28 | **1** |
| B2 set egress | 15 | 1 |
| B3 retired .NET duals | 28 | 1 |
| B4 Math surface | 9 | **0** |
| B5 Counter membership | 21 | 3 |
| B6 window params | 75 | 22 |
| B7 span carrier/ToArray | 204 | 38 |
| B8 aggregation/accessors | 58 | 2 |
| B9 misc removed | 116 | 19 |
| C1 closure form list | 88 | 25 |
| C2 census vocabulary | 187 | 11 |
| C3 mint/announcement | 132 | 15 |
| C4 literal protocol | 217 | 8 |
| C5 named args / imports | 39 | 11 |
| C6 new()/dispatch/order | 107 | 13 |
| D1 archived filenames | 0 | **0** |
| D2 moved anchors/§§ | 551 | 43 |
| E1 derived doc pins | 46 | **0** |
| E2 stdlib history cites | 121 | 9 |
| E3 DECLINED row numbers | 20 | **0** |

**Reading the numbers.** A1, A8, B4, D1, E1 and E3 are **clean over the live set** — the `when`
construct, the multi-parameter `where`, the retired `Math` surface, archived filenames, the
code-generation reference and numbered `DECLINED.md` rows appear nowhere live. The high-yield
targets are **A2** (3 real `out owned var` sites: 0009, 0068, 0069), **A3** (1 real `operator ++`
declaration: 0008 §262), **B1** (1 real `Task.WhenAllOwned` signature: 0001 §287), **C4/C5** (0053's
three `FromCollectionLiteral`-as-mechanism citations and 0026's *"not auto-imported"* claim about
`System.Collections.Linq`). **B7** at 38 and **D2** at 43 are mostly context, not defects.

To list the live set for the scan:

```bash
cd docs/proposals
LIVE="0001 0002 0004 0005 0006 0007 0008 0009 0010 0011 0012 0013 0014 0015 0016 0017 0018 0019 \
0020 0021 0022 0024 0025 0026 0027 0028 0029 0030 0031 0040 0047 0052 0053 0054 0060 0068 0069 \
0070 0071 0088 0103 0104 0105 0106 0108 0110 0111"
FILES=""; for n in $LIVE; do FILES="$FILES $(ls ${n}-*.md)"; done
bash scan.sh "$FILES"
```

---

### §G Known false-positive families

1. **English `when`.** The dominant A1 false positive is prose in parentheses — *"(when the closure
   …", "(when `using var h` is written …"*. Sixteen of the thirty-one files a naive
   `grep -rl '(when'` returns contain no Bezel `when` at all (0122 L1 measured this). The honest
   recipe is `grep -rlE '\(when(\s*$|\s*//|\s+.*=>)'`. **And two `when`s survive**: the arm guard
   (`case v of Some(x) when x > 0 => …`) and the `when true` / `when false` postcondition clause
   (§22.4). Only the ladder construct is retired.
2. **English `case`, `move`, `record`, `single`, `count`, `index`, `type`.** All are ordinary English
   and all appear in the retired-form patterns. `\bSingle\b` (B46) is the worst offender: the LINQ
   operator `Single()` is alive, only the **type name** `Single` is gone. `count:` and `index:`
   (B30) remain correct on `Stream`.
3. **`Owned` inside a type name.** `OwningArray`, `OwningSet`, `OwningBuffer`, `IOwningList`,
   `OwningTask`, `IOwningCollectionLiteralTarget` all contain the substring; only
   `Task.WhenAllOwned` / `Task.WhenEachOwned` were renamed. Anchor the pattern to the full member
   name.
4. **`ReadOnlySpan<T>` vs `IReadOnlySpan<T>` vs `ReadOnlyArraySpan<T>`.** All three are live types.
   `\bReadOnlySpan<` correctly excludes `IReadOnlySpan<` (no word boundary between `I` and `R`), but
   a pattern without the boundary matches 204 lines instead of 148. The only carrier that **moved**
   is §15.4.1's borrowing form; the only member that **left** is `ReadOnlySpan<T>.ToArray()`.
5. **Proposal numbers that look like section numbers.** `0122 §1`, `0123 R5`, `§8's seven open
   questions` — a `§[0-9]` pattern hits proposal-internal sections as often as spec sections. Anchor
   spec citations on the word `spec`/`stdlib`/`rationale` or on a `Bezel_*.md#sec-` link.
6. **Currency notes are deliberately historical.** Every file swept in 2026-07-28 or 2026-08-11
   carries a header `Currency note` that *names* the retired forms it replaced —
   `` `closed` replaces `sealed abstract` (v0.242) ``, `` `case move msg of` transfer-marker respell ``.
   Those 32 `sealed abstract` hits and most `owned`-marker hits are **records of the sweep**, not
   stale text. Do not re-edit a Currency note; add to it.
7. **`++` and `--`.** `--` collides with every command-line flag (`--release`, `--explain`,
   `--debug`, `--main-class`, `--import`) and with em-dash-adjacent markdown. Grep `\+\+` and
   `operator[[:space:]]+--` only; never bare `--`.
8. **`owned` in declaration positions is still correct.** Parameters, fields, returns, `set(owned)`
   and function-type slots keep `owned` (the v0.246 split). Only the **use-site** spellings moved,
   and in this range only `out owned var`. A pattern like `\([[:space:]]*owned[[:space:]]` returns
   151 lines and is 100% legitimate declarations.
9. **Contract keywords.** `requires`, `ensures`, `invariant` are reserved *and* ubiquitous —
   242 lines in the live set. Only an **identifier** position is retired.
10. **`Dispose()` calls.** A17's pattern must target `method Dispose(` declarations; `x.Dispose()`
    call sites are correct, and an **interface** declaring `Dispose()` became admitted at v0.301.
11. **`virtual` / `abstract` / `override`.** 104 live lines; the retirement is scoped to **value
    kinds** (records, enums, choices). Classes are unaffected.
12. **Everything under `docs/proposals/` that is superseded or declined is pinned history.** Only
    the 47 live files are in the sweep population; a hit in a graduated proposal is a record of what
    was designed at its pin and must not be edited.

---

### §H Migration paragraphs that read ambiguously

Three places where a sweep lane should not guess.

1. **v0.290 (line 8845), the `Encoding.GetString` retirement vs. the v0.282 rename.** The Migration
   bullet lists `enc.GetString(bytes, start, length)` → `enc.GetString(bytes.AsReadOnlySpan(start, length))`
   while the same revision's **R5 bullet** (line 8836) says §1's window paragraph *"keeps its v0.282
   rename frame (the three members renamed then were exactly three; the convention's instances are
   many more)"*. So within eight revisions the same three arguments were **renamed** (`count` →
   `length`, v0.282) and then the form was **retired outright** (v0.290). The gate itself flagged
   this as blocking finding **B2**: *"R5's window sentence had turned three v0.282 renames into a
   false claim about the convention's instances (nineteen `(start, length)`/`(startIndex, length)`
   signatures exist)"*. A lane respelling an old `GetString` call should go straight to the span
   form, **not** to the renamed windowed form. `String.From` and `String.Remove` keep their windows
   and keep the v0.282 names.

2. **v0.265 (line 7944), *"Withdrawn guidance: `ToArray()` is no longer a remedy"*.** The paragraph
   reads, in full: *"`ToArray()` is no longer a remedy for the fresh-yield liveness hazard at class
   element types (stdlib §5). Code written to the v0.235 sentence is already broken — the withdrawal
   documents the breakage rather than causing it — and the two working remedies are named in the same
   paragraph."* It never names the two remedies in the changelog, and v0.268 (line 8056) then records
   that v0.266's claim *"nothing in this revision touches what [Guide 5] teaches"* was false because
   **`OwningArray<T>.ToArray()` does exist** and *"its lifetime hazard was unnamed anywhere in the
   guide"*. The member exists and is sound as a borrowing snapshot; only the *guidance* was
   withdrawn. A grep hit on `.ToArray(` is therefore **never** mechanically respellable — it needs
   the element type.

3. **v0.276 (line 8437–8441) vs v0.285 (line 8676), auto-hashable and choices.** v0.276's Migration
   says *"Newly rejected: two shapes, and the set of newly non-compiling conforming programs is
   **not** empty … (i) A choice that overloads homogeneous `=`/`<>` without `Equals(T)` and
   `GetHashCode`"*, while v0.285's ruling then states that those members are **declarable on every
   classification** and that its own first draft *"had blessed the operator rescue on move-only
   choices without noticing that §25.2's never-consume operand rule and §15.3's by-value-move
   parameter rule give those comparison positions no consistent binding"*. The binding question was
   only settled at **v0.286** (borrows). A proposal that declares choice equality operators is
   therefore governed by three revisions at once; do not respell it from v0.276 alone.

*(A fourth, smaller one, recorded rather than listed: v0.300's `Clear()` disposal order is stated as
**reverse-insertion** at line 9111 and re-ruled as **reverse storage order** at v0.302, line 9161.
Take v0.302.)*

## 2. Lane A report — 0001–0016 (verbatim)

Population: `docs/proposals/` 0001, 0002, 0004, 0006, 0007, 0008, 0009, 0010, 0011, 0012, 0013, 0014, 0015, 0016 (14 files).
Input: `currency-inventory-v0.306.md` (§A–§H), read in full; §F scan run per file, then each file read.
Method and two-tier discipline follow `docs/proposals/papers/proposals-currency-2026-08-11.md` §2/§3.
`scripts/check_proposals.sh` passes after the edits (BIJECTION / TOKEN / SECTION / TABLE all clean).

Every file now carries a `**Currency note:**` in its header block; the written-against pins are unchanged.

---

### 1. Tier-1 edits applied (9 edits across 5 files)

Line numbers are post-edit.

#### 0001-networking-http.md (2)

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 12 | `*"`System.Net` and `System.Net.Http` — networking and HTTP. The language specification's async examples use a hypothetical `HttpClient`; a real one will live here."* (stdlib §11, L3808)` | `*"`System.Net` and `System.Net.Http` — networking and HTTP. A real HTTP client will live here."* (stdlib §11, v0.306)` | v0.269 → v0.306 | §E4 (stdlib revision-biography removal; the hypothetical-`HttpClient` clause left the sentence). Landed text verified at `Bezel_Standard_Library.md:5630`. Line-number citation dropped per the inventory's "cite § + revision" rule |
| 288 | `Task.WhenAllOwned<HttpResponse>(owned tasks: OwningArray<OwningTask<HttpResponse>>): OwningTask<OwningArray<HttpResponse>>` | `Task.WhenAll<HttpResponse>(owned tasks: OwningArray<OwningTask<HttpResponse>>): OwningTask<OwningArray<HttpResponse>>` | v0.250 | §B1. Current signature confirmed at stdlib §6.7 (`Bezel_Standard_Library.md:4239-4241`): `public static method WhenAll<T>(owned tasks: OwningArray<OwningTask<T>>): OwningTask<OwningArray<T>> where T: class;` — parameter list and return type unchanged, name only |

#### 0008-integer-toolkit.md (3)

All three are citations of spec §25.3's `Counter` record and its `operator ++` declaration. §25.3 at v0.306 contains `Vector2`/`BitMask`/`TriBool` and no `Counter`; §17.6 states `++`/`--` "are not user-overloadable ([§25.1]), so a type spells advancement as `x := x + 1` under its own `operator +`".

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 218 | ``each `Add` returns a **new** `HashCode` value (the §25.3 `Counter` idiom)`` | ``each `Add` returns a **new** `HashCode` value (§25.3's `Vector2` operator idiom — value in, fresh value out)`` | v0.289 | §A5 (+ deleted §25.3 example) |
| 262 | ``exactly §25.3's `Counter` (`operator ++(c: Counter): Counter => new Counter(c.Value + 1)`, which §17.6 calls "the typical record use")`` | ``exactly §25.3's `Vector2` (`public static method operator +(left: Vector2, right: Vector2): Vector2 => new Vector2(…)`), the value-record operator shape §17.6 now names as the way a type spells advancement — "a type spells advancement as `x := x + 1` under its own `operator +`" (`operator ++`/`--` declarations were retired at v0.289)`` | v0.289 | §A5 |
| 283 | ``(each `Add` returns a new value, §25.3 `Counter` idiom)`` | ``(each `Add` returns a new value, §25.3's `Vector2` operator idiom)`` | v0.289 | §A5 |

#### 0009-base64-hex.md (1)

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 151 | `if Base64.TryDecode(incoming, out owned var bytes) then` | `if Base64.TryDecode(incoming, out var bytes) then` | v0.289 | §A4. This is the site the inventory §F names ("A2 … 3 real `out owned var` sites: 0009, 0068, 0069"). Meaning preserved: spec §22.4 — "`out var x` declares a fresh local that **adopts** the classification the callee's declaration announces (§28.3's adoption clause) … against `out owned`, an **owned local**". The declaration-side `out owned result: Array<byte>?` is untouched, and `using var _ := bytes!;` is v0.244's kept anonymous owning binding (the 2026-08-11 paper's note †; §A9 retires only a Task-family RHS) |

#### 0011-process.md (1)

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 138 | ``Appendix A `PositionalField ::= Identifier ":" Type SerializedClause?` `` | ``Appendix A `PositionalField ::= Identifier ":" Type SerializedClause? EqualityClause?` `` | v0.299 | §D-class (grammar production changed; positional-field `equality skip`, cf. §C42). Landed production at `Bezel_Language_Spec.md:10857`. The point the quotation supports ("no default") is unchanged |

#### 0012-datetimeoffset-timezoneinfo.md (2)

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 17 | `"Records and enums may overload `=` and `<>`,"` | `"Records, enums, and choices may overload `=` and `<>`,"` | (choices joined the sentence with §35.2) | §E1-class verbatim-quotation re-derivation. Landed §25.2 text: "Records, enums, and choices may overload `=` and `<>` ([§35.2])" |
| 70 | ``§25.2 states "Records and enums may overload `=` and `<>`."`` | ``§25.2 states "Records, enums, and choices may overload `=` and `<>`."`` | same | same |

#### Files with zero tier-1 edits

0002, 0004, 0006, 0007, 0010, 0013, 0014, 0015, 0016 — audited, no retired spelling, no renamed/removed stdlib member, no dangling section or anchor. (0006, 0007, 0012, 0016 are **not** "clean" overall — they carry tier-2 flags below.)

**clean** (no tier-1 edit and no tier-2 flag): **0002, 0004, 0010, 0013, 0014, 0015**.

---

### 2. Tier-2 flags (8) — recorded, not fixed

| # | file | passage (quoted) | changelog line / revision | the decision the owner must make |
|---|---|---|---|---|
| A-1 | 0006 §5 + §6 Q1 + acceptance ruling Q1 | *"the `scoped`-borrow ctor lands later as a pure, non-breaking **second signature** (resolved by the `owned` vs `scoped` call-site marker) **once the §28.14 RFC amendment lands**. This is a **spec amendment** — not implemented autonomously per CLAUDE.md."* | v0.180, `Bezel_Changelog.md:6261` — "**Scoped construction lands (§28.14, §28.3, §22.3, §9.7, grammar; #1439, owner-accepted):** `scoped` on class/interface-typed parameters — the borrowed-stream (`leaveOpen`) mechanism… (The `StreamReader`/`StreamWriter` second signature rides proposal 0006's stdlib landing.)" Landed rule at spec §28.14, *"Class- and interface-typed `scoped` parameters: scoped construction"* (`Bezel_Language_Spec.md:7978`) | The amendment 0006 defers to **has landed** (and the changelog says the second signature rides *this* proposal's landing). Decide whether v1 now ships `constructor(scoped stream: Stream)` beside the `owned` ctor, or keeps the owned-only v1 cut with the §5 deferral re-grounded. *(Pre-range — v0.180, so absent from the inventory; surfaced by this sweep.)* Same passage also mis-cites the grammar: *"grammar A.6 already admits `scoped` on any parameter"* — A.6 is Statements; the production is `ParamModifiers` in **A.4**, which now spells the class/interface-typed admission explicitly (`Bezel_Language_Spec.md:11208-11213`) |
| A-2 | 0006 §3 Q5 | *"`TextReader.Read(): int` returns **−1 at EOF** … So EOF stays a sentinel — `null`/`−1`/`0` — exactly as §7 already establishes, and this is *consistent with* the Try convention rather than an exception to it."* | v0.290, `Bezel_Changelog.md:8840` (§B47). Landed stdlib §7.4: `public abstract method Read(): int; // Recorded misfit (§1's failure-signal rule): band 2 would spell this `char?`; the sentinel stands, **moved only by its own graduation ruling**` | The stdlib now carries a recorded-misfit flag on the exact member Q5 argues about, and names *its own graduation ruling* as the only thing that moves it. 0006 graduates the concrete `TextReader`. Decide whether 0006's graduation **is** that ruling (and so should restate or close the misfit), or whether the flag stays untouched and Q5 is simply re-affirmed |
| A-3 | 0007 §5.3 + §9 Q3 | *"**Spans across `await` — an inherited, unresolved tension…** `Stream` (§7.1) *already* declares `ReadAsync(buffer: Span<byte>, token)` / `WriteAsync(ReadOnlySpan<byte>, token)`, and `NetworkStream` inherits them."* and Q3's *"The question is moot once the #1438 span-overload removal (ruled 2026-07-04) reaches the docs."* | v0.179 (#1438). Landed stdlib §7.1 block declares **no** async span overloads, and carries the blockquote *"**No async span overloads (#1438).** Spans are borrow-bearing records and barred as `async` parameters (spec §30.1's borrow-bearing-parameter prohibition)…"* | Q3's own moot-condition is satisfied: the removal reached the docs. Decide whether to close Q3 and delete §5.3's premise sentence (the recommendation it supports — `Array<byte>` for all async socket I/O — is now the only expressible form), or to keep the paragraph as a pinned record. *(Pre-range — v0.179; not a §A/§B row.)* Note §4.2's `Read(Span<byte>) / Write(ReadOnlySpan<byte>)` comment is still correct — the **synchronous** span forms survive |
| A-4 | 0008 §5.4 (and §5.3, §7 item 7) | *"§9.4 is the relevant fact here: it (i) permits mutable body-record *properties*, (ii) defines **no** `mutating`-method receiver model (a grep of spec and rationale finds no value-receiver-mutation semantics at all), and (iii) explicitly calls mutable records **"inadvisable."**"* / §7.7: *"it is the one shape that depends on the **unspecified** value-receiver-mutation semantics"* | v0.255, `Bezel_Changelog.md:7619, 7629` (§C14, §D10). Landed spec §9.4 now carries the anchored subsection **`sec-value-receivers-are-borrows`**: *"In every instance member body of a **value kind** … `this` binds a **borrow of the receiver value**"*, and *"a store that would land in the **receiver's own storage** is a **compile error at the member body**"* | Clause (ii) has expired — the semantics are now specified, and they **reject** the mutable-value-record `HashCode` outright rather than leaving it unspecified. The chosen design (immutable chaining) is unaffected and in fact strengthened. Decide whether to re-derive §5.4 and §7 item 7 on the landed §9.4 rule (turning "unspecified, so avoid" into "specified, and rejected"), or leave the pinned reasoning and add a pointer. Rationale twin: §9.13 `sec-value-receivers-are-borrows-rationale` |
| A-5 | 0012 D3 + §4's `DateTimeOffset` block | D3: *"`CompareTo`, the `<`/`>`/`<=`/`>=` operator group, `IEquatable<DateTimeOffset>.Equals`, and the derived hash are all defined over the **instant**"*; §4 block comment: *"Operator group agrees with CompareTo (instant order), matching the §2.5 v0.165 pattern for DateTime."* | v0.274, `Bezel_Changelog.md:8300` (§B17) — *"`DateTime` / `TimeSpan` / `DateOnly` / `Guid` claimed a `<`, `>`, `<=`, `>=` operator group agreeing with `CompareTo` → **none of the four operators exists** — the claim was deleted; use `CompareTo`"*. Landed stdlib §2.5: *"Ordering is CompareTo only: DateTime declares no [relational operators]"* | The cited `DateTime` precedent no longer exists. Decide whether `DateTimeOffset` **declares** the `<`/`>`/`<=`/`>=` group itself (§25.2 requires the complete group, and §4's listed operator block currently declares only `+`/`-`, so the prose and the block already disagree), or whether it follows the landed temporal-cluster posture of CompareTo-only ordering. Not respellable mechanically: either answer changes the proposed surface |
| A-6 | 0012 D3 | *"the compiler-provided conformance overrides that with an instant-based `=`/`<>` overload (§25.2), and the hash is taken over the instant so the equality/hash contract holds."* | v0.276, `Bezel_Changelog.md:8393, 8440` (§C33) — a **homogeneous** `=`/`<>` overload strips **auto-hashable**; plus §25.2's **co-declaration rule** (a record overloading `=`/`<>` with both operands of the declaring type **must** also declare `Equals(T)` and `GetHashCode`) | `DateTimeOffset`'s overload is homogeneous, so it strips auto-hashable and triggers the compiler-enforced co-declaration rule, and §8.5's qualified-name `ToString` default replaces the record rendering. Decide that the compiler-provided conformance explicitly declares all three (`Equals(DateTimeOffset)`, `override GetHashCode`, `ToString`) and state it — D7 already pins `ToString`, so only the first two are unstated. (Favourable side-effect confirmed: §9.8's admissible-component rule now says explicitly that a homogeneous `=`/`<>` overload leaves the record **serialization-eligible**, `Bezel_Language_Spec.md:1541`.) See also §H3 — choice/record equality in this range is governed by v0.276 + v0.285 + v0.286 together, so do not re-cut from v0.276 alone |
| A-7 | 0016 §6 Q5 (and §4.1's `CompareTo` comment `// operator group agrees`) | *"**Q5 — `Instant` comparison operators?** `IComparable<Instant>` gives `<`, `>`, `<=`, `>=` through the standard operator group (spec §17, the IComparable convention)."* | v0.274, `Bezel_Changelog.md:8300` (§B17); landed stdlib §2.5 *"Ordering is CompareTo only"* | There is no "standard operator group through `IComparable`" — §25.2 requires the four relational operators to be declared as a complete group. Decide whether `Instant` declares them explicitly (and, per §25.2's co-declaration rule, what that implies for `=`/`<>`, which Q5 currently takes from §9.2 auto-equality), or ships CompareTo-only ordering like the rest of the temporal cluster |
| A-8 | cross-file: 0009 §2 (both `TryDecode`s, `TryFromHexString`), 0010 §2.14 delta (`TryReadBytes`), 0012 (`DateTimeOffset.TryParse`, `TryFindSystemTimeZoneById`), 0014 §3 (`TryReceive`) | e.g. 0009: `public static method TryDecode(s: string, out owned result: Array<byte>?): bool;` and 0012: `public static method TryParse(s: string, out result: DateTimeOffset?): bool;  // v0.171 nullable-out shape` | v0.183, `Bezel_Changelog.md:6319` (out-nullability postcondition `when true`, §22.4) and v0.184, `Bezel_Changelog.md:6595` (Try **shape (c)** sanctioned: `bool` + non-nullable value-typed out, scoping shape (a)'s nullability to generic/class-typed payloads). Landed stdlib §1 table row (a): `TryX(..., out item: T? when true): bool` | Two signature questions at graduation, both **signature-changing** and therefore not tier-1: (i) do these `Try*` members carry the `when true` clause (the §1 convention row says yes; the stdlib's own §4.8/§6.9 member blocks do **not** spell it — an inconsistency inside the stdlib, so the answer is a stdlib-wide call, not a per-proposal one); (ii) `DateTimeOffset.TryParse` is a **value-typed** payload, so shape (c) (`out result: DateTimeOffset`) is the landed convention for its direct member while `IParsable<T>` keeps `out value: T?` through the bridged constraint — 0010's `TimeSpan.TryParse(s, out result: TimeSpan)` already uses shape (c), so the two proposals currently disagree. *(Pre-range — v0.183/v0.184; low priority)* |

---

### 3. Pinned-spelling / pinned-claim records added to Currency notes (no body text changed)

These are recorded in the files' header notes rather than retro-edited. Item 1 is retro-editing that the changelog **explicitly forbids**.

| file | record | authority |
|---|---|---|
| 0011, 0013 | stdlib §2.4 renamed `GetEnvironmentVariable`→`Get`, `SetEnvironmentVariable`→`Set`, `GetEnvironmentVariables`→`Variables` at **v0.231**; §2.4 has since also gained `GetInt`/`GetBool`/`Exit`, so 0013 §1's "already exists" enumeration is a pinned list | `Bezel_Changelog.md:7227` and the v0.231 migration entry at `:7234` — *"proposals 0011, 0013, and 0045 … keep their pinned long-name spellings and take a currency note at their next revalidation, **not a retro-edit**"*. This sweep is that revalidation. Also verified: `CurrentDirectory`, `ProcessId`, `OperatingSystem` are still absent from the landed stdlib, so 0013's gap is open |
| 0014 §4, 0015 §4 | the note *"Capturing the bounding object inside a `scoped`-bounded `Spawn` body is admitted by the v0.180 capture train … **in flight**; this example lands with it"* — it has landed | spec §28.14: *"or is `L` itself, or is a value whose borrow provably originates from `L`** (self-capture: `scope.Spawn(lambda() => Work(scope.Token))` … both admitted)"*. Examples need no change |
| 0015 §3 | *"(Current §6.10 is `TaskScope` (v0.175). If §6.11 is taken, adjust numbering at integration time.)"* — §6.11 **is** taken, by `Locked<T>`/`RwLocked<T>` since v0.186. The text is already conditional and stays correct | stdlib §6.10/§6.11 headings |

---

### 4. Observations outside this sweep's scope (reported, not edited)

1. **0008 §5.3** — `"not borrow-bearing (§9.7), not move-only (§15.3), and sendable (§29)"`. §29 is **Iteration**; sendability is §30.14. Verified against the v0.183 spec in git (`e14441f0`), where §29 was already Iteration — so this predates the proposal's own pin and is not a range-caused move. Left as found.
2. **0010 Design Q2** — the `DateTime` serialization sketch inside a ` ```bezel ` fence uses `this.Kind == DateTimeKind.Utc` and a ternary `cond ? a : b`. Bezel has no `==` (equality is `=`, §8.4) and no conditional operator (`grep -c ternary` over the spec returns 0; §20's `(if … then … else …)` atom is the form). Pre-range and missed by both earlier sweeps; a fix would rewrite a code example, so it is reported rather than touched.
3. **0006/0007/0016 ownership and async traces re-checked** against the v0.298 source-set generalization of §28.3 (§C1/§C2): none of the three quotes the retired closed three-form list, and 0006 §6 Q2's *"treated as a borrow-carrying value whose source is the receiver"* uses **the sense §C8 deliberately keeps** (§28.3's frame-descriptor / §9.7's field-level predicate), not the renamed §35.4 *borrow-carrying census*. No edit.
4. **0002** was written against v0.295 and already incorporates v0.296 (§30.6, #4088/#4089/#4090). Its §4.1 trace of §28.14's five bounding-object forms, the `call-bounded` default, the *Call-bounded × deferred activation* rule, and its quotations of §3.6, §22.6 (defaulted-arity tie-break and lambda exact/converting ranking), §14.5 (`U?` binds `X` to `U`), §27.1 ("names are documentation only") and stdlib §2.15 were each checked against the landed text and all still match.

---

### 5. Count summary

| | count |
|---|---:|
| Files swept | 14 |
| Tier-1 edits applied | **9** (in 5 files: 0001 ×2, 0008 ×3, 0009 ×1, 0011 ×1, 0012 ×2) |
| Files with zero tier-1 edits | 9 (0002, 0004, 0006, 0007, 0010, 0013, 0014, 0015, 0016) |
| Tier-2 flags recorded | **8** (0006 ×2, 0007 ×1, 0008 ×1, 0012 ×2, 0016 ×1, cross-file ×1) |
| Files **clean** (no tier-1, no tier-2) | **6** — 0002, 0004, 0010, 0013, 0014, 0015 |
| Currency notes added or updated | 14 / 14 |
| Pinned-spelling / pinned-claim records added to notes | 5 (0011, 0013, 0014, 0015 ×2) |
| Out-of-scope observations reported | 4 |
| Written-against pins changed | 0 |

## 3. Lane B report — 0017–0031, 0047 (verbatim)

*Lane B files: 0017, 0018, 0019, 0020, 0021, 0022, 0024, 0025, 0026, 0027, 0028, 0029, 0030, 0031, 0047 (15 live proposals). Input: the retired-form inventory `currency-inventory-v0.306.md` (§A–§H). Method per the 2026-08-11 paper's two-tier discipline: §F scan, then full read; tier-1 mechanical respells applied in place, tier-2 design interactions flagged only. Every stdlib name and every section citation edited below was confirmed against the landed `docs/Bezel_Standard_Library.md` / `docs/Bezel_Language_Spec.md` at **v0.306** before the edit. `scripts/check_docs.sh` and `scripts/check_proposals.sh` both **OK** after the lane's edits.*

Line numbers are **post-edit** line numbers in the current tree.

---

### 1. Tier 1 — mechanical respells applied

#### 0017-temp-files-path.md

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 34 | `Delete(path: string)` + `Delete(path: string, recursive: bool)` (two lines, §1's transcribed `Directory` surface) | `Delete(path: string, recursive: bool := false)   // IOException if non-empty and not recursive` | v0.290 | **B34** (stdlib §1's overload-pair rule; the named pair; landed text at stdlib §7.4) |
| 8 | *(no Currency note row)* | new `**Currency note**` row | — | sweep record |

#### 0018-async-file.md — **clean**

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 8 | *(no Currency note row)* | new `**Currency note**` row (re-verified clean) | — | sweep record |

#### 0019-json-dom.md

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 28 | §19.6 quotation ending `"**Class leaves are matched by type-test patterns** (`var c: Circle => …`); positional destructuring (§19.3) applies to record leaves, not to class leaves."` | quotation re-derived from the landed §19.6: `"… Class leaves are matched by type-test patterns (`var c: Circle => ...`) and — equivalently for coverage — by **named** destructuring patterns whose head names the leaf (`Circle(Radius = var r) => ...`); it is **positional** destructuring (§19.3) that applies to record leaves and not to class leaves."` + a parenthetical dating the added half | v0.301 | §C "further class-C rulings" (v0.301: §19.3/§19.6 named destructuring over a class-typed scrutinee) |
| 84 | `positional destructuring is a *record-only* surface (§19.3: "Destructuring applies to records only"; §19.6: "positional destructuring … applies to record leaves, not to class leaves")` — the §19.3 fragment no longer exists in the spec | `*positional* destructuring gives classes no pattern surface (§19.3: "Positional destructuring applies to records and choice cases … Classes do not have a positional-destructuring surface, even when they declare a primary constructor"; §19.6: "it is **positional** destructuring … that applies to record leaves and not to class leaves")`, plus a pointer to the tier-2 flag | v0.301 | same |
| 436 | Q1 rejected-alternatives row: `positional destructuring is record-only (§19.3/§19.6)` | `*positional* destructuring gives classes no pattern surface (§19.3/§19.6) … or (v0.301, not adopted here) by a named destructure` | v0.301 | same |
| 9 | existing Currency note | extended with the 2026-09-17 clause + ⚠ tier-2 pointer | — | sweep record |

*Conclusion preserved in all three: the brief's `JsonString(var s)` positional sketch is still rejected; only the stated reason was re-derived.*

#### 0020-regex.md — **clean**

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 8 | existing Currency note | extended ("re-verified clean at v0.306") | — | sweep record |

#### 0021-structured-logging.md — **clean at tier 1** (two tier-2 flags, §2)

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 8 | *(no Currency note row)* | new `**Currency note**` row with ⚠ tier-2 pointer | — | sweep record |

#### 0022-test-framework.md

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 249 | §5.5 comparison table, Import row: `(not auto-imported — spec §2.3 auto-imports only System / System.Text)` | `(not auto-imported — spec §2.3's auto-import list is `System`, `System.Text`, `System.Collections.Generic` (v0.289) and `System.Collections.Linq` (v0.278); `System.Diagnostics` is not on it)` | v0.278, v0.289 | **C31**, **C32** |
| 9 | existing Currency note | extended with the 2026-09-17 clause | — | sweep record |

#### 0024-priority-queue.md — **clean at tier 1** (one tier-2 flag, §2)

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 11 | existing Currency note | extended + ⚠ tier-2 pointer | — | sweep record |

#### 0025-bitarray.md

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 33 | §D1: "`System.Collections.Generic` is **not** auto-imported — spec §2.3 auto-imports only `System` and `System.Text`, and stdlib §4 states \"None are auto-imported except the iteration-interface family.\" A program using `BitArray` therefore writes `import System.Collections.Generic;`" | "`System.Collections.Generic` **is** auto-imported (v0.289 …)" + the landed stdlib §4 sentence quoted in place of the retired one + "needs no import line at all", with a v0.177-pin parenthetical | v0.289 | **C32** (0122 L3; the changelog entry names 0025 by number as a restater of the retired rule) |
| 223 | §6 declined-alternative 2: "(Like the rest of §4 it requires an explicit `import System.Collections.Generic` — it is not auto-imported.)" | "(Like the rest of §4 it is auto-imported — v0.289; no `import System.Collections.Generic` line is needed.)" | v0.289 | **C32** |
| 9 | *(no Currency note row)* | new `**Currency note**` row | — | sweep record |

**Cross-lane note (raised by lane C, resolved here).** Lane C reported 0025's "not auto-imported" premise and asked whether it is tier 1 or tier 2. **Tier 1, at both sites, applied above.** Both passages are factual currency claims about §2.3/stdlib §4, not design premises: 0025's actual design point is *"`BitArray` belongs in §4 rather than a new module"*, and the correction strengthens that argument rather than moving it (there is now no import cost to weigh at all). The retired claim additionally quoted a stdlib §4 sentence — *"None are auto-imported except the iteration-interface family."* — that no longer exists in the document; the live §4 sentence is quoted in its place. **Propagation the coordinator must handle outside this lane:** **0111 §4 quotes 0025 as corroboration** of the retired rule (lane C's finding; 0111 is not in lane B's file set and was not touched). The v0.289 changelog entry (0122 **L3**, line 8807) names both proposals directly — *"**unargued** (no decline record; proposals **0025/0111** restate it without a reason)"* — so 0111's citation of 0025 is now a citation of corrected text and needs the same respell in whichever lane owns 0111.

#### 0026-linq-additions.md

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 7 | Target-module row: "requires `import System.Collections.Linq` — not auto-imported, spec §2.3 auto-imports only System / System.Text, and the spec's own LINQ examples write the import" | "**auto-imported since v0.278** — spec §2.3 carries the module row under the invariant *where the stdlib declares an extension over an always-available type, its module is auto-imported*, so no `import System.Collections.Linq` line is needed and the spec's own LINQ examples carry none" | v0.278 | **C31** (the coordinator's named known item). *Also verified: `grep -c 'import System.Collections.Linq'` = **0** in both spec and stdlib, so the "spec's own examples write the import" half was false too.* |
| 9 | existing Currency note | extended with the 2026-09-17 clause | — | sweep record |

#### 0027-splitspans.md — **clean**

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 9 | *(no Currency note row)* | new `**Currency note**` row (zero scan hits on all 28 §F patterns) | — | sweep record |

#### 0028-random-extensions.md — **clean**

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 9 | *(no Currency note row)* | new `**Currency note**` row | — | sweep record |

#### 0029-checksums.md — **clean**

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 9 | existing Currency note | extended ("re-verified clean at v0.306") | — | sweep record |

#### 0030-unicode-normalize.md — **clean**

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 9 | existing Currency note | extended ("re-verified clean at v0.306") | — | sweep record |

#### 0031-verify-debug.md

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 162 | §5.1's transcribed `Debug` class: `Assert(condition: bool);` + `Assert(condition: bool, message: string);` (two lines) | `public static method Assert(condition: bool, message: string := "");` | v0.290 | **B34** (the pair is named in the row; landed text at stdlib §8.2) |
| 9 | existing Currency note | extended + ⚠ tier-2 pointer | — | sweep record |

#### 0047-mechanized-core-semantics.md — **clean at tier 1** (two tier-2 flags, §2)

| line | old | new | revision | inventory row |
|---|---|---|---|---|
| 7 | existing bold-key Currency note line | extended + ⚠ tier-2 pointer | — | sweep record |

*Verified, no edit needed: every `K<n>.<m>` id 0047 cites is still defined in spec §28.3 (the live set is K1.5–K9.5; 0047 cites no id outside it); the `move` spellings and the declaration-position `owned` markers from the 2026-08-11 refresh are current; rationale §5.1 / §5.3 / §5.11 / §5.23 / §5.25 / §5.26 / §12.1–§12.2 and `spec-change-process.md` §9 all still resolve.*

---

### 2. Tier 2 — design-level flags (recorded, not fixed)

| # | file | passage quoted | changelog line / source | the decision the owner must make |
|---|---|---|---|---|
| B-1 | **0019** §Q1, line 84 + line 436 | *"That form is **not available** and would not compile: positional destructuring is a record-only surface … so they are classes, and classes are matched by **type-test patterns** (`var s: JsonString => s.Value`), reading the field through the bound name."* | v0.301 — spec §19.3 *Named destructuring*: **"A class-typed pattern head is admitted in the named form"**; spec §19.6 *Named-destructure arms*: a named arm over a sealed-hierarchy leaf **contributes to exhaustiveness exactly as the bare type pattern does**. (Changelog v0.301 block; the form itself landed at the named-destructuring revision, changelog line 7104.) | Whether 0019 adopts the named form for its leaves — `JsonString(Value = var s) => …`, `JsonNumber(Raw = var r) => …` — which is now writable, keeps `_`-free exhaustiveness, and gives the brief's ergonomics that Q1 declined as impossible. It is a **presentation** choice, not an ownership one (a named slot over a class head is a getter read that transfers nothing, §19.3 *Named-slot bindings and ownership*), so the type-per-kind hierarchy and the owned-tree design are untouched either way. Options: (a) keep the type-test examples and record the named form as an admitted alternative (smallest); (b) switch §Q1's exhaustive-match example and the §Q4 accessors' examples to the named form; (c) record it as a v2 nicety. The tier-1 respells above state the fact without choosing. |
| B-2 | **0021** §D2 | *"A discriminated `LogValue` union (tag + one-of `{int64, double, bool, string, DateTime}`) would be sendable and reflection-free — **but Bezel has no sum-type / closed-union construct yet**, and inventing one for logging alone is out of scope."* | **Premise dead.** `choice` — closed nominal sums — landed at **v0.200** (changelog line 6799, "The `choice` revision — proposal 0041 graduates"; spec §35). A **value-like** choice over those five payload types is a sendable value type by §35.4/§30.14. *Provenance: pre-range (v0.200), surfaced by this sweep; the 2026-08-11 pass did not reach it.* | Whether D2's "strings-after-formatting in v1" decision stands now that the construct it says does not exist has shipped. A typed `LogValue` choice would make `LogField` carry a sendable, reflection-free typed value and would change `LogEvent`'s serialization story (§D7's deferred JSON formatter). Options: (a) keep strings-after-formatting and re-ground the decline on cost/scope rather than on the missing construct; (b) promote the "recorded for a future revision" note to a v1 design item. Not a respell — §D2's whole argument turns on it. |
| B-3 | **0021** §D2, second bullet | *"A `Value: Object` (boxed) field **is sendable only if the boxed value is sendable**, and reading the value back out at the sink requires inspecting its runtime type and downcasting …"* | **v0.265**, changelog line 7940 (inventory **C21**): *a record or choice with an `Object`- or interface-typed field or payload is **borrow-bearing***, picking up §9.7, §30.1's async bars, **non-sendability** and §28.11's copy-slot obligation. | The `Value: Object` alternative is now **unconditionally** non-sendable for a record (`LogField` is a record), not conditionally so. The decline stands and is strengthened; the owner decides whether to re-state the reason (and whether the same clause bears on any other `Object`-typed component 0021's v2 path might reach for). Cosmetic-looking but it changes a stated rule, so it is flagged rather than respelled. |
| B-4 | **0021** §3 (the proposed surface) | `public extension ILogger` declaring the pairs `Trace(message)` / `Trace(message, fields)`, `Debug(…)`, `Info(…)`, `Warn(…)`, `Error(message)` / `(message, fields)` / `(message, cause)` / `(message, fields, cause)`, `Critical(…)` — **fourteen** declarations | **v0.290**, changelog line 8833 (inventory **B34**), now stated as a convention at **stdlib §1**: *"Where one operation takes an optional trailing argument with a nameable default … this document declares **one** signature carrying the default."* Three shapes keep a pair (differing by parameter *type* or by `owned`; an interface-required member beside its `ignoreCase` twin; an omitted argument with **no nameable default**). | How the ladder re-cuts under the convention. `fields` and `cause` both have the nameable default `null`, so `Trace`/`Debug`/`Info`/`Warn` collapse to one `:=`-defaulted signature each — but the `Error` family's `(message, fields)` vs `(message, cause)` pair differs by *parameter type*, which the convention explicitly keeps, so the collapse is not uniform. Owner ruling needed on the final shape; this is a signature change to a proposed surface, not a respell. |
| B-5 | **0024** §D1 | *"`SortedDictionary<K, V>` (§4.17) establishes the single-order rule … a comparer parameter would be the declined alternative … a `Comparer` constructor overload adds a second ordering contract to manage and lets two queues over the same `TPriority` have inconsistent orderings, which is the kind of hidden-semantic coupling Bezel's single-order rule exists to prevent."* | **v0.282**, changelog line 8621, *The comparer-injection ruling*: the hash containers' `constructor(comparer: IEqualityComparer<K>)` and the sorted containers' refusal are reconciled at **rationale §17.14** — *"The single-order rule's real content is **identity must not fork**"*; an `IEqualityComparer<K>` relocates identity *whole*, an ordering comparer would split it *because sorted identity **is** `CompareTo`-equality*. (Inventory **D20** renumbers this record §17.12 → §17.14; stdlib §4.17 now cites it.) | Whether D1's decline survives the reconciled ground. A `PriorityQueue`'s `TPriority` order participates in **no** identity facet — there is no `Contains`/`TryAdd`/key lookup keyed on priority, so a comparer here would not fork identity the way it would in `SortedDictionary`. The stated basis ("the same rule applies here") therefore no longer transfers automatically. Options: (a) keep the decline on the independent ground D1 already gives (two queues over one `TPriority` with inconsistent orderings is a legibility cost), citing rationale §17.14 rather than §4.17's posture; (b) re-open a `constructor(comparer: (TPriority, TPriority) -> int)` overload now that the single-order rule's content is narrower. The §4.17 quotation itself is verbatim-current, so no tier-1 edit was made. |
| B-6 | **0031** §5.1 / §5.2 | *"`Debug.Assert(false, msg)` and `Debug.Fail(msg)` are equivalent semantically: both are unconditional failure signals in debug builds."* and *"`Debug.Fail` follows the same contract exactly: **Debug build** (`--debug` flag): `Debug.Fail` **raises `VerificationException`**."* | **stdlib §8.2 at v0.306**: *"A failed assertion — `condition` false where the call is not elided — reports the condition's source location and the message … and **terminates the process**: assertion failure is a programming-error report, not a recoverable fault, so **`Assert` never throws**."* Contract recorded at changelog line 6747 (D-12). *Provenance: pre-range; surfaced by this sweep.* | Whether `Debug.Fail` raises a catchable `VerificationException` (0031's design) or reports-and-terminates like `Debug.Assert` (the stdlib's landed contract). Both claims cannot stand: §5.1's "equivalent semantically" and §5.2's "follows the same contract exactly" are false as written under either answer. Options: (a) `Fail` terminates, matching `Assert` — then §5.2's contract sentence stands and §5.1's `VerificationException` line goes, and `VerificationException` becomes `Verify.That`'s exception only (which also re-cuts §8's proposed §8.3 text and declined-alternative 5); (b) `Fail` throws and the equivalence claim is dropped, with the divergence from `Assert` stated. Not a respell: it decides what `Debug.Fail` *is*. |
| B-7 | **0031** §5.1 / §8 | `public static method Fail(message: string);` + `public static method Fail(message: string, detailMessage: string);` | **v0.290** (**B34**), stdlib §1's overload-pair rule — the same rule under which this lane collapsed §5.1's transcribed `Debug.Assert` pair one block above. `detailMessage` has the nameable default `""`. | Whether the proposed `Fail` pair becomes `Fail(message: string, detailMessage: string := "")`. Mechanical in form, but it is a **proposed** signature (unlike the transcribed `Assert` pair, which this lane fixed), so the owner rules on the shape. Note the internal asymmetry the flag creates until ruled: §5.1's quoted `Debug` block now shows one `:=`-defaulted `Assert` beside a two-member proposed `Fail`. |
| B-8 | **0047** §1 (intro), §1.3, §4.2, §4.3, §7 item 2, §11 item 5 | *"the kernel principles **K1–K9** and the axiom register **A1–A14** are adopted from 0046 verbatim"*; *"nine stated principles plus **fourteen** recorded axioms"*; §4.2's carried-assumption cells *"A12, A13"* (T1) and *"A7, A8"* (T2); §4.3's disposition rows *"A2, A3, A6, A10, A11, A12 \| EXCLUDED(L3)"*, *"A13 (inline-array carve) \| EXCLUDED"*, *"A1 (factory heuristic) … see §7 for the tracking consequence **when 0048 lands**"*; §7 item 2 *"Proposal 0048 — **the first live register change**. 0048 deletes A1."* | **rationale §5.26 at v0.306**: *"a register of **eleven** recorded axioms and **four** tombstones (A1–A15; A1 retired v0.201, **A2 retired v0.258**, A8 retired v0.213, **A13 retired v0.260**)"*; spec §28.3 heading **"The axiom register (A1–A15)"**. In range: **A2** tombstoned v0.258 (inventory **C4** — calls-mint unification, changelog line 7695), **A13** tombstoned v0.260 (**C19**, line 7769, *"Register: ten live axioms, four tombstones"*), **A15** added v0.266 (**C25**, line 7986 — the self-returning enumerator). Out of range but unrecorded here: A1 retired **v0.201** (line 6815), A8 retired v0.213. | The §4.3 disposition ledger is the artifact 0046 §11's coverage oracle is operationalized as — *"the model's assumption count and the register must agree"*, enforced by `Bz/Register.lean` — and it currently disagrees with the register on **four** rows. The owner must rule the re-cut: (i) A2 and A13 become **tombstone** rows, not EXCLUDED live axioms — and A13's retirement in particular removes the ground of §4.2's alignment note (*"v1's T1 carries neither A12 … nor A13 … so v1's T1 is exact on its fragment"*), since owning-inline-array reassignment is now a **derivation** (per-slot drop-on-overwrite) rather than an axiom; (ii) **A15** needs a disposition row (its content — a class returning `this` from its own `GetEnumerator()` — is iterator-frame machinery, so EXCLUDED(L3) is the likely answer, but it is a ledger decision); (iii) §4.3's A1 row and §7 item 2's *"the first live register change … when 0048 lands"* are now history, and three real register changes have landed **unledgered** since — which is exactly the drift §7's discipline exists to prevent, and worth recording as the worked example in place of the 0048 one; (iv) §4.2's T2 cell cites **A8**, a tombstone since v0.213. Also minor, same family: §4.3's A7 note says the row was *"re-grounded semantically at v0.246"* — it moved again at **v0.286** (**C35**, line 8684: operator operand positions bind **borrows** of move-only values), and A7's live row now reads *"the operator half is settled (#3560)"* while the `??` half stays gated. A7's two **positions** are unchanged, so MODELED-BY-OMISSION still holds. |
| B-9 | **0047** §3.4, §4.3, §5.2 | §3.4's rule displays are tagged against K3's position list, K4's kill table and K5's placement list — e.g. *"The announced kill, deliberately without a freeze premise (K3.10, K4.2 — unfrozen ⟺ announced ∧ backstopped; the call is the announcement, the flag is the backstop)"*, *"Drop-on-overwrite (K3.11, K5.2 — disposing but not keyword-visible, hence frozen)"*; §5.2 plans *"one Lean file per kernel principle, `Bz/Kernel/K1.lean`…`K9.lean`"* mirroring 0046 §4's 48-row mapping table | **v0.257** (inventory **C9**, changelog line 7678, proposal 0093): `defaultable` becomes a named §3.6 derived property; the three **announcement senses** (*target-announced*, *site-announced*, *type-announced*) and the kill table's **three bits** (*obligation-moves*, *flag-set*, *keyword-visible*) are named. **v0.281** (proposal 0118, changelog entry *"the kernel absorption is complete — one definition site per ownership rule"*): *"**The consume table** (§28.3, new anchor `sec-consume-table`): K3's twelve-position list, K4's kill table, and K5's placement list were three projections of one relation; they are now **one table** — position × marker spelling × subject × post-state × frozen? × disposal moment × kernel tag … The freeze criterion, stated at five sites inside §28.3, is stated once."* The K4.2 two-bit reading note moved to **rationale §5.26** (*"recorded here since v0.281; formerly a reading note beside the spec's kill table"*). | Whether the model's source-of-truth mapping re-bases on the **consume table** (72 cells, one anchor) instead of the three merged projections. This is not a wording question: §5.2's coverage oracle is *"each DERIVED row of 0046 §4 … becomes a named lemma"* and §4.3's ledger is checked against the register — both were drawn against a spec whose ownership rules had 56 DERIVED prose blocks and three overlapping kill/position/placement lists. Since v0.281 there is **one definition site per rule**, which makes the mapping cheaper and the coverage oracle sharper, but it re-numbers what the model must enumerate. Owner ruling needed on: (a) whether §3.4's per-rule K-tags re-cite the consume table's cells (the tags themselves all still resolve — no tier-1 edit was made); (b) whether §5.2's per-principle file split survives the absorption or becomes one consume-table file plus the posture clauses K4 and K5 kept; (c) whether the newly named vocabulary (`defaultable`, the three announcement senses, the three kill bits) is adopted verbatim in the Lean definitions, since §3.4 currently paraphrases two of the three bits informally (*"disposing but not keyword-visible"*). |

---

### 3. Clean files

Zero tier-1 hits **and** zero tier-2 flags — currency note reads "re-verified clean at v0.306":

**0018**, **0020**, **0027**, **0028**, **0029**, **0030** — 6 files.

(0018's §7.3 `File` transcription, 0028's §2.7 `Random` transcription and 0029's §8.6 quotation were each re-checked member-for-member / word-for-word against the landed text; 0027 returned zero hits on all 28 §F patterns.)

---

### 4. Count summary

| | count |
|---|---|
| Files swept | **15** |
| Files with tier-1 edits (excluding the currency note itself) | **6** — 0017, 0019, 0022, 0025, 0026, 0031 |
| Tier-1 respells applied (content edits, currency notes excluded) | **9** |
| — of which: retired stdlib overload pairs in transcribed surfaces (B34, v0.290) | 2 (0017, 0031) |
| — of which: auto-import claims (C31/C32, v0.278/v0.289) | 4 (0022 ×1, 0025 ×2, 0026 ×1) |
| — of which: spec quotations re-derived from landed text (v0.301) | 3 (0019) |
| Currency notes added (file had none) | **6** — 0017, 0018, 0021, 0025, 0027, 0028 |
| Currency notes extended (file already had one) | **9** — 0019, 0020, 0022, 0024, 0026, 0029, 0030, 0031, 0047 |
| Tier-2 flags recorded | **9** — B-1 (0019), B-2/B-3/B-4 (0021), B-5 (0024), B-6/B-7 (0031), B-8/B-9 (0047) |
| Files carrying a tier-2 flag | **5** — 0019, 0021, 0024, 0031, 0047 |
| Files clean on both tiers | **6** — 0018, 0020, 0027, 0028, 0029, 0030 |
| Files touched outside the lane's 15 | **0** (`git status` confirms; `README.md` and every normative `docs/` document untouched) |
| Doc gates after the lane's edits | `check_docs.sh` **OK**, `check_proposals.sh` **OK** |

**Written-against pins:** unchanged in all 15 files, per convention.

**One item for the coordinator to route to another lane:** **0111 §4** quotes 0025 as corroboration of the now-retired *"`System.Collections.Generic` is not auto-imported"* rule (raised by lane C). 0025's two sites are fixed here as tier 1; 0111 is outside lane B's file set and was not touched. The v0.289 changelog entry (0122 L3, line 8807) names **both** proposals as restaters of the rule, so the same respell is owed in 0111.

**Provenance of the two pre-range flags.** B-2 (0021 × `choice`, v0.200) and B-6 (0031 × `Debug.Assert` never throws, ~v0.19x) fall *before* the v0.247–v0.306 inventory window. They are recorded rather than dropped on the 2026-08-11 paper's own precedent (its flags 2 and 4 were pre-program expiries surfaced by a sweep and carried a provenance note). Both are marked as such in the table so the coordinator can scope them.

**Two findings deliberately not recorded as defects** (pre-range, out of the sweep's charter, listed here only so a later lane does not re-derive them): 0018 §6's comment `// stdlib §4.14 span access` on `data.AsReadOnlySpan()` cites the `OwningBuffer`/`Span`/`ReadOnlySpan` section for an `Array<T>` call whose overload is declared at §4.4 and described at §4.15; and 0028 §D2's `ArgumentException` for `NextInt64`'s bad bounds does not mirror live §2.7's `ArgumentOutOfRangeException`. Neither is a §A–§E row and neither moved in range.

## 4. Lane C report — 0053–0111 (verbatim)

Population: the 15 files assigned to lane C. Method per the charter: §F scan, then full read;
tier-1 respells applied in place; tier-2 design interactions flagged, not fixed; a
`**Currency note:**` added or appended in every file's header block in that file's own style.
Written-against pins untouched throughout. No `README.md`, no other proposal, no normative
document was edited (`git diff --stat` confirms the 15 files; the other modified proposals in the
tree are lanes A/B).

Inventory rows cited as `A4`, `B17`, `C29`, `D15`, `E12`, … are rows of
`currency-inventory-v0.306.md`; changelog line numbers are `docs/Bezel_Changelog.md` lines at the
tree this was run against.

---

### 1. Tier-1 edits (applied)

| File | line | old | new | revision | inventory row / source |
|---|---:|---|---|---|---|
| 0054-traceability.md | 54 | §2.2 id-scheme row, scheme cell `` `guide1` / `guide2` `` and "Resolves against" cell `the two guides` | scheme cell `` `guide1` … `guide7` `` and "Resolves against" cell `the seven guides (Bezel_Guide_01_*–Bezel_Guide_07_*)` | pre-range (guide set grew to seven before v0.247) | **not an inventory row** — sourced to the twelve documents of `docs/` (CLAUDE.md's document list; `Bezel_Guide_01`–`Bezel_Guide_07`) and to this file's own Status line, which records a `guide3` id-scheme currency fix in #2041's ruling that was never applied to the table |
| 0068-crypto-primitives.md | 299 (§9 *Usage examples*) | `if not Convert.TryFromHexString(signatureHex, out owned var presented) then` | `if not Convert.TryFromHexString(signatureHex, out var presented) then` | v0.289 | §A row **A4** (changelog 8815, 8816); §D rows **D3** (Appendix A.4 alternative `"out" "owned" "var"` deleted). Declaration-position `out owned` slots untouched (§G.8) |
| 0069-compression.md | 307 (§9 *Usage examples*) | `if Gzip.TryDecompress(upload.AsReadOnlySpan(), 8 * 1024 * 1024, out owned var data) then` | `if Gzip.TryDecompress(upload.AsReadOnlySpan(), 8 * 1024 * 1024, out var data) then` | v0.289 | §A row **A4** (changelog 8815, 8816) / **D3**. The three `out owned result: Array<byte>?` **declarations** in §8's signature block are unchanged |
| 0071-file-metadata.md | 74 (D3) | ``Comparison (`<`, `>`) is tick order — correct for two `Utc` values, which is all this API emits.`` | ``Comparison is `CompareTo` (tick order) — `DateTime` declares no `<`/`>`/`<=`/`>=` overloads (§2.5, v0.274) — and is correct for two `Utc` values, which is all this API emits.`` | v0.274 | §B row **B17** (changelog 8300). Confirmed against stdlib §2.5 at v0.306: *"Ordering is CompareTo only: DateTime declares no `<`,`>`,`<=`,`>=` overloads"* |
| 0071-file-metadata.md | 202 (§9 *Tour by example*) | `  return src.ModifiedAt > built.ModifiedAt;       // DateTime tick-order comparison (§2.5)` | `  return src.ModifiedAt.CompareTo(built.ModifiedAt) > 0;   // DateTime tick-order comparison (§2.5)` | v0.274 | §B row **B17**. Meaning preserved exactly (tick order, strict greater-than) |
| 0106-language-lessons-survey.md | 247 (§7.1) | ``returns **1** — stdlib §8.1's `Stopwatch.Restart()`.`` | ``returned **1** at this study's v0.262 pin — stdlib §8.1's `Stopwatch.Restart()`, a member retired at v0.290 (`Reset(); Start();`), so no `Restart` member remains.`` | v0.290 | §B row **B39** (changelog 8837). The hit sits inside a **quoted grep result**, so it was *not* mechanically respelled — respelling `Restart()` would destroy the measurement. Verified: stdlib §8.1 at v0.306 declares `Start`/`Stop`/`Reset`/`StartNew` and no `Restart`; the measurement itself is also stale (the same grep now returns 11 over the ten documents, mostly rationale's v0.263 recovery-unit record), which is why the count is pinned to v0.262 rather than restated |
| 0106-language-lessons-survey.md | 393 (§10 *Interactions*) | ``§14.6's *"not yet enforced — tracked #3271"* status is this survey's own example`` | ``§14.6's *"not yet enforced — tracked #3271"* status — the note the spec carried at this study's v0.262 pin, deleted at v0.287 (editorial, [#3408](../…), once both halves of the §14.6 check had landed) — is this survey's own example`` | v0.287 | §E row **E1**-class (verbatim spec quotation no longer in the live text); changelog **8713**: *"§14.6's stale compiler-status note is gone (editorial; #3408) … was false at v0.286: both halves of the §14.6 check run"*. Confirmed: `grep -n "not yet enforced" Bezel_Language_Spec.md` returns nothing at v0.306 |
| 0108-editions.md | 28 (§1 *Why now*) | ``§14.6's bounded-instantiation-closure rule carries *"(Compiler status: not yet enforced — tracked #3271.)"*: …`` | ``§14.6's bounded-instantiation-closure rule carried, at this proposal's v0.262 pin, *"(Compiler status…)"*: … (That note was deleted at v0.287 as editorial — [#3408](../…) — once both halves of the §14.6 check had landed; the same changelog entry records that §22.12's sibling note, [#2331](../…), *"is untouched — its arm is still unenforced"*, so the shape the argument needs still has a live instance.)`` | v0.287 | §E row **E1**-class; changelog **8713** (both the deletion and the quoted #2331 sentence come from that one line) |

**8 tier-1 edits across 6 files.**

---

### 2. Tier-2 flags (recorded, not fixed)

| # | File | passage (quoted) | changelog line / source | the decision the owner must make |
|---:|---|---|---|---|
| 1 | 0053-property-testing.md §0, §5, §9 | §5: *"One mechanism, on the model of [§15.4.1]'s `FromCollectionLiteral` (**name-based recognition** of a specifically-named public static with a recognized signature; rationale §5.16 records why that mechanism over attributes)"*; §5 again: *"Like `FromCollectionLiteral`, the convention is **forward-compatible with promotion to a static interface member** ([§11.8]) if the recognition set ever grows — the identical zero-source-change promotion path the stdlib already records for `FromCollectionLiteral` and `Deserialize`"*; §0: *"a type opts into custom generation by declaring one recognized static factory, on the §15.4.1 `FromCollectionLiteral` model"*; §9 ledger row: *"The `FromGenerator` recognition mechanism and its promotion path are `FromCollectionLiteral`'s, transplanted (§5)"* | §B row **B44** / §C row **C29** (changelog **8924**, **8936**, **8945**), v0.294. Verified at v0.306: spec §15.4.1 now reads *"A **class** opts into sequence-literal construction by conforming (§12.7) to one of two static interfaces"* and *"**Recognition category.** This is interface-based recognition"*; stdlib §2.19 declares `ICollectionLiteralTarget<TSelf, T>` / `IOwningCollectionLiteralTarget<TSelf, T>` (§D row **D15**) | The cited precedent is no longer a by-name mechanism, and the promotion 0053 calls "forward-compatible" **has already happened**. Decide whether `FromGenerator` (a) follows the precedent through and is specified as a static-member interface (spec §11.8) from the start, (b) stays by-name and is re-grounded on `IDeserializable`, which stdlib §2.19's own note still records as the surviving by-name protocol, or (c) keeps by-name recognition on new reasons. Not a respell: §5's whole mechanism section rests on the retired premise |
| 2 | 0103-shared-value-generics-study.md — Currency row | *"v0.261 (HEAD `4d783b6`) — written and verified at this revision; **the delta note lands at the first re-verification against a later docs HEAD**. Re-verification before any ruling must cover §2.8.2–§2.8.6, §3.6, §7.10, §11.8, §14.2, §14.5, §15.1–§15.3, §17.10–§17.11, §22.12–§22.13, §22.15–§22.16, §27.1, §28.1, §28.3, …"* | In-range moves inside that set: **C31/C32** (§2.3 auto-imports, v0.278/v0.289); **B44/C29/C30/D15** (§11.8 gains the collection-literal target pair, v0.294); **C43/A25** (§14.2's `new()` row gains the abstract/`closed` exclusion, v0.302, changelog 9155); **C3** (§27.1 admits no named argument through a function-typed value, v0.295, changelog 8953/8959); **C1/C2** (§28.3's closed three-form closure-capture list replaced by the source-set rule, v0.298, changelog 9020/9027); **C38** (§22.12's statics rules 2–5 become verdict tables, v0.291, changelog 8872) | This sweep spot-checked six of the study's verbatim quotations and all six still match the live text, but it did **not** run the delta audit the Currency row promises. Decide whether the 0103 delta note is owed now (the study is `accepted`, so its §7 disposition is already banked) or only when one of its §7 item 4 reopening triggers fires |
| 3 | 0104-witness-abi-open-questions.md — Currency row + §5 | *"v0.261 (HEAD `39250d8`) … the delta note lands at the first re-verification against a later docs HEAD"*; §5: *"**§27 / §28 / §32 seams** — this study's §1 rests on §27.1/§27.1.1/§27.6/§27.7.1, §28.1's erasure arm, and §32.5/§32.8; **a revision touching any of these … "*" | §C row **C3**, v0.295 (changelog 8953, 8959): *"A call through a function-typed value admits no named argument (§27.1)"* — an in-range change to §27.1, one of the seams the study names as its own tripwire. Also **C40** (v0.296, changelog 8981): method-reference ranking, parameter-count mismatch making a candidate inapplicable, the `async` lambda's three-source rule (§30.6) | The study's own seam-watch condition has fired at §27.1. Decide whether §1's function-type-identity resolution needs re-checking against the v0.295 named-argument rule and the v0.296 applicability rules, or whether both are orthogonal to identity (this lane's reading is that they are call-site rules and do not touch identity, but the study reserved that call) |
| 4 | 0105-body-locality-invariant.md §2 | *"**The absence claim — audited, not proven** (0103 §2's own scoping) — with near-misses named: **no rule of v0.261** requires retaining or re-elaborating any body to check another declaration."* — with the Currency row's *"delta note at first re-verification against a later docs HEAD"* | §C row **C20**, v0.262 (changelog 7839, 7840; §D row **D11**): spec §14.6 lands **one revision after this pin** and reads *"Generic declarations request instantiations of one another — through generic calls, generic-type instantiations, base-clause references, and field, choice-payload, and signature types … The **instantiation-request graph** makes the shape checkable at declaration"* — a graph whose edges are read out of member bodies. 0103 §3 already names the discharging fact (*"the body's T-parameterized instantiation-request set"*) | The audit is pinned at v0.261 while the documents are at v0.306 (45 revisions). Decide (a) whether §14.6 is *fact-discharged* (per-body instantiation-request set) or needs a second carve-out beside generic instantiation, and (b) whether the invariant's absence claim is re-audited over the whole v0.262–v0.306 range now or at the streaming tier's opening. This lane did **not** re-run the audit |
| 5 | 0106-language-lessons-survey.md §3.5 and §9 slate row **v** | §3.5: *"**Bit syntax — genuinely-open (reader half)** … The nearest feature is byte-granular only (stdlib §4.14 reads nothing narrower than 16 bits) … *Cost:* small and stdlib-only — ▲`BitReader` … plus a ▲`BitWriter` mirror"*; slate row v: *"**`BitReader`/`BitWriter` stdlib proposal** (§3.5) … **High** on scope; medium on priority"* | §B row **B10**, v0.264 (changelog 7903): the bit tier — `BitReader`, `BitWriter` (stdlib §4.14), `ArrayBitReader`, `ArrayBitWriter` (§4.15), 68 members — plus §D row **D14** (the two new anchors) and §E row **E12** (proposal 0109 graduated v0.264; `0109-bit-reader-writer.md`'s Status reads `graduated-v0.264`). The pattern half was recorded as declined at v0.263 (§D row **D21**, `sec-no-bit-syntax-patterns`) | Both halves of §3.5's recommendation have landed. Decide whether §3.5's verdict and slate row v are marked **discharged** in place (with the landing revisions) or left as the historical record the study's pin implies. Not respelled here: the verdict word *genuinely-open* is the study's own classification vocabulary, not a spelling |
| 6 | 0106-language-lessons-survey.md §9 slate row **i** | slate row i: *"**Record invariants + non-defaultability** (this study's §6.2 (d′)) | Lift §22.16's recorded record-invariant deferral, instrument the three-or-four producer points, and derive non-defaultability as a §3.6 consequence …"* | §E row **E12**: proposal **0107** graduated at **v0.268**; `0107-record-invariants.md`'s Status reads `graduated-v0.268 — superseded` | Same decision as flag 5, for slate row i. The Status row of 0106 records that 0107 was *filed*, not that it *graduated* |
| 7 | 0106-language-lessons-survey.md §2.1 (the auto-import premise) | 0111 §4 quotes it: 0106 §2.1 *"calls the producer 'declared in `System.Collections.Generic`, which **is** auto-imported'"* | §C row **C32**, v0.289 / 0122 L3 (changelog 8816): *"**`System.Collections.Generic` is auto-imported**; every stdlib §4 name is bare-visible; §2.3 states its cost at twenty-nine names."* Confirmed at v0.306: §2.3's table carries an *Auto-imported module: `System.Collections.Generic`* row listing twenty-nine names, and stdlib §4's heading is *"`System.Collections.Generic` (auto-imported)"* | 0106 §2.1's premise, refuted by 0111 (and by 0025) at their v0.262 pins, is **true again** at v0.306. Decide whether the §2.8.3-rule-2 defect candidate is restated on the premise that now holds — see flag 10, which is the same fact from the other side |
| 8 | 0108-editions.md §1 | *"The kernel is still gaining rules … §14.6's bounded-instantiation-closure rule carries '(Compiler status: not yet enforced — tracked #3271.)': the spec already states a rejection and the compiler has not caught up. … So §14.6 is **evidence that the kernel is still gaining rules**"* | changelog **8713** (v0.287): the note was deleted as editorial (#3408) because it was already false at v0.286 — *"both halves of the §14.6 check run (`checkExpandingInstantiationEdges` and `checkExpandingGenericMemberEdges`)"*; the same line records §22.12's sibling note **#2331** as *"untouched — its arm is still unenforced"* | The tier-1 edit pinned the quotation and named the live sibling, but did **not** re-base the argument. Decide whether §1's spec-ahead-of-impl exemplar becomes §22.12/#2331 (which is still in that posture) or whether the paragraph keeps §14.6 as a *closed* instance and argues from the closure instead. Substituting the exemplar is design work, not a respell |
| 9 | 0110-partial-failure-toolkit.md §6 (stage 3) | *"This is the one thing stage 3 needs that a plain library cannot give: **compiler-known status**. … stdlib §6.12 states the precedent verbatim for `Parallel`: 'This surface is not writable in user Bezel …'"*; §7's Currency row lists stdlib §6.12 among the **Quoted** sections | §B row **B48**, v0.284 (changelog 8658). Confirmed at v0.306, stdlib §6.12's opening block: *"⚠ **Spec-ahead-of-implementation** ([#3557](../…)): this entire class is unimplemented — no sema arm, no codegen arm, no runtime team."* The inventory's own note on the row: *"a proposal that builds on `Parallel` should say so"* | Stage 3's compiler-known precedent is now marked unimplemented and kept on recorded design weight alone. Decide whether the precedent still carries stage 3's argument (the quoted sentence is a *normative contract* statement and is unchanged), whether the §6 argument should name the marker, and whether stage 3's gating should acquire a dependency on #3557. The quotation itself is still verbatim-accurate — nothing to respell |
| 10 | 0111-unsafe-partition.md §4, verification delta (i) | *"**(i) 0106 §2.1's auto-import premise does not hold** — it calls the producer 'declared in `System.Collections.Generic`, which *is* auto-imported', but **§2.3 auto-imports only `System` and `System.Text`** ('All other library modules require explicit `import`') and stdlib §4's intro says '**None are auto-imported** except the iteration-interface family'. This is **not a lane reading**: committed proposal **0025** states the same correction in its own words"* | §C rows **C31** (v0.278, changelog 8496/8526) and **C32** (v0.289, changelog 8816). At v0.306 §2.3 carries four auto-imported-module rows — `System`, `System.Text`, `System.Collections.Generic`, `System.Collections.Linq` — and stdlib §4's intro reads *"**Auto-imported:** this module is on language spec §2.3's auto-import list … the same standing as `System`, `System.Text`, and §5"*. Both quoted sentences are dangling | The refutation is now itself refuted: 0106 §2.1's premise holds at v0.306, and both of the documents 0111 quotes against it say the opposite. Respelling the quotations would **invert** the paragraph's claim, so it is tier 2 by the charter's own rule. Decide whether delta (i) is struck, or restated as "the premise was false at v0.262 and became true at v0.289". **Coordinator note:** the same expired premise is quoted from **proposal 0025** ("`System.Collections.Generic` is **not** auto-imported"), which is not in lane C's set |
| 11 | 0111-unsafe-partition.md §4, verification delta (ii) | *"**(ii) It is a document *conflict*, not a silence** — stdlib §4.14's `DangerousPointer()` declaration carries the comment *'The result type lives in System.Interop (import required at the use site)'* …, an assertion rule 2 appears to override"* | At v0.306 stdlib §4.14's comment reads *"The result type lives in System.Interop: **naming** `Ptr<T>` requires that import, **using** the returned value does not (language spec §2.8.3 rule 2)"* — the naming/use distinction was added at **v0.265** (git: the line carried the marker `(language spec §2.8.3 rule 2; v0.265)` until v0.305's marker sweep, §E row **E3**) and is the same correction class as v0.279's member-access import-claim fix (changelog **8545**, #3367) | The document conflict delta (ii) reports was closed three revisions after this proposal's pin. Decide whether delta (ii) is struck or restated as historical. As with flag 10, respelling the quotation inverts the claim, so no edit was applied |

**11 tier-2 flags across 6 files** (0053, 0103, 0104, 0105, 0106 ×3, 0108, 0110, 0111 ×2).

---

### 3. Clean

- **0060-channel-give-back-protocol.md** — clean. Re-verified at v0.306: §30.15's give-back sentence quoted in §2 still reads verbatim (*"An item being handed to the waiter at the instant of disposal is returned to the channel's head (an internal give-back; items remain exactly-once)"*), stdlib §6.9 still records the `TrySend` give-back deferral, §22.3's `owned` consume is still unconditional, and §30.15 still names proposal 0060 as the deferral's home.
- **0070-uri.md** — clean. The one range-sensitive claim, §3's *"`default(Uri)` is a compile error … so `Uri` cannot satisfy `where T: new()`"*, was re-checked against §C row **C43** (v0.300/v0.302): §14.2's `new()` row keys the value-type arm on **defaultable** (§17.11) and the in-range rulings (implicit parameterless constructor; abstract/`closed` excluded) touch the class arm only.
- **0088-concrete-static-selectors.md** — clean. §28.3's *Factory methods and sourceless returns* heading and producers-table rows 4 and 6 are intact; rationale §5.7 is still *Factory method ownership*; all six `(":" "owned"? Type)` return-slot productions stage 2 amends are present in Appendix A.4/A.5 (§D row **D6**'s shared-`Member` merge at v0.287–v0.288 did not touch the return-slot family). The v0.241 line numbers §2.1 cites have moved (they are now ≈10953–11159) but are pinned as a reading aid, per §G.5.

**3 files clean.**

---

### 4. Count summary

| | count |
|---|---:|
| Files swept | **15** |
| Tier-1 edits applied | **8**, across **6** files (0054 ×1, 0068 ×1, 0069 ×1, 0071 ×2, 0106 ×2, 0108 ×1) |
| Tier-2 flags recorded | **11**, across **6** files (0053 ×1, 0103 ×1, 0104 ×1, 0105 ×1, 0106 ×3, 0108 ×1, 0110 ×1, 0111 ×2) |
| Files clean (neither) | **3** (0060, 0070, 0088) |
| Currency notes written | **15** — 5 newly created (0054 bold-key line; 0060, 0068, 0070, 0088 table rows) and 10 appended to an existing note (0053, 0069, 0071, 0103, 0104, 0105, 0106, 0108, 0110, 0111) |
| Written-against pins changed | **0** |
| Files outside the assignment touched | **0** |

#### Scan residue deliberately left (false positives, per §G)

- **A1 `when`** — zero live-construct hits in all 15 files (matches the inventory's live-set count of 0).
- **A4 reserved identifiers** — every hit (0053 §4's `// invariant:` comment, 0054 §7's `<satisfies>` prose, 0105 §1's invariant text, 0106 §5.3/§7.2, 0108 §5, 0110 §3.2) is a contract keyword or ordinary English in a keyword position, never an identifier position (§G.9).
- **B6 window parameters** — 0068's `GetBytes(count: int)` is a byte *quantity*, not a window; 0068 §9's `stream.Read(buffer, 0, buffer.Count)` is stdlib §7.1's recorded `(offset, count)` exception to the v0.282 convention (**B30**).
- **B7 `.ToArray(` / `ReadOnlySpan<`** — 0069 §9's `ReadOnlySpan<byte>` comment and 0068 §2's `IReadOnlySpan<byte>` family list are live types; §15.4.1's carrier move (**B13**) is not cited anywhere in this set. No `.ToArray(` hit in the set needs the §H.2 element-type judgment.
- **B9 `\bSingle\b` / `SortedDictionary`** — 0106 §6's *"Single-field records"* and *"Single-case DU"* are English/foreign-language terms; 0111 §2's `DangerousPointer` hits are the row-D1 member, which exists.
- **B2 `.Take(`** — 0069's `TakeStream()` is a proposal-declared member, not `OwningSet<T>.Take` (**B3**).
- **A3 `++`** — the one hit in the set is `C++`-adjacent prose (§G.7).

#### Provenance

One pass per file: §F scan, full read of every §-citation and fenced example, and — for every claim
this report calls stale or current — a direct check against `docs/Bezel_Language_Spec.md` /
`docs/Bezel_Standard_Library.md` at v0.306 or against the named `docs/Bezel_Changelog.md` line.
Verbatim quotations were re-derived from the live documents, not respelled blind (the §29.11,
§30.15, §29.12, §22.12, §11.8, §14.2, §28.1, §8.3, §12.1, §2.8.2–§2.8.6 and stdlib §2.5 / §4.14 /
§6.12 / §7.1 / §8.1 quotations in 0103, 0104, 0108, 0110 and 0111 were each grepped against the
current text). No build was run (none exists here, #4451); no `git add`/`commit`/`push`.

## 5. Owner-facing digest of the tier-2 flags

The 28 flags are recorded in full in the lane reports above. Grouped by what the owner has to
decide, the ones that change a proposal's *next action*:

| Group | Proposals | The decision |
|---|---|---|
| **A premise the proposal deferred to has landed** | 0006 (§6 Q1's scoped-construction amendment landed at v0.180, spec §28.14; the changelog says the `StreamReader`/`StreamWriter` second signature "rides 0006's stdlib landing"); 0053 (the name-based `FromCollectionLiteral` recognition it calls "forward-compatible" was replaced by the interface-based protocol at v0.294); 0111 (both §4 verification deltas overtaken — auto-import at v0.289, the `DangerousPointer()` comment at v0.265); 0106 (slate rows i and v discharged by 0107's and 0109's graduations) | Re-ledger: the deferred question is answered; the proposal's text should say so at its next revision, and 0006's acceptance note can drop the deferral |
| **An ordering-operator precedent the design rests on was deleted** | 0012, 0016 (both rest on "`<`/`>`/`<=`/`>=` agree with `CompareTo`", deleted at v0.274 — stdlib §2.5 is now "ordering is `CompareTo` only"); 0071 was the transcribed-surface case and was fixed at tier 1 | Rule whether `DateTimeOffset` and `Instant` expose relational operators at all; either answer changes the proposed surface, so this is not a respell |
| **The kernel moved under a mechanization** | 0047 (axiom register now A1–A15, eleven live + four tombstones per rationale §5.26, vs the proposal's A1–A14; A2 tombstoned v0.258, A13 v0.260, A15 added v0.266 — none ledgered in §4.3's disposition table; 0118's v0.281 absorption merged K3/K4/K5 into one consume table, the surface §3.4/§5.2 mirror) | Refresh 0047's §3–§5 against v0.306's kernel before any Lean work starts; the disposition table is the artifact 0046 §11's coverage oracle is operationalized as |
| **A cited precedent is now spec-ahead-of-implementation** | 0110 stage 3 (stdlib §6.12 `Parallel`, ⚠ #3557 at v0.284) | Sequence 0110 stage 3 behind #3557 |
| **An absence audit went stale** | 0105 (v0.261 audit; §14.6's body-read instantiation-request graph, v0.262, is the named candidate) | Re-run the audit at v0.306 before relying on the invariant |
| **Pre-range expiries surfaced incidentally** | 0021 ("Bezel has no sum-type construct" — `choice` shipped at v0.200); 0031 (`Debug.Fail` raising a catchable exception vs stdlib §8.2's "never throws / terminates the process"); 0008 (§9.4's "no value-receiver mutation" premise expired at v0.255, conclusion strengthened); 0012 D3 (homogeneous `=`/`<>` now strips auto-hashable and triggers §25.2's co-declaration rule, v0.276) | Fold into each proposal's next revision; none blocks acceptance |

**Sweep lessons for the next one** (the 2026-08-11 record's §5 precedent): (1) the inventory-then-lanes
split worked — one reader of the changelog, three appliers — and the inventory's tested scan gave each
lane an expected hit count so silence was informative; (2) the mechanical residue after sixty revisions
was 26 edits, but the design residue was 28 flags, which argues for a currency sweep **per ten
revisions** rather than per program, since design flags compound; (3) the transcribed-surface vs
proposed-surface line (fix the former at tier 1, flag the latter at tier 2) should be stated in the
lane brief, as lane B did in its report — it is the one judgement the lanes made differently before
it was stated.
