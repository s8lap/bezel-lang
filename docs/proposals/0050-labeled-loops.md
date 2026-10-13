# 0050 - Labeled `break` and `continue`

**Status:** graduated-v0.201 — superseded — graduated v0.201
**Tracking issue:** [#2037](https://github.com/s8lap/bezel-lang/issues/2037)
**Target:** Spec [§1.6](../Bezel_Language_Spec.md#sec-statements-and-termination) (two bullet clauses), [§21.3](../Bezel_Language_Spec.md#sec-the-while-statement)/[§21.4](../Bezel_Language_Spec.md#sec-the-for-statement) (one sentence each), [§21.5](../Bezel_Language_Spec.md#sec-break-and-continue) (rewritten; full proposed text in §2.2), [§29.2](../Bezel_Language_Spec.md#sec-desugaring-of-for) (label carry + edge-list generalization), Appendix [A.6](../Bezel_Language_Spec.md#sec-appendix-a-statements) (four production edits), Appendix [C](../Bezel_Language_Spec.md#sec-appendix-c) (invariant bullet). Rationale [§17.3](../Bezel_Design_Rationale.md#sec-goto) (amended into the decision record; the `goto` decline stands). **No stdlib change. No new keyword. No Appendix D change** (Core, see §4.12).
**Written against:** spec v0.197 (stdlib/rationale at the same revision)

This proposal expands item **P10** of [future-directions-2026-07-18.md](papers/future-directions-2026-07-18.md). Divergences from that sketch are recorded explicitly in §0.1.

## 0. Summary

Bezel today has no multi-level loop exit: [§21.5](../Bezel_Language_Spec.md#sec-break-and-continue) (v0.197) says *"There are no labeled loops and no labeled `break`/`continue`. For multi-level exits, extract into a method and use `return`, or use a boolean flag."* Rationale [§17.3](../Bezel_Design_Rationale.md#sec-goto) records the same posture as the answer to `goto`. Both remedies have real costs in a class-only language with no block expressions (rationale [§5.13](../Bezel_Design_Rationale.md#sec-no-block-expressions)): extraction moves a loop's logic away from its data purely for control-flow's sake, and the flag idiom smears one exit decision across a mutable local, every loop condition, and every subsequent statement.

The proposal: a `while`, `for`, or `await for` statement may carry an optional **loop label** — `scan: for var row in rows do … end scan;` — and `break scan;` / `continue scan;` target the labeled loop. The design is deliberately minimal, and most of it is *composition rather than new rules*:

- **Closers:** a labeled loop becomes a *named construct* under [§1.6](../Bezel_Language_Spec.md#sec-statements-and-termination)'s existing v0.170 umbrella rule ("`end` may be followed by the construct's name if it has one, or by its introducing keyword otherwise"), so `end scan;` works with **zero new closer rules** — and, as the same rule's exact text already decides, `end for;` stops applying to a loop that has a name (§2.3).
- **Disposal:** a labeled `break`/`continue` through N scopes runs each exited scope's obligations by the rules that already govern every exit edge ([§28.3](../Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows), [§28.10](../Bezel_Language_Spec.md#sec-using-var), the v0.182 uniform reverse-establishment ordering). **No disposal rule is added or altered** (§2.5).
- **Desugaring:** the label rides [§29.2](../Bezel_Language_Spec.md#sec-desugaring-of-for)'s for-desugaring onto the generated `while`, where the existing sentences about `break` and `continue` already say the right thing (§2.4).
- **`goto` stays declined:** labeled break is forward-only and structured — it cannot enter a scope, cannot form a loop, and names its target in the same body. Rationale §17.3's decline is amended, not reversed (§7.2).

### 0.1 Divergences from the P10 sketch

1. **`await for` is included.** P10 says "optional label on `for`/`while`". `await for` is the second alternative of the same A.6 `ForStmt` production, and `break`/`continue` already operate in its body ([§30.7](../Bezel_Language_Spec.md#sec-asynchronous-programming-async-iterators), [§29.2](../Bezel_Language_Spec.md#sec-desugaring-of-for)); excluding it would be an arbitrary hole with its own carve-out sentence. Owner question Q3.
2. **The `end for;`-on-a-labeled-loop consequence is decided, not glossed.** P10's "the closer rule composes as-is" is correct, and composing as-is *means* the keyword closer no longer applies once the loop has a name (§2.3). The sketch did not surface this; this proposal decides it in the strict direction and prices it. Owner question Q2.

Everything else follows the sketch's direction.

## 1. Problem and evidence

Current state, verified against v0.197:

- [§21.5](../Bezel_Language_Spec.md#sec-break-and-continue): `break` exits the immediately enclosing loop; the section's final paragraph declares labeled loops nonexistent and names the two workarounds.
- Rationale [§17.3](../Bezel_Design_Rationale.md#sec-goto) (`goto`): the entire entry is one sentence — "Multi-level loop exit is done by extracting a method and using `return`." The decline of `goto` is thus *load-bearing for a missing feature*: the section is titled `goto` but its content is the multi-level-exit workaround.
- P10's own framing supplies the adoption argument: labeled break is the middle ground every modern comparator chose (Rust, Kotlin, Java; Ada labels loops for its `exit` statement), it removes one of the two real extraction drivers named in the future-directions "deliberately not proposed" list (keeping rationale §5.13's block-expression decline stable), and it is forward-only and structured, sharing none of `goto`'s costs.
- The machinery it needs already exists: v0.170's one uniform closer rule (rationale [§3.11](../Bezel_Design_Rationale.md#sec-uniform-end-closer)) gives named constructs checked closers; v0.182 made cleanup ordering uniform across *every* exit edge including `break`/`continue` ([§28.10](../Bezel_Language_Spec.md#sec-using-var)); v0.182 also phrased the `finally` restriction in terms of "the target loop" ([§21.5](../Bezel_Language_Spec.md#sec-break-and-continue), [§31.2](../Bezel_Language_Spec.md#sec-try-catch-finally)) — wording that generalizes to labeled targets verbatim.

The evidence that the workarounds are the pain, not the feature: the extraction idiom is exactly what P10 calls "moving the loop's logic away from its data for control-flow's sake," and the flag idiom turns one static jump into dynamic state that every reader must re-derive. Both defeat the repo's own regeneration-era preference for locally-checkable meaning.

## 2. The design

### 2.1 Terminology

[§1.6](../Bezel_Language_Spec.md#sec-statements-and-termination) already uses "label" for the optional token after `end` (the *closer label*). This proposal introduces the **loop label** — the `name :` prefix — and keeps §1.6's usage untouched. The two words stay distinct everywhere in the proposed text; a loop label is the construct's *name*, which its closer label may repeat.

### 2.2 Proposed §21.5, in full

The following replaces §21.5's current three paragraphs (the first two survive with the shown edits; the "no labeled loops" paragraph is deleted).

> ### 21.5 `break` and `continue`
>
> `break` exits a loop; `continue` skips the rest of the current iteration and advances to the next. The unlabeled forms target the immediately enclosing loop. Neither is legal outside a loop; doing so is a compile error.
>
> **Loop labels.** A `while`, `for`, or `await for` statement may be prefixed with a *loop label* — an identifier followed by `:` ([§21.3](#sec-the-while-statement), [§21.4](#sec-the-for-statement), grammar A.6). Only loop statements take a loop label. The label names the loop: a labeled loop is a *named construct* in the sense of [§1.6](#sec-statements-and-termination), so its `end` may repeat the loop label (`end scan;`); because the construct now has a name, the introducing-keyword closer (`end for;`, `end while;`) applies only to unlabeled loops, and bare `end;` remains valid everywhere.
>
> ```bezel
> scan: for var row in rows do
>   for var cell in row do
>     if cell.Bad then break scan; end;
>   end;
> end scan;
> ```
>
> **Labeled forms.** `break name;` exits the enclosing loop labeled `name`; `continue name;` skips the rest of that loop's current iteration and advances it. `name` must be the loop label of a **lexically enclosing** loop statement in the **same callable body**; anything else is a compile error naming the labels in scope. The labeled forms are legal exactly where the unlabeled forms are — the label changes which loop is targeted, never where the statement may appear.
>
> After `break name;`, execution continues at the first statement after the labeled loop. `continue name;` transfers control to the labeled loop's *advance point*: for `while`, the condition; for `for` and `await for`, the iteration-advance of the [§29.2](#sec-desugaring-of-for) desugaring — the next `MoveNext()`/`MoveNextAsync()` call on the enumerator paths, and the index/step increment (which then falls into the bound check) on the direct-indexed and counted forms. A `continue`, labeled or not, never re-runs the source expression or the enumerator acquisition, and never skips an advance.
>
> On the way to its target, the jump exits every intervening scope — inner loop bodies, `do … end` blocks, `try` bodies — and each performs its ordinary exit obligations ([§28.3](#sec-local-variables-ownership-and-borrows), [§28.10](#sec-using-var), [§31.2](#sec-try-catch-finally)): `using var`/`await using var` disposals and pending `finally` blocks run in reverse order of establishment (v0.182), and the jump is a body-exit edge of every loop body it exits, so each such body's iteration variable is subject to its own [§29.2](#sec-desugaring-of-for)/[§28.3](#sec-local-variables-ownership-and-borrows) discharge rule at the jump site. The labeled forms add or alter no disposal rule.
>
> **Label scope, namespace, and duplicates.** A loop label is in scope throughout its loop statement and lives in a namespace of its own: it is never resolved against — and never collides with — variables, parameters, types, or members, and it is not an expression (a local `scan` and a label `scan` may coexist; `nameof` does not accept a label). A loop label that repeats a label already in scope at its declaration — the label of an enclosing labeled loop in the same callable body — is a compile error at the inner label: labels do not shadow. Loops that do not enclose one another may reuse a label freely; their labels are never simultaneously in scope. `_` is an ordinary identifier in label position ([§1.3](#sec-identifiers)); it gains no discard meaning here ([§17.13](#sec-the-discard-_)'s three-position list is unchanged).
>
> **Callable boundary.** The enclosing-loop search — for the labeled and unlabeled forms alike — is confined to the callable ([§21.7](#sec-the-return-statement)'s list: method, constructor, destructor, lambda, property accessor, indexer accessor) whose body contains the statement. A `break` or `continue` inside a lambda cannot target a loop of the defining method, labeled or not; a compile error. A loop label is correspondingly visible only within its own callable body: a loop inside a lambda may reuse an enclosing method's label without conflict, since the outer label is not in scope there.
>
> Neither may exit a `finally` block (v0.182): inside a `finally`, a `break` or `continue` — labeled or unlabeled — whose target loop lies **outside** the `finally` is a compile error — the same rule as `return` in a `finally` ([§21.7](#sec-the-return-statement), [§31.2](#sec-try-catch-finally)). When the `finally` runs during exception propagation, such a jump would abandon the in-flight exception silently. Loops wholly inside the `finally`, and jumps among them, are unaffected.

Two notes on that text. The `finally` paragraph is the current v0.182 paragraph with "— labeled or unlabeled —" inserted: the existing wording already keys on *the target loop*, so labels widen which loop that can be and change nothing else. The callable-boundary paragraph makes explicit what v0.197 leaves implicit (§21.5 restricts `return`-in-lambda explicitly via §21.7 but is silent on `break`-in-lambda); this is a clarification codified, not a behavior change — if the reference compiler accepts a loop-crossing `break` in a lambda today, that is a bug to file at landing.

### 2.3 Closers: aligning with §1.6's exact text

§1.6's umbrella rule is untouched: *"`end` may be followed by the construct's name if it has one, or by its introducing keyword otherwise."* A labeled loop **has a name**, so the name branch applies and the keyword branch — which the rule grants only "otherwise" — does not. Consequences, decided here:

- `end scan;` — valid on the loop labeled `scan`; mismatch is a compile error (§1.6's existing check).
- `end;` — always valid (§1.6's existing floor).
- `end for;` on a loop labeled `scan` — **a compile error**, with the diagnostic required to name the fix ("this loop is named `scan`; close with `end scan;` or `end;`"). This is the strict reading of §1.6, and it keeps the revision at literally zero new closer rules: one construct, one valid closer label, exactly the shape rationale [§3.11](../Bezel_Design_Rationale.md#sec-uniform-end-closer) adopted in v0.170. The alternative — accepting both — is declined in §7.6 and offered as owner question Q2.
- The editing consequence is priced honestly: adding `scan:` to an existing loop closed `end for;` requires touching the closer too. The error is immediate, local, and self-explanatory; no silent behavior change is possible.

Concrete §1.6 deltas (both bullets; the umbrella sentence is untouched):

- *Named constructs* bullet: append "; labeled loop statements ([§21.5](#sec-break-and-continue)) — the loop label is the construct's name: `end scan;`".
- *Keyword-labelled constructs* bullet: after the closer list, append "(`end while;`/`end for;` apply to **unlabeled** loops; a loop carrying a loop label is a named construct — previous bullet — per the umbrella rule's 'otherwise')".

### 2.4 Desugaring: the label survives §29.2

The loop label carries onto the generated `while` of [§29.2](../Bezel_Language_Spec.md#sec-desugaring-of-for)'s desugaring — not onto the wrapper `do` block (which, per this design, cannot be labeled):

```bezel
scan: for var x in source do
  // body
end scan;
```

desugars (borrowing enumerator path shown; the owning and async paths are identical in shape) to:

```bezel
do
  using var __source := source;
  using var __enumerator := __source.GetEnumerator();
  scan: while __enumerator.MoveNext() do
    var x := __enumerator.Current;
    // body
  end;
end;
```

- `break scan;` exits the labeled `while`; control then reaches the wrapper block's end, disposing `__enumerator` and `__source` — the very path §29.2's existing sentence describes for unlabeled `break` ("`break` exits the surrounding `do ... end`, triggering disposal of both"). Observably, execution continues after the original labeled `for` statement.
- `continue scan;` jumps to the labeled `while`'s condition — the next `MoveNext()` call. For a `for`, the advance *is* the condition on the enumerator paths, so "continue targets the advance" and "continue targets the condition" coincide here. On the direct-indexed and counted forms (`Range<T>` counted loops per [§16.5](../Bezel_Language_Spec.md#sec-iteration), the inline-array/span direct loops of §29.2), the advance is the index/step increment followed by the bound check, and `continue scan;` targets the **increment** — targeting the bare condition would skip an advance and diverge from the enumerator desugaring, which the "observable behavior is identical" clause already forbids. The proposed §29.2 text states this explicitly rather than leaving it to the equivalence clause.
- The desugaring composes for nesting: an inner `for`'s wrapper blocks lie between a `break scan;` and the labeled `while`, and are exited with their ordinary scope-exit disposals — the desugared program is itself a labeled-loops program, and no rule beyond §2.2's crossing rule is consulted.
- §29.2's body-exit edge lists ("fall-through to the next iteration, `break`, `continue`, `return`, `throw`", and the owning-path list under *Iterator yield ownership*) are generalized by one clause: a labeled `break`/`continue` is such an edge **for every loop body it exits** — so an owning-path iteration variable in each exited body is discharged or auto-dropped (with the v0.150 must-use warning) at the jump, exactly as at an unlabeled `break` of that body.

### 2.5 Disposal across a multi-level exit: no new rules, worked

Every API below is verified current: `StringBuilder()` / `Append(string)` / `ToString()` (stdlib §3.1), `File.ReadAllLines(path): owned Array<string>` (stdlib §7.3), `string.Contains(string)` (stdlib §9.6), `Console.WriteLine` (stdlib §2.1).

```bezel
public method DigestUntilStop(paths: Array<string>)
  scan: for var path in paths do
    using var sb := new StringBuilder();
    for var line in File.ReadAllLines(path) do
      if line.Contains("STOP") then
        break scan;                    // exits both loops
      end;
      sb.Append(line);
    end;
    Console.WriteLine(sb.ToString());
  end scan;
end DigestUntilStop;
```

On the `break scan;` edge, in reverse order of establishment (the v0.182 uniform ordering, [§28.10](../Bezel_Language_Spec.md#sec-using-var)):

1. The inner loop's desugared `__enumerator` is disposed, then its `__source` — the fresh owned `Array<string>` produced by `File.ReadAllLines(path)`, whose disposal on early exit is exactly the leak §29.2's wrapper `using var` exists to close. (`line` itself is a `string` — runtime-managed, no obligation.)
2. `sb` is disposed — the user-written `using var` in an exited scope, discharged by ordinary block-exit disposal ([§28.3](../Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows), [§28.10](../Bezel_Language_Spec.md#sec-using-var)); the pending `Console.WriteLine` for that file simply never runs.
3. The outer loop's desugared `__enumerator` is disposed; its `__source` binding is a no-op pin (`paths` is a borrowed local, §29.2's borrowed-source case).
4. Control reaches the statement after `end scan;`.

An exception thrown at the same point runs disposals 1–3 identically ([§28.3](../Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) *Exception-path discharge*). That equivalence is the proof of "no new rules": the labeled break reuses the multi-scope unwind edge the language already defines for `return`, `throw`, and unlabeled `break` — it only starts it from a farther target.

## 3. Grammar delta (Appendix A.6)

```
LoopLabel       ::= Identifier ":"

WhileStmt       ::= LoopLabel? "while" Expression "do" Statement*
                    "end" ("while" | Identifier)? ";"

ForStmt         ::= LoopLabel? "for" "var" Identifier (":" Type)? "in" Expression
                    "do" Statement* "end" ("for" | Identifier)? ";"
                  | LoopLabel? "await" "for" "var" Identifier (":" Type)? "in" Expression
                    ("with" Expression)? "do" Statement* "end" ("for" | Identifier)? ";"

BreakStmt       ::= "break" Identifier? ";"
ContinueStmt    ::= "continue" Identifier? ";"
```

Comment for the closer alternatives, in A.6's house over-admit-and-reject-semantically style: the `Identifier` closer is admitted grammatically on every loop and checked per §1.6 — it must equal the loop's label; on an unlabeled loop any identifier closer is rejected, and on a labeled loop the keyword closer is rejected (the construct is named). `DoEndBlock`, `IfStmt`, `TryStmt`, and the `Statement` production itself are untouched; a labeled loop stands wherever a loop statement stands today (including as a `case` arm's single statement).

**No parse ambiguity — the argument.** The statements of A.6 that can begin with an `Identifier`-shaped token are `AssignmentStmt` and `IncrementStmt` (via `LValue`), `ExpressionStmt`, and `DiscardStmt` (`_` is lexically an identifier, [§1.3](../Bezel_Language_Spec.md#sec-identifiers)). In every one, the token immediately after the leading identifier is drawn from: `.`, `[`, `(`, an operator token, `++`/`--`, an `AssignOp` (`:=`, `+=`, …), `=>` (a bare-parameter lambda), or `;` — never a bare `:`. Note the lexical point: `:=` is a single lexeme under maximal munch, so `x := 1;` contains no `:` token, while `scan: for` and `scan:for` both lex as `Identifier`, `:`, `for`. Every other colon in the grammar (type ascriptions after `var` and in parameter lists, `DictEntry` inside `[…]`, catch/case `var x: T` bindings, interpolation format specs) occurs inside a construct introduced by an earlier token that is not a statement-leading identifier. `Identifier ":"` at statement start is therefore a guaranteed parse error in v0.197, and the new prefix collides with nothing. Parsing needs two tokens of lookahead at statement start; after consuming `Identifier ":"`, the next token must be `for`, `while`, or `await` (followed by `for`) — anything else gets the diagnostic "a loop label may prefix only a loop statement". Existing programs cannot change meaning (§5).

**Appendix C delta.** Replace the current bullet ("`break` and `continue` appear only inside loops, and never exit a `finally` block …") with:

- `break` and `continue` appear only inside loops of the same callable body; a labeled form names the loop label of a lexically enclosing loop statement of that body; neither exits a `finally` block (the target loop — labeled or unlabeled — must not enclose the `finally`; [§21.5](#sec-break-and-continue), [§31.2](#sec-try-catch-finally)).
- A loop label appears only on `while`/`for`/`await for` statements, duplicates no loop label in scope at its declaration, and — when the loop's `end` carries a closer label — matches it per [§1.6](#sec-statements-and-termination): the loop label if the loop has one, the introducing keyword otherwise.

## 4. Interactions

1. **§1.6 closers** — the core composition of the proposal (§2.3). One real edit hazard: labeling a loop currently closed `end for;` forces a closer edit; the mismatch diagnostic names the fix.
2. **§21.6 `do … end` blocks** — not labelable (defense in §7.5); crossed by a labeled jump like any block, running their `using var` disposals. §21.6's note that loop `do` is part of loop syntax is unaffected.
3. **§21.2 `if` / §19 `case` and `when` statement arms** — not labelable; positional legality is untouched: `break`/`continue` (labeled or not) are admissible wherever a statement inside a loop is — `then`/`else` branches, statement-position arm bodies (A.7's `ArmBody ::= … | Statement | "do" Statement* "end"` derives `BreakStmt`), never expression-position arms (no statements there) — and a labeled jump crosses these constructs freely (an arm's `do … end` body is a block; its locals and `using var`s dispose on the crossing per item 2; [§19.2](../Bezel_Language_Spec.md#sec-arms)). The v0.197 test corpus has no break-inside-arm case either way; a spec-derived test should pin it at landing.
4. **§31.2 `try`/`catch`/`finally`** — a labeled jump out of a protected block runs the pending `finally` blocks it crosses, in establishment order with the crossed disposals: §31.2's v0.182 sentence already says "exited by … a `break` or `continue` targeting an enclosing loop". The in-`finally` restriction composes verbatim (§2.2); jumps from `catch` arm bodies remain legal.
5. **§29.2 + owned iteration variables (§28.3)** — the label rides the desugaring (§2.4); each exited loop body is a body-exit edge for its own iteration variable's discharge rule, so the owning path's auto-drop-with-warning behaves at `break scan;` exactly as at an unlabeled `break` written at the same spot in each body. The `using var c := owned x;` adopt-on-entry idiom §29.2 teaches for conditional `break` covers labeled breaks unchanged.
6. **Iterators (§29.3–§29.7)** — labels are usable inside iterator bodies; an iterator body is a method body, so no label can cross into or out of it (callable rule). `yield return` inside a labeled loop, and early-abandonment disposal of a partially-consumed iterator ([§29.7](../Bezel_Language_Spec.md#sec-disposal-of-partially-consumed-iterators)), are orthogonal.
7. **§30.7 `await for`** — labelable (divergence 0.1.1). A labeled break crossing an `await for` exits it on the same normal edge as an unlabeled break of that loop: the desugared `await using` enumerator disposal awaits `DisposeAsync()` under the async driver, per §30.7's existing normal-path rule. The `with token` clause and the v0.184 across-`yield` restrictions are untouched.
8. **Async methods generally (§30.1)** — none: labels are not values; a labeled jump is intra-body control flow and cannot cross an `await` boundary any differently than unlabeled `break` already does (the state machine already lowers loops containing `await`).
9. **§28.14 scoped/captures, §17.9 `nameof`, §17.10 `typeof`** — none: a label cannot be captured, stored, passed, or named; it lives outside the expression language entirely (own namespace, §2.2).
10. **Appendix B** — no new keyword; a loop label is an `Identifier`, so reserved words (including `end`) cannot be labels, lexically.
11. **§1.3 `_`** — `_` stays an ordinary identifier in label position; §17.13's three-position discard list does not grow (§2.2).
12. **Appendix D** — no edit. Statements are Core; the labeling of an `await for` is exercisable only by implementations of the Asynchronous-programming feature, which follows from the construct it labels, not from a new D.2 row.
13. **Diagnostics** — the reject matrix a lit track should pin: unknown label (naming labels in scope), duplicate visible label, label on a non-loop statement, cross-callable target, in-`finally` crossing target, closer/label mismatch in both directions (`end for;` on labeled; `end scan;` on unlabeled or wrong-named).

## 5. Migration and compatibility

**Purely additive.** No token sequence of any valid v0.197 program parses as a loop label (§3's argument), no existing construct's semantics are touched, and the unlabeled forms and their closers behave identically on unlabeled loops. Stdlib: zero changes. Guides: no edits required (a labeled-break example is optional guide material at some later revision). Conformance corpus: new cases only, no existing case's expected outcome changes. There is no deprecation, no warning period, and nothing to migrate; the only behavioral deltas are programs that do not compile today beginning to compile (labeled forms) and one new *class* of error text (the reject matrix, §4.13). The compiler may lag the spec revision under the established spec-ahead-of-impl pattern, with the implementation issue filed at landing.

## 6. Staging

1. **Docs train (one revision):** spec §1.6 (two bullet clauses), §21.3/§21.4 (one label sentence each, pointing at §21.5), §21.5 (the §2.2 text), §29.2 (label carry, advance-point sentence, edge-list clause), A.6 (§3), Appendix C (§3); rationale §17.3 amended into the decision record (goto stays declined; labeled break adopted; extraction remains good style where the inner logic deserves a name) — plus the changelog entry. All edits are small and this proposal contains their text or a precise description; graduation should be near-mechanical.
2. **Compiler:** lexer — none (`:` exists). Parser — two-token lookahead at statement start; label fields on the loop AST nodes; closer-label check extension. Sema — a per-callable label stack with the duplicate/unknown/boundary/`finally` checks (the `finally` check extends the existing v0.182 one by target instead of by nesting depth 1). Codegen — labeled `break`/`continue` lower to branches to the already-existing per-scope cleanup chains; `return` already exits N scopes running disposals in order, so the unwind edge machinery exists and gains only new branch targets.
3. **Tests:** spec-derived gtests — the §2.2 example, the §2.5 disposal-order trace asserted at runtime (disposal order observable via destructor side effects), owning-path discharge at a labeled break, counted-loop `continue` hitting the increment (a `step` loop that would diverge if `continue` skipped the advance); lit/FileCheck — the §4.13 reject matrix. Conformance cases regenerate from the new spec examples at graduation.

## 7. Alternatives considered and declined

1. **Numeric break levels** (`break 2;`) — declined. The target is positional, not named: inserting or deleting an intermediate loop silently retargets every numeric break it encloses — precisely the fragile-under-edit property this repo's AI-regeneration lens penalizes hardest. A name survives restructuring or fails loudly.
2. **`goto`** — remains declined; rationale §17.3 stands and becomes this decision's record. Labeled break keeps the structured subset of goto's power: forward-only, cannot jump into a scope, cannot form a loop, target declared on a construct the jump is inside. Every scope-exit obligation remains statically enumerable, which is why §2.5 needs no new disposal rules — a general `goto` would have needed them.
3. **Labeled block as expression / break-with-value** — out of scope, per P10's own framing. It would reopen rationale [§5.13](../Bezel_Design_Rationale.md#sec-no-block-expressions)'s block-expression decline; P1 (`choice` sums) and this proposal together relieve the two real pressures on that decline without touching it.
4. **Label any statement** (Java's generality) — declined. `break`/`continue` target only loops, so labels on non-loops are dead weight the compiler must then warn about; Java's own labeled-arbitrary-statement form is a vestige. Narrowest surface that covers the motivation.
5. **Labelable `do … end` blocks with `break label;` as block-exit** — declined for this proposal. That is *new* control flow (early block exit exists nowhere today), not a naming of existing control flow, and it is the first step of alternative 3. Additively revisitable later if evidence arrives; nothing here forecloses it.
6. **Accept both `end scan;` and `end for;` on a labeled loop** — declined (owner may override, Q2). It requires amending §1.6's umbrella rule with a two-valid-closers carve-out for exactly one construct family, forfeiting the "zero new closer rules" property, and it weakens the closer as documentation: `end for;` on a 60-line labeled loop tells the reader less than the name the loop already has.
7. **A sigil form** (Rust's `'scan`) — declined. Bezel's lexicon has no sigil precedent; `name :` is the Ada/Java-family spelling and reads as a declaration, which it is.

## 8. Open questions / owner decisions

- **Q1 — adopt?** Recommended: yes. Size S; the design is mostly composition of v0.170/v0.182 machinery, and §2.5 demonstrates the ownership story needs zero new rules.
- **Q2 — closer strictness on labeled loops** (§2.3): confirm that `end for;` is rejected once the loop is labeled (recommended — §1.6's exact text, zero new closer rules), or direct the two-closers carve-out of §7.6.
- **Q3 — `await for` inclusion** (divergence 0.1.1): confirm labeling the `await for` alternative (recommended — same production, same body rules), or restrict to `for`/`while` per the literal P10 sketch with a recorded carve-out.
- **Q4 — the lambda-boundary clarification** (§2.2): confirm folding the explicit callable-boundary sentence for *unlabeled* `break`/`continue` into the same revision (recommended — the labeled rule cannot be stated without it), and whether to pre-file the compiler-behavior check as its own issue at landing.
- **Q5 — unused labels**: this proposal deliberately specifies **no** unused-label warning — a label with no `break`/`continue` still buys a compiler-checked closer on a long loop, the same readability service §1.6 assigns closer labels. Confirm, or direct a lint.

## Recommendation

**Adopt.** The feature is the recorded middle ground of every comparator language; the two workarounds the spec currently prescribes are the extraction driver P10 names and a flag idiom that trades one static jump for mutable state; and the design costs are almost entirely already paid — v0.170 bought the closers, v0.182 bought the uniform exit edges, and §29.2's desugaring carries the label with two sentences. The one genuinely new obligation (the label table and its reject matrix) is small, local, and fully enumerated above.
