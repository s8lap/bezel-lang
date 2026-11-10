# Static ownership tracking: can the compiler bound stored borrows itself, and retire the trust admissions?

Research lane, 2026-11-10. Base: origin/main `a30bb11df` (spec **v0.355**). Compiler for probes: the r40 prerelease `bezelc 0.0.0-a30bb11d`, built from that commit. Model: a copy of the prior lane's `bzcore_trust.py`, extended in `model/`. Scanner: the prior lane's, copied and extended in `data/`. Nothing under `src/`, `tests/` or the normative documents was edited; nothing was filed or pushed.

Every number and claim carries one of three labels. **MEASURED**: produced by a script or probe in this directory. **QUOTED**: taken from the tree at the cited place. **HYPOTHESIS**: reasoning that nothing here measures. Prior art is from general knowledge unless a line says it was checked by web search. A "DECLINED row *n*" is line *n* of `docs/proposals/DECLINED.md` at this base, always named with its topic.

## 0. One page for the owner

**The question.** K4.3 (§28.3.1, v0.355) admits a borrow stored where the body-local checker cannot bound it — a plain field holding a parameter's object, a collection slot, a comparer, a frame — on *trust*. Checked mode's disposed flag (K4.1) is the backstop; an unchecked build is undefined behaviour. Can a simple rule track these statically instead, and shrink the trust surface?

**The answer: yes, for the field-held, collection-held and comparer-held clauses, with a rule the language already has.** The static regime the question asks for exists today for three kinds of value that carry a borrow: a borrow-bearing record (§9.7), a function value (§28.3's closure bullet, §28.14), and an iterator or query frame (K6). Each is read with a source set, and each is scope-checked and frozen through the local that holds it. Only a *class instance* that holds a borrow is left wholly on trust. **Candidate A** closes that gap: a class with a plain class- or interface-typed field is *borrow-bearing* by a derived type property (§3.6), and its instances are read as K6 reads a frame — a value that carries a source set. The recommended form needs no new syntax:

- a **mint** (a constructor call, or a call whose declared return type is an `owned` borrow-bearing class) carries the receiver and the sources of its plain arguments — K6's rule for a frame, verbatim;
- a **later store** (a method that keeps a parameter) is announced with the `scoped` parameter that exists today (§28.14); a plain parameter becomes call-bounded for a class-typed value as it already is for a function value;
- the holder then obeys the rules a record local and a frame local obey: the scope comparison (K2.3), the freeze through its holder (K5.5), and the dominated-use error (§28.2).

The body-local posture is kept. Nothing is inferred from another body. The property is derived from declarations, like trivial-dispose and sendable.

**Headline numbers.**

| | A (recommended) | A-explicit (`scoped` everywhere) | B (derived retention) | D (stricter freeze) | E (strict mode) |
|---|---|---|---|---|---|
| Recorded K4.3 sites in the corpora the rule covers (MEASURED: 200 field/collection stores + 2 comparer, of 221 recorded sites) | **202 of 221** | 202 of 221 | checks 25 resolved kind-3 constructor stores; retires no clause | 0 | 0 (turns each into an error where opted in) |
| Accepted-tier programs (5,750) newly rejected (MEASURED, hand-checked) | **4** (0.07 %): 3 hazardous, 1 inconvenient | 4 | 0 | at most 4, each a test of the backstop itself | every program with a store: 72 |
| Accepted-tier programs that compile after adding `scoped` | **17** (19 signatures, 8 visible call sites) | 66 (75 signatures) | 0 | 0 | — |
| Model, vars 3 / len 5, straight line: newly rejected of 354,933 accepted | 8,268 (2.3 %): 2,094 race or fault, 5,670 hold a dangling borrow never read, 504 clean | — | 4,218 | 0 (528 of 71,946 with a branch) | 200,103 |
| Model, same bound: of 2,106 programs that read a dead or foreign object through a stored borrow, left accepted | **12** (a slot kill, K5.6) | — | 1,020 | 2,106 | 0 |
| Model: newly accepted / newly unsound, every bound | **0 / 0** | — | 0 / 0 | 0 / 0 | 0 / 0 |
| Model: races through a stored borrow left, every bound | **0** | — | > 0 (nested holders, method stores) | unchanged | 0 |

All model rows are MEASURED (`model/out/`); §4 has every bound, including branches, loops, inner scopes and a holder whose destructor reads its borrow, where more than nine in ten of A's rejections race or fault.

**What A costs.** (1) A method that keeps a plain parameter must say `scoped`: 19 signatures in the accepted tier. (2) A holder declared before its source — the long-lived registry — is rejected; it must reorder, own, or use `Weak<T>`, which works on r40 (probe p28). (3) An `owned` borrow-bearing instance cannot be converted to an interface that is not borrow-bearing: 0 sites in the corpora, 30 classes that could meet it. (4) A fresh temporary cannot be the source of a holder that outlives the statement: 3 accepted-tier programs, all of which read a disposed object by the spec's letter.

**What A does not reach.** The frame-held clause (an unsettled task), §28.14's two residuals, and the slot-displaced transfer (0210) stay recorded: 19 of 221 sites. The flag also keeps two jobs that are not trust: an announced kill on some paths only (K4.2) and a slot kill (K5.6). Those are today's posture for a *local* borrow; A only makes a holder behave like one.

**The limit of the evidence.** The corpora are test programs. They under-represent the long-lived registry (kind 2), so "4 programs" is a floor for real code, not a forecast. Step 1 below exists to replace it with a measurement.

**Recommendation and order of work.**

1. **Adopt A as the direction, in report mode first.** Extend `--report-trust-admissions` to print, per store and per construction, the verdict A would give. That measures real programs at no risk, and it is the ramp `spec-change-process.md` §2 asks of a Ratchet. §9 has the proposal outline.
2. **Before A can land, close the gaps that A reuses** (MEASURED on r40): proposal 0205's borrow and holder legs are not implemented (p07, p05 compile, fault in checked mode and segfault unchecked), and K5.5's frame-local freeze is not implemented (p26). Both are spec-ahead-of-implementation, not design questions. The class-typed `scoped` form still rejects `new Child(scoped this)` (k1e); whether the spec admits it is ruling S1 below.
3. **Two rulings are needed first**: the destructor window (a child's destructor reading its owner during the owner's cascade faults on r40, probe k1a — unchanged since the prior lane), and whether §28.14's ordered bounding forms admit a constructor call assigned to an `owned` field of `this` (§7, S1).
4. **Do not build derived retention's interface carriage (B).** Its caller-side half — the freeze and 0205 reading a holder — is the same code A needs and should be built once. Its callee-side half, a per-parameter bit computed from a body, is what rationale §9.7 (`pure` is declared, not derived) and DECLINED row 68 (a callee-body retention summary at the call) object to, and A does not need it. B stays the fallback if the owner declines A's one rule flip.
5. **Leave D declined, and record the measurement.** With 0205 in the baseline, D rejects only a kill on some paths: 0 programs on straight-line code, 528 of 71,946 with a branch, of which 312 fault on some run. In the corpora 4 sites exist, all tests of the backstop. It contradicts DECLINED row 308 (compile-time borrow-liveness checking; freezing an explicit `Dispose()`) and retires no trust clause. It is a separate question from K4.3.
6. **Decline C** (lifetime parameters). It is the precision ceiling; its measured gain over A in the corpora is 9 sites (a store that relates two parameters), at the cost of a second parameter system on every type and signature that touches a holder.
7. **E after A**, as the prior lane said. Under A a strict mode has little left to forbid: the frame clause and any opt-out field.
8. **Decide the escape hatch on evidence from step 1.** If real programs show registry shapes that `Weak<T>` and ownership do not express, keep today's trust admission behind an explicit per-field marker, so that the recorded set becomes "the marked fields". The corpora here do not show the need.

---

## 1. Method

**Corpora** (MEASURED, `data/scan_static.py`): 9,521 units at `a30bb11df` — 5,902 rule-corpus units, 802 doc-extracted blocks (553 fragments, 8 programs), 2,574 lit programs, 243 gtest files with embedded programs; 17,348 classes. The *accepted tier* is the 5,750 rule, doc and lit units the spec expects to compile. gtest files concatenate variants of one program, so they are counted by distinct site, never by program. The corpora have grown since the prior lane (7,353 units at v0.350).

**Scanner.** `data/bzlex.py` and `data/scan_trust.py` are unmodified copies of the prior lane's. `data/scan_static.py` adds four things. (1) The borrow-bearing classification of every class, to a fixed point: a plain class- or interface-typed field, a primary-constructor capture, a borrowing collection of class elements, an `owned` field of a borrow-bearing class, a borrow-bearing base. (2) For each K4.3 store the prior scanner finds, the kind of member that stores (constructor, method, setter, initializer), which decides whether A needs a marker. (3) Every construction `new C(args)` of a borrow-bearing class: where the instance is held, where each carried argument comes from, and any kill of a carried local while the holder is named later. (4) Every explicit `Dispose()` of a local whose alias is named afterwards (D's population). Two populations the prior lane counted are separated out: 66 stores that carry no borrow (a value instantiation, a static source) and 46 stores into a purely local collection, which the report has been silent on since v0.350.

**Precision.** Every accepted-tier program the scanner rejected or marked as an escaping holder was read by hand: 10 programs, verdicts and reasons in `data/hand_check.tsv`. Six were scanner errors in A's favour (the prior classifier's kind 2 is "the holder leaves the body", which A admits when a signature announces it); four are real. The roll-up uses the hand verdicts. The scanner is syntactic and per unit: it resolves declared types inside one unit, treats an unknown type name as a class, and reads control flow as text order. The store classification inherits the prior lane's 24-hit precision check.

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

The existing static spelling, the class-typed `scoped` parameter, does not close the gap, for three measured reasons. It admits only a `using var` holder: a plain `var` is rejected (p10). The holder cannot be moved (A5). And a nested holder is inexpressible: a `scoped` parameter may not be forwarded into `new Account(scoped c)` stored in an `owned` field (p12, QUOTED §28.14: "Storage reached through `owned` fields of the bounding object is excluded"). The corpora agree: 180 plain stored parameters against 19 class-typed `scoped` parameters.

## 3. The candidates as kernel deltas

### A. Borrow-bearing classes (recommended)

**A-1, the property (§3.6).** A class *holds borrows* when it declares or inherits (i) a non-`owned` instance field of class or interface type other than a runtime-managed type, or (ii) an `owned` field or owning-inline-array slot whose type holds borrows. The stdlib states it for its own types: a borrowing collection at a class, interface or holds-borrows element; an owning collection, `Shared<T>` or `Locked<T>` at a holds-borrows element; a hash collection built with a comparer. It is read per instantiation (§3.6 rule 2). Computed, never declared (rule 1).

**A-2, the value carries a source set (K6 widened).** K6's "frames are values" gains a fourth kind, the *holder*. Its set is built in three ways.
1. A **mint** — a constructor call, or a call whose declared return type is an `owned` class that holds borrows — carries the receiver and what its plain arguments contribute (§28.3 *The sources an argument contributes*). This is K6.2's announcement by declared type.
2. An **`owned` argument that holds borrows** brings its own set to whatever the callee can put it in: the receiver, where the receiver's class holds borrows, and the result, where the call is a mint.
3. A **later store** — a `scoped` argument, or a property or indexer store of a reference — adds the argument's sources, never removes one, and is checked as an assignment to the location that holds the receiver. This is §9.7's *Member stores* rule read for a class.

A borrow of a holder carries the holder's binding and, through it, the holder's set (K2.2).

**A-3, the holder is bounded (K2.3 with no residual for these stores).** Every rule that reads a record local or a frame local reads a holder: the local-scope bullet; the static bullet; K8.4's no-temporary-source rule; K5.5's liveness, so the silent kills of a carried source are frozen while the holder is live; and §28.2's dominated use. Where the class is not trivial-dispose its release is a use, and a source must be declared before the holder (§28.14's test). A holder is live to its last use where the class is trivial-dispose.

**A-4, the callee side.** A plain class- or interface-typed parameter is call-bounded, as a function-typed one is today. Its borrow, direct or carried inside a holder, may reach three places: the object under construction, in a constructor; a purely local holder or collection, which may then be returned through a mint; and, if the parameter is `scoped`, the bounding object. The check reads the stored value's source set, as §9.7 reads a record's. `this` and a receiver-sourced borrow may be stored into storage `this` owns: the self-source case, which is kind 1. K4.3's field-held, collection-held and comparer-held clauses are deleted.

**A-5, erasure at owning positions.** An `owned` value that holds borrows, with a non-empty set, may not be converted to a declared type that does not hold borrows at an *owning* position: an `owned` field, parameter, return or slot typed `Object`, an interface, a base class, or a type parameter substituted with one. Borrowing positions are untouched, because under A-4 a plain parameter cannot retain. This is §9.7's borrow-erasing bullet, narrowed.

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

**Soundness argument.** Invariant: for every live object *o* of a class that holds borrows, each object reached through *o*'s non-owned fields is static, or is *o*'s own owner (self-source), or is the value of a source in *set(o)* as read at the binding that holds *o*. The set changes only at a mint or an announced store, and each is checked against the holder's binding (A-2, A-4). A source in the set cannot end before the holder by scope (A-3), and cannot be killed silently while the holder is live (K5.1, K5.2, a domain transfer included). So no stored borrow is read after a silent kill, and none is read across a domain: the domain clause has nothing left to say for these stores. Two cases remain, both flag-backstopped exactly as for a local borrow today: an announced kill that does not dominate the use, and a kill of a slot's current value. The model agrees at every bound (§4).

**The corners the brief asked about.**

- *How the source set is determined.* By the mint and by announced stores; never from a body. The mint rule over-approximates: an argument the constructor only reads is carried too (p22). MEASURED cost: 1 construction site of 430 carries more than the stored parameters, and it is not rejected. A-explicit removes the over-approximation at the price of 75 markers.
- *Inheritance and virtual dispatch.* The property is inherited (62 of 667 borrow-bearing classes have it from a base). A class that holds borrows may derive from one that does not; the constraint is A-5, at the owned upcast. An override keeps the declaring signature's `scoped` markers, as §28.14 already requires for function types.
- *Interfaces — the crux.* A borrow of a holder may be passed at any interface type. What is rejected is *owning* it at a type that announces nothing: `static method For(c: Customer): owned IReport => new OwnerReport(c)` (p14). MEASURED: 0 such sites in 430 constructions; 30 of 667 borrow-bearing classes list a base or interface that is not borrow-bearing. Every comparable language picked the same default (§6): Rust's `Box<dyn Trait>` means `'static`, Swift's generics and protocols exclude a non-escapable type unless they say `~Escapable`, C# 13 forbids converting a `ref struct` to an interface. The lift, if evidence asks for it, is an interface that declares itself borrow-bearing; an interface that declares a `scoped` class-typed parameter is already announcing that its implementations retain.
- *Generics.* `Box<T>` with `owned _v: T` holds borrows at a `T` that does (p20), read per instantiation. A generic member that stores a bare-`T` parameter in a method (`Bag<T>.Push(item: T)`) needs `scoped item: T`, erased at a value substitution, or the class instantiation is rejected where it is named — §9.7's `Keep<Pair>` rule, extended. 253 classes have a bare-`T` field.
- *Async frames and closures.* A closure that captures a holder contributes the holder's binding and set (provenance clause 1, as for a captured record). A holder built in an `async` body over a parameter is inside the frame's carried set; the frame-held clause is unchanged. HYPOTHESIS: §30.1's ban on a holds-borrows *value* local across a suspension need not be imported for a class, since a class-held borrow is flag-checked at every access; this needs a ruling.
- *`Shared<T>`.* `Shared<Account>` holds borrows; every handle and `Clone()` result carries the set (p24).
- *Owned fields of borrow-bearing type.* The container holds borrows (p13). This is the case a per-parameter bit cannot see: `Branch`'s constructor stores no plain parameter, it stores an owned `Account`.
- *A span or record field.* A would make it sound to lift §9.7's instance-field ban, since a span field would make its class borrow-bearing. This study does not propose the lift; DECLINED row 693's bar stands.
- *Returning one.* An `owned` return is a mint and carries the receiver and plain arguments (p21, k3d). A plain return is receiver-sourced as today.
- *Kind 1, the back-pointer.* Nothing to write. `_child := new Child(this)` stores a value whose only source is `this` into storage `this` owns. A child moved out is returned `owned` from an instance member, so it carries the receiver: the prior lane's "sharp" rule falls out of the mint rule. An owner built from no borrowed argument has an empty set and may be held anywhere, a static included (p23). The retained-UI exploration's `Mount(owned child)`, which writes `child._parent := this` and then moves the child into its owned collection (QUOTED, `docs/research/exploration-ui-vcl-retained.md`), is this case: the store adds `this` to a local holder, and the move is self-sourced. Two rulings are owed (§8).
- *Siblings.* A field of `this` stored into a holder that another `owned` field of `this` keeps — the generated DI root's `_cache := new RedisCache(_db)` (QUOTED, `docs/research/exploration-compile-time-di.md`) — is self-sourced too. The cascade runs in reverse declaration order (§28.4), so the borrowing field must be declared after the field it borrows: the declared-before test, on fields. An overwrite of the borrowed slot stays K5.6.
- *Kind 2, the registry.* A holder declared before its source is rejected (p17, p30). The program reorders (p29), owns the items, keys them (`SlotHandle`), or observes them through `Weak<T>` (p28). None of these is new.
- *Kind 4, the unknown body.* The interface member declares `scoped`; the store is then checked at the caller against a visible holder. A helper that relates two parameters — `Feed(sink: ISink, it: Item) => sink.Keep(it)` — has no spelling and is rejected: the gap only C closes.

**Hybrids.** *A with one explicit escape hatch*: keep today's admission behind a marker on the field, so the trust set becomes the marked fields and E is "none in this module". Recommended only if step 1's measurement shows a need. *A inside a module, B across modules*: not needed. A's property is a §3.6 derived type property and crosses a module boundary in the artifact interface as trivial-dispose and sendable do.

### B. Derived retention (the baseline)

QUOTED (0132 §1): a per-parameter `retains` fact computed from the callee's body, carried in the interface, for a direct store into a `readonly` field of `this`. The caller's freeze and 0205 then see the holder. It adds no syntax and rejects no clean program. What it cannot do, each MEASURED: it sees no nested holder (p13; model `newp`); its definition keys on a `readonly` field, which the tree does not write (0132 §6), and read as the prior lane read it — any constructor store — it still leaves the 62 method stores of 200 outside; it reaches no interface call, function value or uncarried artifact; and it retires no clause — the residual set stays, checked where the fact happens to be visible. It also needs what rationale §9.7 declines (a body-derived fact in the interface) and what DECLINED row 68 rejects (a caller's verdict that depends on another body). A answers both records: the fact is a type property, and the caller reads a signature.

### C. Lifetime or region annotations (the ceiling)

A type that holds a borrow takes a lifetime parameter, and every signature that mentions it names or elides one. This relates any two parameters, accepts a dead holder, and needs no scope over-approximation. Its user cost is a second parameter system: every one of the 667 borrow-bearing classes, and every signature that returns or stores one, would carry it. Elision covers the single-parameter case, which is 90 % of stores (prior lane, QUOTED), and that case is exactly what A's mint rule covers with no parameter at all. MEASURED gain of C over A in the corpora: the stores that relate two parameters — 1 accepted-tier site and 8 distinct gtest sites. In the model, the programs only C would accept are A's "never read" and "clean" rejections (§4). DECLINED row 308 rejects the exclusive-writer rule, not lifetimes as such; C is declined here on cost, not on that row.

### D. Stricter freeze

K5 clause 3 flips: a site-announced kill is frozen while a local borrow of the value is live. With 0205 in the baseline the only new rejections are kills on some paths. MEASURED: 0 on straight-line code; with a branch at vars 3, 528 of 71,946 accepted programs, of which 12 fault on every run, 300 on some run, and 216 on none (a stale borrow copied but never read). In the corpora, 4 sites, all of them tests of the backstop itself. D retires no K4.3 clause. The recorded ground (rationale §5.1, DECLINED row 308) is "a de-facto borrow checker for no soundness gain where the backstop is real". 0205's own record already qualifies "no soundness gain" for the unchecked build. What the model cannot show is the cost of correlated conditions (`if done then x.Dispose(); … if not done then b.Use();`), which a path-insensitive liveness rejects although no run faults. HYPOTHESIS: that is the real cost and it is not small in service code. D is a separate decision from trust.

### E. Per-module strict mode

Every remaining admission is an error unless marked. It rejects exactly the report's lines: 72 accepted-tier programs hold a K4.3 store today. As the prior lane said, it is cheap and it presupposes a complete report (#7465–#7471 remain open per that lane; p22 adds a false line, §7). Under A it shrinks to the frame clause, the §28.14 residuals, and any opt-out field.

### Others considered

- **No stored borrows at all** (Hylo's rule): reject every plain class-typed field. MEASURED: 667 classes, in 231 accepted-tier programs. It is the simplest rule, and it rejects 231 programs where A rejects 4; kind 3 would have to pass its source to every call.
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

1. **A is sound at every bound**: 0 newly accepted, 0 newly unsound, 0 races through a stored borrow left, across ten sweeps and 2.08 million programs at the largest. A rejects 0.8 % to 2.3 % of accepted programs where the holder is trivial-dispose and 6.8 % to 11.4 % where its destructor reads the borrow.
2. **A removes the trust surface; B removes about half of it.** At vars 3 / len 5, 2,106 accepted programs read a dead or foreign object through a stored borrow. A rejects 2,094 of them and leaves 12, all of one shape (next point). B rejects 1,086 and leaves 288 races and 732 certain faults, through the nested holder (`new a / newp b a / send a / pkuse b`) and the method store, which a per-parameter constructor bit does not see.
3. **What A leaves is not trust.** The faults left under A are of two shapes. *An announced kill that does not dominate the use*: `new a / newk b a / if { } else { disp a } / kuse b`, and in a loop `new a / newk b a / while { disp a / asg a } / kuse b` — K4.2, as for a local borrow; the loop form is the residual 0205 records. *A slot kill*: `new a / fbrw b a / fst a / newk c b / kuse c` — K5.6, as for a local borrow of a slot's current value. A-static (the fully static bound §28.14 gives a `scoped` capture) removes the first shape and adds never-read rejections; it is the same trade as D, restricted to holders.
4. **How hazardous the rejections are depends on the holder's destructor.** Where the holder's release reads the borrow, 94 % to 98 % of A's rejections race or fault (37,872 of 40,398 at len 5). Where the holder is trivial-dispose, 18 % to 32 % do, and most of the rest hold a dangling borrow that no statement reads (5,670 of 8,268 at len 5): one added line away from a fault, and the same class K5.1 and K5.2 reject for a local borrow today. The clean rejections (at most 6 %) are transfers of a source while the holder is live (`new a / newk b a / move c a / disp b`): the recorded cost of the freeze, "re-borrow from the new owner".
5. **D rejects nothing on straight-line code and little with a branch**: 528 of 71,946 accepted programs at vars 3 with an `if` (12 fault on every run, 300 on some run, 216 on none), and 72 of 419,160 with a loop (42 and 30). Its never-faulting rejections are a stale local borrow that is copied but not read (`new a / brw b a / if { } else { disp a } / brw c b`). The model has no correlated conditions, so these figures are a lower bound on D's cost.
6. **E** rejects every program with a store: 200,103 of 354,933 at len 5, of which 2,106 are on the trust surface.

What the model cannot express: calls, so interface carriage, drift and A-5's erasure are outside it; the mint rule's over-approximation (one stored argument per holder); generics; frames. It has one holder class and one owner class. "Dangling never read" counts a holder that still exists; a holder already released is clean.

## 5. Corpus results (MEASURED, `data/out/summary.txt`)

**The property.** 667 of 17,348 classes hold borrows under A (3.8 %): 586 by a plain field, 20 by a borrowing collection, 3 by an owned field of a borrow-bearing class, 62 by a base. 10 have a destructor. 253 more have a bare-`T` field and are decided per instantiation.

**The stores.** 200 K4.3 store sites (166 field stores, 15 collection stores, 8 property stores, 7 initializers, 4 primary-constructor captures). 123 are in a constructor, 62 in a method, 11 in an initializer or capture, 4 in a setter. Beside them: 2 comparer-held, 3 frame-held, 13 §28.14 residuals, 3 slot-displaced sites. A covers 202 of the 221.

| population | sites | A: admitted as written | A: needs `scoped` on a method | A: rejected at the store | A-explicit: needs `scoped` |
|---|---|---|---|---|---|
| accepted tier (rules, docs, lit) | 79 | 59 | 19 | 1 | 75 |
| gtest, distinct | 46 | 27 | 11 | 8 | 35 |
| expected-reject tier | 35 | 22 | 13 | 0 | 35 |

At the visible call sites of the accepted tier: 16 kind-3 sites and 2 kind-1 sites are bounded as written; 5 escaping-holder sites were hand-checked (all admitted: a value instantiation, a returned holder that carries its receiver, a local holder over a parameter); 4 are behind an unknown body; 52 have no call site in the unit.

**The holders.** 430 constructions of a borrow-bearing class: 304 are bound to a local (162 `var`, 142 `using var`), 81 are returned, 35 are arguments, 4 are temporaries used as a receiver, 3 go to a field, 2 reassign a local, 1 is unclassified. Today's `scoped` reaches only the 142. B's v1 scope (0132 §2) reaches the same 142. A reaches all of them.

**The programs.** Of 5,750 accepted-tier programs, 176 hold a K4.3 store or construct a borrow-bearing class.

| | unchanged | compile after a `scoped` marker | rejected |
|---|---|---|---|
| A | 155 | 17 | 4 |
| A-explicit | 106 | 66 | 4 |
| B | 176 | 0 | 0 |

The four rejections (`data/hand_check.tsv`): `s28_3_0205` and `s28_3_0206` assert the checked-mode fault of a literal stored through a plain constructor parameter — hazardous by design. `s22_6_2312` stores array-literal temporaries in `Row.Items` and reads them two statements later; the spec disposes them at the declaration's end (K8.1) and r40 prints `30` — hazardous by accident, and a finding (§7, S5). `s28_11_0041` pins the admission of a store into a collection held by another parameter — inconvenient.

**B's reach.** Kind-3 sites with a constructor's store and a visible callee: 11 in the accepted tier and 14 distinct gtest sites. Kind-3 sites with a method's store into a mutable field, which 0132 excludes: 5 and 1. No accepted-tier program kills a source under a live holder, so B rejects none: the accepted tier is clean code and B only fires on a bug.

**D's population.** 4 accepted-tier sites dispose a local whose alias is named afterwards: `s28_2_0011`, `s28_2_0012`, `s28_3_0036`, and one lit test. All four test K4.2 itself.

**What the rejected code becomes.** Five rewrites; each "after" uses only what exists today.

1. *A method keeps its parameter* (19 signatures; `borrow_into_bare_field_1082`):
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

Kinds 1 and 3 with a constructor store need no rewrite. The two-level holder (p13) needs none and has no static spelling today (p12).

## 6. Prior art

From general knowledge, not re-verified, except the three lines marked *checked*.

- **Rust, lifetimes on structs.** `struct Account<'a> { owner: &'a Customer }`: the type carries the borrow's lifetime as a parameter, and every use site re-establishes it. For this exact problem Rust decides four things. A trait object is `'static` by default (`Box<dyn Report>`); a borrowing implementation needs `Box<dyn Report + 'a>` — the interface crux, resolved as A-5 resolves it. A type with a destructor must have its borrowed data strictly outlive it (the drop check) — A-3's declared-before test. A back-pointer to one's owner is not expressible; code uses `Rc`/`Weak` or indices — where Bezel can do better, because a self-sourced set is local to one object. Elision removes the annotation in the one-parameter case. *What Bezel can take*: the defaults, without the parameters.
- **Swift non-escapable types.** *Checked by web search*: SE-0446 was accepted with modifications in October 2024; `Escapable` is a protocol, a type opts out with `~Escapable`, a generic wrapper is conditionally escapable (`struct Box<T: ~Escapable>: ~Escapable`), and a value that is returned or initialised must state its lifetime dependence (`@lifetime`, experimental when checked). From knowledge: only structs and enums can be non-escapable, and existing generic code excludes them unless it opts in. This is candidate A's design applied to values, with dependence stated at the mint. *What Bezel can take*: "the mint states what the value depends on"; A's default applies the same idea to every plain argument, so nothing is written. Swift's `unowned` — a non-owning reference that traps on a stale read — is exactly today's plain field in checked mode, and `unowned(unsafe)` is the unchecked build.
- **C# `ref struct`.** *Checked by web search*: since C# 13 a `ref struct` may implement an interface but may not be converted to the interface type, and a generic parameter accepts one only with the `allows ref struct` anti-constraint; a `ref` field may be declared only in a `ref struct`. From knowledge: its escape rules are scope-based, with no lifetime parameters. This is the closest relative of Bezel's §9.7, and it draws A-5's line in the same place. *What Bezel can take*: confirmation that a scope-based rule with an erasure ban is workable at scale, and the generic opt-in as the model for the interface lift.
- **Mojo.** *Checked by web search*: references carry *origins*, mostly inferred, written out for `ref` arguments and results and for parameterised types such as `Pointer` and `Span`; values are destroyed as soon as possible after their last use, and an origin union extends several lifetimes. So a holder keeps its source alive: Mojo *extends*, where Bezel's K8.4 *rejects*. *What Bezel can take*: nothing directly — extension by origin needs last-use destruction, and Bezel's disposal points are scope ends by design (rationale §5.24).
- **Hylo.** Mutable value semantics; safe code has no stored references. A projection yields a borrow that cannot be kept in a field. The proposed "remote parts" are stored projections whose owner cannot escape — borrow-bearing types again. *Lesson*: "no stored borrows" works when everything is a value; §3's measurement prices it for Bezel at 667 classes.
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
- **S4. §30.1 and a class holder across a suspension.** The ban is stated for a value type that holds borrows. A class instance built over a caller-sourced parameter and held across an `await` is the same borrow one hop away; the text is silent because today it is a field-held trust admission.

Compiler and corpus findings (MEASURED on r40).

- **S5. `s22_6_2312` (expect: run, output 30) reads disposed temporaries.** By K8.1 the array literals die at the declaration's end; r40 prints 30. Either the case is wrong or this is the prior lane's k3e behaviour (#6775).
- **S6. Proposal 0205 is not implemented**: an alias (p07) or a record holder (p05) used after a dominating `Dispose()` compiles, faults in checked mode and segfaults unchecked (exit 139).
- **S7. K5.5's frame-local freeze is not implemented**: a source moved under a live iterator frame compiles and faults (p26).
- **S8. The trust report has one false line and one missing line.** It names `_loud := o.Verbose`, a `bool` read through a parameter stored in a `bool` field (p22, line 22). It is silent on `Items := a`, a constructor parameter of type `Array<int>` stored in a plain field (p31).
- **S9. A class-typed `scoped` argument is diagnosed in closure words** ("a capture-bearing closure is not admitted here", "captured source `a`"; p10, p30).
- **S10. Changes since the prior lane's r35 run**: `Weak<T>.TryUpgrade()` now has the specified shape and the weak registry runs (k2b); every other k-probe is unchanged, the silent `Register(item) => _items.Add(item)` report gap (k2a) and k1e included.

## 8. Migration and open questions

**What breaks** (MEASURED): 4 accepted-tier programs are rejected and 17 need a marker; 12 rule cases pin the report's text and 17 cite the field-held clause; 3 gtest files assert report lines. In the expected-reject tier and gtest, 24 more signatures need `scoped`, and 8 distinct gtest stores write into an object the member does not hold.

**A gradual path exists**, and each step is useful alone.

1. *Report.* `--report-trust-admissions` prints A's verdict beside each line. No program changes.
2. *The consumer.* A construction of a borrow-bearing class carries its plain arguments into K5.5 and §28.2 — the mint rule, read from the type, with no callee-side change yet. This rejects only a program that kills a source under a live holder: 0 in the accepted tier, 1 dead-holder site in gtest.
3. *Warning.* A method store of a plain parameter, a temporary source, a holder that outlives a source, an owned upcast.
4. *Error*, as a Ratchet (process §2), with the opt-out decision taken on step 1's data.

**Open questions.**

1. The two rulings: S1 and S2.
2. A-5's lift: is a borrow-bearing interface declared, or derived from a `scoped` parameter in its members? The corpora give no site to decide on.
3. The mint rule against A-explicit: one over-approximated site in 430 says the implicit rule costs nothing here; real constructors that take a context object they only read may differ.
4. Does a class holder across an `await` need §30.1's treatment (S4)?
5. A store that relates two parameters (9 sites): reject, or let a `scoped` parameter name another parameter as its bounding object? The second is the first step toward C.
6. Should the field-held admission survive as an explicit opt-out? Unknown until step 1 runs on a real service.
7. The corpora are test programs. Kind 2 is under-represented by construction, and no measurement here can price it.

**What could not be done.** The compiler was not built or modified, so A's checker cost is unmeasured. The model has no calls. Prior art beyond the three checked lines is from memory.

## 9. Proposal outline (not a filed proposal)

In the shape `spec-change-process.md` §6 expects. Class: **design change** (it deletes a recorded admission and changes what compiles), landing later as a **Ratchet** with the ramp of §8.

| | |
|---|---|
| **Status** | outline — not drafted |
| **Tracking issue** | none; #7509 is the work package this would revise |
| **Target module(s)** | spec §3.6 (one row), §9.7 (the instance-field and borrow-erasing bullets), §28.3 (K2.3, K4.3, K5.5, K6; the field-store bullet's clauses 1–5; *Diagnostics*), §28.11 (the store rule), §28.14 (class-typed `scoped`: the bounding-object admission widened to any traced holder), §28.2 (the holder list), §30.14 (one cross-reference), Appendix C; stdlib §1 (a holds-borrows statement per compiler-known type), §4; rationale §5.1, §5.26, §5.31; Guide 01 §5; code-generation reference §4 |
| **Written against** | v0.355 (`a30bb11df`) |

1. **Thesis.** A class that declares a borrow is a holder; a holder is a value with a source set; the three clauses that trusted it are deleted.
2. **Ground truth.** §2's table; probes p01–p32; the 221 recorded sites.
3. **The rule.** A-1 to A-5 as kernel text, each tagged *derives* (K2.3, K5.5, K6.2) or as one new axiom (the mint rule's over-approximation is the candidate).
4. **Options.** A, A-explicit, A with an opt-out field, B, status quo; D and C recorded as declined with §3's grounds.
5. **Soundness.** §3's invariant; §4's table; the model as the regression oracle.
6. **Corpus accounting.** The 4 rejected programs, 17 edited, 12 + 17 rule cases and 3 gtest files to convert; the 667 classes whose derived property is new.
7. **Records to answer.** DECLINED row 68 (a caller verdict from another body: not this — the caller reads a type and a signature); row 605 and 0132 §7 (superseded, not reopened); row 693 and 0159 (the posture "stored past its bound at no position" becomes true for classes too); row 808 (not the always-on report: the ramp warns only at the stores A will reject, 19 of 79 in the accepted tier, as a scheduled Ratchet); rationale §9.7 (the fact is structural); A11 (no field is flow-marked: the set is on the value).
8. **Ramp.** §8's four steps, with both ends of the Migration note.
9. **Open questions.** §8's list.

## 10. Files

- `data/scan_static.py` — the scanner; `data/bzlex.py`, `data/scan_trust.py` — unmodified copies of the prior lane's; `data/hand_check.tsv` — the ten hand verdicts; `data/out/` — `classes.tsv` (920 rows), `stores.tsv`, `holders.tsv` (430 rows), `kills.tsv`, `rollup.tsv`, `summary.txt`. Doc units are named by their extracted block (`scripts/extract_doc_examples.py` on an exported copy), not by a path in the tree.
- `model/bzcore_static.py` — the model copy; `model/run_sweeps.sh`; `model/out/` — every number in §4.
- `probes/p*.bzl`, `probes/run_all.sh`, `probes/results.md`, `probes/results_prior_k_probes_on_r40.md`.
