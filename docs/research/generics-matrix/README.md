# Generics matrix: a completeness audit of the spec's generic rules (2026-10-09)

*Research record. Non-normative. The spec decides every rule; this audit only checks whether it does. The run files here are the single record of the audit, and the page is a generated view of them. The method is the [borrow-flow matrix](../borrow-flow-matrix/README.md)'s.*

This matrix asks whether a generic body, checked once and never re-checked, stays sound at every kind of type argument, or a rule rejects the substitution at a stated site; and whether every derived property (hashable, sendable, holds-borrows, trivial-dispose, `owned T`, nullability) is computed consistently at the declaration, the instantiation and dispatch. A cell counts as *decided* only when current normative text settles it **and** the auditor could not write a program that escapes. Otherwise the cell is *missing*, *conflict* or *ambiguous*, comes with a counterexample, and is mapped to a root-cause issue.

## Runs

| Run | Spec | Base cells open | Route cells open | Outcome |
|---|---|---|---|---|
| 1 | v0.333 | 61 of 140 | 13 of 13 | 29 root causes, each filed as an issue (listed in the run file's `groups`); all decided in v0.334 (proposals 0159–0169) |
| 2 | v0.334 | 18 of 140 | 5 of 20 | 8 issues: `is` narrowing (#5835), generic function-value routes (#5836), second-kind ambiguity (#5837), `where T: U` assignability (#5838), assembly-time cycle edges (#5839), and existing #5792, #5798, #5800 |

## Files

- `matrix.json`: the page's title, rows and columns.
- `runs/v0.NNN.json`: one file per run: `cells` (the base cells, in the cell contract below), `routes` (route cells: a specific escape inside a cell), `groups` (the root-cause issues, each with the cells it opens).
- The page: `python3 docs/research/matrix-page/build_page.py docs/research/generics-matrix scratch/generics-matrix.html`.

## How a run is done

Five lanes, each owning a set of columns across every row, fill the cells; each lane quotes every deciding sentence verbatim (checked mechanically against the spec and stdlib), hunts for an escaping program per cell, and writes a counterexample of at most 25 lines for every open cell. A consolidation pass merges the lanes' findings, across matrices, into root causes, and each root cause not already covered by an open issue is filed. A re-run starts from the latest run file, re-derives every cell at the new revision, and records `prev_status`.

## 1. The question every cell answers

> **A generic body is checked once.** The check runs under the weakest assumption its constraints establish: §3.6 rule 2,
> §28.1 clause 3's class-or-interface assumption, and §7.10's possibly-null `T`. The body is never re-checked. Take the
> row's substitution kind S and the column's generic construct, and ask: **does the body's single check hold at S, or does
> a rule reject S at a stated site?** The possible sites are the instantiation site (§14.1), the use or call site, the
> declaration under a constraint, or the dispatched body (§14.2). Every **derived property** the construct reads
> (hashable, sendable, holds-borrows / borrow-bearing, choice classification, defaultable, trivial-dispose, element
> category, `owned T` meaning, nullability) must also be computed by **one** definition at the declaration, at the
> instantiation, and at dispatch or chain (§3.6 rules 1–2).

A cell is **decided** only when all three hold:

1. Text fixes *where* S is checked for this construct and the *verdict* there.
2. The derived properties read along the way agree across the declaration, the instantiation and the dispatch or chain.
3. The auditor could not write a program in which the body's single check admits something that is unsound at S while
   no rule rejects S.

## 2. Rows: substitution kinds (10)

Each row lists sub-kinds. A cell's note must answer for every sub-kind whose verdict differs, as the R3/C5 pilot does.

| Row | Substitution kind S | Sub-kinds the note must separate | Spec sections |
|---|---|---|---|
| **R1** | **Class** (not runtime-managed) | a user class (trivial-dispose, or with a destructor or owned fields); a borrowing stdlib class (`Array<X>`); an owning one (`OwningArray<X>`); abstract/`closed` | §3.1, §28.1 cl. 3, §28.9 trivial-dispose, §14.4, §28.5, §14.2 `new()` |
| **R2** | **Interface or `Object`** | a user interface; an `IDisposable`-extending one; `Object` itself, where a bare-T position becomes set-erasing (§28.3) and borrow-erasing (§9.7) | §14.2 conformance 3, §28.3 *Set-erasing positions*, §9.7 borrow-erasing bullet, §12.9 |
| **R3** | **Borrow-free value type** | primitive; enum with / without a zero constant; record or value-like choice holding no borrow (auto-hashable or not, constructor-only or not); unit `()` | §3.6 table, §9.2, §9.9, §17.11, §35.4, §35.7 |
| **R4** | **Value that holds borrows**, the span family included | (a) borrow-bearing record (`record Pair(Item: Item)`, `CancellationToken`) or a choice that holds borrows; (b) span records `Span<T>`/`ReadOnlySpan<T>`/string spans; (c) `IReadOnlySpan<TElement>`, the one reference type *read as* holding borrows | §3.6 holds-borrows, §9.7 (*In a generic body*, interface paragraph), §28.3 bare-T returns cl. 3, §30.5, stdlib §4.14–§4.16 |
| **R5** | **Inline arrays and move-only value types** | `[N]X` (borrow-free or borrow-bearing); `[N owned]C`, `[N owned]C?`; owning-classified choice | §15.1, §15.1.1, §15.3, §7.10 copy demand, §8.6, §17.4 `as` target, §14.2 dispatched exclusion |
| **R6** | **Function type** | a capture-free value vs a capturing one (same type, different source set); an `owned`-slot or mint-returning function type | §27.1, §27.1.1, §28.3 *Set-erasing positions*, *Function-type substitutions … nested parameter slots*, §28.1 cl. 3 |
| **R7** | **Nullable `U?`** | nullable reference (`Widget?`, `IFoo?`, `string?`); nullable value (`int?`, `C?` for a choice, `R?`) | §7.10 (`?` idempotent), §14.2 notnull/`new()`/type bounds, §3.6 hashable (c), §17.11 defaultable |
| **R8** | **`Task` family** | `Task`, `Task<X>`, `OwningTask<X>` | §30.3, §30.5, §28.3 *`Task`-family substitutions are excluded…*, calls-mint rule, §30.14 |
| **R9** | **Another type parameter** (chained or nested generics) | the argument *is* a parameter in scope (`Bag<T>` in `Outer<T>`, `G<U>(…)` in `F<U>`); the argument *contains* one (`C<W?>`, `C<Array<W>>`); a recursive request (§14.6) | §14.1 *Instantiation and its site*, §14.2 *A type parameter as the type argument*, §3.6 rule 2 (chain), §14.6 |
| **R10** | **Control: `string`** (and `Type`): runtime-managed reference | — | §28.9, §14.4. Expected decided almost everywhere |

A type that fits two rows is filed under the more specific one. A borrow-bearing inline array goes in R5, and a nullable
choice in R7. Where the verdict would differ, the note cross-references the other row.

## 3. Columns: generic constructs (14)

| Col | Construct (body side, and what forms the instantiation) | Spec sections |
|---|---|---|
| **C1** | **Storage typed at T.** An instance field of a generic class, a record component, a choice payload, a `[N]T` slot; the derived properties of the *constructed* type (borrow-bearing, holds-borrows, choice classification, auto-hashable, defaultable, trivial-dispose, C-compatible) computed per instantiation | §3.6 rules 1–2, §9.7, §28.5 generic owned field, §35.11, §15.1, §7.9/§7.10 |
| **C2** | **Parameters and returns at T.** Plain, `owned`, `ref`, `out`, `out owned` parameters at `T`/`T?`; bare-type-parameter returns; `owned T` returns; set-erasing positions; function slots over T | §28.3 *Bare-type-parameter returns*, *Set-erasing positions*, *`Task`-family … excluded*, *nested parameter slots*; §28.1 cl. 3–5; §22.3; §9.7 return bullet |
| **C3** | **Locals, `default(T)`, `new T()`, definite assignment, forming `T?`** | §7.9, §7.10, §17.11, §14.2 `new()`, §15.1 *No nullable inline-array types*, §9.9 |
| **C4** | **Constraints.** Satisfaction and in-body guarantee of `class`, `value`, `notnull`, type bounds, `where T: U`, `new()`, `hashable` and `sendable`; a parameter, or a type formed over one, as the argument; constraints on overrides; contradictory combinations | §14.2 (all of it to *Interface conformance*), §14.5 *Constraints*, §26.4 head check |
| **C5** | **Equality and hashing through T.** `=`/`<>`/`Equals`/`GetHashCode` on T operands (the three kinds); the hashable property vs `where T: hashable`; generated equality of a generic record or choice formed over T | §9.2 *Through a type parameter*, §3.6 hashable (a)–(d), §8.3, §8.4, §25.2, §35.7 |
| **C6** | **Type tests at T.** `is`/`is not`/`as` with T as carrier or target; typed pattern `var t: T`; narrowing; `typeof(T)` | §12.7 *Value-typed operands*, §17.4 *Degenerate value-type targets*, §19.3, §28.3 set-erasing `as`, §9.7 borrow-erasing `as` |
| **C7** | **Conversions over T.** T to `Object` or an interface (boxing), variance at a type argument formed over T, function-type assignability at slots over T, method-reference bridging, `T` to `U` under `where T: U` | §8.6, §12.9 *In a generic body*, §27.1.1, §27.6, §9.7 borrow-erasing bullet, §28.3 set-erasing |
| **C8** | **`owned T` and disposal of T.** Meaning of the marker per instantiation (transfer / redundant / erased); `using var` and `move` over T; drop-on-overwrite; the owning-generic element restriction; generic owned fields; destructors of generic types; trivial-dispose per instantiation | §28.1, §28.5, §28.7, §28.8, §28.10, §14.4, §28.9 |
| **C9** | **Type-argument inference.** The three phases, nullability in a binding, unique instantiation, agreement, constraints checked after inference, lambda and method-reference arguments, extension receiver unification; interaction with §22.6's overloading and declaration guard | §14.5, §22.6, §26.5 step 2, §27.4, §27.6 |
| **C10** | **Generic extension blocks.** Head binding; the head checked against the extended type's constraints; the block's `where` as an applicability filter; **body uses of block-level parameters**; resolution and specificity among generic heads | §26.4, §26.5, §26.5.1, §14.2 *Conditionally-admitted members* (the user-writable form) |
| **C11** | **Static members of generic types.** Per-instantiation static cells; statics at a bare T (mutable: rejected; `static readonly`: classified per instantiation, §22.12 (i)); static constructors and initialization order per instantiation; `T.Member` through a static-interface constraint; statics named through a derived generic type | §22.12, §22.13, §11.8 |
| **C12** | **Collections of T and stdlib generic members.** Element category at T; the conditionally-admitted register (sorting, equality search, the `*OrNull` tier); value-copy members; `hashable` keys and elements (`Set<T>`, `Dictionary<K, V>`); two-presentation types (`OwningBuffer<T>`, spans); iteration over T | §28.11, §14.2 register, §9.7 collection bullet, §3.6 element category, stdlib §4, §5 |
| **C13** | **Async, iterator and cross-domain generic bodies.** T-typed locals and parameters across `await`/`yield`; `Task<T>`/`OwningTask<T>` formed at T; async iterators of T; `Domain.Spawn`, `Channel<T>`, `Locked<T>` with T; `where T: sendable`; `AsyncEventSource<T>` | §30.1, §30.5, §30.7, §29.3–§29.4, §30.14, §30.15, stdlib §6.5/§6.8 |
| **C14** | **Dispatched generic methods and override/implementation constraints.** Requirements decided at the body; the move-only/inline-array exclusion; inherited witnesses; constraint inheritance; bare-T return classification inherited; §14.6 edge attribution; requirement summaries across modules | §14.2 *Constraints on overrides…*, *Dispatched generic methods*; §2.8.6; §28.3 bare-T cl. 2; §14.6 |

**Route cells** work as in the template. A specific escape inside a cell becomes a route cell `{row, col, route, issue}`
next to the 140 base cells. One cross-cutting route is pre-registered:

- **·mref** (owner: L2): a **method-reference formation** over a generic member, or over an extension member of a
  generic block, at type arguments the reference binds. §14.1's list of instantiation sites names calls, not formations.
  §27.6 says how a formation reads a conditionally-admitted member. Neither §27.6 nor §14.5 phase 2 says how a
  referenced *generic method's own* type parameters are bound, or whether such a reference is formable at all. Every lane
  that finds a cell whose only open route is a formation records it here instead of opening the base cell.

## 4. Cell contract

One JSON object per cell. These are the borrow-flow fields plus `lane`:

```json
{"row":"R4","col":"C10","lane":"L4",
 "verdict":"admitted|admitted-bounded|rejected-at-declaration|rejected-at-instantiation|rejected-at-use|conditional|n/a",
 "rule":"\"verbatim quote ≤ 60 words\" | \"another\"","cite":"§x.y[, stdlib §a.b]",
 "status":"decided|missing|conflict|ambiguous|n/a",
 "note":"which check site decides S; the derived properties read and where each is computed; what was hunted",
 "counterexample":"for missing/conflict/ambiguous: ≤ 25 lines of Bezel showing the escape or the two readings; else empty",
 "spec":"v0.333 @ f6e9003"}
```

**Verdicts.**

| Verdict | Meaning |
|---|---|
| `admitted` | The body's single check holds at S, and nothing is read at S. |
| `admitted-bounded` | Admitted under a recorded trust residual or runtime backstop: K4.1's flag, proposal 0153, a field-store residual. Name the clause. |
| `rejected-at-declaration` | Rejected before substitution, whatever the instantiations are. |
| `rejected-at-instantiation` | Rejected at an instantiation site (§14.1), by a constraint, a §3.6 rule 2 requirement, or an instantiation-site rule. |
| `rejected-at-use` | Rejected at a call, member use or formation: a conditionally-admitted member, or a dispatched method's call. |
| `conditional` | The verdict differs by sub-kind or by constraint, and the note spells out each case. |
| `n/a` | See the status below. |

**Statuses** (adapted to this question):

- **decided**: one rule, or a consistent set, fixes the check site and the verdict for every sub-kind of S. Every derived
  property it reads is computed by one definition at the declaration, at the instantiation and at the dispatch or chain.
  The hunt found no program where the body's single check admits an unsound use at S that no rule rejects.
- **missing**: no text says whether S is admitted for this construct, or where it is checked. Two typical shapes: the body
  is not re-checked and no instantiation or use-site rule reaches S; or a route (chain, inference, extension call,
  method-reference formation, dispatch) forms the instantiation at a site no rule lists.
- **conflict**: two passages give different verdicts or different check sites for S. A derived property computed
  differently at the declaration and at the instantiation (or in a constraint and in the property's own definition) is a
  conflict too.
- **ambiguous**: one passage has two defensible readings with different verdicts or check sites, and one reading escapes.
- **n/a**: no instantiation of the construct at S exists. Either S cannot be written there, or a rule of the construct's
  own *kind* excludes it at every formation. A rule of the kind is not a constraint or a §3.6 requirement. The model is
  §2.8.6's "a rule of the method kind". Cite the rule and hunt every formation route; if one forms the instantiation
  anyway, the cell is **missing**. Expect n/a to be rare in this matrix, since nearly every kind can be a type argument
  everywhere. A rejection by a constraint or a requirement is **decided**, never n/a.

Quote verbatim, markdown included: links, `**`, backticks. Split quotes with ` | `, and elide inside a quote only with
`...`. Run `python3 -I check_quotes.py <your.json>` until it says OK. Re-locate every citation;
never cite from memory.
