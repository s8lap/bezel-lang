# Proposal 0100 — The body seam: examined and closed

| | |
|---|---|
| **Status** | graduated-v0.260 — **superseded — graduated v0.260** (charter ruling C12, the **owner's own mid-turn 2026-08-18 direction** — bare-open kept; the blank-line separator landed as §22.16's recommended-style sentence, non-normative, with the docs restyled at the re-measured **10** sites and `check_docs.sh` gaining the style check (landed as check **3d**, not the drafted "check 5" — v0.257's TABLE check took the interim slot; scope landed as the nine non-changelog docs, the 0095-Q4 deviation shape); the floor landed (`requires`/`ensures`/`invariant` unconditionally reserved, two B.2 rows deleted, the one corpus site confirmed and recorded for the compiler-lane flip); the 0055 §3 diagnostic obligation restated live, re-scoped; rationale §3.1's third-category note added; this document standing as the permanent decline record. One train with 0101 and 0102) |
| **Tracking issue** | coordinator files |
| **Target module(s)** | **spec** — §22.16 (one sentence appended to the existing *Recommended style, non-normative* note), Appendix B.1 (`requires`, `ensures`, `invariant` become unconditionally reserved — **three edits in that one subsection**, §4.1), Appendix B.2 (**two** rows delete — `requires`/`ensures` share one; `old` and `result` stay contextual), Appendix A.4 commentary (the clause-region comment's "keyword reading always wins there (the B.2 env rule)" conforms — after reservation the winning reading is unconditional, not contextual). **rationale** — §3.1 (*Always semicolons*) gains the third-category note. **tests** — `tests/parser/contract_clauses_2053_test.cpp` :: `requires_as_parameter_name_not_a_marker_spec_22_16` flips from an accept to a reject (one site). **scripts** — `scripts/check_docs.sh` gains check 5 (docs-only style conformance). **guides** — Guide 06's clause-bearing examples restyle. |
| **Written against** | spec/stdlib **v0.255** (`git show 5fa5a95b:docs/*.md`) |
| **Currency** | **v0.256** (HEAD `8f41e99d` — the defect train `4130d2de` plus its gate fixes `e8f5387a`): every quoted §22.16, Appendix B, rationale §3.1, and test text is unchanged. Adjacent, no conflict: v0.256 added §13.3's fourth visibility exception (*an `invariant` member carries no visibility keyword*) and its Appendix C.2 row — prose about the `invariant` **member kind**, orthogonal to reserving the **word**; the floor's B.1/B.2 edits (§4.1) compose with it untouched. |
| **Origin** | The body-seam study (elegance-review programme, 2026-08). The question: a Bezel member declaration's body opens with **no token at all**, so where a member carries `requires`/`ensures` clauses, the reader must recognise the end of the clause region by noticing that the next line is not a clause. Every language with a contract region and a bare-ish body has faced this; the study inventoried the answers. |

*Reference convention: unprefixed §-references are language-spec references; rationale references are marked. Every quotation and every count is from the v0.255 snapshot (re-verified unchanged at v0.256) or from `tests/` at HEAD. Bold inside a quotation is the source's own unless marked* (emphasis added).

---

## 1. The seam, stated exactly

A method or constructor declaration is: signature, then an optional **clause region** (`requires(E)` / `ensures(E)`, each self-delimiting and carrying **no terminator** — §22.16: *"A clause's parentheses are structural — part of the clause syntax, not of its expression — so a clause is self-delimiting and carries no terminator"*), then the **body**, which begins with no opening token and runs to `end <Name>;`.

```bezel
public method Withdraw(amount: decimal)
  requires(amount > 0m)
  requires(amount <= _balance + _limit)
  ensures(_balance = old(_balance) - amount)
  _balance := _balance - amount;
end Withdraw;
```

Three consecutive lines look alike and one of them is the body. The grammar has no ambiguity — the clause region is `ContractClause*` in the declaration productions (A.4: `GenericConstraints* ContractClause* MethodBody`, with `ContractClause ::= "static"? ("requires" | "ensures") "(" Expression ")"`), and A.4's own commentary states both halves of why that is unambiguous, quoted verbatim:

> */* the parentheses are clause syntax, not expression tokens (§22.16): a clause is self-delimiting and carries no terminator. \*/*
>
> */* the clause region (§22.16): after the signature's GenericConstraints\*, the tokens requires/ensures (or static followed by either) open it; keyword reading always wins there (the B.2 env rule). \*/*

So the region ends at the first token that opens no clause — but the **reader** has to run that rule. That is the seam. It has two halves, and the study separated them:

- **(a) The reading half.** Nothing marks where the promise stops and the doing starts.
- **(b) The stealing half.** Because `requires`/`ensures` are *contextual* keywords (Appendix B.2), a body's first statement could in principle be a call to something *named* `requires` — and the clause region would swallow it. Proposal 0055 §3 analysed this precisely and named the failure modes.

The owner's ruling answers (a) with a **style convention** and (b) with a **floor**, and declines every token-adding design.

## 2. The ruling on the main question: bare-open is kept

No body-opening token is added, in any form, mandatory or optional. **No pending direction is recorded** — this is not a deferral. The reasons are §5's decline record; the summary is that every candidate token costs a five-figure corpus migration, or a second legal spelling, or a reserved word, to buy a cue that costs one blank line.

The contract-free case — a member with no clauses, whose signature is immediately followed by its first statement — is separately **examined and kept** (§6).

## 3. The blank-line separator: a recommended, non-normative style convention

### 3.1 The register, and why it matters

§22.16 already contains one style recommendation, in its *Clause granularity is semantic* note:

> **Clause granularity is semantic.** `requires(a) requires(b)` is not a style variant of `requires(a and b)`: the clause is the unit of tier, blame, evaluation order (stop at first failure), and interface identity. Recommended style, non-normative: one condition per clause, each clause on its own line.

The blank-line convention lands **in that note, in that register**. It is a recommendation about how to write Bezel, not a rule about what Bezel is. There is **no normative obligation on user code**: a conforming implementation neither requires the blank line nor diagnoses its absence, and a program without it is in every respect as correct as one with it.

### 3.2 The sentence, as drafted

The note's last sentence is extended to read:

> **Recommended style, non-normative:** one condition per clause, each clause on its own line and, where a member's clause region is followed by a body, a blank line between the last clause and the first body statement — the promises above the gap, the doing below it. Both recommendations are style only: neither is part of the grammar, no implementation consults the layout, and a member written without them means exactly what the same member written with them means.

Three properties of that wording are deliberate and should survive editing:

1. **It is one sentence continuing an existing recommendation**, not a new normative paragraph — the register the owner named.
2. **It states the reading rule it serves** (*the promises above the gap, the doing below it*), because a convention whose purpose is unstated is a convention nobody follows.
3. **It disclaims parse-significance explicitly** in its own second half, rather than leaving the disclaimer to §1's whitespace rules. §3.4 explains why that sentence is load-bearing rather than defensive.

### 3.3 The docs restyle, scoped and measured

The documents adopt their own convention — a style recommendation the reference documents themselves violate is worse than none.

Measured over the six live normative documents at the v0.255 snapshot (`Bezel_Changelog.md` excluded as pinned history that is never retroactively edited):

| Document | `requires`/`ensures`/`invariant(` occurrences | Clause-region → first-body-statement adjacencies (the restyle sites) |
|---|---|---|
| `Bezel_Language_Spec.md` | 12 | **2** |
| `Bezel_Guide_06_Contracts_and_Purity.md` | 22 | **8** |
| `Bezel_Guide_02_Errors_and_Nullability.md` | 3 | 0 |
| `Bezel_Guide_03_Modeling_Data.md` | 1 | 0 |
| `Bezel_Design_Rationale.md` | 2 | 0 |
| **Total** | **40** | **10** |

*(Methodology: fenced code blocks of every tag; a site is a run of clause lines whose following non-clause line is a statement rather than `end`. The charter cites the study's figure of ~19 fence sites; this re-measurement finds **10** adjacencies by the methodology stated here, the difference being that the study appears to have counted clause-bearing fences including abstract and interface members, which have no body and therefore no seam to restyle. Both numbers say the same thing: the docs restyle is a **single-afternoon, ten-site edit**, not a programme.)*

The remaining 30 clause occurrences are on declaration-only members (abstract methods, interface members), inside prose, or inside `invariant` members — none of which has a clause-region→body adjacency.

### 3.4 The `check_docs.sh` extension

`scripts/check_docs.sh` is the docs gate — five mechanical checks (anchor resolution, version consistency, §-citation sanity, statics-facts derivation, example extraction), each described in the script's own header block as *"a lint, not a proof"*. The convention gets **check 5**, specified here so the graduation train implements the right thing:

> **5. Clause-region body separation (style, docs-only).** Over the six live normative documents' fenced Bezel blocks: for every member whose clause region (`requires`/`ensures`, with or without `static`) is followed by a body statement rather than by `end`, require a blank line between the last clause and that statement. Report every violating block with document, line, and member name; exit non-zero. **Scope is the documents only** — the check exists so the documents follow the style they recommend, and it is neither a compiler diagnostic nor a lint over user code or over `tests/`. Skipped by `--quick`, like check 4.

Two boundaries the implementer must respect: the check reads **only** the ten (or however many, after the restyle) clause-bearing member blocks and must be a no-op over every other block; and it must not be wired into `scripts/gate.sh` or `agent_check.sh`, which gate the compiler, not the prose.

### 3.5 Explicitly not parse-significant — and why that has to be said

Bezel's grammar is **free-form**. That is not an accident of the lexer; it is a design commitment with a history and a scar. Issue [#744](https://github.com/s8lap/bezel-lang/issues/744) removed the language's *only* newline-significant rule — the scrutinee-less `case` heuristic, which reached past the token stream into raw source line numbers, so that *"the **same tokens** can parse differently depending on layout."* Its filed motivation names the principle directly:

> **2. It violates the language's own stated philosophy.** Rationale §3.1 ("Always semicolons") argues structure must come from `;` and `end`, never from layout, explicitly citing the Ada lesson. The `case`-newline rule is the sole violation of that principle.

#744 was closed as completed; the language has been layout-free since. A blank-line separator that the parser consulted would re-introduce, at a *different* seam, exactly the property #744 was opened to remove — and it would do so for a construct where the grammar is already unambiguous. The convention is therefore style, permanently: the ruling records it as such and the reopening bar (§9) does not admit re-litigating this half.

**And the parser never needs it,** because the floor closes the stealing half of the seam (§4). That is the load-bearing composition of this ruling: the blank line answers the *reading* problem, at zero grammatical cost, precisely because the *parsing* problem is answered by reservation rather than by layout.

## 4. The floor

### 4.1 `requires`, `ensures`, `invariant` become unconditionally reserved

Today all three are **contextual keywords** (Appendix B.1's contextual roster; Appendix B.2's rows):

| Keyword | Reserved context (B.2, v0.255) | May be used as identifier |
|---|---|---|
| `requires`, `ensures` | *"Opening a contract clause in the clause region of a method or constructor declaration (§22.16); the keyword reading always wins there"* | *"Anywhere else: as identifiers"* |
| `invariant` | *"Member-leading position in a class body, optionally after `static` (§22.16)"* | *"Anywhere else: as an identifier"* |

They become unconditionally reserved. The edit, stated precisely so the train makes all of it: **B.1 takes three edits in one subsection** — (1) the three words join the **Unconditionally reserved** list, (2) they leave the **Contextual (see §B.2)** list, and (3) they leave B.1's *opening-paragraph roster* as well (the parenthesized contextual-keyword enumeration in B.1's first paragraph repeats the list, and a word removed from the list but left in the roster would make B.1 disagree with itself). **B.2 deletes two rows, not three** — `requires` and `ensures` share a single row (as the table above shows), and `invariant` has its own. `old` and `result` **stay contextual** — they are clause-interior names, they collide with ordinary user vocabulary far more plausibly (`result` especially), and their contextuality is already handled by B.2's marker-wins wording without a seam.

**What it buys.** Proposal 0055 §3's *first-statement collision* — a body whose first statement calls something named `requires` — becomes **impossible**, because nothing can be named `requires` any more. The clause region can no longer steal a statement under any spelling, at any member kind. This is the reason the parser never has to look at the blank line.

**What it costs: one test site, measured.** Across the whole corpus — `tests/**/*.bzl` plus Bezel source reconstructed from `.cpp` string literals — there is exactly **one** identifier-position use of any of the three words:

```
tests/parser/contract_clauses_2053_test.cpp :: requires_as_parameter_name_not_a_marker_spec_22_16
    "  public method M(requires: int): int => requires; "
```

pinning that *"a method whose FIRST parameter is named `requires` parses as a normal parameter name — the marker-wins rule applies only inside the clause region, after the signature."* Under the floor that test **flips to a negative test**: the same source becomes a parse error (a reserved word in identifier position), and the test asserts the diagnostic rather than the acceptance. The docs carry zero such uses. *(A second grep hit is prose inside a lit-test comment, not code.)* The charter's "one test site" is confirmed.

### 4.2 The 0055 §3 diagnostic obligation, restated as live

Proposal 0055 §3 carries an obligation that was recorded and never implemented:

> *Method:* the `;` after the clause matches the lone-`;` MethodBody alternative, so the member parses as **body-less** and is rejected semantically ("a body-less method must be declared `abstract`"), with any intended body statements then erroring in member position — loud, at the member, though not a parse error. **The migration therefore adds the §22.16 remedy (parenthesize `(requires)(x);` or rename) as a note on the non-abstract-body-less diagnostic whenever the member carries clauses.**

The floor closes the **identifier half** of that obligation: after reservation, no user code can name a parameter or static `requires`, so the "intended call was stolen" scenario cannot arise from a name. The obligation nevertheless **stays live**, for two reasons, and the graduation train restates it as such rather than deleting it:

1. **It is a diagnostic-quality obligation, not only a collision remedy.** A non-abstract member that parses body-less *while carrying contract clauses* is a shape whose most likely cause is a clause-region confusion, and the diagnostic should say so — with or without a stealable name. That is the note 0055 asked for, and it is worth having on its own.
2. **It is unimplemented.** Filing it as closed because the collision is closed would retire an obligation nothing ever discharged. It goes to the compiler-side issue list as its own item (the coordinator's small-records list already carries it).

The graduation train's §22.16 edit therefore keeps 0055's remedy sentence, with its parenthesize-or-rename half re-scoped: after reservation the remedy for a stolen statement is not renaming (nothing is named `requires` any more) but recognising that the clause region ended where the writer thought it had not.

### 4.3 The rationale §3.1 third-category note

Rationale §3.1 (*Always semicolons*) is short and absolute:

> Pascal famously had places where a semicolon was wrong — typically before `else` — and places where it was required, with the rule depending on what came next. The Ada designers fixed this by making semicolons unconditional: every statement ends with `;`, every block ends with `end;`, no exceptions.
>
> Bezel follows the Ada rule. The rule is "always put a semicolon," which is impossible to get wrong by accident.

Contract clauses are a **third category** the two-category story does not describe: not "semicolon required", not "semicolon wrong", but **no terminator at all, by construction** — §22.16's *"A clause's parentheses are structural … so a clause is self-delimiting and carries no terminator; the `invariant` member, being a member, keeps the member-terminating `;`."* A reader who has internalised "always put a semicolon" and then meets a clause region reads it as broken. The note names the category and its ground:

> **A third category, added with the contract clauses (v0.203/0055).** A `requires`/`ensures` clause carries **no** terminator — not because a semicolon would be wrong there in the Pascal sense, but because the clause's own parentheses are structural and already delimit it, so there is nothing for a terminator to do. The rule above is therefore about **statements and blocks**, which is where it was always aimed: every statement ends with `;`, every block ends with `end;`. A self-delimiting *clause* is neither. The `invariant` member is the check on the reading — it is a **member**, so it keeps the member-terminating `;` exactly as the rule demands. *(Whether the clause region should also have carried a visible closing cue is the question proposal 0100 examined and closed: the answer is a recommended blank line — style, not grammar.)*

## 5. The decline record, in full

Each candidate is recorded with the exact ground on which it lost. This is the section a future reader is meant to find.

### 5.1 Mandatory `do` — the strongest candidate, declined

`method M(x: int): int requires(x > 0) do … end M;`, with `do` required at every member body.

- **Provenance is real.** This is Eiffel's design (`require … do … ensure … end`) — the language that invented the clause region — and Elixir's universal `do … end`. `do` is already **unconditionally reserved** in Bezel (Appendix B.1) and already opens `for` and `while` bodies, so the token costs nothing to obtain and reads consistently.
- **It closes both halves of the seam by itself**, which no other candidate does: the clause region ends at `do`, visibly and grammatically, and no statement can be stolen.
- **It loses on migration size.** Re-measured at HEAD over the Bezel corpus (`.bzl` files plus Bezel source reconstructed from `.cpp` string literals): existing `do` tokens **5,249** (the study's figure: ~5,165 — the tree has grown since); member-body openings that would each gain one, **on the order of 24,000–31,000** (the study's figure ~23,900; an independent coarse header regex here counts ~31,000 bare-open bodies, over-counting where a declaration is split across a `.cpp` string-literal boundary). The two measurements disagree on the exact number and agree on the only thing that matters: **the mandatory-`do` migration is a five-figure token insertion across the corpus and every line of every example in every document.** For a language with zero external users that is affordable and it is not free — and what it buys, at 99.5% of those sites, is nothing, because 99.5% of members carry no clauses and have no seam.
- **The runner-up, `does`.** The study considered `does` as a body opener for the *reading* reason: `do` already means "loop body" in `for`/`while`, so reusing it makes one token mean two structurally different things, and a member body is not a loop body. `does` reads as a declaration continuation (*"this method does: …"*) and collides with nothing. It loses to `do` on the only ground that separates them — `do` is already reserved and `does` would be a new reserved word, breaking any program that uses it as an identifier and enlarging the keyword set for a distinction no reader has complained about. **Recorded as the runner-up so that a future revisiting starts from `do` versus `does` rather than rediscovering the pair.**
- **Verdict: declined.** A five-figure insertion to mark a seam that occurs at 10 sites in the entire normative corpus.

### 5.2 Optional or conditional `do` — declined

Two variants: `do` admitted everywhere but never required; or `do` **required exactly when the member carries clauses** and forbidden otherwise.

- **Optional-everywhere** creates two legal spellings for every member body in the language, forever. The documents must then either pick one (a de-facto migration, i.e. §5.1's cost paid slowly) or stay neutral (the reader's burden **grows** — both spellings must be learned, and a reader who sees `do` on one member and not the next must decide whether the difference means something). This is the shape proposal 0090 §4 costed as *"the worst long-run state; viable only as a never-finish migration"*, and the v0.246 **one-spelling-per-site** ruling (Q2, owner-ruled) rejected the analogous both-ways surface for the transfer marker at a cost of three corpus lines. Declining here is consistency with an owner-ruled precedent of this same programme — the precedent proposal 0102 §2 records for the redundant-marker question, and the two records cite each other (§10).
- **Conditional-on-clauses** is worse in a specific way: it makes a member's *body syntax* depend on whether it carries contracts, so **adding a `requires` clause to an existing method becomes a breaking edit to its body** — you must also insert `do`. Contracts are meant to be addable to working code; a rule that makes adding one a two-place edit taxes exactly the practice §22.16 exists to encourage. It also gives the parser a mode (`in a clause-bearing member`) that the grammar does not otherwise have.
- **Verdict: both declined.**

### 5.3 A colon (`:`) after the signature or after the clause region — declined

`public method M(x: int): int requires(x > 0): _t := x;` or a colon on its own line.

- `:` is the language's **type-ascription** token, everywhere: parameters, returns, fields, locals, lambda ascriptions, generic constraints. A method's return type is introduced by `:`. A second `:` a few tokens later, introducing something that is not a type, is the most confusable spelling available — and on a unit-returning member (`method M(x: int)`, no return clause) the body-opening colon becomes the *only* colon on the line, which reads like a mis-typed return type.
- **Verdict: declined on collision with the language's most-used punctuation.**

### 5.4 A semicolon terminating the clause region — declined

`requires(x > 0);` per clause, or one `;` closing the region.

- **Per clause** contradicts §22.16's own design decision: *"A clause's parentheses are **structural** — part of the clause syntax, not of its expression — so a clause is **self-delimiting** and carries no terminator."* Proposal 0055 chose structural parentheses *specifically* so the clause needs no terminator, and it recorded the failure mode of the terminator-bearing v0.203 form it replaced: `requires(x > 0)(foo).Bar();` used to parse as *one clause* whose expression was a call of a call, consuming the final `;` — *"the mistake surfaced only as a later type error, never at the token that went wrong."* Re-adding the terminator re-opens that.
- **One `;` for the whole region** invents a terminator that terminates a *region* rather than a statement or a member — a third meaning for the language's most overloaded punctuation, against rationale §3.1's whole argument that `;` should mean one thing unconditionally.
- **Verdict: declined; it reverses proposal 0055's ruling for a cue.**

### 5.5 The `=`-family (`=`, `=>`, `:=`) — declined

- **`=>`** is the expression-body arrow (`method M(): int => 42;`) and the lambda arrow and the `case`-arm arrow. Using it to open a *statement* body would make `=>` mean "the value is" in two places and "the statements are" in a third — and the confusion is live, because a clause-bearing member can legitimately have an expression body (§22.16's own texts admit `requires` on arrow-bodied members; `tests/parser/contract_clauses_2053_test.cpp :: method_clauses_with_expression_body_spec_22_16` pins it). One arrow would then open either a value or a block depending on what followed.
- **`=`** is not an assignment in Bezel at all (`:=` is), and is the **equality operator** — `ensures(_balance = old(_balance) - amount)` in §22.16's own example. A body-opening `=` would sit one line below an `=` used as equality.
- **`:=`** is assignment; a bare `:=` opening a body has no reading.
- **Verdict: declined; every member of the family is already spoken for at exactly this seam.**

### 5.6 A novel symbol (`|`, `::`, `->`, a new digraph) — declined

- Bezel's punctuation vocabulary is deliberately small and every symbol earns its place. Introducing one whose sole job is "the clause region ended" spends a global lexical resource on a local reading cue, and every future construct that wants that symbol is pre-empted. `->` is the function-type arrow; `::` and `|` are unused *and that is an asset*, not an opportunity.
- The general principle the study applied: **a symbol should not be spent on a distinction a blank line makes.**
- **Verdict: declined.**

### 5.7 Do nothing at all — the owner's own question, and what "nothing" actually meant

The owner asked *"what if we do nothing?"*, and the honest answer the study returned is that doing nothing is **very nearly right**: the grammar is unambiguous, the collision is analysable, the corpus has one identifier-position use of the three words, and the seam bites at ten sites in the entire normative corpus. What "nothing" leaves on the table is (a) the reading cue and (b) the un-closed stealing half. The ruling takes both at their true price — a style sentence and a keyword-list move — and declines everything that costs more.

## 6. The contract-free bare opening: examined and kept

A member with **no** clauses opens its body immediately after its signature:

```bezel
public method Add(owned item: T)
  _items.Add(move item);
end Add;
```

This was examined on its own, because it is the 99.5% case and because a mandatory body token would have changed it too.

- **The precedent is Lua**, whose `function f(x) … end` has exactly this shape and is among the most-taught first languages in existence; the bare opening is not a known source of confusion there.
- **The frame is the declaration signature itself.** A Bezel signature is a closed, visually distinctive unit — modifiers, `method`, name, a parenthesised parameter list, an optional `: Type`. The line *ends* with a syntactically complete signature and the next line begins something that is not one. That is a stronger cue than it looks, and it is the same cue Lua's `function f(x)` gives.
- **The terminator is named.** `end Add;` repeats the member's name (§21/§22's `end <Name>` discipline), so the body's *extent* is unambiguous even when its start is unmarked — a property Lua does not have and Bezel does.
- **Verdict: kept, examined.** Recorded here so that a future proposal for a body token cannot claim the contract-free case was never considered.

## 7. The cross-language survey

The study surveyed six languages with a comparable seam. Recorded here so the record stands alone; each conclusion is one line.

| Language | How the body opens after the declarative region | What Bezel takes from it |
|---|---|---|
| **Eiffel** | `require … do … ensure … end` — the contract region is bracketed by keywords and the body is opened by a mandatory `do` | The origin of both the clause region and the `do` opener; it is §5.1's runner-up design and its provenance. Eiffel pays the mandatory token on every routine — which is affordable when it is the language's original grammar and not a migration. |
| **D** | `in { … } out { … } body { … }` (later `do`), all braced; the function body's `{` is mandatory | The seam is closed by a delimiter the language already required everywhere. Bezel has no such universal delimiter — its blocks close with `end`, they do not open with anything — so D's answer is unavailable without inventing the opener. |
| **Ada** | declarative region, then a mandatory `begin`; contracts are **aspects on the specification** (`with Pre => …`), not a region before the body | Two lessons. The body-opening token (`begin`) is the mandatory-`do` design under another name; and Ada moved contracts *out* of the body's neighbourhood entirely, which dissolves the seam rather than marking it. Rationale §3.1 already cites Ada for the semicolon rule; the same source argues for the token, and Bezel declines it on migration cost, not on principle. |
| **Cobra** | `require` / `ensure` blocks with **indentation-significant** bodies | Layout closes the seam — and layout is exactly what #744 removed from Bezel. Unavailable by commitment. |
| **Lua** | `function f(x) … end` — bare open, no body token, no contract region | §6's precedent for the contract-free bare opening. |
| **Elixir** | `def f(x) do … end` — `do` universally opens every block | The universal-`do` end state: consistent, and reached by having always been that way. Bezel's `do` is already universal for **loops**; extending it to member bodies is §5.1, priced. |

**The survey's conclusion, in one sentence:** every language that marks this seam marks it with a token it already required *everywhere* (D's `{`, Ada's `begin`, Elixir's `do`) or with layout (Cobra) — nobody adds a token *for the seam alone* — and Bezel has neither a universal opening delimiter nor layout significance, so marking the seam would mean creating the universal token retroactively, at the corpus-wide price §5.1 measures.

## 8. Migration and compatibility (changelog house categories)

- **Newly rejected:** `requires`, `ensures`, `invariant` as identifiers, anywhere (Appendix B.1). Corpus impact: **one** test site, which flips to a negative test.
- **Newly defined:** nothing. No grammar production changes; no new token; no new diagnostic *rule* (the 0055 §3 note is an existing obligation restated).
- **Style, non-normative:** the §22.16 blank-line recommendation, the ten-site docs restyle, and `check_docs.sh` check 5. **No obligation on user code, no compiler behaviour, no program's meaning changed.**
- **Doc-only:** the rationale §3.1 third-category note; this proposal as the decline record.
- **Unchanged:** the grammar; #744's layout-free property; every parse of every existing program except one test's deliberate use of a now-reserved word.

## 9. The reopening bar

**Field evidence from external users that the seam confuses.** Concretely: reports, from people writing Bezel who did not write the compiler, of misreading where a clause region ends — an intended body statement written as a clause or vice versa, a review comment about it, a bug traced to it. Bezel has zero external users today, so no such evidence can exist yet, and its absence is *not* evidence of absence — which is precisely why the ruling declines to spend a keyword or a migration on a prediction.

Two clarifications on the bar's scope:

- **It does not admit re-litigating parse-significance.** The blank line is style permanently (§3.5, #744). A future body-token proposal is a *token* proposal; the layout route is closed.
- **It does admit the whole token space again, starting from §5.1's `do` versus `does` pair.** If the evidence arrives, the corpus will be larger and the migration more expensive — a cost this ruling knowingly accepts in exchange for not spending it now on a guess.

## 10. Interactions

- **[#744](https://github.com/s8lap/bezel-lang/issues/744)** (closed, completed) — the layout precedent this ruling preserves; §3.5.
- **Proposal 0055** (parenthesized contract clauses) — §3's disambiguation analysis is this proposal's §4.2 input, and §5.4's decline is a defence of 0055's structural-parentheses ruling against re-introducing a terminator.
- **The v0.246 Q2 one-spelling-per-site ruling** — the owner-ruled precedent §5.2 leans on for declining an optional token; proposal 0102 §2 records the same precedent for a different question, and the two should cite each other.
- **Rationale §3.1** — gains the third-category note (§4.3); the note is the only rationale edit this proposal makes.
- **Appendix B.2** — loses two rows (corrected in the graduation round: two rows — `requires`/`ensures` share one, as §4.1 and the landed edit both state); `old` and `result` keep theirs. `equality`, `serialized`, `polymorphic`, `env`, `symbol`, `variadic`, and the constraint words are untouched, and no other contextual keyword is re-examined here.
- **The compiler-side small records** (coordinator-filed): the unimplemented 0055 §3 diagnostic, and the two Pascal-`begin` test residues the study surfaced, ride their own bug issues rather than this proposal.
