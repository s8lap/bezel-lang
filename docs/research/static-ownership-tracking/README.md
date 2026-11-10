# Static ownership tracking: can the compiler bound stored borrows itself, and retire the trust admissions?

Research lane, 2026-11-10. Base: origin/main `a30bb11df` (spec **v0.355**). Compiler for probes: the r40 prerelease `bezelc 0.0.0-a30bb11d`, built from that commit. Model: a copy of the prior lane's `bzcore_trust.py`, extended in `model/`. Scanner: the prior lane's, copied and extended in `data/`. Nothing under `src/`, `tests/` or the normative documents was edited; nothing was filed or pushed.

Every number and claim carries one of three labels. **MEASURED**: produced by a script or probe in this directory. **QUOTED**: taken from the tree at the cited place. **HYPOTHESIS**: reasoning that nothing here measures. Prior art is from general knowledge unless a line says it was checked by web search. A "DECLINED row *n*" is line *n* of `docs/proposals/DECLINED.md` at this base, always named with its topic.

## 0. One page for the owner

**The question.** K4.3 (§28.3.1, v0.355) admits a borrow stored where the body-local checker cannot bound it — a plain field holding a parameter's object, a collection slot, a comparer, a frame — on *trust*. Checked mode's disposed flag (K4.1) is the backstop; an unchecked build is undefined behaviour. Can a simple rule track these statically instead, and shrink the trust surface?

**The answer: yes in direction — for the field-held, collection-held and comparer-held clauses, with a rule the language already has — but not yet as kernel text: three soundness items are open (below).** The static regime the question asks for exists today for three kinds of value that carry a borrow: a borrow-bearing record (§9.7), a function value (§28.3's closure bullet, §28.14), and an iterator or query frame (K6). Each is read with a source set, and each is scope-checked and frozen through the local that holds it. Only a *class instance* that holds a borrow is left wholly on trust. **Candidate A** closes that gap: a class with a plain class- or interface-typed field is *borrow-bearing* by a derived type property (§3.6), and its instances are read as K6 reads a frame — a value that carries a source set. The recommended form needs no new syntax:

- a **mint** (a constructor call, or a call whose declared return type is an `owned` borrow-bearing class) carries the receiver and the sources of its plain arguments — K6's rule for a frame, verbatim;
- a **later store** (a method that keeps a parameter) is announced with the `scoped` parameter that exists today (§28.14); a plain parameter becomes call-bounded for a class-typed value as it already is for a function value;
- the holder then obeys the rules a record local and a frame local obey: the scope comparison (K2.3), the freeze through its holder (K5.5), and the dominated-use error (§28.2).

The body-local posture is kept. A caller reads a type and a signature, never another body; the one exception is the one §3.6 rule 2 already makes, a generic body's requirement read per instantiation, which A-5 needs. The property is derived from declarations, like trivial-dispose and sendable.

**Headline numbers.**

| | A (recommended) | A-explicit (`scoped` everywhere) | B (derived retention) | D (stricter freeze) | E (strict mode) |
|---|---|---|---|---|---|
| Recorded K4.3 sites in the corpora the rule covers (MEASURED: 157 field/collection stores + 2 comparer, of 178 recorded sites) | **159 of 178** | 159 of 178 | checks 22 resolved kind-3 constructor stores; retires no clause | 0 | 0 (turns each into an error where opted in) |
| Accepted-tier programs (5,750) newly rejected (MEASURED, hand-checked) | **5** (0.09 %): 3 hazardous, 2 inconvenient | 5 | 0 | at most 4, each a test of the backstop itself | every program with a store: 53 |
| Accepted-tier programs that compile after adding `scoped` | **12** (12 signatures, 4 visible call sites) | 48 (53 signatures) | 0 | 0 | — |
| Model, vars 3 / len 5, straight line: newly rejected of 354,933 accepted | 8,268 (2.3 %): 2,094 race or fault, 5,670 hold a dangling borrow never read, 504 clean | — | 4,218 | 0 (528 of 71,946 with a branch) | 200,103 |
| Model, same bound: of 2,106 programs that read a dead or foreign object through a stored borrow, left accepted | **12** (a slot kill, K5.6) | — | 1,020 | 2,106 | 0 |
| Model (one body, no calls): newly accepted / newly unsound, every bound | **0 / 0** | — | 0 / 0 | 0 / 0 | 0 / 0 |
| Model: races through a stored borrow left, every bound | **0** | — | > 0 (nested holders, method stores) | unchanged | 0 |

All model rows are MEASURED (`model/out/`); §4 has every bound, including branches, loops, inner scopes and a holder whose destructor reads its borrow, where more than nine in ten of A's rejections race or fault. **The model has one body and no calls.** What it checks is A-2 and A-3: the carried set, the scope comparison, the freeze and the dominated use. A-4's callee side and A-5's erasure — interfaces, generics, virtual dispatch, async frames, closures, `Shared<T>` — are argued in §3, not modelled.

**What A costs.** (1) A method that keeps a plain parameter must say `scoped`: 12 signatures in the accepted tier, 19 more in the expected-reject tier and gtest. A method that keeps an `Object` parameter has no spelling, because `scoped` is not admitted at `Object`: 1 accepted-tier program. This is the callee-side no-store rule that DECLINED row 48 and rationale §4.2.6 decline, narrowed to members that are not constructors; §3 answers the record. (2) A holder declared before its source — the long-lived registry — is rejected; it must reorder, own, or use `Weak<T>`, which works on r40 (probe p28). (3) An `owned` borrow-bearing instance cannot be converted to an interface that is not borrow-bearing: 0 sites in the corpora, 23 classes that could meet it. (4) A fresh temporary cannot be the source of a holder that outlives the statement: 3 accepted-tier programs, all of which read a disposed object by the spec's letter.

**Three soundness items are open; A is not ready to be kernel text until they are closed or ruled.** (1) *Generic-body erasure.* A-5 as first stated checked call positions only. A generic body such as `Hide<T>(owned x: T): owned Object => x`, instantiated at a holder class, erases a non-empty source set inside a body that is not re-checked. §3 states the repair — the erasure is a requirement of the body's use of `T`, read at each instantiation, as `Keep<Pair>` is today — but it is argued, not modelled, and a dispatched generic method has no instantiation site. (2) *The destructor window.* The self-source case of the invariant (kind 1) is unproven: a child's destructor reading its owner during the owner's cascade faults on r40 (k1a). (3) *A class holder across an `await`* is unresolved (S4).

**What A does not reach.** The frame-held clause (an unsettled task), §28.14's two residuals, and the slot-displaced transfer (0210) stay recorded: 19 of 178 sites. The flag also keeps two jobs that are not trust: an announced kill on some paths only (K4.2) and a slot kill (K5.6). Those are today's posture for a *local* borrow; A only makes a holder behave like one.

**The limit of the evidence.** The corpora are test programs. They under-represent the long-lived registry (kind 2), so "5 programs" is a floor for real code, not a forecast. Step 1 below exists to replace it with a measurement.

**Recommendation and order of work.**

1. **Adopt A as the direction, in report mode first.** Extend `--report-trust-admissions` to print, per store and per construction, the verdict A would give. That measures real programs at no risk, and it is the ramp `spec-change-process.md` §2 asks of a Ratchet. §9 has the proposal outline.
2. **Before A can land, close the gaps that A reuses** (MEASURED on r40): proposal 0205's borrow and holder legs are not implemented (p07, p05 compile, fault in checked mode and segfault unchecked), and K5.5's frame-local freeze is not implemented (p26). Both are spec-ahead-of-implementation, not design questions. The class-typed `scoped` form still rejects `new Child(scoped this)` (k1e); whether the spec admits it is ruling S1 below.
3. **Before A becomes kernel text, close or rule on the open soundness items**: the generic-erasure hole (A-5, §3), the destructor window (a child's destructor reading its owner during the owner's cascade faults on r40, probe k1a — unchanged since the prior lane), and the class holder across an `await` (S4). One further ruling is owed: whether §28.14's ordered bounding forms admit a constructor call assigned to an `owned` field of `this` (§7, S1).
4. **Decide up front whether a per-field trust opt-out survives.** A-4 reopens a recorded decline (DECLINED row 48: a callee-side no-store rule "retires the back-reference idiom"). The idiom survives in its constructor form and as a `scoped` method, but a method store with no bounding object loses its spelling. An explicit marker on the field that keeps today's trust admission is the escape route for that case; with it the recorded set becomes "the marked fields". Whether it exists is a design decision to take with the proposal, not after the ramp; step 1's data then sizes it.
5. **Do not build derived retention's interface carriage (B).** Its caller-side half — the freeze and 0205 reading a holder — is the same code A needs and should be built once. Its callee-side half, a per-parameter bit computed from a body, is what rationale §9.7 objects to by analogy (that section is about `pure`: declared, not derived) and what DECLINED row 68 (a callee-body retention summary at the call) rejects. A does not need it. B stays the fallback if the owner declines A's one rule flip.
6. **Leave D declined, and record the measurement.** With 0205 in the baseline, D rejects only a kill on some paths: 0 programs on straight-line code, 528 of 71,946 with a branch, of which 312 fault on some run. In the corpora 4 sites exist, all tests of the backstop. It contradicts DECLINED row 308 (compile-time borrow-liveness checking; freezing an explicit `Dispose()`) and retires no trust clause. It is a separate question from K4.3.
7. **Decline C** (lifetime parameters). It is the precision ceiling; its measured gain over A in the corpora is 9 sites (a store that relates two parameters), at the cost of a second parameter system on every type and signature that touches a holder.
8. **E after A**, as the prior lane said. Under A a strict mode has little left to forbid: the frame clause and any opt-out field.

The sequence, as the non-author review of this report put it: report mode first; the generic-erasure hole and the destructor window closed or ruled before A is written as kernel text; the opt-out decided up front.

---

## 1. Method

**Corpora** (MEASURED, `data/scan_static.py`): 9,521 units at `a30bb11df` — 5,902 rule-corpus units, 802 doc-extracted blocks (553 fragments, 8 programs), 2,574 lit programs, 243 gtest files with embedded programs; 17,348 classes. The *accepted tier* is the 5,750 rule, doc and lit units the spec expects to compile. gtest files concatenate variants of one program, so they are counted by distinct site, never by program. The corpora have grown since the prior lane (7,353 units at v0.350).

**Scanner.** `data/bzlex.py` and `data/scan_trust.py` are unmodified copies of the prior lane's. `data/scan_static.py` adds four things. (1) The borrow-bearing classification of every class, to a fixed point: a plain class- or interface-typed field, a primary-constructor capture, a borrowing collection of class elements, an `owned` field of a borrow-bearing class, a borrow-bearing base. (2) For each K4.3 store the prior scanner finds, the kind of member that stores (constructor, method, setter, initializer), which decides whether A needs a marker. (3) Every construction `new C(args)` of a borrow-bearing class: where the instance is held, where each carried argument comes from, and any kill of a carried local while the holder is named later. (4) Every explicit `Dispose()` of a local whose alias is named afterwards (D's population). Of the 312 stores the prior scanner finds, 155 are not K4.3 admissions and are separated out: 66 that carry no borrow at the visible call (a value argument, a static source); 42 where a generic class stores a bare-`T` parameter and the unit names the class only at value type arguments, so no instantiation in the unit stores a borrow (§3.6 rule 2); 1 function-typed field, which the closure rules govern; and 46 stores into a purely local collection, which the report has been silent on since v0.350. gtest class counts are raw: a file's variants repeat a class.

**Precision.** Every accepted-tier program the scanner rejects, or in which the prior classifier marks an escaping holder (kind 2, in any population), was read by hand: 11 programs, the count `data/out/summary.txt` prints, with verdicts and reasons in `data/hand_check.tsv`. Six are admitted by A (the prior classifier's kind 2 is "the holder leaves the body", which A admits when a signature announces it, and two are value instantiations); five are rejected. The roll-up uses the hand verdicts. A first version of this scanner overcounted in two ways that a non-author review caught: it read a function-typed field as a class-typed one (bzlex records `() -> int` as `(`), and it asked for a `scoped` marker on a bare-`T` parameter of a generic class instantiated only at values. Both are fixed, and every corpus number in this report is from the corrected run. The scanner is syntactic and per unit: it resolves declared types inside one unit, treats an unknown type name as a class, and reads control flow as text order. The store classification inherits the prior lane's 24-hit precision check.

**Model.** `model/bzcore_static.py` is the prior lane's model plus three statements and five knobs (its docstring lists them). `kset x y` is a *method* of the holder that stores its parameter. `newp x y` is a nested holder, `new P(new K(y))`. `--block` enumerates an inner scope so a holder can outlive a source. `--kdtor` gives the holder class a destructor that reads its borrow, so a holder's release is a use. The baseline is brought from the v0.345 pin to v0.355 by three flips (`--v0355`): 0205's two errors and K5 clause 2's v0.346 sentence on a maybe-consumed left-hand side. Each accepted program is run on every path; a run is classified as clean, FAULT (the flag caught a stale read) or RACE (a read of an object another domain owns). A rejection is classified by what the program did at the baseline: it raced, it faulted on every run, it faulted on some run, it held a dangling stored borrow that no run read, or it was clean. Selftest 42/42.

**Probes.** `probes/p*.bzl`, 30 programs, each compiled by r40 with the report, in checked mode and in unchecked mode, then run (`probes/run_all.sh`, `probes/results.md`). The prior lane's 14 `k*` probes were re-run on r40 (`probes/results_prior_k_probes_on_r40.md`). No probe was run on r37.

## 2. What the language already does, and the one kind of value it leaves out

| value that carries a borrow | tracked by | stored in a class field | outlives a source by scope | source moved while it is live | source disposed, then used | trust clause |
|---|---|---|---|---|---|---|
| borrow-bearing record, span (§9.7) | type property + source set | rejected (p01) | rejected (p02) | rejected, K5.1 (p04) | error by §28.2; **r40 accepts** (p05) | none |
| function value (§28.3, §28.14) | source set from captures | rejected unless `scoped` (p25) | rejected | rejected | error by §28.2 | none for the store; quiescence for `scoped` |
| iterator / query frame (K6) | carried set from the call's arguments | admitted: the frame-held clause | rejected | rejected by K5.5; **r40 accepts** (p26) | error by §28.2 | frame-held, only where no local holds it |
| **class instance with a plain field** | nothing | — | **admitted** (p17) | **admitted** (k3b, p27) | **admitted** (k3a, p13) | **field-held, collection-held, comparer-held** |

All verdicts MEASURED on r40 against the QUOTED spec rule. The last row is the whole of the question. A plain field `_owner: Customer` already *declares* that its class holds a borrow; the checker does not read the declaration that way.

The existing static spelling, the class-typed `scoped` parameter, does not close the gap, for three measured reasons. It admits only a `using var` holder: a plain `var` is rejected (p10). The holder cannot be moved (A5). And a nested holder is inexpressible: a `scoped` parameter may not be forwarded into `new Account(scoped c)` stored in an `owned` field (p12, QUOTED §28.14: "Storage reached through `owned` fields of the bounding object is excluded"). The corpora agree: 152 plain stored parameters against 19 class-typed `scoped` parameters.

## 3. The candidates as kernel deltas

### A. Borrow-bearing classes (recommended)

**A-1, the property (§3.6).** A class *holds borrows* when it declares or inherits (i) a non-`owned` instance field of class or interface type other than a runtime-managed type, or (ii) an `owned` field or owning-inline-array slot whose type holds borrows. The stdlib states it for its own types: a borrowing collection at a class, interface or holds-borrows element; an owning collection, `Shared<T>` or `Locked<T>` at a holds-borrows element; a hash collection built with a comparer. It is read per instantiation (§3.6 rule 2). Computed, never declared (rule 1).

**A-2, the value carries a source set (K6 widened).** K6's "frames are values" gains a fourth kind, the *holder*. Its set is built in three ways.
1. A **mint** — a constructor call, or a call whose declared return type is an `owned` class that holds borrows — carries the receiver and what its plain arguments contribute (§28.3 *The sources an argument contributes*). This is K6.2's announcement by declared type.
2. An **`owned` argument that holds borrows** brings its own set to whatever the callee can put it in: the receiver, where the receiver's class holds borrows, and the result, where the call is a mint.
3. A **later store** — a `scoped` argument, a property or indexer store of a reference, or a store position of a standard borrowing collection (§28.11) — adds the argument's sources, never removes one, and is checked as an assignment to the location that holds the receiver. This is §9.7's *Member stores* rule read for a class.

A borrow of a holder carries the holder's binding and, through it, the holder's set (K2.2).

**A-3, the holder is bounded (K2.3 with no residual for these stores).** Every rule that reads a record local or a frame local reads a holder: the local-scope bullet; the static bullet; K8.4's no-temporary-source rule; K5.5's liveness, so the silent kills of a carried source are frozen while the holder is live; and §28.2's dominated use. Where the class is not trivial-dispose its release is a use, and a source must be declared before the holder (§28.14's test). A holder is live to its last use where the class is trivial-dispose.

**A-4, the callee side.** A plain class- or interface-typed parameter is call-bounded, as a function-typed one is today. Its borrow, direct or carried inside a holder, may reach three places: the object under construction, in a constructor; a purely local holder or collection, which may then be returned through a mint; and, if the parameter is `scoped`, the bounding object. The check reads the stored value's source set, as §9.7 reads a record's. `this` and a receiver-sourced borrow may be stored into storage `this` owns: the self-source case, which is kind 1. K4.3's field-held, collection-held and comparer-held clauses are deleted.

*The record A-4 reopens.* DECLINED row 48 (a callee-side rule forbidding the store of every plain `Object` or interface parameter) and rationale §4.2.6 decline this kind of rule: "a callee-side no-store rule for every `Object` or interface parameter, which retires the back-reference idiom" (QUOTED). Proposal 0146 §3.4 gives the grounds: the idiom's named form is `constructor(logger: ILogger) _logger := logger;`, and the rule's reach was "every class-typed-or-interface borrow stored from a parameter across the repository, for a hazard that only a boxed value carries" (QUOTED). A-4 is that rule widened to class-typed parameters and narrowed to members that are not constructors. It contradicts a recorded decision, so it is a design change on that ground alone (process §2), and it must answer the grounds. Three answers. The idiom's named form is untouched: a constructor stores its plain parameter with no marker, and 93 of the 157 stores are that form. The method form survives with one keyword wherever a bounding object exists (`scoped`), and a `this`-sourced store needs nothing. And the hazard is no longer a box's: it is the whole field-held surface, 2,106 stale-reading programs at one model bound. What does not survive is a method store with no bounding object, and a method store of an `Object` parameter, since `scoped` is not admitted at `Object` (§28.14). MEASURED price: 12 signatures in the accepted tier and 19 elsewhere take `scoped`; 2 accepted-tier stores, 1 in the expected-reject tier and 8 distinct gtest stores are rejected — 9 write into an object the member does not hold, 2 keep an `Object` parameter. Whether those keep a trust form is the opt-out decision of §0.

*Standard collections.* A store position of a standard borrowing-family collection — `Add`, `Insert`, `Push`, the indexer set and the other members of §28.11's *Value-copy members* — takes no marker at the call. The spec already reads those signatures as stores (QUOTED, K8.4: "their declared signatures are read as stores"), and the collection's type holds borrows by A-1. So `a.Add(it)` is a later store under A-2.3: it adds `it`'s sources to `a`'s set and is checked against the binding that holds `a`. Rewrite 4 in §5 relies on this. A *user* class that stores its parameter in a method is not compiler-known and takes `scoped` at the parameter and the marker at each call: 4 visible call sites in the accepted tier.

**A-5, erasure at owning positions.** An `owned` value that holds borrows, with a non-empty set, may not be converted to a declared type that does not hold borrows at an *owning* position: an `owned` field, parameter, return or slot typed `Object`, an interface, a base class, or a type parameter substituted with one. Borrowing positions are untouched, because under A-4 a plain parameter cannot retain. This is §9.7's borrow-erasing bullet, narrowed.

*In a generic body* the conversion is at no call. `static method Hide<T>(owned x: T): owned Object => x;`, instantiated at a holder class, erases a non-empty set inside a body that §9.7 says is not re-checked, and the caller's check over the signature sees an `owned T` going in and an `owned Object` coming out. So A-5 needs a second half. An owning conversion, in a generic body, of a value whose type mentions a type parameter — to `Object`, an interface, a base class or another type parameter — is a *requirement of the body's use of that parameter* (§3.6 rule 2), read at each instantiation: an instantiation that substitutes a class that holds borrows is rejected where it is named, whatever the value's set, as `Keep<Pair>` is rejected at its `_last := a` store today. The requirement is a structural fact of the body and travels in the interface as §2.8.6's requirement summaries do. A dispatched generic method has no instantiation site and decides at its body (§14.2): there the conversion is an error unless a constraint excludes holders, and no such constraint exists. **This half is argued, not modelled, and it is the least certain part of A.** The generics matrix has not been run against it. A route through a second generic, through a standard-library body (which contributes no requirement, §3.6 rule 2) or through an `as` at a type parameter may remain: proposal 0159 records that the same family, for values, "has produced a new route at each of four matrix runs" (QUOTED). Until that is checked, generic-body erasure is an open soundness hole.

**Unchanged.** K4.2: an explicit `Dispose()` of a carried source is not frozen; the flag backstops it and 0205 makes the dominated use an error. K5.6: a slot is not frozen. Sendability: a class that holds borrows already fails rule 3.

*Example* (a sketch of A's verdicts over the kernel's own `Widget`; `Take` has an `owned` parameter):

```bezel
class Tag
  private _owner: Widget;                  // a plain field: `Tag` holds borrows (A-1)

  public constructor(owner: Widget)
    _owner := owner;                       // admitted with no clause: a constructor stores into the object it mints (A-4)
  end constructor;

  public method Retarget(scoped w: Widget) => _owner := w;   // a later store is announced (A-2.3)
  public method Name(): string => _owner.Name;
end Tag;

var a := new Widget("a");
using var t := new Tag(a);                 // `t` carries `a` (A-2.1)
Take(move a);                              // error (K5.1 through K5.5): `t` is a live holder of `a`
Console.WriteLine(t.Name());

using var b := new Widget("b");
using var u := new Tag(b);
do
  using var inner := new Widget("inner");
  u.Retarget(scoped inner);                // error (K2.3): `u` would outlive its source `inner`
end;

var c := new Widget("c");
using var v := new Tag(c);
c.Dispose();                               // allowed (K4.2): an announced kill is not frozen
Console.WriteLine(v.Name());               // error (§28.2): the disposal of `c` precedes this use on every path
```

**Soundness argument.** Invariant: for every live object *o* of a class that holds borrows, each object reached through *o*'s non-owned fields is static, or is *o*'s own owner (self-source), or is the value of a source in *set(o)* as read at the binding that holds *o*. The set changes only at a mint or an announced store, and each is checked against the holder's binding (A-2, A-4). A source in the set cannot end before the holder by scope (A-3), and cannot be killed silently while the holder is live (K5.1, K5.2, a domain transfer included). So no stored borrow is read after a silent kill, and none is read across a domain: the domain clause has nothing left to say for these stores. Three things remain outside the argument. Two are flag-backstopped exactly as for a local borrow today: an announced kill that does not dominate the use, and a kill of a slot's current value. The third is not settled at all: **the destructor window** (S2). The self-source case assumes an owner outlives what it owns, and during the owner's own teardown that fails in checked mode on r40: a child's destructor that reads its owner in the cascade faults (k1a). Until that is ruled, the self-source case — kind 1 and the sibling order — is unproven, and the argument above holds for a source outside the object. The model agrees on the first two at every bound (§4). It has no owner destructor bodies and no calls, so it is silent on the third, on A-4 and on A-5.

**The corners the brief asked about.**

- *How the source set is determined.* By the mint and by announced stores; never from a body. The mint rule over-approximates: an argument the constructor only reads is carried too. So A rejects p22's shape — an unstored second argument that ends before the holder — when that argument is transferred or reassigned (K5.1, K5.2), is declared in an inner scope (K2.3), or is the only source and is disposed (the `Tag` example's third case), although nothing dangles. p22 as written is admitted only because its kill is an explicit `Dispose()` of one source of two, which K4.2 leaves unfrozen and §28.2 does not reach. MEASURED: 1 construction site of 312 carries an argument its constructor does not store (`l6_5_0019`); it escapes rejection because that argument is never killed or scoped inside the holder's life. A-explicit removes the over-approximation at the price of 53 markers.
- *Inheritance and virtual dispatch.* The property is inherited (55 of 534 borrow-bearing classes have it from a base). A class that holds borrows may derive from one that does not; the constraint is A-5, at the owned upcast. An override keeps the declaring signature's `scoped` markers, as §28.14 already requires for function types.
- *Interfaces — the crux.* A borrow of a holder may be passed at any interface type. What is rejected is *owning* it at a type that announces nothing: `static method For(c: Customer): owned IReport => new OwnerReport(c)` (p14). MEASURED: 0 such sites in 312 constructions; 23 of 534 borrow-bearing classes list a base or interface that is not borrow-bearing. Every comparable language picked the same default (§6): Rust's `Box<dyn Trait>` means `'static`, Swift's generics and protocols exclude a non-escapable type unless they say `~Escapable`, C# 13 forbids converting a `ref struct` to an interface. The lift, if evidence asks for it, is an interface that declares itself borrow-bearing; an interface that declares a `scoped` class-typed parameter is already announcing that its implementations retain.
- *Generics.* `Box<T>` with `owned _v: T` holds borrows at a `T` that does (p20), read per instantiation. A generic member that stores a bare-`T` parameter in a method (`Bag<T>.Push(item: T)`) carries a requirement read per instantiation: at a value substitution nothing is stored and no marker is needed, which is every instantiation the corpora name (42 stores); at a class substitution the member needs `scoped item: T`, or the instantiation is rejected where it is named — §9.7's `Keep<Pair>` rule, extended. An owning conversion of a `T`-typed value inside a generic body is A-5's second half, and is open. 253 classes have a bare-`T` field.
- *Async frames and closures.* A closure that captures a holder contributes the holder's binding and set (provenance clause 1, as for a captured record). **A holder held across an `await` is unresolved** (S4). A holder built in an `async` body over a caller-sourced parameter is the same borrow as the parameter, one hop away. Two readings exist. Reading it as part of the frame's carried set keeps it under the frame-held clause, which is trust: A would then not have retired it. Importing §30.1's ban on a holds-borrows local across a suspension rejects it, and async code builds such helpers routinely. It would be wrong to argue that the ban is unneeded because a class-held borrow is flag-checked at every access: that defends the backstop A exists to stop relying on. The corpora have no site that prices either reading.
- *`Shared<T>`.* `Shared<Account>` holds borrows; every handle and `Clone()` result carries the set (p24).
- *Owned fields of borrow-bearing type.* The container holds borrows (p13). This is the case a per-parameter bit cannot see: `Branch`'s constructor stores no plain parameter, it stores an owned `Account`.
- *A span or record field.* A would make it sound to lift §9.7's instance-field ban, since a span field would make its class borrow-bearing. This study does not propose the lift; DECLINED row 693's bar stands.
- *Returning one.* An `owned` return is a mint and carries the receiver and plain arguments (p21, k3d). A plain return is receiver-sourced as today.
- *Kind 1, the back-pointer.* Nothing to write. `_child := new Child(this)` stores a value whose only source is `this` into storage `this` owns. A child moved out is returned `owned` from an instance member, so it carries the receiver: the prior lane's "sharp" rule falls out of the mint rule. An owner built from no borrowed argument has an empty set and may be held anywhere, a static included (p23). The retained-UI exploration's `Mount(owned child)`, which writes `child._parent := this` and then moves the child into its owned collection (QUOTED, `docs/research/exploration-ui-vcl-retained.md`), is this case: the store adds `this` to a local holder, and the move is self-sourced. Two rulings are owed (§8).
- *Siblings.* A field of `this` stored into a holder that another `owned` field of `this` keeps — the generated DI root's `_cache := new RedisCache(_db)` (QUOTED, `docs/research/exploration-compile-time-di.md`) — is self-sourced too. The cascade runs in reverse declaration order (§28.4), so the borrowing field must be declared after the field it borrows: the declared-before test, on fields. An overwrite of the borrowed slot stays K5.6.
- *Kind 2, the registry.* A holder declared before its source is rejected (p17, p30). The program reorders (p29), owns the items, keys them (`SlotHandle`), or observes them through `Weak<T>` (p28). None of these is new.
- *Kind 4, the unknown body.* The interface member declares `scoped`; the store is then checked at the caller against a visible holder. A helper that relates two parameters — `Feed(sink: ISink, it: Item) => sink.Keep(it)` — has no spelling and is rejected: the gap only C closes.

**Hybrids.** *A with one explicit escape hatch*: keep today's admission behind a marker on the field, so the trust set becomes the marked fields and E is "none in this module". It is the escape route for the method stores A-4 leaves without a spelling, and whether to have it is decided with the proposal (§0, item 4), not after the ramp. *A inside a module, B across modules*: not needed. A's property is a §3.6 derived type property and crosses a module boundary in the artifact interface as trivial-dispose and sendable do.

### B. Derived retention (the baseline)

QUOTED (0132 §1): a per-parameter `retains` fact computed from the callee's body, carried in the interface, for a direct store into a `readonly` field of `this`. The caller's freeze and 0205 then see the holder. It adds no syntax and rejects no clean program. What it cannot do, each MEASURED: it sees no nested holder (p13; model `newp`); its definition keys on a `readonly` field, which the tree does not write (0132 §6), and read as the prior lane read it — any constructor store — it still leaves the 52 method stores of 157 outside; it reaches no interface call, function value or uncarried artifact; and it retires no clause — the residual set stays, checked where the fact happens to be visible. It also needs what rationale §9.7 declines by analogy — that section is about `pure`, declared rather than derived, and 0132 §5 is what applies its reasons to a body-derived fact in the interface — and what DECLINED row 68 rejects (a caller's verdict that depends on another body). A answers both records: the fact is a type property, and the caller reads a signature.

### C. Lifetime or region annotations (the ceiling)

A type that holds a borrow takes a lifetime parameter, and every signature that mentions it names or elides one. This relates any two parameters, accepts a dead holder, and needs no scope over-approximation. Its user cost is a second parameter system: every one of the 534 borrow-bearing classes, and every signature that returns or stores one, would carry it. Elision covers the single-parameter case, which is 90 % of stores (prior lane, QUOTED), and that case is exactly what A's mint rule covers with no parameter at all. MEASURED gain of C over A in the corpora: the stores that relate two parameters — 1 accepted-tier site and 8 distinct gtest sites. In the model, the programs only C would accept are A's "never read" and "clean" rejections (§4). DECLINED row 308 rejects the exclusive-writer rule, not lifetimes as such; C is declined here on cost, not on that row.

### D. Stricter freeze

K5 clause 3 flips: a site-announced kill is frozen while a local borrow of the value is live. With 0205 in the baseline the only new rejections are kills on some paths. MEASURED: 0 on straight-line code; with a branch at vars 3, 528 of 71,946 accepted programs, of which 12 fault on every run, 300 on some run, and 216 on none (a stale borrow copied but never read). In the corpora, 4 sites, all of them tests of the backstop itself. D retires no K4.3 clause. The recorded ground (rationale §5.1, DECLINED row 308) is "a de-facto borrow checker for no soundness gain where the backstop is real". 0205's own record already qualifies "no soundness gain" for the unchecked build. What the model cannot show is the cost of correlated conditions (`if done then x.Dispose(); … if not done then b.Use();`), which a path-insensitive liveness rejects although no run faults. HYPOTHESIS: that is the real cost and it is not small in service code. D is a separate decision from trust.

### E. Per-module strict mode

Every remaining admission is an error unless marked. It rejects exactly the report's lines: 53 accepted-tier programs hold a K4.3 store today. As the prior lane said, it is cheap and it presupposes a complete report (#7465–#7471 remain open per that lane; p22 adds a false line, §7). Under A it shrinks to the frame clause, the §28.14 residuals, and any opt-out field.

### Others considered

- **No stored borrows at all** (Hylo's rule): reject every plain class-typed field. MEASURED: 534 classes, in 177 accepted-tier programs. It is the simplest rule, and it rejects 177 programs where A rejects 5; kind 3 would have to pass its source to every call.
- **A weak reference over an ordinary owned object** (the prior lane's design A): the right companion for kind 2 under A, since a field of `Weak<T>` does not make its class borrow-bearing. Unchanged by this study.

## 4. Model results (MEASURED, `model/out/`)

Columns: programs accepted at the v0.355 baseline; programs in which some run reads a dead or foreign object through a stored borrow (the *trust surface*: race + fault on every run + fault on some run); then, per candidate, the newly rejected programs split by what they did at the baseline, and what is left.

| bounds | accepted | trust surface | A: newly rejected = race + fault every + fault some + dangling never read + clean | A: left (race; fault every / some) | B: newly rejected; left (race; fault every / some) | D: newly rejected | newly accepted or unsound, any candidate |
|---|---|---|---|---|---|---|---|
| vars 3, len 4 | 24,501 | 36 = 12 + 24 + 0 | 204 = 12 + 24 + 0 + 156 + 12 | 0; 0 / 0 | 96; 6; 12 / 0 | 0 | 0 |
| vars 3, len 4, holder destructor reads | 24,501 | 1,800 = 474 + 1,326 + 0 | 1,848 = 474 + 1,314 + 0 + 6 + 54 | 0; 12 / 0 | 978; 222; 636 / 0 | 0 | 0 |
| vars 2, len 4, `if` | 12,732 | 102 = 38 + 12 + 52 | 240 = 38 + 12 + 26 + 164 + 0 | 0; 0 / 26 | 132; 18; 6 / 38 | 72 | 0 |
| vars 2, len 4, `if`, holder destructor reads | 12,732 | 1,128 = 364 + 390 + 374 | 1,018 = 364 + 390 + 240 + 16 + 8 | 0; 0 / 134 | 556; 168; 178 / 246 | 72 | 0 |
| vars 3, len 4, `if` | 71,946 | 306 = 114 + 36 + 156 | 1,206 = 114 + 36 + 78 + 966 + 12 | 0; 0 / 78 | 648; 54; 18 / 114 | 528 | 0 |
| vars 3, len 4, `while` | 419,160 | 924 = 24 + 24 + 876 | 3,822 = 24 + 24 + 864 + 2,898 + 12 | 0; 0 / 12 | 2,058; 12; 12 / 438 | 72 | 0 |
| vars 3, len 4, inner `do … end` | 507,678 | 1,554 = 366 + 1,188 + 0 | 7,134 = 366 + 1,188 + 0 + 5,472 + 108 | 0; 0 / 0 | 3,396; 180; 582 / 0 | 0 | 0 |
| vars 3, len 4, inner `do … end`, holder destructor reads | 507,678 | 32,598 = 6,438 + 26,160 + 0 | 34,326 = 6,438 + 25,692 + 0 + 288 + 1,908 | 0; 468 / 0 | 18,420; 2,970; 12,522 / 0 | 0 | 0 |
| vars 3, len 5 | 354,933 | 2,106 = 600 + 1,506 + 0 | 8,268 = 600 + 1,494 + 0 + 5,670 + 504 | 0; 12 / 0 | 4,218; 288; 732 / 0 | 0 | 0 |
| vars 3, len 5, holder destructor reads | 354,933 | 38,136 = 9,750 + 28,386 + 0 | 40,398 = 9,750 + 28,122 + 0 + 438 + 2,088 | 0; 264 / 0 | 22,392; 4,356; 13,068 / 0 | 0 | 0 |

Reading.

1. **A is sound at every bound, for what the model contains**: 0 newly accepted, 0 newly unsound, 0 races through a stored borrow left, across ten sweeps. The largest enumerates 2,078,070 programs and accepts 507,678; the 0 / 0 result is over the accepted programs. The model checks A-2 and A-3 inside one body; A-4 and A-5 are outside it. A rejects 0.8 % to 2.3 % of accepted programs where the holder is trivial-dispose and 6.8 % to 11.4 % where its destructor reads the borrow.
2. **A removes the trust surface; B removes about half of it.** At vars 3 / len 5, 2,106 accepted programs read a dead or foreign object through a stored borrow. A rejects 2,094 of them and leaves 12, all of one shape (next point). B rejects 1,086 and leaves 288 races and 732 certain faults, through the nested holder (`new a / newp b a / send a / pkuse b`) and the method store, which a per-parameter constructor bit does not see.
3. **What A leaves is not trust.** The faults left under A are of two shapes. *An announced kill that does not dominate the use*: `new a / newk b a / if { } else { disp a } / kuse b`, and in a loop `new a / newk b a / while { disp a / asg a } / kuse b` — K4.2, as for a local borrow; the loop form is the residual 0205 records. *A slot kill*: `new a / fbrw b a / fst a / newk c b / kuse c` — K5.6, as for a local borrow of a slot's current value. A-static (the fully static bound §28.14 gives a `scoped` capture) removes the first shape and adds never-read rejections; it is the same trade as D, restricted to holders.
4. **How hazardous the rejections are depends on the holder's destructor.** Where the holder's release reads the borrow, 94 % to 98 % of A's rejections race or fault (37,872 of 40,398 at len 5). Where the holder is trivial-dispose, 18 % to 32 % do, and most of the rest hold a dangling borrow that no statement reads (5,670 of 8,268 at len 5): one added line away from a fault, and the same class K5.1 and K5.2 reject for a local borrow today. The clean rejections (at most 6 %) are transfers of a source while the holder is live (`new a / newk b a / move c a / disp b`): the recorded cost of the freeze, "re-borrow from the new owner".
5. **D rejects nothing on straight-line code and little with a branch**: 528 of 71,946 accepted programs at vars 3 with an `if` (12 fault on every run, 300 on some run, 216 on none), and 72 of 419,160 with a loop (42 and 30). Its never-faulting rejections are a stale local borrow that is copied but not read (`new a / brw b a / if { } else { disp a } / brw c b`). The model has no correlated conditions, so these figures are a lower bound on D's cost.
6. **E** rejects every program with a store: 200,103 of 354,933 at len 5, of which 2,106 are on the trust surface.

What the model cannot express: calls, so interface carriage, drift and A-5's erasure are outside it; the mint rule's over-approximation (one stored argument per holder); generics; frames. It has one holder class and one owner class. "Dangling never read" counts a holder that still exists; a holder already released is clean.

## 5. Corpus results (MEASURED, `data/out/summary.txt`)

**The property.** 534 of 17,348 classes hold borrows under A (3.1 %): 461 by a plain field, 19 by a borrowing collection, 3 by an owned field of a borrow-bearing class, 55 by a base; a class may have more than one reason. 358 are in rules, docs and lit; the 176 in gtest are raw counts over repeated variants. 8 have a destructor. 9 more classes hold only a `scoped` callback field, which §28.14 already bounds. 253 more have a bare-`T` field and are decided per instantiation.

**The stores.** 157 K4.3 store sites (128 field stores, 11 collection stores, 8 property stores, 7 initializers, 3 primary-constructor captures). 93 are in a constructor, 52 in a method, 10 in an initializer or capture, 2 in a setter. Beside them: 2 comparer-held, 3 frame-held, 13 §28.14 residuals, 3 slot-displaced sites. A covers 159 of the 178.

| population | sites | A: admitted as written | A: needs `scoped` on a method | A: rejected at the store | A-explicit: needs `scoped` |
|---|---|---|---|---|---|
| accepted tier (rules, docs, lit) | 58 | 44 | 12 | 2 | 53 |
| gtest, distinct | 37 | 19 | 10 | 8 | 26 |
| expected-reject tier | 29 | 19 | 9 | 1 | 28 |

The rejected stores are of two kinds: a store into an object the member does not hold (1 accepted, 8 gtest) and a method store of an `Object` parameter (1 accepted, 1 expected-reject). At the visible call sites of the accepted tier: 15 kind-3 sites and 2 kind-1 sites are bounded as written; 1 escaping-holder site was hand-checked and is admitted (a returned holder that carries its receiver); 1 is behind an unknown body; 39 have no call site in the unit.

**The holders.** 312 constructions of a borrow-bearing class: 186 are bound to a local (129 `var`, 57 `using var`), 81 are returned, 35 are arguments, 4 are temporaries used as a receiver, 3 go to a field, 2 reassign a local, 1 is unclassified. Today's `scoped` reaches only the 57. B's v1 scope (0132 §2) reaches the same 57. A reaches all of them.

**The programs.** Of 5,750 accepted-tier programs, 131 hold a K4.3 store or construct a borrow-bearing class.

| | unchanged | compile after a `scoped` marker | rejected |
|---|---|---|---|
| A | 114 | 12 | 5 |
| A-explicit | 78 | 48 | 5 |
| B | 131 | 0 | 0 |

The five rejections (`data/hand_check.tsv`): `s28_3_0205` and `s28_3_0206` assert the checked-mode fault of a literal stored through a plain constructor parameter — hazardous by design. `s22_6_2312` stores array-literal temporaries in `Row.Items` and reads them two statements later; the spec disposes them at the declaration's end (K8.1) and r40 prints `30` — hazardous by accident, and a finding (§7, S5). `s28_11_0041` pins the admission of a store into a collection held by another parameter, and `s28_3_0119` keeps an `Object` parameter in a method, where `scoped` is not admitted — both inconvenient; nothing dangles in either.

**B's reach.** Kind-3 sites with a constructor's store and a visible callee: 10 in the accepted tier and 12 distinct gtest sites. Kind-3 sites with a method's store into a mutable field, which 0132 excludes: 5 and 1. No accepted-tier program kills a source under a live holder, so B rejects none: the accepted tier is clean code and B only fires on a bug.

**D's population.** 4 accepted-tier sites dispose a local whose alias is named afterwards: `s28_2_0011`, `s28_2_0012`, `s28_3_0036`, and one lit test. All four test K4.2 itself.

**What the rejected code becomes.** Six rewrites; each "after" uses only what exists today.

1. *A method keeps its parameter* (12 signatures; `borrow_into_bare_field_1082`):
   ```bezel
   public method Set(c: Config) => _cfg := c;          // before: K4.3 trust
   public method Set(scoped c: Config) => _cfg := c;   // after; the call is h.Set(scoped cfg)
   ```
2. *A temporary is the source* (`s22_6_2312`; both "after" forms compile and run on r40):
   ```bezel
   var rows: Array<Row> := [new Row([1, 2]), new Row([3, 4, 5])];   // before: each array literal dies at this statement's end
   var a: Array<int> := [1, 2];                                      // after, either: bind the source first …
   using var r1 := new Row(a);
   public owned Items: Array<int>;                                   // … or: let Row own its items,
   public constructor(owned a: Array<int>) Items := a; end constructor;   // so `new Row([1, 2])` transfers the literal
   ```
3. *The holder is declared before its source* (p17, p30 → p29):
   ```bezel
   using var w := new Watch();  using var a := new Item("a");  w.Attach(a);          // before: admitted, faults if `a` dies first
   using var a := new Item("a");  using var w := new Watch();  w.Attach(scoped a);   // after: the source is declared first
   ```
   Where the registry must outlive its items, it observes them: `OwningArray<Weak<Item>>` and `TryUpgrade()` (p28; runs clean on r40 in both modes).
4. *A store relates two parameters* (`s28_11_0041`):
   ```bezel
   public static method Store(a: Array<Item>, it: Item): int  a.Add(it); return a.Count;  end Store;   // before
   a.Add(it);                                                                                          // after: store at the caller, where the holder's binding is visible
   ```
5. *An owned holder is returned at an interface type* (p14):
   ```bezel
   public static method For(c: Customer): owned IReport => new OwnerReport(c);      // before: the type announces nothing
   public static method For(c: Customer): owned OwnerReport => new OwnerReport(c);  // after: the caller's `using var r` carries `c`
   ```

6. *A method keeps an `Object` parameter* (`s28_3_0119`):
   ```bezel
   public method Put(o: Object)  _o := o;  end Put;            // before: K4.3 trust; `scoped` is not admitted at `Object` (§28.14)
   public method Put(scoped s: IShow)  _kept := s;  end Put;   // after: declare the parameter at the type the callee stores
   ```

Kinds 1 and 3 with a constructor store need no rewrite. The two-level holder (p13) needs none and has no static spelling today (p12).

## 6. Prior art

From general knowledge, not re-verified, except the three lines marked *checked*.

- **Rust, lifetimes on structs.** `struct Account<'a> { owner: &'a Customer }`: the type carries the borrow's lifetime as a parameter, and every use site re-establishes it. For this exact problem Rust decides four things. A trait object is `'static` by default (`Box<dyn Report>`); a borrowing implementation needs `Box<dyn Report + 'a>` — the interface crux, resolved as A-5 resolves it. A type with a destructor must have its borrowed data strictly outlive it (the drop check) — A-3's declared-before test. A back-pointer to one's owner is not expressible; code uses `Rc`/`Weak` or indices — where Bezel can do better, because a self-sourced set is local to one object. Elision removes the annotation in the one-parameter case. *What Bezel can take*: the defaults, without the parameters.
- **Swift non-escapable types.** *Checked by web search*: SE-0446 was accepted with modifications in October 2024; `Escapable` is a protocol, a type opts out with `~Escapable`, a generic wrapper is conditionally escapable (`struct Box<T: ~Escapable>: ~Escapable`), and a value that is returned or initialised must state its lifetime dependence (`@lifetime`, experimental when checked). From knowledge: only structs and enums can be non-escapable, and existing generic code excludes them unless it opts in. This is candidate A's design applied to values, with dependence stated at the mint. *What Bezel can take*: "the mint states what the value depends on"; A's default applies the same idea to every plain argument, so nothing is written. Swift's `unowned` — a non-owning reference that traps on a stale read — is exactly today's plain field in checked mode, and `unowned(unsafe)` is the unchecked build.
- **C# `ref struct`.** *Checked by web search*: since C# 13 a `ref struct` may implement an interface but may not be converted to the interface type, and a generic parameter accepts one only with the `allows ref struct` anti-constraint; a `ref` field may be declared only in a `ref struct`. From knowledge: its escape rules are scope-based, with no lifetime parameters. This is the closest relative of Bezel's §9.7, and it draws A-5's line in the same place. *What Bezel can take*: confirmation that a scope-based rule with an erasure ban is workable at scale, and the generic opt-in as the model for the interface lift.
- **Mojo.** *Checked by web search*: references carry *origins*, mostly inferred, written out for `ref` arguments and results and for parameterised types such as `Pointer` and `Span`; values are destroyed as soon as possible after their last use, and an origin union extends several lifetimes. So a holder keeps its source alive: Mojo *extends*, where Bezel's K8.4 *rejects*. *What Bezel can take*: nothing directly — extension by origin needs last-use destruction, and Bezel's disposal points are scope ends by design (rationale §5.24).
- **Hylo.** Mutable value semantics; safe code has no stored references. A projection yields a borrow that cannot be kept in a field. The proposed "remote parts" are stored projections whose owner cannot escape — borrow-bearing types again. *Lesson*: "no stored borrows" works when everything is a value; §3's measurement prices it for Bezel at 534 classes.
- **Vale.** Generational references: every non-owning reference is checked against the object's generation at each use, and region borrowing elides the check where a region is known immutable. Bezel's checked mode is in this family, with a flag instead of a generation. Vale never elides the check unsafely; Bezel's unchecked build does. *Lesson*: if the flag is to stay the answer, the unchecked build is the weak point; A removes stores from the flag's care instead.
- **Pony.** Six reference capabilities govern aliasing and sending; lifetime is the collector's. It has no use-after-dispose question. *What Bezel can take*: `tag`, a reference that cannot be read, is Bezel's `SlotHandle` key; and viewpoint adaptation — a field's capability seen through its container's — is the shape of A-1's "an owned field of a type that holds borrows".
- **Ada accessibility levels.** An access value may not designate an object declared at a deeper level than the access type. The check is static where levels are known and dynamic for anonymous access parameters. This is K2.3's split exactly. The dynamic half is widely regarded as the confusing part of Ada. SPARK's ownership rules confine a borrow to a local object. *What Bezel can take*: evidence for reject-not-check, and for comparing against the *holder's binding* rather than the type's declaration level.

Sources for the checked lines: the Swift forums' acceptance post for SE-0446 (forums.swift.org/t/accepted-with-modifications-se-0446-nonescapable-types/75504), Microsoft Learn's "What's new in C# 13" and the ref-struct-interfaces proposal (learn.microsoft.com/en-us/dotnet/csharp/whats-new/csharp-13), and the Mojo manual's lifetimes page (docs.modular.com/mojo/manual/values/lifetimes).

Across all of them the same three choices recur: a type that holds a borrow is marked as such by its type; it may not be erased to a type that does not say so; and a mint states what the value depends on. None infers the dependence from a callee's body.

## 7. Findings along the way (not filed)

Spec questions.

- **S1. §28.14's ordered bounding forms and a constructor call stored in a field.** Form 1 ("a constructor call → the constructed object") is tested before form 3 ("a call whose return is `owned` and is directly assigned to an `owned` field of `this` → `this`"), and the admission accepts only a `using var` or `this`. Read by the letter, `_child := new Child(scoped this)` resolves form 1 and is admitted nowhere, so the back-pointer has no static spelling; the prior lane's k1e assumed form 3. The text does not say which. r40 rejects it with a function-value diagnostic.
- **S2. The destructor window** (prior lane, k1a): a child's destructor reading its owner during the owner's cascade faults on r40. Unchanged, and still needed before kind 1 can be called proven.
- **S3. §9.7's instance-field bullet gives as its ground** "A class instance's lifetime is not bounded by any borrow source's scope". Under §28.14's scoped construction that is already false for a `using var` holder. A sentence that A would rewrite, noted so the rewrite is deliberate.
- **S4. §30.1 and a class holder across a suspension.** The ban is stated for a value type that holds borrows. A class instance built over a caller-sourced parameter and held across an `await` is the same borrow one hop away; the text is silent because today it is a field-held trust admission. Unresolved under A: see the async corner in §3.

Compiler and corpus findings (MEASURED on r40).

- **S5. `s22_6_2312` (expect: run, output 30) reads disposed temporaries.** By K8.1 the array literals die at the declaration's end; r40 prints 30. Either the case is wrong or this is the prior lane's k3e behaviour (#6775).
- **S6. Proposal 0205 is not implemented**: an alias (p07) or a record holder (p05) used after a dominating `Dispose()` compiles, faults in checked mode and segfaults unchecked (exit 139).
- **S7. K5.5's frame-local freeze is not implemented**: a source moved under a live iterator frame compiles and faults (p26).
- **S8. The trust report has one false line and one missing line.** It names `_loud := o.Verbose`, a `bool` read through a parameter stored in a `bool` field (p22, line 22). It is silent on `Items := a`, a constructor parameter of type `Array<int>` stored in a plain field (p31).
- **S9. A class-typed `scoped` argument is diagnosed in closure words** ("a capture-bearing closure is not admitted here", "captured source `a`"; p10, p30).
- **S10. Changes since the prior lane's r35 run**: `Weak<T>.TryUpgrade()` now has the specified shape and the weak registry runs (k2b); every other k-probe is unchanged, the silent `Register(item) => _items.Add(item)` report gap (k2a) and k1e included.

## 8. Migration and open questions

**What breaks** (MEASURED): 5 accepted-tier programs are rejected and 12 need a marker; 12 rule cases pin the report's text and 17 cite the field-held clause; 3 gtest files assert report lines. In the expected-reject tier and gtest, 19 more signatures need `scoped`, 8 distinct gtest stores write into an object the member does not hold, and 1 expected-reject case keeps an `Object` parameter in a method.

**A gradual path exists**, and each step is useful alone.

1. *Report.* `--report-trust-admissions` prints A's verdict beside each line. No program changes.
2. *The consumer.* A construction of a borrow-bearing class carries its plain arguments into K5.5 and §28.2 — the mint rule, read from the type, with no callee-side change yet. This rejects only a program that kills a source under a live holder: 0 in the accepted tier, 1 dead-holder site in gtest.
3. *Warning.* A method store of a plain parameter (an `Object` one included), a temporary source, a holder that outlives a source, an owned upcast.
4. *Error*, as a Ratchet (process §2). The opt-out is decided with the proposal, before this ramp starts; step 1's data sizes it.

**Open questions.**

1. The three open soundness items of §0: generic-body erasure (A-5's second half), the destructor window (S2), a class holder across an `await` (S4). And the ruling S1.
2. A-5's lift: is a borrow-bearing interface declared, or derived from a `scoped` parameter in its members? The corpora give no site to decide on.
3. The mint rule against A-explicit: one over-approximated site in 312 says the implicit rule costs nothing here; real constructors that take a context object they only read may differ.
4. Does a method that keeps an `Object` parameter get a spelling, or is retyping the answer (DECLINED row 141 rejects `scoped` at `Object`)?
5. A store that relates two parameters (9 sites): reject, or let a `scoped` parameter name another parameter as its bounding object? The second is the first step toward C.
6. Should the field-held admission survive as an explicit per-field opt-out? It is the escape route for what A-4 takes from the back-reference idiom (DECLINED row 48), so it is to be decided with the proposal; how often it would be written is unknown until step 1 runs on a real service.
7. The corpora are test programs. Kind 2 is under-represented by construction, and no measurement here can price it.

**What could not be done.** The compiler was not built or modified, so A's checker cost is unmeasured. The model has no calls. Prior art beyond the three checked lines is from memory.

## 9. Proposal outline (not a filed proposal)

In the shape `spec-change-process.md` §6 expects. Class: **design change** (it deletes a recorded admission and changes what compiles), landing later as a **Ratchet** with the ramp of §8.

| | |
|---|---|
| **Status** | outline — not drafted, and not ready to draft as kernel text: three soundness items are open (§0) |
| **Tracking issue** | none; #7509 is the work package this would revise |
| **Target module(s)** | spec §3.6 (one row), §9.7 (the instance-field and borrow-erasing bullets), §28.3 (K2.3, K4.3, K5.5, K6; the field-store bullet's clauses 1–5; *Diagnostics*), §28.11 (the store rule), §28.14 (class-typed `scoped`: the bounding-object admission widened to any traced holder), §28.2 (the holder list), §30.14 (one cross-reference), Appendix C; stdlib §1 (a holds-borrows statement per compiler-known type), §4; rationale §5.1, §5.26, §5.31; Guide 01 §5; code-generation reference §4 |
| **Written against** | v0.355 (`a30bb11df`) |

1. **Thesis.** A class that declares a borrow is a holder; a holder is a value with a source set; the three clauses that trusted it are deleted.
2. **Ground truth.** §2's table; probes p01–p32; the 178 recorded sites.
3. **The rule.** A-1 to A-5 as kernel text, each tagged *derives* (K2.3, K5.5, K6.2) or as one new axiom (the mint rule's over-approximation is the candidate).
4. **Options.** A, A-explicit, A with an opt-out field, B, status quo; D and C recorded as declined with §3's grounds.
5. **Soundness.** §3's invariant; §4's table; the model as the regression oracle for A-2 and A-3. Owed before drafting: A-5's generic half run against the generics matrix, and the destructor-window ruling for the self-source case.
6. **Corpus accounting.** The 5 rejected programs, 12 edited, 12 + 17 rule cases and 3 gtest files to convert; the 534 classes whose derived property is new.
7. **Records to answer.** DECLINED row 48 and rationale §4.2.6 (a callee-side no-store rule for every `Object` or interface parameter "retires the back-reference idiom": A-4 is that rule, widened to class types and narrowed to non-constructor members; the idiom's constructor form is untouched, the method form takes `scoped`, 31 signatures are edited and 11 stores rejected across the corpora, and the per-field opt-out is the escape route — §3 has the argument); DECLINED row 68 (a caller verdict from another body: not this — the caller reads a type and a signature); row 605 and 0132 §7 (superseded, not reopened); row 693 and 0159 (the posture "stored past its bound at no position" becomes true for classes too); row 808 (not the always-on report: the ramp warns only at the stores A will reject, 19 of 79 in the accepted tier, as a scheduled Ratchet); rationale §9.7, by analogy only — the section is about `pure` (A's fact is structural, but A-5's generic half is a body-derived requirement of the kind §3.6 rule 2 already carries); DECLINED row 141 (`scoped` at `Object` stays rejected: a method cannot keep an `Object` parameter); A11 (no field is flow-marked: the set is on the value).
8. **Ramp.** §8's four steps, with both ends of the Migration note.
9. **Open questions.** §8's list.

## 10. Files

- `data/scan_static.py` — the scanner; `data/bzlex.py`, `data/scan_trust.py` — unmodified copies of the prior lane's; `data/hand_check.tsv` — the eleven hand verdicts; `data/out/` — `classes.tsv` (796 rows), `stores.tsv`, `holders.tsv` (312 rows), `kills.tsv`, `rollup.tsv`, `summary.txt`. Doc units are named by their extracted block (`scripts/extract_doc_examples.py` on an exported copy), not by a path in the tree.
- `model/bzcore_static.py` — the model copy; `model/run_sweeps.sh`; `model/out/` — every number in §4.
- `probes/p*.bzl`, `probes/run_all.sh`, `probes/results.md`, `probes/results_prior_k_probes_on_r40.md`.
