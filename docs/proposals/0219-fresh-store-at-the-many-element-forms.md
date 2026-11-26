# Proposal 0219 — The fresh-store rule at the many-element forms

| | |
|---|---|
| **Status** | under-review — critiqued (pc-0219, SOUND-WITH-FIXES), revised 2026-11-26: the critique's fourteen findings applied, and the coordinator's ruling on the draft's question 4 folded in as §2.4; recommends option (b), with option (a), keeping proposal 0207's scope, argued in §4.1; owner decision pending on §12's four questions |
| **Tracking issue** | [#7426](https://github.com/s8lap/bezel-lang/issues/7426) (`spec-question`, `campaign:collection-elements`; filed at the v0.346 landing of [proposal 0207](0207-fresh-store-into-a-borrowing-collection-is-an-error.md)). The compiler half of #7426 (its question 1) is a bug under every option (§3, §9.1). [#3047](https://github.com/s8lap/bezel-lang/issues/3047) (0207's tracker, open for lint B) is where the compiler arms would go if (b) is accepted. |
| **Target module(s)** | **spec**: §15.4 (the store paragraph: the position sentence, one sentence on bulk copies, one remedy sentence), §28.11 (*Borrowing collections* clause 4; *Value-copy members*, one sentence), K8.4 (the exception sentence), Appendix C.3 (one bullet). **stdlib**: §4 (*Value-copy members are stores*, one sentence), §4.4, §4.10 and §4.15 (one sentence or comment each). **rationale**: §5.28.6 (one paragraph). **code-generation reference**: §4.5 (one paragraph), §7.2 (two callouts). **proposals**: `README.md` (this row), `DECLINED.md` (three rows). **guides**: none. **compiler**: spec-ahead-of-impl, one tracker (§11). **conformance**: no banked case changes expectation; ten proposed cases (§10). **tests**: one comment to correct at landing (§6). |
| **Written against** | spec/stdlib **v0.361** at `cdd2022c7` (`origin/main`). The first draft was written against v0.360 (`bd12fe038`). Every edit anchor in §8 was re-verified verbatim against `cdd2022c7`: each occurs exactly once, and v0.361 changed none of them. This proposal composes with accepted [proposal 0212](0212-borrow-bearing-classes.md) §8.3 (stage 3), which rewrites K8.4's plain-parameter sentence. The new K8.4 text here anchors to §28.11's borrowing-collection residual, not to that sentence, so the two can land in either order. §13 gives the interaction, and §2.3's two readings must be re-read against 0212's text at graduation. |
| **Origin** | #7426, filed by the v0.346 composition from lane G's critique. Triage for v0.361 (`triage.tsv` row 51) classed it DESIGN: "Extending the fresh-store error to many-element forms reverses proposal 0207's recorded scope … The checked-build non-fault of AddRange is a separate compiler bug under §28.11 value-copy." |
| **Change class** | Design change ([`spec-change-process.md`](spec-change-process.md) §2). For `Fill`, `fill` and the span-view indexer-set, option (b) narrows an admission K8.4 states. For `AddRange`/`UnionWith` it decides between two readings of the text (§2.3), and §2.4's ruling makes the admitting reading the odd one out. §1.3 states what the §6 reopening bar asks and whether the issue meets it. |

*Conventions. Unprefixed § references are language-spec sections at v0.361; "stdlib §" is the standard library; K tags are §28.3's kernel ids. MEASURED marks a verdict taken with `bezelc 0.0.0-8ccf7ff0` (the r42 release build) in both disposal modes. The author's probes `p01`–`p24` are under `/var/tmp/bezel-scratch/v361/p-0219/`; the critic's probes `q01`–`q15` are under `/var/tmp/bezel-scratch/v361/pc-0219/probes/`, and the author re-ran `q05`, `q10`, `q11`, `q13`, `q14` and `q15`. QUOTED marks repository text; HYPOTHESIS marks a reading or a judgement; RULED marks the coordinator's clarification-class ruling of 2026-11-26. Every program uses only members stdlib §4 declares; nothing is invented and no new name is proposed. Probes call the owning-array factory `Make()`; the prose calls it `LoadAll()`. Both are `owned OwningArray<Widget>` with one fresh `Widget("x")`.*

---

## 0. Summary

Proposal 0207 (v0.346) made a fresh unbound owned temporary of class type in a single-element store position of a borrowing collection a compile error. `ws.Add(new Widget("a"))` is rejected because the argument dies at the end of the statement while the slot keeps a reference to it. 0207 left the many-element forms outside the rule. #7426 asks whether these follow:

```bezel
ws.AddRange(LoadAll());                  // LoadAll(): owned OwningArray<Widget> — the array and its elements die at the statement's end
ws.AsSpan().Fill(new Widget("f"));       // every slot of ws borrows one Widget that dies at the statement's end
var a := new Array<Widget>(3, new Widget("f"));   // three slots, one dead Widget
```

Each is the dangle 0207 rejects, with certainty, at every slot the call writes. Today all three compile with no diagnostic (MEASURED).

**What the text already decides (§2.4, RULED).** Three shapes are already rejected at v0.361:
- `ws.Add(LoadAll()[0])`, by K8.4 through `Add`'s announced retention;
- `var a := LoadAll().ToArray()`, by §28.11 *Value-copy members* shape (i) with K8.4's binding escape;
- `new LinkedList<Widget>(LoadAll())`, the same shape-(i) copy with the temporary as an argument.

The compiler admits all three, so they are compiler bugs (§9.1).

**What remains open:**
- `AddRange` and `UnionWith` copying a temporary's element borrows into their receiver (shape (ii)). This includes a lazy frame over a temporary.
- A fresh class value placed itself by `ArraySpan<T>.Fill`, by `Array<T>(size, fill)`, or by a span view's indexer-set.

**The record.** 0207 drew its line with one stated ground, in the lint's negative-control comment that its §4.3 cites: "the source collection's own death leaves no dangling element slot". It has no decline record. That ground holds for a borrowing source and fails for an owning one, which is the line (b) draws. No `DECLINED.md` row exists, so the §6 reopening bar does not apply. Where 0207 §4.3's sentence is read as a decision, the owning-source measurement (`p01`) is new evidence against its one ground (§1.3).

**The options (§4):**
- (a) keep 0207's scope, which now means adding an admission that gives one copy shape two verdicts: `LoadAll().ToArray()` rejected and `ws.AddRange(LoadAll())` admitted;
- (b) extend the rule to the positions where a standard store places a fresh value itself, and to `AddRange`/`UnionWith` from a collection argument whose placed values are sourced at a fresh temporary of the same full expression;
- (c) also reach iterator calls with enumerator-owned yields;
- (d) a warning.

**Recommendation: (b).** It closes the open shapes at zero measured migration, makes shape (ii) agree with shape (i) on one copy, and removes the inversion `ws.AddRange(loc)` (a nested-block local, rejected, `p06`) against `ws.AddRange(LoadAll())` (narrower, admitted, `p01`). (a) remains coherent, and §4.1 states its case: no field record exists at the new positions, and no program in the tree writes the shapes. (c) is declined because enumerator-owned yields need the per-iterator yield summary the record keeps same-unit. (d) is declined on 0207's reason for not keeping a certain dangle at a warning.

**The compiler half of #7426 (question 1).** The checked build does not fault because the temporary is never disposed. This is wider than the many-element forms: `Console.WriteLine(LoadAll()[0].Name)` leaks with no store at all (`q11`). §3 and §9.1 list the bugs that hold under any option.

---

## 1. The question and the record

### 1.1 What 0207 decided

QUOTED, 0207 §3 *Position*: "The many-element forms (`AddRange`, `UnionWith`, `CopyFrom`) are not single-element stores and are outside the rule (§9 records the one open question about them)."

0207 §4.3, the list of what stays accepted: "a named holder, a parameter, a field read, an iteration variable, a value-typed or `string` element, an owning-family store, a user-declared callee, a many-element form — each is outside the predicate, as the lint's negative controls pin today (`fresh_store_warning_3364`'s six "does not warn" rows), and each stays accepted."

0207 §9 mentions the forms only as a compiler divergence: "`ws.AddRange(Make())` over a fresh `OwningArray<Widget>` compiles silently and does not fault in a checked build (`g04` …)".

The landed text follows. §15.4's rule says "in a single-element store position of a borrowing-family collection", and K8.4's exception sentence names "The single-element store positions of the standard borrowing-family collections, and `OwningDictionary<K, V>`'s key". 0207's error was implemented as `d00ad3a33` ("narrow #3047: §15.4 lint A is a compile error", 2026-11-04). #3047 stays open for lint B.

### 1.2 What 0207 left open, and the one ground it stated

QUOTED, the v0.346 changelog, *Filed*: "[#7426] (`spec-question`): the many-element forms — `AddRange` of a fresh owning temporary, `ArraySpan<T>.Fill` and `Array<T>(size, fill)` with a fresh class value — are outside 0207's single-element rule; extend it or keep them at the backstop (and the checked build does not fault today)."

`DECLINED.md` has rows for 0207's options (a), (c) and (d) (rows 1087–1089) and annotations on rows 68, 408, 467 and 837. None concerns the many-element forms. Rationale §5.28.6's promotion paragraph does not mention them.

The lint's negative controls include one many-element row: `fresh_store_warning_3364`'s `bulk_add_range_is_not_rejected` (`a.AddRange(src)`, where `src: Array<int>` is a parameter). Its comment gives the only recorded ground for the exclusion. QUOTED, `tests/ownership/fresh_store_warning_3364_test.cpp` lines 329–331 at `cdd2022c7`:

> `AddRange` is the BULK store: its argument is a COLLECTION whose elements are copied out, so the source collection's own death leaves no dangling element slot. §15.4 names the element store positions only.

0207 §4.3 cites these rows as the pin for "a many-element form … stays accepted". #7426 itself says 0207 "deliberately leaves the many-element forms out".

### 1.3 Does #7426 meet the reopening bar?

Spec-change-process §2 keys the reopening bar on "a rationale decline record or a `DECLINED.md` entry": reopened "only with *new evidence against the stated reasons*, never by re-asking". No such record exists for the many-element forms, so the bar in its own terms does not apply. Deciding them is a design change, not a reopening.

Stated plainly, #7426 brings no new evidence of its own. Its argument ("the certain-dangle argument applies to a many-element store …") is 0207's §2 argument applied to more positions. Its one measurement is 0207 §9's `g04`.

The exclusion did have one stated ground, the control's comment in §1.2, and 0207 §4.3 relies on it. That ground holds for a borrowing source, whose reads are sourced where its elements came from (§28.11, "the collection is a transit point, not a source"; `p23` is sound). It fails for an owning source, whose reads are sourced at the collection that the boundary disposes together with its elements (§28.11 *Owning collections*; `p01`). The comment's author had an `int`-element, named-parameter source in view. So if the owner reads 0207 §4.3's sentence as a decision, `p01` is new evidence against its one ground, and the bar is met either way.

0207 also recorded a consideration that bears on scope, and it is the strongest argument for keeping the line. Its case against the v0.238 decline rested partly on the lint's track record at exactly its predicate: "The error takes that predicate unchanged" (0207 §6). No lint ever ran at the many-element positions, so the extension cannot cite such a record. §4.1 carries that argument.

What this proposal adds beyond the issue:
- the lifetime inversion in the many-element store (§2.2, `p06` against `p01`);
- §2.4's settled shapes, which show that (a) splits one copy shape by receiver against argument;
- the frame cases, with the line between a lazy frame over a temporary (sourced by §28.3) and an iterator with enumerator-owned yields (not sourced: §4.3);
- the answer to question 1 (§3);
- a measured reach of zero (§6).

---

## 2. The text today, traced

### 2.1 The three repros

The declarations used by every probe:
- `Widget` has a destructor that prints `dtor <name>`, so it is non-trivial-dispose.
- `Box` has none, so it is trivial-dispose.
- `LoadAll()` (`Make()` in the probes) returns an `owned OwningArray<Widget>` holding one fresh `Widget("x")`.

**R1: `ws.AddRange(LoadAll()); ws[0].Name`** (`p01`, the issue's program).

- *Producer.* `LoadAll()` is an `owned`-returning call result used only as a non-`owned` argument, so it is an unbound owned temporary (§28.3.11). K8.1 disposes it at the end of the statement. An owning collection's disposal "disposes every element and then frees the backing storage" (§28.11 *Owning collections*).
- *Copy.* `AddRange(items: IEnumerable<T>)` is §28.11 value-copy shape (ii). "A copy is judged as a store of each placed value into the destination, at the call", and "a value from the receiver or from a collection or span argument carries what an element read from it carries". An element read from an owning collection is a borrow "whose source is the collection". So each placed value is a borrow sourced at the temporary.
- *Store check.* §28.11's clauses 1–3 judge borrows of parameters, receivers, fields and scope-bound bindings. Clause 4 judges "a fresh unbound owned temporary … in a single-element store position". Neither reaches a borrow of a temporary placed by `AddRange`.
- *Kernel.* K8.4: "a borrow or frame that would escape [the boundary] is rejected … The escapes rejected are every escape K2.3 can compare — among them a binding, store … of the borrow in the producing body — and every retention a declaration announces … A plain parameter announces none: a callee that stores it holds the borrow under … the borrowing-collection store rule's recorded residual … The single-element store positions … are the exception".
- *Verdict.* Two readings (§2.3).
- *After the statement.* Under either reading the slot holds a borrow of a disposed `Widget`. A checked build should fault at `ws[0].Name`, and an unchecked build reads freed memory. The one exception is an empty `LoadAll()` result, which is dead code.

**R2: `ws.AsSpan().Fill(new Widget("f")); ws[0].Name`**, with `ws` non-empty (`p04`; `p16` binds the span first).

- *Producer.* `new Widget("f")` is an unbound owned temporary at a non-`owned` argument, disposed at the statement's end.
- *Copy.* `ArraySpan<T>.Fill(value: T)` is shape (ii) through a span view, "a span receiver's destination being the buffer or array it views" (§28.11). Stdlib §4.15 says "`Fill(myClass)` makes every slot borrow `myClass`".
- *Store check.* Clause 4 and §15.4's list name the single-element stores and the indexer-set "on every collection of the borrowing family". `Fill` is not among them.
- *Kernel.* `value` is a plain parameter, and K8.4's exception does not list `Fill`.
- *Verdict.* Admitted. After the statement every slot of the span's range borrows one disposed `Widget`. The one exception is an empty span, which is dead code.

**R3: `var a := new Array<Widget>(3, new Widget("f")); a[0].Name`** (`p05`; `p13` is the trivial-dispose `Box` twin).

- *Copy.* Shape (i): a constructor "whose result is an `owned` collection … copies into that result every element value it takes: … from each argument declared at one of them (`fill`)". The destination is "where the result lands (its binding …)".
- *What is placed.* The fresh value itself, not a borrow of a temporary's element. So §2.4's shape-(i) ruling, which concerns placed borrows of a temporary, does not decide it.
- *Kernel.* `fill` is a plain parameter, and K8.4's exception does not list it.
- *Verdict.* Admitted, the same as R2. The one exception is `size = 0`, which is dead code.

### 2.2 The controls, and the inversion

| probe | program (abridged) | verdict at v0.361 | MEASURED (both modes) |
|---|---|---|---|
| `p11` | `ws.Add(new Widget("a"))` | §15.4 fresh-store error | rejected: "§15.4 fresh-store error, v0.346" |
| `p06` | `if true then var loc := LoadAll(); ws.AddRange(loc); end` (`ws` declared outside) | §28.11 clause 2 through *Value-copy members* (#5693's `_all.AddRange(local)`) | rejected: "borrow of `loc` passed to `.AddRange(...)` on `ws` would outlive its source" |
| `p01` | `ws.AddRange(LoadAll())` | §2.3: two readings | accepted, silent, prints `x`, no `dtor x` ever |
| `p15` | `using var keep := LoadAll(); var ws := …; ws.AddRange(keep)` | admitted (same scope) | accepted, prints `x`, then `dtor x` |
| `p23` | `ws.AddRange(Pass(w))`, where `Pass(w: Widget): owned Array<Widget>` holds `w` | admitted: a borrowing collection is a transit point | accepted, prints `p` |
| `p02` | `ws.Add(LoadAll()[0])` | RULED rejected: K8.4, `Add`'s announced retention (§2.4) | accepted, silent, no `dtor x` |
| `p03` | `var a := LoadAll().ToArray(); a[0].Name` | RULED rejected: §28.11 *Value-copy members* shape (i), the destination being the binding; the result is announcement-table row 1 (§2.4) | accepted, silent, no `dtor x` |
| `q13` | `var l := new LinkedList<Widget>(LoadAll())` | RULED rejected: shape (i), as `p03` (§2.4) | accepted, prints `x`, no `dtor x` |

`p06` against `p01` is the inversion 0207 §6 removed for the single-element stores: "the source with the *narrowest* scope of all — the temporary, whose scope is the statement — was the one source the store rule warned about instead of rejecting". Under the admitting reading of R1, the inversion is still there in the many-element store.

### 2.3 Why this is design-level, not a clarification

**R1.** The text points two ways.

- *Rejecting reading.* *Value-copy members* makes the call "a store of each placed value into the destination, at the call", that is, a store "in the producing body", the first escape K8.4 lists. The placed value is a borrow of a temporary. §2.4's ruling takes exactly this reading for shape (i).
- *Admitting reading.* `items` is a plain parameter. K8.4 says "A plain parameter announces none", and its one exception names only "the single-element store positions". 0207 §4.3 says a many-element form "stays accepted", and the v0.346 filing says "outside 0207's single-element rule".

The graduation took the admitting reading. Since §2.4, that reading separates `AddRange` from `ToArray` and `LinkedList<T>(source)` only by where the copy lands: the receiver under shape (ii), the result under shape (i). Choosing whether that difference carries a verdict is the decision.

**R2 and R3.** The text admits them by enumeration. K8.4's exception gives its reason: "their declared signatures are read as stores (§28.11 *Value-copy members*) … so the retention is announced". That reason is true of `Fill` and `fill` as well: shape (ii) names `Fill` in the sentence that names `Add`, and shape (i) names `fill`. The admission rests on the list, not on the ground. Rejecting them still narrows a stated admission, so it is a design change.

### 2.4 Settled by the existing text (RULED, 2026-11-26)

The coordinator ruled the draft's question 4 under the owner's delegation, as a clarification-class reading of existing text. These shapes are already rejected at v0.360/v0.361:

- **`ws.Add(LoadAll()[0])`** (`p02`; also `ws.Add(LoadAll().First())`, `q08`). Rejected by K8.4 through `Add`'s announced retention: a borrow of a temporary at a position whose retention a declaration announces. The compiler's own K8.4 diagnostic for the binding twin `var w := Make()[0];` (`q01`, rejected) already reads "the borrow may not be bound, stored, or returned".
- **`var a := LoadAll().ToArray()`** (`p03`). Rejected by §28.11 *Value-copy members* shape (i). The placed values carry what an element read from the receiver carries, a borrow sourced at the temporary; the copy is judged as a store into the destination, the binding; and K8.4 rejects the binding of such a borrow. Stdlib §4.5's snapshot paragraph states a receiver-outlives-snapshot obligation under §28.2's backstop. That paragraph is the residual for a named receiver the checker cannot compare, not for a temporary.
- **`new LinkedList<Widget>(LoadAll())`** (`q13`). Rejected the same way as `ToArray`: shape (i), the temporary an argument instead of the receiver.

Two neighbours follow from text the ruling did not need, and are listed with the compiler bugs:
- `var q := LoadAll().Where(p)` (`q14`) is rejected in terms by §28.3 *Temporaries as sources*: "Where the frame outlives the expression — bound to a name … — the program is rejected".
- `var a := LoadAll().Where(p).ToArray()` (`q15`) is a shape-(i) copy of the frame's carried sources into a binding, rejected on `p03`'s ground (HYPOTHESIS, the same reading).

These need no normative edit. §8.3 names `ToArray` and `LinkedList<T>(source)` beside the `AddRange` sentence so that the agreement is visible. The compiler's acceptance of each is a bug to file (§9.1).

---

## 3. What the compiler does (question 1)

MEASURED, `bezelc 0.0.0-8ccf7ff0`, `--checked-disposal` and `--unchecked-disposal`:

| probe | shape | compiles | checked run | unchecked run |
|---|---|---|---|---|
| `p01` | R1, `ws.AddRange(LoadAll())`, read `ws[0].Name` | yes, no diagnostic | prints `x`; no `dtor x` | the same |
| `p20` | R1, no read | yes | no `dtor x` at any point | — |
| `q05` | `ws.AddRange(move o)`, `o` a named owning local | yes, no diagnostic | prints `x`; no `dtor x` | the same |
| `p04`/`p16` | R2, `Fill(new Widget("f"))` | yes, no diagnostic | prints `f`; no `dtor f` | the same |
| `p12` | R2 at trivial-dispose `Box` | yes | prints `7` | prints `7` |
| `p05` | R3, `new Array<Widget>(3, new Widget("f"))` | yes, no diagnostic | prints `f`; no `dtor f` | the same |
| `p19` | R3 at `size = 0` | yes | no `dtor z` at any point | — |
| `p09` | `sp[0] := new Widget("s")`, `sp` a span over `ws` | yes, no diagnostic | prints `s`; no `dtor s` | the same |
| `p07` | `s.UnionWith(MakeSet())` on a borrowing `Set<Widget>` | checked: yes; unchecked: no ("produced no value … not implemented by this compiler (#2457)") | prints `Count 0`: the union is a no-op | does not compile |
| `p10` | `ws.AddRange([new Widget("l")])` | checked: yes; unchecked: no (#2457) | `IndexOutOfRangeException` at `ws[0]`: the call is a no-op | does not compile |
| `q11` | `Console.WriteLine(LoadAll()[0].Name)`, no store | yes | prints `x`, `after`; no `dtor x` | — |
| `q10` | `Console.WriteLine(LoadAll().Count)` | yes | prints `1`, `dtor x`, `after` | — |
| `p18` | control: `Use(new Widget("u"))`, a user static method's plain parameter | yes | prints `u`, `after`, then `dtor u` | — |

So the checked build does not fault because the argument is never disposed. The disposed flag is not set and then left unread: there is no flag to read.

- *An owning temporary.* This is not specific to the many-element forms. `Console.WriteLine(LoadAll()[0].Name)` leaks `LoadAll()` with no store at all (`q11`), while `LoadAll().Count` disposes it (`q10`). The mechanism reads as "an owning temporary whose element is read through an indexer or a value-copy is not disposed at K8" (HYPOTHESIS). `p02`, `p03`, `q07`, `q13`, `q14` and `q15` leak the same way.
- *`Fill` and `fill`.* The fresh value is treated as stored, not borrowed, as 0207's `g15` (`ws.Add(move w)` not faulting) was. This may share a mechanism with [#7428](https://github.com/s8lap/bezel-lang/issues/7428) (HYPOTHESIS).

The ASan leg could not run on this host: the probe toolchain lacks `libclang_rt.asan`. The evidence is the destructor's output, which never appears, even at program exit.

`p07` and `p10` matter beyond the leak. `UnionWith` from an `IEnumerable<T>` and `AddRange` of a literal are #2457-class unimplemented members in an unchecked build, and silent no-ops in a checked one. That affects (b)'s cost (§4.2) and the negative control of case `s15_4_…e` (§10). `p10` should also meet §15.4's literal-site rule, since the literal's element is a fresh non-trivial producer.

---

## 4. Options

### 4.1 (a) Keep proposal 0207's scope

*The rule.* The fresh-store rule stays at the single-element stores. `AddRange` and `UnionWith` copying a temporary's element borrows (R1), `Fill`'s value (R2), `fill` (R3) and the span-view indexer-set stay admitted under §28.11's borrowing-collection store residual, and a later read meets §28.2's backstop. Since §2.4, this is not "no change". R1's admitting reading must now be written in as a new admission beside a shape-(i) copy that the text rejects (§8.7). The compiler fixes the leaks (§3), so a checked build faults at the first read.

*Traced.*
- R1: admitted by §8.7's sentence. After the leak fix the checked build raises `DisposedAccessException` at `ws[0].Name`, and the unchecked build reads freed memory.
- R2 and R3: the same.
- §2.4's shapes stay rejected: `var a := LoadAll().ToArray()` and `new LinkedList<Widget>(LoadAll())` are errors, while `ws.AddRange(LoadAll())` compiles.
- The spec's own examples are unchanged: §28.11's `_all.AddRange(items)` admitted, `_all.AddRange(local)` rejected, stdlib §4.15's `Fill(myClass)` over a named value admitted.

*The case for it*, stated in full because it may be the better answer:

1. **The evidence base does not carry over.** 0207 reversed a ruled decline on three grounds, and one was empirical: the warning it promoted "ran at that scope from its implementation (#3364) with no false positive on record" (rationale §5.28.6). It promoted the lint's predicate "unchanged". The new positions never carried a warning, so there is no record of how the predicate behaves on real code. (b) would be the first rejection at these positions, with no ramp.
2. **Nothing in the tree writes these shapes.** The reach scans (§6), the author's and the critic's, find no test, rule-corpus case, doc fragment, guide or spec example with:
   - a fresh owning collection passed to `AddRange`/`UnionWith`;
   - a fresh class value passed to `Fill`, as `fill`, or at a span-view indexer-set.

   0207's shape had 62 gtest rows; this one has none. A rule guarding a shape nobody writes still costs a diagnostic, a predicate, test rows and paragraphs in four documents. The subtraction review ([`subtraction-review-2026-08.md`](subtraction-review-2026-08.md)) counts that cost.
3. **Some line remains.** Under (b), an iterator call with enumerator-owned yields stays admitted (`ws.AddRange(Gen())`, `p21`: `DisposedAccessException` when checked, a segfault when unchecked). The rule's boundary moves from "single-element against many-element" to "a value sourced at a temporary against an iterator's own yields". That is principled, but a reader must still learn it.
4. **The backstop is the recorded answer for what the analysis does not reach.** 0205 and 0207 share a posture: "where the fault is certain and the analysis exists, the language says so at compile time; where it is conditional, the backstop stays the answer" (0207 §6). These shapes are certain, but the analysis for them is unwritten: the compiler's `AddRange` arm checks scope-bound locals only, and nothing checks `Fill` or `fill`.
5. **The cost is small and lands soon.** The leak fixes are needed anyway. With them, every one of these programs faults in a checked build at its first read, the conformance-mode default ([#4602](https://github.com/s8lap/bezel-lang/issues/4602)).
6. **No churn on a rule that has just landed.** 0207's error landed in the compiler as `d00ad3a33` (narrow #3047, 2026-11-04), three weeks before this proposal; #3047 stays open for lint B. The extension adds arms beside a freshly landed one, before the error has a release's field record of its own.

*The case against it.*

1. **One copy shape gets two verdicts.** With §2.4, `var a := LoadAll().ToArray()` and `var l := new LinkedList<Widget>(LoadAll())` are rejected (shape (i)), while `ws.AddRange(LoadAll())` is admitted (shape (ii)). All three copy a temporary's element borrows into a destination that outlives the statement. They differ only in whether the copy lands in a result or in the receiver, which the copy rule itself does not distinguish ("the destination is where the result lands …, the receiver, or the written span's viewed storage"). §8.7's sentence must say so in terms.
2. **It keeps the inversion `p06`/`p01` and writes it into the spec.** A nested-block local copied by `AddRange` is rejected, and a full-expression temporary copied by `AddRange` is admitted.
3. **`Fill` and `Add` have the same signature shape** (a bare `T`, no return, shape (ii)), and K8.4's stated reason for its exception covers both (§2.3). Yet `sp.Fill(new W())`, which writes the same dead reference into every slot, would be admitted next to a rejected `ws.Add(new W())`.
4. **An unchecked build reads freed memory with no diagnostic**, the harm 0207 §2 measured as decisive for severity.

*Cost.* Text: two sentences (§8.7), plus a ruling on the span-view indexer-set (§12 question 3). Compiler: the leak fixes only.

*Migration.* None.

### 4.2 (b) Extend at the store positions and to temporary-sourced bulk copies (recommended)

*The rule.* §15.4's fresh-store rule gains two arms.

1. *The fresh value itself*, at a closed list of positions where a standard-library store places its argument into a borrowing slot:
   - the single-element stores as today (`Add`, `Insert`, `Push`, `Enqueue`, `TryAdd`, `LinkedList<T>`'s `AddFirst`/`AddLast`/`AddBefore`/`AddAfter`, `GetOrAdd`'s key, `Counter<K>.Increment`'s key);
   - the indexer-set, with key and value positions alike;
   - the indexer-set of a span view over a borrowing array (`ArraySpan<T>`'s, which §28.11 already calls "a single-element store in its own right");
   - `ArraySpan<T>.Fill`'s `value`;
   - `Array<T>(size, fill)`'s `fill`.

   The value set is 0207's, unchanged: a `new` expression, a collection literal, an `owned`-returning call, a `move` result, a `Task`-family call, at any class type, trivial-dispose ones included. The list is closed, as 0207's is: a future standard member that places its argument into a borrowing slot joins it by a list edit.
2. *A bulk copy whose placed values are sourced at a fresh temporary*: `Array<T>.AddRange` and `Set<T>.UnionWith`, from a collection argument whose placed values carry, as their source, a fresh unbound owned temporary of the call's own full expression. Two forms qualify:
   - (2a) the argument is itself a fresh temporary of a type whose element slots are owned, at a class or interface element type. That is an `Owning…` collection (stdlib §4's naming note: `OwningArray<T>`, `OwningLinkedList<T>`, `OwningSet<T>`, `OwningStack<T>`, `OwningQueue<T>`, `OwningSlotMap<T>`, and `OwningBuffer<T>` at a class `T`) or an owning collection interface (stdlib §4.2). An owning collection's reads are sourced at the collection, so `ws.AddRange(LoadAll())` and `ws.AddRange(move o)` qualify.
   - (2b) the argument is a frame that carries such a temporary, as §28.3 *Deferred-activation callees* computes a frame's carried sources from its receiver and plain arguments: `ws.AddRange(LoadAll().Where(p))`, or `ws.AddRange(GenFrom(LoadAll()))` for an iterator that takes the temporary as a plain argument. This is the critique's (c1). It needs no new source fact: §28.3 *Temporaries as sources* already rejects binding such a frame (`q14`, a compiler divergence).

The verdict is 0207's: a compile error at the call, whatever the destination's own lifetime.

*What it does not reach.*
- `LinkedList<T>(source)`, `ToArray` and the other shape-(i) copies of a temporary are already rejected (§2.4).
- `ArraySpan<T>.CopyFrom` is not among the bulk copies: its `ReadOnlyArraySpan<T>` parameter views only a borrowing `Array<T>` (stdlib §4.15), so no owning argument reaches it, and a borrowing array is a transit point.
- A fresh borrowing-family temporary (`p23`) is outside: its reads are sourced where its elements came from.
- An iterator call whose yields are enumerator-owned (`ws.AddRange(Gen())`) is outside. That is (c), declined in §4.3, and it stays at the backstop as a stated residual.

*Traced.*
- R1, R2, R3, `q05` and `p09` are rejected at the call.
- `ws.AddRange(LoadAll().Where(p))` (`p22`) is rejected by arm 2b.
- `p23` (a fresh borrowing temporary), `p15` (a named owning local, same scope) and `p06` (still rejected, by clause 2) are unchanged.
- A value type, a `string` or a box as `fill` (`new Array<Pt>(2, new Pt(1, 1))`, `new Array<Object>(1, new Pt(1, 2))`) is outside, by 0207's value set.
- `p21` stays admitted under the residual, stated in terms.
- The spec's own examples are unchanged.

*Soundness.* Both arms are local and certain.
- Arm 1 reads the argument's producer classification (`producerState`, which 0207's arm already reads).
- Arm 2a reads that classification and one type head.
- Arm 2b reads the frame's carried sources, which K8.4 and §28.3 already require for the frame's own binding.

There is no flow analysis and no callee body. The rejected programs that would not fault are dead stores: an empty span, `size = 0`, an empty owning result. That is the class 0207 §4.3 item 1 already rejects at the single-element stores and §28.6 rejects at a field.

*Ergonomics.* The remedy is one line in each case.
- Bind the source first: `using var src := LoadAll(); ws.AddRange(src);` (the store rule then compares `src`'s scope with `ws`'s), or `using var w := new Widget("f"); span.Fill(w);`.
- For `ws.AddRange(move o)`, drop the `move` (`ws.AddRange(o)`), after which the store rule compares `o`'s scope.
- Use the owning family's draining overload: `OwningArray<T>.AddRange(owned items)`, `OwningSet<T>.UnionWith(owned other)`.

`Fill` and `fill` have only the first remedy. One owner cannot hold N references (stdlib §4.14, *Why the mutation methods differ by category*), so there is no owning `Fill`. A program that wants N distinct owned values builds them in a loop.

*Cost* (estimated from `src/` read-only; non-normative).
- Neither head gate names `ArraySpan` today: `checkSec154FreshStoreCall` (`src/ownership/Ownership_Checks.cpp:18493`) and the indexer-set gate (`:18691`) list `Array … LinkedList` only, which is why `p09` compiles. Both gain it, and the call arm admits `Fill`: about 10 lines.
- The constructor arm is new: at a `new Array<T>(size, fill)` with two arguments, `rejectSec154FreshStore` on `args[1]`. About 15 lines.
- Arm 2a sits in the existing bulk-store path (`bulkStoring`, around `:17644`, today `AddRange` only, judging a scope-bound local argument). `UnionWith` joins it, and a new branch rejects an argument whose `producerState` is owned at an owned-slot head over a class element, with one diagnostic variant naming the element borrows. About 30 lines. `UnionWith` from an `IEnumerable<T>` is unimplemented in an unchecked build and a no-op in a checked one (`p07`, #2457). The arm can land before that implementation, but its gtest row must be a compile-rejection row.
- Arm 2b depends on the compiler tracking a lazy frame's carried temporary, which it does not do today (`q14`). Once that §28.3 divergence is fixed, the arm is a few lines in the same branch. Until then it is spec-ahead-of-impl with the frame bug.
- Tests: about 15 rows, with the §10 corpus cases as the controls.

This is one lane, single-file in ownership, disjoint from the `typeCall`/`lowerBinary` hot functions. The leak fixes (§9.1) are separate codegen items under either option.

*Migration.* None measured (§6). No test, banked corpus case or document example flips. One test comment must be corrected (§6).

### 4.3 (c) Also reach iterator calls with enumerator-owned yields

*The rule.* (b), plus a bulk copy from an iterator call whose yields are enumerator-owned (§29.2): `ws.AddRange(Gen())`, where `Gen()` yields `new Widget("g")`. Here the placed value's source is the iterator's own frame, a temporary, rather than a temporary the frame carries. (The critique called this (c2); its (c1), a frame that carries a temporary, is part of (b) as arm 2b.)

*Traced.*
- `p21`: rejected. MEASURED today: it compiles silently, raises `DisposedAccessException` when checked, and segfaults when unchecked.
- An imported iterator (`ws.AddRange(lib.Items())`): the verdict needs the iterator's yield classification. That is a same-unit, three-valued fact, and its cross-module form is deferred (`DECLINED.md` row 634). The rule would either over-reject every imported iterator at a class element, including a sound one that yields borrows of a long-lived field, or split its verdict by module.

*Soundness.* At an iterator with mixed yields the source is a may-source: values are sourced at the frame only on the paths that yield owned values. That is §29.11's terminal-accessor territory, which the record keeps at a warning. A materializer-site diagnostic is deferred (row 733, which names `for var x in Load(p).ToArray()` as "a silent certain fault" and still defers it). Source-chain tracing through iterator bodies is declined, with a counterexample: a frame-resident borrowing collection whose elements outlive the frame (row 950).

*Cost.* It would thread the terminal-accessor trace's per-iterator yield summary into the store check, in a second file.

*Migration.* Unmeasured for imported iterators; zero in the tree for same-unit shapes.

*Declined.* It reopens rows 733 and 950 without meeting their bars, and it pays the frame machinery's cost for a shape no program in the tree writes. Under (b) it stays at the backstop as a stated residual (§8.1, §8.4); §12 question 2.

### 4.4 (d) A warning at the new positions

A mandated warning at (b)'s predicate, with acceptance unchanged. This is not viable. `DECLINED.md` row 1087 declines keeping the single-element shape at a warning because it is "a certain dangle whose only sound carriers are dead stores … kept at a warning with no suppression spelling", and row 408 forbids a suppression spelling. The same ground applies, and a warning would also add a lint, which the subtraction posture weighs against more than an error that closes the shape. Declined.

---

## 5. Recommendation

(b), for these reasons:

1. **The certainty is the same as 0207's, and so is the analysis.** Each rejected call writes a reference to a value that dies at the end of its statement, on every path. The predicate reads what 0207's arm reads (the argument's producer classification), plus one type head, plus the frame sources §28.3 already requires.
2. **It gives one copy one verdict.** With §2.4, shape (i) copies of a temporary's element borrows are already rejected. (b) rejects the shape (ii) copy of the same borrows, so whether the copy lands in a result or in the receiver stops mattering.
3. **It removes an inversion against lifetime that (a) would write down.** A nested-block local and a full-expression temporary at `AddRange` take the same verdict, as they do at `Add`.
4. **The cost is small and the migration is zero.** Three compiler arms in one file (arm 2b waits on the `q14` fix), no test flips, and no corpus case moves.

The case for (a) (§4.1) is real. It is the right choice if the owner weighs these more heavily:
- the absence of a field record at the new positions;
- the absence of any program that writes the shapes.

If (a) is chosen, §8.7's sentences should land so that the admission, and its two verdicts for one copy shape, are stated rather than left to a reading. The leak fixes then become the whole of #7426.

What (b) does not do:
- it does not reach iterator calls with enumerator-owned yields (§4.3);
- it does not reject a fresh temporary at a user-declared member's plain parameter (row 68 stands);
- it edits nothing for §2.4's settled shapes beyond naming them.

---

## 6. Reach, measured

Scans over `tests/` (gtest, lit, both corpora) and `docs/`. The author's ran at `bd12fe038` and the critic's at `origin/main`; v0.361's corpus sweep touched none of the cited cases.

- **`AddRange`/`UnionWith` with a call, `new`, a literal or `move` as argument:** every hit is outside the rule.
  - `dst.AddRange(Gen())` (`array_addrange_ienumerable_2721`) yields `string`.
  - `MakeSet().UnionWith(Other())` (`linq_set_terminal_unbound_reclaim_6394`) is over `Set<int>`.
  - The `s1.UnionWith(move s2)` rows (`owned_set_algebra_1259`, `owned_set_union_1474`) and `l4_5_0011`'s `dst.AddRange(move src)` have owning receivers and use the draining overloads.
- **`Fill` with a `new`, `move` or call argument:** records only (`array_record_add_string_coown_8386`'s `E` and `P2`) and `int`. The other `Fill` hits are user methods named `Fill`.
- **`new Array<…>(n, <producer>)`:** all outside. The hits are records (`E`, `Pt`, `Pair` in `s28_11_0023`), `DateTime`, `string`, a box (`new Array<Object>(1, new Pt(1, 2))`), an enum constant, a generic `G.Id(x)` at `string`, and named function values.
- **`new LinkedList<…>(<producer>)`:** `int` only.
- **Span-view indexer-sets with a fresh value:** all over owning `Span<T>`/`OwningBuffer<T>`.
- **Nested-collection fills:** none.
- **Document examples:** no spec, stdlib, guide or code-generation reference example writes the shape. Guide 05's `header.Fill(0)` and Guide 01's `fresh.CopyFrom(_source)` are value-typed or named.

Newly rejected: nothing in the tree. Banked rule-corpus cases that change expectation: none. Doc-corpus fragments: none.

Stale needles: none. The error reuses 0207's `§15.4 fresh-store error` text with new store spellings, so no needle vanishes. The code-generation reference's callouts gain words and lose none.

One test comment must be corrected at landing. `fresh_store_warning_3364`'s `bulk_add_range_is_not_rejected` row (an `int` element, a named parameter source) is untouched by (b) and stays green. Its comment, "the source collection's own death leaves no dangling element slot", is false for an owning class source once (b) lands. It should read: "for a borrowing or value-typed source; a fresh owning source is §15.4's fresh-store error (proposal 0219)".

---

## 7. Declined alternatives (`DECLINED.md`, `declined-at-proposal`)

If (b) is accepted, three rows are added:

| decline | ground | reopening bar |
|---|---|---|
| Keeping proposal 0207's single-element scope for the many-element forms: `AddRange`/`UnionWith` copying a fresh temporary's element borrows, `ArraySpan<T>.Fill`'s value, `Array<T>(size, fill)`'s `fill` and a span view's indexer-set admitted under §28.11's borrowing-collection residual (proposal 0219 option (a)) | The same certain dangle as the single-element store, at every slot the call writes. With shape (i)'s copy of a temporary's element borrows already rejected (`LoadAll().ToArray()`, `new LinkedList<T>(LoadAll())`), keeping it gives one copy two verdicts by whether it lands in the receiver or the result. It writes into the spec an inversion against lifetime (a nested-block local rejected at `AddRange`, the full-expression temporary admitted). It leaves an unchecked build reading freed memory. | —(none stated) |
| Extending the fresh-store rule to a bulk copy from an iterator call whose yields are enumerator-owned (`ws.AddRange(Gen())`; proposal 0219 option (c)) | The placed value's source is the iterator's own frame, readable only through the per-iterator yield summary the record keeps same-unit (row 634) and the source-chain tracing declined at row 950. A may-source at an iterator with mixed yields. Would over-reject imported iterators or split the verdict by module. Stays at the backstop, a stated residual. A frame that merely carries a temporary is not this row: (b) rejects it (arm 2b). | The cross-module yield summary of row 634, or a measured program that writes `AddRange` over such an iterator |
| A warning instead of the error at the new positions (proposal 0219 option (d)) | Row 1087's ground: a certain dangle whose only sound carriers are dead stores, at a warning with no suppression spelling (row 408) | —(none stated) |

If (a) is accepted instead, the first row is replaced by a row declining option (b), on these grounds: no field record at the new positions, and zero programs that write the shapes. Its reopening bar would be a measured program, test or corpus case that writes one of the shapes. Rows (c) and (d) stand as written.

---

## 8. The normative text (option (b))

No version header or pin moves; the landing adds those and the changelog entry. Each edit quotes the v0.361 text it replaces verbatim. Each quoted anchor occurs exactly once at `cdd2022c7`.

### 8.1 Spec §15.4, the store paragraph

Replace

> in a single-element store position of a borrowing-family collection is a compile error at the store site. The positions are the single-element stores of [§28.11](#sec-borrowing-collections)'s value-copy shape (ii) — `Add`, `Insert`, `Push`, `Enqueue`, `TryAdd`, `LinkedList<T>`'s `AddFirst`/`AddLast`/`AddBefore`/`AddAfter`, `GetOrAdd`'s key, `Counter<K>.Increment`'s key — and the indexer-set, **key and value positions alike**, on every collection of the borrowing family

with

> in a store position of a borrowing-family collection is a compile error at the store site. The positions are the parameters at which a standard-library store places its argument itself into a borrowing slot: the single-element stores (stdlib [§4](Bezel_Standard_Library.md#sec-value-copy-members), *Value-copy members are stores*: `Add`, `Insert`, `Push`, `Enqueue`, `TryAdd`, `LinkedList<T>`'s `AddFirst`/`AddLast`/`AddBefore`/`AddAfter`, `GetOrAdd`'s key, `Counter<K>.Increment`'s key) and the indexer-set, **key and value positions alike**, a span view's over a borrowing array included; `ArraySpan<T>.Fill`'s value, which [§28.11](#sec-borrowing-collections)'s shape (ii) places in every slot the span views; and the `fill` argument of `Array<T>(size, fill)`, which shape (i) places in every slot of the constructed array — the list is exhaustive — on every collection of the borrowing family

After the sentence ending "so a class-typed key is held as a borrow exactly as `Dictionary<K, V>`'s is.", insert

> A bulk copy from a collection argument whose placed values are sourced at a fresh unbound owned temporary of the call's own full expression is the same error: `Array<T>.AddRange` and `Set<T>.UnionWith`, given such a temporary of a type whose element slots are owned — an `Owning…` collection or an owning collection interface (stdlib [§4.2](Bezel_Standard_Library.md#sec-owning-collection-interfaces)) — at a class or interface element type (`ws.AddRange(LoadAll())`, `LoadAll()` returning `owned OwningArray<Widget>`; `ws.AddRange(move o)`), whose reads are sourced at the collection ([§28.11](#sec-owning-collections)), or a frame carrying one as [§28.3](#sec-borrow-lifetime-rules)'s *Deferred-activation callees* computes its sources (`ws.AddRange(LoadAll().Where(p))`), place borrows its [K8](#sec-kernel-k8) boundary kills. A fresh **borrowing**-family temporary is not such an argument: its reads are sourced where its elements came from, and the store rule judges those sources. Nor is an iterator call whose yields are enumerator-owned ([§29.2](#sec-iterator-yield-ownership)): what it places is held under [§28.11](#sec-borrowing-collections)'s borrowing-collection store residual and meets [§28.2](#sec-checked-disposal)'s backstop.

After the sentence ending "for dynamic task fan-in, `OwningArray<OwningTask<T>>` with the owning `Task.WhenAll` overload, stdlib [§6.7](Bezel_Standard_Library.md#sec-task-static-helpers))." insert

> At a bulk copy the remedies are the same two: bind the source first (`using var src := LoadAll(); ws.AddRange(src);`, whose scope the store rule then compares with the receiver's — for `ws.AddRange(move o)`, drop the `move`), or use the owning family's draining overload (`OwningArray<T>.AddRange(owned items)`, `OwningSet<T>.UnionWith(owned other)`); `Fill`'s value and `fill` have the first only, one owner holding one reference (stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans), [§28.6](#sec-leak-prevention)).

The rest of the paragraph ("The rule covers the trivial-dispose shape …", the boundary-event split, "The rule reaches no other callee …") is unchanged and reads correctly over the wider set.

### 8.2 Spec §28.11, *Borrowing collections*, clause 4

Replace

> 4. a **fresh unbound owned temporary** of class type in a single-element store position — `Add`/`Insert`/indexer-set and the other stores [§15.4](#sec-collection-literals) names — is not a borrow of any named source but a temporary that dies at its K8 boundary ([§28.3.11](#sec-unbound-owned-temporaries)): the store is rejected

with

> 4. a **fresh unbound owned temporary** of class type in a store position — `Add`/`Insert`/indexer-set, `ArraySpan<T>.Fill`'s value, `Array<T>(size, fill)`'s `fill` and the other stores [§15.4](#sec-collection-literals) names — is not a borrow of any named source but a temporary that dies at its K8 boundary ([§28.3.11](#sec-unbound-owned-temporaries)), and a value that `AddRange` or `UnionWith` places from an owned-slot temporary of the same full expression, or from a frame carrying one, is a borrow of such a temporary: the store is rejected

The clause's remainder, "at the store site by [§15.4]'s fresh-store rule — clause 2's rejection with the source's scope at its narrowest …", is unchanged.

### 8.3 Spec §28.11, *Value-copy members*

After "… and `_all.AddRange(local)` and `return local.ToArray()` over a scope-bound local are rejected, as `_all.Add(local[0])` is ([#5693](https://github.com/s8lap/bezel-lang/issues/5693))." insert

> A copy of a fresh temporary's element borrows is rejected wherever it lands: into a binding, as `var a := LoadAll().ToArray()` and `new LinkedList<Item>(LoadAll())` are ([K8.4](#sec-kernel-k8)), and into the receiver, as `_all.AddRange(LoadAll())` and `_all.AddRange(LoadAll().Where(p))` are under [§15.4](#sec-collection-literals)'s fresh-store rule; an iterator call whose yields are enumerator-owned is not such a copy, and holds what it yields under the residual above.

### 8.4 Spec K8.4, the exception sentence

Replace

> The single-element store positions of the standard borrowing-family collections, and `OwningDictionary<K, V>`'s key, are the exception: their declared signatures are read as stores ([§28.11](#sec-borrowing-collections) *Value-copy members*; the owning map's key is held as a borrow, [§28.11](#sec-element-slot-policy)), so the retention is announced and a fresh unbound owned temporary there is [§15.4](#sec-collection-literals)'s fresh-store error.

with

> The store positions [§15.4](#sec-collection-literals) names — the single-element stores of the standard borrowing-family collections and of a span view over one, `ArraySpan<T>.Fill`'s value, `Array<T>(size, fill)`'s `fill`, the bulk copies `AddRange` and `UnionWith` from a collection argument whose placed values are sourced at a fresh temporary, and `OwningDictionary<K, V>`'s key — are the exception: their declared signatures are read as stores ([§28.11](#sec-borrowing-collections) *Value-copy members*; the owning map's key is held as a borrow, [§28.11](#sec-element-slot-policy)), so the retention is announced, and a fresh unbound owned temporary there, or one whose element borrows such a copy places, is [§15.4](#sec-collection-literals)'s fresh-store error. An iterator call whose yields are enumerator-owned, passed to a bulk copy, is not among them: what it places is held under [§28.11](#sec-borrowing-collections)'s borrowing-collection store residual.

The sentence before it ("A plain parameter announces none: …") is left as v0.361 has it. §13 covers 0212's rewrite of it.

### 8.5 Spec Appendix C.3

In the bullet beginning "A fresh-producer element of non-trivial-dispose class type in a borrowing-family collection literal is rejected", replace

> in a single-element store position of a borrowing-family collection (`Add`/`Insert`/indexer-set and the other stores [§15.4](#sec-collection-literals) names, key and value positions alike) or in the key position of `OwningDictionary<K, V>` is rejected at the store site,

with

> in a store position of a borrowing-family collection (`Add`/`Insert`/indexer-set, `ArraySpan<T>.Fill`'s value, `Array<T>(size, fill)`'s `fill` and the other stores [§15.4](#sec-collection-literals) names, key and value positions alike) or in the key position of `OwningDictionary<K, V>`, and an owned-slot temporary, or a frame carrying one, whose element borrows `AddRange` or `UnionWith` places, are rejected at the store site,

### 8.6 Standard library

- **§4, *Value-copy members are stores*.** After "… are stores in their own right under the slot policy's store row (language spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy)), whatever they return." append: "Language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)'s fresh-store rule reaches these positions among them: a fresh class value where a member places its argument itself into a borrowing slot (the single-element stores, an indexer-set, `ArraySpan<T>.Fill`, `Array<T>(size, fill)`'s `fill`), and a bulk copy by `AddRange` or `UnionWith` whose placed values are sourced at a fresh temporary, are compile errors at the call."
- **§4.4 `Array<T>`.** After "To have elements disposed, use `OwningArray<T>` ([§4.5](#sec-ownedarrayt)) — element disposal never happens through the borrowing type (spec §28.6)." append: "A fresh class value as `fill`, and a fresh owning collection (or a query over one) as `AddRange`'s argument, are spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)'s fresh-store error: the array would hold borrows of a value its statement's end disposes. Bind it first, or use `OwningArray<T>.AddRange(owned items)`."
- **§4.10 `Set<T>`.** At `public method UnionWith(other: IEnumerable<T>);` add the comment `// a fresh OwningSet (or a query over one) as other: spec §15.4's fresh-store error; use OwningSet<T>.UnionWith(owned other)`.
- **§4.15 `ArraySpan<T>`, *Bulk mutation*.** Replace "For class T, the assigned-into slot stores a borrow (`Fill(myClass)` makes every slot borrow `myClass`); for value T, the slot stores a copy." with "For class T, the assigned-into slot stores a borrow (`Fill(myClass)` makes every slot borrow `myClass`, and a fresh class value there, as at the indexer-set, is spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)'s fresh-store error); for value T, the slot stores a copy."

### 8.7 The text for option (a), if it is chosen instead

The admission is new text either way, because §2.4's rejection of shape (i) leaves the admitting reading of R1 without a ground of its own.

- **K8.4**, after "… is [§15.4](#sec-collection-literals)'s fresh-store error." insert: "The many-element forms are not among them: a fresh temporary, or a borrow of one, that `AddRange`, `UnionWith` or `Fill` places in its receiver, or that `Array<T>(size, fill)` places as `fill`, is held under [§28.11](#sec-borrowing-collections)'s borrowing-collection store residual, the temporary disposed at its boundary — although the same copy into a binding (`var a := LoadAll().ToArray()`) is rejected."
- **§28.11 *Value-copy members***, after the #5693 sentence insert: "A temporary's element borrows copied into the receiver by a many-element form (`_all.AddRange(LoadAll())`) are admitted under the residual above and meet [§28.2](#sec-checked-disposal)'s backstop; copied into a result that is bound (`LoadAll().ToArray()`, `new LinkedList<Item>(LoadAll())`) they are [K8.4](#sec-kernel-k8)'s rejected escape."
- **§15.4**, a ruling on the span-view indexer-set (§12 question 3).

Under (a), no other document changes.

### 8.8 Rationale §5.28.6

After the paragraph *The store lint is promoted to an error*, add:

> **The many-element forms ([proposal 0219](proposals/0219-fresh-store-at-the-many-element-forms.md)).** The promotion took the lint's single-element predicate, and its one recorded ground for leaving `AddRange` out — that a bulk copy's source collection dies without leaving a slot dangling — holds for a borrowing source and fails for an owning one, whose reads are sourced at the collection its boundary disposes. The many-element forms carry the same certain dangle at every slot they write: `ws.AsSpan().Fill(new W())` makes every slot borrow one value its statement's end disposes, `new Array<W>(n, new W())` does the same to a fresh array, and `ws.AddRange(LoadAll())` over a fresh owning collection places borrows of elements that collection's boundary disposes — the copy shape (i) already rejects when it lands in a binding (`LoadAll().ToArray()`). Left admitted, `AddRange` would also reject a nested-block local and admit the full-expression temporary, the inversion against lifetime the promotion removed for `Add`. So the rule reaches every position at which a standard store places a fresh value itself, and every bulk copy whose placed values are sourced at a fresh temporary, a frame that carries one included. An iterator's enumerator-owned yields stay outside: their source is readable only through the per-iterator yield summary the record keeps same-unit, so they stay at the backstop, a stated residual. No program in the tree wrote the shapes when the rule landed; the decision rests on the certainty and the consistency, with the absence of a field record at these positions stated as its cost.

### 8.9 Code-generation reference

- **§4.5, *Fresh values into collections*.** Replace "On the **borrowing** family the same shape via `Add`/`Insert`/indexer-set is a compile **error** (the temporary would die at the statement's end and the slot dangle)" with "On the **borrowing** family the same shape via `Add`/`Insert`/indexer-set, `span.Fill(…)` or `new Array<T>(n, …)`'s fill is a compile **error** (the temporary would die at the statement's end and the slot dangle), as is `AddRange`/`UnionWith` of a fresh **owning** collection or a query over one (`ws.AddRange(LoadAll())`: bind it first, or use `OwningArray<T>.AddRange(owned …)`)".
- **§7.2, the fresh-store callout.** Replace "in `Add`/`Insert`/indexer-set position (and a fresh `OwningDictionary` key) is rejected at the store site" with "in `Add`/`Insert`/indexer-set position, as `span.Fill`'s value or `new Array<T>(n, fill)`'s fill (and a fresh `OwningDictionary` key), and a fresh owning collection (or a query over one) given to `AddRange`/`UnionWith`, is rejected at the store site".
- **§7.2, the value-copy callout.** Replace "and at a class element whenever the source is a scope-bound local." with "and at a class element whenever the source is a scope-bound local or a fresh temporary, wherever the copy lands (§15.4's fresh-store error at `AddRange`/`UnionWith`; K8.4 at a bound `ToArray()`)."

---

## 9. Changelog sketch

*§15.4, §28.11 (clause 4, *Value-copy members*), K8.4, Appendix C.3; stdlib §4, §4.4, §4.10, §4.15; rationale §5.28.6; the code-generation reference §4.5, §7.2. The fresh-store rule reaches the many-element forms (design change; proposal 0219; #7426's spec half).*

- §15.4's fresh-store positions are now an exhaustive list of the parameters at which a standard store places its argument itself into a borrowing slot. The list is the single-element stores as before, a span view's indexer-set, `ArraySpan<T>.Fill`'s value, and `Array<T>(size, fill)`'s `fill`.
- The rule also reaches `AddRange` and `UnionWith` from a collection argument whose placed values are sourced at a fresh temporary of the same full expression: an owned-slot temporary, or a frame carrying one.
- An iterator call whose yields are enumerator-owned is a stated residual at the backstop.
- K8.4's exception sentence names the same set and anchors its residual to §28.11's.
- The shape-(i) copies of a temporary's element borrows (`LoadAll().ToArray()` bound, `new LinkedList<T>(LoadAll())`), already K8.4's rejected escapes, are named beside it.

*Migration.*

- **Newly rejected (spec-ahead-of-impl).**
  - A fresh unbound owned temporary of class type as `ArraySpan<T>.Fill`'s value, as `Array<T>(size, fill)`'s `fill`, or at a span view's indexer-set over a borrowing array.
  - A fresh owned-slot collection at a class element, or a frame carrying one, given to `Array<T>.AddRange` or `Set<T>.UnionWith`.
  - Measured reach: no gtest row, lit row, banked corpus case or document example.
  - Remedy: bind to a named holder first (for a `move` argument, drop the `move`), or use the owning family's draining overload (`AddRange`/`UnionWith` only).
- **Unchanged.** A fresh borrowing-family temporary at a bulk copy; an iterator call with enumerator-owned yields (residual); a user-declared member's plain parameter; value-typed, `string` and boxed fills.
- **Newly warned:** nothing. **Ramps:** none.
- **Test comment corrected:** `fresh_store_warning_3364`'s `bulk_add_range_is_not_rejected` (§6).

*Owner decisions recorded:* option (b) over (a) (§12 question 1), and questions 2–4 as ruled. *Coordinator ruling recorded:* §2.4, clarification class, 2026-11-26.

### 9.1 Compiler bugs that hold under every option

To file for triage at landing. None of them depends on the ruling.

| probe | bug | ground |
|---|---|---|
| `q11` (with `p01`, `p02`, `p03`, `q07`, `q13`, `q14`, `q15`) | An owning temporary whose element is read through an indexer or a value-copy is not disposed at its K8 boundary: `Console.WriteLine(LoadAll()[0].Name)` leaks with no store, while `LoadAll().Count` disposes (`q10`). This is #7426's question 1. | K8.1, §28.3.11 |
| `p04`, `p05`, `p19`, `p09` | A fresh class value at a stdlib bare-`T` store (`Fill`, `fill`, a span-view indexer-set) is never disposed. Possibly #7428's mechanism. | K8.1 |
| `p07` | `Set<T>.UnionWith(other: IEnumerable<T>)` is a silent no-op in a checked build and unimplemented in an unchecked one. | stdlib §4.10; #2457 |
| `p10` | `ws.AddRange([new Widget("l")])` is a silent no-op when checked and unimplemented when unchecked; it should meet §15.4's literal-site rule. | stdlib §4.4, §15.4; #2457 |
| `q13` | `new LinkedList<Widget>(LoadAll())` is admitted (and leaks). | §2.4: §28.11 shape (i), K8.4 |
| `q14` | `var q := LoadAll().Where(p)` is admitted (and leaks). | §28.3 *Temporaries as sources* |
| `p02`, `q08` | `ws.Add(LoadAll()[0])` and `ws.Add(LoadAll().First())` are admitted. | §2.4: K8.4, announced retention |
| `p03`, `q15` | `var a := LoadAll().ToArray()` and its frame twin are admitted. | §2.4: §28.11 shape (i), K8.4 |
| `p18` | A plain-argument temporary is disposed at the end of the method, not at the end of its statement. | K8.1 |
| `p24` | `for var g in Gen() do ws.Add(g); end` (an enumerator-owned yield stored into an outer collection) compiles silently and faults. | §28.11 clause 2 (the hidden `__source` binding) |

---

## 10. Proposed rule-corpus cases

Verdicts follow the cited sentence of the landed text (§8), never the compiler. Ids are assigned at harvest (the next free `s15_4_` / `s28_11_` numbers). Each is a full program in the form of `s15_4_0016`. `Item` has a printing destructor and a public `Tag: int` field; `Box` is trivial-dispose; `LoadAll(): owned OwningArray<Item>` and `LoadSet(): owned OwningSet<Item>` each hold one fresh `Item`.

| case | program core | expect | cites |
|---|---|---|---|
| `s15_4_…a` | `var a: Array<Item> := []; a.AddRange(LoadAll());` | `reject`, `diagnostic: §15.4 fresh-store error` | 15.4, 28.11 |
| `s15_4_…b` | `using var w := new Item(); var a: Array<Item> := []; a.Add(w); var sp := a.AsSpan(); sp.Fill(new Item());` | `reject` | 15.4, 28.11 |
| `s15_4_…c` | `var a := new Array<Item>(3, new Item());` | `reject` | 15.4, 28.11 |
| `s15_4_…d` | `var bs := new Array<Box>(2, new Box());` (trivial-dispose: the rule's full type set) | `reject` | 15.4, 28.9 |
| `s15_4_…e` | `var s: Set<Item> := []; s.UnionWith(LoadSet());` | `reject` (a compile-time verdict, independent of `p07`'s unimplemented member) | 15.4, 28.11 |
| `s15_4_…f` | `using var w := new Item(); var a: Array<Item> := []; a.Add(w); var sp := a.AsSpan(); sp[0] := new Item();` | `reject` (§12 question 3) | 15.4, 28.11 |
| `s15_4_…g` | `var a: Array<Item> := []; a.AddRange(LoadAll().Where(lambda(x: Item) => true));` | `reject` (arm 2b) | 15.4, 28.11, 28.3 |
| `s15_4_…h` (control) | `using var src := LoadAll(); var a: Array<Item> := []; a.AddRange(src); Console.WriteLine(a.Count);` | `run`, `stdout: 1` | 15.4, 28.11 |
| `s15_4_…i` (control) | `using var w := new Item(); var a: Array<Item> := []; a.AddRange(Pass(w)); Console.WriteLine(a.Count);` (`Pass(w: Item): owned Array<Item>` holding `w`: a fresh borrowing temporary) | `run`, `stdout: 1` | 15.4, 28.11 |
| `s15_4_…j` (residual) | `var a: Array<Item> := []; a.AddRange(Gen());` (`Gen(): IEnumerable<Item>` yields `new Item()`), then `try Console.WriteLine(a[0].Tag); catch var ex: DisposedAccessException => Console.WriteLine("faulted"); end try;` | `run`, `stdout: faulted`, `flags: --checked-disposal` | 15.4, 28.11, 28.2, 29.2 |

§2.4's settled shapes are the harvest's own cases, independent of this proposal: `new LinkedList<Item>(LoadAll())` `reject` (cites 28.11, K8.4); `var a := LoadAll().ToArray();` `reject`; `a.Add(LoadAll()[0]);` `reject`.

Existing cases that state neighbouring rules and stand: `s15_4_0016` (single-element), `s28_11_0019`/`0020`/`0024` (#5693's `AddRange` from a parameter or a scope-bound local), `s28_11_0023` (a record `fill`), `l4_5_0011` (the owning `AddRange`).

---

## 11. Tracker sketch (to file at landing, `spec-tracking`)

*§15.4 / §28.11 / K8.4 (v0.3NN, proposal 0219): the fresh-store rule at the many-element forms.*

1. Add `ArraySpan` to both head gates (`checkSec154FreshStoreCall`, `:18493`; the indexer-set gate, `:18691`) and `Fill` to the call arm.
2. Add a `new Array<T>(size, fill)` arm on `fill`.
3. In the bulk-store path, add `UnionWith` beside `AddRange` and reject an argument whose producer state is owned at an owned-slot head (`OwningArray`, `OwningLinkedList`, `OwningSet`, `OwningStack`, `OwningQueue`, `OwningSlotMap`, `OwningBuffer` at a class `T`, the owning interfaces) over a class element, with a diagnostic naming the element borrows and both remedies (and the drop-the-`move` spelling).
4. Arm 2b (a frame carrying such a temporary) follows the `q14` fix.
5. Add the §10 corpus cases and about 15 gtest rows, each proven fail-without.
6. Correct `fresh_store_warning_3364`'s `bulk_add_range_is_not_rejected` comment (§6).

The §9.1 bugs are filed separately and do not wait on this tracker. Two leak mechanisms are involved: the owning temporary indexed or copied (`q11`), and the fresh value at a stdlib bare-`T` store (`p04`/`p05`).

---

## 12. Owner questions

1. **(b) or (a)?** Extend the fresh-store rule to `Fill`, `fill`, the span-view indexer-set and the temporary-sourced bulk copies (recommended, §5)? Or keep 0207's scope, with the admission written in terms (§4.1, §8.7)? Under (a), `LoadAll().ToArray()` and `new LinkedList<T>(LoadAll())` are rejected while `ws.AddRange(LoadAll())` is admitted: one copy, two verdicts. Either way the §9.1 bugs are fixed.
2. **Iterator calls with enumerator-owned yields.** Under (b), `ws.AddRange(Gen())` (`p21`: it faults when checked and segfaults when unchecked) stays at the backstop, a stated residual. Confirm the residual (recommended, §4.3), or take (c) and accept the cross-module question row 634 defers? This no longer concerns a lazy frame over a temporary: §28.3 already sources that, and (b) rejects it (arm 2b).
3. **A span view's indexer-set.** `sp[0] := new Widget("s")` (`p09`) compiles silently today. §15.4 names the indexer-set "on every collection of the borrowing family", and stdlib §4.1's family does not include span views. Recommended: covered. Under (b) it is in §8.1's list. Under (a) it is a ruling, not a clarification: it narrows the span view's admission, as §2.3 says of `Fill`. It would land with §8.7's sentences.
4. **A fresh value into a fresh result.** In `new Array<Item>(3, new Item()).Count`, both values die at one boundary and nothing can read the slots. 0207 rejects the analogous temporary receiver uniformly (`new Array<W>().Add(new W())`). Under (b) the fill constructor is rejected the same way, whatever its result's landing (recommended: one verdict per position).

The draft's question 4 (`p02`, `p03`) is ruled (§2.4) and is no longer an owner question.

---

## 13. Interaction with proposal 0212

Accepted proposal 0212 (`accepted — staged`; stage 1 graduated v0.357, stages 2–3 scheduled) §8.3 replaces K8.4's sentence "A plain parameter announces none: a callee that stores it holds the borrow under the borrowed-field rule's or the borrowing-collection store rule's recorded residual … (K8.1)" with "A plain parameter announces none, and its callee may not keep it (§28.15 *The callee side*) — a constructor's excepted …".

The first draft anchored its residual to "the residual above", which after 0212 stage 3 would no longer name the borrowing-collection residual. This revision anchors every residual reference (§8.1, §8.3, §8.4, §8.7) to §28.11's borrowing-collection store residual (*The residual is a K4.3-recorded trust clause …*), which 0212 leaves in place. So:

- **Order.** 0219 edits K8.4's exception sentence, the one after the sentence 0212 rewrites, and quotes neither sentence's neighbour. The two edits apply in either order.
- **Meaning.** After 0212 stage 3, a standard borrowing-collection member is still read off its declared signature (§28.11 *Value-copy members*). 0212's "its callee may not keep it" governs user-declared callees under §28.15. So 0219's positions remain the exception that K8.4 names, and its residual for enumerator-owned yields remains §28.11's.
- **Re-read at graduation.** §2.3's admitting reading of R1 rests on the v0.361 plain-parameter sentence. Under 0212's rewrite that reading loses the words "holds the borrow under … the borrowing-collection store rule's recorded residual", so it would weaken further. That does not affect (b), but it changes how (a)'s §8.7 sentence must be phrased if (a) lands after stage 3.

---

## 14. Self-critique

- **Is arm 2a keyed on a type head rather than a source?** Yes, for a fresh argument the two coincide: an owned-slot collection's reads are sourced at it. An `owned IEnumerable<Item>` result that is an owning collection at run time is not reached, because its static type is not owned-slot. That is the conservative direction: such a result is admitted under the residual, as an enumerator-owned iterator is.
- **Does §2.4's ruling decide `fill` too?** No. The ruling concerns placed values that are borrows of a temporary's elements. `fill` places the fresh temporary itself, which K8.4 governs through the plain-parameter sentence and the enumerated exception, exactly as `Add(new W())` was governed before 0207. So it stays a design change (§2.3).
- **Does (b) reopen row 68?** No. It reads no callee body. Every position it adds is a standard member whose signature §28.11 already reads as a store.
- **Does (b) contradict 0207 §4.3's "a many-element form … stays accepted"?** It changes that statement. §1.3 gives the recorded ground and the evidence against it for an owning source.
- **Is the reach really zero?** Two independent regex scans over call spellings, each hit read, found none (§6). A store reached through a method group (`var f := ws.AddRange; f(LoadAll())`) is not matched; none exists in `tests/` or the normative documents (`git grep -n "\.AddRange;\|\.Fill;"`: the one hit, in `docs/research/generics-matrix/`, is a method reference to a user extension named `Fill`).
- **An editorial defect seen, not fixed here.** v0.361 §15.4 calls `TryAdd`, `GetOrAdd` and `LinkedList<T>`'s `AddFirst`…`AddAfter` "the single-element stores of §28.11's value-copy shape (ii)". Shape (ii) reads only members that declare no return, and these return a value. Stdlib §4 makes them "stores in their own right … whatever they return". §8.1's rewrite cites stdlib §4 instead, which removes the mislabel as a side effect. If (a) is chosen, the mislabel should be filed as an editorial defect.
- **The worst case for (b).** A program that fills a borrowing array with one fresh trivial-dispose value as a sized placeholder (`new Array<Box>(n, new Box())`) and reads only `Count` is rejected. Its remedy is one line (`using var b := new Box(); var bs := new Array<Box>(n, b);`). Zero sites.

---

## 15. Critique record

**Round 1** (`pc-0219`, a non-author adversarial critic, read-only, on `acb4c8b5b`). Verdict: SOUND-WITH-FIXES, 14 findings. All are applied in this revision:

- **F1.** The record: 0207's one stated ground, from the 3364 control's comment, is quoted and answered (§1.2, §1.3). The draft's grep used a basic-regex `|` and missed it.
- **F2.** An exhaustive list replaces "by reference to the register" (§4.2, §8.1).
- **F3.** Q4's coupling to R1, and the corrected `p03` citation (announcement-table row 1, not row 4). Superseded by the coordinator's ruling (§2.4).
- **F4.** (c) is split: the critic's (c1), a frame carrying a temporary, joins (b) as arm 2b; (c2), enumerator-owned yields, stays declined (§4.3, §7, §12 question 2).
- **F5.** The K8.4 anchor moves to §28.11's residual, with a 0212 section (§13).
- **F6.** The stale spec-ahead-of-impl claim is corrected (`d00ad3a33`).
- **F7.** R2/R3 are admitted by enumeration, not by ground (§2.3).
- **F8.** Terminology: "bulk copy", and `CopyFrom`'s exclusion stated (§4.2).
- **F9.** The owned-slot head set is spelled out (§4.2, §8.1, §11).
- **F10.** Question 1's answer is broadened, and `p07`/`p10` are reported (§3, §9.1).
- **F11.** The `src/` line references are corrected.
- **F12.** Q3 under (a) is a ruling.
- **F13.** The C.3 verb agreement, the §4.14 citation, the `move` remedy, and the corpus `try` grammar.
- **F14.** The Status prose.

The coordinator's ruling of 2026-11-26 on the draft's question 4 is §2.4.
