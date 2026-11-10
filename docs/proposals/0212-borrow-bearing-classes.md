# Proposal 0212 — Borrow-bearing classes: an object that stores a borrow carries its sources, and the field-, collection- and comparer-held trust clauses are retired

| | |
|---|---|
| **Status** | accepted — staged: stage 1 graduated v0.357 (owner-delegated rulings 2026-11-10, §11 *Rulings*); stages 2–3 scheduled (gates §7, Q9). Written under the owner's direction of 2026-11-10 on the static-ownership-tracking research note ("Proceed with Fable work to make static ownership tracking part of the spec"); graduated under the owner's direction of the same day ("Please resolve all open questions in these proposals, then adjust any proposal or write any new ones as needed. Then, graduate the two proposals."), which delegated the rulings. A design change (spec-change-process §2): it deletes three recorded K4.3 admissions, changes what compiles, and reopens a recorded decline (`DECLINED.md`, the callee-side no-store row; rationale §4.2.6) on new evidence (§9) — at its stage 3, a Ratchet whose design this acceptance rules. Stage 1 (v0.357) changes no verdict: it lands the property, the rules as the trust-admission report's classification, and the ramp (§7.5 records what landed and every anchor adapted). **Critique round 1** (an independent adversarial critic, on the first draft `f72926c68`): *rejected as drafted, direction judged sound* — five unsound holes and one sendability regression, and six notes; revision 2 (`4dfa0e331`) closes each. **Critique round 2** (a second independent critic, on revision 2): *ACCEPT-W-NOTES* — all six blockers fixed, no new unsound route; its three notes are folded in (§4.5, §4.7, §8.2). §12 is the record of both rounds. |
| **Tracking issue** | [#8411](https://github.com/s8lap/bezel-lang/issues/8411); [#7509](https://github.com/s8lap/bezel-lang/issues/7509) is the open work package this proposal would revise. |
| **Target documents** | **spec**: §3.6 (one table row), §28 *Chapter at a glance* (one row), **§28.15 (new)**, §28.3.1 (K2.3; a new K2 clause 5; K4.3 and its example; K5.5; K8.4), §28.3.5 (*The sources an argument contributes*, items 1 and 5), §28.3.8 (the field-store bullet's five clauses; one phrase of the closure bullet's provenance clause 1), §28.3.12 (the trust-admission report: clause 1, a new clause 4, the closing paragraph), §28.11 (*Storing a borrow*), §28.14 (scoped construction: three paragraphs and one new bullet), §22.3 (one sentence), §28.2 (the holder list), §9.7 (one sentence of the instance-field bullet), §30.1 (one paragraph added), §30.14 (one sentence), Appendix C.3 (one bullet), and the consequential mentions and example blocks listed in §8.13. **stdlib**: §1 (a new statement paragraph), §4 (*A retained comparer is a borrow*). **tour**: one example. **rationale**: §4.2.6 (one sentence), §5.1 (one paragraph), §5.31 (one paragraph), a new §5.32. **guides**: Guide 01 §3 and §5; Guide 05 §1 (one paragraph added). **code-generation reference**: §4.4, §4.6, §7.2. **proposals**: `README.md` (this row), `DECLINED.md` (six rows annotated, eight added). **conformance**: §7.4 lists the cases to add at each stage and the cases whose verdict changes at the last. **compiler** (spec-ahead-of-implementation, non-normative): §10. |
| **Written against** | spec **v0.355** at `a77e4718a`; re-verified for graduation at `156fdf9c9` (`origin/main`, v0.355): every anchor of §8 still occurs exactly once (`check_anchors.py`: 41 anchors, 0 failures), and stage 1 graduated as v0.357 with the adaptations §7.5 lists. Original pin: (`origin/main`; the twelve documents are byte-identical to `a30bb11df`, the base of the research note). Section numbers are v0.355's. Every `old` anchor quoted in §8 is checked to occur exactly once at this base by `docs/research/static-ownership-tracking/0212/check_anchors.py`. |
| **Origin** | `docs/research/static-ownership-tracking/README.md` (2026-11-10) recommended its candidate A — a class with a plain class-typed field is borrow-bearing, and its instances are read as K6 reads a frame — in report mode first, and listed three soundness items as blocking kernel text: generic-body erasure, the destructor window, and a class instance across an `await`. This proposal is candidate A finished: those three items ruled, the note's other open questions answered, the rules extended in an executable model, and the corpora re-measured. Supporting material is under `docs/research/static-ownership-tracking/0212/`; the critic's five probes are kept there as `probes/c01`–`c05`. |

*Reference convention. Unprefixed §-references are language-spec references at v0.355; K tags are §28.3.1's kernel ids; "the note" is the research note above. ▲ marks proposed normative text. Every statement of fact carries one of three labels: **MEASURED** (produced by a script, model run or probe under `0212/`, or re-run from the note's own scripts), **QUOTED** (taken from the tree at the cited place), **HYPOTHESIS** (reasoning nothing here measures). Probes were compiled with the r40 prerelease `bezelc 0.0.0-a30bb11d` in three modes (checked with the report, checked, unchecked) and run; `0212/probes/results.md` has every verdict. "The first draft" is this proposal at `f72926c68`. Programs use only members the standard library declares or declare their own.*

---

## 0. Summary

**The gap.** Three kinds of value that carry a borrow are tracked statically today: a borrow-bearing record (§9.7), a function value (§28.3.8, §28.14) and an iterator or query frame (K6). Each is read with a source set, scope-checked, and frozen through the local that holds it. A fourth kind is not: a *class instance* with a plain class-typed field. Its stored borrow is admitted on K4.3's field-held, collection-held or comparer-held trust clause. Checked mode's flag is the backstop, a transfer to another domain has no backstop at all, and an unchecked build is undefined behaviour.

**The proposal.** A class that declares a stored borrow is a **borrow-bearing class** — a derived type property (§3.6), computed from declarations. Its instances carry a source set, built from what callers can read in a signature:

- a **mint** — a constructor call, a call that returns such an instance `owned`, or one that hands it back through an `out owned` parameter — carries the receiver and what each argument contributes;
- a **later store** — an argument at a `scoped` parameter, a property store, a standard collection's `Add` — adds sources and is checked as an assignment to the location that holds the instance;
- a **move-only value that owns such an instance** — an owning choice or an owning inline array, `Result<T, E>` among them — carries what the instance carries;
- the instance then obeys the rules a record local and a frame local obey today: the scope comparison (K2.3), the no-temporary-source rule (K8.4), the freeze (K5.5) and the dominated-use error (§28.2).

On the callee side a plain parameter becomes call-bounded for an object borrow, as it already is for a callback: only a constructor may keep it, or a member that declares it `scoped`. An `owned` parameter that carries sources may go only where its caller can account for it: into `this`, back out, or nowhere. K4.3's three clauses for stored borrows are deleted. The frame-held clause, §28.14's two callback residuals and the slot-displaced transfer stay.

**The three blocking items, ruled** (§3).

1. *Generic erasure.* An owned value whose source set is not empty may not be converted, at an owning position, to a type that cannot carry one. The owning positions are the consume table's rows, read one by one (§3.1). In a generic body that conversion is a requirement of the body's use of the type parameter, read at each instantiation (§3.6 rule 2) — the mechanism `Keep<Pair>` uses today. A dispatched generic method decides it at its body. MEASURED cost in the corpora: 0 programs. The hole is real today: four generic probes compile, fault in checked mode, and print an empty line in an unchecked build.
2. *The destructor window.* It is already ruled: spec §28.12 (v0.355, #7506) says the accesses of an instance's own disposal, "a child's read of the instance through a back-reference included", run on a live instance. The note's fault (probe k1a) is r40 behind that text. What 0212 adds is one static order test for a holder that borrows a sibling field, and the rule that a release which may run user code is a use. One flag-backstopped residual remains and is named: teardown code that reaches an owned field the cascade has already disposed. It exists without any stored borrow.
3. *Async.* No new rule is needed and §30.1's record ban is not imported. Every source of a borrow formed in an `async` body is frame-resident or is already carried by the frame, so a local instance across an `await` adds nothing to the settlement clause the frame already has. Two sentences extend what a frame and an awaited result carry. The frame-held clause is **not** retired: it is K4.3 trust, and through it a source can still be sent to another domain while an unsettled task holds it — a race no flag catches, today and after 0212 (§3.3).

**Headline measurements.**

| | result |
|---|---|
| Model, every sweep run (largest: 2,921,382 programs, 729,072 accepted): races through a stored borrow left by the revised rules, outside frames | **0** at every bound (MEASURED, `0212/model/out/`). The model has no frames; the frame-held clause stays trust (§3.3) |
| Model, 3 variables, 5 statements, both destructor variants, children linked in constructors: of 89,010 accepted programs that read a dead or foreign object through a stored borrow (22,860 of them a race), left accepted | **first draft: 9,522 races and 30,108 faults. Revised: 0 races and 1,350 faults** — each a slot kill (K5.6) or an announced kill of one source of several (K4.2), which the flag catches |
| Model: what the revision costs over the first draft, at its worst (every child of a borrow-bearing class, a child destructor, 4 statements) | 1,752 of 31,497 accepted programs rejected against 384; 1,206 of them clean where children are not linked, 984 of those by the taken-out rule. A refinement that would relieve part of them is recorded as open (§4.7) |
| Model: programs newly accepted or newly unsound, any rule set | 0 / 0 — a consistency check: every rule set here only tightens |
| Corpora (9,521 units, 5,750 accepted-tier programs): accepted-tier programs rejected / needing a `scoped` marker / unchanged | **5 / 12 / 114** of the 131 that store a borrow or build a borrow-bearing instance — the note's figures, reproduced; the rules 0212 adds, revision 2's included, reject **0** more (MEASURED, `0212/data/out/summary_0212.txt`) |
| Recorded K4.3 sites retired | 159 of 178 (the note's count, unchanged) |
| Corpora: `async` bodies with a borrow-bearing local across an `await` / generic bodies erasing an `owned` type-parameter value, instantiated at a borrow-bearing class | 0 of 1,064 `async` methods / 0 |
| Corpora, the positions revision 2 adds: `out owned` parameters, owning choices and inline arrays, `Result<…>` namings, `owned` parameters, moves of another object's field, comparer arguments — at a borrow-bearing class | 0 of 27 / 0 / 0 of 108 / 0 / 0 of 18 / 0 |

**The ramp** (§7). Stage 1: the property and the rules land as the *classification the trust-admission report prints*; no verdict changes. Stage 2: a default-build warning at exactly the sites stage 3 will reject, and the widening of `scoped` that gives each a rewrite. Stage 3, a Ratchet: the warnings become errors and the three clauses are deleted. Each stage is one revision; the gates between them are the owner's (Q9).

**What stays argued, not modelled** (§5.4). The model has one body and no calls. So the callee-side rule (R4), the `owned` parameter rule among it, the per-instantiation reading of the erasure requirement, interfaces, virtual dispatch, closures, `Shared<T>`, the comparer rule and frames rest on argument and probes. The model does check the mint's carried set — an `out owned` recipient's included —, the scope comparison, the freeze, the dominated use, owned erasure as a caller sees it, an owning choice that owns a holder, a child taken out of its owner, an owned argument that brings its sources to a receiver, and the destructor window.

**Owner decisions** are in §11: sixteen questions, each with a recommendation, and all sixteen ruled under the owner's delegation of 2026-11-10 (§11 *Rulings*): each recommendation adopted as written. The ones that shape the rest are Q3 (the mint reads the signature, with no marker on constructors), Q13 (a comparer at a sendable instantiation is static-sourced) and Q14 (a move-only value that owns a borrow-bearing instance is a carrier). Q2 (no per-field trust opt-out) is recorded provisionally and ruled at the stage-3 gate, on the stage-1 report's count. Q16 (a declared-reach refinement of the taken-out rule) is recommended for the stage-2 gate, on the same report.

---

## 1. The text today

> **Recorded trust, or nothing.** Where an admission's temporal guarantee can neither be statically compared (K2.3) nor flag-backstopped […] the rule must either reject statically or carry an **individually recorded trust clause**. The recorded clauses (field-held, collection-held, comparer-held and frame-held borrows versus domain transfer […]) are the model's honest limit
>
> — §28.3.1, K4 clause 3, v0.355 (QUOTED)

> **The residual.** The residual is a K4.3-recorded trust clause: whether the caller-side source actually outlives the object is the programmer's obligation, not a compile-time guarantee, with K4.1's backstop as the named safety net
>
> — §28.3.8, the field-store bullet, clause 3, v0.355 (QUOTED)

The note's §2 table states the gap and is MEASURED on r40: a record that carries a borrow is rejected in a class field, rejected when it outlives a source, and its source cannot be moved under it; the same three programs written with a class instance compile, and fault or race. The existing static spelling, a class-typed `scoped` parameter, admits only a `using var` holder, cannot be moved, and cannot be nested in an `owned` field. The corpora write 152 plain stored parameters against 19 class-typed `scoped` ones (the note, MEASURED).

---

## 2. The design

Eight rules. §8.2 gives them as normative text; this section states each in plain words. `Widget` is the kernel's example class (§28.3.1).

**R1 — the property.** A class is *borrow-bearing* when it declares or inherits (1) a **borrow field**: an instance field that is neither `owned` nor `scoped` and whose declared type is a class or interface type that is not runtime-managed; (2) an `owned` field, or a field of move-only value type, that owns a borrow-bearing instance; or (3) a **store member**: a member with a class- or interface-typed `scoped` parameter, or a settable property or indexer of such a type that is not `set(owned)`. An interface is borrow-bearing when it declares or inherits a store member. The standard library states the property for its own classes. The property is computed, never declared, and read per instantiation.

A **carrier type** is any type whose owned values can carry a stored borrow's sources: a borrow-bearing class or interface, or a move-only value type — an owning choice or an owning inline array — with an `owned` payload or slot of a carrier type. `Result<Account, E>` over a borrow-bearing `Account` is one.

**R2 — the source set.** A value of a carrier type carries one source set.
- *Mint.* `new C(…)`; a call whose declared return type is `owned` at a carrier type; a call's `out owned` recipient at a carrier type; and a choice case constructor with an `owned` payload. Each carries the receiver's contribution and each argument's, whether or not the callee stores it. The caller reads the signature, never the body.
- *Later store.* An argument at a class-typed `scoped` parameter, a property or indexer store of a reference, a store position of a standard collection, a field assignment `x.f := e` to a borrow field, and an `owned` argument that itself carries sources, each add sources to an instance. Nothing removes one. The store is checked as an assignment of the updated instance to the location that holds it.
- *Reads.* A borrow of the instance, or anything read out of it, carries the instance's binding and its set. So does an owned value **taken out** of it — by one of its members or by a replacing `move`, inside a member or outside one: the body that takes it cannot see whether the instance gave it a borrow of itself. A standard-library member may state that its result carries the set alone: taking an element out of a collection, `Shared<T>.Clone()`.
- *An empty set.* A value built from nothing that contributes carries no source and may be held anywhere, static storage included. Static storage itself contributes nothing: it is always live.

**R3 — the bound.** The value may not outlive a source by scope (K2.3). A fresh temporary may not be a source of a value that outlives the statement (K8.4). A local that holds it is a live local borrow of every source (K5.5), so a move or reassignment of a source is frozen while it is live; an explicit `Dispose()` of a source is not frozen (K4.2), and a use it dominates is §28.2's error. Where its release may run user code, the release is a use — at scope end, at an explicit disposal, at a drop-on-overwrite — so the local is live to it, and a later-stored source must be declared before it.

**R4 — the callee side.** A borrow sourced at a plain class- or interface-typed parameter is call-bounded. It may be stored into the object under construction, in a constructor; into an instance or collection that is purely local to the body; or, where the parameter is `scoped` or is a setter's `value`, into `this`. A borrow sourced at the receiver may be stored into storage `this` owns, in any member. An **`owned` parameter of a carrier type** carries sources its callee cannot see, so the body treats it as carrying an opaque source of the caller's: it may be stored into the object under construction or storage `this` owns, returned `owned`, handed on as an `owned` argument on the same terms, or consumed. Every other store is an error.

**R5 — owned erasure.** An owned value whose set is not empty may not reach an owning position whose declared type is not a carrier type. The owning positions are the places the consume table (§28.3.1, K3) lets an owned value change holder: an `owned` field or owning slot, an `owned` parameter, an `owned` return or a task result, an `out owned` slot, a choice payload, an owning iterator's `yield return`, and the operand of `throw`. In a generic body the conversion is a requirement read per instantiation (§3.1).

**R6 — teardown.** A borrow of `this` held by something `this` owns is bounded by the cascade (§28.4, §28.12): the holder is released inside the owner's disposal, before the owner's block. Two order tests apply where the holder's release may run user code: a holder that borrows a sibling `owned` field is declared after it, and a holder that borrows `this` is declared after every `owned` field of its class (§3.2).

**R7 — frames.** A frame carries what its arguments and captures contribute, an instance's sources included; the result of awaiting a task, and an element an owning iterator yields, carry the frame's carried set (§3.3).

**R8 — the report.** The trust-admission report prints the classification these rules give every store (§7).

*Example* (the verdicts are 0212's; today every line compiles):

```bezel
class Tag
  private _owner: Widget;                  // a borrow field: `Tag` is a borrow-bearing class (R1)

  public constructor(owner: Widget)
    _owner := owner;                       // admitted: a constructor stores into the object it builds (R4)
  end constructor;

  public method Retarget(scoped w: Widget)
    _owner := w;                           // admitted: a later store is announced by `scoped` (R4)
  end Retarget;

  public method Name(): string => _owner.Name;
end Tag;

public static method Take(owned w: Widget) => w.Dispose();

var a := new Widget("a");
using var t := new Tag(a);                 // `t` carries `a` (R2, a mint)
Take(move a);                              // error (K5.1 through K5.5): `t` is a live instance that carries `a`
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

**Kernel classification** (spec-change-process §9). R1 is a derived type property. R2–R3 derive from K2.3 + K2.4 + K5.5 + K8.4, stated once as a new K2 clause 5. R4 derives from K2.4 (a plain signature announces no retention), the ground §28.14 already gives for a callback. R5 derives from K2.3 + K2.4 over K3's consume table, the ground of §9.7's borrow-erasing bullet. R6 derives from K9 and §28.4. No axiom is added. Axiom A11 is *extended*: a class-typed `scoped` argument is admitted against any traced location, not only a `using var` local or `this`.

**Body-locality classification** (proposal 0105 §1). R1: local — it reads declarations, and crosses a module boundary in the artifact interface as trivial-dispose and sendable do. R2, R3, R4, R6: local — a caller reads a type and a signature; a callee is checked alone. R5 in a generic body: **carve-out** (generic instantiation), the clause §9.7's generic-body paragraph already uses.

---

## 3. The three blocking items

### 3.1 Generic erasure

**The hole** (the note, §3 A-5). `static method Hide<T>(owned x: T): owned IShow where T: IShow => x;` instantiated at a borrow-bearing class takes an owned instance in and hands an owned interface value out. The caller's check over the signature sees nothing to reject, and §9.7 says a generic body is not re-checked.

**It is admitted today, on trust** (MEASURED, r40):

| probe | shape | checked | unchecked |
|---|---|---|---|
| g01 | `Hide<T>(owned x: T): owned IShow => x` at `Account` | `DisposedAccessException` | exit 0, prints an empty line |
| g02 | the same through a forwarding generic `Outer<T>` | fault | empty line |
| g03 | a generic class storing `owned T` in an `owned IShow` field | fault | empty line |
| g04 | a dispatched generic method `ISink.Take<T>(owned x: T)` whose implementation stores at `IShow` | fault | empty line |
| g05 | `OwningArray<IShow>.Add(new Account(c))` — erasure at a caller, no generic body | fault | empty line |
| g06 | `throw new CtxError(c)` past `c`'s scope | fault in the handler | empty line |
| c01 (critic) | `Hide<IStore>(move st)` at a borrow-bearing *interface* `IStore`, erased to `IShow` | fault | empty line |
| c02 (critic) | `Make(c, out var a)` over `out owned a: Account`, then `keep := move a` past `c`'s scope | fault | empty line |
| c04 (critic) | `static Give(owned a: Account, b: Box)` whose body calls `b.Put(move a)` | fault | empty line |
| c05 (critic) | `Slot.Full(new Account(c))`, an owning choice, returned past `c`'s scope | fault | empty line |
| c11 | `throw new WrapError(e)` from the catch arm that borrowed `e`; the outer handler reads the stored cause | **no fault**: prints an empty line, exit 0 | the same |

The unchecked runs are the worst outcome: a read of freed memory that returns normally. c11 does the same in a checked build. c01, c02, c04 and c05 are routes the first draft's list of owning positions missed; the critic's c03, a replacing `move` from outside a member, is argued, since r40 does not implement that form.

**The ruling: a structural, per-instantiation requirement, computed at the generic's single check.**

1. *At a concrete site* the rule reads the value's set: an owned value whose set is not empty may not reach an owning position whose declared type is not a carrier type (R5). g05, g06 and c11 are this case.
2. *In a generic body* an owning conversion of a value whose type mentions a type parameter, to a target type, is a **requirement of the body's use of that parameter**: at every instantiation at which the value's type, after substitution, is a carrier type — a borrow-bearing class **or interface**, or a move-only value type that owns one — the target type, after substitution, must be a carrier type too. An instantiation that fails is rejected where it is named, with the diagnostic naming the conversion. The set is not consulted, because a body checked once cannot know it: the requirement is read off types. (The first draft said "class" here, and c01 erased through an interface.)
3. A requirement reaches a declaration through a substitution as §3.6 rule 2 already says (`Outer<T>` forming `Hide<T>`, probe g02).
4. A **dispatched generic method** has no instantiation site for the bodies dispatch reaches (§14.2), so the conversion is an error at the body unless the method's constraints establish that the parameter is not a carrier type. `where T: sendable` does: no carrier type is sendable (§4.5). `where T: value` does not by itself — an owning choice is a value — so it needs `sendable` beside it, or a body that keeps the value at a type that mentions the parameter (probe g04 is the unconstrained case).
5. A **standard-library** generic body contributes no requirement (§3.6 rule 2), so the library states which of its types are carriers (§8.8). Where a library member takes `owned T` and its type is not a carrier at that instantiation, an argument whose set is **not empty** is rejected at the call; an argument that carries nothing is admitted, so `Result<Conn, E>.Ok(conn)` and every other library use of a class with an empty set are untouched.

**Why this rule and not the alternatives.**

- *An instantiation-site rejection with no body fact* — reject every generic instantiated at a borrow-bearing class unless the generic opts in (Swift's `~Escapable`, C#'s `allows ref struct`). It needs a new constraint form, and it rejects `Box<Account>`, which keeps its `T` and is sound (probe g08). Declined (§8.10, row D4).
- *A constraint the generic's author writes.* Bezel has no "holds no borrows" constraint, and a requirement the compiler can read from the body is what §3.6 rule 2 exists for: `Keep<Pair>` is rejected today with no constraint written.

**Soundness (argued): the list of owning positions is the consume table.** A carrier value can lose its set only where it changes holder. Inside one body the set is flow-tracked on bindings, whatever their declared type, so a local declared `IShow` loses nothing. An owned value changes holder only at an owned-consuming position, and K3's consume table is "its single definition site" for those (QUOTED, §28.3.1). The first draft enumerated the positions by hand and missed four. This revision reads the table row by row; every row has a rule.

| consume-table row | where the value goes | the rule that keeps or refuses the set |
|---|---|---|
| K3.1 store into an `owned` field or owning slot, `set(owned)` included | the object that owns the slot | R5 at the slot's declared type; then the object carries it (R2, later store) and R4 says which objects a body may store into. A static field takes an empty set only |
| K3.2 `return` under an `owned` type; a task result | the caller; the task | R5 at the declared return or result type; the caller's result is a mint (R2), an awaited result carries the task's set (R7) |
| K3.3 an `owned` argument | the callee | R5 at the parameter's declared type; in the callee the parameter carries an opaque caller source (R4), so it reaches only `this`, the result, or its end. c04 |
| K3.4 a marked binding transfer; the owning-match scrutinee; an arm transfer | a local of the same body | flow: the set travels with the `move`; an owning match's payload bindings carry the matched value's set; arms join by union |
| K3.12 a store of a move-only value | as the row its destination falls under | a move-only value that owns an instance is a carrier and is read by that row. c05 |
| K3.5, K3.6 `await` of an owned task; chaining | the awaiting body; the outer task | the result carries the task's carried set (R7) |
| K3.7 `yield return` in an owning iterator | the consumer | R5 at the declared element type; the element carries the frame's set (R7) |
| K3.8 `throw` | an unknown handler | refused outright for a non-empty set. g06, c11 |
| K3.9, K3.11 explicit disposal; displacement | nowhere | the value ends; a release that may run user code is a use (R3) |
| an `out owned` slot (§22.3: ownership reaches the caller on normal return) | the caller's recipient | the callee's assignment is held to the return rule; the recipient is a mint (R2). c02 |
| the replacing `move` (K9.1): an owned read of a slot | a local of the taking body | the result carries the binding of the instance it left, and that instance's set (R2). c03 |

A *borrowing* position cannot lose a set: a borrow passed at `Object` or an interface contributes its binding and the instance's sources at the caller, read from the argument and not from the parameter's type (R2), and the callee may keep it only where a signature says so (R4). Probe g07 is that case: a `Keeper` built over a borrow at an interface type carries the instance and its source.

The three routes the note feared, each closed by a rule that exists:

- *Through a second generic*: the requirement propagates through the substitution (§3.6 rule 2; g02).
- *Through a standard-library body*: the library's entry states which types are carriers; where it is silent an argument that carries sources is rejected at the caller, where the types are concrete (g05).
- *Through an `as` at a type parameter*: an `as` yields a borrow, never an owned value, so it is not an owning position. `x as T` over a borrow-bearing instance yields a borrow that carries the instance's binding and set.

**Cheap.** The single check of a generic declaration records one pair of types per owning conversion whose source type mentions a parameter. Each instantiation evaluates the pairs under its substitution. That is the cost model of §9.7's bullets and of proposal 0159's `as T` requirement.

**What it rejects** (MEASURED, `0212/data/out/summary_0212.txt`). The corpora hold 161 `owned` parameters typed at a type parameter in members of generic declarations, and for the positions this revision adds: 27 `out owned` parameters, none at a borrow-bearing class; no owning choice and no owning inline array that owns one; 108 namings of `Result<…>`, none at a borrow-bearing class; no `owned` parameter of a borrow-bearing class at all; 18 moves of another object's field, none of a borrow-bearing type. Of the 161, 47 are kept at a type that mentions the parameter, 35 are moved into a local or forwarded, 61 are only used or unused, and 18 are erased — every one a `throw x` under a `where T: Exception` bound. None is in a dispatched generic method. No erasing declaration is instantiated at a borrow-bearing class; the whole corpus names a borrow-bearing class as a type argument five times (`Array<Row>`, three `Channel<…>` that sendability already rejects, one user generic that keeps its parameter). **Programs rejected: 0.** The scanner is syntactic and reads one unit at a time; a forwarded value is not followed into its callee.

**In the model.** The model has no generics. It has the caller's view of one: `erase y x` converts a holder to an owned value whose type announces nothing, and `euse y` reads through it. With R5 off (the note's candidate) the conversion is admitted and the model finds the faults and races; with R5 on they are rejected (§5.2). That checks R5 as a rule over values; the per-instantiation reading is argued. Three of the critic's routes are now in the model too — an `out owned` recipient, an owning choice that wraps a holder, and a child linked to its owner in a constructor and then taken out — and the model measures what the first draft left open through them (§5.2).

### 3.2 The destructor window

**The question** (the note, S2). A child holds a borrow of the object that owns it. §28.4 runs the owner's destructor body, then its owned fields in reverse declaration order, then the base. So the child's destructor runs while its owner is being torn down. The note's probe k1a faults there in checked mode on r40.

**The spec already answers it.**

> An instance is **logically disposed** — the state §28.2's checked-mode test faults on — once its logical disposal has completed; until then the disposal's own accesses (a destructor body's, the `DisposeAsync` body's and its chained `base.DisposeAsync()`'s, and those of the destructors that the owned-field cascade (§28.4) runs for the instance's owned fields, a child's read of the instance through a back-reference included) run on a live instance. An owned field that the cascade has already disposed is itself logically disposed, so a member access through it from a destructor the cascade runs later faults in checked mode.
>
> — §28.12, v0.355 (QUOTED; the changelog names #7506)

The rule-corpus case `s28_12_0014` states it for a destructor body's own method call (QUOTED). The note's base was this revision; its S2 read r40's behaviour as the rule. **r40 does not implement the paragraph** (MEASURED):

| probe | shape | checked (r40) | unchecked (r40) | by §28.12 |
|---|---|---|---|---|
| d01 | a destructor body reads its own field and property, then calls its own instance method | field and property print; the method call faults | prints all four lines | no fault |
| d02 | the owner's destructor body disposes its child early; the child's destructor reads the owner | faults | clean | no fault |
| d03 | k1a with a field read: a child's destructor reads a public field of its owner in the cascade | faults | clean | no fault |
| d11 | a tree: each node's destructor reads its parent in the parent's cascade | faults at the first child | clean | no fault |
| d04 | a child declared first reads, through its owner, a sibling declared after it | faults, after the sibling's destructor ran | **segfault** | fault (the sibling's own flag) |
| d08 | no stored borrow at all: a base destructor calls a virtual member whose override reads a derived owned field | faults | **segfault** | fault |
| d05, d12 | a back-pointing child with no destructor; a tree whose destructors do not read the parent | clean | clean | clean |

So r40 sets the flag when `Dispose()` begins, and a method call on `this` inside a destructor body faults. That is spec-ahead-of-implementation under #7506, not a design question (§10).

**The ruling: the cascade order makes the self-sourced borrow sound, and 0212 adds three static tests.**

1. *The stored borrow itself never dangles.* A holder stored in storage its source owns is released inside the source's disposal, and the source's block is reclaimed only after the disposal reaches the root of the hierarchy (§28.4, §28.2's compiler-inserted free). By §28.12 a read of the source in that window is a read of a live instance. So the invariant's self-source case holds through teardown.
2. *A holder moved out of its owner carries the owner.* It leaves through an `owned` return of an instance member, or through a replacing `move` — written in a member or outside one — and R2 gives the result the binding of the instance it left and that instance's set. From there it is an ordinary instance with an outside source. (The first draft's normative text gave a replacing `move` "the instance's set" only, which is empty for an owner whose child borrows it; the critic's c03 is that case, and §8.2 now says what this item always said.)
3. *New — the sibling order test.* A holder in an `owned` field `f` that carries the value of a sibling `owned` field `g` must have `g` declared before `f` where the holder's release may run user code: the cascade then releases `f` first. Probe d06 (right order) is clean in both modes; d07 (wrong order) faults in checked mode and segfaults unchecked, and is an error under 0212.
4. *New — a holder that borrows `this` is declared last.* A holder in an `owned` field `f` that carries `this` can reach every field of its owner, so where its release may run user code every `owned` field its owner's class declares must be declared before `f`: the cascade then releases the holder while those fields are live. This is the sibling test with `this` read as all of the siblings. It makes probe d04 — a child declared first that reads a later sibling through its owner — a compile error at the store, and so closes the stored-borrow route to the residual below inside one class. The critic offered it as optional; it is adopted because it is the same test and costs nothing measured (0 constructions carry `this` in the corpora). Q15 lets the owner drop it.
5. *New — a release that may run user code is a use.* For a local instance this is the note's A-3. The key is stated so that it is sound for a subclass: the release is taken to run user code unless the instance's class is trivial-dispose **at its produced type**, where a `new` expression fixes it, or its declared class is `sealed` and trivial-dispose. (§28.9 reads the declared type where no producer is known; that is a lint's key and would be unsound here, since `var b: Base := new Derived()` may run a destructor.) Probe d09 (a flush-on-dispose holder declared after its source) is clean; d10 (declared before it) faults and is an error under 0212.

**What 0212 does not do: forbid a destructor from reading a stored borrow.** Three static rules were considered and declined.

- *No read of a borrow field in a destructor body.* It retires flush-on-dispose over a borrowed sink (d09), which is sound, and it is not complete: a destructor that calls its own method reads the field indirectly.
- *A self-sourced holder must be trivial-dispose.* It rejects every tree with parent pointers, because `OwningArray<T>` is never trivial-dispose (§28.9, QUOTED), and it does not bound a subclass's destructor.
- *Whole-subtree deferred reclamation*, so that no read in the window can dangle in an unchecked build. It is a runtime mechanism of the kind §28.9 says does not exist.

**The residual, named.** One read in the window is not sound, and §28.12's last quoted sentence already names it: teardown code that reaches an owned field the cascade has already disposed. With test 4 a stored borrow reaches it only across a class boundary: a holder declared in a base class is released after the derived class's `owned` fields (§28.4 runs subclass to base), and an open base cannot order itself against fields it cannot see. d08 reaches the same read with no stored borrow at all. It is not trust: the disposed field's own flag is set, the reader and the object are in one domain by construction (an owner and what it owns move together, and a borrow-bearing class is not sendable), so checked mode faults deterministically. It is K5.6's posture: a slot kill is not frozen, and the backstop is real there. An unchecked build is undefined there, as for every K5.6 residual today.

**In the model** (§5.2). The model has a child destructor that reads its back-pointer, the two flag timings, and a variant in which children are linked to their owners in constructors. Under §28.12's timing the revised rules leave no fault and no race through a child's back-pointer at any bound. Under r40's timing 4,938 of 31,497 accepted programs fault on every run at 3 variables and 4 statements: every program whose child reads its owner in the cascade.

### 3.3 Async

**The question** (the note, S4). §30.1 bans a local of a value type that holds borrows across a suspension unless every source is frame-resident. A class instance built over a caller-sourced parameter and held across an `await` is the same borrow one hop away. Import the ban, or let the frame-held clause cover it?

**The ruling: §30.1's frame and settlement rules cover it; the record ban is not imported; two sentences are added.**

*Why the frame already covers it.* In an `async` body every borrow has a source of one of two kinds. It is frame-resident — an owned local, a `using var`, an `owned` parameter — and then the frame owns it and the ordinary local rules bound it inside the one body. Or it is caller-side — a plain parameter, `this`, a capture — and then it is one of the frame's own carried sources: K6 says a task carries "the constructing call's receiver, the sources its plain arguments contribute […] and the capture-set contributions" (QUOTED). An instance built in the body can only store borrows formed in the body. So its set is a subset of the frame-resident sources and the frame's carried set. It gives the frame nothing to hold that the frame does not hold already, and the caller's obligation — "every borrow source reaching an async call must outlive the settlement of the returned task" (§30.1, QUOTED) — is unchanged in extent.

*Why the record ban does not carry over.* The ban's ground is §9.7's posture that a value type that holds borrows is stored past its bound at no position. A class-typed borrow has never had that posture in a frame: `var b := c; await …; b.M();` over a plain parameter `c` is admitted today on the frame-held clause (probe a01, MEASURED: clean when awaited). A holder across the `await` is the same admission (a02: clean when awaited; a03: faults when the caller disposes the source before settlement — and so does the plain alias, a08). Importing the ban would reject the routine helper built over a parameter and leave the alias next to it admitted.

*What is added.*

1. **A frame carries an instance's sources.** As an argument or a capture, an instance of a borrow-bearing class contributes its binding's source and the sources it carries; passed `owned`, the sources it carries (R2's contribution rule, §8.4). Probes a05 (a plain parameter), a06 (an `async` lambda's capture) and a07 (an `owned` parameter) are the cases.
2. **An awaited result carries the task's carried set.** Where a task's result type is a carrier type — `OwningTask<Account>`, or a `Task<Account>` whose awaiters borrow — the value an `await` yields carries what the task carries; an element an owning iteration yields carries its frame's set the same way. Probe a04 is the case: `using var acct := await Open(c); c.Dispose(); acct.OwnerName();` compiles today and faults; under 0212 the use is §28.2's error.

*What stays, stated plainly.* "Covered by §30.1" means covered by the **frame-held trust clause**, not by a static rule. A task is not traced by K5 (K6.4). So a caller may kill a source while a frame that holds it is unsettled: a disposal is reported in report mode and backstopped by the flag (a03, a08); a **transfer to another domain** is reported and has no backstop — the object stays alive there, no flag is set, and the frame's later read is a data race. That is true today of a plain parameter the frame holds, and it stays true under 0212 of an instance in the frame that carries it: an unawaited task that captured a holder can have the holder's source sent away. 0212 neither creates nor closes this; every "no race left" figure in this proposal is for stored borrows outside frames, and the model has no frames. A strict mode is where the clause ends (the note's candidate E), not this proposal.

**Evidence.** Probes a01–a08 (MEASURED, r40). The corpora price neither reading: of 1,064 `async` methods, none has a parameter or a task result of borrow-bearing class type, and none holds a borrow-bearing local across an `await` (MEASURED). The model has no frames, so this ruling is argued.

---

## 4. The other open questions

**4.1 Lifting the interface restriction — derived, not declared.** R5 rejects an owned borrow-bearing instance at an interface that announces nothing (the note's p14). The lift is R1's clause 3: an interface that declares a store member — a `scoped` class-typed parameter, or a settable reference-typed property — is borrow-bearing, and every owning position typed at it carries a set. This is §3.6 rule 1's posture, and it is also forced: a later store through such an interface adds sources to its receiver, so an owned value of that interface type must be able to carry them. A *declared* borrow-bearing interface, for one whose implementations store only in constructors, is declined with a bar (§8.10, row D3): the corpora hold no owned upcast of a borrow-bearing instance with a non-empty set (MEASURED, 0 of 312 constructions), of their 765 interfaces none declares a class-typed `scoped` parameter and three, all in gtest files, declare a reference-typed property that may be settable (MEASURED), and the rewrite — return the concrete class — exists.

**4.2 A store that relates two parameters — no new spelling; the storing object is the receiver.** `static method Store(a: Array<Item>, it: Item)` that calls `a.Add(it)` stores one parameter into an object another parameter holds: 1 accepted-tier site and 8 distinct gtest sites (the note, MEASURED). R4 rejects it, because the body cannot see the binding that holds `a`. The spelling exists: make the storing object the receiver — an instance method or an extension method — and declare the stored parameter `scoped`. The caller then checks the store against the receiver's binding. MEASURED on r40: for a user class both forms compile and run today (probes q01, q02). Where the storing object is a *standard collection*, the extension form needs `scoped` at a type parameter, which §22.3 does not admit today (probe q06: rejected); 0212 admits it (§8.7), and until then the rewrite is to store at the caller. A marker that names another parameter as the bounding object (`scoped(a) it`) is declined (§8.10, row D2): it is the first step toward lifetime parameters, and the receiver form or the caller-side store expresses every measured site.

**4.3 A per-field trust opt-out — recommended: none.** The note asked for this decision up front. The case for an opt-out is migration and the shapes with no static form. The case against, which this proposal takes:

- It keeps the field-held clause alive under another name, and with it the one hazard checked mode cannot catch: a stored borrow of an object later sent to another domain. "Retire the clause" becomes "rename the clause".
- The measured need is small and each shape has a rewrite. After the receiver-form rewrite of §4.2, the stores with no static form across every corpus are two methods that keep an `Object` parameter (retype the parameter) and the holder-before-source registry (§4.4).
- The ramp is the safety valve. Stages 1 and 2 reject nothing, and stage 1's report counts the sites with no static form in real programs. That count is the evidence an opt-out would need; it does not exist yet.

So `DECLINED.md`'s callee-side row is reversed for members that are not constructors, with no escape hatch, and the opt-out is recorded as declined-at-proposal with that count as its reopening bar (§8.10, row D1). The critic recommended recording this **provisionally** and ruling it at the stage-3 gate, since the count that decides it is stage 1's product; that is adopted. Stages 1 and 2 are the same either way. If the owner then prefers an opt-out, Q2 sketches it; nothing else in the proposal changes.

**4.4 Registries and weak references — what users write instead.** A long-lived holder declared before the things it refers to cannot hold tracked borrows of them (the note's p17, p30). Four spellings exist, none new:

1. *Reorder*: declare the source first (the note's p29).
2. *Own the items*: an owning collection, with `OwningSlotMap<T>` and `SlotHandle` keys where callers need a stable name (stdlib §4.21; marked spec-ahead-of-implementation, #4604, so not probed).
3. *Observe them*: `OwningArray<Weak<Item>>` over `Shared<Item>`, with `TryUpgrade()` (the note's p28: runs clean on r40 in both modes). An `owned` field of type `Weak<Item>` does not make its class borrow-bearing, `Item` not being one.
4. *Events* for callbacks (stdlib §6.5), which §28.14 already bounds.

**Observer self-registration has no static form, and is named here so nobody looks for one.** A constructor that hands `this` to a longer-lived object — `constructor(bus: Bus) bus.Subscribe(this);`, with or without `scoped` — stores a borrow into an object the constructor does not hold, and that object is by design declared before the observer. R4 rejects it. Today it compiles with no report line, faults in checked mode and segfaults unchecked when the bus notifies after the observer died (probe c09, MEASURED). What users write instead is the event: the bus exposes an `IEvent<T>`, and the observer keeps the `Subscription` that `Subscribe` returns in an `owned` field, so its own disposal severs the registration (stdlib §6.5; §28.14's bounding form 3; probe c10: compiles and runs clean on r40). Where the bus must hold objects and not callbacks, it holds `Weak<T>` handles (item 3). The corpora hold no constructor of this shape (MEASURED, 0).

A registry held in a **static** takes only static-sourced borrows: §28.3.8's *Static stores* already rejects the direct store, and under 0212 a store into a collection a static holds is checked against that location (probe q03: admitted today, faults). The corpora under-represent this shape by construction; stage 1 is what measures it. A weak reference over an ordinary owned object, the prior lane's design, is a separate proposal (Q10, ruled: written after stage 1's data; recorded as a `deferred` row of `DECLINED.md`).

**4.5 Collections that retain a comparer, and sendability — a comparer at a sendable instantiation is static-sourced.** The first draft made `Dictionary<K, V>`, `OwningDictionary<K, V>`, `Set<T>` and `OwningSet<T>` borrow-bearing at every instantiation, because their `comparer` constructors retain a borrow, and said a borrow-bearing class is not sendable. Together those made `Dictionary<string, int>`, `OwningDictionary<string, Session>` and `Locked<Dictionary<string, int>>` — stdlib §4's registry idiom — stop being sendable. That was a regression, and §8.8's claim that existing rules "already say so" was wrong for these types.

The fix keeps one invariant: **a sendable type is never a carrier type**, so a value that may cross a domain never carries a source, and `where T: sendable` keeps meaning what R5 needs. These four collections are therefore carriers exactly where the other collections are — by their element types. At an instantiation that is *not* a carrier, a `comparer` constructor takes only a comparer that contributes no source: a static-storage-sourced borrow, such as a `static readonly` instance. At a carrier instantiation any comparer is admitted and the collection carries it.

*The alternative, declined:* keep the type a carrier at every instantiation and reject, at each domain transfer, a value whose set is not empty. It admits a local or field comparer at `Dictionary<string, int>`. Its price is a value-level check at every spawn and send, a sendable type whose values may carry sources — so `where T: sendable` no longer answers R5's question for a dispatched generic method — and two levels to explain (the type may carry; this value does not). Comparers are stateless objects in practice, and a stateless object is what a `static readonly` field is for.

*Measured cost.* No corpus program builds one of these collections with a comparer: the five constructions with a non-capacity argument are expected-reject cases that pass a string or a literal (MEASURED). r40 does not implement the comparer constructors at all (probe c06: "not yet implemented"), so no program can depend on a retained comparer today. Q13.

*How narrow the rule is* (critique round 2, note 1). At an instantiation that is not a carrier, the only comparer admitted is one held in static storage. That has three costs, stated here so the rule is chosen with them in view.

- **The spelling this rule teaches is a warned one.** A `static readonly` field of a user class draws §22.12 rule 4's declaration-site warning — the instance is reachable from every domain — unless its class is in that rule's warning-suppression set, and a user comparer is not (probes c07 and c14: the warning, MEASURED). The standard library declares no comparer instances of its own.
- **A comparer cannot be forwarded.** `class CaseIndex` with `constructor(cmp: IEqualityComparer<string>) _d := new Dictionary<string, int>(cmp);` is an error at the construction: `cmp` is a plain parameter, not static storage, whatever its callers pass (probe c14). The same body in a generic `Index<K>` is rejected where `Index<string>` is named, and admitted at `Index<Item>` for a class `Item`, whose dictionary is a carrier.
- **A comparer with per-instance state has no spelling** at these instantiations.

*What users write instead: put the rule on the key.* Wrap the key in a record that declares its own `Equals` and `GetHashCode` — `record FoldedKey(Value: string)` — and build `Dictionary<FoldedKey, int>` with no comparer at all. This is the rewrite `SortedDictionary<K, V>` already requires for an ordering: "a type that needs a different order wraps its key in a record with the desired `CompareTo`" (stdlib §4.17, QUOTED). It draws no warning; the dictionary is forwarded and stored as any other is; and it stays sendable, the key being a sendable value (§30.14, QUOTED: "a **record** is sendable iff it is not borrow-bearing and every field's type is sendable"). MEASURED on r40 (probe c13): it compiles with no warning and no report line and finds `"ANN"` under the key `"Ann"` in both modes. Guide 05 teaches it first (§8.11). The comparer constructors keep their use where the collection is a carrier anyway — class keys or class values — and there the comparer may be a local, a forwarded parameter or a stateful object, and the collection carries it.

Two widenings are left to the owner and are not proposed here (Q13). Standard-library comparer instances that declare the immutable fact would remove the warning; they would not restore forwarding. The comparer *type parameter* that rationale §12.13 records as deferred (*The comparer*: "`Dictionary<K, V, C>`, or a constructor constrained `where C: IEqualityComparer<K>, sendable`", QUOTED) would restore forwarding with a static bound, at the cost that record names: an API redesign across four types.

**4.6 `Result<T, E>` and the other move-only carriers.** `Result<T, E>` is an owning choice with `Ok(owned value: T)` (stdlib §2, QUOTED). The first draft had no rule for a value that *owns* a borrow-bearing instance without being one, so the critic's c05 lost its source through an owning choice, and the draft's stdlib catch-all, read by its letter, rejected `Result<Conn, E>.Ok(conn)` for any class `Conn` it did not list — Guide 02's main idiom. Both are fixed by one rule: a move-only value type with an `owned` payload or slot of a carrier type is a carrier type; its case constructor is a mint and carries what its `owned` arguments carry; an owning match hands the payload on with the value's set. `Result<Conn, E>` over an ordinary `Conn` carries nothing and is exactly what it was. `Result<Account, E>` over a borrow-bearing `Account` carries what the account carries, so a factory may return it and its caller is checked as for the account itself. The value-payload combinators (`Map`, `AndThen`, `UnwrapOr`) are not admitted at an owning instantiation today (stdlib §2, QUOTED), so they add no route. MEASURED: the corpora name `Result<…>` 108 times, never at a borrow-bearing class; they declare no owning choice and no owning inline array that owns one. Q14.

**4.7 The taken-out rule over-approximates — a refinement, recorded as open.** (Critique round 2, note 2.) R2 gives an owned value taken out of an instance — a member's `owned` result, or a replacing `move` — the binding that holds the instance, whether or not the instance ever gave the value a borrow of itself. So a builder's product is tied to its builder:

```bezel
public static method Make(c: Customer): owned Report
  var b := new Builder(c);
  var r := b.Build();        // `Build` is `=> new Report(_c)`: the report never touches the builder
  return r;                  // error under 0212: `r` carries the local `b`
end Make;
```

Four facts place this rejection.

- *A value that is not of a carrier type carries nothing.* A product that stores no borrow is untouched by the rule.
- *The model prices the rule at its worst.* Its child class always declares a back-pointer to its owner's class. Of the 1,206 clean rejections at the bound §5.2 item 6 cites, 984 are this rule's, and 954 of those are in programs that never link a child (MEASURED, `out/takeout_v3l4_qdtor.txt`).
- *What r40 does today is not a guide.* It rejects the direct form `return b.Build();` with "borrow returned from `b` would outlive its source" (the critic's probe e01, MEASURED) — and rejects it equally when `Build` returns a class that stores no borrow at all (probe e04), and accepts the bound form above, which runs clean (probe e03). So today's verdict follows the shape of the `return`, not what the product holds. Under 0212 the bound and the direct form are both rejected for a borrow-bearing product, and both admitted for a product that is not one. Whether v0.355 requires r40's verdict on e04 was not established here; it is reported to the coordinator, not worked.
- *The rewrite exists.* The product is built where its sources are in hand (`return new Report(c);`), or the builder belongs to the caller: `Make(b: Builder): owned Report => b.Build()` returns a value that carries a parameter, which the caller re-establishes.

**The refinement the critic proposed** reads declarations only. The value carries the holder's binding only where its type can reach — through borrow fields, followed transitively through `owned` fields — a type that the holder's class, or the type of something the holder owns, converts to. A `Report` whose one borrow field is a `Customer` cannot hold a borrow of a `Builder`; it would carry the builder's set (the customer) and not the builder.

**As worded it is not sound, and it is not adopted.** A class is open to inheritance unless it is `sealed`, so a declared type does not list the borrow fields its values can have:

```bezel
public class Spy: Report                 // `Report` declares one borrow field, a `Customer`
  private _b: Builder;                   // the subclass adds a borrow of the builder
  public constructor(c: Customer, b: Builder)
    base(c);
    _b := b;
  end constructor;
  public override method Show(): string => _b.Label;
end Spy;

// in Builder:
public method Build(): owned Report => new Spy(_c, this);
```

`Build`'s body is admitted under 0212 — it returns a value that carries its receiver, which its callers re-establish — and its declared return type shows no field that could hold a `Builder`. A caller that read only the declared type would let `b` be moved away under a live result. Probe e02 is this program with `Take(move b)` before the result is used: it compiles on r40, faults in checked mode and prints an empty line in an unchecked build (MEASURED). The same gap opens on the holder's side, where a subclass of the holder may own fields its static type does not show, and at an interface-typed borrow field, which a class written later may implement.

**A closed form is sound in the model.** Where the value's class and every class it owns are `sealed`, the holder's class and what it owns are too, and no field reached is of function type, the declarations are complete and the conversion test is exact (HYPOTHESIS: argued from single inheritance; no subclass can add a field). The model has no subclasses, so the closed form is exactly what it can check. It was extended with a member that returns a holder over the receiver's own source (`kdup`, the e01 shape) and one that returns a holder over the receiver (`kself`, the e02 shape). At 4 variables and 4 statements with a holder destructor (MEASURED, `out/reach_v4l4_kdtor.txt`):

| rule set | rejected, of 167,240 accepted | of those, clean | left: races; certain faults |
|---|---|---|---|
| 0212 revised | 17,940 | 4,224 | 0; 360 |
| the refinement, closed form: the binding is dropped for `kdup` only | 17,844 | 4,128 | 0; 360 |
| the refinement read off the declared type of an open class: dropped for `kself` too | 17,748 | 4,128 | 0; 456 |

The closed form removes 96 clean rejections and admits nothing that faults. The declared-only reading admits 96 more programs and every one is a certain fault: a builder moved away under a product that borrows it (`new a / newk b a / kself c b / move d b`). With both destructors the figures are 21,288, 21,192 and 21,096 rejected, and 336, 336 and 432 faults left (`out/reach_v4l4_kq.txt`); with no destructor the three rule sets agree at this length (`out/reach_v4l4.txt`), four statements leaving no room to use the product after the builder is moved, and at 3 variables they agree because the move needs a fourth binding (`out/reach_v3l4_kdtor.txt`). Five self-tests pin the shapes under the three readings, the five-statement one among them. A sweep at 5 statements with 4 variables was not run: it outgrew this machine's memory.

**Why it is recorded and not proposed.**

1. The sound form needs `sealed` on the product, on the builder, and on what each owns. Neither class in the critic's example is sealed, so the refinement would not relieve the case that motivated it without an edit to both.
2. It is a second derived property — the types a class can hold a borrow *of* — that has to cross module boundaries in the artifact interface beside the first, and be read per instantiation.
3. There is no count to weigh it against. No corpus program meets the taken-out rule (0 moves of a borrow-bearing field from outside its class; the 81 returned constructions are admitted, §6). Stage 1's report is what counts the sites in real programs (§10).
4. The rewrite above exists for every shape found.

It is recorded in §8.10 (row D8) with that count as its reopening bar, and asked as Q16.

---

## 5. Soundness

### 5.1 The invariant and the argument

**Invariant.** For every live object *o* of a borrow-bearing class, each object reached through a borrow field of *o* is static; or is an object that owns *o*, directly or through `owned` fields and move-only values, or a field value of one; or is the value of a source in *set(o)* as read at the location that holds *o* — where, for an *o* owned by a move-only value, that location is the one that holds the value.

**Why it is preserved.** *set(o)* changes only at a mint or a later store (R2). A mint is read from the signature and so over-approximates what the body stores (R4 bounds the body: a plain parameter reaches only the object under construction or a local instance that leaves through a mint; an `owned` parameter that carries sources reaches only `this`, the result, or its end). A later store is announced by a signature or is a field assignment, and is checked against the location that holds *o*. A source in the set cannot end before the instance by scope (K2.3, K8.4), cannot be killed silently while the instance is live (K5.5 over K5.1 and K5.2, a transfer to another domain included), and the set cannot be dropped where the value changes holder: §3.1's table reads every row of the consume table. The self-source case holds through teardown by §3.2, and a value taken out of its owner carries the owner.

**What remains outside it**, each flag-backstopped and none a trust clause:

1. an announced kill (K4.2) that does not dominate the use — on some paths only, or of one source of several;
2. a kill of a slot's current value (K5.6), the teardown case of §3.2 included.

The frame-held clause is outside the invariant and is not flag-backstopped against a domain transfer: it is the recorded trust that stays (§3.3).

### 5.2 Model results (MEASURED, `0212/model/out/`)

`0212/model/bzcore_0212.py` is a copy of the note's model. Its docstring lists the additions. For the first draft: `newk2`, a two-argument mint that stores one argument; `erase` and `euse`, an owned conversion to a type that announces nothing and a read through it; R2's owned-argument clause at `fstm`; a child destructor that reads its back-pointer; the flag set at the beginning of `Dispose()`, as r40, in place of §28.12. For this revision: `newko`, an `out owned` recipient of a holder; `wrap` and `wuse`, an owning choice that owns a holder and a read through it; the rule that a child taken out by a replacing `move` carries its owner; and a machine variant in which every object is built with its child already linked back to it, which the checked body cannot see (the critic's c03). The note's 42 self-tests still pass, and 23 new programs, each checked under two rule sets, cover the additions; five more, each under three readings of the taken-out rule, cover the refinement of §4.7 (`out/selftest0212.txt`, 61 checks). The baseline is v0.355 with proposal 0205's rules applied.

Each sweep compares four rule sets against the baseline: the note's candidate; **0212 as first drafted**; **0212 revised**; and the revised rules with an oracle mint that carries only stored arguments. Columns: programs enumerated; accepted at the baseline; the *trust surface* (accepted programs in which some run reads a dead or foreign object through a stored borrow); what the first draft left; what the revision rejects and leaves.

| bounds (3 variables unless stated) | programs | accepted | trust surface = race + fault every + fault some | first draft: left (race; fault every / some) | revised: newly rejected = race + fault every + fault some + dangling never read + clean | revised: left (race; fault every / some) |
|---|---|---|---|---|---|---|
| 4 statements | 95,382 | 31,497 | 54 = 18 + 36 + 0 | 6; 12 / 0 | 786 = 18 + 36 + 0 + 336 + 396 | 0; 0 / 0 |
| 4, holder destructor reads | 95,382 | 31,497 | 3,114 = 834 + 2,280 + 0 | 258; 768 / 0 | 3,540 = 834 + 2,220 + 0 + 12 + 474 | 0; 60 / 0 |
| 4, child destructor reads (§28.12 timing) | 95,382 | 31,497 | 78 = 24 + 54 + 0 | 6; 12 / 0 | 1,752 = 24 + 54 + 0 + 468 + 1,206 | 0; 0 / 0 |
| 4, both destructors | 95,382 | 31,497 | 3,138 = 840 + 2,298 + 0 | 258; 768 / 0 | 4,368 = 840 + 2,244 + 0 + 18 + 1,266 | 0; 54 / 0 |
| 4, both destructors, children linked in constructors | 95,382 | 31,497 | 4,032 = 1,074 + 2,958 + 0 | 486; 1,422 / 0 | 4,368 = 1,074 + 2,904 + 0 + 42 + 348 | 0; 54 / 0 |
| 4, 2 variables, `if` | 60,408 | 14,106 | 156 = 58 + 18 + 80 | 20; 6 / 54 | 546 = 58 + 18 + 40 + 258 + 172 | 0; 0 / 40 |
| 4, `if` | 379,188 | 89,754 | 468 = 174 + 54 + 240 | 60; 18 / 162 | 4,026 = 174 + 54 + 120 + 2,088 + 1,590 | 0; 0 / 120 |
| 4, `while` | 2,921,382 | 607,872 | 1,518 = 36 + 36 + 1,446 | 12; 12 / 498 | 16,104 = 36 + 36 + 1,428 + 6,234 + 8,370 | 0; 0 / 18 |
| 4, inner `do … end` | 2,921,382 | 729,072 | 2,514 = 594 + 1,920 + 0 | 198; 630 / 0 | 28,704 = 594 + 1,920 + 0 + 12,420 + 13,770 | 0; 0 / 0 |
| 4, inner `do … end`, both destructors | 2,921,382 | 729,072 | 60,648 = 11,766 + 48,882 + 0 | 3,582; 16,506 / 0 | 103,332 = 11,766 + 46,968 + 0 + 1,056 + 43,542 | 0; 1,914 / 0 |
| 5 statements | 2,120,646 | 471,603 | 3,702 = 1,062 + 2,640 + 0 | 354; 936 / 0 | 25,290 = 1,062 + 2,580 + 0 + 12,198 + 9,450 | 0; 60 / 0 |
| 5, both destructors | 2,120,646 | 471,603 | 70,512 = 18,270 + 52,242 + 0 | 5,112; 16,386 / 0 | 97,356 = 18,270 + 50,892 + 0 + 1,416 + 26,778 | 0; 1,350 / 0 |
| 5, both destructors, children linked in constructors | 2,120,646 | 471,603 | 89,010 = 22,860 + 66,150 + 0 | 9,522; 30,108 / 0 | 97,356 = 22,860 + 64,800 + 0 + 2,370 + 7,326 | 0; 1,350 / 0 |

Reading.

1. **The revised rules leave no race through a stored borrow at any bound**, and every fault they leave is one of the two named residuals (item 4). Nothing is newly accepted or newly unsound in any sweep; since every rule set here only tightens, that is a consistency check on the model, not a finding. The model has no frames (§3.3).
2. **The first draft was unsound on the critic's routes, and the model measures by how much.** Once the three statements are in the model, the first draft leaves races at every bound: 6 at 4 statements; 258 where a holder's destructor reads its borrow; 486 where children are linked in constructors; 9,522 at 5 statements with both. `out/left_v3l4_kq_draft1.txt` classifies each of the 1,908 programs it leaves at 4 statements by route: an `out owned` recipient (996), a child taken out of its owner (834), an owning choice (12), and the two residuals (66).
3. **R5 closes the erasure hole it was written for.** Against the note's candidate, the first draft's R5 rejects 132 more programs at 4 statements and removes 12 certain faults where the holder's destructor reads its borrow (780 to 768); at 5 statements it removes 6 races (360 to 354).
4. **What the revision leaves is not trust.** At 4 statements with both destructors and linked children it leaves 54 programs: 36 slot kills (`new a / fbrw b a / fst a / newk c b` — K5.6) and 18 announced kills of one source of a two-argument mint (`new a / new b / newk2 c a b / disp a` — K4.2). At 5 statements every one of the 1,350 programs left was classified (`out/left_v3l5_kq.txt`): 738 slot kills, 588 announced kills of one source of a two-argument mint, and 24 of one source of two where a later store added the second (`new a / new b / newk c a / kset c b / disp b` — a later store adds to the set and never removes, so a borrow it replaced stays in it). None is a race, and none is a read through a child's back-pointer.
5. **The mint's over-approximation, priced.** With the holder destructor at 4 statements the oracle rejects 54 fewer clean programs than the revision (420 against 474) and 24 more faulting ones (it leaves 36 where the revision leaves 60). At 5 statements with both destructors: 1,404 fewer clean rejections and 576 more faults caught. In the corpora one construction site of 312 carries an argument its constructor only reads (the note, MEASURED). Q3, Q11.
6. **The taken-out rule, priced.** A child taken out of its owner carries the owner whether or not it was linked. Where children are not linked, the rejections this adds are clean: with a child destructor at 4 statements the revision rejects 1,752 of 31,497 accepted programs against the first draft's 384, and 1,206 of them are clean. Where children are linked in constructors the same rejections are the races and faults of item 2. The model's child class is always a borrow-bearing one, so this is the rule at its most expensive; in the corpora no `move` of another object's field has a borrow-bearing type (§6). Of the 1,206, 984 are this rule's — accepted again with the rule switched off — and 954 of those are in programs that never link a child; the other 222 are owned erasures and holder rules (`out/takeout_v3l4_qdtor.txt`). §4.7 measures a refinement that would relieve the values whose type cannot reach their holder, and says why it is recorded and not adopted.
7. **The destructor window is closed by §28.12, not by a 0212 rule.** With the child destructor and §28.12's timing the revision leaves 0 faults; with r40's timing the baseline has 4,938 certain faults at 4 statements and the revision leaves 4,806 of them (`out/compare_v3l4_qdtor_r40.txt`), because the fault is in the runtime's flag timing and no static rule here addresses it.
8. **A release is a use at every release point, a drop-on-overwrite included.** The first version of the model's release rule counted the scope end and an explicit disposal only. A sweep at 5 statements then left 6 races: `new a / link a / fmove b a / send a / asg b` — the reassignment of `b` runs the displaced child's destructor, which reads an owner already sent to another domain. §8.2's *Release* bullet names the three release points for that reason, and a self-test pins the program.
9. **R2's owned-argument clause is needed.** A linked child moved out of one owner and stored into another brings its old owner along. The self-tests cover the three cases: declared in the wrong order (rejected), in the right order (clean), and the old owner then disposed (rejected at the new owner's release).

### 5.3 Probes (MEASURED, r40, `0212/probes/results.md`)

51 probes: d01–d12 (the destructor window), g01–g08 (erasure), a01–a08 (async), q01–q03, q05 and q06 (the open questions), c01–c14 (the critique: c01–c05 are the first critic's own, c06–c12 cover finding 6 and the notes, c13 and c14 the record-key rewrite and a forwarded comparer), and e01–e04 (critique round 2: the second critic's builder, the open-class counter-example to its refinement, and two variants that show what r40's verdict on the builder depends on). Each records today's verdict and behaviour and the verdict 0212 gives. No probe tests 0212's rules: the compiler does not implement them.

### 5.4 What is argued, not modelled

- **R4, the callee side.** The model has one body. That a constructor's plain parameter reaches only the object under construction, and that a `scoped` parameter reaches only `this`, is the callee-side check §28.14 already runs for `scoped`. The `owned` parameter rule (critique finding 3) is callee-side too and is argued; its probe is c04.
- **R5's per-instantiation reading**, and the completeness of §3.1's table. The generics matrix (`docs/research/generics-matrix`) has not been run against it. Proposal 0159 records that the same family, for values, "has produced a new route at each of four matrix runs" (QUOTED), and the first draft of this proposal lost four routes that a critic found; the table is now read off the consume table instead of written by hand, which is an argument for completeness and not a proof.
- **R1 through interfaces, inheritance and virtual dispatch**; **R7** and everything in a frame; closures that capture an instance; `Shared<T>` handles and the library members stated to carry the set alone; the comparer rule.
- **The two order tests** (R6): probes d04, d06 and d07, no model.
- **The mint over a temporary** (K8.4): the note's hand-checked programs, no model statement.

---

## 6. Measurement in the corpora

**Method.** `0212/data/scan_0212.py` re-runs the note's scanner unmodified over the same corpora — 5,902 rule-corpus units, 802 doc-extracted blocks, 2,574 lit programs, 243 gtest files with embedded programs — and adds one pass per rule 0212 adds. The re-run reproduces the note's `summary.txt` byte for byte (MEASURED). The scanner is syntactic and per unit, as the note's is.

**Stores under R4** (157 K4.3 store sites; MEASURED).

| population | sites | admitted as written | needs `scoped` | rejected: relates two parameters | rejected: keeps an `Object` parameter |
|---|---|---|---|---|---|
| accepted tier | 58 | 44 | 12 | 1 | 1 |
| expected-reject tier | 29 | 19 | 9 | 0 | 1 |
| gtest, distinct | 37 | 19 | 10 | 8 | 0 |

**The rules 0212 adds** (MEASURED).

| rule | population | finding |
|---|---|---|
| R1, interfaces | 765 interfaces | 0 borrow-bearing by a `scoped` parameter; 3 distinct interfaces (11 rows, all gtest) by a reference-typed property not marked `readonly`; 18 borrow-bearing classes implement an interface of their own unit that is not borrow-bearing; 0 owned upcasts of an instance with a non-empty set |
| R5, generic bodies | 161 `owned` type-parameter-typed parameters | 18 erased (all `throw x`), 0 in a dispatched generic method, 0 instantiated at a borrow-bearing class |
| R5, `throw` | every `throw new C(…)` | 0 of a borrow-bearing class |
| R6, destructors | 8 borrow-bearing classes with a destructor | 1 destructor names a field that makes its class borrow-bearing (a lit case that tests a removed collection member); 0 call an instance method; 0 constructions carry `this`; 3 constructions are held directly in a field |
| R7, async | 1,064 `async` methods | 0 parameters or task results of borrow-bearing class type; 0 borrow-bearing locals across an `await` |
| R4, statics | every method storing a parameter into a static | 2, both already expected-reject (§28.3.8 *Static stores*) |
| R2 + K5.5, local collections | a local owned binding stored into a local borrowing collection and then moved (`data/out/collection_transfer.txt`) | 4 sites, the rule cases of the report's transfer entry; in none is the collection named after the transfer, so 0 are rejected. At stage 3 their report line goes with the collection-held clause |

**The positions revision 2 adds** (MEASURED, `data/out/summary_0212.txt`, `rev2_findings.tsv`; one pass per critique finding).

| finding | population | at a borrow-bearing class |
|---|---|---|
| 1, `out owned` | 27 `out owned` parameters (13 in the accepted tier) | 0 |
| 2, move-only carriers | every owning choice and owning inline array; 108 namings of `Result<…>` (5 at a class of the unit) | 0; 0; 0 |
| 3, `owned` parameters | every `owned` parameter of a class the unit declares | 0 are of a borrow-bearing class, in any member kind |
| 4, a `move` of another object's field | 18 (7 in the accepted tier) | 0 |
| 6, comparer constructors | 5 constructions of the four collections with a non-capacity argument | all five are expected-reject cases that pass a string or a literal; none passes a comparer. r40 does not implement the comparer constructors (c06) |
| 7, `Clone` / `Downgrade` / `TryUpgrade` | units that name `Shared<C>` or `Weak<C>` at a borrow-bearing `C` | 0 units |
| 10, a constructor handing `this` to a parameter's member | every constructor | 0 |
| 11, exception classes | every class deriving from an exception type | 0 user classes; one standard-library listing, `TypeInitializationException`, declares `InnerException: Exception?` (§8.13) |

So the six fixes change no corpus verdict: every count at a borrow-bearing class is zero. That is a statement about the corpora, which are test programs written before any of these rules; it prices the fixes at nothing measured, not at nothing.

**Programs.** Of 5,750 accepted-tier programs, 131 store a borrow or build a borrow-bearing instance: **114 unchanged, 12 compile after a `scoped` marker, 5 rejected**. The rules 0212 adds to the note's candidate reject none.

**The five rejected programs, with rewrites.** Each "after" uses only what exists today.

1. `s28_3_0205` and `s28_3_0206` assert the checked-mode fault of a literal stored through a plain constructor parameter. Under 0212 the construction is K8.4's no-temporary-source error. They become expected-reject cases of the new rule; the fault they observe is unreachable.
   ```bezel
   h := new B([1, 2, 3]);                       // before (s28_3_0205): admitted; the array literal dies at this statement's end
   var xs: Array<int> := [1, 2, 3];             // after, either: bind the source first, in a scope that outlives `h` …
   var h := new B(xs);
   private owned _xs: Array<int>;               // … or: `B` owns it, through `constructor(owned xs: Array<int>)`, and the literal transfers
   ```
2. `s22_6_2312` stores array-literal temporaries in `Row.Items` and reads them two statements later. By K8.1 the literals are disposed at the declaration's end; r40 prints `30` (the note, finding S5).
   ```bezel
   var rows: Array<Row> := [new Row([1, 2]), new Row([3, 4, 5])];             // before
   public owned Items: Array<int>;                                             // after: `Row` owns its items,
   public constructor(owned a: Array<int>) Items := a; end constructor;        // so `new Row([1, 2])` transfers the literal
   ```
3. `s28_11_0041` pins the admission of a store into a collection another parameter holds.
   ```bezel
   public static method Store(a: Array<Item>, it: Item): int  a.Add(it); return a.Count;  end Store;   // before
   a.Add(it);                                              // after: store at the caller, where `a`'s binding is visible
   ```
   Where the storing object is a class of the program's own, the receiver form is the other rewrite (probes q01, q02: `sink.Feed(scoped a)`).
4. `s28_3_0119` keeps an `Object` parameter in a method, where `scoped` is not admitted (§28.14).
   ```bezel
   public method Put(o: Object)  _o := o;  end Put;            // before
   public method Put(scoped s: IShow)  _kept := s;  end Put;   // after: declare the parameter at the type the callee stores
   ```

**The twelve marker edits** are all of one shape: `public method Set(c: Config)` becomes `public method Set(scoped c: Config)`, and each visible call takes the marker (4 call sites in the accepted tier).

**The limit of the evidence** is the note's: the corpora are test programs. Kind 2 — the long-lived registry — and async code that builds helpers are under-represented, so "5 programs" is a floor. Stage 1 exists to replace it with a measurement.

---

## 7. The ramp

Three stages, each one revision. Process §2 calls the last a **Ratchet**: its design is ruled by this proposal's acceptance, the stage-2 warning is its ramp, and its Migration note states both ends. **Stage 1 landed as v0.357** (§7.5); stages 2 and 3 are scheduled, each behind its gate (Q9 as ruled): their text in §8 stays here, tagged [S2] and [S3], until the train that lands it re-verifies its anchors against its own head (process §4).

### 7.1 Stage 1 — the report (no verdict changes)

**Lands:** §3.6's row; §28.15 as the definition of the property and of the classification; §28.3.12's new clause 4 (§8.5). K4.3 and every admission stay as they are.

**What the mode prints.** At each field-held, collection-held and comparer-held line, the classification §28.15 gives the store:

- **bounded** — admitted as written: a constructor's store, a self-sourced store, a store through a `scoped` parameter or a setter, a standard collection's store into a location the body holds;
- **needs `scoped`** — a member that is not a constructor keeps a plain parameter; the line names the parameter;
- **no static form** — the line names the reason: the store relates two parameters; an `Object` parameter is kept by a member that is not a constructor; the holder is static storage or is reached through a borrow.

And one line, under the same mode, at **each site that is not a store and that §28.15 rejects**. The rule is stated that way, by reference to the section, so that the list cannot fall behind it. Today that is: a value that would outlive a source, an `out owned` recipient and a move-only value among them; a temporary source; a transfer or reassignment of a carried source under a live value; a use that a disposal of every carried source dominates; an owned value with a non-empty set at an owning position whose type is not a carrier type, a generic instantiation that fails that requirement included; an `owned` parameter of a carrier type stored anywhere but `this`, the result or its end; a `throw` of a value that carries a source; a comparer that contributes a source at an instantiation that is not a carrier; a failed order test.

The zero-report claim is unchanged. The stage adds one statement: **a unit whose report has no "needs `scoped`", no "no static form" and no non-store line compiles unchanged at stage 3.** The claim is only as good as the rule set is complete: against the first draft it was false for the critic's five programs, which stage 3 would have had to reject and stage 1 would not have named. It was re-checked after the fixes, two ways. Each of the critic's programs now has a rejecting rule and therefore a line (c01: the generic requirement; c02 and c05: a value that outlives a source; c03: the same, through the taken-out child; c04: the `owned` parameter rule). And in the model no program accepted under the revised rules races through a stored borrow at any bound, the bounds that contain those three shapes included (§5.2).

### 7.2 Stage 2 — the warning

**Lands:** one paragraph of §28.15 changes (§8.2, *Staging*), and §28.14's widening of the class-typed `scoped` parameter (§8.7). In the default build every site stage 1 marks "needs `scoped`", "no static form" or with a non-store line draws a warning that names the rule and the rewrite. Existing admissions are unchanged; a "bounded" store draws nothing.

The widening lands here, not at stage 3, so that every warning has a rewrite that already compiles: today a `scoped` argument is rejected against a plain `var` holder (the note's p10), which is where many method stores will move. It only admits more. For a store it newly admits, §28.15's bound is enforced as an error from this stage: those programs do not compile today, so nothing is taken from anyone, and the new admission is never a trust admission.

This is not the always-on trust report that `DECLINED.md` records as declined (proposal 0131 §4 item 2; rationale §5.31): that row's ground is that a trust admission "is not a hazard but the taught spelling of the back-reference". The stage-2 warning fires only where the taught spelling is about to change — 12 of 58 store sites in the accepted tier, plus 2 rejected — and it is the ramp of a scheduled rejection.

### 7.3 Stage 3 — the error (Ratchet)

**Lands:** the §28.15 staging paragraph is deleted; K2.3, K4.3, K5.5, K8.4, §28.3.8, §28.3.12, §28.11, §28.14 and the rest of §8 take their final text; the three clauses leave the report.

**The ramp, both ends** (the form process §5 item 2 requires of the Migration note; *W* and *E* are the revisions that land stages 2 and 3):

> *Ramp.* The warning landed in v0.*W*: a member that is not a constructor keeping a plain class- or interface-typed parameter, a store with no static form, and each non-store site of §28.15 drew a warning naming the rule. This revision, v0.*E*, makes each of them a compile error and deletes K4.3's field-held, collection-held and comparer-held clauses. Between the two, a program that compiled before v0.*W* compiled unchanged, with the warning at each such site; a program with no such warning compiles unchanged at v0.*E*.

**Gates** (owner, Q9). Recommended: stage 1 lands with acceptance. Stage 2 lands when the compiler implements stage 1 and its prerequisites (§10) and the report has been read on the in-tree programs. Stage 3 lands when the compiler implements the stage-2 warning, the in-tree corpora are migrated (§7.4), and the report over the reference programs shows no "no static form" site without a rewrite — or the owner rules on Q2 with that count in hand.

### 7.4 Conformance cases

The rule corpus states the verdict the spec requires (process §5 item 3c), so each stage adds cases and stage 3 re-states others. Proposed cases, by family; the expectation is given per stage as S1 / S2 / S3. `report: X` means the report mode's line carries classification X; outside the mode S1 is `quiet`.

| # | case | S1 | S2 | S3 |
|---|---|---|---|---|
| 1 | a constructor stores its plain parameter in a borrow field (the back-reference) | report: bounded | quiet | accept, no report line |
| 2 | a method stores its plain parameter in a borrow field | report: needs `scoped` | warn | reject |
| 3 | the same with `scoped`, the holder a plain `var` | reject (§28.14 today) | accept | accept |
| 4 | a setter stores `value`; an auto-property assignment | report: bounded | quiet | accept |
| 5 | a static method stores one parameter into a collection another holds | report: no static form | warn | reject |
| 6 | the same as a member of the storing class with a `scoped` parameter, the receiver a `using var` declared after the source | accept | accept | accept |
| 7 | a method stores an `Object` parameter | report: no static form | warn | reject |
| 8 | a holder's source is moved while the holder is used later | report: non-store line | warn | reject (K5.1) |
| 9 | a holder's source is reassigned while the holder is used later | report | warn | reject (K5.2) |
| 10 | a holder's only source is disposed, then the holder is used | report | warn | reject (§28.2) |
| 11 | one source of two is disposed, then the holder is used | quiet | quiet | accept; run faults in checked mode |
| 12 | a later store of an inner-scope source into an outer holder | report | warn | reject (K2.3) |
| 13 | a holder built over a fresh temporary and bound | report | warn | reject (K8.4) |
| 14 | a holder built over a temporary and used within the statement | quiet | quiet | accept |
| 15 | a two-level holder: an `owned` field of borrow-bearing type; the outer instance carries the inner's source | report: bounded | quiet | accept; the kill of the source rejected as in 8 |
| 16 | a factory returning `owned C` over its parameter; the result carries the argument | quiet | quiet | accept; the kill rejected as in 8 |
| 17 | an owned borrow-bearing instance returned at an interface that is not borrow-bearing | report | warn | reject (R5) |
| 18 | the same interface with a `scoped` member (borrow-bearing by derivation) | quiet | quiet | accept; the result carries the argument |
| 19 | `Hide<T>(owned x: T): owned IShow`, instantiated at a borrow-bearing class | report at the instantiation | warn | reject where named |
| 20 | the same generic at a class that is not borrow-bearing | quiet | quiet | accept |
| 21 | a dispatched generic method whose body erases, unconstrained; then under `where T: sendable` | report; quiet | warn; quiet | reject at the body; accept |
| 22 | `throw` of an instance that carries a local | report | warn | reject (R5) |
| 23 | a back-pointing child built with `new Child(this)` into an `owned` field | report: bounded | quiet | accept |
| 24 | that child moved out with a replacing `move` and returned `owned`; the caller's result carries the owner | quiet | quiet | accept; the owner's kill rejected as in 8 |
| 25 | sibling fields, the borrowing field declared first, its class with a destructor | report | warn | reject (R6) |
| 26 | the same, declared after | report: bounded | quiet | accept; run clean |
| 27 | a holder with a destructor declared before a later-stored source | report | warn | reject (declared-before) |
| 28 | the same holder with a trivial-dispose class fixed by `new` | quiet | quiet | accept |
| 29 | an instance with an empty set stored in a static | quiet | quiet | accept |
| 30 | a store into a collection a static holds | report: no static form | warn | reject |
| 31 | a local holder over a plain parameter across an `await`, awaited by the caller | report: bounded | quiet | accept; run clean |
| 32 | `await` of an `OwningTask<C>` over a parameter, the source then disposed, the result used | report | warn | reject (§28.2) |
| 33 | a child's destructor reads its owner in the cascade (§28.12) | run clean | run clean | run clean |

| 34 | `Make(c, out var a)` over `out owned a: Account`; the recipient moved to a binding that outlives `c` (critic c02) | report | warn | reject (K2.3) |
| 35 | the same recipient used inside `c`'s scope | quiet | quiet | accept |
| 36 | a callee assigns a holder over its own `using var` to an `out owned` parameter | report | warn | reject (the return rule) |
| 37 | an owning choice `Slot.Full(owned a: Account)` returned from a factory over a parameter; the caller's value outlives the argument (critic c05) | report | warn | reject (K2.3) |
| 38 | `Result<Conn, E>.Ok(conn)` over a class `Conn` that is not borrow-bearing, returned and matched | quiet | quiet | accept |
| 39 | `Result<Account, E>.Ok(new Account(c))` returned over a parameter; the payload taken by an owning match and used inside `c`'s scope | quiet | quiet | accept; the payload carries `c` |
| 40 | an owning inline array `[2 owned]Account?` whose slots hold holders over a local, stored in a static | report | warn | reject (static storage) |
| 41 | `static Give(owned a: Account, b: Box)` whose body calls `b.Put(move a)` (critic c04) | report | warn | reject in `Give` (the `owned` parameter rule) |
| 42 | an instance method `Put(owned a: Account)` that stores into an `owned` field of `this`; the caller's receiver then carries the argument's source | quiet | quiet | accept; the source's kill rejected as in 8 |
| 43 | a static method that takes `owned a: Account` and returns it `owned` | quiet | quiet | accept; the result carries the argument's sources |
| 44 | `k := move n.Kid with null` from outside `Node`, where the constructor linked the child to `n`; `k` outlives `n` (critic c03) | report | warn | reject (K2.3) |
| 45 | the same take, the child used and released inside `n`'s scope | quiet | quiet | accept |
| 46 | `Hide<IStore>(move st)` at a borrow-bearing interface `IStore` (critic c01) | report at the instantiation | warn | reject where named |
| 47 | `new Dictionary<string, int>(cmp)` with a local comparer; with a `static readonly` comparer; `new Dictionary<string, Item>(cmp)` with a local comparer | report; quiet; quiet | warn; quiet; quiet | reject; accept; accept, the dictionary carries `cmp` |
| 48 | a `Dictionary<string, int>` built with a `static readonly` comparer, sent to another domain | accept | accept | accept (it is sendable and carries nothing) |
| 49 | `second := first.Clone()` over `Shared<Account>`, then `first` moved away while `second` is used | quiet | quiet | accept; a later kill of the account's source rejected through `second` |
| 50 | an element taken from an `OwningArray<Account>` with a `Pop` member and returned `owned` from the method that built the array over a parameter | quiet | quiet | accept; the result carries the parameter, not the local array |
| 51 | `constructor(bus: Bus) bus.Subscribe(this);` (observer self-registration) | report: no static form | warn | reject |
| 52 | the observer keeps the `Subscription` from `bus.Changed.Subscribe(…)` in an `owned` field | accept | accept | accept; run clean |
| 53 | `throw new WrapError(e)` from the catch arm of `e`, `WrapError` storing its cause in a borrow field | report | warn | reject (`throw`) |
| 54 | the same exception keeping the cause's message, or an `owned` cause given a fresh exception | accept | accept | accept; run prints the message |
| 55 | a back-pointing child with a destructor declared before a sibling `owned` field (probe d04's shape) | report | warn | reject (the order test) |
| 56 | a class forwards its constructor's parameter `cmp` to `new Dictionary<string, int>(cmp)` in an `owned` field (probe c14) | report | warn | reject at the construction |
| 57 | the same body in a generic `Index<K>`: named at `Index<string>`; named at `Index<Item>` for a class `Item` | report where named; quiet | warn; quiet | reject where named; accept, the index carries `cmp` |
| 58 | `new Dictionary<string, int>(IgnoreCase.Instance)` over a `static readonly` field of a user class (probe c07) | accept; §22.12 rule 4's warning at the field's declaration and none at the construction | the same | the same; the dictionary carries nothing |
| 59 | `Dictionary<FoldedKey, int>` over a record key that declares its own equality, built with no comparer, then sent to another domain (probe c13) | accept, no warning | accept | accept |
| 60 | `var b := new Builder(c); var r := b.Build(); return r;` where `Build` returns `owned Report`, a borrow-bearing class that never borrows the builder (probes e01, e03) | report: non-store line | warn | reject (the return rule: `r` carries the local `b`) |
| 61 | the same product built where its source is in hand, `return new Report(c);`; and `Make(b: Builder): owned Report => b.Build()` | quiet | quiet | accept; the result carries the parameter |
| 62 | `Build`, declared `owned Report`, returns a subclass that stores `this`; the builder is moved away while the result is used (probe e02) | report: non-store line | warn | reject (K5.1) |
| 63 | `return b.Build();` where `Build` returns an `owned` value of a class that is not a carrier type (probe e04) | accept | accept | accept; the value carries nothing |
| 64 | a static method that takes `owned a: Account` and returns `new View(a)`, a holder that borrows the parameter (beside case 43, which returns the parameter itself) | report | warn | reject (the return rule: the parameter's binding is not re-established) |

Case 33 is not a 0212 case; it is listed because the corpus lacks it beside `s28_12_0014` and r40 fails it. Cases 34–55 are revision 2's: one or more per critique item, with an accepting twin where the fix must not over-reject. Cases 56–64 are critique round 2's: the forwarded and the `static readonly` comparer with the record-key rewrite beside them, the builder and its rewrites, the open-class counter-example, and the two halves of the return rule for an `owned` parameter. Case 63 states what the spec requires under 0212; r40 rejects that program today (probe e04).

**Existing cases stage 3 changes** (the note, MEASURED): 5 accepted-tier programs become expected-reject (§6); 12 take a `scoped` marker; 12 rule cases pin the report's text and 17 cite the field-held clause; 3 gtest files assert report lines; in the expected-reject tier and gtest, 19 more signatures take `scoped`, 8 distinct gtest stores relate two parameters, and 1 expected-reject case keeps an `Object` parameter.


### 7.5 Stage 1 as landed (v0.357)

**Landed.** The [S1] edits, applied against `156fdf9c9` (v0.355): spec §3.6's table entry (adaptation 8), §28's chapter-at-a-glance row, §28.15 (new), and §28.3.12's clause 4; stdlib §1 *Borrow-bearing statements*; rationale §5.32; in `DECLINED.md`, rows D1–D7 as `declined-at-proposal` (D1 marked provisional) and two `deferred` rows, D8 and the Q10 follow-up — D8 is an open refinement, not a decline, so the `deferred` class records it more truly than §8.10's heading does. Outside the normative set, the code-generation reference moves its three pins and gains one forward note in §4.4, and the rule corpus gains 20 cases (below). No verdict changes.

**Adapted.** All 41 anchors still occurred exactly once. Eight texts were adapted minimally:

1. §28.15's opening paragraph omits "([K2](#sec-kernel-k2) clause 5)", and its property paragraph omits "No carrier type is sendable" — the two phrases §8.2's [S3] note holds back — and gains one sentence pointing at *Staging*.
2. *The bound*, *Returns*: `return move a;` became `return a;`. The operand of a `return` under an `owned` return is a marker-free position, where a `move` is the redundant-marker error (§28.3; code-generation reference §4.2). The rule the sentence states is unchanged, and §8.2 is corrected to match.
3. *Staging* says that no verdict changes in positive form ("a program is accepted or rejected as the rest of this specification decides"). It adds the ramp's two later ends: a warning in the default build, then a compile error with the three clauses retired. That is process §5 item 2's ramp requirement, in the one place the [S1] text gives the spec.
4. *Example*: over §28.3.1's `Widget`. The comments give §28.15's verdict and the report's line, and say that every line compiles in this revision. The `Retarget(scoped inner)` block was dropped: §28.14's declared-before admission already rejects it, so it could not illustrate staging.
5. Stdlib §1: the sentence on rejection at the call is held back, as §8.8 tags it. So is "which no carrier type is" after `Locked<T>` and `RwLocked<T>`, which is §30.14's [S3] sentence.
6. Rationale §5.32 is written in stage 1's present tense: the field-held clause *is* left on trust. It gains a paragraph on the ramp and its gates, and states three rulings: Q2's decline is provisional, the comparer paragraph gives Q13's price, and "What was not done" names the Q16 and Q10 follow-ups.
7. Rationale §5.1: the [S3] replacement is not applied, but the paragraph's "planned work" sentences were brought current. They had described derived retention as the plan; they now cite spec §28.15 and §5.32, and say that proposal 0132 is superseded (§9).
8. Spec §3.6: §8.1's separate **borrow-bearing class** row fails `check_docs.sh`'s TABLE check. Arm 1 requires the table's row names to have pairwise-distinct first components, and the record property's row is already **borrow-bearing**: the row names are what diagnostics quote (§3.6 rule 4). So the entry is merged into that row as its class-and-interface arm, defined in §28.15, with §8.1's text kept. One sentence is added: a rule that reads the property of a value type reads the record, inline-array and choice arms only, and the class arm is read where a rule names a borrow-bearing class or a carrier type. The property's name in §28.15, *borrow-bearing class*, is unchanged.

**Scheduled.** *Stage 2*: §8.2's [S2] *Staging* paragraph; §8.7's widening, `scoped` at a type parameter included (Q8); the `DECLINED.md` annotation of the always-on row. *Stage 3*: every [S3] edit of §8, §8.13's consequential mentions, §8.14's Migration sketch, the remaining `DECLINED.md` annotations, and the S3 column of §7.4. The gates are Q9's. Q2 is re-ruled at the stage-3 gate and Q16 at the stage-2 gate. Each later train re-verifies its anchors against its own head (process §4); §8.2's text is superseded there by §28.15 as landed.

**Conformance at stage 1.** The corpus expresses report-mode expectations: `flags: --report-trust-admissions` with `warn` or `quiet`. The cases written are:

- the store classifications, §7.4 cases 1, 2, 5 and 7: `s28_3_0924`–`s28_3_0927`;
- the non-store lines with their twins: cases 8 and 9 (`s28_15_0001`, `s28_15_0002`, `s28_15_0004`), 10 and 11 (`0005`, `0006`), 13 and 14 (`0007`, `0008`), 17 and 18 (`0009`, `0010`), 19 and 20 (`0011`, `0012`), 25 and 26 (`0013`, `0014`), and 60 and 61 (`0015`, `0016`);
- `s28_15_0003`, *Staging* in the default build.

The non-store cases mint through a constructor that reads its argument and does not store it (Q3's signature reading), so no K4.3 line exists in them today. Each `warn` among them fails until the classification is implemented, and its `quiet` twin passes (MEASURED on r40: all twenty compile, and the non-store `warn` cases print nothing). The store-classification cases pass the gate on today's line. Their `diagnostic:` field names the classification, and it is what the classification run compares.

**Owed** (left to a corpus lane, not written under landing pressure):

- the report-line cases whose programs need a K4.3 store beside the line, or an `out owned` recipient, an owning choice, a replacing `move` or a comparer constructor (r40 implements the last two in no form): cases 4, 12, 15, 21–23, 27, 30–32, 34, 36, 37, 40, 41, 44, 46, 47, 51, 53, 55–57, 62 and 64;
- the cases whose S1 column is a default-build verdict of today's rules, which stage 1 does not change: cases 3, 6, 16, 24, 28, 29, 35, 38, 39, 42, 43, 45, 48–50, 52, 54, 58, 59 and 63;
- case 33, which is not a 0212 case (§7.4).

**Trackers** (`spec-tracking`, filed at the landing): [#8419](https://github.com/s8lap/bezel-lang/issues/8419) (the property and carrier types), [#8420](https://github.com/s8lap/bezel-lang/issues/8420) (the store-line classification), [#8421](https://github.com/s8lap/bezel-lang/issues/8421) (source sets and the bound), [#8422](https://github.com/s8lap/bezel-lang/issues/8422) (owned erasure), [#8423](https://github.com/s8lap/bezel-lang/issues/8423) (the callee side, the order tests and the comparer), and [#8424](https://github.com/s8lap/bezel-lang/issues/8424) (§10 prerequisite 4's `scoped`-parameter over-rejections, F4 and F4b). Prerequisites already tracked: [#7435](https://github.com/s8lap/bezel-lang/issues/7435) and [#7441](https://github.com/s8lap/bezel-lang/issues/7441) (0205's legs), [#5698](https://github.com/s8lap/bezel-lang/issues/5698) (K5.5's frame-local freeze), [#7506](https://github.com/s8lap/bezel-lang/issues/7506) (§28.12), [#5923](https://github.com/s8lap/bezel-lang/issues/5923) (the replacing `move`), [#5932](https://github.com/s8lap/bezel-lang/issues/5932) (a destructor's `Dispose()` of its own field), and [#7471](https://github.com/s8lap/bezel-lang/issues/7471) (the comparer constructors).

---

## 8. Normative text

Edits are tagged with the stage that lands them: **[S1]**, **[S2]**, **[S3]**. An *anchor* is a phrase that occurs exactly once in the named document at this base (`0212/check_anchors.py` verifies each). "Replace from A through B" means the text beginning at anchor A and ending with anchor B, both included.

### 8.1 Spec §3.6 and the chapter table [S1]

*Insert a table row before the row beginning* `| **auto-hashable** | [§9.2](#sec-body-records) |`:

> ▲ | **borrow-bearing class** | [§28.15](#sec-borrow-bearing-classes) | every class and interface: whether its instances can hold a borrow — a class through a borrow field, an `owned` field of a carrier type, or a store member; an interface through a store member. With the move-only value types that own such an instance these are the **carrier types**, whose owned values carry a source set. It is distinct from **holds-borrows**, which classifies value types by the borrows they hold directly: a rule that reads "a value type that holds borrows" does not read a class, and does not read an owning choice for what its owned payloads store |

*In §28's "Chapter at a glance", insert after the row beginning* `| Letting a callee keep a callback or borrowed object (`:

> ▲ | What bounds a borrow an object stores in a field or a collection? | [§28.15](#sec-borrow-bearing-classes) |

### 8.2 Spec §28.15 (new) [S1; the *Staging* paragraph changes at S2 and is deleted at S3]

*Insert before* `<a id="sec-iteration-2"></a>`:

> ▲ `<a id="sec-borrow-bearing-classes"></a>`
> ### 28.15 Borrow-bearing classes
>
> A class that stores a borrow says so in its declaration: a plain class-typed field is one. This section reads such a class as [§9.7](#sec-records-and-disposal) reads a record that carries a borrow and as [K6](#sec-kernel-k6) reads a frame: its instances are values that carry a source set ([K2](#sec-kernel-k2) clause 5), built from what a caller can read in a signature and bounded by the rules that bound any borrow (derives: [K2.3](#sec-kernel-k2) + [K2.4](#sec-kernel-k2) + [K5.5](#sec-kernel-k5) + [K8.4](#sec-kernel-k8); [§28.14](#sec-scoped-callback-parameters)'s class-typed `scoped` parameter under axiom [A11](#sec-axiom-register)).
>
> **The property.** A class is a **borrow-bearing class** — a derived type property ([§3.6](#sec-derived-type-properties)) — if it declares or inherits:
>
> 1. a **borrow field**: an instance field, an auto-property's backing field and a primary-constructor capture field ([§22.10](#sec-capture-rule-and-layout-stability)) included, that is neither `owned` nor `scoped` and whose declared type, nullability stripped, is a class or interface type that is not runtime-managed ([§28.9](#sec-runtime-managed-types)). `Object` and every interface type are such types; `string`, `Type` and the function types are not;
> 2. an `owned` field whose type is a carrier type (below), or a field of a move-only value type that is one;
> 3. a **store member**: a method, constructor or indexer with a class- or interface-typed `scoped` parameter ([§28.14](#sec-scoped-callback-parameters)), or a settable instance property or indexer, not `set(owned)`, whose type is a class or interface type that is not runtime-managed.
>
> An interface is borrow-bearing if it declares or inherits a store member. A **carrier type** is a borrow-bearing class or interface, or a move-only value type ([§15.3](#sec-move-only-value-types)) — an owning inline array or an owning choice — one of whose `owned` slots or payloads is declared at a carrier type. A value of a carrier type is read as an instance of a borrow-bearing class is, and *instance* in this section includes it. The standard library states which of its types are carrier types (stdlib [§1](Bezel_Standard_Library.md#sec-borrow-bearing-statements)). The property is the least one these clauses give: a class that reaches itself only through `owned` fields and declares no borrow field and no store member is not borrow-bearing. It is evaluated per instantiation ([§3.6](#sec-derived-type-properties) rule 2): a field, slot or payload declared at a type parameter counts at a substitution that makes it one. No carrier type is sendable ([§30.14](#sec-sendable-types)).
>
> **The source set.** An expression or binding whose value is of a carrier type carries one source set, read off its provenance:
>
> 1. **A mint.** Each of these yields a value that carries what the call's receiver contributes and what each of its arguments contributes ([§28.3](#sec-argument-contributions)), whether or not the callee's body stores them — a caller reads the signature, never the body ([K2.4](#sec-kernel-k2)): a constructor call; a call whose declared return type is `owned` and whose result type is a carrier type, the invocation of a function value with such a return slot included, the invoked value's source set counting as the receiver's; the recipient of an `out owned` parameter ([§22.3](#sec-parameter-modifiers)) whose type is a carrier type, on the call's normal return; and a choice case constructor or an owning inline-array production, which carries what its `owned` arguments carry.
> 2. **A later store** adds sources and removes none. It is one of: an argument at a class- or interface-typed `scoped` parameter, which adds what the argument contributes to the constructed object or, for any other call, to the receiver; an assignment to a property or indexer of a class- or interface-typed value ([§28.3](#sec-borrow-lifetime-rules)'s *Property stores*), which adds to the receiver; an assignment `x.f := e` to a borrow field, which adds to the instance `x` names; a store position of a standard-library collection ([§28.11](#sec-borrowing-collections) *Value-copy members*), which adds each placed value's sources to the destination; and an `owned` argument that itself carries sources, which adds them to the constructed object or, at an instance call, to the receiver — at a call with neither, the callee can only return the value or end it (*The callee side*), so nothing is added.
> 3. **A read.** A borrow of the instance — an unmarked read of its binding, a borrow one of its members returns, an element read out of it — carries the binding's source and the instance's set ([K2.2](#sec-kernel-k2)). An owned value **taken out** of an instance — the `owned` result of one of its members (clause 1), or a replacing `move` of one of its `owned` fields or slots ([§28.8.1](#sec-the-replacing-move)), written in a member or outside one — carries the source of the binding that holds the instance and the instance's set: the taking body cannot see whether the instance gave the value a borrow of itself. A payload that an owning match ([§35.5](#sec-owning-match)) takes out of a move-only value carries that value's set. A standard-library member may state that its `owned` result carries the receiver's set and not the receiver's binding (stdlib [§1](Bezel_Standard_Library.md#sec-borrow-bearing-statements)): the members that take an element out of a collection, and those that mint another handle to a shared value.
> 4. **A frame's results.** The value an `await` of a task yields, and an element an owning iteration yields, where the result or element type is a carrier type, carry the frame's carried sources ([K6](#sec-kernel-k6)).
> 5. A local's set is the union over the assignments that reach the read ([K7.2](#sec-kernel-k7)), and travels with a `move`. Static storage contributes nothing to a set: it is always live. A value minted from arguments that contribute nothing carries the empty set and is held wherever an owned value of its type may be, static storage included.
>
> A later store is checked as an assignment of the updated instance to the location that holds it, the reading [§9.7](#sec-records-and-disposal) gives a member store into a record: at a local or parameter by [§28.3](#sec-borrow-lifetime-rules)'s local-scope bullet; at storage `this` owns — a borrow field of `this`, or an `owned` field or owning slot reached from `this` through `owned` hops — by *The callee side* below; at static storage, or an instance static storage holds, only where it adds nothing. At any other location — an instance reached through a borrow, a plain parameter's object — the store is a compile error unless it adds nothing: the body cannot see the binding that holds the instance.
>
> **The bound.** Every rule that bounds a borrow by its sources bounds an instance by its set:
>
> - **Scope.** The instance may not be bound to, or stored into an instance held by, a binding whose scope strictly encloses the scope of a source ([K2.3](#sec-kernel-k2)).
> - **Temporaries.** A fresh unbound owned temporary may not be a source of an instance that outlives its full expression ([K8.4](#sec-kernel-k8)); bind the source first, or let the class own it through an `owned` parameter. An instance that is itself consumed within the full expression is admitted.
> - **Returns.** An `owned` return of an instance, and an assignment to an `out owned` parameter, are admitted where every source is one the callers re-establish: the receiver's contribution, a parameter's, or static storage. An `owned` parameter of a carrier type brings two things, and they are read apart. The opaque caller source it carries (*The callee side*) **is** re-established: the caller's result carries what its argument carried (*The source set*, clause 1). The parameter's own binding **is not**: the callee owns the value and its scope ends it. So returning the parameter itself — `return a;` — is admitted, the returned value being the one the caller handed in; an instance that *borrows* the parameter — `new View(a)` — carries the parameter's binding and is rejected. A source that is a scope-bound local, a `using var`, an `owned` parameter's binding or a temporary is rejected there, as [K6.2](#sec-kernel-k6) rejects a returned frame.
> - **The freeze.** A local that holds the instance is a live local borrow of every source it carries ([K5.5](#sec-kernel-k5)): a transfer or reassignment of a source is frozen while the local is live. An explicit disposal of a source is not frozen ([K4.2](#sec-kernel-k4)); a use of the instance that a disposal of every source dominates is [§28.2](#sec-checked-disposal)'s compile error.
> - **Release.** The local is live to its last use, unless the instance's release may run user code; then the release — at the scope's end, at an explicit disposal, or at a drop-on-overwrite of the binding ([§28.7](#sec-reassigning-owned-fields)) — is a use, and the local is live to it. The release may run user code unless the instance's class is trivial-dispose ([§28.9](#sec-trivial-dispose-classes)) at the type its producer fixes — a `new` expression's class — or, where no producer is known, its declared class is `sealed` and trivial-dispose. Where the release is a use, a later store requires each added source to be declared before the instance's binding, in the same or an enclosing scope; `this` and the enclosing member's parameters count as declared before every local.
> - **`throw`.** An instance that carries a source is not thrown: a catch site re-establishes no set.
>
> **The callee side.** A borrow sourced at a plain class- or interface-typed parameter — the parameter, a borrow reached through it, or an instance that carries it — is **call-bounded**, as a plain function-typed parameter is ([§28.14](#sec-scoped-callback-parameters)). The body may read it, pass it on and hold it in locals. It may store it, directly in a borrow field or carried by an instance placed in an `owned` field or owning slot, only:
>
> 1. into the object under construction, in a constructor, a field initializer or a primary-constructor capture;
> 2. into an instance or collection that is purely local to the body — minted in it and held by its local bindings — which the body then consumes or returns `owned`;
> 3. into storage `this` owns, where the parameter is `scoped`, or is the `value` of a setter or an indexer setter.
>
> A borrow sourced at the receiver — `this`, a field of `this`, a borrow reached through one — may be stored into storage `this` owns in any member: an owner's child may hold a borrow of its owner. Two order tests apply where such a store places an instance in an `owned` field `f` of `this` and the instance's release may run user code (*The bound*, above): if the instance carries the value of an `owned` field `g` of `this`, `g` must be declared before `f`; if it carries `this`, every `owned` field the class declares must be declared before `f`. The cascade then disposes `f` first ([§28.4](#sec-the-destructor)). Every other store of a parameter- or receiver-sourced borrow is a compile error.
>
> An **`owned` parameter of a carrier type** holds a value whose sources the body cannot see: beside what the body adds, it carries an opaque source of the caller's. The body may store it into the object under construction or into storage `this` owns, where its caller adds its sources (*The source set*, clause 2); return it `owned` or assign it to an `out owned` parameter; pass it on as an `owned` argument, to a callee bound by this same paragraph; or end it. Any other store — into an object the body does not hold, as `b.Put(move a)` is at a plain parameter `b`; into static storage; a `throw` — is a compile error.
>
> In a generic declaration a store of a parameter declared at a type parameter is a requirement read at each instantiation that substitutes a class or interface type ([§3.6](#sec-derived-type-properties) rule 2): there the parameter must be `scoped` under clause 3, and the call takes the marker ([§22.3](#sec-parameter-modifiers)); at any other substitution the modifier and the marker have no effect, as `owned` has none ([§28.1](#sec-where-the-owned-modifier-can-appear)).
>
> **Owned erasure.** An owned value whose source set is not empty may not reach an **owning position whose declared type is not a carrier type**. The owning positions are those at which the consume table ([K3](#sec-consume-table)) lets an owned value change holder: an `owned` field or owning slot, a `set(owned)` accessor included; an `owned` parameter; an `owned` return slot or a task's result; an `out owned` slot; an `owned` payload of a choice case or slot of an owning inline array; and an owning iterator's `yield return`, read at its declared element type. The conversion is a compile error at the conversion site; the declared type announces no source, so no rule reading the position could bound the value — [§9.7](#sec-records-and-disposal)'s ground for a value that holds borrows, narrowed to owning positions. The operand of `throw` is such a position at every type (*The bound*). A borrowing position is not one: a borrow of the instance contributes its sources where it is passed (*The source set*, clause 3), and its callee may keep it only where a signature says so. A local binding is not one either: its set is read by flow, whatever its declared type.
>
> In a generic body, an owning conversion of a value whose type mentions a type parameter is a requirement of the body's use of that parameter ([§3.6](#sec-derived-type-properties) rule 2): at each instantiation at which the value's type, after substitution, is a carrier type — a borrow-bearing class or interface, or a move-only value type that is one — the target type, after substitution, must be a carrier type, or the instantiation is rejected where it is named. The requirement is read off the two types; the value's set is not consulted. A dispatched generic method ([§14.2](#sec-dispatched-generic-methods)) decides it at its body: the conversion is a compile error there unless the method's constraints establish that the parameter is not a carrier type, as `where T: sendable` does.
>
> **Teardown.** An instance that `this` owns and that carries `this` is released inside the disposal of `this`, whose accesses run on a live instance ([§28.12](#sec-asynchronous-disposal)); its stored borrow of its owner is therefore valid for its whole life, its destructor included, and the order tests of *The callee side* keep the `owned` fields of its owner's class live for that destructor. An owned value taken out of `this` carries `this` (*The source set*, clause 3) and is bounded as any instance is. A destructor that still reaches an owned field the cascade has already disposed — a derived class's, from an instance a base class holds — meets that field's flag ([§28.12](#sec-asynchronous-disposal)): a slot kill, [K5.6](#sec-kernel-k5)'s posture, and no recorded trust.
>
> **Frames.** A deferred-activation frame carries what its arguments and captures contribute, an instance's sources among them ([§28.3](#sec-argument-contributions)). A local instance in an `async` body may be live across a suspension: every source it can carry is frame-resident or is one the frame already carries, and the frame's own obligation is [§30.1](#sec-async-methods)'s recorded clause.
>
> **Staging.** In this revision the rules of this section define the classification the trust-admission report prints ([§28.3](#sec-diagnostics), clause 4) and change no verdict: [K4.3](#sec-kernel-k4)'s field-held, collection-held and comparer-held clauses admit what they admitted.
>
> *Example.* (the `Tag` example of this proposal's §2, with its comments)

**[S2]** *Replace the Staging paragraph with:*

> ▲ **Staging.** In this revision a site this section rejects, and that [K4.3](#sec-kernel-k4)'s field-held, collection-held or comparer-held clause admits, compiles and draws a warning that names this section's rule and its rewrite. A store that [§28.14](#sec-scoped-callback-parameters) admits only since this revision — a class-typed `scoped` argument against a location its bounding forms do not resolve — is bounded by this section in full: for it the rules above are errors. The trust-admission report prints the same classification ([§28.3](#sec-diagnostics), clause 4).

**[S3]** *Delete the Staging paragraph.* Two phrases of the text above also wait for stage 3, because the rules they cite land then: the opening paragraph's "([K2](#sec-kernel-k2) clause 5)", and the property paragraph's last sentence, "No carrier type is sendable", which lands with §30.14's (§8.8).

### 8.3 Spec §28.3.1, the kernel [S3]

**K2.3.** *Replace the sentence beginning* `Where they are not, an admission is made only under K4's posture:` *and ending* `[§28.11](#sec-collection-element-ownership)).` *with:*

> ▲ Where they are not — a pending frame's carried sources against what its caller does before settlement ([K6.4](#sec-kernel-k6)) — an admission is made only under K4's posture: a structural, decidable check at the site plus a recorded residual obligation with a named backstop ([§30.1](#sec-async-methods), [§28.14](#sec-scoped-callback-parameters)). A borrow stored in an object is never such an admission: the object's class is borrow-bearing and the instance carries the borrow's sources (clause 5).

**K2, a new clause 5.** *Insert before* `*Example.* A plain return must name a source every caller re-establishes (K2.4)`:

> ▲ 5. **Stored borrows are carried.** A value that stores a borrow carries the borrow's sources as its own source set, and every clause above reads it by that set: a value that holds borrows ([§9.7](#sec-records-and-disposal)), a function value ([§28.3](#sec-borrow-lifetime-rules)'s closure bullet), a frame ([K6](#sec-kernel-k6)), and a value of a carrier type — an instance of a borrow-bearing class, or a move-only value that owns one ([§28.15](#sec-borrow-bearing-classes)) — whose set a mint reads off the call's signature and a later store adds to.

**K4.3.** *Replace from* `3. **Recorded trust, or nothing.**` *through* `*Scope* clause 1 states apart.` *with:*

> ▲ 3. **Recorded trust, or nothing.** Where an admission's temporal guarantee can neither be statically compared (K2.3) nor flag-backstopped — the domain clause ([§28.3](#sec-borrow-lifetime-rules)'s field-store bullet, clause 4): a borrowed-from object transferred to, or made reachable from, another domain stays alive, so no flag is ever set — the rule must either reject statically or carry an **individually recorded trust clause**. The recorded clauses (frame-held borrows versus domain transfer, [§30.1](#sec-async-methods), and [§28.14](#sec-scoped-callback-parameters)'s registration-lifetime and quiescence residuals) are the model's honest limit, and no new admission may be made without recording its clause or its backstop. No clause covers a borrow an object stores, and none is needed: the object's class is borrow-bearing, and its instances are bounded by the sources they carry ([K2](#sec-kernel-k2) clause 5, [§28.15](#sec-borrow-bearing-classes)). No clause covers a **static-held** borrow either: a borrow of anything but static storage is stored into no static field or static property ([§28.3](#sec-borrow-lifetime-rules)'s *Static stores*), and an instance a static holds carries only static-storage sources. The recorded set is enumerable, and the trust-admission report mode ([§28.3](#sec-diagnostics)) enumerates it at every site, the slot kill by move's residual that [§28.3.9](#sec-consume-site-freeze) *Scope* clause 1 states apart.

**K4, the second example.** *Replace from* `*Example.* A plain field that holds a parameter's borrow is admitted on the field-held trust clause (K4.3)` *through the closing fence of that example with:*

> ▲ *Example.* A plain field that holds a parameter's borrow makes its class borrow-bearing; the instance carries the argument, and no trust clause is recorded ([§28.15](#sec-borrow-bearing-classes)):
>
> ```bezel
> class Tag
>   private _owner: Widget;           // a borrow field: `Tag` is a borrow-bearing class
>
>   public constructor(owner: Widget)
>     _owner := owner;                // allowed: a constructor stores into the object it builds
>   end constructor;
> end Tag;
>
> var a := new Widget("a");
> using var t := new Tag(a);          // `t` carries `a`
> var b := move a;                    // error (K5.1, K5.5): `t` is a live instance that carries `a`
> ```

**K5.5.** *In the sentence containing* `'s holds-borrows property), and locals — a `for` desugaring's hidden `__source``*, insert after* `'s holds-borrows property),`:

> ▲  locals that hold a value of a carrier type ([§28.15](#sec-borrow-bearing-classes)), each a live local borrow of every source the value carries, its release counting as a use where it may run user code,

**K8.4.** *Replace the sentence beginning* `A plain parameter announces none: a callee that stores it holds the borrow under the borrowed-field rule's` *and ending* `at its boundary (K8.1).` *with:*

> ▲ A plain parameter announces none, and its callee may not keep it ([§28.15](#sec-borrow-bearing-classes) *The callee side*) — a constructor's excepted: a constructor call of a borrow-bearing class is a mint, the constructed instance carries the argument, and a fresh unbound owned temporary there is rejected unless the instance is itself consumed within the full expression.

### 8.4 Spec §28.3.5, *The sources an argument contributes*, and a capture's [S3]

*Replace* `1. a class- or interface-typed argument contributes its source set;` *with:*

> ▲ 1. a class- or interface-typed argument contributes its source set, and, where its value is an instance of a borrow-bearing class ([§28.15](#sec-borrow-bearing-classes)), the sources the instance carries;

*Replace* `5. an `owned` argument, and a `null`-literal, other value-typed or other runtime-managed argument, contributes nothing.` *with:*

> ▲ 5. an `owned` argument contributes no borrow of itself; where it is of a carrier type — a move-only value argument of one included — it contributes the sources it carries ([§28.15](#sec-borrow-bearing-classes) *The source set*, clause 2). A `null`-literal, other value-typed or other runtime-managed argument contributes nothing, and so does a borrow of static storage.

**§28.3.8, the closure bullet's provenance clause 1 [S3].** *After* `a captured function value its own set, transitively` *insert:*

> ▲ ; a captured local or parameter whose value is an instance of a borrow-bearing class ([§28.15](#sec-borrow-bearing-classes)) the sources the instance carries, beside the binding's own

### 8.5 Spec §28.3.8 and §28.3.12 [S1 for clause 4; S3 for the rest]

**The field-store bullet [S3].** *Replace from* `- A borrow may be stored in a field of an object only if the borrow's source provably outlives that object. In clauses:` *through* `upgrades the admission to a fully static bound at participating call sites.` *with:*

> ▲ - A borrow may be stored in a field of an object only where the object carries the borrow's sources ([K2](#sec-kernel-k2) clause 5). In clauses:
>   1. **The field.** A non-owning class- or interface-typed field — the `_owner: Customer` back-reference of [§28.5](#sec-owned-fields) — is a **borrow field**; it stores a plain reference, it is the back-edge serialization synthesis treats specially ([§9.8](#sec-serialization-synthesis)), and it makes its class borrow-bearing ([§28.15](#sec-borrow-bearing-classes)).
>   2. **The check.** The store is admitted where [§28.15](#sec-borrow-bearing-classes)'s *The callee side* admits it: a plain parameter's borrow into the object under construction or a purely local instance, a `scoped` parameter's or a setter's `value` into `this`, a receiver-sourced borrow into storage `this` owns. A borrow of a scope-bound local, of an `owned` parameter — which the method disposes at its own end (*Auto-drop (affine)* below) — or of a temporary is rejected into a field of `this`. A borrow returned by **invoking a function value** is sourced as [§28.3](#sec-argument-contributions) counts it, and is admitted exactly where each of those sources is.
>   3. **No residual.** The store records no trust: at each caller the instance carries the stored borrow's sources, and the rules that bound a borrow bound the instance ([§28.15](#sec-borrow-bearing-classes) *The bound*). The caller's check reads the callee's signature only, for a local, virtual, interface or imported callee alike.
>   4. **The domain clause.** Where a recorded trust clause does admit a borrow — a pending frame's ([§30.1](#sec-async-methods)), or a surviving borrow of a slot-displaced value ([§28.3.9](#sec-consume-site-freeze) *Scope* clause 1) — the obligation includes not transferring the source's object to another domain, and not consuming it into a construction that makes it reachable from other domains: `FrozenArray<T>.From(move …)` (stdlib §4.19), a `Shared<T>`, `Locked<T>` or `RwLocked<T>` constructor whose result is then shared across domains ([§28.13](#sec-shared-ownership), [§30.12](#sec-data-owning-locks)), or a store into an admitted static ([§22.12](#sec-static-members)). Checked disposal mode does not catch a violation: the object stays alive in the receiving domain, so no flag is set; it is a data race. An instance of a borrow-bearing class needs no such clause: a transfer of a source it carries is frozen while it is live ([K5.5](#sec-kernel-k5)).
>   5. **Reach.** [§28.11](#sec-borrowing-collections)'s store into a borrowing collection and a standard collection's retained comparer are stores under this bullet: the collection is an instance of a borrow-bearing class and carries what it is given.

*The two examples that follow the bullet list (*"The field-store bullet: a plain parameter's borrow may be stored…"*) are revised with it: `Attach(i: Item) => _item := i` becomes an error and `Attach(scoped i: Item)` the admitted form.*

**The report, clause 1 [S3].** *Replace from* `1. **The clauses:** the **field-held** clause` *through* `each stated at its home.` *with:*

> ▲ 1. **The clauses:** the **frame-held** clause ([§30.1](#sec-async-methods), [§30.5](#sec-result-lifetime-and-task-disposal)), and [§28.14](#sec-scoped-callback-parameters)'s registration-lifetime and quiescence residuals, each stated at its home.

**The report, a new clause 4 [S1].** *Insert before* `The report is informative: it changes no verdict and no generated code, and outside the mode it is not emitted.`:

> ▲ 4. **The static classification.** At each field-held, collection-held and comparer-held line the warning also states the classification [§28.15](#sec-borrow-bearing-classes) gives the store: **bounded**, where that section admits the store as written; **needs `scoped`**, where a member that is not a constructor keeps a plain parameter, naming the parameter; or **no static form**, naming the reason — the store relates two parameters, an `Object` parameter is kept by a member that is not a constructor, or the holder is static storage or is reached through a borrow. The mode also emits one warning at each site that is not a store and that [§28.15](#sec-borrow-bearing-classes) rejects, naming the rule. A compilation unit that draws no "needs `scoped`" line, no "no static form" line and no such warning is one that section admits unchanged.

*At S3 clause 4 is deleted with the three clauses it annotates.*

**The closing paragraph [S3].** *Replace the sentence beginning* `For *field-held* and *collection-held* borrows (` *and ending* `backstopped by checked disposal mode.` *with:*

> ▲ For a borrow an object stores in a field or a collection, liveness is enforced through the instance that carries it ([§28.15](#sec-borrow-bearing-classes)), to the same extent: statically up to an explicit disposal of a source, and by checked disposal mode past it.

### 8.6 Spec §28.11, *Storing a borrow* [S3]

*Replace from* `**Storing a borrow (derives: [K2.3](#sec-kernel-k2)).**` *up to, not including,* `**Value-copy members.**` *with:*

> ▲ **Storing a borrow (derives: [K2.3](#sec-kernel-k2) + [K2](#sec-kernel-k2) clause 5).** A borrowing collection at a class or interface element type is an instance of a borrow-bearing class ([§28.15](#sec-borrow-bearing-classes); stdlib [§1](Bezel_Standard_Library.md#sec-borrow-bearing-statements)), and a store of a class-typed borrow into it is a later store: it adds the element's sources to the collection's source set and is checked as an assignment to the location that holds the collection. The same reading reaches the key slot of `OwningDictionary<K, V>`, a borrowing position ([§15.4](#sec-collection-literals)).
>
> 1. A collection a **local** of the body holds, or an `owned` parameter — the body's own, read as a local of its outermost block ([§28.3](#sec-borrow-lifetime-rules)'s parameter-scope sentence) — takes an element whose sources are declared in the collection's scope or an enclosing one; a borrow of a binding of narrower scope is rejected at the store. The collection then carries the element's sources: a transfer or reassignment of one is frozen while the collection's local is live ([K5.5](#sec-kernel-k5)), and a returned collection carries them to the caller.
> 2. A collection **`this` owns** — an `owned` field, or one reached through `owned` hops — takes what [§28.15](#sec-borrow-bearing-classes)'s *The callee side* admits into storage `this` owns: a plain parameter's borrow in a constructor, a `scoped` parameter's in any member, a receiver-sourced borrow in any member.
> 3. A collection held by a **parameter that is not `owned`**, reached through a borrow, or held by static storage takes no borrow but a static-storage-sourced one: the body cannot see the binding that holds it. Store at the caller, or make the object that owns the collection the receiver of a member with a `scoped` parameter.
> 4. A **fresh unbound owned temporary** in a single-element store position is rejected by [§15.4](#sec-collection-literals)'s fresh-store rule, as before; the rule covers the key position of `OwningDictionary<K, V>`.
>
> No residual is recorded. A value that holds borrows copied into a collection is bounded by [§9.7](#sec-records-and-disposal)'s collection bullet, whose scope comparison this rule now shares; a kill of a source it carries is frozen through the collection's local as for a class-typed element.

### 8.7 Spec §28.14, scoped construction [S2 for the widening; S3 for the callee-side clause on a plain argument and for the closing paragraph]

**Callee side.** *Replace from* `- **Callee side:** the callee may store the borrow in a direct non-owned field of the bounding object` *through* `remains legal with the trust residual.` *with:*

> ▲ - **Callee side:** the callee may store the borrow in storage the bounding object owns — a borrow field of the bounding object (a primary-constructor parameter's compiler-generated capture field is such a field, [§22.10](#sec-capture-rule-and-layout-stability)), or an instance placed in one of its `owned` fields or owning slots — and may forward it to another `scoped` parameter with the same bounding object. It may not return it or store it anywhere else; passing it as a plain argument is an ordinary borrow argument, which the receiving callee may not keep ([§28.15](#sec-borrow-bearing-classes) *The callee side*). An instance in an `owned` field may hold it because an owned value taken out of the bounding object carries the bounding object ([§28.15](#sec-borrow-bearing-classes) *The source set*), which is not so of a callback. No new field kind is introduced: `scoped` on the *parameter* is what lets a member that is not a constructor keep its argument.

**Caller side.** *Replace the sentence fragment beginning* `and is admitted under the bounding-object forms above with *the argument's borrow source*` *and ending* `and it is frozen until the bound's scope exits.` *with:*

> ▲ and is a later store ([§28.15](#sec-borrow-bearing-classes) *The source set*, clause 2): it adds what the argument contributes to the constructed object, or for any other call to the receiver, and is checked as an assignment to the location that holds that instance — any location that section traces, a plain `var` local and an `owned` field of `this` among them. Where the bounding object resolves, by the forms above, to a `using var` local or to `this`, the argument's source is in addition frozen against explicit disposal until the bound's scope exits ([K5.7](#sec-kernel-k5)), as a captured source is; elsewhere an explicit disposal of the source is a site-announced kill ([K4.2](#sec-kernel-k4)), and a use of the instance it dominates is [§28.2](#sec-checked-disposal)'s error.

**The closing paragraph.** *Replace from* `The asymmetry with ordinary borrows is deliberate:` *through* `(rationale [§5.25](Bezel_Design_Rationale.md#sec-deferred-activation-capture)).` *with:*

> ▲ The two halves of this section now agree. A plain parameter is call-bounded for a callback and for an object borrow alike, and `scoped` is the signature-carried announcement that a member keeps its argument. One store needs no announcement for an object borrow and has none for a callback: a constructor's, whose call mints the instance that carries the argument ([§28.15](#sec-borrow-bearing-classes)); a callback has no such carrier outside a `scoped` field, since a function value's set is not read off a class. Frame residence (*Call-bounded × deferred activation* above) is not a store but the retention the deferred-activation return type itself announces (rationale [§5.25](Bezel_Design_Rationale.md#sec-deferred-activation-capture)).

**At a type parameter (new).** *Insert after the bullet beginning* `- **Not at `Object`:**`:

> ▲ - **At a type parameter:** a parameter declared at a bare type parameter may be declared `scoped`. At an instantiation that substitutes a class or interface type other than `Object`, it is a class-typed `scoped` parameter. At any other substitution the modifier has no effect: a value or a runtime-managed argument carries no borrow to bound, and at `Object` the member's store is one no member but a constructor may make, so the instantiation is rejected where it is named. The call-site marker ([§22.3](#sec-parameter-modifiers)) is written wherever the argument's static type is a class or interface type or a type parameter.

*§22.3's sentence on the class- or interface-typed `scoped` parameter gains the same case. The "Not at `Object`" bullet stands. The note's ruling S1 — whether `_child := new Child(scoped this)` resolves form 1 or form 3 — needs no answer: for a class-typed argument a constructor call is a mint and resolves no bounding form.*

### 8.8 Spec §28.2, §9.7, §30.1, §30.14, Appendix C; stdlib [S3]

**§28.2.** *In the sentence containing* `a closure that captures such a holder, or an iterator, enumerator or lazy-query frame that carries it`*, replace that phrase with:*

> ▲ a closure that captures such a holder, a value of a carrier type that carries it ([§28.15](#sec-borrow-bearing-classes)), or an iterator, enumerator or lazy-query frame that carries it

*and append to that sentence's list of uses (*"…enumerating or disposing the frame"*):* ▲ `, releasing an instance whose release is a use`.

**§9.7, the instance-field bullet.** *Replace the sentence beginning* `A class instance's lifetime is not bounded by any borrow source's scope, and a **direct** class-typed borrow field` *and ending* `flag as its backstop.` *with:*

> ▲ A **direct** class-typed borrow field makes its class borrow-bearing, and its instances carry the borrow's sources ([§28.15](#sec-borrow-bearing-classes)); a record-carried borrow stays out of class fields on this section's own posture.

*(This proposal does not lift the instance-field ban for records or spans; `DECLINED.md`'s deferred row for that retirement stands, with its bar.)*

**§30.1.** *Insert before* `*Example.* Awaiting before the source's scope ends discharges the settlement obligation; abandoning the task leaves it to the caller's trust:`:

> ▲ **Values that carry sources, in a frame (derives: [K6](#sec-kernel-k6) + [K2](#sec-kernel-k2) clause 5).** An argument or capture of a carrier type ([§28.15](#sec-borrow-bearing-classes)) contributes its binding's source and the sources it carries to the frame, and an `owned` one the sources it carries; the obligation above — its domain clause included — reaches each. A local of a carrier type built in an `async` body may be live across a suspension point, whatever it carries: a borrow formed in the body is sourced at a location the frame owns or at one of the frame's own carried sources, so the value holds nothing the frame does not already hold, and the rule above for a value type that holds borrows is not extended to it. The value an `await` yields, where the task's result type is a carrier type, carries the task's carried sources.

**§30.14.** *Append to the sendability rules' statement on class fields:* ▲ `No carrier type ([§28.15](#sec-borrow-bearing-classes)) is sendable.` This changes one verdict only: a class that is borrow-bearing by a store member alone. For every other carrier type the existing rules already say so — a class with a borrow field or an `owned` field of a non-sendable class (the note's probe p16), an interface, an owning choice or inline array over a non-sendable class, a borrowing collection at class elements. It does **not** reach `Dictionary<string, int>`, `OwningDictionary<string, Session>` or `Locked<Dictionary<string, int>>`: under §4.5 those are not carrier types, and their sendability is untouched. (The first draft made them carriers at every instantiation and so made them unsendable; that was the regression of critique finding 6.)

**Appendix C.3.** *Insert after the bullet beginning* `- A choice that holds borrows ([§35.4](#sec-choice-classification)'s property, whatever the choice's classification) is subject to`:

> ▲ - A value of a carrier type — an instance of a borrow-bearing class, or a move-only value that owns one — carries the sources of the borrows it stores: it is bounded by scope, by the no-temporary-source rule and by the live-local freeze, it does not reach an owning position whose declared type is not a carrier type, and a member that is not a constructor keeps a plain parameter only where it is declared `scoped` ([§28.15](#sec-borrow-bearing-classes); [K2](#sec-kernel-k2) clause 5 + [K5.5](#sec-kernel-k5)).

**Stdlib §1 [S1; its sentence on rejection at the call lands at S3, with R5].** *Insert after the sendability-statements paragraph (anchor `sec-sendability-statements`):*

> ▲ `<a id="sec-borrow-bearing-statements"></a>` **Borrow-bearing statements** (language spec [§28.15](Bezel_Language_Spec.md#sec-borrow-bearing-classes)). The standard-library types that are carrier types:
>
> - a **borrowing** collection, at an instantiation whose element type — for a map, its key or value type — is a class or interface type that is not runtime-managed;
> - an **owning** collection, `Shared<T>` and `Weak<T>`, at an instantiation whose owned type argument is a carrier type;
> - `Result<T, E>`, and every other standard-library choice or owning inline array, by the language's own rule: where an `owned` payload or slot is of a carrier type.
>
> No other standard-library type is a carrier type; `Locked<T>` and `RwLocked<T>` require a sendable interior, which no carrier type is. Where a member of a type that is not a carrier at its instantiation takes `owned T`, an argument whose source set is not empty is rejected at the call; an argument that carries nothing is admitted. `Task<T>` and `OwningTask<T>` are frames: the value an `await` of one yields carries the frame's sources (language spec §28.15).
>
> **Results that carry the set alone.** The `owned` result of a member that takes an element out of a collection — `Drain()`, the `Pop*` family, the replacing `move` of a slot, an owning iteration's elements — and of `Shared<T>.Clone()`, `Shared<T>.Downgrade()` and `Weak<T>.TryUpgrade()` carries the receiver's source set and not the receiver's binding: a standard collection gives no element a borrow of itself, and the handles to one shared value do not borrow one another. The store positions are those of language spec §28.11's *Value-copy members*.

**Stdlib §4, *A retained comparer is a borrow* [S3].** *Replace from* `**A retained comparer is a borrow.**` *through* `The comparerless constructors retain nothing.` *with:*

> ▲ **A retained comparer is a borrow.** The `comparer` constructors of `Dictionary<K, V>`, `OwningDictionary<K, V>`, `Set<T>` and `OwningSet<T>` ([§4.8](#sec-dictionaryk-v)–[§4.10](#sec-sett-and-ownedsett)) take a plain parameter and retain it: every later lookup consults it. At an instantiation that is a carrier type ([§1](#sec-borrow-bearing-statements); language spec [§28.15](Bezel_Language_Spec.md#sec-borrow-bearing-classes)) the constructed collection carries the comparer's sources: it may not outlive the comparer, and the comparer may not be transferred while the collection is live. At any other instantiation — every sendable one among them — the comparer must contribute no source: a borrow of static storage, as a `static readonly` instance is. Any other argument is a compile error at the construction, because the collection's type could not announce what it would hold; a parameter that forwards a comparer is such an argument, whatever its callers pass. In a generic body the requirement is read at each instantiation (language spec [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties) rule 2), and an instantiation that fails it is rejected where it is named. A key equality that needs no comparer object is a key record that declares its own `Equals` and `GetHashCode` — the form [§4.17](#sec-sorted-dictionary) requires for an ordering — and it is admitted at every instantiation. The comparerless constructors retain nothing.

### 8.9 Rationale [S3, except §5.32, which lands at S1]

**§4.2.6.** *After the sentence fragment* `a callee-side no-store rule for every `Object` or interface parameter, which retires the back-reference idiom;` *append:*

> ▲ (reopened by [proposal 0212](proposals/0212-borrow-bearing-classes.md): the rule landed for members that are not constructors, for every class- or interface-typed parameter, once the constructor's store was given a carrier — the idiom's named form, `constructor(logger: ILogger) _logger := logger;`, is untouched; [§5.32](#sec-borrow-bearing-classes))

**§5.1.** *Replace the paragraph beginning* `**Where neither applies, the trust is recorded.**` *through its sentence ending* `keeps the set closed.` *with:*

> ▲ **Where neither applies, the trust is recorded.** An admission the checker can neither compare statically nor backstop by flag is either rejected or carries an individually recorded trust clause (K4.3). The case is the domain clause: a borrowed-from object transferred to, or made reachable from, another domain stays alive, so no flag is ever set. The recorded clauses are the frame-held borrows and spec §28.14's two residuals. A borrow an object stores is not among them: the plain back-reference field (Guide 01 §5) makes its class borrow-bearing, and the instance carries the borrow's sources ([§5.32](#sec-borrow-bearing-classes)). These clauses are the model's honest limit, and the rule that no admission is made without recording its clause or its backstop keeps the set closed.

*(The rest of that paragraph, on the planned work to shrink the set, is revised to cite this section and the frame clause as what remains.)*

**§5.31.** *In the first paragraph, replace* `field-held, collection-held, comparer-held ([proposal 0138](proposals/0138-compiler-known-sendability.md) §7) and frame-held borrows, plus §28.14's two residuals` *with* ▲ `frame-held borrows, plus §28.14's two residuals — the field-held, collection-held and comparer-held clauses having been retired by spec §28.15 ([§5.32](#sec-borrow-bearing-classes))`.

**§5.32 (new) [S1].** *Insert before* `## 6. Nullability`:

> ▲ `<a id="sec-borrow-bearing-classes"></a>`
> ### 5.32 Borrow-bearing classes: an object that stores a borrow carries its sources
>
> Three kinds of value that carry a borrow were always read with a source set: a record that holds one, a function value, a frame. A class instance with a plain class-typed field was the fourth and was left on trust (spec K4.3's field-held clause): its stored borrow was the programmer's promise, backstopped by the disposed flag, and not backstopped at all against a transfer to another domain. [Proposal 0212](proposals/0212-borrow-bearing-classes.md) reads the field as what it is, a declaration that the class holds a borrow, and gives the instance the set (spec §28.15).
>
> **The mint reads the signature.** A constructor call carries every argument, stored or not. The alternative — a marker on each stored parameter — is more precise and was measured: it would add a `scoped` to 53 signatures in the accepted corpus where the signature reading adds 12, to buy one construction site in 312. The signature reading is K6.2's, the rule a frame already uses.
>
> **The callee side.** A plain parameter is call-bounded for an object borrow as it is for a callback. v0.329 declined this rule on the ground that it "retires the back-reference idiom" (§4.2.6). It does not, once the constructor's store has a carrier: the idiom's named form is a constructor, and 93 of the 157 measured stores are one. What changes is a member that is not a constructor, which says `scoped`.
>
> **No opt-out.** A per-field marker that keeps the old admission was declined: it would keep the clause, and the domain race with it, under another name. The reopening bar is a measured count of stores with no static form in real programs (`DECLINED.md`).
>
> **Erasure only at owning positions.** A borrow of an instance may be passed at `Object` or an interface, because what it contributes is read from the argument at the caller. An *owned* instance may not be converted to a type that announces nothing: Rust's `Box<dyn Trait>` defaulting to `'static`, Swift's `~Escapable` opt-in and C# 13's ban on converting a `ref struct` to an interface draw the same line. In a generic body the conversion is a requirement read per instantiation, the mechanism of §9.7's bullets.
>
> **The owning positions are the consume table's.** A set can be lost only where an owned value changes holder, and spec K3 already enumerates those places. The proposal's first draft listed them by hand and missed four — an `out owned` recipient, an owning choice's payload, an `owned` argument to a static callee, a replacing `move` written outside a member — which an adversarial critic found with running programs. Reading the table row by row is what replaced the list.
>
> **A sendable type is never a carrier.** The collections that retain a comparer are sendable at value instantiations, so at those the comparer must be static-sourced; checking each value at a domain transfer instead was declined, because it would let a sendable type's values carry sources and stop `where T: sendable` from meaning what the erasure rule needs (`DECLINED.md`).
>
> **What was not done.** Lifetime parameters (the precision ceiling; nine measured sites over this design, at the price of a second parameter system), a freeze of explicit `Dispose()` under a live instance (§5.1's recorded boundary stands), a ban on destructors reading a stored borrow (it retires flush-on-dispose and is not complete), and the retirement of the frame-held clause, which remains recorded trust.

### 8.10 `DECLINED.md` [S3 for the annotations; the new rows land with acceptance]

Annotations, appended to the named row's last cell:

| row (by its topic) | annotation |
|---|---|
| *A callee-side rule forbidding the store of every plain `Object` or interface parameter…* | ▲ *(v0.E, [proposal 0212](0212-borrow-bearing-classes.md): reopened on new evidence and landed in a narrowed and widened form — every class- or interface-typed plain parameter, in members that are not constructors. The stated reason, that the rule "retires §28.3's back-reference idiom", is answered by the mint: a constructor's store is carried by the constructed instance, so the idiom's named form is untouched. The second half of this row — a rule keyed on an interface's implementers — stays rejected.)* |
| *Rejecting an unbound temporary or a scope-bound local passed to a plain parameter because the callee's body stores the parameter* | ▲ *(v0.E, proposal 0212: this row stands. A fresh temporary at a constructor of a borrow-bearing class is rejected because the constructed instance carries every argument — read off the class's derived property and the call's signature, never off a body.)* |
| *A caller-side warning when a fresh temporary reaches a plain parameter that the callee's body stores* | ▲ *(v0.E, proposal 0212: superseded, not reopened — a member that is not a constructor may not store a plain parameter, and a constructor's argument is carried, so no body-derived fact is needed.)* |
| *An always-on trust-admission warning (the report mode in the default build)* | ▲ *(v0.W, proposal 0212: this row stands. The stage-2 warning fires only at sites a scheduled rejection will reach — a Ratchet's ramp — not at every admission.)* |
| *`scoped` on a parameter declared `Object`* | ▲ *(v0.E, proposal 0212: this row stands; a member that is not a constructor therefore cannot keep an `Object` parameter, and declares it at the stored type.)* |
| *Retiring §9.7's instance-field ban for buffer- and array-backed spans…* | ▲ *(v0.E, proposal 0212: this row stands. A span field would now make its class borrow-bearing, so the retirement is sound to propose; the bar is unchanged.)* |

New rows (`declined-at-proposal`; authoritative record: this proposal, the cited section). D1 is recorded *provisionally*: Q2 is ruled at the stage-3 gate, on the stage-1 count. D8 is an *open refinement*, not a decline of the idea: Q16 asks when to rule it:

| id | topic | reason | reopening bar |
|---|---|---|---|
| D1 | A per-field marker that keeps the field-held trust admission for that field (§4.3) | It keeps K4.3's clause, and the domain race no flag catches, under another name; every measured store has a static rewrite. | A measured count, from the stage-1 report over programs that are not tests, of stores classified "no static form" that the rewrites of §6 do not express. |
| D2 | A `scoped` parameter that names another parameter as its bounding object (§4.2) | The receiver form, or a store at the caller, expresses every measured site (9), and the marker is the first step of a second parameter system. | A measured helper that must be a static method and whose rewrite as an instance or extension member is not available. |
| D3 | A declared borrow-bearing interface, for interfaces whose implementations store only in constructors (§4.1) | §3.6 rule 1: a derived property is not declared; 0 measured owned upcasts; the concrete return type is the rewrite. | A measured program that must return an owned borrow-bearing instance at an interface with no store member. |
| D4 | Rejecting every generic instantiation at a borrow-bearing class unless the generic opts in (§3.1) | It needs a new constraint form and rejects generics that keep their parameter, which are sound. | —(none stated) |
| D5 | A rule that a destructor may not read a borrow field; a rule that a self-sourced holder be trivial-dispose (§3.2) | The first retires flush-on-dispose and is incomplete; the second rejects every tree with parent pointers and does not bound a subclass. | —(none stated) |
| D6 | Importing §30.1's ban on a holds-borrows local across a suspension for a class instance (§3.3) | The frame already carries every source such an instance can hold; the plain alias beside it is admitted. | The retirement of the frame-held clause itself (a strict mode). |
| D7 | Keeping the comparer-retaining collections carriers at every instantiation and rejecting, at each domain transfer, a value whose set is not empty (§4.5) | It makes a sendable type's values able to carry sources, so `where T: sendable` stops answering R5's question, and adds a value-level check at every spawn and send. | A measured program that needs a comparer with per-instance state at a sendable instantiation, or that must forward a comparer through a constructor and cannot key by a record (§4.5). |
| D8 | Reading the taken-out rule off declarations: an owned value taken out of an instance carries the holder's binding only where its type can reach, through borrow fields, a type the holder's class or something it owns converts to (§4.7) | As worded it is unsound for a class that is not `sealed`: a subclass adds the borrow field the declared type does not show (probe e02). The closed form — every class involved `sealed` — is sound in the model and removes 96 of 17,940 rejections at one bound, but it does not relieve the unsealed builder that motivated it and adds a second derived property to the artifact interface. | A count, from the stage-1 report over programs that are not tests, of taken-out values rejected for carrying a holder they cannot reach; and a decision on whether `sealed` is an acceptable price for the relief. |

The note's candidates C (lifetime parameters) and D (freezing explicit disposal) are covered by existing rows — *Compile-time borrow-liveness checking* stands — and need no new row.

### 8.11 Guides 01 and 05 [S3; each pin moves to the landing revision]

**Guide 01.**

**§3.** *Replace the paragraph beginning* `A stashed reference is not proved safe.` *with:*

> ▲ A method that wants to keep what it is passed has to say so. A plain parameter is yours for the call only: `PrintSummary` may read `account` and pass it on, and may not put it in a field. A constructor may — that is §5's back-reference — and any other method declares the parameter `scoped` (spec §28.14, §28.15). Either way the object that keeps the reference *carries* it: the compiler knows `stmt` depends on `acct`, will not let `acct` be moved or reassigned while `stmt` is in use, and rejects a `stmt` that would outlive it.

**§5, the bullet beginning** `- **Back-references are not `owned`.**` *— replace with:*

> ▲ - **Back-references are not `owned`.** `_owner` points *up* to something that outlives this object; this object did not create it and must not dispose it. The compiler checks the "outlives": a `BankAccount` built over a `Customer` carries that customer, so `var acct := new BankAccount(customer);` followed by `Take(move customer);` is rejected while `acct` is still used (spec §28.15).

**§5, from the paragraph beginning** `**The back-reference's guarantee is yours — unless you say `scoped`.**` **through the paragraph ending** `never returned or stored in another object (spec §28.14).` *— replace with:*

> ▲ **Who may keep a reference.** A *constructor* stores its plain parameter with nothing more to write, as `BankAccount` does above. A *method* that keeps its parameter marks it `scoped`, and so does the call:
>
> ```bezel
> class Statement
>   private _account: BankAccount? := null;              // a plain field: `Statement` holds a borrow
>
>   public method Attach(scoped account: BankAccount)
>     _account := account;                               // OK: `scoped` announces that this method keeps it
>   end Attach;
> end Statement;
>
> method PrintStatement(customer: Customer)
>   using var acct := new BankAccount(customer);         // the source: declared BEFORE the holder
>   using var stmt := new Statement();
>   stmt.Attach(scoped acct);                            // `stmt` now carries `acct`
> end PrintStatement;                                    // stmt disposed first, then acct
> ```
>
> Four shapes the compiler rejects, each with a one-line remedy. *A holder that outlives its source*: `stmt` declared in an outer block, `acct` in an inner one — declare the source first. *A fresh temporary as the source*: `new BankAccount(new Customer("x"))` bound to a local — bind the customer first, or let the class own it with an `owned` parameter. *A source moved while the holder is in use* — use the holder first, or scope it in a `do … end` block. *A holder handed out at a type that hides the borrow*: returning `owned IReport` built over a parameter — return the concrete class.
>
> An explicit `acct.Dispose()` while `stmt` is alive is allowed, as it is under any borrow; a use of `stmt` that always follows it is a compile error, and one that follows it on some paths is checked mode's `DisposedAccessException`. A registry that must outlive the things it refers to does not hold borrows of them: it owns them, keys them, or observes them through `Weak<T>` (spec §28.13). And an observer does not hand `this` to a bus in its constructor — `bus.Subscribe(this)` is a store into an object the constructor does not hold, and has no checked form. It subscribes to the bus's event and keeps the `Subscription` in an `owned` field, so that its own disposal ends the registration (stdlib §6.5).

**Guide 05, §1.** *Insert after the paragraph beginning* `**Dictionaries in practice.**` *(it ends* `checked against the dictionary at every call as `Add`'s arguments are (spec §28.11).`*):*

> ▲ **A different notion of key equality.** A dictionary or a set compares keys with the key type's own `Equals` and `GetHashCode`. To look keys up another way — case-insensitively, say — put the rule on the key: wrap it in a record that declares its own equality.
>
> ```bezel
> record FoldedKey(Value: string)
>   public method Equals(other: FoldedKey): bool => Value.ToUpper() = other.Value.ToUpper();
>   public override method GetHashCode(): int => Value.ToUpper().GetHashCode();
> end FoldedKey;
>
> using var ages := new Dictionary<FoldedKey, int>();
> ages.Add(new FoldedKey("Ann"), 41);
> Console.WriteLine(ages.ContainsKey(new FoldedKey("ANN")));     // True
> ```
>
> This is the rewrite the sorted collections require for an ordering (stdlib §4.17). The dictionary stays an ordinary one: it can be passed on, stored, sent to another domain or put in a `Locked<T>`. The `comparer` constructors (stdlib §4.8) are for a dictionary whose keys or values are objects, which carries its comparer as it carries them (spec §28.15). At string or value keys and values they take only a comparer held in static storage — a `static readonly` instance, which draws spec §22.12's every-domain warning — and a class cannot hand on a comparer it was given. Reach for the record first.

### 8.12 Code-generation reference [S3; the pin moves]

**§4.4.** *Replace the bullet beginning* `- A borrow of a **scope-bound local** may not be stored in a field or in a longer-lived collection.` *(its first sentence group, through* `so it is scope-bound — §28.3).`*) with:*

> ▲ - **Who may keep a borrow.** A **constructor** may store a plain class- or interface-typed parameter in a plain field (the back-reference: `private _owner: Customer;`). Any **other member** may store one only if it is declared `scoped`, with `scoped` at the call too. `this` and its own fields may be stored into storage `this` owns. A borrow of a scope-bound local, a temporary or an `owned` parameter is never stored into a field (§28.3, §28.15).
> - **The object that keeps it carries it.** `new C(a, b)` of a class with a plain class-typed field carries `a` and `b`; so does the result of a call returning `owned C`. While that object's local is in use, `a` and `b` may not be moved or reassigned; the object may not be assigned to a wider scope than theirs; a fresh `new …` argument must be bound first unless the parameter is `owned`; and the object may not be returned or stored as `owned Object` or an `owned` interface that has no `scoped` member (§28.15).

**§4.6.** *Add one row to the diagnostics table:* ▲ `| "a member that is not a constructor keeps the plain parameter `p`" | declare `scoped p: T` and write `scoped x` at each call; or make the storing object own `p` |`.

**§7.2.** *Add three* Looks right, is rejected *pairs:* a method storing a plain parameter (remedy: `scoped`); a static helper storing one parameter into another's collection (remedy: the receiver form); a factory returning `owned IReport` over a parameter (remedy: the concrete type).

### 8.13 Consequential mentions [S3]

The clauses retired at stage 3 are restated or cited in other places. `0212/check_anchors.py --mentions` lists each occurrence with its line (MEASURED counts at this base):

| phrase | spec | stdlib | rationale | code-gen | Guide 01 |
|---|---|---|---|---|---|
| "field-held" | 13 | 2 | 4 | 2 | 1 |
| "collection-held" | 8 | 0 | 7 | 0 | 0 |
| "comparer-held" | 2 | 1 | 4 | 0 | 0 |
| "borrowed-field" (the rule's old name) | 11 | 0 | 2 | 0 | 0 |
| "recorded residual" / "under its residual" / "under the residual" | 16 | 1 | 1 | 1 | 0 |

**Example blocks whose verdict changes** (MEASURED: the doc-extracted blocks that hold a K4.3 store, `data/out/summary_0212.txt`). Three blocks hold a store that stage 3 rejects as written; each sits in a block that already has rejected lines, and each takes `scoped`:

| extracted block | document, section | the store | at stage 3 |
|---|---|---|---|
| `spec_0244` | spec §28.3.8, the field-store example | `Tag.Attach(i: Item) => _item := i` | `Attach(scoped i: Item)`; the comment "admitted (clause 2)" is rewritten (§8.5 says so) |
| `spec_0251` | spec §28.3.11, *Unbound owned temporaries* | `Holder.Hold(…)` storing its plain parameter | `scoped`, and the passage on a temporary passed to a storing callee is restated: the call is rejected, not admitted with a fault |
| `tour_0012` | Tour, *Borrows cannot outlive their owners* | `Cache.Remember(…)` storing its plain parameter | `scoped`, with the marker at the call |

Eight blocks hold a store that stays admitted as written — a constructor's or an initializer's — and need only their comment or surrounding prose changed where it cites the trust clause: `spec_0226` (the K4 example, replaced in §8.3), `spec_0264`, `guide1_0006`, `guide1_0007`, `codegen_0040`, `codegen_0080`, `stdlib_0023`, `tour_0011`. One standard-library listing needs a sentence: `TypeInitializationException` declares `InnerException: Exception?`, which reads as a borrow field; its entry should say who holds that exception. No doc-extracted block is rejected as a whole program.

The edits of §8.3–§8.12 cover the defining occurrences. The remaining ones are of four kinds, each with one instruction, and are composed by the stage-3 train against its own head (process §4: a train re-verifies every anchor before composing): (a) a citation of the field-held clause as the reason a class-typed borrow is admitted somewhere — re-cite §28.15; (b) "admitted under the residual" in a worked example (`Keep<Item>`, `Bag<Item>`, `_all.AddRange(items)`) — state the §28.15 verdict: a constructor or `scoped` store is admitted, a plain method store is not; (c) the report's description of what it names — drop the three clauses; (d) §9.7's collection bullet, whose "collection-held clause" for a record-carried borrow in a local collection becomes the collection's carried set. §28.3.9's *Scope* clause 1 and the slot-displaced entry (proposal 0210) are untouched: they keep the domain clause through the re-homed clause 4 of §8.5.

### 8.14 Changelog and Migration (sketch for the stage-3 entry)

- **Newly rejected:** a member that is not a constructor storing a plain class- or interface-typed parameter (write `scoped`); a store into an object the member does not hold (store at the caller, or the receiver form), an `owned` parameter that carries sources handed to another parameter's object included; a transfer or reassignment of a source under a live value of a carrier type; such a value — a class instance, an `out owned` recipient, an owning choice or inline array that owns one, a child taken out of its owner — bound in a wider scope than a source; one built over a fresh temporary and bound; an owned value that carries sources at an owning position whose type is not a carrier type, in a generic body per instantiation; a `throw` of such a value; a comparer that is not static-sourced at an instantiation that is not a carrier; a field that borrows a sibling or `this` and is declared before what it can reach, where its release may run user code.
- **Newly admitted** (already at stage 2): a class-typed `scoped` argument against a plain `var` holder, a moved holder, and a holder placed in an `owned` field of the bounding object; `scoped` at a parameter declared at a type parameter.
- **Newly defined:** the source set of a class instance and of a move-only value that owns one; what an `out owned` recipient, an awaited result and an owning iteration's element carry; which standard-library results carry the receiver's set alone.
- **No longer warned:** the report's field-held, collection-held and comparer-held lines.
- **Ramp:** §7.3's paragraph.
- **Spec-ahead-of-impl:** §10's trackers.

---

## 9. Records this proposal answers

A change is a design change the moment it contradicts a recorded decision, and such a decision is reopened only with new evidence against its stated reasons (process §2).

**The callee-side no-store rule** (`DECLINED.md`; rationale §4.2.6; proposal 0146 §3.4). *Stated reasons* (QUOTED): the rule "retires the back-reference idiom", whose named form is `constructor(logger: ILogger) _logger := logger;`, and its reach was "every class-typed-or-interface borrow stored from a parameter across the repository, for a hazard that only a boxed value carries". *New evidence.* (1) The idiom's named form is a constructor, and under R2 a constructor's store has a carrier: it is untouched, with no marker. MEASURED: 93 of the 157 stores are constructor stores; 44 of 58 accepted-tier stores are admitted as written. (2) The hazard is not a box's: at one model bound 3,138 of 31,497 accepted programs read a dead or foreign object through a stored borrow, 840 of them a race no flag catches (MEASURED, §5.2). (3) The cost is measured: 12 accepted-tier signatures take `scoped`, 5 programs are rewritten. The half of that row that keys a rule on an interface's implementers is not reopened.

**A caller verdict from a callee's body** (`DECLINED.md`, the K8.4 row; rationale §5.24; proposal 0105 §1). Not contradicted: a caller reads a derived type property and a signature. R5's generic requirement is a body-derived fact of the kind §3.6 rule 2 already carries, confined to generic instantiation.

**`pure` is declared, not derived** (rationale §9.7), by analogy: the borrow-bearing property is structural — fields and signatures — not a fact about what a body does.

**Compile-time borrow-liveness checking; freezing explicit `Dispose()`** (`DECLINED.md`; rationale §5.1). Not contradicted: an explicit disposal of a carried source stays unfrozen. One existing exception is kept and not widened: §28.14's freeze for a `scoped` argument whose bounding object is a `using var` local or `this` (Q6).

**The holds-borrows posture** (proposal 0159; `DECLINED.md`'s deferred row on retiring the span field ban). Not reopened. The posture's sentence — a value type that holds borrows is stored past its bound at no position — becomes true of a class-stored borrow as well.

**The always-on report** (`DECLINED.md`; rationale §5.31). Not contradicted (§7.2).

**Derived retention** (proposal 0132, a study). Superseded: its caller-side half is R3; its callee-side half, a per-parameter bit computed from a body, is not needed.

---

## 10. Implementation sketch (non-normative)

Read, not edited. File and function names are at `a77e4718a`.

**Where the checks live.**

| rule | existing code it extends | what is added |
|---|---|---|
| R1 | `isBorrowBearingRecordCommon` and `isBorrowBearingRecordOrSpan` in `src/common/OwnershipFacts.cpp` compute the record property per instantiation; `isNonTrivialDisposeClass` in the same file is the class-level precedent, carried in the artifact interface | a sibling predicate over class and interface declarations, to a fixed point, with the stdlib statements in the builtin registry |
| R2, R3 (carry, scope, temporaries) | `MethodOwnership::carriedBorrowSourceOfValueExpr` and `carriedEscapeBoundSourceLiveAt` (`src/ownership/Ownership_Checks.cpp`) read the sources a record value carries | the same reading for a `new` of, or an `owned` call returning, a borrow-bearing class; later stores at `scoped` arguments, property stores and the collection store positions |
| R3 (freeze) | `MethodOwnership::checkConsumeFreeze` | a local holding an instance joins the traced holders, as a record local does |
| R3 (dominated use) | proposal 0205's holder leg — **not implemented in r40** | reads the instance's set once that leg exists |
| R4 | `MethodOwnership::checkBorrowStoreEscape` and `checkCollectionBorrowStoreEscape` admit and report the field- and collection-held stores (the `reportTrustAdmission` calls); `scopedCalleeScan` and `scopedRestrictedUse` run the callee-side check for a `scoped` parameter | the admission becomes: constructor, purely local, `scoped`, or receiver-sourced; the report call becomes the classification, then the warning, then the error |
| R5 (concrete) | `ExprTC::checkBorrowErasingPosition` (`src/sema/ExprTypecheck.cpp`) rejects a value that holds borrows at `Object` or an interface | an owning-position arm for a class instance; the set is the ownership pass's, so the check is split: the type test in sema, the non-empty-set test in the ownership pass |
| R5 (generic) | `ExprTC::checkDeferredAsTargetInstantiations` / `Resolver::checkAsTargetInstantiations` record a body's `as T` use and re-check it per instantiation (proposal 0159) | one more recorded use: a pair of types per owning conversion |
| R6 | field declaration order is available where `checkBorrowStoreEscape` sees a field path | the sibling order test |
| R7 | `MethodOwnership::checkBorrowBearingLocalsAcrossSuspension`, `checkAsyncSignatureBorrowBearing` | nothing for the suspension rule; the contribution rule in the frame's carried-set computation |
| R2, `out owned` and move-only carriers | the caller-side treatment of an `out owned` recipient as an owned production; `isBorrowBearingChoice` (`src/ownership/Ownership_Checks.cpp`) and the choice classification, which already walk payload types per instantiation | the recipient and a case constructor join the mints; the carrier predicate walks `owned` payloads and slots as well as borrow-carrying ones |
| R4, the `owned` parameter | the callee-side scan that already tracks where an `owned` parameter is consumed (the consume table's positions) | one more fact per `owned` parameter of a carrier type: an opaque source, tested by the same store check as a plain parameter's |
| the comparer rule | the `new`-expression check in `src/sema/ExprTypecheck_Exprs.cpp` that today rejects the four collections' comparer constructors as not implemented (probe c06), with their entries in `src/common/BuiltinRegistry.cpp` | when those constructors are implemented, one check at the construction: the instantiation is a carrier, or the argument is static-storage-sourced |
| `scoped` widening | `MethodOwnership::scopedBoundingLocal`, `checkScopedAdmission`, `checkScopedFreeze` | a class-typed argument is no longer rejected when no bounding form resolves; it becomes a later store |

**Cost** (HYPOTHESIS; the compiler was not built). R1 is a per-class fixed point of the kind trivial-dispose and sendable already run. R2–R3 add no pass: they extend the per-body dataflow that already carries source sets for records, closures and frames. R4 replaces an admission with a check in the same visitor. R5's generic half is one recorded pair per conversion and one evaluation per instantiation.

**Prerequisites: gaps r40 has against v0.355** (MEASURED).

1. Proposal 0205's borrow and holder legs are not implemented (the note's p07, p05; #7435, #7441). R3's dominated-use error is that rule read for an instance.
2. K5.5's frame-local freeze is not implemented (the note's p26; #5698). R3's freeze is the same code path for one more holder kind.
3. §28.12's live-instance rule (#7506) is not implemented: the flag is set when `Dispose()` begins (d01–d03, d11). Until it is, every back-pointing child with a destructor that reads its owner faults at teardown in checked mode — today as well as under 0212.
4. Smaller ones met while probing (trackers: #8424 for the two `scoped`-parameter over-rejections, #5923 for the replacing `move`, #5932 for the destructor's `Dispose()`): a `scoped` parameter may be forwarded only to another `scoped` parameter and not passed to a standard collection's store, which §28.14 calls an ordinary borrow argument (q06; the same probe shows `scoped` rejected at a type-parameter-typed parameter, which is the spec's rule today and which §8.7 changes); the replacing `move` is not implemented (g08's first form); an explicit `Dispose()` of a non-nullable owned field in a destructor is rejected as a double free, against §28.8.1 (d02's first form); a member access through a class-typed `scoped` parameter is rejected with the callback rule's message (q05).

**An order for implementation lanes.**

1. The three prerequisites above, each its own lane (0205's legs first: every later step reports through it).
2. R1 and the report's classification (stage 1). No verdict changes; it can be gated on the conformance cases of §7.4 in report mode. The report's summary can count, at no extra analysis, the non-store lines whose only carried source is the binding of a holder the value was taken out of — the count §4.7's refinement and Q16 wait for.
3. The consumer half: R2's mint and R3, as warnings (stage 2's non-store lines). It rejects nothing in the accepted tier (the note, MEASURED: 0 programs kill a source under a live holder).
4. R4's classification as warnings; the `scoped` widening (which only admits more).
5. R5 and R6 as warnings.
6. The flip to errors (stage 3), with the corpus migration of §7.4.

---

## 11. Owner questions

| # | question | recommendation |
|---|---|---|
| Q1 | Accept the direction: a class that stores a borrow is borrow-bearing, its instances carry their sources, and K4.3's field-, collection- and comparer-held clauses are retired through the three-stage ramp? | **Yes.** |
| Q2 | Reverse the callee-side decline in full for members that are not constructors, or keep a per-field opt-out that preserves the trust admission? | **In full, no opt-out — recorded provisionally, ruled at the stage-3 gate** (§4.3; the critic's recommendation, adopted). Stages 1 and 2 are the same either way; what differs is which K4.3 text stage 3 deletes. The stage-1 report's count of "no static form" stores in programs that are not tests is the evidence, and it does not exist yet. If the count says an opt-out is needed: a field modifier that exempts its stores from R4 and its class's instances from carrying what that field holds; the report then lists exactly the marked fields, and the domain clause stays for them. |
| Q3 | Does the mint read the signature (every argument carried, no marker on constructors), or must each stored parameter be `scoped`? | **The signature.** It needs 12 markers where the explicit form needs 53 (the note, MEASURED). Its price is measured: at one model bound 54 clean programs rejected and 24 certain faults left to the flag that the explicit form would reject; at a larger one 1,404 and 576 (§5.2 item 5). |
| Q4 | Is an interface borrow-bearing by derivation from its store members, by declaration, or never? | **By derivation** (§4.1). A declared form is declined with a bar. |
| Q5 | Does a store that relates two parameters get a spelling? | **No**; the storing object is the receiver, or the store is made at the caller (§4.2). |
| Q6 | A class-typed `scoped` argument today freezes explicit disposal of its source (K5.7). Keep that where the bounding object is a `using var` local or `this`, and use the unfrozen posture (K4.2) for the locations 0212 newly admits — or one posture everywhere? | **Keep it where it exists.** No verdict that is static today becomes flag-backstopped. The cost is two postures for one keyword. |
| Q7 | A member that is not a constructor cannot keep an `Object` parameter (`scoped` is not admitted at `Object`). Leave it without a spelling? | **Yes**; declare the parameter at the stored type (2 sites). |
| Q8 | A user generic container's `Push(item: T)` needs `scoped item: T` — which means admitting `scoped` at a type parameter, new in §22.3 and §28.14 (§8.7; r40 rejects it today, probe q06) — and the marker at class instantiations, while a standard collection's `Add` needs neither. Accept the admission and the asymmetry, or make such a parameter marker-free at the call? | **Admit `scoped` at a type parameter; accept the asymmetry for stage 1 and measure.** The corpora name such a container at a class type nowhere (42 stores, all at value instantiations). The marker-free variant is a small follow-up if the report shows the pattern. |
| Q9 | Who gates each stage, on what? | Stage 1 with acceptance; stage 2 after stage 1 and its prerequisites are implemented and the report has been read on the in-tree programs; stage 3 after the corpus migration and a report with no unexpressed "no static form" site (§7.3). |
| Q10 | Should a weak reference over an ordinary owned object (the prior lane's design) be pursued as the companion for registries? | **As a separate proposal, after stage 1's data.** Reordering, owning with keys, and `Weak<T>` over `Shared<T>` cover the measured cases. |
| Q11 | For an instance that carries several sources, should §28.2's dominated-use rule fire when *every* source is disposed (today's letter for a record) or when *any* is? | **Every.** "Any" rejects a clean program that disposes an argument the constructor only read; "every" leaves a certain fault to the flag when the stored one of two is disposed (18 programs at 4 statements, 588 at 5; §5.2 item 4). Both are sound. |
| Q12 | The note's ruling S1 (which bounding form `_child := new Child(scoped this)` resolves). | **Moot under 0212**: a constructor call is a mint for a class-typed argument and resolves no form (§8.7). If 0212 is declined, S1 is still owed. |
| Q13 | *(new, critique finding 6.)* Collections that retain a comparer are sendable at value instantiations and cannot then announce a carried comparer. Require the comparer to be static-sourced at an instantiation that is not a carrier, or keep the type a carrier everywhere and check each value at a domain transfer? | **Static-sourced** (§4.5). *Price:* at an instantiation that is not a carrier the rule is narrow. A comparer with per-instance state cannot be given to `Dictionary<string, int>`. The one admitted spelling, a `static readonly` instance, draws §22.12 rule 4's warning for a user class, and the standard library declares no comparer instances. And **forwarding is lost**: a class or a generic that takes a comparer in its constructor and passes it to such a dictionary is an error — where the generic is named at that instantiation — whatever its callers pass. What users write instead is a key record with its own equality, the rewrite `SortedDictionary` already requires; it has none of the three costs (probe c13). Measured sites: 0 constructions with a comparer, 0 forwarded, and r40 does not implement these constructors. Two widenings are open to the owner and not proposed: standard-library comparer instances (they remove the warning only) and the comparer type parameter rationale §12.13 defers (it restores forwarding). *Price of the alternative:* a value-level check at every spawn and send; `where T: sendable` no longer rules out a carried source, so a dispatched generic method that erases needs a new constraint; two levels of the property to teach. |
| Q14 | *(new, critique finding 2.)* Is a move-only value that owns a borrow-bearing instance — an owning choice, an owning inline array, `Result<T, E>` — a carrier, by one general rule? | **Yes** (§4.6). *Price:* a factory returning `Result<Account, E>` over a parameter is checked as one returning the account is, so the same rewrites apply (bind the source first; return within its scope). Measured sites: 0 of 108 namings of `Result<…>`. *The alternative* is to reject such a type wherever it is named with an owned payload of a borrow-bearing class: simpler to state, and it takes `Result` away from every class that stores a borrow. |
| Q15 | *(new, critique note 9.)* Adopt the second order test: a holder that borrows `this` and whose release may run user code is declared after every `owned` field of its class? | **Yes.** It makes probe d04 a compile error and leaves the teardown residual reachable only across a base/derived boundary or with no stored borrow. *Price:* a declaration-order rule on back-pointing children; 0 constructions carry `this` in the corpora. Dropping it leaves d04 to the flag, as §28.12 already says. |
| Q16 | *(new, critique round 2, note 2.)* The taken-out rule ties a builder's product to its builder even where the product cannot hold a borrow of it. A refinement that reads declared types would relieve that, and is sound only in a closed form: every class involved `sealed` (§4.7). Adopt the closed form now, rule it later on stage 1's count, or decline it? | **Rule it at the stage-2 gate, on stage 1's count** (row D8). *Price of waiting:* a product of a borrow-bearing class cannot be returned from the method that built its builder; the rewrites are to build the product where its sources are in hand, or to take the builder as a parameter. Measured sites: 0. *Price of adopting now:* a second derived property in the artifact interface, relief only for `sealed` classes, and text that no measurement has asked for. The refinement as the critic worded it — without `sealed` — is unsound (probe e02; 96 certain faults at one model bound) and is not on offer. |


### Rulings (owner-delegated, 2026-11-10)

On 2026-11-10 the owner directed: "Please resolve all open questions in these proposals, then adjust any proposal or write any new ones as needed. Then, graduate the two proposals." The rulings below are made under that delegation. Each adopts the recommendation above as written, with the critique round-2 views this proposal already records folded in; none needed a change to the design, and §7.5 lists the text adjustments the landing made.

| # | ruling | where it lands |
|---|---|---|
| Q1 | **Accepted.** A class that stores a borrow is borrow-bearing, its instances carry their sources, and K4.3's field-held, collection-held and comparer-held clauses are retired through the three-stage ramp. | stage 1 at v0.357 (§7.5); stages 2 and 3 scheduled |
| Q2 | **No per-field opt-out — recorded provisionally**, and ruled again at the stage-3 gate on the stage-1 report's count of "no static form" stores in programs that are not tests (the critic's view, adopted). Stages 1 and 2 are the same either way; if the count calls for one, the opt-out Q2 sketches is what stage 3 lands instead. | `DECLINED.md` row D1, marked provisional; rationale §5.32 |
| Q3 | **The mint reads the signature**: every argument is carried, with no marker on constructors. | §28.15 *The source set* clause 1 (v0.357) |
| Q4 | **An interface is borrow-bearing by derivation** from its store members; a declared form is declined with a bar. | §28.15 *The property* (v0.357); row D3 |
| Q5 | **No spelling for a store that relates two parameters**: the storing object is the receiver, or the store is made at the caller. | row D2; the report's "no static form" reason (§28.3.12 clause 4, v0.357) |
| Q6 | **K5.7's freeze of a `scoped` argument's source is kept where it exists** (a `using var` bounding object or `this`); the locations stage 2's widening newly admits take K4.2's unfrozen posture. | §8.7 [S2] |
| Q7 | **A member that is not a constructor has no spelling for keeping an `Object` parameter**; it declares the parameter at the stored type. | the report's "no static form" reason (v0.357); §8.7 [S3] |
| Q8 | **`scoped` is admitted at a type parameter**, with the marker at class instantiations and the asymmetry with a standard collection's `Add` accepted; the marker-free variant is a follow-up if the report shows the pattern. | §8.7 [S2] |
| Q9 | **The gates, as recommended.** Stage 1 lands with acceptance (v0.357). Stage 2 lands when the compiler implements stage 1 and its prerequisites (§10) and the report has been read over the in-tree programs. Stage 3 lands when the compiler implements the stage-2 warning, the in-tree corpora are migrated (§7.4), and the report over the reference programs shows no "no static form" site without a rewrite — or the owner has re-ruled Q2 with that count in hand. | §7; the v0.357 changelog entry's ramp |
| Q10 | **A weak reference over an ordinary owned object is a separate proposal, written after stage 1's data.** It is not written now. | `DECLINED.md`, a `deferred` row; rationale §5.32; the v0.357 changelog entry |
| Q11 | **A dominated use fires when every carried source is disposed**, as for a record. | §28.15 *The bound* (v0.357) |
| Q12 | **Moot under this proposal**: a constructor call is a mint for a class-typed argument and resolves no bounding form. | §8.7 [S2/S3] |
| Q13 | **A comparer at an instantiation that is not a carrier must be static-sourced**, accepted with its price stated: a comparer with per-instance state has no spelling there, the one admitted spelling (a `static readonly` instance) draws §22.12 rule 4's warning for a user class, and **forwarding is lost** — a class or a generic that takes a comparer in its constructor and passes it to such a collection is an error, where the generic is named at that instantiation, whatever its callers pass. The rewrite is the record key that declares its own equality (§4.5), which Guide 05 teaches at stage 3. The two widenings (standard-library comparer instances; rationale §12.13's comparer type parameter) are not proposed. | §8.8 [S3]; row D7; rationale §5.32 |
| Q14 | **A move-only value that owns a borrow-bearing instance is a carrier**, by one rule: an owning choice, an owning inline array, `Result<T, E>`. | §28.15 *The property*; stdlib §1 *Borrow-bearing statements* (v0.357) |
| Q15 | **The second order test is adopted**: a holder that borrows `this` and whose release may run user code is declared after every `owned` field of its class. | §28.15 *The callee side* (v0.357) |
| Q16 | **The declared-reach refinement of the taken-out rule is ruled at the stage-2 gate, on stage 1's count.** The unsealed form stays off the table (probe e02). | `DECLINED.md` row D8, as a `deferred` row |

---

## 12. Critique record

### 12.1 Round 1 — an independent adversarial critic, on the first draft (`f72926c68`)

**Verdict: rejected as drafted; the direction judged sound.** The first draft enumerated by hand the places a source set can be lost, and the list was incomplete. The critic found five unsound routes and one regression, each with a suggested fix, and six notes. Every one is taken; none is rebutted. The critic's probes are kept as `0212/probes/c01`–`c05`.

| # | finding | what changed | evidence |
|---|---|---|---|
| 1 | An `out owned` recipient was no mint, so its set was dropped (c02). | The recipient is a mint; the callee's assignment to the slot is held to the return rule (§8.2, *The source set* clause 1, *The bound*). | c02: compiles, faults checked, empty line unchecked (MEASURED). Model `newko`: see below. |
| 2 | An owning choice or owning inline array that owns a borrow-bearing instance carried nothing (c05); and the stdlib catch-all, read by its letter, rejected `Result<Conn, E>.Ok(…)`. | *Carrier types*: a move-only value type with an `owned` payload or slot of a carrier type; a case constructor is a mint; `Result` is listed; the catch-all fires only on a non-empty set (§4.6, §8.2, §8.8). | c05 (MEASURED). Model `wrap`. Corpora: 0 of 108 `Result<…>` namings at a borrow-bearing class. |
| 3 | An `owned` argument's sources were added "to the constructed object or the receiver"; a static call has neither, and its callee could stash the value in another parameter's object (c04). | An `owned` parameter of a carrier type carries an opaque source of the caller's in the callee: it goes into `this`, back out, on to a callee under the same rule, or to its end (§8.2, *The callee side*). | c04 (MEASURED). Not in the model: it has one body. Corpora: no `owned` parameter of a borrow-bearing class. |
| 4 | §8.2 gave a replacing `move` "the instance's set", §3.2 said "the receiver's contribution"; taken from outside a member, a back-pointing child lost its owner (c03). | An owned value taken out of an instance — by a member or by a replacing `move`, wherever written — carries the binding that holds the instance and its set; the prose and the normative text now agree (§3.2 item 2; §8.2 clause 3). | Argued: r40 does not implement the replacing `move`. Model, children linked in constructors: see below. |
| 5 | The generic requirement was read where the substituted type is a borrow-bearing *class*; a borrow-bearing interface erased (c01). | "Carrier type" — class, interface, or move-only value that owns one — in §3.1 rules 2 and 4 and §8.2's generic paragraph. `where T: value` no longer counts as establishing the requirement; `where T: sendable` does. | c01 (MEASURED). |
| 6 | Making the four comparer-retaining collections borrow-bearing at every instantiation, with "a borrow-bearing class is not sendable", made `Dictionary<string, int>` and `Locked<Dictionary<string, int>>` unsendable; §8.8's "existing rules already say so" was wrong. | §4.5: they are carriers by their element types only; at any other instantiation the comparer must be static-sourced. §8.8 is corrected and names the three types it does not reach. The per-value alternative is declined with a bar (D7) and priced in Q13. | c06, c07: r40 does not implement these constructors. Corpora: 0 comparer arguments. |
| 7 (note) | Read as a mint, `Shared<T>.Clone()` would carry and freeze the original handle. | A library member may state that its result carries the receiver's set and not its binding: `Clone`, `Downgrade`, `TryUpgrade`, and the members that take an element out of a collection (§8.2 clause 3, §8.8). | c08: both handles valid, clean in both modes (MEASURED). |
| 8 (note) | "Covered by §30.1" hid that the cover is the frame-held trust clause, through which a source can still be sent to another domain. | §0, §3.3 and §5.1 say so; the "no race left" figures are qualified as outside frames. | a03, a08. |
| 9 (note) | Optionally, a self-sourced child with a user-code release could be required to be declared last. | Adopted as R6's second order test; Q15 lets the owner drop it. | d04 becomes a compile error. |
| 10 (note) | Observer self-registration has no static form and was not named. | §4.4 and the Guide 01 text name it and give the event-subscription form. | c09 (faults; no report line), c10 (clean). |
| 11 (note) | §8.13 should list the example blocks whose verdict flips; cases were wanted for items 1–7 and for an exception that stores its cause. | §8.13 lists three blocks that need `scoped` and eight whose prose changes; §7.4 adds cases 34–55. | `summary_0212.txt`; c11 prints an empty line in checked mode, c12 is the rewrite. |
| 12 (note) | The stage-1 claim holds only once 1–6 are fixed. | Re-verified two ways and stated with its condition (§7.1). | §7.1, §5.2. |

**What the model says about the first draft.** The model now holds three of the five routes (an `out owned` recipient, an owning choice over a holder, a child linked in a constructor and taken out). On them the first draft's rules left, at 3 variables and 4 statements, 6 races and 12 certain faults with no destructors, 258 races and 768 faults where a holder's destructor reads its borrow, and 486 races and 1,422 faults where children are linked in constructors (MEASURED, `0212/model/out/`). The revised rules leave no race at any bound run (§5.2). The first draft's own sweeps could not have shown this: the three statements were not in its model. That is the general weakness of a model as evidence, and it applies to this revision too.

### 12.2 Round 2 — a second independent critic, on revision 2 (`4dfa0e331`)

**Verdict: ACCEPT-W-NOTES.** All six blockers fixed; no new unsound route; the consume-table walk complete, with the foreign-function boundary argued. Three notes, each folded in. Its probe is kept as `0212/probes/e01`.

| # | note | what changed | evidence |
|---|---|---|---|
| 1 | The comparer rule (Q13) is too narrow to use as taught: at an instantiation that is not a carrier the only admitted comparer is a static; a `static readonly` user-class instance draws §22.12 rule 4's warning and the stdlib has no static comparers; and a comparer cannot be forwarded through a constructor, in a class or in a generic named at such an instantiation. | The rule is unchanged. §4.5 states the three costs and names the record-key rewrite — the one `SortedDictionary` requires for an ordering; Guide 05 teaches it (§8.11); the stdlib paragraph says a forwarding parameter is rejected and how a generic is read (§8.8); Q13 states the forwarding loss and the two widenings open to the owner; D7's bar is widened; cases 56–59. | c07, c14: the rule-4 warning (MEASURED); the comparer constructors are not implemented in r40. c13: the record-key rewrite compiles with no warning and runs clean (MEASURED). |
| 2 | The taken-out rule over-approximates: the clean rejections are children taken out of a holder that never linked them. A declaration-only refinement was suggested — carry the holder's binding only where the value's type can reach a type the holder or something it owns converts to — to be added if sound, else recorded. | **Recorded as an open refinement, not added** (§4.7, row D8, Q16). As worded it is unsound for a class that is not `sealed`: a subclass adds the borrow field the declared return type does not show. The closed form is sound in the model and its gain is measured. Cases 60–63. | e02: compiles on r40, faults checked, empty line unchecked (MEASURED). Model, 4 variables, 4 statements, holder destructor: revised 17,940 rejected; closed form 17,844, with the same 0 races and 360 faults left; declared-only reading 17,748, with 96 more certain faults (MEASURED). The cited bound's count is unchanged at 1,752 — the model's child type declares a back-pointer of its owner's class, so the test never drops it there — and 984 of its 1,206 clean rejections are this rule's (MEASURED). e01: r40 rejects `return b.Build();` today; e03 and e04 show that verdict follows the shape of the `return`, not the product (MEASURED). |
| 3 | The return rule could be misread as rejecting the return of an `owned` parameter. | §8.2's *Returns* bullet says it outright: the opaque caller source the parameter carries is re-established, the parameter's own binding is not; returning the parameter is admitted, returning an instance that borrows it is rejected. Case 64, beside case 43. | Text only. |

**On note 2, what "sound" would need.** The suggestion is right about the cost and right that types can bound it. It fails on the one thing a declared type does not fix in this language, the set of fields a value has. Three ways to close that were weighed: `sealed` everywhere (the closed form: sound, narrow, D8); a rule that an owned upcast may not narrow what a type can hold a borrow of (a second erasure rule, and a new rejection class no measurement has priced); and a callee-side rule that a member whose declared return type cannot reach its class returns nothing that carries its receiver (it needs the body to tell a receiver-sourced borrow from one read out of the receiver's set, and an override to answer for the declaration it overrides). None is small enough to add in a note, and none has a measured site.

### 12.3 What a further critic should attack first

In the author's order of doubt.

1. **The consume-table argument (§3.1).** It claims that an owned value changes holder only at a consume-table row, an `out owned` slot or a replacing `move`. A position that takes ownership and is none of these would be a seventh hole. Candidates: a `Shared<T>` constructor and `Locked<T>` interior (read as `owned` arguments to a library type); a channel send (sendable only); a closure, which may not consume a captured owned binding (K5.5); the foreign-function boundary.
2. **The taken-out rule over-approximates, and its refinement is open** (§4.7). A value taken out of a user object carries that object's binding, whether or not the object ever gave it a borrow of itself, so a builder's product of a borrow-bearing class cannot be returned from the method that built the builder. The second critic attacked this; the declaration-only relief it proposed is unsound for an open class and is recorded with a bar. What to attack now: the claim that the *closed* form is sound (it is argued, and checked only in a model with no subclasses), and whether one of the three other closures in §12.2 is cheaper than it looks. No corpus program meets the rule; real builders may.
3. **R4's `owned` parameter rule** leans on every callee being bound by it. A standard-library callee is bound by its entry instead; an entry that takes `owned T`, is not a carrier, and is silent is covered only by the non-empty-set rejection at the caller.
4. **Q13's choice.** At `Dictionary<string, int>` a comparer with per-instance state is refused, a forwarded one is refused, and the admitted spelling is a warned one (§4.5). The record-key rewrite covers what was measured, which is nothing: no corpus program passes a comparer. If comparers turn out to be common, D7's bar is met.
5. **An owning choice with both a borrow payload and an owned carrier payload** is read by §9.7 for the first and by §28.15 for the second. The two source sets should be one; the text says a carrier value "carries one source set" and relies on §9.7's own union.
6. **Two postures for `scoped`** (Q6); **clause 3 of R1 on classes** (a class is borrow-bearing if it declares a `scoped` class-typed parameter, even where it stores nothing).
7. **The scanner** is syntactic, reads one unit at a time, and does not follow a forwarded value into its callee. **§8.13 is a list, not an edit set**: the defining text is exact and anchored; the consequential mentions and example blocks are enumerated and classified, not rewritten.
8. **No number here is from real programs.** Every corpus is a test corpus, and every revision-2 count at a borrow-bearing class is zero — which says the tests do not write these shapes, not that programs will not.

### 12.4 What could not be done

The compiler was not built or modified, so no 0212 rule was run and its cost is unmeasured. The generics matrix was not run against R5. The fold-in of round 2's notes has not itself been critiqued. The critic's c03 could not be run: r40 does not implement the replacing `move`. The comparer rule could not be probed, a forwarded comparer included: r40 does not implement those constructors. The refinement sweep of §4.7 was run at 4 statements only; at 5 statements with 4 variables it outgrew the machine's memory and was stopped. Whether v0.355 requires r40's rejection of `return b.Build();` for a product that stores no borrow (probe e04) was not established. `OwningSlotMap<T>` is not implemented in r40, so that registry rewrite was not probed. The exact text of the consequential mentions (§8.13) and of the revised §28.3.8 examples is left to the stage-3 train.

---

## 13. Files

Under `docs/research/static-ownership-tracking/0212/`:

- `README.md` — an index of this directory.
- `probes/` — 51 programs (`d*`, `g*`, `a*`, `q*`, `c*` for critique round 1 and its follow-ups, `e*` for round 2; c01–c05 and e01 are the critics' own), `run_all.sh`, `results.md` (r40, three modes).
- `model/bzcore_0212.py` — the model; `model/run_sweeps.sh`, `model/table.py`, `model/breakdown.py`, `model/left.py`, `model/takeout.py`; `model/out/` — the self-tests, every sweep of §5.2, the per-rule breakdowns, the classification of what is left, and for §4.7 the `reach_*.txt` comparisons and `takeout_v3l4_qdtor.txt`.
- `data/scan_0212.py` — the corpus scanner, with one pass per critique finding; `data/scan_collection_transfer.py` — one extra count; `data/out/` — `summary_0212.txt`, `rev2_findings.tsv`, `collection_transfer.txt`, the per-pass tables, and `base/`, the note's scanner re-run.
- `check_anchors.py` — verifies that each `old` anchor of §8 occurs exactly once at this base, and lists the consequential mentions.
