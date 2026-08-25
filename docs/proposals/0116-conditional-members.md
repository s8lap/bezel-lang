# Proposal 0116 — Conditional members: the standard library has five, the language defines one, and it defines it in the Choices chapter

| | |
|---|---|
| **Status** | graduated-v0.277 — superseded (owner go, 2026-08-21). Landed as spec §14.2 *Conditionally-admitted members* with its three-row register, §35.2's narrowing, §3.6's pointer sentence, Appendix C.1's row, the `ReadOnlySpan<T>.ToArray()` deletion, D9's respell, and rationale §5.15; five `DECLINED.md` rows landed with it. ⚠ **The register's keystone is not landed and was not this revision's to land:** [#3336](https://github.com/s8lap/bezel-lang/issues/3336) — §26.4's block `where` clause does not filter extension applicability, so D9's respell is correct and **inert** until it does. *(prior: `draft`, **Draft 2** (2026-08-21), re-argued against v0.276 after a three-lane costing of option (c) — **Draft 2 reversed Draft 1's recommendation**, see this proposal's §11; Draft 1 was authored against v0.275.)* Origin is the closing comment on [#3324](https://github.com/s8lap/bezel-lang/issues/3324), which carried the question forward explicitly: *"The `OwningArray<T>` half and the conditional-member question are being carried forward as their own work rather than left implicit here."* |
| **Tracking issue** | [**#3328**](https://github.com/s8lap/bezel-lang/issues/3328) (opened at the v0.276 landing). The issues this proposal's §10 asks for are filed: [#3329](https://github.com/s8lap/bezel-lang/issues/3329) (the C1 diagnostic), [#3330](https://github.com/s8lap/bezel-lang/issues/3330) (is a member-level `where` inert?), [#3331](https://github.com/s8lap/bezel-lang/issues/3331) (the unenforced conditions), [#3332](https://github.com/s8lap/bezel-lang/issues/3332) (can `check_docs.sh` police the register?). |
| **Target module(s)** | **spec** — [§14.2](../Bezel_Language_Spec.md#sec-constraints) gains one subsection (*Conditionally-admitted members*, with a register table: the whole normative payload); [§35.2](../Bezel_Language_Spec.md#sec-choice-members)'s v0.233 paragraph is **narrowed to its choice-specific half** and cites the general rule for the rest; [§3.6](../Bezel_Language_Spec.md#sec-derived-type-properties)'s reservation paragraph gains one sentence (D5). **stdlib** — four prose sites gain a citation apiece; §5's six hashable-requiring operators move to a constrained extension block (D9); [§4.5](../Bezel_Standard_Library.md#sec-ownedarrayt) gains two declaration lines under D3's branch A. **rationale** — [§5.15](../Bezel_Design_Rationale.md#sec-owned-buffers-and-spans) one decision record. **`Bezel_For_Code_Generation.md`** — one line, D3-dependent (§9 obligation). **guides** — Guide 02 teaches C3's combinators but states no rule this proposal moves, so no pin moves (verified, this proposal's §6, *Not edited*). **Appendix A** — untouched: **no grammar changes, no new tokens** (D7 is a semantic rejection of an already-grammatical form). **Appendix C** — one row (D7's rejection). **`DECLINED.md`** — four decline-index rows (this proposal's §9). |
| **Written against** | Draft 2: spec/stdlib/rationale/changelog **v0.276**, HEAD `b26ad189`. ⚠ **Every `file:line` written in Draft 1 has drifted 1–6 lines** under v0.276's edits to §9.2, §25.2 and §35.7; citations in sections Draft 2 rewrote are re-verified, and those in sections it did not are marked *(Draft 1 numbering)*. Draft 1: **v0.275**, HEAD `17db9bee`. Every quotation was re-verified against that tree. Compiler citations are `file:line` at that commit and were obtained **by reading only** — this proposal's author could not build or run `bezelc`, and every claim that would require a build is marked as such and left unasserted. |
| **Currency** | v0.275. **Quoted:** spec §3.6, §14.2, §14.4, §26.4, §26.5, §35.2, §35.4, Appendix A.4; stdlib §2.20, §4.4, §4.5, §4.14, §5; changelog v0.55→v0.56, v0.96→v0.97, v0.274, v0.275. **Cited without quotation:** spec §11.8, §22.6, §28.11, §32.12, §32.13; stdlib §4.8, §4.10, §4.15. **Re-verify before graduation:** §35.2's v0.233 paragraph (this proposal rewrites it), §14.2's *Derived-property constraints* subsection (the new subsection sits beside it and borrows its growth discipline), and stdlib §4.4's comment block (rewritten at v0.275 and again here). |
| **Origin** | [#3324](https://github.com/s8lap/bezel-lang/issues/3324) asked whether *"the per-member constraint form [is] admissible on an unconstrained generic class."* v0.275 answered *no* and shipped the capability anyway, by a mechanism it named only by example. This proposal asks what that mechanism **is**. |

**Decision index.** D1, D2, D5–D9 are in this proposal's §3; **D3** (`OwningArray<T>`) is in §4, which it needs a section of its own; **D4** (the census) is in §2, because every later decision reads it. D1–D4 keep the numbers the framing question gave them.

*Reference convention: unprefixed §-references are **language-spec** references at v0.275; **stdlib**, **rationale**, **changelog**, and **process** references are marked as such. References to this document's own sections are always written "this proposal's §N". ▲ marks proposed normative text, landed nowhere. Every quotation is verbatim from the v0.275 tree. **Emphasis inside a quotation is this proposal's, added to mark the clause under discussion, unless the surrounding sentence says otherwise** — the quoted words are always the source's.*

---

## 0. What this proposes, in one page

A **conditional member** is a member of a generic type whose *admissibility* — whether the member exists at all on a given receiver — depends on the enclosing type's type argument, decided per instantiation and reported at the call.

Four facts, each established in this proposal's §1 and §4:

1. **The standard library has five of them**, not the two the framing question named (this proposal's §2). The three additions are `Result<T, E>`'s four value-payload combinators, `ReadOnlySpan<T>.ToArray()`, and §5's six hashable-element LINQ operators; a sixth candidate is discharged as an ordinary missing `where` clause. Only the `Result` row has a normative definition.
2. **That definition already exists** — §35.2's *"Compiler-provided per-instantiation members (v0.233)"* paragraph (`Bezel_Language_Spec.md:10105`) states the admissibility rule, the rejection site, the diagnostic contract, and the reservation. It is complete. It is also filed in the **Choices** chapter and written entirely about `Result<T, E>`, so nothing in the language points the other four at it.
3. **v0.275 did not know it was there.** Its entry states that *"a conditional-member reading exists **nowhere**"* (`Bezel_Changelog.md:8351`). Read as a claim about a **`where`-clause** reading that is exactly right; read as the claim it makes, it is false, and the false reading is what left the restored `Array<T>.Sort()` with prose and no anchor.
4. ⚠ **The escape hatch does not work today, which Draft 1 did not know.** Spec §26.4 states that a block `where` clause *"constrains which instantiations of the extended type the extension applies to"*. The compiler does not implement it: `ExprTC::lookupExtensionMember` (`src/sema/ExprTypecheck.cpp:6937-7218`) reads `genericConstraints` only to collect free variables for target matching (`:6994`) and to spell the clause in an ambiguity diagnostic (`:7181`) — **no candidate is ever discarded for failing its `where` clause**, and no test in the tree asserts that one is. The proof is in the standard library: §5.1's shipped `extension IEnumerable<T> where T: IComparable<T>` still needs a hand-written gate at `src/sema/ExprTypecheck_Calls.cpp:5249-5295` to enforce its own clause. Filed as **[#3336](https://github.com/s8lap/bezel-lang/issues/3336)**, and it is the **keystone**: both C3's route and C5's respell run through constrained extensions, so neither works until it lands. Point 5 below is therefore true of the *language* and false of the *implementation*.

5. **Users are not as helpless as the framing assumes.** §26.4's own worked example is a constrained extension declaring `Sort()` — *"`extension IEnumerable<T> where T: IComparable<T>`"* (`Bezel_Language_Spec.md:5870`) — and §26.4 states its meaning normatively: *"A `where` clause on the block constrains which instantiations of the extended type the extension applies to."* A user **can** write a conditional member today, under any name the receiver type does not already declare, whenever the condition is a §14.2 constraint. The stdlib's five are precisely the cases where one of those two conditions fails.

**Draft 2 recommends a fourth option the framing did not contain, and that Draft 1 argued against: retire four of the five, and register what is left.**

Draft 1 recommended option (b) — register all four surviving rows as a standing privilege. A costing of option (c) (this proposal's §5.3, rebuilt) found that Draft 1 had argued the weak version of it. Option (c) is not *"delete the members"*; it is **remove the conditionality**, and on that reading four of the five rows leave the census on their own evidence:

| Row | What the compiler does today | Disposition |
|---|---|---|
| **C4** `ReadOnlySpan<T>.ToArray()` | **The member does not exist and the call is rejected** | **Delete.** Zero cost — the documents assert a call that does not compile |
| **C2** `OwningBuffer<T>.DangerousPointer()` | Ships nothing, ninety revisions | **Route to the v0.283 subtraction review** with its motivating evidence attached — *not* deleted here (§5.3.1) |
| **C5** LINQ hashable-element set | No gate found | **Respell** as a constrained extension (D9, unchanged from Draft 1) |
| **C3** `Result<T, E>`'s four combinators | Shipped, ~455 lines | **Route to its own ruling** — proposal 0075 §3.3 Option B makes it an ordinary constrained extension |
| **C1** `Array<T>.Sort()`/`Sorted()` | Shipped, hardened, five issues | **Register it** — the one true residue |

⚠ **Corrected before graduation.** An earlier wording of this paragraph said the register lands with *"one row"*. That was true only of the version in which C2 was **deleted** and C3's ruling landed simultaneously. §5.3.1 reverses the first, and D1 item 3 routes the second, so **both remain conditional members until their own forums decide them — and a member that remains is a member that must be anchored, which is the entire purpose of the register.** The true shape is:

- **Retired from the census: two.** C4 is deleted; C5 is respelled.
- **Registered: three.** C1, C2, C3.

**What makes this different from Draft 1 is not the count — it is that no row is a standing privilege.** Draft 1 would have registered four members with a growth discipline and nothing else. Every row here carries its own disposition off the register: C1 an **expiry** (re-examined when proposal 0005 self-hosts the collections), C2 a **route** (the v0.283 subtraction review, with its status as unimplemented on the row's face — D8's discipline), C3 a **route** (its own ruling, per proposal 0075 §3.3 Option B). The register is a holding pen with three occupants and three exit doors, not a grant of four permanent privileges. If all three routes complete, it empties.

**What C1's residue actually is.** Its predicate `T: IComparable<T>` is already a writable §14.2 constraint; it is blocked only by §26.5 rule 1's name rule. Renaming does not rescue it, because `Array<T>` is a declaration-less builtin (`src/common/BuiltinRegistry.cpp:679`) and `src/runtime/` is C — the sort stays a compiler intrinsic under any spelling. **C1 is an artifact of the standard library not being self-hosted, not a language design commitment**, and its register row says so and points at proposal [0005](0005-selfhost-stdlib-collections.md).

Option (a) — a user-writable per-member `where` — remains declined, on Draft 1's reasons, which Draft 2 did not disturb. But **R1's reopening bar is suspended**: it asks for evidence that the predicate vocabulary rather than the name blocks users, and #3336 establishes that the mechanism the bar presumes does not currently function (fact 4 above).

---

## 1. The defect

### 1.1 Two members, stated in prose, anchored to nothing

**`OwningBuffer<T>.DangerousPointer()`**, stdlib §4.14, since v0.186. The declaration carries the condition as a comment (`Bezel_Standard_Library.md:2506–2510`):

```bezel
  // Foreign-memory access (v0.186; language spec §32.12) — admitted only for
  //   T in the language spec §32.12 FFI-pointee set; see prose. The result type
  //   lives in System.Interop: naming Ptr<T> requires that import, using the
  //   returned value does not (language spec §2.8.3 rule 2; v0.265).
  public method DangerousPointer(): Ptr<T>;
```

and the prose states it (`:2535`):

> Declared on the value-typed presentation only, and **admitted only where `T` is in the FFI-pointee set** […] every other `T` […] **is rejected at the call's instantiation** with a diagnostic naming the set.

**`Array<T>.Sort()` / `Sorted()`**, stdlib §4.4, restored at v0.275. Same shape (`Bezel_Standard_Library.md:1620–1622`):

```bezel
  // Natural order; admitted only where `T: IComparable<T>` (see the note below).
  public method Sort();                    // in place
  public method Sorted(): owned Array<T>;  // fresh sorted copy
```

with the note (`:1626–1633`) naming the precedent explicitly:

> These are arity-0 overloads of the names above (§22.6), **admitted only where `T: IComparable<T>`; the condition is checked at the call's instantiation and reported there, the same shape `OwningBuffer<T>.DangerousPointer()` uses** (§4.14).

Neither member carries a `where` clause. Neither cites a section of the language specification that defines what "admitted only where" *means* for a member — because, for these two, none exists.

### 1.2 The mechanism does exist, and it is filed under Choices

§35.2 (`Bezel_Language_Spec.md:10105`) defines it, in one paragraph, completely:

> **Compiler-provided per-instantiation members (v0.233).** The standard library's `Result<T, E>` (stdlib §2.20) carries four **compiler-provided** value-payload combinators — `Map`, `MapError`, `AndThen`, `UnwrapOr` — declared in its own listing and admissible **only where the receiver instantiation and the member's result instantiation are both value-like** (§35.4 rule 3) […] **the rejection lands at the use site under §3.6 rule 2, the diagnostic naming the classification, the deciding payload, and the owning-match remedy** (§35.5). These members stand outside §35.11's once-and-classification-independent body discipline — their bodies are compiler-provided and exist only where the members are admissible — **on the compiler-known-standard-library footing of §3.6's behavior-dispatch reservation and the compiler-provided-conformance precedent of §11.8; a user-declared choice method cannot be conditioned on the instantiation's classification.** A call to one of these members is an **ordinary call** […]

Every element the other four instances need is in that paragraph: the admissibility predicate ("admissible only where"), the declaration convention ("declared in its own listing"), the rejection site ("at the use site"), the authority for per-instantiation evaluation ("§3.6 rule 2"), a three-part diagnostic contract, the ordinary-call clause, and the reservation. It is the general rule, written about one type, in the chapter for a different one.

Nothing points the other four at it. §4.14's `DangerousPointer` prose cites §32.12 and §32.13; §4.4's `Sort` note cites §22.6, §26.5 and §4.14. Neither cites §35.2, and §35.2 mentions neither.

### 1.3 v0.275 asserted the absence of what it was standing on

The v0.275 entry's fourth bullet (`Bezel_Changelog.md:8351`):

> **A per-member `where` on an unconstrained generic class is *not* the mechanism**, and the check mattered: it is grammatical (A.4's `MethodDecl` carries `GenericConstraints*`) but semantically undefined — §14.2 defines `where` as filtering substitutions at instantiation, and **a conditional-member reading exists nowhere**. The changelog said so **218 revisions ago**, which is *why* the extension was written in the first place.

The first half is verified below (this proposal's §1.4) and stands. The final clause is the defect. A conditional-member reading exists at §35.2, has since v0.233, and is precisely the reading the same revision then used — its own next-but-one bullet names `DangerousPointer` as the shape it is copying. The revision reached for the right mechanism by analogy to an instance of it, while recording that the mechanism does not exist.

That is not a cosmetic error. It is why the restored declarations landed with a **prose** condition and no §-anchor: an author who believes no rule exists cannot cite one. Every future reader of stdlib §4.4 is now told the condition is *"the same shape `DangerousPointer()` uses"* — a definition by reference to a second undefined instance.

### 1.4 The `where` clause genuinely cannot carry it, and the reason is not syntax

Appendix A.4's `MethodDecl` does carry the slot (`Bezel_Language_Spec.md:10725–10728`):

```ebnf
MethodDecl      ::= Visibility "static"? ("async" | "pure")? VirtualKind?
                    "method" Identifier GenericParams?
                    "(" ParamList? ")" (":" "owned"? Type)?
                    GenericConstraints* ContractClause*
                    MethodBody
```

and `GenericConstraints ::= "where" Identifier ":" Constraint …` (`:10968`) puts no restriction on which identifier. So `public method Sort() where T: IComparable<T>;` inside `class Array<T>` **parses**.

§14.2 then defines what a `where` clause means, and the definition has no room for it (`:2411`):

> The `where` clause **restricts the substitutions a type parameter accepts**

— an admission filter on substitutions, decided *at the instantiation of the parameter's own declaration*. Its own *Derived-property constraints* subsection restates the point in the strongest available words (`:2443`): *"the constraint is an admission filter on substitutions, never a declaration of the property"*, and the *Constraints on overrides* subsection binds the clause set to the dispatch surface. Under that reading a clause naming the enclosing `T` on a member of `Array<T>` says one of two things, both wrong: either it filters `Array<T>`'s instantiations (making `Array<Widget>` itself inadmissible — the whole type, not the member), or it means nothing at all.

**And the compiler puts the enclosing parameters in scope for exactly that clause.** `src/sema/TypeResolution_Checks.cpp:851–864` collects a method's own generic parameters and then appends the enclosing type's before running the constraint-cycle check, with a comment stating the intent:

> A method's `where` clause may bound a method parameter by an enclosing-type parameter (§14.2), so both are in scope.

That intent is legitimate — `where U: T` on a method of `Array<T>` is the §14.2 type-parameter-bound form and is well-defined. But the same scoping makes `where T: IComparable<T>` on a member of `Array<T>` pass the only declaration-time check this proposal could find, with no reading attached to it. Whether it is then enforced, ignored, or diagnosed elsewhere **could not be settled without a build** and is not asserted here; it is filed (this proposal's §10, issue C) and is the motivation for D7.

### 1.5 The language had this mechanism once, and lost it as collateral

The v0.96→v0.97 entry removes the whole `<owned T>` generic-argument apparatus, and one of its casualties is named exactly (`Bezel_Changelog.md:3356`):

> **Removed:** the `where T is owned` and `where T is not owned` constraint forms introduced in v0.96. **Their motivation (conditioning method existence on T's modifier inside a merged class declaration) no longer applies**, since v0.97 has no merged class declarations.

"Conditioning method existence on T" is this proposal's subject, spelled in v0.96's vocabulary. The spec still carries the tombstones: §14.4 preserves the anchors `sec-merged-declarations-and-form-dependent-members` and `sec-conditioning-method-existence-on-ts-modifier` *"so that historical changelog links resolve"* (`Bezel_Language_Spec.md:2557`).

**This matters for how the decline is classified.** The v0.97 removal was *not* a judgment that conditional members are a bad idea. It removed the vehicle (`<owned T>`), and the passenger went with it — the entry says so: the motivation "no longer applies". So process §2's rule that *"a change is a design change the moment it contradicts a recorded decision"* is engaged only weakly here: there is a recorded removal, but its recorded reason does not survive to argue against a differently-grounded mechanism. This proposal treats it as evidence about **cost** (the v0.96 apparatus needed *"substitution rules, constraint forms, inheritance projection"* and each fix *"introduced another"* issue — `:3296`), not as a decline of the idea.

### 1.6 The earlier decline, at v0.56, is the one that actually binds

`Bezel_Changelog.md:211`:

> **`Sort()` constraint moved to extension.** `Array<T>.Sort()` (the no-comparer form) **cannot express `T: IComparable<T>` on the class-level T parameter**, so it is moved to a generic extension `extension Array<T> where T: IComparable<T>`. `Array<T>.Sort(comparer)` (the comparer form) remains an unconstrained instance method.

This is the sentence v0.275 called "218 revisions ago", and it is exact: the language cannot express the condition **on the class-level parameter**. It is also the sentence that sent `Sort` down the extension road that v0.274 correctly found unreachable (§26.5 rule 1) and v0.275 correctly reversed. The capability then spent 219 revisions being implemented anyway — with no declared home in either document.

### 1.7 Users are less helpless than the question assumes, and the gap is two specific things

The framing weighs "surface is debt" against *"a shipped, tested capability users cannot replicate."* The second half needs qualifying, because §26.4 gives users a conditional-member mechanism that is fully defined:

> ```bezel
> extension IEnumerable<T> where T: IComparable<T>
>   public method Sort(): owned Array<T>
> ```
> […] A `where` clause on the block **constrains which instantiations of the extended type the extension applies to.** (`Bezel_Language_Spec.md:5870–5893`)

§26.5.1 ranks candidates *"with block-level `where` clauses satisfied by the arguments inferred"* (`:5912`), and §26.5 rule 4's ambiguity diagnostic names *"its **declared head including any `where` clause**"* (`:5903`). The constrained extension block is a first-class, user-writable, specificity-ranked conditional member.

So a user **can** write `extension Array<T> where T: IComparable<T> … SortNatural()` today, and get a member that exists at comparable element types and does not exist elsewhere. The stdlib's six instances are exactly the residue where that mechanism fails, and it fails for exactly two reasons:

| Barrier | Instances it blocks | Why the extension cannot carry it |
|---|---|---|
| **B1 — name collision** | `Array<T>.Sort()`/`Sorted()` | §26.5 rule 1: the receiver type *declares* `Sort`, *"of any arity"*, so *"extension blocks are **never consulted** for that name on that receiver"* (`:5900`). The capability is replicable; the **name** is not. |
| **B2 — predicate vocabulary** | `DangerousPointer` (FFI-pointee set), the `Result` combinators (value-like), `ReadOnlySpan<T>.ToArray()` (not borrow-bearing) | §14.2's constraint grammar admits `class`/`value`/`notnull`/`sendable`/`hashable`/`new()`/a type — and its derived-property arm is closed: *"Admitted set of this revision: **`sendable`** and **`hashable`**"* (`:2445`), growing *"only by recorded ruling"* (`:2443`). "In the FFI-pointee set", "value-like", and "not borrow-bearing" are not writable at all. |
| **— neither** | the LINQ hashable set (C5) | **Nothing blocks it.** Its condition *is* a §14.2 constraint (`hashable`, v0.214), its home *is* an extension block, and §26.6 admits *"Multiple `extension` blocks targeting the same type […] The compiler merges them"* (`Bezel_Language_Spec.md:5969`). Six operators in a second block headed `extension IEnumerable<T> where T: hashable` is legal Bezel today. This is D9. |

B1 is a naming problem with a known cost. **B2 is the real one**, and it is what makes option (a) expensive: a user-writable per-member `where` buys nothing for four of the six instances unless §14.2's predicate vocabulary is widened by three or four separate rulings first.

---

## 2. D4 — The census

**Method.** The standard library was scanned at `17db9bee` for every member whose *admissibility* is stated to depend on the enclosing type's type argument, with no `where` clause carrying the dependence. The search terms were `admitted only`, `admissible only`, `only where`, `at the call's instantiation`, `rejected at the call site`, `per-instantiation`, `rule 2`, `deliberately absent`, `declared only on`, and `presentation only`; every hit was read and classified. The exclusions are stated at this proposal's §2.2 so the boundary can be checked rather than trusted.

### 2.1 The candidates — six rows, of which five are conditional members

| # | Member(s) | Declared at | Condition on the enclosing parameter | Normative anchor today | Compiler |
|---|---|---|---|---|---|
| **C1** | `Array<T>.Sort()`, `Array<T>.Sorted()` | stdlib §4.4, `Bezel_Standard_Library.md:1620–1622`; note `:1626–1633`; prose `:1646` | `T: IComparable<T>` | **none** — prose only, defined by reference to C2 | **shipped**: `src/sema/ExprTypecheck_Calls.cpp:5521–5636`; five hardening issues (#472, #1195, #2768, #2790, #3139); **ten** test files exercise the parameterless forms — `grep -rln '\.Sort()\|\.Sorted()' tests/`, across three areas (codegen 6, module 2, ownership 2). ⚠ v0.275 says *"twelve test files across four areas"* (`Bezel_Changelog.md:8346`); that count does not reproduce under this recipe and no recipe was recorded beside it. The divergence is stated rather than resolved — the fourth area is plausibly `tests/lit/sema`, whose five `Sort`/`Sorted` cases are all **comparer**-form and so exercise a different member |
| **C2** | `OwningBuffer<T>.DangerousPointer()` | stdlib §4.14, `:2506–2510`; prose `:2535` | `T` in the §32.12 **FFI-pointee set** (and the value-typed presentation) | **none** — prose only | **not implemented**, confirmed by exhaustive grep at Draft 2: one comment in `src/` (`src/codegen/LlvmBackend_Exprs.cpp:3672`), one comment in `tests/`, one never-compiled conformance listing (`tests/conformance/manifest.tsv:492`, `stdlib_0068 … skip:signature`). ⚠ **Draft 2 corrects two Draft 1 claims here.** The citation surface is **five** spec sites, not four — Draft 1 misses `Bezel_Language_Spec.md:9764` — plus a stdlib site Draft 1 does not cite at all, §10.1 `:5420`, where `CString.Pointer`'s *stated purpose* is argv-building over `OwningBuffer<IntPtr>`. And Draft 1's *"no other spelling"* is **overstated**: `[N]IntPtr` at a by-address `extern` parameter (§32.5 `:9398`, §15.1.3 `:2828`) covers the statically-sized case normatively today, and the read direction (`Ptr<IntPtr>.LoadAt`, `:9825`) never needed this member. The genuine residual is **runtime-sized** Bezel-owned pointer storage only |
| **C3** | `Result<T, E>.Map`, `.MapError`, `.AndThen`, `.UnwrapOr` | stdlib §2.20, `:1172–1175`; prose `:1186` | receiver **and** result instantiation both **value-like** (§35.4 rule 3) | **§35.2** (`Bezel_Language_Spec.md:10105`) — the only anchored instance | **shipped**: `src/sema/ExprTypecheck_Calls.cpp:8932–9090`, with the classification diagnostic |
| **C4** | `ReadOnlySpan<T>.ToArray()` | stdlib §4.14, `:2780–2783`; prose `:2795` | value-typed `T` **and** `T` not **borrow-bearing** | partial — cites *"§3.6 rule 2"* for the borrow-bearing half; the value-typed half rides the two-presentation split | ⚠ **Draft 1 understated this, and Draft 2 corrects it: the member does not exist and the call is rejected.** The span family's member surface is a closed allow-list ending in a *"no member"* report (`src/sema/ExprTypecheck_Calls.cpp:7903–8188`, terminal diagnostic `:8174–8185`, which enumerates the surface and omits `ToArray`), reachable for every span head; codegen's `tryLowerArraySpanCall` (`LlvmBackend_Calls3.cpp:3145+`) has no arm either. **Call sites are zero** in `docs/**`, zero in `tests/**`, and zero exercised in `tests/conformance/cases/` (both listings `skip:signature-listing`, manifest `:496–497`). Draft 1 said *"no gate found by reading"*, which reads as *the member works and is ungated*; the opposite is true |
| **C5** *(respelled by D9 — needs no privilege)* | LINQ `Distinct`, `Union`, `Intersect`, `Except`, `Contains`, `ToSet` on `extension IEnumerable<T>` | stdlib §5, `:3489` (the extension head), `:3504`, `:3544–3547`, `:3566`; prose `:3588` | `T` **hashable** | partial — *"enforced at the operator's instantiation site exactly as for `Set<T>`/`Dictionary<K, V>` (§4.8/§4.10; spec §3.6 rule 2)"* | **no gate found by reading.** `checkHashContainerKeys` (`src/sema/TypeResolution_Checks.cpp:2323`) runs over hash-container *key positions*, not over these operators' call sites. Not asserted without a build; filed (this proposal's §10, issue D) |
| **C6** | LINQ `GroupBy<TKey>`, `ToDictionary` | stdlib §5, `:3560`, `:3564–3565` | `TKey` **hashable** | same sentence, `:3588` | as C5 |

**C6 is the odd one and is listed to be discharged, not carried.** `TKey` is the *method's own* type parameter, so `where TKey: hashable` is an ordinary, fully-defined §14.2 clause — `Counter<K>` already spells exactly that (`Bezel_Standard_Library.md:3450`, *"the first stdlib surface to state its key requirement with the `where K: hashable` constraint"*), and `Sum<U>`/`Min<U>`/`OrderBy<TKey>` in the same listing already carry theirs. C6 is therefore **not** a conditional member; it is a missing `where` clause on a member-level parameter, and this proposal's Edit 7 simply writes it. ▲

That leaves **five** true conditional members, in four families, of which **one** is anchored. D9 then finds that one of the five (C5) is expressible with no privilege at all and respells it, so the privileged set the register carries is **four**.

### 2.2 What was excluded, and why — so the boundary is checkable

- **Type-level instantiation conditions.** `Dictionary<K, V>`/`Set<T>` requiring hashable `K`/`T` (`:1930`, `:2232`), the owning family's `where T: class` with its runtime-managed exclusion (§14.4, `Bezel_Language_Spec.md:2528`), `OwningBuffer<T>`'s move-only exclusions. These reject the **type**, not a member of it, and §3.6 rule 2 already governs them in terms. They are the contrast case, not the subject.
- **Two-presentation declarations.** `OwningBuffer<T>`, `Span<T>`, `ReadOnlySpan<T>` are each declared twice, class-typed and value-typed — *"different specializations of the same generic class. The compiler resolves T's category at instantiation"* (`Bezel_Standard_Library.md:2513`). A whole-declaration split is a stronger and separately-recorded device (§3.6's behavior-dispatch reservation names it by example), and every member of one presentation is trivially "conditional". Only members that are conditional **within** a presentation are counted — which is why C2 and C4 appear (each is gated *again*, inside the value-typed presentation) and the rest do not.
- **Conditional interface conformance.** `OwningBuffer<T>` implements `IEnumerable<T?>` at class `T` and nothing at value `T`, and the stdlib says why in the vocabulary this proposal is about (`:2537`): *"the type-parameter-category dispatch would require **conditional interface implementation, which Bezel does not currently provide as a general mechanism**."* That is a real, adjacent, and **unresolved** gap — it is conditional *conformance*, not conditional membership, and it interacts with variance, dispatch, and the artifact interface in ways member admissibility does not. **Explicitly out of scope** (this proposal's §8.3), and named so a later reader does not read D1 as having decided it.
- **Ownership-conditioned behavior that is not admissibility.** `Array<T>` vs `OwningArray<T>` slot policy (§28.11), `ToArray()` returning a borrowing container at class `T`. Behavior, not existence.

### 2.3 What the census tells the owner

Five members. Four families. Eighty-nine revisions between the first (v0.186) and the newest ruling about one (v0.275). **One anchored.** Two of the five — C2 and C4 — are in the buffer/span family, which is also the family §3.6's reservation names as its worked example; one more (C5) turns out not to need the privilege at all (D9). So the practice is neither scattered nor uniform: it is a small set with one member that is properly a language question (C1's name collision), two that are properly compiler-known-primitive questions (C2, C4), one that is already ruled (C3), and one that is a stdlib spelling choice (C5).

The practice is small enough to enumerate on one screen and old enough that no revision has enumerated it. Draft 1 read that as the argument for a **register** over either a general feature or a removal.

**Draft 2 reads it differently, because the costing changed two of the five facts above.** An enumerable practice with no enumeration does grow silently — but three of these five rows are not a *practice* at all: C4 is a member the compiler rejects, C2 is a member the compiler never had, and C5 needs no privilege by this proposal's own D9. What the census actually shows is **one shipped conditional member (C1), one shipped-and-anchored one (C3), and three documents-only entries**. Registering all four would have written two fictions into a normative table and given them a growth discipline — C4, which the compiler rejects, and C5, which needs no privilege at all. The register is still right for what is left; what is left is **three rows, each with a route or an expiry off it** (this proposal's §0).

---

## 3. The design ▲

### D1 — A conditional member is a **standing, stated, compiler-known-standard-library privilege**, defined once in §14.2 and carried in a closed register ▲

**Decision (Draft 2).** **Retire two, register three — and no row is a standing grant** (this proposal's §0, corrected before graduation). Draft 1's decision — option (b) over all four surviving rows — is superseded; what remains of it is the mechanism, applied to a single row.

1. **C4 and C2 are deleted from the standard library**, as ordinary corrections: C4 because §4.14 documents a call the compiler rejects (the defect class v0.275 corrected in the opposite direction), C2 because ninety revisions of zero implementation and zero call sites is what process §10's subtraction review exists to act on. Neither deletion needs this proposal's mechanism, and neither is a design change — see §5.3.
2. **C5 is respelled** as `extension IEnumerable<T> where T: hashable` (D9, unchanged from Draft 1), which needs no privilege.
3. **C3 is routed to its own ruling**, not carried here. Proposal [0075](0075-result-ergonomics.md) §3.3 **Option B** already specifies it: admit the classification-neutral payload set as **one** §14.2 derived-property constraint, amend §26.2 to open extensions to the choice kind, and the four combinators become an ordinary §26.4 constrained extension — the block clause carrying the receiver half, an ordinary method-level `where U:` carrying the result half. **No member-level `where` on an enclosing parameter is needed, so D7 stands.**
4. **C1 alone gets the register row**, in the new §14.2 subsection, and the row **carries its own expiry**: the member is a compiler intrinsic because `Array<T>` is a declaration-less builtin (`src/common/BuiltinRegistry.cpp:679`) and the standard library is written in C, not because the language decided a member may be conditional. When proposal [0005](0005-selfhost-stdlib-collections.md) self-hosts the collections, the row is re-examined; §26.5 rule 1's name rule is then the only remaining obstacle, and that is a §26 question.

Everything below in this D1 — the lift of §35.2's rule into §14.2, the narrowing of §35.2 to its choice-specific half, the growth discipline, and the register's shape — is **unchanged and still recommended**. Only the table's size changes, from four rows to one.

⚠ **Sequencing.** Items 2 and 3 both produce constrained extensions, and [#3336](https://github.com/s8lap/bezel-lang/issues/3336) establishes that the compiler does not enforce a block `where` clause at all. **#3336 lands first**, or C5's respell silently admits every instantiation and C3's route has nothing to stand on.

**What Draft 1 got right and Draft 2 keeps:** that §35.2 already contains the complete rule and the move is a *lift* rather than an invention; that the rule belongs in §14.2 beside the derived-property constraints; the diagnostic contract (D2); the §3.6 boundary (D5); and the rejection of a member-level `where` (D7).

**Draft 1's decision, for the record — option (b) over four rows, executed as a lift rather than as a fresh rule:**

1. §35.2's v0.233 paragraph is **generalized** and rehomed. Its rule — admissibility per instantiation, rejection at the use site, the diagnostic contract, the compiler-known footing, the no-user-declaration clause — becomes a new §14.2 subsection, ▲ **"Conditionally-admitted members"**, sitting beside *Derived-property constraints* and borrowing its growth discipline verbatim: *"The admitted set grows only by recorded ruling — one property, one decision; there is no negated form and no user-extensible mechanism"* (`Bezel_Language_Spec.md:2443`).
2. §35.2 keeps only what is choice-specific — the four combinators' identity, the value-like condition, the §35.11 body-discipline exemption, and the ordinary-call clause — and cites the general rule for the rest.
3. The subsection carries a ▲ **register table** of every conditional member the standard library declares — C1–C4 of this proposal's §2.1, C5 having been respelled by D9 — each row naming the member, the condition, and the section that defines the condition. **New rows only by recorded ruling.**
4. Each of the four remaining stdlib prose sites gains a citation to the new subsection, so the practice is discoverable from either end.

**Why not (a) — a user-writable per-member `where`.** Four reasons, in strength order.

- **It buys almost nothing.** Three of the five instances are blocked by B2, not B1 (this proposal's §1.7), and a fourth (C5) is blocked by nothing at all (D9): the B2 conditions — "in the FFI-pointee set", "value-like", "not borrow-bearing" — are not §14.2 constraints and would each need its own admission to the derived-property constraint set before any spelling could express them. §14.2 states that each such admission is *"one property, one decision"*. Option (a) is therefore not one design change but five, of which the per-member `where` is the last and cheapest.
- **It reopens `where`'s meaning, which four other rules depend on.** §14.2's *Constraints on overrides and interface implementations* subsection makes the clause set part of the dispatch surface — *"an **added** constraint on the override would go unenforced at every virtual call (unsound)"* (`Bezel_Language_Spec.md:2490`). A member-level clause that filters *the member's existence* rather than *the parameter's substitutions* has no defined interaction with that rule, with §22.6 overload resolution (does an inadmissible overload participate in ranking?), with §2.8.2's artifact interface (is the clause a matching fact?), or with §2.8.6's breaking-change table (is narrowing a member's condition breaking, or is it member removal?). None of those questions has an obvious answer, and each is the kind that surfaces after landing.
- **The cost profile is recorded.** v0.96's `where T is owned` mechanism is the closest thing the language has tried, and the v0.97 entry's verdict on the family is explicit (`Bezel_Changelog.md:3296`): it *"required progressively more machinery — substitution rules, constraint forms, inheritance projection — each of which fixed one issue and introduced another."*
- **Users have most of it already.** §26.4's constrained extension is the user-facing conditional member, is normatively defined, and is specificity-ranked (this proposal's §1.7). What it cannot do is take a name the receiver declares — which is a §26.5 question, not a §14.2 one.

**Why not (c) — remove the capability.** Argued at full strength at this proposal's §5.3, and declined there.

**Body-locality classification (proposal 0105 §1): carve-out.** The rule is a **generic instantiation** rule — admissibility decided after substitution at a use site — which 0105 confines to exactly that class. It consumes no per-body fact and reads no information from any body but the one containing the call. **Kernel classification (v0.198): no contact** — no §28 or §30.5 rule is touched.

### D2 — The diagnostic contract, stated once and identically for every row ▲

**Decision.** ▲ A call to a conditional member at an instantiation that does not satisfy its condition is **rejected at the call** — not at the declaration of the enclosing type, and not at the instantiation of the enclosing type — and the diagnostic names four things:

1. **the member and its receiver type as instantiated**, including the offending type argument spelled out (`Array<Widget>.Sort()`, not `Array<T>.Sort()`);
2. **the condition, in the words of the section that defines it** — this is §3.6 rule 4's discipline (*"A diagnostic produced by a property-dependent rule names the property in the words of the table"*) extended from properties to conditions;
3. **the structural reason** the argument fails it — for a derived property, §14.2's *"as a component path"* sharpening; for a set membership (C2), the set enumerated;
4. **the remedy**, where the language has one — the comparer overload for C1, `OwningBuffer<IntPtr>` for C2, the owning `case move` for C3.

**Why "at the call" and not at the type's instantiation.** Because the type is well-formed without the member. `Array<Widget>` is a perfectly good array; only `Array<Widget>.Sort()` is not a call. Rejecting at the type's instantiation would make constructing the collection an error, which is both wrong and far more destructive than the condition warrants. §35.2 already rules exactly this for C3 (*"the rejection lands at the use site"*), and C1's and C2's prose already say *"at the call's instantiation"*. D2 is therefore a consolidation, not a new decision, for three of the five.

**What is wrong today, and what the edit fixes.** C1's shipped diagnostic (`src/sema/ExprTypecheck_Calls.cpp:5625–5635`) reads:

> `Array<T>.Sort() requires T to implement IComparable<T> (§3 / #472); T is not a built-in comparable type. Use Sort(comparer: (T, T) -> int) instead.`

Against D2 it fails items 1 and 3: it prints the *unsubstituted* `T` in both places, so a user reading it learns the rule but not which of their types broke it; and *"is not a built-in comparable type"* names the implementation's internal accept-list rather than the structural reason. It also cites **`§3`**, a stdlib section number retired long before v0.275 — the current home is stdlib §4.4. ▲ The corrected text is given at this proposal's §6, Edit 8. This is a compiler-side fix riding a documentation decision and is filed as such (this proposal's §10, issue B), not landed by the revision.

### D5 — §3.6's reservation covers this **partially, and by accident**; the general rule is the home, and §3.6 gains one sentence pointing at it ▲

The framing asks whether §3.6's reservation already covers conditional members. Both readings are available in one sentence, which is the defect (`Bezel_Language_Spec.md:536`):

> **Dispatch of *behavior* (not merely admissibility)** on a derived type property […] is reserved to compiler-known standard-library types; it is not a user-available mechanism in this revision […] **users still cannot write code conditioned on a type parameter's category.** The reservation bounds the **user-facing surface** — what user code may condition on — not an implementation's realization strategy for generics.

**The argument that it covers this.** The closing clause is categorical and not restricted to behavior: *users still cannot write code conditioned on a type parameter's category*. A conditional member is code conditioned on a type parameter's category. On that reading C1–C5 are already inside the reservation, they are already compiler-known standard-library types, and nothing needs deciding — only writing down.

**The argument that it does not.** The parenthetical *"(not merely admissibility)"* is a deliberate exclusion, and it is doing work: it exists to say that the reserved thing is behavior dispatch, *as distinct from* admissibility. Read that way, §3.6 says nothing about conditional members at all — they are neither reserved nor permitted; they are outside its scope. Two further facts support this reading. First, the properties §3.6 governs are its own table's ten, and **three of the five conditions in the census are not on it**: `IComparable<T>` conformance is an ordinary interface conformance (§14.2's rule), the FFI-pointee set is a §32.12 set membership, and "value-typed presentation" is a kind test. §3.6 cannot reserve a dispatch on a predicate it does not define. Second, §35.2 cites §3.6 as a **footing** — *"on the compiler-known-standard-library footing of §3.6's behavior-dispatch reservation"* — which is the word one uses for an analogy, not for a containing rule.

**Decision: they are different things, and the second reading is right.** ▲ §3.6's reservation governs **dispatch of behavior on a derived type property**. A conditional member is **dispatch of a member's existence on any predicate over the enclosing type argument** — a wider predicate class and a narrower consequence. The two overlap at exactly one census row (C3, whose condition *is* a derived type property) and are disjoint at three others.

The edit is minimal and in one direction only: ▲ §3.6 gains one sentence stating that member *admissibility* conditioned on a type argument is governed by §14.2's conditionally-admitted-members subsection, so a reader arriving at the parenthetical is sent somewhere instead of being left to infer. §3.6's own reservation is **not** widened — widening it would be a design change about behavior dispatch that this proposal has no evidence for, and would swallow C1's `IComparable<T>` condition into a property vocabulary it does not belong to.

### D6 — The rule's home is §14.2, not §22 and not a new chapter ▲

Three candidate homes were considered. **§22.6 (overloading)** is wrong: C2 and C3 are not overloads of anything, and C1's overload-ness is incidental. **A new §14.7** is wrong: it would separate the rule from the `where` clause it exists to distinguish itself from, and §1.4's whole difficulty is that readers reach for `where` first. **§14.2** is right for three reasons: it is where a reader looks when asking "how do I condition this member on `T`"; its *Derived-property constraints* subsection is a structurally identical rule (a closed admitted set, per-instantiation evaluation, growth only by ruling, a stated diagnostic shape) whose discipline can be reused rather than reinvented; and placing the register two paragraphs from the sentence that says `where` cannot do this makes the negative and the positive answer legible together.

### D7 — A `where` clause on a member, naming an enclosing type parameter, is **rejected at the declaration** ▲

**Decision.** ▲ A `GenericConstraints` clause on a member declaration whose constrained identifier is a **type parameter of the enclosing declaration** rather than of the member itself is a **compile error at the declaration**, naming §14.2 and the conditionally-admitted-members subsection, and stating that member admissibility is not user-conditionable.

**Why this belongs in the same ruling.** Today the form parses (A.4), passes the one declaration-time check this proposal could locate (`src/sema/TypeResolution_Checks.cpp:851–864` deliberately puts the enclosing parameters in scope for it), and has **no defined meaning**. A construct that parses, survives checking, and means nothing is the worst of the three available states — worse than rejecting it and worse than defining it. D1 declines to define it; D7 is the other half of that decline, and without D7 the decline is unenforced.

**The form that stays legal, precisely.** A member-level clause whose constrained identifier is the **member's own** type parameter remains admitted in every existing form, including when the *bound* names an enclosing parameter: `method Copy<U>() where U: T` on `class Box<T>` is §14.2's type-parameter-bound form and is untouched. The rejection keys on the **constrained identifier**, never on the bound. Edit 7's `where TKey: hashable` on `GroupBy<TKey>` is on the admitted side and is why the two edits ride together.

**Ramp.** ▲ Per process §2, a tightening that is not already scheduled is a design change, which is what this proposal is. Whether it lands as an error immediately or warn-first is a genuine question and is put to the owner (this proposal's §10, Q3). The evidence for landing it as an error directly: **the corpus contains zero instances.** `grep -rn 'where' docs/Bezel_Standard_Library.md` finds every stdlib `where` on a type declaration, an extension block, or a member-level parameter, and none on an enclosing parameter; the same holds for `tests/conformance/cases/`. A rejection with an empty affected set has nothing to ramp.

### D8 — `DangerousPointer()` stays, and its non-implementation is filed, not fixed here ▲

C2 is spec-only: the compiler implements nothing (this proposal's §2.1). Two consequences, decided rather than left implicit.

- **It is not removed.** Its absence from the compiler is a spec-ahead-of-impl delta — process §5 item 3's ordinary case — not evidence that the member is unwanted. It is cited from three normative spec sites (`Bezel_Language_Spec.md:9765`, `:9827`, `:9828`) and Appendix D's FFI row (`:12041`); removing it would take the FFI's only Bezel-owned-buffer address accessor for array data with it, and §32.12's argv shape (*"Pointer arrays are `OwningBuffer<IntPtr>` — exactly the argv shape"*) with that.
- **It is the register's weakest row, and the register says so.** ▲ The register's C2 row carries the delta explicitly. A register whose rows do not distinguish shipped from specified would repeat, at a smaller scale, precisely the v0.274/v0.275 failure: a document asserting a compiler behavior nobody checked. The direction of that failure is what makes this row's honesty cheap — asserting an *unimplemented* member exists costs a user a missing-member error, where asserting an implemented member *does not* exist cost v0.274 a reversal.


### D9 — The LINQ hashable set is **not** a privilege; spell it as a constrained extension block ▲

**The finding.** C5's six operators are the only census row that needs no privilege at all. Every ingredient is already legal:

- the condition is a §14.2 constraint — `hashable`, admitted at v0.214, and stdlib `Counter<K>` already spells `where K: hashable` (`Bezel_Standard_Library.md:3450`);
- the home is an extension block, whose `where` clause §26.4 defines normatively;
- and §26.6 admits more than one block per type: *"Multiple `extension` blocks targeting the same type may appear across files in the same module. The compiler merges them"* (`Bezel_Language_Spec.md:5969`).

So `Distinct`, `Union`, `Intersect`, `Except`, `Contains`, `ToSet` can move to a second block headed `extension IEnumerable<T> where T: hashable`, and the requirement stops being prose. That is not a language change; it is the stdlib writing down what it already means, in the mechanism the language already has.

**Decision ▲: spell it, and drop C5 from the register.** The register should carry the members that *cannot* be spelled; a row that can be spelled and is not is a privilege claimed without need — and each such row makes the register's growth clause easier to erode. Dropping C5 takes the privileged set from five to **four**.

**The cost, stated rather than discovered later.** The diagnostic changes shape. Today the requirement is (nominally) enforced *"at the operator's instantiation site"* with a `hashable` diagnostic; under a constrained block, a non-hashable `T` makes the block inapplicable, so the call falls through to §26.5's *"no applicable extension"* path. That is a **worse message for the same program** — it names the absence rather than the reason. Two mitigations exist, and neither is invented here: §26.5 rule 4 already requires an extension-ambiguity diagnostic to name *"its **declared head including any `where` clause**"*, so the machinery for naming an extension's constraint in a diagnostic exists; and D2's contract could be extended to the inapplicable-extension case. ▲ **Recorded as liftable, not proposed** — a general improvement to the no-applicable-extension diagnostic is a separate, wider question than this proposal.

**Separability.** D9 is independent of D1: the register works with four rows or five, and if the owner prefers to keep C5 registered (for the diagnostic's sake) nothing else changes. Put as Q5.

---

## 4. D3 — `OwningArray<T>` natural-order sorting

### 4.1 What was true when this proposal was written, and what changed under it

At `17db9bee`, stdlib §4.4's comment block ended (`Bezel_Standard_Library.md:1633–1640`, at that commit):

> `OwningArray<T>`: **NOT DOCUMENTED HERE, AND DELIBERATELY NOT DENIED.** Sema's admissibility arm is `Array`-only, but codegen's `Sort()` arm carries no receiver guard […] Whether the call is admitted **could not be settled without building the compiler** […] So the question is filed, not answered here.

**The parallel investigation settled it while this proposal was being drafted**, and its verdict is recorded on [#3325](https://github.com/s8lap/bezel-lang/issues/3325) (2026-08-21). ⚠ **Status note for a later reader:** at the time of writing, that verdict exists as an **uncommitted working-tree edit** to `docs/Bezel_Standard_Library.md` and as an open issue — it is **not** at `17db9bee` and is not part of v0.275. This proposal does not depend on it landing; D3 is stated in two branches, and the branch selection is a fact about the world, not a decision this proposal makes.

### 4.2 The two branches ▲

**Branch A — the compiler admits `OwningArray<T>.Sort()`** (what #3325 reports, and what this proposal independently reproduced by reading). ▲ Then stdlib §4.5 **declares** it, and doing so documents a shipped capability rather than adding one. Four sites, each verified here at `17db9bee`:

| Site | What it does |
|---|---|
| `src/sema/ExprTypecheck_Calls.cpp:4915` | §22.5's owning-container allow-list contains `mm == "Sort"`, name-keyed and arity-blind — so the *"no member `Sort` on `OwningArray`"* rejection at `:4971` never fires |
| `:5673` | `if ((colName == "Array" \|\| colName == "OwningArray") && m == "Sort" …)` names `OwningArray` **explicitly** as a legitimate `Sort` receiver |
| `:5694` | the only `OwningArray` rejection arm enumerates `Reversed`/`Sorted` and **omits** `Sort` |
| `src/common/BuiltinRegistry.cpp:98` | `kArrayBackedCollections` contains `"OwningArray"`, so codegen's name-and-arity-keyed `Sort` arm (`src/codegen/LlvmBackend_Calls.cpp:3572`) is reached |

**Branch B — some path this reading missed rejects it.** ▲ Then stdlib §4.5 declares nothing, and §4.4's comment says so affirmatively with the site that rejects it named. **Only a build settles which branch holds** — #3325 states the same and supplies the probe.

### 4.3 The decision, which is the same under both branches ▲

▲ **`OwningArray<T>` should have `Sort()`.** Under branch A that is a documentation edit; under branch B it is new work, and it should be done. The reasons do not depend on the branch:

**1. The ownership question the framing raises is already answered, and answered in this document.** A sort permutes owned slots. That is sound, and the standard library has said so since the sorting contract landed — stdlib §4.4's contract clause (3) (`Bezel_Standard_Library.md:1648`):

> A comparer that throws mid-sort propagates the exception and leaves the collection holding **exactly the original elements in unspecified order** (none lost, none duplicated). **For `OwningArray<T>` this is the Appendix C disposed-exactly-once guarantee: every owned slot remains present and singly-held on the throw edge.**

The reason is structural: a permutation is a bijection on slots, so every element ends in exactly one slot and every slot holds exactly one element. Nothing is copied, nothing is dropped, no ownership is duplicated or lost — even on the exception edge, which is the only edge where a permutation could go wrong. This is precisely why `OwningArray<T>` may have `Sort` and may **not** have `Sorted`: the copy-forms *"would either double-own each element […] or drain the source"* (`:1646`), and the compiler rejects them with that reason in its own words (`src/sema/ExprTypecheck_Calls.cpp:5694–5704`).

**2. `OwningArray<T>.Sort(comparer)` already exists** (`Bezel_Standard_Library.md:1681`), and has since long before this question. So the owning-sort permutation is not a thing D3 proposes; it is a thing the library ships. The parameterless overload adds **exactly one thing to it: the comparability gate.** It introduces no ownership question that the comparer form has not already answered.

**3. The asymmetry has no defensible reason.** `Array<T>` and `OwningArray<T>` differ in who owns the elements. Natural-order comparison reads elements; it does not consume them, and §4.5 already states that *"`Sort`'s comparer parameter takes borrows of T (the comparer doesn't consume; it inspects)"* (`:1699`). A gate keyed on `T: IComparable<T>` is a statement about `T`, identical on both receivers.

**4. v0.274's error was one-directional, and the second half is still uncorrected.** #3325 reports that the parameterless `Sort` was documented on **both** types from v0.242 and deleted from both by v0.274's ruling 2 — of which v0.275 reversed only the `Array<T>` half. If that is right, D3 is not an addition at all; it is the rest of a reversal.

### 4.4 The interaction that matters more than the declaration ▲

▲ **Under branch A, `OwningArray<T>.Sort()` is a conditional member whose condition is not enforced.** The gate at `src/sema/ExprTypecheck_Calls.cpp:5521` is `colName == "Array"`; #3325 traces the consequence to codegen's `isStringElem` fallback (`src/codegen/LlvmBackend_Calls.cpp:3609`, `:3661`), which selects `bz$strcmp` for any class element with no `CompareTo` — **`strcmp` over each element's vtable pointer**. That is the exact defect #2768 and #2790 fixed on the borrowing side, never carried across, and #2790's was a *measured* silent-wrong-answer.

Two things follow, and they pull in opposite directions — which is why this proposal states both rather than pretending the decision is free:

- **For D3:** the defect is an argument *for* the register, not against the member. A member whose condition is stated in prose on one receiver and not stated at all on its twin is exactly what an unregistered practice produces. Under D1 the register carries one row for the family, and the row's condition is the same on both receivers by construction.
- **Against landing D3's declaration ahead of the fix:** documenting `OwningArray<T>.Sort()` while its gate is missing publishes a member that is *specified* to reject non-comparable elements and *observed* to accept them silently. ▲ **Ruling: the declaration lands with the condition stated, and #3325 is filed as its spec-ahead-of-impl delta** (process §5 item 3) — the same posture every other row takes, and the same posture C2 takes for a member the compiler does not implement at all. The alternative — withholding the declaration until the compiler catches up — would leave the documents asserting an absence for a call that compiles, which is the error v0.274 made and v0.275 spent a revision reversing.

**What D3 does *not* propose.** No `Sorted()`, no `Reversed()`, no `ToArray()` on `OwningArray<T>`. Those are copy-forms, are unsound for an owning collection, are already rejected with reasons in both documents and the compiler, and nothing here disturbs them. `ToArray()` remains reachable-by-extension exactly as v0.275's #3286 ruling states (`Bezel_Standard_Library.md:1646`).

---

## 5. The options, compared honestly

### 5.1 Option A — define it in §14.2 as a **user-writable** feature (framing option (a)) — **declined, with a reopening bar**

The strongest version: `where` on a member may name an enclosing type parameter; the clause filters the *member's* admissibility rather than the *type's* substitutions; a call at a non-satisfying instantiation is rejected under D2's contract.

**What it would genuinely buy.** One thing, and it is real: `Array<T>.Sort()` becomes *writable*, so the stdlib's own declaration would stop being a privilege. That is not nothing — a standard library that can only be written by the compiler is a standard library users cannot audit against the language, and process §6's cardinal rule (*no invented current stdlib APIs*) is easier to keep when the stdlib is expressible.

**What it costs, itemized.**

| Cost | Detail |
|---|---|
| **It solves one row in five** | C1 only. C2, C3 and C4 are blocked by B2 (§1.7): their predicates are not §14.2 constraints, and §14.2 admits new ones only *"by recorded ruling — one property, one decision"* (`Bezel_Language_Spec.md:2443`). C5 needs no new mechanism at all (D9). Option A is the fifth of five design changes, not the one. |
| **Four undefined interactions** | override/interface-implementation constraint inheritance (`:2490` — the rule is explicitly about soundness under virtual dispatch); §22.6 overload resolution (does an inadmissible overload rank?); §2.8.2's artifact interface (is the clause a matching fact?); §2.8.6's breaking-change table (is tightening a member's condition *breaking* or *member removal*?). |
| **A recorded cost profile** | the v0.96 `where T is owned` family, whose v0.97 obituary is *"required progressively more machinery […] each of which fixed one issue and introduced another"* (`Bezel_Changelog.md:3296`). |
| **Surface is debt** | §2.8.5's *"real programs hitting real walls"* bar. There are no users yet, and the wall this clears is `SortNatural` vs `Sort` on one collection type. |

**Reopening bar** (this proposal's §9, R1): a body of real Bezel code in which the constrained-extension mechanism (§26.4) is reached for and **fails for reasons other than the §26.5-rule-1 name collision** — i.e. evidence that B2 bites users, not just the stdlib. Absent that, the case for A is a case for widening §14.2's predicate vocabulary, which is a different proposal.

### 5.2 Option B — a stated, registered, compiler-known privilege (framing option (b)) — **recommended**

D1. Costs: one spec subsection, one register table, one narrowing of §35.2, one sentence in §3.6, four stdlib citations, and one stdlib respelling (D9). Adds **no** grammar, **no** token, **no** user-facing capability, and **no** admissibility rule that is not already being applied. Buys: the practice becomes enumerable, citable, and bounded, and the five prose sites stop defining themselves by reference to each other.

The honest weakness: a register is a *record*, not an *enforcement*. Nothing mechanically stops a future revision from writing "admitted only where…" into the stdlib without a row. Two mitigations, both cheap: the growth clause is normative (*"only by recorded ruling"*, borrowed verbatim from §14.2's neighbouring subsection), and `scripts/check_docs.sh` grew a **SETS** check at v0.275 precisely for *"a passage naming the set and enumerating it"* — the register is exactly that shape. Whether SETS can be parameterized to this register is a tooling question, filed (this proposal's §10, issue E), not claimed.

### 5.3 Option C — remove the capability (framing option (c)) — ⚠ **Draft 2: substantially ADOPTED**

**Draft 1 declined this, and Draft 1 argued the wrong version of it.** Draft 1 read option (c) as *delete the members*, priced that, and declined it — correctly, for that reading. The strong version is different: **keep every capability that exists and remove the *conditionality*,** by re-homing each member onto a device the language already has or by deleting what the compiler never had. A three-lane costing against v0.276 (`c1-rename`, `c3-result`, `c24-spans`) settled it row by row.

#### 5.3.1 What the costing found, row by row

**C4 — delete. Zero cost, and the strongest row of the five.** The member does not exist in the compiler and the call is rejected (§2.1, corrected). Call sites are zero everywhere. The replacement is the one the standard library already prescribes for the sibling family — *"a loop or an explicit `Array<T>`"* — and v0.272's decline of the mirror on `ReadOnlyArraySpan<T>` priced the resulting asymmetry as *"the migration's one genuine loss"*: **that loss is zero**, because the `ReadOnlySpan` side does not compile either. Deleting C4 removes a documents-only asymmetry rather than creating one.

**C2 — route to the v0.283 subtraction review; do *not* delete it here.** ⚠ **This reverses Draft 2's own first recommendation**, on evidence found after that recommendation was written: the question *why was this needed in the first place* was put to proposal [0033](0033-ffi-v2.md), and the answer defeats the deletion.

`DangerousPointer()` was not a convenience. It came out of the **2026-07-07 owner-commissioned FFI design review**, which ran the three canonical real bindings against §32 and found all three failing — sqlite3, libcurl, and **BLAS**. This member is the BLAS fix, and 0033 §3.5 names it as such: *"the double-copy exemplar **dissolved by `DangerousPointer`**"*. Its worked example is

```bezel
var c := new OwningBuffer<double>(m * n);
cblas_dgemm(…, a.DangerousPointer(), k, b.DangerousPointer(), n, 0.0, c.DangerousPointer(), n);
return c;                                              // zero copies, zero swizzling
```

against v1's only route: *"per-element `BitConverter` swizzling into `Array<byte>`, `RawBytes.From` (a copy), the call, `CopyTo` (a copy), per-element reassembly — per matrix, per direction."* The second motivating case is argv marshalling for `execve`/`posix_spawn`, which proposal [0011](0011-process.md) needs and which 0033 §9 routes through `CString.Pointer` + `OwningBuffer<IntPtr>.DangerousPointer()`.

**Both motivating cases are runtime-sized** — `m * n`, and an argv whose length is not a literal — which is precisely the residual §2.1 identifies as having **no alternative spelling**. §32.5's `[N]IntPtr` at a by-address `extern` parameter covers the static case and neither of these. So the residual is not a marginal leftover: it is the whole of what the member was built for.

That does not make the row healthy — ninety revisions of zero implementation is exactly what a subtraction review is for, and §2.8.5's *"real programs hitting real walls"* bar is unmet because there are no real programs. But **"ships nothing" is evidence about implementation priority, not about design merit**, and Draft 1 said so in terms: *"the spec-ahead-of-impl gap is process §5 item 3's ordinary condition, not evidence of an unwanted member."* Draft 2 overrode that on the strength of the census alone and was wrong to; the subtraction review is the forum that can weigh the BLAS evidence against the ninety revisions, and this proposal is not. **Draft 1's R3 is restored, with the motivating evidence now attached to it.**

Draft 2's remaining findings on this row stand. Draft 2 adds that the presentation-split alternative is **not available in principle**: §32.12 `:9820` states the pointee set *"is an enumerated closed set plus one derived property; it is deliberately not a second §3.6 table row"*, so the device has nothing to split on, and §14.2 admits only §3.6 properties. The honest residual is runtime-sized Bezel-owned pointer storage (§2.1, corrected) — a capability the compiler has never provided, no program has used, and whose absence costs a missing-member error rather than a wrong answer.

**C5 — respell.** Unchanged from D9.

**C3 — routed, not deleted, and this is where Draft 1's decline actually fails.** Draft 1 declined option (c) partly because *"the removal does not achieve its own goal — C3 stays regardless"*. That is true of deletion and false of the strong reading. §26.2's own exclusion sentence names the way out:

> Choices. The extendable list above is closed against the choice kind — **no [§14.2] constraint names the value-like **classification bound** a payload-generic extension surface would need, and the value-payload combinator surface of stdlib `Result<T, E>` is **compiler-provided** ([§35.2]), not an extension block.

The privilege exists *because* the constraint vocabulary is too narrow — stated in the section that closes the escape route, added at **the same v0.233 landing that created C3**, citing C3 as its reason. Admit the constraint and the exclusion's own stated ground evaporates. Proposal 0075 §3.3 **Option B** specified exactly this and was declined at v0.233 as *"permanent user-facing constraint vocabulary bought on one program's evidence"*.

**That stated ground no longer holds, which is the new evidence a reopening requires.** It is not one program's evidence: it is five standard-library members across four types, unenumerated for eighty-nine revisions, three of them unimplemented, plus one closed extension kind, plus three consecutive revisions of churn over the practice. The demand side is unchanged; the **cost** side is what the census measured for the first time.

⚠ **Draft 1 cites neither §26.2 nor proposal 0075 anywhere** — verified by grep at Draft 2. For a proposal whose central claim is why the privilege is necessary, missing the section that says why it exists, and the prior proposal that specified the alternative, is a research defect and is recorded as one in §11.

**C1 — stays, and the reason is not what Draft 1 assumed.** The obvious removal is a rename: the predicate `T: IComparable<T>` is already a writable §14.2 constraint, so if the name `Sort` were free, a constrained extension would do. It does not work, for two reasons independent of the name. **There is no Bezel-source standard library** — `Array<T>` is a declaration-less builtin (`src/common/BuiltinRegistry.cpp:679`), `src/runtime/` is C — so the sort stays a compiler intrinsic lowered from a name-keyed arm under any spelling; the compiler line delta of either rename direction is ≈ zero. And **the constrained-extension mechanism is itself unenforced** ([#3336](https://github.com/s8lap/bezel-lang/issues/3336)). A rename would disturb five hardening suites and two spec worked examples to relocate a privilege rather than remove it.

#### 5.3.2 The two hypotheses that failed, recorded so they are not re-tried

**The two-presentation device does not absorb these gates.** `OwningBuffer<T>`/`Span<T>`/`ReadOnlySpan<T>` are each declared twice by `T`'s category, and §2.2 excludes that device from the census as *"a stronger and separately-recorded device"*. Draft 2 tested whether a finer split could absorb the second gate. It fails for C2 categorically (§32.12 `:9820`, above) and for C3 four times over — `Result<T, E>` is a **choice**, and §26.2 closes extensions against choices; the split axis is wrong (category, not the three-way choice classification); the **result** half is structurally invisible to a receiver-keyed split, since `Map<U>` returns `Result<U, E>` and `U` is inferred at the call site, so a value-like `T` with an owning `U` sits inside the admitting presentation; and the device is not privilege-free anyway — §3.6 `:536` reserves *behavior* dispatch to compiler-known standard-library types, and respelling C3 that way moves it from the weaker dispatch to the **stronger** one. For C4 a split is structurally available but buys one row, a worse diagnostic, and a third duplicated listing.

**Widening §14.2 wholesale is not the answer either.** The admitted set is `sendable` and `hashable` only (`:2446`) out of §3.6's twelve rows. C1 and C5 are already writable. C2 is unreachable in principle. **C4 is unreachable as an admission** — its predicate is a *negation*, and §14.2 says *"there is no negated form"* categorically; reaching it would need a new positively-spelled §3.6 row plus a §2.8.6 row of inverted polarity. Only **C3** is a clean *one property, one decision*. Three permanent contextual keywords to make two of five rows writable is not a trade the §2.8.5 evidence bar supports; **one**, for the row that is shipped, anchored, and guide-taught, is.

#### 5.3.3 What survives of Draft 1's decline

Two things, and both are load-bearing:

- **C1's removal stays declined**, on Draft 1's reasons in full: v0.275 reversed exactly this deletion hours before Draft 1 was written, the capability ships with five hardening issues behind it, and reopening needs new evidence against those stated reasons. Draft 2 has none — the costing *strengthens* the decline by showing a rename does not even relocate the problem. R2's bar is unchanged.
- **The mechanism is still needed.** Retiring four rows does not retire the practice: C1 remains, and until #3336 lands and C3's ruling is taken, C3 does too. A register of one with a stated expiry is a smaller claim than Draft 1's four, not an absent one.

*(Draft 1's argument, superseded but kept because its premises are the record Draft 2 argues against:)*

**The case for C, at full strength.**

1. **One of the two named members ships nothing.** `DangerousPointer()` has no sema arm, no codegen arm, and a `skip:signature` conformance row (this proposal's §2.1, C2). It has been in the document since **v0.186** — eighty-nine revisions — and no program has ever called it. Deleting it costs zero lines of compiler and zero test files, and process §10's subtraction review (*"what is no longer earning its place?"*, first pass due at **v0.283**) exists for precisely this shape: surface carried, never exercised.
2. **The capability C1 provides is a one-liner, not a no-liner.** #3324 said so in its own words: *"callers pass `IComparable.CompareTo` as the comparer explicitly […] the natural-order case stays a one-liner rather than a no-liner."*
3. **It is replicable, modulo the name.** §26.4's constrained extension gives a user `SortNatural()` with a real `where` clause today (§1.7). The stdlib could take that spelling too — v0.274 proposed exactly it.
4. **Removing collapses the whole question.** No register, no §14.2 subsection, no §3.6 sentence, no D7 — the entire proposal evaporates, and the language gets *smaller*, which is the direction §2.8.5 and process §10 both push.

**Why it is declined anyway.**

- **For C1, the project has already run this experiment and reversed it — earlier the same day this proposal was written.** v0.274 deleted the parameterless forms; v0.275's headline is that doing so was wrong, and its reason is not aesthetic (`Bezel_Changelog.md:8346`): *"for one revision the documents asserted a compile error for a call that compiles, runs and sorts."* The capability ships — sema gate, lowering, interface-element dispatch, cross-module paths, ten test files, **five** hardening issues, one of them a measured silent-wrong-answer. Removing it now means deleting working, hardened, tested compiler code to shrink a document. That is not subtraction; it is regression with a paperwork motive. Process §2's rule applies with full force: this would contradict a recorded decision **hours old** (v0.275 landed at `17db9bee`, 2026-08-21 07:29 UTC), and reopening it needs *new evidence against the stated reasons*. This proposal has none — it has the opposite.
- **For C2, "unimplemented" is the wrong premise.** The spec-ahead-of-impl gap is process §5 item 3's ordinary condition, not evidence of an unwanted member; the compiler is behind on a great deal. And the member is load-bearing in text: three normative §32 sites and Appendix D's FFI row cite it (`Bezel_Language_Spec.md:9765`, `:9827`, `:9828`, `:12041`), and it is the FFI's only address accessor over array-shaped Bezel-owned storage — §32.12's argv shape has no other spelling. Removing it removes a documented FFI capability to avoid documenting a mechanism.
- **The removal does not achieve its own goal.** C3 stays regardless: the `Result` combinators are shipped, guide-taught (`Bezel_Guide_02_Errors_and_Nullability.md:143–170`), and normatively defined at §35.2. So option C deletes two rows and leaves the practice — and its one anchored definition, still filed under Choices — exactly where it is. It buys a smaller stdlib, not a smaller language.

**What survives from C, and is adopted.** Its diagnosis is right about one thing: **an unexercised member should be visible as unexercised.** D8 adopts that — C2's register row carries its spec-ahead-of-impl status on its face — and this proposal recommends that the **v0.283 subtraction review** take `DangerousPointer()` as a named candidate (this proposal's §10, Q4), with the register row as the evidence it needs. That is the right forum for a removal; a proposal about defining a mechanism is not.

### 5.4 Option D — do nothing — **declined**

The status quo is not stable. v0.275 restored C1 with prose that defines the condition by pointing at C2, and C2 defines it by pointing at nothing; #3325 has since found the same condition unenforced on a third receiver. Each of the last three revisions has touched this practice without naming it, and each has been corrected by the next. The cost of "nothing" is measured, not hypothetical.

### 5.5 Prior art — what other languages do ▲

⚠ **Draft 1 contained none of this**, which matters because it declines option (a) without recording that option (a) is the mainstream design. Three strategies exist:

**(1) Constrain the member itself — option (a).** **Rust** does it two ways: conditional inherent impls (`impl<T: Ord> Vec<T> { fn sort(&mut self) }`) and `where` clauses on individual methods naming the *enclosing* impl's parameter (`impl<T> Container<T> { fn dedup(&mut self) where T: PartialEq }`) — which is D7's form, admitted. **Swift**: constrained extensions (`extension Array where Element: Comparable`), plus conditional *conformance*, which is this proposal's R4. **C++20**: a `requires` clause on a non-template member of a class template; before concepts, `enable_if` SFINAE plus the lazy-instantiation rule, which made members of class templates *accidentally* conditional — ill-formed for some `T`, but only when called.

**(2) Constrain a separate extension or free function** — Bezel's §26.4 today. **C#** is the closest relative and has **Bezel's exact gap**: a `where` clause on a non-generic method of `Class<T>` is a hard error (CS0080), so C# routes everything through constrained extension methods — and hits the same collision, because instance methods beat extension methods in resolution, which is §26.5 rule 1 by another name. **Java** and **Go** are in this family (`Collections.sort`; Go forbids method-level type parameters outright).

**(3) Require evidence at the call.** **Scala**'s `def sorted(implicit ord: Ordering[A])`, **Haskell**'s instance contexts. The member always exists; the call needs a witness. Bezel has no analogue.

**The finding that bears on D1.** Rust cannot express these cases with separate impl blocks either — two inherent impls defining the same method name on one type are a duplicate-definition error *even when their bounds are disjoint*, because Rust has no overlap-based inherent resolution. The method-level `where` clause exists in Rust **because the separate-block route collides on the name**, which is precisely why C1 needs a privilege here. A mature language facing the identical obstacle chose option (a).

That does not meet R1's bar as worded — R1 asks for evidence that the *predicate vocabulary*, not the name, blocks users — but it suggests R1 is aimed at the wrong thing: the name collision is not a lesser problem Rust worked around, it is the reason Rust built the feature. Combined with #3336 (the constrained-extension route does not currently function at all), **R1 should be re-drawn at graduation rather than defended as written.**

**And what no one else does is the status quo.** C++ standard libraries leaned on lazy instantiation and C# has runtime intrinsics, but no mainstream language adopts *"the standard library may do what users cannot"* as a **stated** design. Draft 1's option (b) makes that honest by writing it down, which is a real improvement over an unstated privilege; Draft 2's point is that it should be written down for **one** member with an expiry, not four as a standing grant.

### 5.6 The comparison

⚠ **Draft 2 replaces this table.** Draft 1's version scored option C at *"2 of 5 rows resolved"* and *"contradicts a recorded decision — v0.275, hours old"*, both of which priced the deletion reading. The strong reading of C resolves four and contradicts nothing: v0.275's decision is about **C1**, which Draft 2 also declines to remove.

| | A (user feature) | B (register all four) | **C′ (retire two, register three with routes)** | D (nothing) |
|---|---|---|---|---|
| Census rows resolved | 1 of 5 | 5 of 5, by registering 2 fictions | **5 of 5: one deleted, one respelled, three registered with routes** | 0 |
| Normative claim made | a new admissibility form | a standing four-row privilege | **three rows, none standing: one expiry, two routes** | none |
| Contradicts a recorded decision | v0.56, v0.97 (weakly) | no | **no** — v0.275 governs C1, which C′ keeps | no |
| New grammar / tokens | new semantics for an existing form | none | **none here**; C3's routed ruling costs one contextual keyword | none |
| Undefined interactions opened | four | none | **none** | four already open |
| Compiler work | large | none for the rule | **#3336 (a fix owed anyway), then two deletions** | none |
| Documents a member the compiler rejects | — | **yes (C4)** | no | yes |

*(Draft 1's table, superseded:)*

| | A (user feature) | **B (register)** | C (remove) | D (nothing) |
|---|---|---|---|---|
| Census rows resolved | 1 of 5 | **5 of 5** | 2 of 5 | 0 |
| New grammar / tokens | possibly none, but new semantics for an existing form | **none** | none | none |
| Undefined interactions opened | four (§14.2 overrides, §22.6, §2.8.2, §2.8.6) | **none** | none | four already open |
| Contradicts a recorded decision | v0.56, v0.97 (weakly — §1.5) | **no** | **v0.275, hours old** | no |
| Compiler work | large | **none for the rule**; two filed deltas | deletion of hardened code | none |
| Leaves the practice enumerable | yes | **yes** | partly (C3 remains) | no |

---

## 6. Exact edits ▲

Nine edits. Line numbers are at `17db9bee`. Every `OLD:` string was verified unique in its file at that commit. ▲ marks proposed text.

### Edit 1 — spec §14.2 gains the *Conditionally-admitted members* subsection ▲

Inserted immediately before `#### Interface conformance` (`Bezel_Language_Spec.md:2450`, the `<a id="sec-interface-conformance">` anchor line at `:2449`), i.e. directly after the *Derived-property constraints* subsection.

**OLD:**
```
<a id="sec-interface-conformance"></a>
#### Interface conformance
```

**NEW:** ▲
```
<a id="sec-conditionally-admitted-members"></a>
#### Conditionally-admitted members

A **conditionally-admitted member** is a member of a generic type whose *admissibility* — whether it exists on a given receiver at all — is decided per instantiation from the enclosing type's type arguments, rather than from the member's own signature. The enclosing type is well-formed at every admitted substitution; only the member is absent where the condition fails.

Five rules govern every such member:

1. **Compiler-known standard-library types only.** A conditionally-admitted member may be declared only on a type this specification's standard-library companion declares, and only where a row of the register below names it. **A user declaration cannot be conditioned on the enclosing type's arguments**, and a `where` clause on a member naming a type parameter of the *enclosing* declaration is a compile error naming this subsection (a clause naming the *member's own* type parameter is the ordinary constraint form of this section and is unaffected, including where its bound names an enclosing parameter).
2. **Decided after substitution, at the use site.** The condition is evaluated on the receiver's instantiated type arguments, following the per-instantiation rule of [§3.6](#sec-derived-type-properties) rule 2. A call at a non-satisfying instantiation is rejected **at the call**, never at the declaration or at the enclosing type's instantiation.
3. **The diagnostic.** The rejection names (a) the member and the receiver type as instantiated, with the offending type argument spelled out; (b) the condition, in the words of the section that defines it; (c) the structural reason the argument fails it — as a component path where the condition is a derived type property (§3.6 rule 4); and (d) the remedy the register row states.
4. **An ordinary call otherwise.** Where the member is admitted, the call is an ordinary call: overload resolution ([§22.6](#sec-overloading)), argument evaluation order ([§22.5](#sec-optional-parameters-and-named-arguments)), purity, and ownership all apply unchanged. Conditional admissibility grants no exemption from any of them.
5. **Part of the public contract.** A change to a member's condition is a breaking change for clients relying on it, exactly as [§3.6](#sec-derived-type-properties) rule 3 makes a derived-property flip one.

**The register.** The set is closed and grows **only by recorded ruling** — one member family, one decision; there is no negated form and no user-extensible mechanism.

| Member(s) | Condition on the enclosing type argument | Condition defined in | Remedy the diagnostic states |
|---|---|---|---|
| `Array<T>.Sort()`, `Array<T>.Sorted()` — and `OwningArray<T>.Sort()` | `T` conforms to `IComparable<T>` (this section's *Interface conformance*) | this section | the comparer-taking overload |
| `OwningBuffer<T>.DangerousPointer()` | `T` is in the FFI-pointee set (value-typed presentation only) | [§32.12](#sec-foreign-memory), [§32.13](#sec-c-compatible-records) | `OwningBuffer<IntPtr>` for pointer elements |
| `Result<T, E>.Map`, `.MapError`, `.AndThen`, `.UnwrapOr` | receiver **and** result instantiation both value-like | [§35.4](#sec-choice-classification) rule 3; member-specific clauses at [§35.2](#sec-choice-members) | the owning match ([§35.5](#sec-choice-matching)) |
| `ReadOnlySpan<T>.ToArray()` | value-typed `T` that is not **borrow-bearing** | [§9.7](#sec-records-and-disposal), [§3.6](#sec-derived-type-properties) | `move span[i]`, or the owning collections' transfer surface |


(The standard library's `hashable`-requiring sequence operators are **not** in this register: their requirement is an ordinary [§14.2](#sec-constraints) constraint on a [§26.4](#sec-generic-extensions) extension block, and the standard library spells it there.)

The register is a statement about the *documents*; a row whose member the compiler does not yet implement, or whose condition it does not yet enforce, is an ordinary specification-ahead-of-implementation delta and is tracked as one.

**This is not the `where` clause.** A `where` clause filters the substitutions a type parameter accepts, decided when that parameter's own declaration is instantiated (this section's opening). A conditional member's condition decides nothing about substitutions: `Array<Widget>` is a well-formed type for a `Widget` with no ordering, and only `Array<Widget>.Sort()` is not a call. **The user-writable form of a conditional member is the constrained extension block** ([§26.4](#sec-generic-extensions)), whose `where` clause *"constrains which instantiations of the extended type the extension applies to"* — available for any member name the receiver's type does not itself declare ([§26.5](#sec-resolution) rule 1).

<a id="sec-interface-conformance"></a>
#### Interface conformance
```

### Edit 2 — spec §35.2's v0.233 paragraph narrows to its choice-specific half ▲

`Bezel_Language_Spec.md:10105`.

**OLD (first clause):**
```
**Compiler-provided per-instantiation members (v0.233).** The standard library's `Result<T, E>` (stdlib [§2.20](Bezel_Standard_Library.md#sec-result)) carries four **compiler-provided** value-payload combinators — `Map`, `MapError`, `AndThen`, `UnwrapOr` — declared in its own listing and admissible **only where the receiver instantiation and the member's result instantiation are both value-like** ([§35.4](#sec-choice-classification) rule 3)
```

**NEW:** ▲
```
**Compiler-provided per-instantiation members (v0.233).** The standard library's `Result<T, E>` (stdlib [§2.20](Bezel_Standard_Library.md#sec-result)) carries four **conditionally-admitted members** ([§14.2](#sec-conditionally-admitted-members)) — the value-payload combinators `Map`, `MapError`, `AndThen`, `UnwrapOr` — declared in its own listing and admissible **only where the receiver instantiation and the member's result instantiation are both value-like** ([§35.4](#sec-choice-classification) rule 3)
```

The remainder of the paragraph is unchanged **except** its penultimate sentence, whose general half moves to Edit 1:

**OLD:**
```
on the compiler-known-standard-library footing of [§3.6](#sec-derived-type-properties)'s behavior-dispatch reservation and the compiler-provided-conformance precedent of [§11.8](#sec-static-interface-members); a user-declared choice method cannot be conditioned on the instantiation's classification.
```

**NEW:** ▲
```
under [§14.2](#sec-conditionally-admitted-members) rule 1, whose compiler-known-standard-library footing is [§3.6](#sec-derived-type-properties)'s behavior-dispatch reservation together with the compiler-provided-conformance precedent of [§11.8](#sec-static-interface-members); a user-declared choice method cannot be conditioned on the instantiation's classification.
```

*Nothing normative is lost: every clause of the old paragraph survives, in §35.2 if choice-specific and in §14.2 if not. The composition check for this edit is that §35.2 and Edit 1 must state rule 2's rejection site once, not twice — §35.2 keeps its own "the rejection lands at the use site" sentence because it also names the classification-specific diagnostic content, which §14.2's rule 3(b)/(c) delegates to the defining section.*

### Edit 3 — spec §3.6's reservation paragraph gains one pointer sentence ▲

`Bezel_Language_Spec.md:536`, appended to the paragraph.

**OLD (final sentence):**
```
The reservation bounds the **user-facing surface** — what user code may condition on — not an implementation's realization strategy for generics, which [§11.8](#sec-static-interface-members) leaves open so long as the observable binding is unchanged.
```

**NEW:** ▲
```
The reservation bounds the **user-facing surface** — what user code may condition on — not an implementation's realization strategy for generics, which [§11.8](#sec-static-interface-members) leaves open so long as the observable binding is unchanged. Conditioning a member's **admissibility** — as distinct from its behavior — on the enclosing type's arguments is a separate mechanism with its own register and its own reservation, defined at [§14.2](#sec-conditionally-admitted-members); its conditions are not confined to the properties of this section.
```

*The reservation itself is not widened. The parenthetical* "(not merely admissibility)" *stays, and now points somewhere.*

### Edit 4 — spec Appendix C.1 gains one row ▲

**NEW row** (after the `default(C)` / `where T: new()` row, `Bezel_Language_Spec.md:11751`): ▲
```
- A `where` clause on a member declaration may constrain only that member's own type parameters; a clause naming a type parameter of the enclosing declaration is a compile error ([§14.2](#sec-conditionally-admitted-members)).
```

### Edit 5 — stdlib §4.4's comment block cites the rule ▲

`Bezel_Standard_Library.md:1626–1633`.

**OLD:**
```
// These are arity-0 overloads of the names above (§22.6), admitted only
// where `T: IComparable<T>`; the condition is checked at the call's instantiation
// and reported there, the same shape `OwningBuffer<T>.DangerousPointer()` uses
// (§4.14).
```

**NEW:** ▲
```
// These are arity-0 overloads of the names above (§22.6), and CONDITIONALLY
// ADMITTED members in the sense of language spec §14.2: admitted only where
// `T: IComparable<T>`, with the condition decided after substitution and the
// rejection reported at the call. §14.2's register carries the row; the other
// members of that register -- `OwningBuffer<T>.DangerousPointer()` (§4.14),
// `Result<T, E>`'s combinators (§2.20), `ReadOnlySpan<T>.ToArray()` (§4.14),
// and §5's hashable-element operators -- take the identical shape.
```

### Edit 6 — stdlib §4.5 declares `OwningArray<T>.Sort()` ▲ (D3, branch A)

**OLD** (`Bezel_Standard_Library.md:1681`):
```
  public method Sort(comparer: (T, T) -> int);          // comparer takes borrows
  public method Reverse();
```

**NEW:** ▲
```
  public method Sort(comparer: (T, T) -> int);          // comparer takes borrows
  public method Sort();                                 // natural order; conditionally
                                                        //   admitted where `T: IComparable<T>`
                                                        //   (language spec §14.2)
  public method Reverse();
```

⚠ **This edit is already present in the working tree at the time of writing**, in a slightly different comment form and citing #3325 rather than §14.2 (this proposal's §4.1). If the parallel work lands first, Edit 6 becomes a **comment-only** reconciliation onto §14.2's vocabulary; if it does not, Edit 6 lands as written. Under D3 branch B, Edit 6 is dropped and §4.4's comment block states the absence with the rejecting site named.

### Edit 7 — stdlib §5 writes the `where` clauses that are already expressible ▲ (C6 and D9)

`Bezel_Standard_Library.md:3560` and `:3564–3565`. These are **not** conditional members (this proposal's §2.1, C6): `TKey` is the member's own type parameter, so the ordinary §14.2 clause applies, and `Counter<K>`, `Sum<U>`, `Min<U>` and `OrderBy<TKey>` already spell theirs in the same listing.

**OLD:**
```
  public method GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>>;
```
**NEW:** ▲
```
  public method GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>> where TKey: hashable;
```

with the matching clause on both `ToDictionary` overloads (`:3564–3565`).

▲ **And, per D9,** `Distinct` (`:3544`), `Union` (`:3545`), `Intersect` (`:3546`), `Except` (`:3547`), `Contains` (`:3504`) and `ToSet` (`:3566`) move out of the single `public extension IEnumerable<T>` block (`:3489`) into a second one:

```bezel
public extension IEnumerable<T> where T: hashable
  public method Contains(value: T): bool;
  public method Distinct(): IEnumerable<T>;
  public method Union(other: IEnumerable<T>): IEnumerable<T>;
  public method Intersect(other: IEnumerable<T>): IEnumerable<T>;
  public method Except(other: IEnumerable<T>): IEnumerable<T>;
  public method ToSet(): owned Set<T>;
end;
```

with §5's prose at `:3588` losing the six names from its list and gaining one sentence: ▲ *"the six hashable-requiring operators are declared in the constrained block above; `GroupBy`/`ToDictionary` state their requirement on `TKey`."* §26.6 merges the two blocks, so no call site changes and no member moves module.

*Neither half of Edit 7 is a design change: the prose at `:3588` already states both requirements, §14.2 admits `hashable` as a constraint (v0.214), §26.4 defines the block clause, and §26.6 admits the second block. The whole edit writes down a requirement the document already asserts, in a mechanism the language already has. What D9 decides is that this is preferable to registering it as a privilege.*

### Edit 8 — the three remaining stdlib citations ▲

| Site | Edit |
|---|---|
| §4.14, `:2535` (`DangerousPointer` prose) | ▲ after *"admitted only where `T` is in the **FFI-pointee set**"*, insert `— a conditionally-admitted member in the sense of language spec §14.2 (Conditionally-admitted members), whose register carries this row —` |
| §2.20, `:1186` (combinator prose) | ▲ *"compiler-provided per-instantiation members (spec §35.2)"* → `conditionally-admitted members (spec §14.2, Conditionally-admitted members; the choice-specific clauses at spec §35.2)` |
| §4.14, `:2795` (`ReadOnlySpan<T>.ToArray()` prose) | ▲ after *"rejected at the call site per the per-instantiation rule of language spec §3.6 rule 2"*, insert `— the conditionally-admitted-member shape of language spec §14.2 (Conditionally-admitted members)` |

*§5's `:3588` is **not** in this table: under D9 it is rewritten by Edit 7 instead, and under Q5's alternative (keep C5 registered) it takes the same insertion as the three rows above.*

### Edit 9 — rationale §5.15 records the decision ▲

One paragraph in the buffer-and-span design home, stating: that the practice was found in five places and defined in one; that the register rather than a user-writable `where` was chosen, with option A's reopening bar; and that §26.4's constrained extension is the user-facing form. *(Rationale §5.15 is chosen over §5.16 because three of the five rows are in the buffer/span family and §5.15 is already the recorded home of the compiler-known-specialization argument that §3.6 cites.)*

### Not edited, verified

- **Appendix A** — no production changes. D7 rejects an already-grammatical form semantically, as A.4 already does for `owned` on a non-class return and for `Variance` on a class parameter.
- **The guides** — Guide 02 §143–170 *teaches* C3's combinators but states no admissibility rule and cites no section this proposal moves; no pin moves.
- **`Bezel_For_Code_Generation.md`** — its two affected lines (`:821` `Sort`/`Sorted`, `:973` `DangerousPointer`) already carry the conditions in the right words (*"only where T: IComparable<T>"*, *"value T only"*). Under D3 branch A it gains the `OwningArray<T>.Sort()` line (§9 obligation, since the stdlib surface changes); otherwise it is untouched and its pin moves with the revision.
- **The conformance corpus** — `stdlib_0068` (`DangerousPointer`) and the §4.4/§4.5 rows are `skip:signature`/`skip` rows that re-extract from the edited blocks; **no case is deleted or converted**, and no `skip` classification changes. Process §6's deletion-shaped-proposal obligation is engaged only by Edit 6's branch B, which deletes nothing either.

---

## 7. Blast radius and migration

### 7.1 Programs

**Newly rejected: one form, with zero instances.** D7's rejection of a member-level `where` naming an enclosing type parameter. Measured: **zero** occurrences in `docs/Bezel_Standard_Library.md` (every member-level clause there constrains the member's own parameter — `Min<T>`, `ToText<T>`, `Parse<T>`, `Sum<U>`, `Min<U>`, `OrderBy<TKey>`, `ThenBy<TKey>`, `FromOwnedResult<T>`), **zero** in `tests/conformance/cases/`, and the specification uses the form nowhere.

**Newly accepted: nothing.** The register documents admissibility the compiler already applies (C1, C3) or already fails to apply (C2, C4, C5) — it changes no accept/reject verdict on its own authority. Edit 6 under branch A documents a call `bezelc` already compiles; under branch B it is dropped.

**Newly defined:** the conditional-member mechanism itself; the diagnostic contract (D2); the register's four rows; the boundary between §3.6's reservation and this one (D5).

**Doc-only:** Edits 3, 5, 8, 9.

### 7.2 The compiler — no work for the rule, three filed deltas

The rule imposes **no** new compiler work: rules 2–4 describe what `Result`'s gate and `Array`'s gate already do. Three deltas exist independently and are filed rather than landed (this proposal's §10):

| Delta | Row | Status |
|---|---|---|
| C1's diagnostic does not name the substituted type argument, cites the retired **§3** rather than stdlib §4.4, and states an accept-list rather than a structural reason (`src/sema/ExprTypecheck_Calls.cpp:5625–5635`) | C1 | issue B |
| the `IComparable<T>` gate is `Array`-only, so `OwningArray<T>.Sort()` over a class element without `CompareTo` is admitted and mis-lowered | C1 | **already filed: [#3325](https://github.com/s8lap/bezel-lang/issues/3325)** |
| C2 unimplemented; C4's and C5's conditions have no gate this proposal could find by reading | C2, C4, C5 | issue D |

D7's rejection is new compiler work of the smallest kind: one check at the declaration, where the enclosing parameters are already in scope (`src/sema/TypeResolution_Checks.cpp:851–864`).

### 7.3 Tests

No existing test is invalidated. The ten files exercising C1's parameterless forms assert behavior the register documents unchanged. New tests belong to the filed deltas, not to this proposal, with one exception: ▲ D7's rejection ships one lit/FileCheck case pinning the diagnostic, per `tests/README.md`'s spec-cited-regression discipline.

### 7.4 The change class, and the artifacts it owes

**Design change** under process §2 — it adds a named mechanism to the language and a new rejection. It therefore takes §6's proposal path (this document) before any landing, and the landing is ruling-class: mechanical gate, adversarial critique, composition gate if it rides a train, the §5 artifacts. Its §9 checklist obligations: **kernel — no contact**; **body-locality — carve-out** (generic instantiation, D1); **`DECLINED.md`** — four rows (this proposal's §9); **code-generation reference** — one line under D3 branch A, since the stdlib surface changes; **rationale §18 deferred list** — checked, carries no entry this ships.

---

## 8. Interactions

### 8.1 [#3325](https://github.com/s8lap/bezel-lang/issues/3325) — the missing gate on the owning side

Sequencing is one-directional and stated so a later reader can rely on it: ▲ **#3325 is independent of every decision here.** Its fix (widen `colName == "Array"` to include `OwningArray`) is correct whether or not the register lands, and the register's C1 row is correct whether or not the fix lands — the row states the condition, and process §5 item 3 carries the gap. Landing the register **first** improves #3325's fix, because the row gives its regression test a section to cite; landing #3325 first changes nothing here.

### 8.2 [#3314](https://github.com/s8lap/bezel-lang/issues/3314) — §3.6's category-dispatch reservation

Open, and touched by D5. This proposal **narrows** #3314's question rather than deciding it: after Edit 3, §3.6's reservation is explicitly about *behavior* dispatch on a derived property, and member *admissibility* has its own home. Whether §15.4.1's user-authored factory body is inside or outside the behavior reservation — #3314's actual subject — is untouched, because a factory body is behavior, not admissibility. Proposal 0115 §6.1 drew the same line from the other side and should be read with this.

### 8.3 Conditional interface **conformance** — explicitly out of scope

stdlib §4.14 records the gap in this proposal's own vocabulary (`Bezel_Standard_Library.md:2537`): for value-typed `T`, `OwningBuffer<T>` *"does not implement `IEnumerable<T>` (the type-parameter-category dispatch would require **conditional interface implementation, which Bezel does not currently provide as a general mechanism**)."* ▲ **Nothing in D1 admits it.** Rule 1's register is a register of *members*, and a conformance is not a member: it changes assignability, variance (§12.9), constraint satisfaction (§14.2's *Interface conformance*), and the artifact interface's base-type list — none of which member admissibility touches. A future proposal wanting conditional conformance inherits this proposal's register as a precedent and **not** as an authorization; it must argue those four interactions on their own.

### 8.4 The rest — audited

- **§26.4 / §26.5.** No edit. The constrained extension is the user-facing mechanism and this proposal *cites* it (Edit 1's closing paragraph) rather than changing it. §26.5 rule 1 is untouched: B1 stays a barrier, deliberately — relaxing it would let an extension shadow a declared name, which rule 1 exists to prevent.
- **§22.6 (overloading).** No edit. Rule 4 states that admitted conditional members resolve ordinarily; the question option A would have raised (do inadmissible overloads rank?) does not arise, because on a compiler-known type the compiler decides admissibility before it ranks.
- **§2.8.2 / §2.8.6 (artifact interface, breaking-change classification).** No edit. Rule 5 routes a condition change to §3.6 rule 3's existing verdict — breaking — so no new row in §2.8.6's table is needed. *Recorded as liftable:* whether a *widening* of a condition is `preserving` by the same table's derived-property row is not decided here; no proposal needs it yet.
- **§11.8 (static interface members).** No edit; cited as the compiler-provided-conformance precedent exactly as §35.2 already cites it.
- **§28 / §30.5 (kernel).** No contact.
- **`hashable` / `sendable` derived-property constraints (§14.2).** No edit. The new subsection sits beside them and borrows the growth clause; it does not add to the admitted *constraint* set.

---

## 9. Reopening bars, collected

⚠ **Draft 2 changes R3 and suspends R1.** R3 routed `DangerousPointer()`'s removal to the v0.283 subtraction review and declined removing it *as a side effect of defining a mechanism*; an intermediate wording of Draft 2 recommended removing it outright; that was reversed on the BLAS/argv evidence (§5.3.1, C2), so **R3 stands as written** and is the row `DECLINED.md` indexes. R1 is **suspended, not withdrawn**: its bar asks for evidence about the predicate vocabulary rather than the name, and [#3336](https://github.com/s8lap/bezel-lang/issues/3336) shows the mechanism the bar presumes does not currently function, so the bar can be neither met nor missed until #3336 lands. §5.5's prior art suggests it is aimed at the wrong thing and should be re-drawn at graduation. **R2 and R4 are unchanged.**

**Draft 2 adds one bar of its own:**

| # | Declined | Reopening bar |
|---|---|---|
| **R5** | **Registering a conditional member that the compiler does not implement** (Draft 1 would have registered C2 and C4) | A register row must name a member the compiler admits, or say on its face that it does not — D8's discipline, applied as a precondition rather than an annotation. Draft 1 would have written two members the compiler rejects into a normative table and given them a growth discipline; that is the defect class v0.275 corrected in the opposite direction, and §5 item 3b of the change process now forbids the asserting half of it. Reopened only by a decision to register aspirational surface deliberately, which would need its own reasons. |

**Draft 1's bars, R1–R4:**

| # | Declined | Reopening bar |
|---|---|---|
| **R1** | **Option A** — a user-writable per-member `where` on an enclosing type parameter (this proposal's §5.1) | Real Bezel code in which §26.4's constrained extension is reached for and **fails for a reason other than the §26.5-rule-1 name collision** — that is, evidence that the *predicate vocabulary* (B2), not the *name*, is what blocks users. A wish for `Sort` over `SortNatural` is not that evidence. |
| **R2** | **Option C** — removing `Array<T>.Sort()`/`Sorted()` (this proposal's §5.3) | New evidence against v0.275's stated reasons — i.e. evidence that the capability does **not** ship, or that its five hardening issues did not harden a real defect. Re-asking is closed; v0.275 answered it hours before this proposal (`17db9bee`, 2026-08-21). |
| **R3** | **Option C** — removing `OwningBuffer<T>.DangerousPointer()` | **Deliberately low, and routed rather than barred.** Its removal is a legitimate candidate for the **v0.283 subtraction review** (process §10), which is the right forum: eighty-nine revisions carried, zero compiler implementation, zero call sites. The register row is the evidence that review needs. What is declined here is removing it *as a side effect of defining a mechanism*. |
| **R4** | **Conditional interface conformance** (this proposal's §8.3) | A proposal that argues the four interactions §8.3 names — assignability, variance, constraint satisfaction, artifact interface — on their own evidence. This proposal's register is precedent for members only and authorizes nothing about conformance. |

---

## 10. Questions for the owner, and what the coordinator must do

### Questions

⚠ **Draft 2 supersedes Q1, Q2 and Q4.** **Q1** — D1's option — is now answered by §5.3's costing rather than put to the owner as an open choice: retire two, register three, no row a standing grant. **Q2** (D3's branch) was landed at v0.276. **Q4** (should `DangerousPointer()` be a subtraction-review candidate) is **answered yes** by §5.3.1 — an intermediate wording of Draft 2 recommended deleting it outright and was reversed on the evidence; Draft 1's R3 stands. **Q3 and Q5 stand.**

**Draft 2's questions:**

- **Q6 — the C3 ruling.** Take proposal 0075 §3.3 Option B now, as its own ruling (one §14.2 constraint + the §26.2 amendment), or defer until the constraint is wanted on its own merits? Draft 2 recommends taking it: §26.2's exclusion names the missing constraint as its reason, and the *"one program's evidence"* ground of its v0.233 decline no longer describes the case.
- **Q7 — C2's disposition.** ⚠ **Re-answered.** Draft 2 first recommended deleting it now; §5.3.1 reverses that. The member is the fix for the BLAS double-copy exemplar and for `execve` argv marshalling, both **runtime-sized** and therefore both inside the residual that has no alternative spelling. The recommendation is Draft 1's R3: **route it to the v0.283 subtraction review**, with 0033 §3.5's worked example as the evidence that review needs on the keep side, and ninety revisions of zero implementation as the evidence on the drop side. The owner may still prefer deletion now; what changed is that deletion is no longer this proposal's recommendation.
- **Q8 — C1's expiry.** Should the register row's re-examination be bound to proposal 0005 landing, as §0 proposes, or left as an open review item with no trigger?

**Draft 1's questions:**

- **Q1 — D1's option.** Register (B) recommended. The proposal argues (A) and (C) at strength in §5 and declines both; the owner may prefer (C) for `DangerousPointer` alone, which R3 routes to the v0.283 subtraction review rather than deciding here.
- **Q2 — D3's branch.** Branch A (declare `OwningArray<T>.Sort()`) is recommended and appears to be what the parallel investigation found, but the working-tree evidence is uncommitted and this proposal does not assume it. If a build contradicts #3325's reading, Edit 6 is dropped and §4.4's comment states the absence with the rejecting site named.
- **Q3 — D7's ramp.** Error immediately, or warn-first? The proposal recommends **error immediately**: the affected set is measurably empty (§7.1), so a ramp would warn nobody. Process §2's ratchet class does not apply either way — this tightening is not previously scheduled, so it rides the design change.
- **Q4 — the v0.283 subtraction review.** Should `DangerousPointer()` be a named candidate on its agenda? The proposal recommends yes, with the register row as evidence.
- **Q5 — D9.** Spell the LINQ hashable set as a constrained extension block (recommended: four privileged rows), or keep it registered as a fifth (better diagnostic today, one more privilege)? The register works either way; nothing else in the proposal depends on the answer.

### What the coordinator must do

- **`docs/proposals/README.md`** — add the row for **0116**, `Status: draft`, target modules as the header table states. Per process §8, the README's Status column is the owner-review queue, so this row *is* the queue entry.
> **Coordinator note (added at landing, v0.276).** The `DECLINED.md` instruction immediately below is **not followed at this landing, and should not be.** That file carries a scope rule ruled 2026-08-20 — *"A recommendation is not a verdict. An under-review draft's proposed declines are **not** indexed, and the `recommended — awaiting owner ruling` class stays empty"* — which governs, being the register's own ruled scope. The four rows are owed **at graduation**, not now; process §9's checklist line and this proposal's instruction both read as if a draft's recommendations were indexable, and the register's scope rule is the narrower, later, and authoritative statement. The `DECLINED.md` index-size line records the deliberate zero so a later reader does not read it as an omission.

> **v0.276 also landed D3's branch A.** Stdlib §4.5 now declares `OwningArray<T>.Sort()` and §4.4's comment block is rewritten, in the same commit that lands this file — so §4.1's *"uncommitted working-tree edit … not at `17db9bee`"* status note, §6's *"if the parallel work lands first"* conditional, the §4 blockquote of the retired *"NOT DOCUMENTED HERE, AND DELIBERATELY NOT DENIED"* comment, and **Q2** are all historical as written. They are left in place rather than rewritten, because a proposal's record of what it could and could not verify at drafting time is worth more than a tidied one — but a reader should not take them as current. The remaining D3 question is the §14.2 **register row**, not the declaration; the declaration is settled. The gate defect that rides with it is [#3325](https://github.com/s8lap/bezel-lang/issues/3325), still open.

- **`docs/proposals/DECLINED.md`** — per process §9's checklist line, **four** decline-index rows in the same commit as any status change: R1 (`declined`, this proposal §5.1, bar as R1), R2 (`declined — settled at v0.275`, this proposal §5.3, bar as R2), R3 (`routed`, to the v0.283 subtraction review, bar as R3), R4 (`out of scope`, this proposal §8.3, bar as R4). Each row names its verdict class, its authoritative record (this file + section), and its reopening bar.
- **Issues to file** (none exist yet; #3325 covers only the third row of §7.2):
  - **A — the proposal's own tracking issue.** "Conditional members: the stdlib has five, the language defines one, in the Choices chapter." Links this file, #3324's closing comment, #3325.
  - **B — the C1 diagnostic.** `src/sema/ExprTypecheck_Calls.cpp:5625–5635` prints unsubstituted `T`, cites the retired stdlib **§3**, and states an accept-list (*"is not a built-in comparable type"*) rather than the structural reason. Fix to D2's four-part contract; one lit case.
  - **C — is a member-level `where` on an enclosing parameter currently inert?** Source reading says it parses and passes the only declaration-time check found (`src/sema/TypeResolution_Checks.cpp:851–864`); whether anything downstream enforces or diagnoses it **could not be settled without a build**. Probe: `class Box<T> … public method Only() where T: IComparable<T> … end` instantiated at a non-comparable `T`. This is D7's precondition.
  - **D — the unenforced conditions.** C2 (`DangerousPointer`, no implementation at all), C4 (`ReadOnlySpan<T>.ToArray()`, no borrow-bearing gate found), C5 (LINQ hashable set, no operator-site gate found). One issue, three probes; each a specification-ahead-of-implementation delta under process §5 item 3.
  - **E — tooling.** Can `check_docs.sh`'s v0.275 **SETS** check be parameterized to the §14.2 register, so a stdlib passage saying *"admitted only where"* with no matching row fails mechanically? The register's whole weakness (§5.2) is that it is a record and not an enforcement.
- **Comment on #3324** noting that its closing comment's carried-forward question is now this file, and on **#3325** noting that its member is the register's C1 row.

---

## 11. Revision record

### Draft 2 (2026-08-21), against v0.276 at `b26ad189` — **the recommendation reverses**

Draft 1 recommended registering four conditional members as a standing compiler privilege. Draft 2 recommends retiring four and registering **one**, with an expiry. The reversal came from costing option (c) properly — three read-only lanes, one per row family — after the owner said they were *"leaning into not allowing conditional members."* Draft 1 had declined option (c) after arguing only its weak form (*delete the members*) rather than its strong one (*remove the conditionality*).

**Five findings changed the recommendation. Four contradict something Draft 1 asserted.**

1. **C4 does not merely lack a gate — the member does not exist and the call is rejected.** Draft 1's *"no gate found by reading"* reads as *the member works and is ungated*; the opposite is true (`src/sema/ExprTypecheck_Calls.cpp:7903–8188`). Zero call sites anywhere. This turns C4 from the hardest row to price into the easiest: deleting it costs nothing and stops the documents asserting a call that does not compile.
2. **§26.2 says why the privilege exists, and Draft 1 never cites it.** Extensions are closed against the choice kind *because* no §14.2 constraint names the classification — added at the same v0.233 landing that created C3, citing C3 as its reason. Draft 1 cites neither §26.2 nor proposal **0075**, whose §3.3 Option B already specified the way out. Verified by grep: zero hits for either. For a proposal about why the privilege is necessary, that is a research defect, and it is the single largest one in Draft 1.
3. **The escape hatch Draft 1 leans on does not function.** §26.4's block `where` clause is not enforced by the compiler ([#3336](https://github.com/s8lap/bezel-lang/issues/3336)) — no extension candidate is ever discarded for failing it, and §5.1's shipped constrained extension needs a hand-written gate to enforce its own clause. Draft 1's refuted-premise 3 (*users are not helpless*) is true of the language and false of the implementation, and R1's bar presumes a mechanism that does not work.
4. **A rename cannot rescue C1**, which Draft 2 tested in both directions before accepting Draft 1's conclusion by a different route. There is no Bezel-source standard library; `Array<T>` is a declaration-less builtin and the sort stays a compiler intrinsic under any spelling. Draft 2 also withdraws a naming argument it started from: this standard library has no `MinBy`/`MaxBy`/`DistinctBy`, and its `-By` names all take a **key selector**, never a comparer, so `SortBy(comparer)` would give the suffix two meanings.
5. **Two Draft 1 claims about C2 are overstated**: the citation surface is five spec sites plus an uncited stdlib site, not four; and *"no other spelling"* holds only for **runtime-sized** pointer storage — §32.5's `[N]IntPtr` at a by-address `extern` parameter covers the static case normatively today.

**What Draft 2 did not disturb.** D2, D5, D6, D7, D8, D9 stand as written. Option (a) stays declined on Draft 1's reasons. C1's removal stays declined on v0.275's reasons, which the costing *strengthens*. §35.2 is still the complete rule and the move is still a lift.

**One lane disagreement, resolved here.** The `c24-spans` lane held that C3's route still needs a member-level `where` naming the enclosing `T`, so option (a) would have to land too. That is wrong for the extension spelling: in `extension Result<T, E> where T: <neutral>` the clause names the **extension's** parameters (an ordinary §26.4 block clause), and `Map<U>(…) where U: <neutral>` names the **method's own** parameter (an ordinary §14.2 clause). Neither is a member-level clause on an enclosing parameter, so **D7 stands and C3 can still leave the census**. The `c3-result` lane's reading is adopted.

**Conformance-corpus accounting** (process §6's deletion-shaped-proposal rule). C2: `tests/conformance/manifest.tsv:492` (`stdlib_0068`, `skip:signature`) — delete the row, no case to convert. C4: manifest `:496–497`, both listings `skip:signature-listing` — delete both rows. **No exercised case is affected, because neither member ever compiled.**

**One correction made after Draft 2 was first written**, recorded rather than folded in silently. Draft 2 initially recommended **deleting** C2 outright, on the census evidence that it ships nothing. Asked *why it was needed in the first place*, proposal 0033 answers: it is the fix for the BLAS double-copy exemplar and for `execve` argv marshalling, both runtime-sized, and therefore both squarely inside the residual §2.1 identifies as having no alternative spelling. The recommendation is now Draft 1's R3 — route it to the v0.283 subtraction review. **The lesson generalizes past this row:** the census measured what each member *costs* and never asked what any of them was *for*, and for four of the five rows that was harmless because the answer is in the same document. For C2 the answer was in a graduated proposal the census never opened — the same class of research gap Draft 2 charges Draft 1 with at finding 2.

**Still not verified** (no build available in any lane): #3336's probe, C2's and C4's rejection behaviour, and whether §5.1's hand-written gate becomes deletable once #3336 lands. Seven build-gated probes are named across the three lane memos.

### Draft 1 (2026-08-21), against v0.275 at `17db9bee`

Authored from #3324's closing comment. Four findings changed the shape of the document as it was written, and are recorded because each contradicts a premise the drafting started from:

1. **The framing said "two members"; there are five** (plus one near-miss, C6, which is an ordinary missing `where` clause). The three additions — `Result<T, E>`'s combinators, `ReadOnlySpan<T>.ToArray()`, and §5's hashable-element operators — were found by the `§3.6 rule 2` and `rejected at the call site` search terms rather than by `admitted only where`, which is why prose-keyed searching alone would have reproduced the framing's count.
2. **The framing said "no conditional-member reading exists anywhere", quoting v0.275. §35.2 is one**, complete, since v0.233. The proposal's central move — lift rather than invent — follows from that and from nothing else.
3. **The framing said users cannot replicate the capability.** §26.4's constrained extension is a defined, specificity-ranked, user-writable conditional member, and §26.4's own worked example is a `Sort()`. The claim survives only in the narrow form B1/B2 state (§1.7), and stating it narrowly is what makes option A's cost visible.
4. **`DangerousPointer()` is unimplemented** — no sema arm, no codegen arm, a `skip:signature` conformance row. This strengthened option (c) enough that §5.3 argues it at length, and it is the reason R3 routes the member to the subtraction review instead of barring its removal.
5. **One of the five needs no privilege at all.** §26.6 admits more than one extension block per type, so §5's six hashable-requiring operators can simply move to a constrained second block. This was found last, while checking option A's cost table, and it shrank the proposal's own recommendation from five privileged rows to four (D9). A proposal about defining a privilege that ends by removing one of its instances is the outcome the adversarial pass was for.

**Mid-draft, the working tree changed under the document.** `docs/Bezel_Standard_Library.md` gained an uncommitted `OwningArray<T>.Sort()` declaration and an [#3325](https://github.com/s8lap/bezel-lang/issues/3325) reference while §4 was being written — the parallel investigation D3 was told not to presume. Every one of #3325's four cited compiler sites was re-verified here by reading before being relied on, and §4.1 marks the state as uncommitted so a later reader does not mistake it for v0.275.

**What could not be verified.** This author could not build or run `bezelc`. Every claim requiring one is marked and filed: whether a member-level `where` on an enclosing parameter is currently inert (issue C), whether C4's and C5's conditions are enforced anywhere (issue D), and whether `OwningArray<T>.Sort()` is admitted (D3's branch selection, #3325's probe). None of the proposal's decisions turns on any of them: D1 and D2 are decisions about the documents, D3 is stated in both branches, and D7's affected set was measured from the documents and the corpus rather than from the compiler.
