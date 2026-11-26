# Proposal 0219 — The fresh-store rule at the many-element forms

| | |
|---|---|
| **Status** | under-review — draft (2026-11-26; #7426's spec half; recommends option (b), the extension at the argument's own shape, with option (a), keeping proposal 0207's scope as landed, argued in full in §4.1; five owner questions in §12) |
| **Tracking issue** | [#7426](https://github.com/s8lap/bezel-lang/issues/7426) (`spec-question`, `campaign:collection-elements`; filed at the v0.346 landing of [proposal 0207](0207-fresh-store-into-a-borrowing-collection-is-an-error.md)); the compiler half of the same issue (question 1) is a bug under either option (§3); [#3047](https://github.com/s8lap/bezel-lang/issues/3047) (0207's tracker) is where the compiler arms would go if (b) is accepted |
| **Target module(s)** | **spec**: §15.4 (the store paragraph: the position sentence, one new sentence on the many-element forms, one remedy sentence), §28.11 (*Borrowing collections* clause 4; *Value-copy members*, one sentence), K8.4 (the exception sentence), Appendix C.3 (one bullet). **stdlib**: §4 (*Value-copy members are stores*, one sentence), §4.4, §4.6, §4.10 and §4.15 (one sentence each). **rationale**: §5.28.6 (one paragraph). **code-generation reference**: §4.5 (one paragraph), §7.2 (two callouts). **proposals**: `README.md` (this row), `DECLINED.md` (three rows). **guides**: none. **compiler**: spec-ahead-of-impl, one tracker (§11). **conformance**: no banked case changes expectation; nine proposed cases (§10). |
| **Written against** | spec/stdlib **v0.360** at `bd12fe038` (`origin/main`). The v0.361 train (branch `spec/v0361-2026-11` at `6c125097d`) was diffed against every paragraph this proposal edits and touches none of them; its §6.x generic-inference sentence on literal arguments and its Appendix C.3 edit to the 0205 bullet are neighbours, not overlaps. Every quotation below is v0.360 text. |
| **Origin** | #7426, filed by the v0.346 composition from lane G's critique; triage for v0.361 (`triage.tsv` row 51) classed it DESIGN: "Extending the fresh-store error to many-element forms reverses proposal 0207's recorded scope … The checked-build non-fault of AddRange is a separate compiler bug under §28.11 value-copy." |
| **Change class** | Design change ([`spec-change-process.md`](spec-change-process.md) §2): option (b) narrows an admission K8.4 states (a plain parameter announces no retention, with the single-element stores as the only exception) and moves the line proposal 0207 drew. §1.3 says what the §6 reopening bar asks here and whether the issue meets it. |

*Conventions. Unprefixed § references are language-spec sections at v0.360; "stdlib §" is the standard library; K tags are §28.3's kernel ids. MEASURED marks a verdict taken with `bezelc 0.0.0-8ccf7ff0` (the r42 release build) in both disposal modes, programs and logs under `/var/tmp/bezel-scratch/v361/p-0219/` (probe ids `p01`–`p24`); QUOTED marks repository text; HYPOTHESIS marks a reading or a judgement. Every program uses only members stdlib §4 declares (`Array<T>`, `ArraySpan<T>`, `Set<T>`, `LinkedList<T>`, `OwningArray<T>`, `OwningSet<T>`); nothing is invented and nothing is proposed as a new name.*

---

## 0. Summary

Proposal 0207 (v0.346) made a fresh unbound owned temporary of class type in a single-element store position of a borrowing collection a compile error: `ws.Add(new Widget("a"))` is rejected because the argument dies at the end of the statement while the slot keeps a reference to it. It left the many-element forms outside the rule. #7426 asks whether three of them should follow:

```bezel
ws.AddRange(LoadAll());                  // LoadAll(): owned OwningArray<Widget> — the array and its elements die at the statement's end
ws.AsSpan().Fill(new Widget("f"));       // every slot of ws borrows one Widget that dies at the statement's end
var a := new Array<Widget>(3, new Widget("f"));   // three slots, one dead Widget
```

Each is the dangle 0207 rejects, with certainty, at every slot the call writes. Today all three compile with no diagnostic (MEASURED). Under the spec as written, `Fill` and the fill constructor are admitted (K8.4's plain-parameter sentence, whose one exception is the single-element stores), and `AddRange` of a fresh owning collection is admitted by one reading of the text and rejected by another (§2.3).

The record. 0207 placed the many-element forms outside its rule without a reason beyond "not single-element stores", pointed to its §9 for "the one open question about them", and the v0.346 changelog filed this issue as "extend it or keep them at the backstop". `DECLINED.md` has no row for them. So there is no recorded reason for the issue to answer, and the issue brings no new evidence: its argument is 0207's own, and its one measurement was already in 0207 §9 (§1.3).

The options (§4): (a) keep 0207 as landed, closing the text's ambiguity in the admitting direction; (b) extend the rule to the positions where a standard value-copy member places a fresh value itself, and to the many-element forms whose argument is a fresh owning collection; (c) extend by source set, so that frames (an iterator call, a lazy query over a temporary) are reached too; (d) a warning at those positions.

Recommendation: (b). It closes the three shapes at zero measured migration with three small compiler arms, removes a lifetime inversion the spec otherwise keeps (`ws.AddRange(loc)` of a nested-block local is rejected while `ws.AddRange(LoadAll())`, the narrower scope, is admitted: MEASURED `p06` vs `p01`), and lets the position set be stated structurally from §28.11's value-copy register instead of as a list. (a) is a coherent alternative and §4.1 states its case: the extension has none of the field record 0207 relied on, it guards shapes no program in the tree writes, and the frame neighbours stay admitted whichever line is drawn. (c) is declined because frames need the per-iterator yield summaries the record has deferred (`DECLINED.md` rows 634, 733, 950). (d) is declined on 0207's own reason for not keeping a certain dangle at a warning.

The compiler half of #7426 (question 1) is answered: the checked build does not fault because the temporary is never disposed. The argument to `AddRange`, `Fill` and the fill constructor is leaked, not freed with its flag unread (MEASURED, §3). That is a bug under every option.

---

## 1. The question and the record

### 1.1 What 0207 decided

QUOTED, 0207 §3 *Position*: "The many-element forms (`AddRange`, `UnionWith`, `CopyFrom`) are not single-element stores and are outside the rule (§9 records the one open question about them)." 0207 §4.3, the list of what stays accepted: "a named holder, a parameter, a field read, an iteration variable, a value-typed or `string` element, an owning-family store, a user-declared callee, a many-element form — each is outside the predicate, as the lint's negative controls pin today (`fresh_store_warning_3364`'s six "does not warn" rows), and each stays accepted." 0207 §9's only mention is a compiler divergence, not a design question: "`ws.AddRange(Make())` over a fresh `OwningArray<Widget>` compiles silently and does not fault in a checked build (`g04` — the elements are not disposed at the boundary, or the flag is not read)."

The landed text follows: §15.4's rule says "in a single-element store position of a borrowing-family collection", and K8.4's exception sentence names "The single-element store positions of the standard borrowing-family collections, and `OwningDictionary<K, V>`'s key".

### 1.2 What 0207 left open

QUOTED, the v0.346 changelog, *Filed*: "[#7426] (`spec-question`): the many-element forms — `AddRange` of a fresh owning temporary, `ArraySpan<T>.Fill` and `Array<T>(size, fill)` with a fresh class value — are outside 0207's single-element rule; extend it or keep them at the backstop (and the checked build does not fault today)." `DECLINED.md` has rows for 0207's options (a), (c) and (d) (rows 1087–1089) and annotations on rows 68, 408, 467 and 837; none concerns the many-element forms. Rationale §5.28.6's promotion paragraph does not mention them. The lint's negative controls in `tests/ownership/fresh_store_warning_3364_test.cpp` include no many-element row (`git grep AddRange|Fill|UnionWith` over the file: no hit).

### 1.3 Does #7426 meet the reopening bar?

Spec-change-process §2 and §6: a recorded decision is reopened "only with *new evidence against the stated reasons*, never by re-asking".

Stated plainly: #7426 does not supply new evidence in that sense. Its argument ("the certain-dangle argument applies to a many-element store …") is 0207's §2 argument applied to more positions, and its one measurement (`ws.AddRange(Make())` compiling silently and not faulting) is 0207 §9's `g04`.

But the bar has nothing to bite on, because no reason was stated. 0207 excluded the forms by definition ("not single-element stores"), took its predicate from the lint, and recorded the question as open. The graduation filed it as a question with two answers. Extending the rule therefore reverses no decline. It decides a question the v0.346 record left open. It is still a design change, because it narrows an admission the landed K8.4 text states.

One consideration that 0207 did record bears on scope, and it is the strongest argument for keeping the line. 0207's case against the v0.238 decline rested partly on the lint's track record at exactly its predicate: "The error takes that predicate unchanged" (0207 §6). No lint ever ran at the many-element positions, so the extension cannot cite such a record. §4.1 carries that argument.

What this proposal adds beyond the issue is measurement, not argument: the lifetime inversion in the many-element store (§2.2, `p06` vs `p01`); the frame shapes that a producer-shaped predicate misses, one of which segfaults in an unchecked build (`p21`); the answer to question 1 (§3); and a reach of zero (§6).

---

## 2. The text today, traced

### 2.1 The three repros

Declarations, used by every probe: `Widget` has a destructor that prints `dtor <name>` (non-trivial-dispose); `Box` has none (trivial-dispose); `LoadAll(): owned OwningArray<Widget>` builds one fresh `Widget("x")` into a fresh owning array and returns it.

**R1: `ws.AddRange(LoadAll()); ws[0].Name`** (`p01`, the issue's program).

- *Producer.* `LoadAll()` is an `owned`-returning call result used only as a non-`owned` argument: an unbound owned temporary (§28.3.11). K8.1 disposes it at the end of the statement, and an owning collection's disposal "disposes every element and then frees the backing storage" (§28.11 *Owning collections*).
- *Copy.* `AddRange(items: IEnumerable<T>)` is §28.11 value-copy shape (ii): "a method of a borrowing-family collection … that declares no return copies into its receiver the values it takes from a parameter declared `IEnumerable<T>`". "A copy is judged as a store of each placed value into the destination, at the call", and "a value from the receiver or from a collection or span argument carries what an element read from it carries". An element read from an owning collection is a borrow "whose source is the collection" (§28.11 *Owning collections*). So each placed value is a borrow sourced at the temporary.
- *Store check.* §28.11's clauses 1–3 judge borrows of parameters, receivers, fields and scope-bound bindings. A temporary is none of these. Clause 4 judges "a fresh unbound owned temporary … in a single-element store position". `AddRange` is not one, and the placed values are borrows of the temporary, not the temporary itself.
- *Kernel.* K8.4: "a borrow or frame that would escape [the boundary] is rejected … The escapes rejected are every escape K2.3 can compare — among them a binding, store … of the borrow in the producing body — and every retention a declaration announces … A plain parameter announces none: a callee that stores it holds the borrow under … the borrowing-collection store rule's recorded residual … The single-element store positions … are the exception".
- *Verdict.* Two readings, §2.3.
- *After the statement.* Under either reading the slot holds a borrow of a disposed `Widget`. A checked build faults at `ws[0].Name`; an unchecked build reads freed memory. Every path does this. The only exception is an empty `LoadAll()` result, where the call places nothing and is dead code.

**R2: `ws.AsSpan().Fill(new Widget("f")); ws[0].Name`** with `ws` non-empty (`p04`; `p16` binds the span first).

- *Producer.* `new Widget("f")` is an unbound owned temporary at a non-`owned` argument (§28.3.11), disposed at the statement's end.
- *Copy.* `ArraySpan<T>.Fill(value: T)` is shape (ii) through a span view, and "a span receiver's destination being the buffer or array it views" (§28.11). Stdlib §4.15: "For class T, the assigned-into slot stores a borrow (`Fill(myClass)` makes every slot borrow `myClass`)". "A value from a plain argument carries that argument's" sources.
- *Store check.* Clause 4 names single-element positions only. §15.4's position list is "the single-element stores of §28.11's value-copy shape (ii) … and the indexer-set … on every collection of the borrowing family". `Fill` is in neither.
- *Kernel.* `value` is a plain parameter, and K8.4's exception does not name `Fill`. So the plain-parameter sentence governs: admitted under the residual, the temporary still disposed at its boundary.
- *Verdict.* Admitted. After the statement, every slot of the span's range borrows one disposed `Widget`. The only exception is an empty span, which is dead code.

**R3: `var a := new Array<Widget>(3, new Widget("f")); a[0].Name`** (`p05`; `p13` is the trivial-dispose `Box` twin).

- *Copy.* The constructor is shape (i): a constructor "whose result is an `owned` collection … copies into that result every element value it takes: … from each argument declared at one of them (`fill`)". The destination is "where the result lands (its binding …)", here `a`.
- *Kernel.* `fill` is a plain parameter of a standard member that K8.4's exception does not name. The same admission as R2.
- *Verdict.* Admitted. All three slots borrow one disposed `Widget` from the statement's end. The only exception is `size = 0`, which is dead code.

### 2.2 The controls, and the inversion

| probe | program (abridged) | reading of the v0.360 text | MEASURED (both modes) |
|---|---|---|---|
| `p11` | `ws.Add(new Widget("a"))` | §15.4 fresh-store error | rejected: "§15.4 fresh-store error, v0.346" |
| `p06` | `if true then var loc := LoadAll(); ws.AddRange(loc); end` (`ws` declared outside) | §28.11 clause 2 through *Value-copy members* (#5693's `_all.AddRange(local)`) | rejected: "borrow of `loc` passed to `.AddRange(...)` on `ws` would outlive its source" |
| `p01` | `ws.AddRange(LoadAll())` | §2.3: two readings | accepted, silent, prints `x`, no `dtor x` ever |
| `p15` | `using var keep := LoadAll(); var ws := …; ws.AddRange(keep)` | admitted (same scope) | accepted, prints `x`, then `dtor x` at scope end |
| `p23` | `ws.AddRange(Pass(w))`, `Pass(w: Widget): owned Array<Widget>` holding `w` | admitted: a borrowing collection's reads are sourced where the borrow came from (§28.11, "the collection is a transit point") | accepted, prints `p` |
| `p02` | `ws.Add(LoadAll()[0])` | HYPOTHESIS: rejected by K8.4 (a borrow of a temporary at a position whose retention is announced) | accepted, silent, prints `x`, no `dtor x` |
| `p03` | `var a := LoadAll().ToArray(); a[0].Name` | HYPOTHESIS: rejected by K8.4 (a binding of a receiver-sourced copy of a temporary, announcement table row 4) | accepted, silent, prints `x`, no `dtor x` |

`p06` against `p01` is the inversion 0207 §6 removed for the single-element stores: "the source with the *narrowest* scope of all — the temporary, whose scope is the statement — was the one source the store rule warned about instead of rejecting." Under the graduated reading of R1 it is still there in the many-element store: a nested-block local is rejected, and the full-expression temporary is admitted.

`p02` and `p03` are outside this proposal. On the reading above, both are already rejected by the v0.360 text, so the compiler's acceptance is a divergence and not a design question. §12 question 4 asks the owner to confirm that reading.

### 2.3 Why this is design-level, not a clarification

For R2 and R3 the text is clear: K8.4's plain-parameter sentence admits them, and its exception is enumerated. Rejecting them narrows a stated admission, which is a design change.

For R1 the text points two ways:

- *Rejecting reading.* *Value-copy members* makes the call "a store of each placed value into the destination, at the call". That is a store "in the producing body", the first escape K8.4 lists. The placed value is a borrow of a temporary. The code-generation reference already describes the copy this way ("checked as your own `Add` of each element would be"). §28.11 clause 4 calls the single-element case "clause 2's rejection with the source's scope at its narrowest", which applies to these borrows as much as to the temporary itself.
- *Admitting reading.* `items` is a plain parameter, and K8.4 says "A plain parameter announces none", with one exception that names only "the single-element store positions". 0207 §4.3 says a many-element form "stays accepted", and the v0.346 filing says "outside 0207's single-element rule".

The graduation took the admitting reading. Either way the spec needs a sentence: under (a), one that admits R1 in terms; under (b), one that rejects it. Choosing between them is the decision.

---

## 3. What the compiler does (question 1)

MEASURED, `bezelc 0.0.0-8ccf7ff0`, `--checked-disposal` and `--unchecked-disposal`:

| probe | shape | compiles | checked run | unchecked run |
|---|---|---|---|---|
| `p01` | R1, `ws.AddRange(LoadAll())`, read `ws[0].Name` | yes, no diagnostic | prints `x`; no `dtor x` | prints `x`; no `dtor x` |
| `p20` | R1, no read | yes | no `dtor x` at any point | — |
| `p04`/`p16` | R2, `Fill(new Widget("f"))`, read `ws[0].Name` | yes, no diagnostic | prints `f`; no `dtor f` | the same |
| `p12` | R2 at trivial-dispose `Box` | yes | prints `7` | prints `7` |
| `p05` | R3, `new Array<Widget>(3, new Widget("f"))` | yes, no diagnostic | prints `f`; no `dtor f` | the same |
| `p19` | R3 at `size = 0` | yes | no `dtor z` at any point | — |
| `p09` | `sp[0] := new Widget("s")`, `sp` a span over `ws` | yes, no diagnostic | prints `s`; no `dtor s` | the same |
| `p18` | control: `Use(new Widget("u"))`, a user static method's plain parameter | yes | prints `u`, `after`, then `dtor u` | — |

So the checked build does not fault because the argument is never disposed: R1's owning temporary and its elements, and R2's and R3's fresh value, are leaked. The disposed flag is not set and then left unread. The issue's first alternative ("the temporary's elements are not disposed at the boundary (a leak)") is the one that holds. The ASan leg could not be run on this host (the probe toolchain lacks `libclang_rt.asan`); the evidence is the destructor's output, which never appears, even at program exit.

This is a compiler bug under every option. Under (a) it is what keeps the backstop from firing. Under (b) the rejected shapes no longer compile, but the admitted neighbours (§4.3's frames, `p22`) leak the same way. It may share a mechanism with [#7428](https://github.com/s8lap/bezel-lang/issues/7428) ("a `move` or `Task`-family temporary at a plain argument is not disposed at its K8 boundary"); that is a HYPOTHESIS. Two neighbouring divergences were seen along the way and are left for triage, not filed here: `p18` disposes a plain-argument temporary at the end of the method rather than at the end of its statement (K8.1), and `p24` (`for var g in Gen() do ws.Add(g); end`, an enumerator-owned yield stored into an outer collection) compiles silently and faults.

---

## 4. Options

### 4.1 (a) Keep proposal 0207 as landed

*The rule.* Unchanged. The many-element forms stay admitted under K8.4's plain-parameter residual, and a later read meets §28.2's backstop. The text closes §2.3's seam in the admitting direction with one sentence in K8.4 and one in §28.11 *Value-copy members* (§8.7). The compiler fixes the leak (§3), so a checked build faults at the first read.

*Traced.* R1: admitted by the new sentence; after the leak fix the checked build raises `DisposedAccessException` at `ws[0].Name`, and the unchecked build reads freed memory. R2 and R3 are the same. The spec's own examples are unchanged: §28.11's `_all.AddRange(items)` is admitted, `_all.AddRange(local)` is rejected, and stdlib §4.15's `Fill(myClass)` over a named value is admitted.

*The case for it*, stated in full because it may be the better answer:

1. **The evidence base does not carry over.** 0207 reversed a ruled decline on three grounds, and one was empirical: the warning it promoted "ran at that scope from its implementation (#3364) with no false positive on record" (rationale §5.28.6). It promoted the lint's predicate "unchanged". The many-element positions never carried a warning, so there is no record of how the predicate behaves on real code. Option (b) would be the first time the language rejects at these positions, with no warning ramp. Promoting a known predicate and inventing a new one are not the same step, and 0207 took only the first.
2. **Nothing in the tree writes these shapes.** The reach scan (§6) finds no test, rule-corpus case, doc-corpus fragment, guide or spec example with a fresh owning collection passed to `AddRange`/`UnionWith`/`LinkedList<T>(source)`, or a fresh class value passed to `Fill` or as `fill`. 0207's shape had 62 gtest rows, written by authors who reached for it; this one has none. A rule guarding a shape nobody writes still costs a diagnostic, a predicate, test rows and a paragraph in three documents. The subtraction review ([`subtraction-review-2026-08.md`](subtraction-review-2026-08.md)) counts that cost.
3. **The line moves rather than disappears.** Under (b) the frame neighbours stay admitted: `ws.AddRange(Gen())` over an iterator whose yields are enumerator-owned (`p21`: a checked-build `DisposedAccessException`, an unchecked-build segfault), and `ws.AddRange(LoadAll().Where(p))` (`p22`). The language would go from "single-element stores rejected, many-element forms admitted" to "an owning collection argument rejected, a frame over the same collection admitted". 0207's line had the merit of matching the lint's. (b)'s line is principled (§4.2 *Soundness*), but a reader still has to learn it.
4. **The backstop is the recorded answer for what the analysis does not reach.** Proposals 0205 and 0207 share a posture: "where the fault is certain and the analysis exists, the language says so at compile time; where it is conditional, the backstop stays the answer" (0207 §6). The many-element forms are certain, but the analysis for them does not exist yet: the compiler's `AddRange` arm checks scope-bound locals only, and nothing checks `Fill` or the fill constructor. Under (a) they wait, like the mixed-kill joins 0205 states as a residual, until someone writes the shape.
5. **The cost is small and lands soon.** The leak fix is needed anyway. With it, every one of these programs faults in a checked build at its first read, the conformance-mode default ([#4602](https://github.com/s8lap/bezel-lang/issues/4602)).
6. **No churn on a rule that graduated thirty days ago.** 0207 landed on 2026-10-27. Reopening its scope before any implementation has run at it (0207's own error is still spec-ahead-of-impl under #3047) changes the target under the lane that implements it.

*The case against it.*

1. It keeps the inversion `p06`/`p01` and writes it into the spec. The admitting sentence must say that a borrow of a temporary copied by `AddRange` is admitted, while the same borrow stored by `Add` is rejected (`p02`, if §12 question 4 is confirmed), and a nested-block local copied by `AddRange` is rejected. The only reason it can give is the member's arity.
2. `Fill` and `Add` have the same signature shape: a bare `T`, no return, shape (ii). `sp[0] := new W()` would be rejected (§12 question 3), while `sp.Fill(new W())` writes the same dead reference into every slot and is admitted.
3. An unchecked build reads freed memory with no diagnostic, the harm 0207 §2 measured as decisive for severity.

*Cost.* Text: two sentences (§8.7). Compiler: the leak fix only.

*Migration.* None.

### 4.2 (b) Extend at the argument's own shape (recommended)

*The rule.* §15.4's fresh-store rule reaches every position at which a standard-library value-copy member places a fresh value itself into a borrowing slot, and every many-element form whose argument owns the elements it supplies:

- *The fresh value itself*, at a parameter declared at the element type: the single-element stores as today; the indexer-set of a span view over a borrowing array (`ArraySpan<T>`'s, which §28.11 already calls "a single-element store in its own right"); `ArraySpan<T>.Fill`'s `value`; and `Array<T>(size, fill)`'s `fill`. The value set is 0207's, unchanged: a `new` expression, a collection literal, an `owned`-returning call, a `move` result, a `Task`-family call, at any class type including trivial-dispose ones.
- *A fresh owning collection*, at a many-element form's collection parameter: `Array<T>.AddRange`, `Set<T>.UnionWith` and the `LinkedList<T>(source)` constructor, given a fresh unbound owned temporary of an owning-family collection type (stdlib §4.2) at a class or interface element type. Each placed value is a borrow sourced at that temporary, which its boundary disposes together with its elements.

The verdict is 0207's: a compile error at the call, whatever the destination's own lifetime. The remedies are 0207's two, specialised in §8.1.

*Traced.* R1, R2, R3: rejected at the call. Controls: `p23` (a fresh borrowing temporary) stays admitted, because its elements are not sourced at it. `p15` (a named owning local, same scope) stays admitted. `p06` stays rejected by clause 2. A value type, a `string` or a box as `fill` (`new Array<Pt>(2, new Pt(1, 1))`, `new Array<Object>(1, new Pt(1, 2))`) is outside the rule, as 0207's value set already says. `p21` and `p22` (frames) stay admitted under the residual, stated in terms. The spec's own examples are unchanged.

*Soundness.* Both arms are local and certain. They read the argument expression's producer classification (`producerState`, which 0207's arm already reads) and, for the second arm, the argument's static type head. There is no flow analysis and no callee body. The second arm keys on an owning family because that is where a read's source is the collection itself (§28.11 *Owning collections*). A borrowing collection is "a transit point, not a source", so a fresh borrowing temporary places borrows that outlive it. That is why `p23` is sound and `p01` is not. The rejected programs that would not fault are dead stores (an empty span, `size = 0`, an empty owning result), the class 0207 §4.3 item 1 already rejects at the single-element stores and §28.6 rejects at a field.

*Ergonomics.* The remedy is one line in each case: `using var src := LoadAll(); ws.AddRange(src);`, or the owning family's draining overload (`OwningArray<T>.AddRange(owned items)`, `OwningSet<T>.UnionWith(owned other)`); `using var w := new Widget("f"); span.Fill(w);`. `Fill` and `fill` have only the first remedy. One owner cannot hold N references (stdlib §4.14's *Why the mutation methods differ by category*), so there is no owning `Fill`, and a program that wants N distinct owned values builds them in a loop. The position set is stated by reference to §28.11's register ("the parameters at which a value-copy member places its argument itself"), so a future standard member of the same shape is covered without a list edit. That answers 0207 §9's note that three passages had to be kept naming one set.

*Cost* (estimated from `src/` read-only).

- `checkSec154FreshStoreCall` (`src/ownership/Ownership_Checks.cpp`, around line 18479) admits `Fill` when the receiver's head is `ArraySpan`. Its `borrowingFamily` head gate has an `ArraySpan` arm today only for the indexer-set: about 10 lines.
- The indexer-set path (`checkIndexerSetBorrowStoreEscape`, around 18741) gains the `ArraySpan` head (`p09`).
- The constructor arm is new: at a `new Array<T>(size, fill)` whose argument count is 2, `rejectSec154FreshStore` on `args[1]`; the same for `LinkedList<T>(source)` under the second arm. About 20 lines at the `new`-expression check.
- The second arm sits in the existing bulk-store path (`bulkStoring`, around 17644, which today handles `AddRange` only and judges a scope-bound local argument). `UnionWith` joins it, and one branch rejects an argument whose `producerState` is owned and whose head is an owning-family collection at a class element. That needs one new diagnostic variant naming the element borrows. About 30 lines.
- Tests: about 15 rows, the nine corpus cases of §10 and their negative controls.

This is one lane, single-file in ownership, disjoint from the `typeCall`/`lowerBinary` hot functions. The §3 leak fix is a separate codegen item under either option.

*Migration.* None measured (§6). No test, banked corpus case or document example flips.

### 4.3 (c) Extend by source set, frames included

*The rule.* Read §28.11's *Value-copy members* through K8.4 at full generality: any placed value whose source set includes an unbound owned temporary of the call's own full expression, at any value-copy member, is rejected. That covers (b), plus `ws.AddRange(LoadAll().Where(p))` (a §5 lazy frame carrying the temporary), `ws.AddRange(Gen())` over an iterator with enumerator-owned yields (`p21`), and `new LinkedList<Widget>(LoadAll().Where(p))`.

*Traced.* R1–R3 rejected; `p21` and `p22` rejected; `p23` admitted. For an imported iterator (`ws.AddRange(lib.Items())`) the verdict needs the iterator's yield classification. That is a same-unit fact only, and its cross-module form is deferred (`DECLINED.md` row 634). The rule would therefore either over-reject every imported iterator at a class element, including a sound one that yields borrows of a long-lived field, or split its verdict by module.

*Soundness.* Strictly more than (b), but at the frame arms the predicate is a may-source: an iterator with one enumerator-owned yield on one path places values sourced at the frame only on that path. That is §29.11's terminal-accessor territory, which the record keeps at a warning. A materializer-site diagnostic is deferred there (row 733, which names `for var x in Load(p).ToArray()` as "a silent certain fault" and still defers it), and source-chain tracing through iterator bodies is declined, with a counterexample: a frame-resident borrowing collection whose elements outlive the frame (row 950).

*Cost.* The frame arm needs the yield-ownership summary that the terminal-accessor trace computes (same-unit, three-valued) threaded into the store check, and a §5 frame's carried sources threaded into the bulk-store path. That is several times (b)'s cost, in a second file.

*Migration.* Unmeasured for imported iterators; zero in the tree for the same-unit shapes.

*Declined.* It reopens rows 733 and 950 without meeting their bars, and its arm-specific cost lands in the frame machinery for shapes no program writes. The frame shapes stay at the backstop under (b), stated as a residual (§8.1 and §8.4), and the choice is §12 question 2.

### 4.4 (d) A warning at the many-element positions

A mandated warning at (b)'s predicate, with acceptance unchanged. This is not viable. `DECLINED.md` row 1087 declines keeping the single-element shape at a warning because it is "a certain dangle whose only sound carriers are dead stores … kept at a warning with no suppression spelling", and row 408 forbids a suppression spelling. The same reason applies here, and a warning would also be a new lint, which the subtraction posture weighs against more than an error that closes the shape. Declined.

---

## 5. Recommendation

(b), for these reasons:

1. **The certainty is the same as 0207's, and so is the analysis.** Each rejected call writes a reference to a value that dies at the end of its statement, with no path, input or data dependency that avoids it. The predicate reads only what 0207's arm reads (the argument's producer classification) plus one type head.
2. **It removes an inversion against lifetime that the spec otherwise writes down.** Under (a), `AddRange` of a nested-block local is rejected and `AddRange` of a full-expression temporary is admitted (`p06`/`p01`). 0207 removed that inversion for `Add`. Keeping it for `AddRange` needs an admitting sentence (§8.7) whose only reason is arity.
3. **The position set becomes a structural reference to §28.11's register.** Today it is a list. Under (b) it is one phrase, so §15.4, §28.11 clause 4, K8.4 and Appendix C.3 name one set by construction, and the span view's indexer-set question (§12 question 3) disappears.
4. **The cost is small and the migration is zero.** Three compiler arms in one file, no test flips, no corpus case moves.

The case for (a) (§4.1) is real, and it is the right choice if the owner weighs these more heavily:

- the absence of a field record at the new positions;
- the absence of any program that writes the shapes;
- that the frame neighbours stay admitted either way.

If (a) is chosen, the two sentences of §8.7 should land so that the admission is stated rather than left to a reading, and the leak fix becomes the whole of #7426.

What (b) does not do: it does not reach frames (§4.3); it does not reject a fresh temporary at a user-declared member's plain parameter (row 68 stands untouched); and it does not decide `p02`/`p03`, which the current text already decides (§12 question 4).

---

## 6. Reach, measured

Scans over `tests/` (gtest, lit, both corpora) and `docs/` at `bd12fe038`:

- `\.(AddRange|UnionWith)\(` with an argument that is a call, `new`, a literal or `move`: 14 hits. Every one is outside the rule. `dst.AddRange(Gen())` in `array_addrange_ienumerable_2721` yields `string`. `MakeSet().UnionWith(Other())` in `linq_set_terminal_unbound_reclaim_6394` is over `Set<int>`. The `s1.UnionWith(move s2)` rows in `owned_set_algebra_1259` and `owned_set_union_1474`, and `l4_5_0011`'s `dst.AddRange(move src)`, are owning receivers (the draining overloads). The rest are comments.
- `\.Fill\(` with a `new`, `move` or call argument: 2 hits (`array_record_add_string_coown_8386`, `rows.AsSpan().Fill(new E(…))` and `ps.AsSpan().Fill(new P2(i, 5))`), both records. The other `Fill` hits are user methods named `Fill`.
- `new Array<…>(n, <producer>)`: 8 hits. Records (`E`, `Pt`, `Pair` in `s28_11_0023`), `DateTime`, a box (`new Array<Object>(1, new Pt(1, 2))`), and a generic `G.Id(x)` at `string`. All are outside.
- `new LinkedList<…>(<producer>)`: no hit.
- Document examples: no spec, stdlib, guide or code-generation reference example writes the shape. Guide 05's `header.Fill(0)` and Guide 01's `fresh.CopyFrom(_source)` are value-typed or named.

Newly rejected: nothing in the tree. Banked rule-corpus cases changing expectation: none. Doc-corpus fragments: none. Stale needles: the error reuses 0207's `§15.4 fresh-store error` text with a new store spelling, so no needle vanishes. The code-generation reference's callouts gain words and lose none.

---

## 7. Declined alternatives (`DECLINED.md`, `declined-at-proposal`)

If (b) is accepted, three rows are added:

| decline | ground | reopening bar |
|---|---|---|
| Keeping proposal 0207's single-element scope, the many-element forms (`AddRange`/`UnionWith`/`LinkedList<T>(source)` at a fresh owning collection; `ArraySpan<T>.Fill` and `Array<T>(size, fill)` at a fresh class value) admitted under K8.4's plain-parameter residual (proposal 0219 option (a)) | The same certain dangle as the single-element store, at every slot the call writes; keeping it writes into the spec an inversion against lifetime (a nested-block local rejected at `AddRange`, the full-expression temporary admitted) whose only ground is the member's arity, and leaves an unchecked build reading freed memory | —(none stated) |
| Extending the fresh-store rule by source set through frames (an iterator call with enumerator-owned yields, a §5 lazy query over a fresh owning temporary) at the value-copy members (proposal 0219 option (c)) | Needs the per-iterator yield summary the record keeps same-unit (row 634) and the source-chain tracing declined at row 950; a may-source at an iterator with mixed yields; would over-reject imported iterators or split the verdict by module; the frame shapes stay at the backstop, stated as a residual | The cross-module yield summary of row 634, or a measured program that writes `AddRange` over such a frame |
| A warning instead of the error at the many-element positions (proposal 0219 option (d)) | Row 1087's ground: a certain dangle whose only sound carriers are dead stores, at a warning with no suppression spelling (row 408) | —(none stated) |

If (a) is accepted instead, the first row is replaced by a row declining option (b), with this ground: no field record at the new positions, zero programs that write the shapes, and the frame neighbours admitted either way. Its reopening bar would be a measured program, test or corpus case that writes one of the shapes. Rows (c) and (d) stand as written.

---

## 8. The normative text (option (b))

No version header or pin moves; the landing adds those and the changelog entry. Each edit gives the v0.360 text it replaces, verbatim.

### 8.1 Spec §15.4, the store paragraph

Replace

> in a single-element store position of a borrowing-family collection is a compile error at the store site. The positions are the single-element stores of [§28.11](#sec-borrowing-collections)'s value-copy shape (ii) — `Add`, `Insert`, `Push`, `Enqueue`, `TryAdd`, `LinkedList<T>`'s `AddFirst`/`AddLast`/`AddBefore`/`AddAfter`, `GetOrAdd`'s key, `Counter<K>.Increment`'s key — and the indexer-set, **key and value positions alike**, on every collection of the borrowing family

with

> in a store position of a borrowing-family collection is a compile error at the store site. The positions are the parameters at which a standard-library value-copy member ([§28.11](#sec-borrowing-collections), *Value-copy members*) places the argument itself into a borrowing slot: the single-element stores of shape (ii) — `Add`, `Insert`, `Push`, `Enqueue`, `TryAdd`, `LinkedList<T>`'s `AddFirst`/`AddLast`/`AddBefore`/`AddAfter`, `GetOrAdd`'s key, `Counter<K>.Increment`'s key — and the indexer-set, **key and value positions alike**, a span view's over a borrowing array included; `ArraySpan<T>.Fill`'s value, which shape (ii) places in every slot the span views; and the `fill` argument of `Array<T>(size, fill)`, which shape (i) places in every slot of the constructed array — on every collection of the borrowing family

After the sentence ending "so a class-typed key is held as a borrow exactly as `Dictionary<K, V>`'s is.", insert

> A many-element form whose argument owns the elements it supplies is the same error: `Array<T>.AddRange`, `Set<T>.UnionWith` and the `LinkedList<T>(source)` constructor, given a fresh unbound owned temporary of an owning-family collection type (stdlib [§4.2](Bezel_Standard_Library.md#sec-owning-collection-interfaces)) at a class or interface element type, place borrows whose source is that temporary — an owning collection's reads are sourced at the collection ([§28.11](#sec-owning-collections)) — and its [K8](#sec-kernel-k8) boundary disposes it with every element it holds. A fresh **borrowing**-family temporary is not such an argument: its reads are sourced where its elements came from, and the store rule judges those sources. Nor is a frame — an iterator call, a lazy query: a borrow of a temporary that a frame carries into a many-element form is held under [K8.4](#sec-kernel-k8)'s plain-parameter residual and meets [§28.2](#sec-checked-disposal)'s backstop.

After the sentence ending "for dynamic task fan-in, `OwningArray<OwningTask<T>>` with the owning `Task.WhenAll` overload, stdlib [§6.7](Bezel_Standard_Library.md#sec-task-static-helpers))." insert

> At a many-element form the remedies are the same two: bind the argument first (`using var src := LoadAll(); ws.AddRange(src);`, whose scope the store rule then compares with the receiver's), or use the owning family's draining overload (`OwningArray<T>.AddRange(owned items)`, `OwningSet<T>.UnionWith(owned other)`); `Fill`'s value and `fill` have the first only, one owner holding one reference ([§28.6](#sec-leak-prevention)).

The rest of the paragraph ("The rule covers the trivial-dispose shape …", the boundary-event split, "The rule reaches no other callee …") is unchanged and reads correctly over the wider set.

### 8.2 Spec §28.11, *Borrowing collections*, clause 4

Replace

> 4. a **fresh unbound owned temporary** of class type in a single-element store position — `Add`/`Insert`/indexer-set and the other stores [§15.4](#sec-collection-literals) names — is not a borrow of any named source but a temporary that dies at its K8 boundary ([§28.3.11](#sec-unbound-owned-temporaries)): the store is rejected

with

> 4. a **fresh unbound owned temporary** of class type in a store position — `Add`/`Insert`/indexer-set, `ArraySpan<T>.Fill`'s value, `Array<T>(size, fill)`'s `fill` and the other stores [§15.4](#sec-collection-literals) names — is not a borrow of any named source but a temporary that dies at its K8 boundary ([§28.3.11](#sec-unbound-owned-temporaries)), and a fresh owning-family temporary whose elements `AddRange`, `UnionWith` or `LinkedList<T>(source)` place makes each placed value a borrow of such a temporary: the store is rejected

(the clause's remainder, "at the store site by [§15.4]'s fresh-store rule — clause 2's rejection with the source's scope at its narrowest …", is unchanged).

### 8.3 Spec §28.11, *Value-copy members*

After "… and `_all.AddRange(local)` and `return local.ToArray()` over a scope-bound local are rejected, as `_all.Add(local[0])` is ([#5693](https://github.com/s8lap/bezel-lang/issues/5693))." insert

> At a fresh owning-family temporary the same copy is [§15.4](#sec-collection-literals)'s fresh-store error (`_all.AddRange(LoadAll())`, `LoadAll()` returning `owned OwningArray<Item>`); a frame argument (`_all.AddRange(LoadAll().Where(p))`, an iterator call) is not, and holds what it carries under [K8.4](#sec-kernel-k8)'s plain-parameter residual.

### 8.4 Spec K8.4, the exception sentence

Replace

> The single-element store positions of the standard borrowing-family collections, and `OwningDictionary<K, V>`'s key, are the exception: their declared signatures are read as stores ([§28.11](#sec-borrowing-collections) *Value-copy members*; the owning map's key is held as a borrow, [§28.11](#sec-element-slot-policy)), so the retention is announced and a fresh unbound owned temporary there is [§15.4](#sec-collection-literals)'s fresh-store error.

with

> The store positions [§15.4](#sec-collection-literals) names — the single-element stores of the standard borrowing-family collections and of a span view over one, `ArraySpan<T>.Fill`'s value, `Array<T>(size, fill)`'s `fill`, the many-element forms at an argument that owns its elements, and `OwningDictionary<K, V>`'s key — are the exception: their declared signatures are read as stores ([§28.11](#sec-borrowing-collections) *Value-copy members*; the owning map's key is held as a borrow, [§28.11](#sec-element-slot-policy)), so the retention is announced, and a fresh unbound owned temporary there, or one whose elements such a form places, is [§15.4](#sec-collection-literals)'s fresh-store error. A frame passed to a many-element form is not among them: a borrow of a temporary it carries is held under the residual above.

### 8.5 Spec Appendix C.3

In the bullet beginning "A fresh-producer element of non-trivial-dispose class type in a borrowing-family collection literal is rejected", replace

> in a single-element store position of a borrowing-family collection (`Add`/`Insert`/indexer-set and the other stores [§15.4](#sec-collection-literals) names, key and value positions alike) or in the key position of `OwningDictionary<K, V>` is rejected at the store site,

with

> in a store position of a borrowing-family collection (`Add`/`Insert`/indexer-set, `ArraySpan<T>.Fill`'s value, `Array<T>(size, fill)`'s `fill` and the other stores [§15.4](#sec-collection-literals) names, key and value positions alike) or in the key position of `OwningDictionary<K, V>`, and a fresh owning-family temporary whose elements `AddRange`, `UnionWith` or `LinkedList<T>(source)` place, is rejected at the store site,

### 8.6 Standard library

- **§4, *Value-copy members are stores*.** After "… are stores in their own right under the slot policy's store row (language spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy)), whatever they return." append: "Language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)'s fresh-store rule reads its positions off the same register: a fresh class value where a member places its argument itself into a borrowing slot (the single-element stores, `ArraySpan<T>.Fill`, `Array<T>(size, fill)`'s `fill`), and a fresh owning collection whose elements `AddRange`, `UnionWith` or `LinkedList<T>(source)` place, are compile errors at the call."
- **§4.4 `Array<T>`.** After "To have elements disposed, use `OwningArray<T>` ([§4.5](#sec-ownedarrayt)) — element disposal never happens through the borrowing type (spec §28.6)." append: "A fresh class value as `fill`, and a fresh owning collection as `AddRange`'s argument, are spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)'s fresh-store error: the array would hold borrows of a value its statement's end disposes. Bind it first, or use `OwningArray<T>.AddRange(owned items)`."
- **§4.6 `LinkedList<T>`.** At the source constructor's line, extend the declaration comment: `public constructor(source: IEnumerable<T>);   // a fresh owning collection as source: spec §15.4's fresh-store error`.
- **§4.10 `Set<T>`.** At `public method UnionWith(other: IEnumerable<T>);` add the comment `// a fresh OwningSet as other: spec §15.4's fresh-store error; use OwningSet<T>.UnionWith(owned other)`.
- **§4.15 `ArraySpan<T>`, *Bulk mutation*.** Replace "For class T, the assigned-into slot stores a borrow (`Fill(myClass)` makes every slot borrow `myClass`); for value T, the slot stores a copy." with "For class T, the assigned-into slot stores a borrow (`Fill(myClass)` makes every slot borrow `myClass`, and a fresh class value there, as at the indexer-set, is spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)'s fresh-store error); for value T, the slot stores a copy."

### 8.7 The text for option (a), if it is chosen instead

K8.4: after "… is [§15.4](#sec-collection-literals)'s fresh-store error." insert "The many-element forms are not among them: a fresh temporary, or a borrow of one, that `AddRange`, `UnionWith`, `Fill`, a fill or source constructor places is held under the residual above, the temporary disposed at its boundary." §28.11 *Value-copy members*: after the #5693 sentence insert "A temporary's borrows copied by a many-element form (`_all.AddRange(LoadAll())`) are admitted under [K8.4](#sec-kernel-k8)'s plain-parameter residual and meet [§28.2](#sec-checked-disposal)'s backstop." Under (a), no other document changes.

### 8.8 Rationale §5.28.6

After the paragraph *The store lint is promoted to an error*, add:

> **The many-element forms ([proposal 0219](proposals/0219-fresh-store-at-the-many-element-forms.md)).** The promotion took the lint's single-element predicate and left the many-element forms open. They carry the same certain dangle at every slot they write: `ws.AsSpan().Fill(new W())` makes every slot borrow one value its statement's end disposes, `new Array<W>(n, new W())` does the same to a fresh array, and `ws.AddRange(LoadAll())` over a fresh owning collection places borrows of elements that collection's boundary disposes. Left admitted, `AddRange` would reject a nested-block local and admit the full-expression temporary, the inversion against lifetime the promotion removed for `Add`. So the rule reaches every position at which a standard value-copy member places a fresh value itself, and every many-element form whose argument owns its elements. The position set is named by the value-copy register, not listed. Two things stay outside. A fresh borrowing collection's elements are sourced where they came from, so the store rule judges those sources. A frame is outside because its carried sources at an iterator need the per-iterator yield summary the record keeps same-unit and the source-chain tracing it declined, so a temporary's borrow carried by a frame stays at the backstop, a stated residual. No program in the tree wrote the shapes when the rule landed. The decision rests on the certainty and the consistency, with the absence of a field record at these positions stated as its cost.

### 8.9 Code-generation reference

- **§4.5, *Fresh values into collections*.** Replace "On the **borrowing** family the same shape via `Add`/`Insert`/indexer-set is a compile **error** (the temporary would die at the statement's end and the slot dangle)" with "On the **borrowing** family the same shape via `Add`/`Insert`/indexer-set, `span.Fill(…)` or `new Array<T>(n, …)`'s fill is a compile **error** (the temporary would die at the statement's end and the slot dangle), as is `AddRange`/`UnionWith` of a fresh **owning** collection (`ws.AddRange(LoadAll())`: bind it first, or use `OwningArray<T>.AddRange(owned …)`)".
- **§7.2, the fresh-store callout.** Replace "in `Add`/`Insert`/indexer-set position (and a fresh `OwningDictionary` key) is rejected at the store site" with "in `Add`/`Insert`/indexer-set position, as `span.Fill`'s value or `new Array<T>(n, fill)`'s fill (and a fresh `OwningDictionary` key), and a fresh owning collection given to `AddRange`/`UnionWith`, is rejected at the store site".
- **§7.2, the value-copy callout.** Replace "and at a class element whenever the source is a scope-bound local." with "and at a class element whenever the source is a scope-bound local or a fresh owning collection (§15.4's fresh-store error; a frame over one is not checked and meets the backstop)."

---

## 9. Changelog sketch

*§15.4, §28.11 (clause 4, *Value-copy members*), K8.4, Appendix C.3; stdlib §4, §4.4, §4.6, §4.10, §4.15; rationale §5.28.6; the code-generation reference §4.5, §7.2. The fresh-store rule reaches the many-element forms (design change; proposal 0219; #7426's spec half).* The positions of §15.4's fresh-store rule are now the parameters at which a standard value-copy member places its argument itself into a borrowing slot: the single-element stores as before, a span view's indexer-set, `ArraySpan<T>.Fill`'s value and `Array<T>(size, fill)`'s `fill`. The rule also reaches `AddRange`, `UnionWith` and `LinkedList<T>(source)` at a fresh owning-family collection, whose elements' borrows its boundary kills. A frame argument is a stated residual at the backstop. K8.4's exception sentence names the same set, so the retention at these positions is announced.

*Migration.*

- **Newly rejected (spec-ahead-of-impl):** a fresh unbound owned temporary of class type as `ArraySpan<T>.Fill`'s value, as `Array<T>(size, fill)`'s `fill`, or at a span view's indexer-set over a borrowing array; and a fresh owning-family collection at a class element given to `Array<T>.AddRange`, `Set<T>.UnionWith` or `new LinkedList<T>(source)`. Measured reach: no gtest row, lit row, banked corpus case or document example. Remedy: bind to a named holder first, or use the owning family's draining overload (`AddRange`/`UnionWith` only).
- **Unchanged:** a fresh borrowing-family temporary at a many-element form; a frame argument (residual); a user-declared member's plain parameter; value-typed, `string` and boxed fills.
- **Newly warned:** nothing.
- **Ramps:** none.

*Owner decisions recorded:* option (b) over (a) (§12 question 1), and questions 2–5 as ruled.

---

## 10. Proposed rule-corpus cases

Verdicts follow the cited sentence of the landed §15.4 (§8.1), never the compiler. Ids are assigned at harvest (next free `s15_4_` / `l4_15_` numbers). Each is a full program in the form of `s15_4_0016`. `Item` has a printing destructor and a public `Tag: int` field; `Box` is trivial-dispose.

| case | program core | expect | cites |
|---|---|---|---|
| `s15_4_…a` | `var a: Array<Item> := []; a.AddRange(LoadAll());` (`LoadAll(): owned OwningArray<Item>`) | `reject`, `diagnostic: §15.4 fresh-store error` | 15.4, 28.11 |
| `s15_4_…b` | `using var w := new Item(); var a: Array<Item> := []; a.Add(w); var sp := a.AsSpan(); sp.Fill(new Item());` | `reject` | 15.4, 28.11 |
| `s15_4_…c` | `var a := new Array<Item>(3, new Item());` | `reject` | 15.4, 28.11 |
| `s15_4_…d` | the same at `Box`: `var bs := new Array<Box>(2, new Box());` (trivial-dispose: the rule's full type set) | `reject` | 15.4, 28.9 |
| `s15_4_…e` | `var s: Set<Item> := []; s.UnionWith(LoadSet());` (`LoadSet(): owned OwningSet<Item>`) | `reject` | 15.4, 28.11 |
| `s15_4_…f` | `var l := new LinkedList<Item>(LoadAll());` | `reject` | 15.4, 28.11 |
| `s15_4_…g` (control) | `using var src := LoadAll(); var a: Array<Item> := []; a.AddRange(src); Console.WriteLine(a.Count);` | `run`, `stdout: 1` | 15.4, 28.11 |
| `s15_4_…h` (control) | `using var w := new Item(); var a: Array<Item> := []; a.AddRange(Pass(w)); Console.WriteLine(a.Count);` (`Pass(w: Item): owned Array<Item>` holding `w`: a fresh borrowing temporary) | `run`, `stdout: 1` | 15.4, 28.11 |
| `s15_4_…i` (residual) | `var a: Array<Item> := []; a.AddRange(LoadAll().Where(lambda(x: Item) => true)); try Console.WriteLine(a[0].Tag); catch DisposedAccessException => Console.WriteLine("faulted"); end;` | `run`, `stdout: faulted`, `flags: --checked-disposal` | 15.4, 28.11, 28.2 |

A span view's indexer-set case (`sp[0] := new Item();`, `reject`) joins if §12 question 3 is ruled as recommended. Existing cases that state neighbouring rules and stand: `s15_4_0016` (single-element), `s28_11_0019`/`0020`/`0024` (#5693's `AddRange` from a parameter or a scope-bound local), `s28_11_0023` (a record `fill`), `l4_5_0011` (the owning `AddRange`).

---

## 11. Tracker sketch (to file at landing, `spec-tracking`)

*§15.4 / §28.11 / K8.4 (v0.3NN, proposal 0219): the fresh-store rule at the many-element forms.* Extend `checkSec154FreshStoreCall` to `ArraySpan<T>.Fill`'s value and the indexer-set path to an `ArraySpan` receiver; add a `new Array<T>(size, fill)` arm on `fill` and a `new LinkedList<T>(source)` arm; in the bulk-store path, add `UnionWith` beside `AddRange` and reject an argument whose producer state is owned at an owning-family head over a class element, with a diagnostic naming the element borrows and both remedies. Add the §10 corpus cases and ~15 gtest rows, each proven fail-without. Independently of the ruling, fix #7426 question 1: the argument temporaries of `AddRange`, `Fill` and the fill constructor are not disposed at their K8 boundary (§3, `p01`/`p04`/`p05`/`p19`/`p20`), possibly the mechanism of #7428.

---

## 12. Owner questions

1. **(b) or (a)?** Extend the fresh-store rule to the many-element forms (recommended, §5), or keep 0207's single-element scope with the admission stated in terms (§4.1, §8.7)? Either way the leak is fixed.
2. **Frames.** Under (b), a frame passed to a many-element form stays at the backstop: an iterator call (`p21`, which faults when checked and segfaults when unchecked) or a §5 lazy query over a fresh owning temporary (`p22`). Confirm the residual (recommended, §4.3), or extend the rule to §5 lazy frames whose receiver chain roots at a fresh owning temporary? That subset needs no yield summary, but it adds a chain walk the rule otherwise avoids.
3. **A span view's indexer-set.** `sp[0] := new Widget("s")` (`p09`) compiles silently today. §15.4 names "the indexer-set … on every collection of the borrowing family"; stdlib §4's value-copy paragraph names "an indexer-set, a span view's included" among the single-element stores. Under (b) the span view is covered in terms (§8.1). Under (a) the owner must still say whether 0207 reaches it. Recommended: covered, as a clarification of 0207 if (a) is chosen.
4. **`p02` and `p03`.** This proposal reads `ws.Add(LoadAll()[0])` and `var a := LoadAll().ToArray()` as already rejected by K8.4 on the v0.360 text (the first through the announced retention of `Add`, the second as a binding of a receiver-sourced copy). The compiler admits both, silently and with a leak. Confirm the reading, which makes them compiler bugs to file, or rule them as a question of their own.
5. **A fresh value into a fresh result.** `new Array<Item>(3, new Item()).Count` has both values die at one boundary, and nothing can read the slots. 0207 rejects the analogous temporary receiver uniformly (`new Array<W>().Add(new W())`). Under (b) the fill constructor is rejected the same way, whatever its result's landing (recommended: one verdict per position).

---

## 13. Self-critique

- **Is (b)'s second arm keyed on a type head rather than a source?** Yes. It is phrased as a source fact ("an owning collection's reads are sourced at the collection") and implemented as a head test on the argument's static type, which coincide for a fresh argument. An `owned IEnumerable<Item>` result that is an owning collection at run time is not reached: its static type is not owning-family, and the spec does not track run-time types. That is a residual of the same kind as the frames.
- **Does (b) reopen row 68?** No. It reads no callee body. Every position it adds is a standard member whose signature §28.11 already reads as a store.
- **Does (b) contradict 0207 §4.3's "a many-element form … stays accepted"?** It changes that statement, and §1.3 says why that is a decision on an open question rather than a reversal of a reasoned one. If the owner reads §4.3's sentence as a recorded decision, the class is unchanged (design change) and the bar is the one §1.3 discusses.
- **Is the reach really zero?** The scans are regex scans over call spellings, each hit read. A many-element store reached through a helper or a method group (`var f := ws.AddRange; f(LoadAll())`) would not be matched. A method reference to a standard member taking such a parameter is its own question, and none exists in the tree (`git grep "\.AddRange;"`: no hit).
- **Could the leak fix change a verdict?** No: it changes run-time behaviour only. Under (a) it turns `p01`'s silent `x` into a checked fault, which is the backstop the spec already promises.
- **The worst case for (b).** A program that fills a borrowing array with one fresh trivial-dispose value and reads only `Count` (`new Array<Box>(n, new Box())` as a sized placeholder) is rejected. Its remedy is `using var b := new Box(); var bs := new Array<Box>(n, b);`, one line. Zero sites.
