# Proposal 0064 — Keyword-led lambdas: `lambda(params): T => body`

| | |
|---|---|
| **Status** | graduated-v0.217 — **superseded — graduated v0.217** (owner-accepted 2026-07-22, same day as drafting; spec §27.3/§27.4, grammar A.7 `LambdaExpr`, Appendix B.1; rationale §3.13). Graduation decided §8's open questions — hard cutover (0057 precedent, zero external users), the widening extension in the same revision, `async lambda` order — recorded in the v0.217 changelog entry. Impl delta tracked by [#2101](https://github.com/s8lap/bezel-lang/issues/2101); the §7 hole-delimitation adjacency filed as [#2102](https://github.com/s8lap/bezel-lang/issues/2102). |
| **Tracking issue** | — (assign at review) |
| **Target** | spec §27 (Lambdas and Function Types), §17.1 note, Appendix A.7 `LambdaExpr`, Appendix B.1 (one keyword addition); no stdlib surface |
| **Written against** | spec v0.216 |
| **The one-line pitch** | Lambdas are the last major expression form with no committing first token — a leading `lambda` keyword makes every closure announce itself, retires the parser's last unbounded lookahead, and opens a return-ascription slot that fixes a recorded §27.3 limitation, while every other lambda rule carries over verbatim. |

## 1. The problem

Today's form — `(x: int) => x * 2`, A.7 `LambdaExpr ::= "async"? "(" LambdaParams? ")" "=>" LambdaBody` — has three distinct readability/engineering defects. Naming them separately matters because they have different fixes, and because two of the three are *verified* against the v0.216 grammar (§9, the verification record):

1. **No leading signal.** Reader and parser alike discover the form is a function only on reaching `=>`. Post-v0.216 this is unique among the major expression forms: `(if …)`, `(case …)`, `(when …)`, and `(var …)` all commit at their second token; the lambda is the parser's one remaining use of unbounded buffered lookahead (scan to the matching `)`, then test for `=>`), and it needed a dedicated suppression carve so a `when`-guard `(t > 0) => …` is not mis-scanned as a lambda (#1256).
2. **It dissolves into surrounding parentheses.** In argument position — the form's most common home — the parameter list's `(` is indistinguishable from grouping: `Register((x) => f(x), (y) => g(y))`.
3. **No return-type slot.** A lambda has no syntax for its return type. §27.3 (v0.183) deliberately refuses to unify divergent `return` types in a no-target block body and errors "asking for a target type or an ascribed variable type" — the fix today must move *away* from the lambda, to the variable's declaration. (The refusal itself is sound and untouched — see §2.3.)

The current form cannot safely grow a return slot in place: `(x: int): T => e` puts a parenthesized form before `:`, which collides with the dictionary-literal key position (`[(x): …]`, §15.4). A leading keyword makes the slot trivially unambiguous.

## 2. The recommended design

```
LambdaExpr ::= "async"? "lambda" "(" LambdaParams? ")" (":" Type)? "=>" LambdaBody
```

Everything not stated here is **unchanged**: `LambdaParams`, `LambdaParam`, destructuring heads (§27.4), `LambdaBody` (expression or `do … end`), parameter-type inference under target types (§27.4), the procedure/value classification and block-body return rules (§27.3), capture semantics (§27.5, §28.9, §28.14), function types (§27.1 — `->` remains the *type* arrow; `=>` the *value* arrow).

```bezel
// Target-typed positions: params and return inferred, exactly as today.
var admins := users.Where(lambda(u) => u.IsAdmin)
                   .OrderBy(lambda(u) => u.LastName)
                   .Select(lambda(u) => u.Email)
                   .ToArray();

// No target type: params carry ascriptions (as today), return inferred
// from the expression body (as today).
var clamp := lambda(x: int, low: int, high: int) =>
    (if x < low then low elif x > high then high else x);

// The new slot: a block body with divergent returns, self-contained.
// Today this is a §27.3 compile error unless the *variable* is ascribed.
var classify := lambda(score: int): string? => do
  if score < 0 then return null; end;
  if score >= 90 then return "A"; end;
  return "pass";
end;

// Function-type ascriptions end before `=>` (no Type contains `=>`; A.5
// function types use `->`) — verified, §9.
var adder := lambda(n: int): (int) -> int => lambda(m: int) => n + m;

// Dictionary of handlers: each value announces itself.
var ops: Dictionary<string, (int) -> int> := [
  "double": lambda(x) => x * 2,
  "square": lambda(x) => x * x,
];

// Async keeps modifier-first order, continuous with `async method`.
fx.Run(async lambda() => do … end);
```

### 2.1 Parse commitment

`lambda` is reserved and begins no other expression or statement, so the reading commits at the first token (second, after `async`). Consequences, in the same register as the v0.208 atoms' recorded wins: the unbounded lambda lookahead is deleted; the #1256 guard suppression is deleted (a parenthesized guard can never be a lambda prefix again); zero-parameter lambdas keep their mandatory empty pair (`lambda() => 42` — one spelling, no paren-free form).

### 2.2 Interpolated-string holes

§18.5's lexer-level hole delimitation is unchanged. The parenthesized parameter list nests, so `$"{lambda(x: int, y: int) => x + y}"` survives where an *unparenthesized* keyword form verifiably does not (§5.1, §9). The residual body-side edge — a top-level `<…>` generic-argument comma in a hole is cut as an alignment — is pre-existing and shared with the current form (noted as an adjacency, §7).

### 2.3 The return ascription is optional — inference is otherwise untouched

1. **Expression bodies**: the body expression's type, as today (§27.3).
2. **Target-typed positions**: parameters and return flow from the target, as today (§27.4) — the dominant case, where any mandatory annotation would be pure ceremony.
3. **Block bodies with uniform returns**: inferred, as today (v0.183).
4. **Block bodies with divergent returns and no target**: today's error gains a *third remedy* — the on-site slot — alongside a target type and an ascribed variable. §27.3's no-unification posture (no silent common-supertype search; hierarchy-edit stability) is deliberately untouched: the slot is where the programmer *states* the type, never where the compiler computes one.
5. **When present**, the ascription behaves exactly as a method's declared return type: §5.1 implicit widening applies at each `return`, and §5.1's typed-context literal widening (v0.159) extends to it (`lambda(): long => 3_000_000_000`) — the same "declared integer return type" clause methods already have. A declared nullable return gives `null`-producing arms their target (§20's null-arm rule, v0.210).

The design principle: **the keyword is the only new obligation; the form is no heavier than the status quo anywhere the status quo works, and strictly more capable where it does not.**

## 3. The keyword: why `lambda`

Owner-selected at drafting; the comparison is recorded so the choice is revisitable only against these reasons.

- **`lambda`** — the language's own name for the construct: spec §27 is "Lambdas and Function Types", the production is `LambdaExpr`, §27.3 is "Lambda expressions", and existing diagnostics say "lambda". Any other keyword is a permanent keyword≠concept mismatch or a six-document renaming. The term-of-art objection is answered by precedent: `async`, `await`, and `yield` are equally terms of art and already reserved. Unreserved today, used as an identifier nowhere in the corpus, squarely inside Appendix B.3's addition policy. Cost: six characters.
- **`function`** (runner-up) — matches the "function types"/"function values" vocabulary and is fully spelled, but is heaviest at exactly the hottest sites, and implies a category (free functions) the language deliberately lacks — Bezel has *methods*.
- **`func` / `fn`** — right weight, wrong register: the keyword set contains no abbreviations (`constructor`, `extension`, `destructor`), and either would still mismatch the concept name.
- **`method`** — zero new keywords (already reserved, begins nothing in expression position; Go's shared-keyword precedent), but semantically wrong twice: a lambda has no receiver and is not a member; and it forces a context switch against the declaration meaning one scope up.
- **`closure`** — over-promises capture (many lambdas capture nothing; §27.5 uses the word for the capture aspect specifically). **`proc`** — abbreviation plus borrowed Ruby semantics. **`do`** — would make `do (x) => do … end` legal. **`callback`** — names a role, not the construct.

**Reservation class**: unconditionally reserved (B.1), not contextual. Reservation is free (zero corpus collisions), and contextual status would reopen the `Identifier`-lookalike seams (a call to a user function named `lambda`; the loop-label shape) that the keyword exists to close.

## 4. The survey — what other languages do, and why each family fails or survives here

Recorded so the space stays mapped. Verdicts cite v0.216 token facts, several adversarially verified (§9).

| Family | Exemplars | Verdict for Bezel |
|---|---|---|
| Arrow, delimiter-led | C#, JS/TS, Java (`->`), Scala, Dart, Julia | The status quo. Java/Julia's `->` would pun the function-*type* arrow; Bezel's `->`-types / `=>`-values split is a clarity asset. |
| Keyword-led | Python, OCaml/F# (`fun`), Erlang/Elixir, Go, Ruby (`lambda`), Lua | The live direction — this proposal. OCaml's space-separated curried parameters clash with Bezel's non-curried function types. |
| Keyword + `end`-delimited | Elixir `fn x -> e end` | Declined by 0057's recorded reason, transferring verbatim: "end-noise inside nested expressions". |
| Brace blocks | Swift `{ x in e }`, Kotlin `{ x -> e }`, Ruby | Braces are technically free in Bezel expressions, but Swift's separator is fatal (`in` is §17.5 membership — `{ x in xs }` is ambiguous), Kotlin's puns the type arrow, and a brace block invites statement-block pressure against §5.13. |
| Square blocks | Smalltalk `[:x \| e]` | Triple collision: `[` (collection literals), `:` (fatal, §5.1), `\|` (pattern alternatives / bitwise-or). |
| Placeholders / implicit params | Scala `_`, Kotlin `it`, Swift `$0`, Clojure `%` | Declined on *meaning*: Bezel's `_` is the discard — "ignore this", the exact inverse of "use this" (§17.13); `it`/`$0` carry hidden arity, against the announced-flow personality. |
| Sigil-prefix | Haskell `\x -> e`, Rust `\|x\| e`, R 4.1 `\(x)`, MATLAB `@` | The words-not-sigils ground (rationale §3.3's `?:` precedent) plus the `\|` collision. |
| Heavy or absent by design | Zig (none), C, Ada, Pascal/Delphi, C++ | The instructive family: clarity-first systems languages keep inline functions heavy to push named helpers — Bezel's existing extraction doctrine. C++'s explicit capture list is separately noted in §7. |

### 4.1 The literal Python form (`lambda x: body`) — declined, with verified grounds

Examined in depth (2026-07-22 audits, adversarially confirmed — the refutation attempt failed on every front):

1. **The colon is unavailable.** `:` is the ascription token in every binding form; primitive type keywords are themselves expression atoms (A.7 `PrimitiveType` — `int.MaxValue`, `int(d)`), so `lambda x: int` has two live readings; the generics disambiguator (A.7) admits a `<…>` type reading only before `.`, `?.`, `(`, `[` — a following `:` is in neither set, so `lambda x: Array<int>: body` falls outside the existing machinery; and `[lambda x: A.B: C]` has two complete parses decidable only by whether `A.B` names a type — a semantic oracle in the parser.
2. **Dictionary literals break normatively.** §15.4 *defines* sequence-vs-dictionary discrimination by "top-level colon in an element" and makes an element colon a compile error outside dictionary targets — an ordinary array of such lambdas (`[lambda x: x * 2, lambda y: y - 1]`) is unwritable, and the form falsifies §15.4's recorded premise that the pair syntax "reuses no token in a conflicting role".
3. **String-interpolation holes cut it at the lexer** (§18.5): `$"{lambda x: x}"` splits at the format-spec colon before parsing begins.
4. Python's own escape — forbidding parameter annotations — is blocked here: §27.4 normatively *requires* ascriptions wherever no target type exists. And a zero-param `lambda: e` at statement start is lexically `Identifier ":"` (the loop-label production) unless the keyword is unconditionally reserved.

### 4.2 Alternatives within the keyword family

- **Bare parameters** (`lambda x, y => body`) — the terser sibling, audited viable at the grammar level (no comma operator, no expression-level `;`, arm-arrow theft only in type-nonsense programs) but **refuted in interpolated-string holes**: the §18.5 lexer cut severs an unparenthesized parameter list at its comma/colon (`$"{lambda x, y => x + y}"`). Adopting it would need a stated parenthesize-in-holes carve plus a new parameter grammar. Recorded as the alternative if call-site weight is ever judged to outrank uniformity; the recommended form keeps §27.4 verbatim instead.
- **Semicolon parameter separators** (`lambda(x: int; y: int) => e`) — parseable, but a second list-separator convention against v0.214's uniform comma lists and against function-type spelling (`(int, string) -> bool`), with no ambiguity to pay for it (the keyword already commits). The one design where `;` becomes load-bearing is Pascal-style shared-ascription groups (`lambda(x, y: int; z: string)`), which is a *parameter-list* question (methods included), out of this proposal's scope.
- **The wrapped paren-atom** (`(lambda (x) => e)` in the v0.208 family style) — dominated by the recommended form: same commit benefit, plus a self-supplied pair at every call site, i.e. the inverse of 0057's frequency trade.

## 5. Migration, and the honest tension with D-0003

Facts from the 2026-07-22 audit:

- Reserving `lambda` breaks **zero** corpus code (never used as an identifier in stdlib, docs, or tests; B.3 addition policy explicitly covers it).
- The respelling itself is wide: `=>` appears ~239× (spec) / 66× (stdlib) / 15× (guides) / 212× (conformance cases, 79 of 444 files), of which lambdas are only **~10–13%** — the token is dominated by arms and expression-bodied members, so migration sites are **not grep-separable**; a rewriter needs a parser. The rewrite itself is minimal (insert one token per lambda; the recommended form changes nothing else).
- **D-0003** is the on-point precedent *against* spelling-only migrations ("a whole-corpus migration to retire zero rules is the definition of churn"). The counter this proposal must and does make: it is **not** zero-retirement — it deletes the parser's last unbounded lookahead and the #1256 carve, and it ships one genuine capability (the return slot, §2.3 rule 4) that today's grammar *cannot* add in place (§1). Whether that clears the bar is precisely the owner decision this proposal exists to pose.
- Three recorded decisions cite the current lambda spelling and need their citations re-verified (not reopened) at graduation: 0055's contract-clause parens (its lambda-ambiguity ground becomes historical — the parens stand on their other grounds), the `[k => v]` dictionary-form rejection (`=>` remains overloaded by arms; unaffected), and 0061/rationale §3.12's separator-choice note.
- Staging: a hard cutover (0057-style) or a deprecation window are both viable; note that the parser simplification — half the payoff — arrives only when the old form is removed.

## 6. What this proposal deliberately does not do

- **No shorthand tier** (placeholders, implicit parameters, paren-free single params) — each declined in §4 on recorded grounds; `x => e` additionally collides with the arm form (`ready => 1` *is* `Identifier => Expression` in a `when` arm).
- **No local named methods.** Considered as a companion and **deferred by owner decision (2026-07-22)**: C# practice prefers lambdas, and the construct reads as a niche tool; revisit only on concrete evidence of `=> do … end` bodies churning in review.
- **No capture-list syntax** (C++-style `[borrow pool]`). Bezel's capture rules are compiler-checked (§27.5, §28.9, §28.14); an optional *reader-facing* capture annotation is a separately recordable direction for an ownership language, noted here so the idea has a home.
- **No change to `->`/`=>` division of labor**, to §27.3's no-unification rule, or to §27.4's parameter inference.

## 7. Adjacencies noted in passing (not this proposal's scope)

- **§18.5 hole delimitation does not track `<…>`**: `$"{Cache<int, string>.Default}"` is cut at the generic-argument comma *today* — defined behavior with the documented parenthesize remedy, but a sharp edge; a hole-nesting extension to balanced `<…>` (or a spec note) is worth its own small issue.
- Proposal 0050 line 166 calls a statement-leading `Identifier "=>"` a "bare-parameter lambda"; no such production exists (A.7 mandates parens). Editorial-tier cleanup for that superseded file's next touch.

## 8. Open questions

1. Hard cutover vs. deprecation window (§5) — and whether the conformance corpus migrates by regeneration or hand-edit (the #1380 discipline applies either way).
2. Does the v0.159 typed-context widening extension (§2.3 rule 5) land in the same revision, or as a follow-up ruling?
3. `async lambda` is the recommended modifier order (continuity with `async method` and the current `async (…)`); confirm at review.

## 9. Verification record (2026-07-22)

The load-bearing claims were checked against the v0.216 spec by independent audit and adversarial refutation passes run at drafting: the colon-fatality of the literal Python form (refutation attempted and failed; §4.1's grounds are its findings); the arrow-hybrid ambiguity sweep (no silent misparse found; arm-arrow theft only in ill-typed programs; no Type production contains `=>`); the hole-extent refutation of the *bare*-parameter form (§4.2 — the finding that selected the parenthesized recommendation); and the keyword/migration counts of §5. Where this proposal asserts a grammar fact, it is one of those verified findings, not an assumption.
