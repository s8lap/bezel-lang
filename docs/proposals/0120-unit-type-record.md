# 0120 — The unit type, recorded: why `()` exists, where the no-value boundary sits, and the function-type-syntax question

**Status:** accepted — owner-directed 2026-08-26 ("write the rationale for the unit and land it"); graduation targeted at v0.283
**Tracking issue:** [#3381](https://github.com/s8lap/bezel-lang/issues/3381)
**Target module(s):** Docs-only. Rationale (one new §3 section), `DECLINED.md` (rows per §4), no spec text changes, no stdlib changes, no compiler work.
**Written against:** v0.282

## 1. Why this proposal exists

The 2026-08-26 unit-type investigation (three lanes: occurrence census, design space, keyword-syntax study — scratch records summarized here, evidence re-cited from the documents) found that **no record anywhere motivates the unit type's existence**: no rationale entry, no `DECLINED.md` row, no proposal. §4 defines it ("exactly one value, also written `()`"), §2.3 lists it as type-shaped syntax, and its justification is implicit at best (spec §27.1's method⇄function-type parallel). An unrecorded load-bearing decision is re-derived by every fresh reader and re-litigated forever — the exact failure mode the decline register exists to prevent. The owner asked the question directly ("do we need the unit type at all?"), which is the proof it recurs.

This proposal records the answer. It changes no language rule.

## 2. What the census found (the facts the record rests on)

All against v0.282; counts by `grep -F -- "-> ()"` and companions:

1. **Unit is nearly invisible in ordinary code.** A unit-returning method writes **no** return clause (§22.1: "no `:` clause for a procedure (return type `()`)"), so declarations never spell it. Zero real unit locals, fields, parameters, patterns, or choice payloads exist in the documents or the test corpus (choices use payload-free cases; the #3095-family regression tests exercise unit storage deliberately, as regressions).
2. **Three load-bearing homes.** (a) Function-type return position `(T) -> ()` — spec 24, stdlib 19, code-generation reference 12, tests 241. (b) **Generic instantiation at unit**: exactly one live client, the no-payload event family — `EventSource<()>` / `SharedEventSource<()>` / `ISharedEvent<()>` (stdlib §6.5, Guide 04), adopted consciously at proposal 0035 ("one token of honesty about what the payload always was"), with `Raise()` sugar for `Raise(())`. (c) The suppression idiom `catch Exception => ()` (§31.2.5).
3. **The quiet dependents.** §21.9's unused-result warning is keyed on non-`()` results; §27.3 classifies procedure lambdas by the `()` result; §32 admits `: ()` as an FFI return type mapped to C `void`; `where T: value` includes unit (§14.2). The compiler models unit as a real type (`TypeRef::Kind::Unit`) lowering to LLVM `void` in return position and an inert byte in storage.
4. **Bezel is deliberately hybrid about "no value".** At the *task* seam the language chose a dedicated non-generic type — `Task` beside `Task<T>`, non-generic `TaskCompletionSource` — and `Task<()>` appears nowhere outside one parser test. At the *event* seam it chose generic-instantiation-at-unit (`EventSource<()>`). Both answers are landed and neither was an accident.

## 3. The record (lands as rationale §3.15)

**Why unit exists.** Bezel has no tuples, so `()` is not a degenerate tuple — it is a freestanding built-in whose job is to make *"returns nothing"* a first-class instantiation and result like any other. That buys three things the census shows are real: a single function-type grammar (`(T) -> U` covers every arity of result, so higher-order code is never bifurcated — `method Retry<T>(op: () -> T): T` accepts a no-result operation at `T = ()` with no `Retry` twin, and the same holds for every future memoize/backoff/`Locked.With`-shaped combinator); a no-payload instantiation where a generic wrapper is thin (`EventSource<()>`); and a typed statement calculus (§21.9's warning, §27.3's classification, `catch … => ()`) that can *name* the no-value result instead of special-casing it.

**Where the hybrid boundary sits, and why it is principled.** A dedicated non-generic type is used where the no-value case differs *behaviorally* from the valued case — `Task` is not `Task<()>` because the task family's rules are arity-sensitive through and through (§30.5's Pattern A/B split, result lifetime, the await positions), so a shared generic would bury real rule differences under one head. Generic-instantiation-at-unit is used where the wrapper is *thin* — an event's payload flows through unchanged, no rule differs by arity, and the one-token instantiation is exactly as honest as the type is. The boundary rule, stated once: **a no-value variant earns a dedicated type when its rules differ from the valued case; when they do not, instantiate at `()`.**

**Why unit's near-invisibility is a feature, not an argument for removal.** The procedure form (no `:` clause) means programmers meet `()` only at the three places it carries information. Removing the type would not remove those places; it would re-spell them worse — a `void` pseudo-type bifurcates every higher-order signature (the C# lesson: `Action`/`Func` naming is avoidable with structural types, the *member* bifurcation is not), a non-generic event family duplicates a working one for zero behavioral difference, and the statement calculus would key on an untyped absence.

## 4. Declined directions (rows land in `DECLINED.md` at graduation)

1. **Removing the unit type in favor of a C#-style `void`** — `rejected`. The reasons above; the Retry-shaped member bifurcation is the operative cost. **Reopening bar:** evidence that unit's uniformity fails in practice — a real higher-order surface that unit-instantiation cannot serve, or measured confusion costs from `()` that the procedure form does not already hide.
2. **`Task<()>` unification / `EventSource` de-generification** (moving the hybrid boundary in either direction) — `rejected`; the boundary rule in §3 is the record. **Reopening bar:** a task-family rule shown to be arity-insensitive after all, or an event-family rule that becomes arity-sensitive.

## 5. The function-type-syntax direction (the owner's follow-on)

*The owner's framing, 2026-08-26: the `lambda` keyword removed punctuation-led lambda values; a keyword-led function TYPE could make the result clause elidable exactly as method declarations elide it — unit invisible in types — "then, perhaps, the single arrow `->` may go away too, because the spelling could already be unambiguous."*

**[PENDING — completed from the keyword-syntax study before graduation: candidate spellings, the colon-chain parse analysis (the lambda value form `lambda(params): T => body` already parses the colon result clause, with `=>` marking the value form), the parser-source LL-resolvability finding, the full-surface respelling samples, migration size, and the routing verdict — recommended / deferred-with-bar / declined-with-reasons.]**
