# Proposal 0057 — Paren-delimited expression forms: condition-first `(if …)`, `(case …)`, `(when …)`, and `with(…)`

| | |
|---|---|
| **Status** | graduated-v0.208 — superseded — graduated v0.208 (`68d8002`); composition-gate fixes folded pre-push. Impl delta tracked at the landing issue |
| **Tracking issue** | [#2077](https://github.com/s8lap/bezel-lang/issues/2077) |
| **Target** | Spec: §20 (rewritten), §17.1 (the conditional's entry leaves the level-15 row; `throw` keeps it) + §17.2's atom note, §19 preamble + §19.7 *Termination* (both currently state the `end`-terminator norm), §19.1/§19.2 expression-position examples, §9.5 (`with(…)`), §30.7 (the header resolution, restated for the new spelling), Appendix **A.6** (new `CaseStmt`/`WhenStmt` statement productions) and A.7 (ConditionalExpr retired; the atoms; ThrowExpr notes; the `with` suffix), every expression-position occurrence across the six docs; rationale §3.3 (superseded and rewritten); both guides (idiom change — revalidation pins bump). |
| **Written against** | spec v0.206 (graduating against v0.207) |
| **Origin** | Owner review 2026-07-21. Supersedes the postfix half of the #1373 ruling (2026-07-02) — see §7 for the honest accounting: this **re-weighs and overturns** that ruling's affirmative value-first preference; what is *new* since the ruling is the paren-delimited variant, which deletes the `end`-noise cost that had sunk the condition-first alternative considered then. |
| **Depends on** | Nothing. Proposal 0056's graduation depends on *this* landing first. |

*Reference convention: unprefixed §-references are language-spec references.*

---

## 1. The principle

> **No expression-position compound value form is `end`-closed: each is paren-delimited. `end` closes statement blocks.**

`end` remains for method/class/type bodies, statement `if`/loops, statement-position `case`/`when` (whose arms are statements), and `do … end` blocks wherever they appear — including inside expressions (a lambda's `do`-body, a statement-arm body): those are statement blocks, and the principle is about the *value forms*, not about the token never appearing between parentheses.

## 2. The four forms

### 2.1 The conditional atom (replaces the postfix `a if c else b`)

```
IfExpr ::= "(" "if" Expression "then" Expression
           ("elif" Expression "then" Expression)*
           "else" Expression ")"
```

A **PrimaryAtom**. In atom-start position, `(` followed by `if` is decided with one token of lookahead (`if` is reserved and begins no expression). **In call- and index-argument position the leading `(` is the call's, so the atom needs its own pair: `f((if c then a else b))`** — a disclosed regression against the postfix form, which sat bare in argument lists; the dominant positions (initializers, assignment RHS, `return`, arm bodies) take exactly one pair. `then`/`elif`/`else` are reserved separators; `else` is mandatory; **every condition and branch is a full Expression** — a `throw` branch is admitted in *any* branch position and is bottom-typed, deleting the v0.182 true-branch parse trap (`throw a if c else b` ⇒ `throw (a if c else b)`) outright. Ownership: the K7 arm-meet and the v0.184 arm rules re-key verbatim (verified against K7/K8.3/A7's arm-keyed wording).

```bezel
var port  := (if requested > 0 then requested else 8080);
var grade := (if score >= 90 then "A" elif score >= 80 then "B" else "C");
using var x := (if useFresh then new Buffer() else source.CloneBuffer());
```

Also deleted by the retirement: the conditional's precedence entry and §20's `(a ?? b) if c else d` grouping paragraph; the word-order asymmetry against the statement form and its erratum class (#748); Python's right-nested chaining, replaced by first-class `elif`.

### 2.2 The case atom (expression position) — and the statement split

```
CaseExpr ::= "(" "case" "owned"? Expression "of" CaseArm ("," CaseArm)* ","? ")"
                                                   /* arms: ArmBody = Expression only */
CaseStmt ::= "case" "owned"? Expression "of" CaseArm ("," CaseArm)* ","? "end" "case"? ";"
                                                   /* NEW A.6 Statement alternative; arms admit
                                                      Expression, Statement, and "do" Statement* "end" */
```

Today there is **no** statement production: statement-position `case` reaches statements as an `ExpressionStmt` over the one `end`-closed atom, with the Statement/do arm forms gated semantically ("valid only in statement-position case"). The split makes that gating structural: the A.7 atom is Expression-armed; the **new A.6 `CaseStmt`** carries the `end` form, the `owned` scrutinee marker, and the statement arm bodies. `WhenExpr`/`WhenStmt` split identically. An expression-statement over the atom (`(case …);`) remains legal as any expression-statement is; the `CaseStmt` is the statement form. Interior syntax is unchanged throughout: `of`, `|`-alternative patterns, guards, trailing comma.

Delimiter soundness (verified): pattern-embedded expressions parse at Shift level or tighter, guards and arm bodies balance their own parens — the atom's closing `)` is the first unbalanced one. Argument position trades `end` for a wrapping pair — `f((case x of 1 => a, 2 => b), y)` versus today's `f(case x of 1 => a, 2 => b end, y)` — matched delimiters at the same token count, not an improvement claim.

### 2.3 The when atom — same split, same analysis

`WhenExpr ::= "(" "when" WhenArm ("," WhenArm)* ","? ")"`; `WhenStmt` keeps `end when?;` with statement arms. The construct keyword remains position-distinguished from the arm guard `when`.

### 2.4 The record update: `with(…)` (replaces `with … end`)

```
PostfixSuffix ::= … | "with" "(" InitAssign ("," InitAssign)* ","? ")"
```

`with` is a primary *suffix*; the right delimiters are keyword-attached — the assignment list gets call-style parens, reading as the named-argument list of §22.5 (`M(b := Next())`), which §9.5's own class-typed diagnostic already names as the sibling remedy. C# precedent: `p with { X = 1 }`.

```bezel
var p2 := p1 with(Y := 99);
var q  := p with(Y := 9).Norm();          // suffix chaining continues past the ')'
```

**The `await for` header keeps today's content-based resolution, restated for the new spelling.** Today the header distinguishes the two `with` roles by what follows: `Identifier :=` opens the record-update suffix (binding greedily to the source expression), anything else opens the cancellation clause. Under `with(…)` the rule is the same with the paren interposed: in the header, `with` followed by `(` `Identifier` `:=` is the update suffix on the source; any other `with` (including `with (expr)` over a parenthesized token) opens the cancellation clause. `await for x in rec with(Y := 1) with tok do …` remains expressible, exactly as its `end`-form ancestor was. No behavior changes; the resolution becomes explicit spec text at this landing.

## 3. Grammar deltas

- **A.7**: `ConditionalExpr ::= LogicalOr ("if" Expression "else" Expression)?` loses its postfix option (the level collapses; `ThrowExpr` stays at the top as today); `IfExpr` joins the PrimaryAtom alternatives; `CaseExpr`/`WhenExpr` respelled per §2.2/§2.3; the `with` suffix per §2.4; the ThrowExpr true-branch notes are rewritten (the carve-out dissolves).
- **A.6**: `Statement` gains `CaseStmt | WhenStmt` (the `end` forms, statement arm bodies) — the productions §2.2 writes out.
- **§17.1**: the *conditional's entry* leaves the level-15 row; **`throw expr` keeps the row** (its maximal-right-extent placement is independent and stays; §17.2's note is rewritten accordingly, and its atom sentence now names the paren atoms).

## 4. What does not change

Semantics, everywhere: the K7 arm meet, the v0.184 arm-ownership rules, K8.3's case-scrutinee boundary ("the whole construct" — wording already closer-agnostic, verified), exhaustiveness, `with`'s copy-not-construct semantics, evaluation order, typing. §19's preamble and §19.7 (*Termination*) currently state the `end`-terminator and composes-without-parenthesization norms — both are rewritten to the atom story (they are in Target, not swept under a catch-all).

## 5. Migration

Breaking-across-corpus, mechanical in *shape* but not a token substitution: bare expression-position forms gain a wrapping pair and lose `end` (`var x := case … end;` → `var x := (case …);`); already-parenthesized forms only drop `end` (a blind `end`→`)` rewrite would double-wrap them). Measured surface, honestly: postfix-`if` — 29 single-line doc occurrences plus multi-line chains, and **~58–200+ test files** depending on match strictness (many already parenthesized, migrating paren-for-paren); record-`with` — **~108 occurrences in tests** plus §9.5; expression-position `case`/`when` — not grep-separable from statement position, hand-classified (the statement majority is untouched). Conformance corpus per #1380 discipline (hand-annotations preserved). Compiler: tracked by an impl issue at landing. Guides: idiom change — revalidation pins bump.

## 6. Alternatives declined

1. **Keep the postfix conditional** — overturned with reasons; see §7.
2. **`if c then a else b end`** (#1373's proposal) — the end-noise objection stands; this form adopts its word order and deletes that cost.
3. **C's `c ? a : b`** — §3.3's sigil argument is untouched and survives.
4. **Kotlin-style bare `if c then a else b`** — statement-position prefix collision, dangling-`else` under nesting, extent-by-precedence subtleties. A restricted bare form only inside already-delimited argument lists (to avoid §2.1's double parens) was considered and declined: two spellings whose choice depends on context is worse than one honest paren pair.
5. **Whole-expression parens for `with`** — wrong shape for a suffix; replaced by the keyword-attached form.
6. **De-`end`ing statement-position case/when** — impossible without a closer (a statement arm ends at `;`; the next pattern is indistinguishable from the next statement).

## 7. The #1373 supersession, honestly

The 2026-07-02 ruling kept the postfix form for *affirmative* reasons — value-first "reads naturally in expression position where the value is the point," no dangling-else, Python familiarity — with the word-order asymmetry as the recorded cost; `end`-noise was the objection to the *prefix alternative*, not the basis for keeping postfix. This proposal therefore does two distinct things and says so: it **overturns the value-first preference** — the owner, the same decider, now weighs one selection word order (deleting the #748 erratum class), first-class `elif`, the throw-trap deletion, and a smaller precedence surface above the value-first reading, whose strongest idiom retains `??` only for the *nullable* default case (`??` does not cover boolean-conditioned defaults; that residue is a real cost, accepted) — and it **introduces the genuinely new element**, paren delimitation, which removes the cost that made the condition-first alternative lose when it was last considered. Recorded as a re-weighing by the original decider on partially new design, not as "the old decline never applied."

## 8. Landing order

v0.208, **before** proposal 0056's graduation. Independent of 0055 (landed v0.207).

## 9. Critique record (2026-07-21)

An adversarial critique of the first draft confirmed the core soundness claims — atom interiors cannot swallow their closers; `(if`/`(case`/`(when` collide with no expression start; `with(…)`'s suffix mechanics, chaining, and InitAssign balance; the ownership/boundary kernel re-keys verbatim; the throw-trap deletion is real — and landed six substantive corrections, all resolved in this revision of the text: (1) the call/index-argument double-paren regression was undisclosed (now §2.1, with the declined bare-in-arguments alternative recorded in §6.4); (2) the grammar delta omitted the statement productions entirely — A.6 has no `CaseStmt`/`WhenStmt` today, statement-position case/when reaching statements only as expression-statements over the `end` atom, so the split must *add* those productions or orphan statement arms (now §2.2/§3, A.6 in Target); (3) deleting the whole level-15 row would have orphaned `throw expr`, which shares it (now §3); (4) the first draft's `await for` rule ("header-`with` is always cancellation") would have silently *changed* today's content-based greedy-suffix resolution and was itself unparseable as stated — replaced by the restated content rule (§2.4); (5) migration counts were materially undercounted and the "argument position improves" claim for case was wrong — §19.7 normatively states today's zero-paren composition (both corrected, §2.2/§5, §19.7 added to Target); (6) the #1373 supersession framing was selective about the ruling's affirmative basis (rewritten as §7). One principle-wording nit (lambda `do … end` bodies sit inside expressions) is resolved by §1's value-form phrasing.
