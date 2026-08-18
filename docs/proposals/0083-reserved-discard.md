# Proposal 0083 — Reserve `_`: always a discard or wildcard, never a declarable or readable name

| | |
|---|---|
| **Status** | graduated-v0.244 — superseded — graduated v0.244 (owner-ruled 2026-08-11, all questions as recommended) |
| **Tracking issue** | [#3049](https://github.com/s8lap/bezel-lang/issues/3049); fresh context: the v0.241 `ref _` ruling ([#2679](https://github.com/s8lap/bezel-lang/issues/2679) part B) |
| **Target module(s)** | **spec** — §1.3 (the identifier rule), §17.13 (rewritten shorter; the recap subsection shrinks; the write-once corollary and the `var _ := 5; Console.WriteLine(_)` worked example delete), §21.1/§28.10 (`using var _` as the anonymous binding), §22.4 (`out var _` folds into `out _`), §27.3/§27.4 (lambda-parameter ignore), Appendix A (A.1 identifier note; `LocalDecl`, `ForStmt`, `LambdaParam` alternatives; A.4 `ArgExpression` comment), Appendix B.1 (`_` becomes a reserved token). **rationale** — §3.2 rewritten (its closing claim becomes true). **guides** — none (no guide teaches `var _`). |
| **Written against** | spec/stdlib v0.241 |
| **Origin** | The 2026-08-11 elegance review (front-half critic, finding 3, confidence high; coordinator Tier 2 item 6), owner-accepted. The window argument is the review's: C# cannot reserve `_` for compatibility reasons; Bezel has no users yet and can. |

*Reference convention: unprefixed §-references are language spec references; rationale references are marked.*

---

## 0. Thesis

Today `_` is two things. In three positions it is a discard (§17.13: pattern, `out`-argument, discard statement); in **every other position it is an ordinary identifier** (§1.3), declarable and readable. The overlap produces the language's strangest corner, each piece individually ruled but jointly indefensible:

- `_ := OpenLog();` and `var _ := OpenLog();` are different constructs with different disposal timing (end of statement vs. scope end with a must-use warning) — distinguished by lexical shape alone.
- A declared `var _` can be **read** (`var _ := 5; Console.WriteLine(_);` — §17.13's own worked example) but can never be **assigned again**: any later `_ := e;` is a new discard, not an assignment (§17.13's corollary). A readable, never-assignable binding is a corner no one designed; it fell out of overlapping rules.
- `ref _` needed a v0.241 owner ruling (#2679 part B) just to decide *which* thing it was: the parser treated it as a discard, the three-position rule said identifier, and the resolution ("after `ref`, `_` is an ordinary identifier — it must resolve to a binding named `_` in scope") is only expressible *because* such bindings can exist.
- Rationale §3.2 closes with "`_` is a sink, not a binding. It cannot be read." — a statement the spec's own worked example falsifies. The rationale believes the design this proposal writes down.
- §17.13 needs a dedicated "Recap of the difference" subsection to re-teach its own rule — the document apologizing for the split.

**Proposed:** the bare token `_` is **reserved** — never an identifier, never declarable, never readable. Every position either keeps its discard/wildcard meaning, becomes a sanctioned anonymous form, or becomes a compile error with a diagnostic naming the discard forms. Identifiers *containing* underscores, including the `_camelCase` private-field convention (§1.3), are untouched — only the bare single-character token is reserved.

## 1. Every position `_` can appear today, and its new behavior

Enumerated from Appendix A (every grammar site that names `"_"` or admits `Identifier`) and §1.3/§17.13:

| # | Position (grammar site) | Today | Proposed |
|---|---|---|---|
| 1 | Pattern (`Pattern ::= "_"`, A.7; slots of destructurings; named-destructure slots) | wildcard, no binding | **unchanged** |
| 2 | `when`-arm condition slot (`WhenArm ::= (Expression \| "_")`, A.7) | else-arm marker | **unchanged** |
| 3 | `out` argument (`ArgExpression ::= Expression \| "_"`, A.4; out-only per the comment) | discard | **unchanged** |
| 4 | Discard statement (`DiscardStmt ::= "_" ":=" Expression ";"`, A.6) | discard; disposal at end of statement per §17.13's type-keyed rules | **unchanged** |
| 5 | `var _ := e;` (`LocalDecl`, A.6) | a local *named* `_`: must-use warning, auto-drop, readable, never re-assignable | **compile error**, diagnostic naming both remedies: `_ := e;` (end-of-statement) or `using var _ := e;` (scope-end) |
| 6 | `using var _ := e;` / `await using var _ := e;` (`LocalDecl`) | a binding named `_` with using-var disposal — §17.13's blessed unnamed-disposal idiom | **kept, redefined as the anonymous binding**: owns and disposes at scope end (timing unchanged), binds no name — `_` is not in scope after it; the one `_` form that owns storage |
| 7 | `ref _` (argument position) | per the v0.241 ruling: an ordinary identifier that must resolve to a `_` binding in scope; unresolved → unknown-identifier error | **always a compile error** (no `_` binding can exist): "`_` is a discard; a `ref` argument reads its operand, which a discard cannot supply." The #2679 corner deletes rather than needing a rule |
| 8 | `out var _` (§22.4 inline out declaration) | declares a local named `_` | **compile error** pointing at `out _` (which already means exactly this) |
| 9 | Lambda parameter (`LambdaParam ::= FuncParamMods? Identifier …`, A.7) | a parameter *named* `_`: readable in the body; two `_` parameters collide as duplicate names | **a true ignore**: accepted, binds nothing, contributes its slot to the function type as normal; `lambda(_, _) => 0` becomes legal (no duplicate-name collision — nothing is named). Matches `LambdaSlot`, which already admits `_` |
| 10 | `for var _ in xs do … end` (`ForStmt`, A.6) | a loop variable named `_` | **an ignore**: iterate for effects, element unbound; borrow/ownership semantics of the iteration protocol (§29.2) unchanged — for owning iteration the unbound element is disposed per iteration exactly as a bound-but-unused one is |
| 11 | Method/constructor parameter named `_` (`Param`, A.4) | ordinary parameter | **compile error**: method parameter names are API surface — they are addressable by named arguments (§22.5) and carried by the artifact interface (§2.8.2); an unnameable parameter is a hole in that surface. Rename it. (Q2 offers the ignore alternative) |
| 12 | Field, property, method, type, module, enum-constant, choice-case, label, or alias named `_` (various, A.2–A.4) | legal identifier (§17.13 even mentions the private-field use) | **compile error**: `_` cannot be declared |
| 13 | `const _ := e;` (`LocalConstDecl`) | a runtime/compile-time const named `_` | **compile error** (a never-readable constant is useless by construction) |
| 14 | Expression position (a bare `_` read) | resolves to a `_` binding if one is in scope | **always a compile error** (nothing can bind `_`) |
| 15 | Identifiers merely containing `_` (`IdentStart`/`IdentContinue`, A.1) — `_camelCase`, `snake_case`, `_f` | ordinary identifiers | **unchanged** — only the bare token is reserved |

Rows 1–4 are today's three discard positions plus the `when` marker: no program changes. Rows 5, 7, 8, 11–14 are the newly-rejected declarations and reads. Rows 6, 9, 10 are the newly-sanctioned anonymous forms.

## 2. Grammar and reserved-word deltas

- **A.1 / §1.3:** the bare `_` is lexed as its own token (spell it `DISCARD` in the lexer); `Identifier` excludes it. §1.3's sentence "The bare identifier `_` is a valid identifier name everywhere it appears as a name" deletes; the replacement sentence points at §17.13.
- **B.1:** `_` joins the reserved tokens (a punctuation-like reservation, listed with a pointer to §17.13 — not a keyword in the word sense; B.3's "future reservations" discipline is the precedent for holding a token out of the identifier namespace).
- **A.6 `LocalDecl`:** the plain-`var` alternative keeps `Identifier`; the `using var`/`await using var` alternatives become `"using" "var" (Identifier | "_") …` (row 6). A comment records that `var _` is rejected with the two-remedy diagnostic.
- **A.6 `ForStmt`:** `"for" "var" (Identifier | "_") (":" Type)? "in" …` (both alternatives; a type ascription on `_` stays legal — it types the unbound element, mirroring `out _`'s type-from-parameter rule in reverse? No: mirroring nothing — the ascription is simply allowed as documentation of the element type, or **rejected** for symmetry with `out _`'s no-annotation rule; see Q4).
- **A.7 `LambdaParam`:** gains the alternative `| "_"` (no modifiers, no annotation on the bare ignore — a typed ignore slot is spelled by the target type, exactly the §27.4 inference story; `FuncParamMods` with `_` is rejected: `ref _`'s reasoning, and a `scoped` parameter that binds nothing stores nothing).
- **A.4 `ArgExpression`:** comment extended: after this change the `"_"` alternative *is* the only reading anywhere in argument position — the "ordinary identifier elsewhere" clause of the old comment retires.
- **`DiscardStmt`, `Pattern`, `WhenArm`, `LambdaSlot`:** unchanged productions; their `"_"` now denotes the reserved token.

## 3. Diagnostics (texts)

- Row 5: `` `_` is a discard, not a declarable name — write `_ := expr;` to evaluate and release the value at end of statement, or `using var _ := expr;` to hold it anonymously until scope end (§17.13) ``
- Row 7: `` `ref _` is invalid — a `ref` argument reads its operand, which a discard cannot supply; pass a named location (§17.13, §22.3) ``
- Row 8: `` `out var _` declares nothing — write `out _` (§17.13, §22.4) ``
- Rows 11–13: `` `_` cannot be declared — it is the discard token (§17.13); choose a name `` (for row 11 the message adds: parameter names are addressable by named arguments, §22.5).
- Row 14: `` `_` is not a value — nothing binds it (§17.13) ``

## 4. What gets simpler

- **§17.13 shrinks**: the write-once corollary, the `var _ := 5; Console.WriteLine(_);` worked example, the binding-vs-discard recap's third form, and the v0.241 `ref _` resolution paragraph all delete; the recap reduces to the two-form timing table (`_ :=` vs `using var _ :=`), which was always the useful content.
- **Rationale §3.2's closing line becomes true** as written: "`_` is a sink, not a binding. It cannot be read." The section gains this proposal's decision record and drops nothing else.
- **The #2679 class of question becomes unaskable.** The v0.241 ruling had to define which of two meanings `_` carried after `ref`; under reservation there is one meaning everywhere, and every future position (`scoped _`? a new modifier?) answers itself: a discard is valid exactly where receiving-and-ignoring is coherent, an error everywhere else.
- **Unused lambda parameters get their natural spelling** (row 9). §27 has no unused-parameter warning today, so the friction was never a warning to silence — it was that `lambda(_) => …` *looked* like an ignore while actually declaring a readable name, and that two ignores collided (`lambda(_, _)` is a duplicate-name error today). Both artifacts delete.

## 5. Corpus evidence

Counted at v0.241 — occurrence counts by substring grep, shapes as named per row, over `tests/` (C++ harness strings and lit `.bzl` files, recursively) and the **ten normative documents** in `docs/` (proposals excluded; the pinned changelog's contribution broken out per row where it appears):

| Shape | Count | Disposition |
|---|---|---|
| `var _ := e;` (plain) | **128** in tests; **3** in the spec (all §17.13's own teaching text — the binding example, the recap sentence, the worked example) plus **2** in the pinned changelog; **0** in the stdlib doc | tests: all are bind-and-ignore (`var _ := a + b;` result-typing probes and similar); mechanical rewrite to `_ := e;` — semantics preserved (the values are non-owned or the end-of-statement release is equivalent for the probe's purpose; each rewritten test re-verified by its own run). Docs: the spec sites are rewritten by this amendment (the changelog is pinned history) |
| Reads of any `_` binding | **0** anywhere | the readable-binding corner is exercised by no code — only by §17.13's worked example |
| `using var _ := e;` | **72** — 46 in tests + 26 across the ten normative documents (spec 17, stdlib 1, changelog 8) | unchanged (row 6) |
| `out _` | **69** — 55 in tests + 14 across the ten normative documents (spec 9, changelog 5) | unchanged |
| `ref _` (genuine) | **8** occurrences in tests, all in one file — `tests/module/xmod_ctor_out_discard_2658_test.cpp` (one live source line, `var w := new W8(ref _);`, in its U3 case `imported_ctor_ref_discard_is_declined_loudly`; the rest comment references); **3** across the ten normative documents (the spec's §17.13-adjacent ruling text and the changelog's #2679 entry) | U3 pins the current decline by diagnostic text (`#2658`, "no constructor call could be lowered"); under row 7 it flips to the new unconditional diagnostic — the decline itself is preserved, its message re-pointed (§8) |
| `lambda` parameters named `_` | **2** (`tests/codegen/event_test.cpp`, `lambda(_) => …` twice); plus destructure-slot `_` uses (already wildcard positions) | semantics preserved as ignores (neither body reads `_`) |
| Parameters `(_: T)`, fields/consts/types named `_`, `out var _`, `for var _` | **0** each | nothing to migrate |

**The migration claim, from the greps:** nothing of value breaks. Exactly one shape needs rewriting (128 test `var _ :=` sites, a one-line `sed` class reviewed per file), zero sites read a `_` binding, the two genuinely-affected lambda sites keep their meaning without edits, and the one `ref _`-pinning test re-points its expected diagnostic (row 7) with its decline verdict intact.

## 6. Alternatives considered

1. **Status quo + record the corner.** Write the rationale entry defending the identifier reading. Declined: rationale §3.2 already *disowns* it ("cannot be read"), and the recorded costs — the #2679 ruling, §17.13's recap subsection, the write-once corollary — keep accruing at every future modifier or binding form.
2. **C#'s design** (contextual discard: `_` is a discard only when no `_` binding is in scope). This is approximately today's rule, and is the source of the split-brain; C# keeps it for backward compatibility Bezel does not have. Declined.
3. **Reserve `_` and also delete `using var _`** (force naming for scope-end holds). Declined: the anonymous scope-end hold is a real, corpus-proven idiom (72 sites across tests and the ten normative documents, §5; stdlib §4.11's own `using var _ := PopAt(index);` example), and it is discard-*shaped* — it binds nothing readable. Row 6 keeps it as the one owning `_` form, now honestly anonymous.
4. **Reserve every `_`-leading identifier.** Breaks the `_camelCase` convention (§1.3) and thousands of corpus sites for no semantic gain. Declined; only the bare token is reserved.
5. **Make method-parameter `_` an ignore (row 11 alternative).** Uniform with lambdas, but method parameter names are call-site-addressable (§22.5 named arguments) and interface-carried (§2.8.2) — an unnameable slot would need rules for named-argument skipping and interface matching. Lambdas have neither concern (function-type parameter names are documentation, A.5 `FuncParamType`). Declined as the default; offered as Q2.

## 7. Migration and compatibility (changelog categories)

- **Newly rejected:** `var _ := e;` (remedy in the diagnostic); `out var _`; `ref _` (now unconditionally — v0.241 already rejected the unresolved case); any declaration named `_` (parameters, fields, methods, types, consts, labels); any read of `_`.
- **Newly defined:** `using var _`/`await using var _` as anonymous bindings (timing unchanged; the name no longer enters scope); lambda-parameter `_` as an ignore (duplicate `_` parameters legal); `for var _` as an ignore.
- **Doc-only:** §17.13 shrink, §1.3 rewording, rationale §3.2 correction, B.1 row.
- **Compiler-visible behavior that does *not* change:** every discard position's semantics (disposal timing, the type-keyed legality list including the `Task` rejection, announced-consume on named owned bindings) is untouched — this proposal moves the *boundary* of what `_` can be, not what discards do.

## 8. Implementation sketch

- **lexer/parser** (`src/lexer`, `src/parser`): bare `_` becomes its own token; the parser's existing `isDiscard` plumbing (#2679) simplifies — the flag *is* the token. Grammar alternatives per §2.
- **sema** (`src/sema/SymbolCollection.cpp`, `ExprTypecheck*`): declaration sites reject the token (rows 5, 8, 11–13) with the §3 diagnostics; lambda/`for` binding paths skip binding for the ignore forms; the expression-position read error falls out of name resolution (no symbol can exist).
- **codegen**: nothing — ignores lower as today's unused bindings do, minus the dead store.
- **tests**: spec-cited regression tests per `tests/README.md` — one per row of §1's table (rejections with diagnostic-text checks in the lit lane; the ignore forms' semantics in the gtest lanes, including owning-iteration `for var _` disposal and `lambda(_, _)` legality); plus the 128-site mechanical migration, landed as its own commit so the behavioral tests diff cleanly. The existing `xmod_ctor_out_discard_2658` U3 case (`imported_ctor_ref_discard_is_declined_loudly`), which pins today's `ref _` decline by the `#2658` "no constructor call could be lowered" diagnostic, is re-pointed at row 7's diagnostic in the same change — the decline verdict does not move.
- **Size:** moderate by breadth, small by depth — many small sites, no new analysis.

## 9. Questions for the owner

- **Q1** — adopt the reservation (rows per §1's table)? Recommended; the review's window argument stands — this is cheap now and impossible after users exist.
- **Q2** — method/constructor parameters named `_`: **compile error** (recommended — names are API surface, §6 alternative 5) or ignore-like-lambdas (uniformity at the cost of named-argument and interface-matching rules)?
- **Q3** — `using var _` retained as the anonymous owning binding (recommended, row 6) — or should the reservation be total, with scope-end anonymous holds dropped (§6 alternative 3)?
- **Q4** — `for var _ : T in` and other ascribed-ignore forms: allow the ascription (documentation value) or reject it (symmetry with `out _`'s no-annotation rule, §17.13)? Recommendation: **reject** — one rule ("a bare ignore takes its type from context, never an annotation") everywhere.

## 10. Interactions

- **Proposal 0090 (sibling: marker study)** reads the same §17.13 text this proposal rewrites; the coordinator should sequence the two at graduation (this proposal shrinks the section 0090 cites).
- **#2679 (parts A/C remain open):** part B's compiler fix (the parser's `ref _` handling) is subsumed by this proposal's row 7 if adopted — the tracked fix should be coordinated so the diagnostic lands once.
- **v0.241's `ref _` ruling:** this proposal deletes the corner that ruling defined; the changelog entry frames it as completing that direction, not reversing it (the ruling said `_` after `ref` is not a discard; reservation agrees and removes the identifier fallback too).
- **Proposal 0056/v0.209 arm-position markers, §28.3 announced-kill forms:** unaffected — `_ := x;` announced-consume semantics are untouched.
