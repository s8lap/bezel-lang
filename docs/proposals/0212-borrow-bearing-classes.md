# Proposal 0212 — Borrow-bearing classes: an object that stores a borrow carries its sources, and the field-, collection- and comparer-held trust clauses are retired

| | |
|---|---|
| **Status** | under-review — draft (2026-11-10), written under the owner's direction of 2026-11-10 on the static-ownership-tracking research note ("Proceed with Fable work to make static ownership tracking part of the spec"). A design change (spec-change-process §2): it deletes three recorded K4.3 admissions, changes what compiles, and reopens a recorded decline (`DECLINED.md`, the callee-side no-store row; rationale §4.2.6) on new evidence (§9). It proposes a staged landing — report, warning, error — and nothing lands before owner acceptance. The three items the research note listed as blocking kernel text are ruled in §3. No adversarial critique has been run on this draft yet; §12 is the author's own pass and names what a critic should attack first. |
| **Tracking issue** | to be filed by the coordinator. [#7509](https://github.com/s8lap/bezel-lang/issues/7509) is the open work package this proposal would revise. |
| **Target documents** | **spec**: §3.6 (one table row), §28 *Chapter at a glance* (one row), **§28.15 (new)**, §28.3.1 (K2.3; a new K2 clause 5; K4.3 and its example; K5.5; K8.4), §28.3.5 (*The sources an argument contributes*, items 1 and 5), §28.3.8 (the field-store bullet's five clauses; one phrase of the closure bullet's provenance clause 1), §28.3.12 (the trust-admission report: clause 1, a new clause 4, the closing paragraph), §28.11 (*Storing a borrow*), §28.14 (scoped construction: three paragraphs and one new bullet), §22.3 (one sentence), §28.2 (the holder list), §9.7 (one sentence of the instance-field bullet), §30.1 (one paragraph added), §30.14 (one sentence), Appendix C.3 (one bullet), and the consequential mentions listed in §8.13. **stdlib**: §1 (a new statement paragraph), §4 (*A retained comparer is a borrow*). **rationale**: §4.2.6 (one sentence), §5.1 (one paragraph), §5.31 (one paragraph), a new §5.32. **guides**: Guide 01 §3 and §5. **code-generation reference**: §4.4, §4.6, §7.2. **proposals**: `README.md` (this row), `DECLINED.md` (six rows annotated, six added). **conformance**: §7.4 lists the cases to add at each stage and the cases whose verdict changes at the last. **compiler** (spec-ahead-of-implementation, non-normative): §10. |
| **Written against** | spec **v0.355** at `a77e4718a` (`origin/main`; the twelve documents are byte-identical to `a30bb11df`, the base of the research note). Section numbers are v0.355's. Every `old` anchor quoted in §8 is checked to occur exactly once at this base by `docs/research/static-ownership-tracking/0212/check_anchors.py`. |
| **Origin** | `docs/research/static-ownership-tracking/README.md` (2026-11-10) recommended its candidate A — a class with a plain class-typed field is borrow-bearing, and its instances are read as K6 reads a frame — in report mode first, and listed three soundness items as blocking kernel text: generic-body erasure, the destructor window, and a class instance across an `await`. This proposal is candidate A finished: those three items ruled, the note's other open questions answered, the rules extended in an executable model, and the corpora re-measured. Supporting material is under `docs/research/static-ownership-tracking/0212/`. |

*Reference convention. Unprefixed §-references are language-spec references at v0.355; K tags are §28.3.1's kernel ids; "the note" is the research note above. ▲ marks proposed normative text. Every statement of fact carries one of three labels: **MEASURED** (produced by a script, model run or probe under `0212/`, or re-run from the note's own scripts), **QUOTED** (taken from the tree at the cited place), **HYPOTHESIS** (reasoning nothing here measures). Probes were compiled with the r40 prerelease `bezelc 0.0.0-a30bb11d` in three modes (checked with the report, checked, unchecked) and run; `0212/probes/results.md` has every verdict. Programs use only members the standard library declares or declare their own.*

---

## 0. Summary

**The gap.** Three kinds of value that carry a borrow are tracked statically today: a borrow-bearing record (§9.7), a function value (§28.3.8, §28.14) and an iterator or query frame (K6). Each is read with a source set, scope-checked, and frozen through the local that holds it. A fourth kind is not: a *class instance* with a plain class-typed field. Its stored borrow is admitted on K4.3's field-held, collection-held or comparer-held trust clause. Checked mode's flag is the backstop, a transfer to another domain has no backstop at all, and an unchecked build is undefined behaviour.

**The proposal.** A class that declares a stored borrow is a **borrow-bearing class** — a derived type property (§3.6), computed from declarations. Its instances carry a source set, built from what callers can read in a signature:

- a **mint** — a constructor call, or a call that returns such an instance `owned` — carries the receiver and what each argument contributes;
- a **later store** — an argument at a `scoped` parameter, a property store, a standard collection's `Add` — adds sources and is checked as an assignment to the location that holds the instance;
- the instance then obeys the rules a record local and a frame local obey today: the scope comparison (K2.3), the no-temporary-source rule (K8.4), the freeze (K5.5) and the dominated-use error (§28.2).

On the callee side a plain parameter becomes call-bounded for an object borrow, as it already is for a callback: only a constructor may keep it, or a member that declares it `scoped`. K4.3's three clauses for stored borrows are deleted. The frame-held clause, §28.14's two callback residuals and the slot-displaced transfer stay.

**The three blocking items, ruled** (§3).

1. *Generic erasure.* An owned value whose source set is not empty may not be converted, at an owning position, to a type that is not borrow-bearing. In a generic body that conversion is a requirement of the body's use of the type parameter, read at each instantiation (§3.6 rule 2) — the mechanism `Keep<Pair>` uses today. A dispatched generic method decides it at its body. MEASURED cost in the corpora: 0 programs. The hole is real today: four generic probes compile, fault in checked mode, and print an empty line in an unchecked build.
2. *The destructor window.* It is already ruled: spec §28.12 (v0.355, #7506) says the accesses of an instance's own disposal, "a child's read of the instance through a back-reference included", run on a live instance. The note's fault (probe k1a) is r40 behind that text. What 0212 adds is one static order test for a holder that borrows a sibling field, and the rule that a release which may run user code is a use. One flag-backstopped residual remains and is named: teardown code that reaches an owned field the cascade has already disposed. It exists without any stored borrow.
3. *Async.* No new rule is needed and §30.1's record ban is not imported. Every source of a borrow formed in an `async` body is frame-resident or is already carried by the frame, so a local instance across an `await` adds nothing to the settlement clause the frame already has. Two sentences extend what a frame and an awaited result carry. The frame-held clause is not retired by this proposal.

**Headline measurements.**

| | result |
|---|---|
| Model, every sweep run (largest: 2,350,698 programs, 561,474 accepted): programs newly accepted / newly unsound under 0212 | **0 / 0** (MEASURED, `0212/model/out/`). 0212 only tightens, so this is a consistency check; the result that carries weight is the next row |
| Model: reads of an object another domain owns, through a stored borrow, left under 0212 | **0** at every bound |
| Model, 3 variables, 5 statements, both destructor variants: of 45,126 accepted programs that read a dead or foreign object through a stored borrow (11,778 of them a race), left accepted | 1,266, none a race — a slot kill (K5.6) or an announced kill of one source of several (K4.2); the flag catches each |
| Model: programs the owned-erasure rule (R5) rejects that the note's candidate accepted; faults and races it removes | 126 of 26,031 at 3 variables, 4 statements; 24 faults or races where the holder's destructor reads its borrow; at 5 statements, 6 races the note's candidate leaves and 0212 does not |
| Corpora (9,521 units, 5,750 accepted-tier programs): accepted-tier programs rejected / needing a `scoped` marker / unchanged | **5 / 12 / 114** of the 131 that store a borrow or build a borrow-bearing instance — the note's figures, reproduced; the rules 0212 adds reject **0** more (MEASURED, `0212/data/out/summary_0212.txt`) |
| Recorded K4.3 sites retired | 159 of 178 (the note's count, unchanged) |
| Corpora: `async` bodies with a borrow-bearing local across an `await` / generic bodies erasing an `owned` type-parameter value, instantiated at a borrow-bearing class | 0 of 1,064 `async` methods / 0 |

**The ramp** (§7). Stage 1: the property and the rules land as the *classification the trust-admission report prints*; no verdict changes. Stage 2: a default-build warning at exactly the sites stage 3 will reject, and the widening of `scoped` that gives each a rewrite. Stage 3, a Ratchet: the warnings become errors and the three clauses are deleted. Each stage is one revision; the gates between them are the owner's (Q9).

**What stays argued, not modelled** (§5.4). The model has one body and no calls. So the callee-side rule (R4), the per-instantiation reading of the erasure requirement, interfaces, virtual dispatch, closures, `Shared<T>` and frames rest on argument and probes. The model does check the mint's carried set, the scope comparison, the freeze, the dominated use, owned erasure as a caller sees it, an owned argument that brings its sources to a receiver, and the destructor window.

**Owner decisions** are in §11: twelve questions, each with a recommendation. The two that shape the rest are Q2 (no per-field trust opt-out — recommended) and Q3 (the mint reads the signature, with no marker on constructors — recommended).

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

**R1 — the property.** A class is *borrow-bearing* when it declares or inherits (1) a **borrow field**: an instance field that is neither `owned` nor `scoped` and whose declared type is a class or interface type that is not runtime-managed; (2) an `owned` field, owning-inline-array slot or owning choice payload whose type is a borrow-bearing class; or (3) a **store member**: a member with a class- or interface-typed `scoped` parameter, or a settable property or indexer of such a type that is not `set(owned)`. An interface is borrow-bearing when it declares or inherits a store member. The standard library states the property for its own classes. The property is computed, never declared, and read per instantiation.

**R2 — the source set.** An instance of a borrow-bearing class carries one source set.
- *Mint.* `new C(…)`, or a call whose declared return type is `owned` at a borrow-bearing class or interface, carries the receiver's contribution and each argument's, whether or not the callee stores it. The caller reads the signature, never the body.
- *Later store.* An argument at a class-typed `scoped` parameter, a property or indexer store of a reference, a store position of a standard collection, a field assignment `x.f := e` to a borrow field, and an `owned` argument that itself carries sources, each add sources to an instance. Nothing removes one. The store is checked as an assignment of the updated instance to the location that holds it.
- *Reads.* A borrow of the instance, or anything read out of it, carries the instance's binding and its set.
- *An empty set.* An instance built from nothing that contributes carries no source and may be held anywhere, static storage included.

**R3 — the bound.** The instance may not outlive a source by scope (K2.3). A fresh temporary may not be a source of an instance that outlives the statement (K8.4). A local that holds the instance is a live local borrow of every source (K5.5), so a move or reassignment of a source is frozen while it is live; an explicit `Dispose()` of a source is not frozen (K4.2), and a use it dominates is §28.2's error. Where the instance's release may run user code, the release is a use — at scope end, at an explicit disposal, at a drop-on-overwrite — so the local is live to it, and a later-stored source must be declared before the instance.

**R4 — the callee side.** A borrow sourced at a plain class- or interface-typed parameter is call-bounded. It may be stored into the object under construction, in a constructor; into an instance or collection that is purely local to the body; or, where the parameter is `scoped` or is a setter's `value`, into `this`. A borrow sourced at the receiver may be stored into storage `this` owns, in any member. Every other store is an error.

**R5 — owned erasure.** An owned instance whose set is not empty may not reach an owning position whose declared type is not borrow-bearing: an `owned` field, parameter, return or slot typed `Object`, a base class or an interface that is not borrow-bearing, or the operand of `throw`. In a generic body the conversion is a requirement read per instantiation (§3.1).

**R6 — teardown.** A borrow of `this` held by something `this` owns is bounded by the cascade (§28.4, §28.12): the holder is released inside the owner's disposal, before the owner's block. A holder in an `owned` field that borrows a sibling `owned` field must be declared after it, where the holder's release may run user code (§3.2).

**R7 — frames.** A frame carries what its arguments and captures contribute, an instance's sources included; the result of awaiting a task carries the task's carried set (§3.3).

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

**Kernel classification** (spec-change-process §9). R1 is a derived type property. R2–R3 derive from K2.3 + K2.4 + K5.5 + K8.4, stated once as a new K2 clause 5. R4 derives from K2.4 (a plain signature announces no retention), the ground §28.14 already gives for a callback. R5 derives from K2.3 + K2.4, the ground of §9.7's borrow-erasing bullet. R6 derives from K9 and §28.4. No axiom is added. Axiom A11 is *extended*: a class-typed `scoped` argument is admitted against any traced location, not only a `using var` local or `this`.

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

The unchecked runs are the worst outcome: a read of freed memory that returns normally.

**The ruling: a structural, per-instantiation requirement, computed at the generic's single check.**

1. *At a concrete site* the rule reads the value's set: an owned instance whose set is not empty may not reach an owning position whose declared type is not borrow-bearing (R5). g05 and g06 are this case.
2. *In a generic body* an owning conversion of a value whose type mentions a type parameter, to a target type, is a **requirement of the body's use of that parameter**: at every instantiation at which the value's type, after substitution, is a borrow-bearing class, the target type, after substitution, must be borrow-bearing too. An instantiation that fails is rejected where it is named, with the diagnostic naming the conversion. The set is not consulted, because a body checked once cannot know it: the requirement is read off types.
3. A requirement reaches a declaration through a substitution as §3.6 rule 2 already says (`Outer<T>` forming `Hide<T>`, probe g02).
4. A **dispatched generic method** has no instantiation site for the bodies dispatch reaches (§14.2), so the conversion is an error at the body unless the method's constraints establish that the parameter is not a borrow-bearing class. `where T: value` and `where T: sendable` do (probe g04 is the unconstrained case).
5. A **standard-library** generic body contributes no requirement (§3.6 rule 2), so the library states the property per type: a library class that takes `owned T` is borrow-bearing at a borrow-bearing `T` only where its entry says so, and every other library instantiation at a borrow-bearing class in an owning position is rejected at the site (§8.8).

**Why this rule and not the alternatives.**

- *An instantiation-site rejection with no body fact* — reject every generic instantiated at a borrow-bearing class unless the generic opts in (Swift's `~Escapable`, C#'s `allows ref struct`). It needs a new constraint form, and it rejects `Box<Account>`, which keeps its `T` and is sound (probe g08). Declined (§8.10, row D4).
- *A constraint the generic's author writes.* Bezel has no "holds no borrows" constraint, and a requirement the compiler can read from the body is what §3.6 rule 2 exists for: `Keep<Pair>` is rejected today with no constraint written.

**Soundness (argued).** A borrow-bearing instance can lose its set only where its static type stops announcing one. Inside one body the set is flow-tracked on bindings, whatever their declared type, so a local declared `IShow` loses nothing. The set therefore can be lost only where a value leaves a body or enters storage: an `owned` field or slot, an `owned` argument, an `owned` return, a `throw`, an owned `ref` or `out` location, a task result. Each is an owning position with a declared type, and R5 reads that type at the conversion. A *borrowing* position cannot lose a set: a borrow passed at `Object` or an interface contributes its binding and the instance's sources at the caller, read from the argument and not from the parameter's type (R2), and the callee may keep it only where a signature says so (R4). Probe g07 is that case: a `Keeper` built over a borrow at an interface type carries the instance and its source.

The three routes the note feared, each closed by a rule that exists:

- *Through a second generic*: the requirement propagates through the substitution (§3.6 rule 2; g02).
- *Through a standard-library body*: the library's entry states the property; where it is silent the instantiation is rejected at the caller, where the types are concrete (g05).
- *Through an `as` at a type parameter*: an `as` yields a borrow, never an owned value, so it is not an owning position. `x as T` over a borrow-bearing instance yields a borrow that carries the instance's binding and set.

**Cheap.** The single check of a generic declaration records one pair of types per owning conversion whose source type mentions a parameter. Each instantiation evaluates the pairs under its substitution. That is the cost model of §9.7's bullets and of proposal 0159's `as T` requirement.

**What it rejects** (MEASURED, `0212/data/out/summary_0212.txt`). The corpora hold 161 `owned` parameters typed at a type parameter in members of generic declarations. 47 are kept at a type that mentions the parameter, 35 are moved into a local or forwarded, 61 are only used or unused, and 18 are erased — every one a `throw x` under a `where T: Exception` bound. None is in a dispatched generic method. No erasing declaration is instantiated at a borrow-bearing class; the whole corpus names a borrow-bearing class as a type argument five times (`Array<Row>`, three `Channel<…>` that sendability already rejects, one user generic that keeps its parameter). **Programs rejected: 0.** The scanner is syntactic and reads one unit at a time; a forwarded value is not followed into its callee.

**In the model.** The model has no generics. It has the caller's view of one: `erase y x` converts a holder to an owned value whose type announces nothing, and `euse y` reads through it. With R5 off (the note's candidate) the conversion is admitted and the model finds the faults and races; with R5 on they are rejected (§5.2). That checks R5 as a rule over values; the per-instantiation reading is argued.

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

**The ruling: the cascade order makes the self-sourced borrow sound, and 0212 adds two static tests.**

1. *The stored borrow itself never dangles.* A holder stored in storage its source owns is released inside the source's disposal, and the source's block is reclaimed only after the disposal reaches the root of the hierarchy (§28.4, §28.2's compiler-inserted free). By §28.12 a read of the source in that window is a read of a live instance. So the invariant's self-source case holds through teardown.
2. *A holder moved out of its owner carries the owner.* It leaves through an `owned` return of an instance member, or a replacing `move`, and R2 gives the result the receiver's contribution. From there it is an ordinary instance with an outside source.
3. *New — the sibling order test.* A holder in an `owned` field `f` that carries the value of a sibling `owned` field `g` must have `g` declared before `f` where the holder's release may run user code: the cascade then releases `f` first. Probe d06 (right order) is clean in both modes; d07 (wrong order) faults in checked mode and segfaults unchecked, and is an error under 0212.
4. *New — a release that may run user code is a use.* For a local instance this is the note's A-3. The key is stated so that it is sound for a subclass: the release is taken to run user code unless the instance's class is trivial-dispose **at its produced type**, where a `new` expression fixes it, or its declared class is `sealed` and trivial-dispose. (§28.9 reads the declared type where no producer is known; that is a lint's key and would be unsound here, since `var b: Base := new Derived()` may run a destructor.) Probe d09 (a flush-on-dispose holder declared after its source) is clean; d10 (declared before it) faults and is an error under 0212.

**What 0212 does not do: forbid a destructor from reading a stored borrow.** Three static rules were considered and declined.

- *No read of a borrow field in a destructor body.* It retires flush-on-dispose over a borrowed sink (d09), which is sound, and it is not complete: a destructor that calls its own method reads the field indirectly.
- *A self-sourced holder must be trivial-dispose.* It rejects every tree with parent pointers, because `OwningArray<T>` is never trivial-dispose (§28.9, QUOTED), and it does not bound a subclass's destructor.
- *Whole-subtree deferred reclamation*, so that no read in the window can dangle in an unchecked build. It is a runtime mechanism of the kind §28.9 says does not exist.

**The residual, named.** One read in the window is not sound, and §28.12's last quoted sentence already names it: teardown code that reaches an owned field the cascade has already disposed. A child declared before a sibling can reach that sibling through its owner (d04). It is not a stored-borrow residual — d08 reaches it with no stored borrow — and it is not trust: the disposed field's own flag is set, the reader and the object are in one domain by construction (an owner and what it owns move together, and a borrow-bearing class is not sendable), so checked mode faults deterministically. It is K5.6's posture: a slot kill is not frozen, and the backstop is real there. An unchecked build is undefined there, as for every K5.6 residual today.

**In the model** (§5.2). The model gains a child destructor that reads its back-pointer, and the two flag timings. Under §28.12's timing, 0212 leaves no fault and no race through a child's back-pointer at any bound. Under r40's timing 4,194 of 26,031 accepted programs fault on every run at 3 variables and 4 statements: every program that links a child to its owner.

### 3.3 Async

**The question** (the note, S4). §30.1 bans a local of a value type that holds borrows across a suspension unless every source is frame-resident. A class instance built over a caller-sourced parameter and held across an `await` is the same borrow one hop away. Import the ban, or let the frame-held clause cover it?

**The ruling: §30.1's frame and settlement rules cover it; the record ban is not imported; two sentences are added.**

*Why the frame already covers it.* In an `async` body every borrow has a source of one of two kinds. It is frame-resident — an owned local, a `using var`, an `owned` parameter — and then the frame owns it and the ordinary local rules bound it inside the one body. Or it is caller-side — a plain parameter, `this`, a capture — and then it is one of the frame's own carried sources: K6 says a task carries "the constructing call's receiver, the sources its plain arguments contribute […] and the capture-set contributions" (QUOTED). An instance built in the body can only store borrows formed in the body. So its set is a subset of the frame-resident sources and the frame's carried set. It gives the frame nothing to hold that the frame does not hold already, and the caller's obligation — "every borrow source reaching an async call must outlive the settlement of the returned task" (§30.1, QUOTED) — is unchanged in extent.

*Why the record ban does not carry over.* The ban's ground is §9.7's posture that a value type that holds borrows is stored past its bound at no position. A class-typed borrow has never had that posture in a frame: `var b := c; await …; b.M();` over a plain parameter `c` is admitted today on the frame-held clause (probe a01, MEASURED: clean when awaited). A holder across the `await` is the same admission (a02: clean when awaited; a03: faults when the caller disposes the source before settlement — and so does the plain alias, a08). Importing the ban would reject the routine helper built over a parameter and leave the alias next to it admitted.

*What is added.*

1. **A frame carries an instance's sources.** As an argument or a capture, an instance of a borrow-bearing class contributes its binding's source and the sources it carries; passed `owned`, the sources it carries (R2's contribution rule, §8.4). Probes a05 (a plain parameter), a06 (an `async` lambda's capture) and a07 (an `owned` parameter) are the cases.
2. **An awaited result carries the task's carried set.** Where a task's result type is a borrow-bearing class — `OwningTask<Account>`, or a `Task<Account>` whose awaiters borrow — the value an `await` yields carries what the task carries. Probe a04 is the case: `using var acct := await Open(c); c.Dispose(); acct.OwnerName();` compiles today and faults; under 0212 the use is §28.2's error.

*What stays.* The frame-held clause is K4.3 trust and 0212 does not retire it. A task is not traced by K5 (K6.4), so a caller that kills a source while a frame is unsettled is admitted, reported in report mode, and backstopped by the flag — for a holder inside the frame exactly as for a parameter inside it. A strict mode is where that clause ends (the note's candidate E), not this proposal.

**Evidence.** Probes a01–a08 (MEASURED, r40). The corpora price neither reading: of 1,064 `async` methods, none has a parameter or a task result of borrow-bearing class type, and none holds a borrow-bearing local across an `await` (MEASURED). The model has no frames, so this ruling is argued.

---

## 4. The other open questions

**4.1 Lifting the interface restriction — derived, not declared.** R5 rejects an owned borrow-bearing instance at an interface that announces nothing (the note's p14). The lift is R1's clause 3: an interface that declares a store member — a `scoped` class-typed parameter, or a settable reference-typed property — is borrow-bearing, and every owning position typed at it carries a set. This is §3.6 rule 1's posture, and it is also forced: a later store through such an interface adds sources to its receiver, so an owned value of that interface type must be able to carry them. A *declared* borrow-bearing interface, for one whose implementations store only in constructors, is declined with a bar (§8.10, row D3): the corpora hold no owned upcast of a borrow-bearing instance with a non-empty set (MEASURED, 0 of 312 constructions), of their 765 interfaces none declares a class-typed `scoped` parameter and three, all in gtest files, declare a reference-typed property that may be settable (MEASURED), and the rewrite — return the concrete class — exists.

**4.2 A store that relates two parameters — no new spelling; the storing object is the receiver.** `static method Store(a: Array<Item>, it: Item)` that calls `a.Add(it)` stores one parameter into an object another parameter holds: 1 accepted-tier site and 8 distinct gtest sites (the note, MEASURED). R4 rejects it, because the body cannot see the binding that holds `a`. The spelling exists: make the storing object the receiver — an instance method or an extension method — and declare the stored parameter `scoped`. The caller then checks the store against the receiver's binding. MEASURED on r40: for a user class both forms compile and run today (probes q01, q02). Where the storing object is a *standard collection*, the extension form needs `scoped` at a type parameter, which §22.3 does not admit today (probe q06: rejected); 0212 admits it (§8.7), and until then the rewrite is to store at the caller. A marker that names another parameter as the bounding object (`scoped(a) it`) is declined (§8.10, row D2): it is the first step toward lifetime parameters, and the receiver form or the caller-side store expresses every measured site.

**4.3 A per-field trust opt-out — recommended: none.** The note asked for this decision up front. The case for an opt-out is migration and the shapes with no static form. The case against, which this proposal takes:

- It keeps the field-held clause alive under another name, and with it the one hazard checked mode cannot catch: a stored borrow of an object later sent to another domain. "Retire the clause" becomes "rename the clause".
- The measured need is small and each shape has a rewrite. After the receiver-form rewrite of §4.2, the stores with no static form across every corpus are two methods that keep an `Object` parameter (retype the parameter) and the holder-before-source registry (§4.4).
- The ramp is the safety valve. Stages 1 and 2 reject nothing, and stage 1's report counts the sites with no static form in real programs. That count is the evidence an opt-out would need; it does not exist yet.

So `DECLINED.md`'s callee-side row is reversed for members that are not constructors, with no escape hatch, and the opt-out is recorded as declined-at-proposal with that count as its reopening bar (§8.10, row D1). If the owner prefers to keep one, Q2 sketches it; nothing else in the proposal changes.

**4.4 Registries and weak references — what users write instead.** A long-lived holder declared before the things it refers to cannot hold tracked borrows of them (the note's p17, p30). Four spellings exist, none new:

1. *Reorder*: declare the source first (the note's p29).
2. *Own the items*: an owning collection, with `OwningSlotMap<T>` and `SlotHandle` keys where callers need a stable name (stdlib §4.21; marked spec-ahead-of-implementation, #4604, so not probed).
3. *Observe them*: `OwningArray<Weak<Item>>` over `Shared<Item>`, with `TryUpgrade()` (the note's p28: runs clean on r40 in both modes). An `owned` field of type `Weak<Item>` does not make its class borrow-bearing, `Item` not being one.
4. *Events* for callbacks (stdlib §6.5), which §28.14 already bounds.

A registry held in a **static** takes only static-sourced borrows: §28.3.8's *Static stores* already rejects the direct store, and under 0212 a store into a collection a static holds is checked against that location (probe q03: admitted today, faults). The corpora under-represent this shape by construction; stage 1 is what measures it. A weak reference over an ordinary owned object, the prior lane's design, is a separate proposal (Q10).

---

## 5. Soundness

### 5.1 The invariant and the argument

**Invariant.** For every live object *o* of a borrow-bearing class, each object reached through a borrow field of *o* is static; or is an object that owns *o*, directly or through `owned` fields, or a field value of one; or is the value of a source in *set(o)* as read at the location that holds *o*.

**Why it is preserved.** *set(o)* changes only at a mint or a later store (R2). A mint is read from the signature and so over-approximates what the body stores (R4 bounds the body: a plain parameter reaches only the object under construction or a local instance that leaves through a mint). A later store is announced by a signature or is a field assignment, and is checked against the location that holds *o*. A source in the set cannot end before the instance by scope (K2.3, K8.4), cannot be killed silently while the instance is live (K5.5 over K5.1 and K5.2, a transfer to another domain included), and the set cannot be dropped at an owning position (R5). The self-source case holds through teardown by §3.2.

**What remains outside it**, each flag-backstopped and none a trust clause:

1. an announced kill (K4.2) that does not dominate the use — on some paths only, or of one source of several;
2. a kill of a slot's current value (K5.6), the teardown case of §3.2 included.

The frame-held clause is outside this proposal's subject (§3.3).

### 5.2 Model results (MEASURED, `0212/model/out/`)

`0212/model/bzcore_0212.py` is a copy of the note's model with three statements (`newk2`, a two-argument mint that stores one argument; `erase` and `euse`, an owned conversion to a type that announces nothing and a read through it), R2's owned-argument clause at `fstm`, and two machine variants (a child destructor that reads its back-pointer; the flag set at the beginning of `Dispose()`, as r40, in place of §28.12). Its docstring lists them. The note's 42 self-tests still pass, and 15 new programs, each checked at the baseline and under 0212, cover the additions (`out/selftest0212.txt`, 30 checks). The baseline is v0.355 with proposal 0205's rules applied.

Columns: programs enumerated; accepted at the baseline; the *trust surface* (accepted programs in which some run reads a dead or foreign object through a stored borrow) as race + fault on every run + fault on some run; then 0212.

| bounds (3 variables unless stated) | programs | accepted | trust surface | 0212: newly rejected = race + fault every + fault some + dangling never read + clean | 0212: left (race; fault every / some) | newly accepted / unsound |
|---|---|---|---|---|---|---|
| 4 statements | 81,174 | 26,031 | 36 = 12 + 24 + 0 | 342 = 12 + 24 + 0 + 192 + 114 | 0; 0 / 0 | 0 / 0 |
| 4, holder destructor reads | 81,174 | 26,031 | 1,992 = 534 + 1,458 + 0 | 2,130 = 534 + 1,410 + 0 + 6 + 180 | 0; 48 / 0 | 0 / 0 |
| 4, child destructor reads (§28.12 timing) | 81,174 | 26,031 | 60 = 18 + 42 + 0 | 366 = 18 + 42 + 0 + 192 + 114 | 0; 0 / 0 | 0 / 0 |
| 4, both destructors | 81,174 | 26,031 | 2,016 = 540 + 1,476 + 0 | 2,154 = 540 + 1,428 + 0 + 6 + 180 | 0; 48 / 0 | 0 / 0 |
| 4, 2 variables, `if` | 56,548 | 12,732 | 102 = 38 + 12 + 52 | 240 = 38 + 12 + 26 + 164 + 0 | 0; 0 / 26 | 0 / 0 |
| 4, `if` | 331,692 | 75,030 | 306 = 114 + 36 + 156 | 1,614 = 114 + 36 + 78 + 1,146 + 240 | 0; 0 / 78 | 0 / 0 |
| 4, `while` | 2,350,698 | 465,942 | 936 = 24 + 24 + 888 | 6,072 = 24 + 24 + 876 + 3,144 + 2,004 | 0; 0 / 12 | 0 / 0 |
| 4, inner `do … end` | 2,350,698 | 561,474 | 1,572 = 372 + 1,200 + 0 | 11,058 = 372 + 1,200 + 0 + 6,342 + 3,144 | 0; 0 / 0 | 0 / 0 |
| 4, inner `do … end`, both destructors | 2,350,698 | 561,474 | 36,516 = 7,176 + 29,340 + 0 | 47,046 = 7,176 + 27,972 + 0 + 342 + 11,556 | 0; 1,368 / 0 | 0 / 0 |
| 5 statements | 1,812,918 | 383,451 | 2,316 = 666 + 1,650 + 0 | 10,806 = 666 + 1,602 + 0 + 6,690 + 1,848 | 0; 48 / 0 | 0 / 0 |
| 5, both destructors | 1,812,918 | 383,451 | 45,126 = 11,778 + 33,348 + 0 | 48,150 = 11,778 + 32,082 + 0 + 582 + 3,708 | 0; 1,266 / 0 | 0 / 0 |

Reading.

1. **No race through a stored borrow is left at any bound**, and every fault left is one of the two named residuals (item 3). Nothing is newly accepted or newly unsound in any sweep; since 0212 only tightens, that column is a consistency check on the model, not a finding.
2. **The owned-erasure rule closes a measured hole.** At 3 variables and 4 statements the note's candidate, with no R5, accepts 126 programs that 0212 rejects (`out/breakdown_v3l4.txt`). Where the holder's destructor reads its borrow, 24 of them fault or race at the baseline (`new a / newk b a / erase c b / asg a`; with `send a`, a race), and the note's candidate leaves 60 certain faults where 0212 leaves 48. At 5 statements the note's candidate leaves 6 races that no flag catches (`new a / newk b a / erase c b / send a / euse c`); 0212 leaves none.
3. **What 0212 leaves is not trust.** The 48 are of two shapes, 24 each (MEASURED by a direct count). *A slot kill*: `new a / fbrw b a / fst a / newk c b` — K5.6. *An announced kill of one source of two*: `new a / new b / newk2 c a b / disp a` — the mint carries both arguments, §28.2's rule needs every source disposed, so the holder's release is not rejected and faults. The oracle that carries only stored arguments rejects those 24. This is the measured price of reading the mint from the signature (Q3, Q11): sound, flag-backstopped, and less precise than a marker would be.
4. **The mint's over-approximation costs clean programs, modestly.** At 4 statements with the holder destructor the oracle rejects 54 fewer clean programs than 0212 (126 against 180: a transfer or reassignment of an argument the constructor only read) and 24 more faulting ones, so it accepts 30 more programs in all (`out/compare_v3l4_kdtor.txt`). At 5 statements with both destructors the figures are 1,734 fewer clean rejections and 738 more faults caught. In the corpora one construction site of 312 carries such an argument (the note, MEASURED).
5. **The destructor window is closed by §28.12, not by a 0212 rule.** With the child destructor and §28.12's timing 0212 leaves 0 faults; with r40's timing the baseline has 4,194 certain faults at 4 statements and 91,044 at 5, and 0212 leaves 4,122 and 86,850 of them (`out/compare_v3l4_qdtor_r40.txt`, `out/compare_v3l5_qdtor_r40.txt`), because the fault is in the runtime's flag timing and no static rule here addresses it.
6. **A release is a use at every release point, a drop-on-overwrite included.** The first version of the model's release rule counted the scope end and an explicit disposal only. The sweep at 5 statements with both destructors then left 6 races: `new a / link a / fmove b a / send a / asg b` — the reassignment of `b` runs the displaced child's destructor, which reads an owner already sent to another domain. §8.2's *Release* bullet names the three release points for that reason; with the overwrite counted the sweep leaves no race, and a self-test pins the program (`out/selftest0212.txt`).
7. **R2's owned-argument clause is needed.** A linked child moved out of one owner and stored into another brings its old owner along. The self-tests cover the three cases: declared in the wrong order (rejected), in the right order (clean), and the old owner then disposed (rejected at the new owner's release).

### 5.3 Probes (MEASURED, r40, `0212/probes/results.md`)

33 probes: d01–d12 (the destructor window), g01–g08 (erasure), a01–a08 (async), q01–q03, q05 and q06 (the open questions). Each records today's verdict and behaviour and the verdict 0212 gives. No probe tests 0212's rules: the compiler does not implement them.

### 5.4 What is argued, not modelled

- **R4, the callee side.** The model has one body. That a constructor's plain parameter reaches only the object under construction, and that a `scoped` parameter reaches only `this`, is the callee-side check §28.14 already runs for `scoped`.
- **R5's per-instantiation reading**, and its enumeration of owning positions. The generics matrix (`docs/research/generics-matrix`) has not been run against it. Proposal 0159 records that the same family, for values, "has produced a new route at each of four matrix runs" (QUOTED), so a route may remain; the reading is conservative in the direction that matters only if the list of owning positions is complete.
- **R1 through interfaces, inheritance and virtual dispatch**; **R7**; closures that capture an instance; `Shared<T>` handles.
- **The sibling order test** (R6): probes d06 and d07, no model.
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

Three stages, each one revision. Process §2 calls the last a **Ratchet**: its design is ruled by this proposal's acceptance, the stage-2 warning is its ramp, and its Migration note states both ends.

### 7.1 Stage 1 — the report (no verdict changes)

**Lands:** §3.6's row; §28.15 as the definition of the property and of the classification; §28.3.12's new clause 4 (§8.5). K4.3 and every admission stay as they are.

**What the mode prints.** At each field-held, collection-held and comparer-held line, the classification §28.15 gives the store:

- **bounded** — admitted as written: a constructor's store, a self-sourced store, a store through a `scoped` parameter or a setter, a standard collection's store into a location the body holds;
- **needs `scoped`** — a member that is not a constructor keeps a plain parameter; the line names the parameter;
- **no static form** — the line names the reason: the store relates two parameters; an `Object` parameter is kept by a member that is not a constructor; the holder is static storage or is reached through a borrow.

And one line, under the same mode, at each site that is not a store and that §28.15 would reject: an instance that would outlive a source; a temporary source; a transfer or reassignment of a carried source under a live instance; a use that a disposal of every carried source dominates; an owned conversion to a type that is not borrow-bearing, a generic instantiation that fails that requirement included; a `throw` of an instance that carries a source; a failed declared-before test.

The zero-report claim is unchanged. The stage adds one statement: **a unit whose report has no "needs `scoped`", no "no static form" and no non-store line compiles unchanged at stage 3.**

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

Case 33 is not a 0212 case; it is listed because the corpus lacks it beside `s28_12_0014` and r40 fails it.

**Existing cases stage 3 changes** (the note, MEASURED): 5 accepted-tier programs become expected-reject (§6); 12 take a `scoped` marker; 12 rule cases pin the report's text and 17 cite the field-held clause; 3 gtest files assert report lines; in the expected-reject tier and gtest, 19 more signatures take `scoped`, 8 distinct gtest stores relate two parameters, and 1 expected-reject case keeps an `Object` parameter.

---

## 8. Normative text

Edits are tagged with the stage that lands them: **[S1]**, **[S2]**, **[S3]**. An *anchor* is a phrase that occurs exactly once in the named document at this base (`0212/check_anchors.py` verifies each). "Replace from A through B" means the text beginning at anchor A and ending with anchor B, both included.

### 8.1 Spec §3.6 and the chapter table [S1]

*Insert a table row before the row beginning* `| **auto-hashable** | [§9.2](#sec-body-records) |`:

> ▲ | **borrow-bearing class** | [§28.15](#sec-borrow-bearing-classes) | every class and interface: whether its instances can hold a borrow — a class through a borrow field, an `owned` field of a borrow-bearing class, or a store member; an interface through a store member. It is distinct from **holds-borrows**, which classifies value types: a rule that reads "a value type that holds borrows" does not read a class |

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
> 2. an `owned` field, an owning-inline-array slot or an owning choice payload whose type is a borrow-bearing class;
> 3. a **store member**: a method, constructor or indexer with a class- or interface-typed `scoped` parameter ([§28.14](#sec-scoped-callback-parameters)), or a settable instance property or indexer, not `set(owned)`, whose type is a class or interface type that is not runtime-managed.
>
> An interface is borrow-bearing if it declares or inherits a store member. The standard library states the property for its own classes (stdlib [§1](Bezel_Standard_Library.md#sec-borrow-bearing-statements)). The property is the least one these clauses give: a class that reaches itself only through `owned` fields and declares no borrow field and no store member is not borrow-bearing. It is evaluated per instantiation ([§3.6](#sec-derived-type-properties) rule 2): a field declared at a type parameter is a borrow field at a class or interface substitution. A borrow-bearing class is not sendable ([§30.14](#sec-sendable-types)).
>
> **The source set.** An expression or binding whose value is an instance of a borrow-bearing class carries one source set, read off its provenance:
>
> 1. **A mint.** A constructor call, and a call whose declared return type is `owned` and whose result type is a borrow-bearing class or interface, yield an instance that carries what the call's receiver contributes and what each of its arguments contributes ([§28.3](#sec-argument-contributions)), whether or not the callee's body stores them: a caller reads the signature, never the body ([K2.4](#sec-kernel-k2)).
> 2. **A later store** adds sources and removes none. It is one of: an argument at a class- or interface-typed `scoped` parameter, which adds what the argument contributes to the constructed object or, for any other call, to the receiver; an assignment to a property or indexer of a class- or interface-typed value ([§28.3](#sec-borrow-lifetime-rules)'s *Property stores*), which adds to the receiver; an assignment `x.f := e` to a borrow field, which adds to the instance `x` names; a store position of a standard-library collection ([§28.11](#sec-borrowing-collections) *Value-copy members*), which adds each placed value's sources to the destination; and an `owned` argument that is itself an instance that carries sources, which adds them to the constructed object or the receiver.
> 3. **A read.** A borrow of the instance — an unmarked read of its binding, a borrow one of its members returns, an element read out of it — carries the binding's source and the instance's set ([K2.2](#sec-kernel-k2)). An owned value taken out of it by a replacing `move` ([§28.8.1](#sec-the-replacing-move)) carries the instance's set.
> 4. **An awaited result.** The value an `await` of a task yields, where the task's result type is a borrow-bearing class or interface, carries the task's carried sources ([K6](#sec-kernel-k6)).
> 5. A local's set is the union over the assignments that reach the read ([K7.2](#sec-kernel-k7)), and travels with a `move`. An instance minted from arguments that contribute nothing carries the empty set and is held wherever an owned class instance may be, static storage included.
>
> A later store is checked as an assignment of the updated instance to the location that holds it, the reading [§9.7](#sec-records-and-disposal) gives a member store into a record: at a local or parameter by [§28.3](#sec-borrow-lifetime-rules)'s local-scope bullet; at storage `this` owns — a borrow field of `this`, or an `owned` field or owning slot reached from `this` through `owned` hops — by *The callee side* below; at static storage by [§28.3](#sec-borrow-lifetime-rules)'s *Static stores*, which admits a static-storage source and no other. At any other location — an instance reached through a borrow, a plain parameter's object — the store is a compile error unless what it adds is static-storage-sourced: the body cannot see the binding that holds the instance.
>
> **The bound.** Every rule that bounds a borrow by its sources bounds an instance by its set:
>
> - **Scope.** The instance may not be bound to, or stored into an instance held by, a binding whose scope strictly encloses the scope of a source ([K2.3](#sec-kernel-k2)).
> - **Temporaries.** A fresh unbound owned temporary may not be a source of an instance that outlives its full expression ([K8.4](#sec-kernel-k8)); bind the source first, or let the class own it through an `owned` parameter. An instance that is itself consumed within the full expression is admitted.
> - **Returns.** An `owned` return of an instance is admitted where every source is one the callers re-establish: the receiver's contribution, a parameter's, or static storage. A source that is a scope-bound local, a `using var`, an `owned` parameter or a temporary is rejected at the return, as [K6.2](#sec-kernel-k6) rejects a returned frame.
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
> A borrow sourced at the receiver — `this`, a field of `this`, a borrow reached through one — may be stored into storage `this` owns in any member: an owner's child may hold a borrow of its owner. Where such a store places, in an `owned` field `f` of `this`, an instance that carries the value of an `owned` field `g` of `this`, and the instance's release may run user code (*The bound*, above), `g` must be declared before `f`: the cascade then disposes `f` first ([§28.4](#sec-the-destructor)). Every other store of a parameter- or receiver-sourced borrow is a compile error. An `owned` parameter that is an instance that carries sources may be stored where clause 3 admits a `scoped` parameter, in any member, and returned `owned`; its sources are the caller's to account (*The source set*, clause 2).
>
> In a generic declaration a store of a parameter declared at a type parameter is a requirement read at each instantiation that substitutes a class or interface type ([§3.6](#sec-derived-type-properties) rule 2): there the parameter must be `scoped` under clause 3, and the call takes the marker ([§22.3](#sec-parameter-modifiers)); at any other substitution the modifier and the marker have no effect, as `owned` has none ([§28.1](#sec-where-the-owned-modifier-can-appear)).
>
> **Owned erasure.** An owned instance whose source set is not empty may not reach an **owning position whose declared type is not borrow-bearing**: an `owned` field, an owning slot, an `owned` parameter, an `owned` return slot, an owned `ref` or `out` location or a task result declared at `Object`, at a class or interface that is not borrow-bearing, or at a type parameter an instantiation substitutes with one. The conversion is a compile error at the conversion site; the declared type announces no source, so no rule reading the position could bound the value — [§9.7](#sec-records-and-disposal)'s ground for a value that holds borrows, narrowed to owning positions. A borrowing position is not such a position: a borrow of the instance contributes its sources where it is passed (*The source set*, clause 3), and its callee may keep it only where a signature says so. A local binding is not such a position either: its set is read by flow, whatever its declared type.
>
> In a generic body, an owning conversion of a value whose type mentions a type parameter is a requirement of the body's use of that parameter ([§3.6](#sec-derived-type-properties) rule 2): at each instantiation at which the value's type, after substitution, is a borrow-bearing class, the target type, after substitution, must be borrow-bearing, or the instantiation is rejected where it is named. The requirement is read off the two types; the value's set is not consulted. A dispatched generic method ([§14.2](#sec-dispatched-generic-methods)) decides it at its body: the conversion is a compile error there unless the method's constraints establish that the parameter is not a borrow-bearing class, as `where T: value` and `where T: sendable` do.
>
> **Teardown.** An instance that `this` owns and that carries `this` is released inside the disposal of `this`, whose accesses run on a live instance ([§28.12](#sec-asynchronous-disposal)); its stored borrow of its owner is therefore valid for its whole life, its destructor included. An owned value taken out of `this` carries `this` (*The source set*) and is bounded as any instance is. A destructor that reaches, through its owner, an owned field the cascade has already disposed meets that field's flag ([§28.12](#sec-asynchronous-disposal)): a slot kill, [K5.6](#sec-kernel-k5)'s posture, and no recorded trust.
>
> **Frames.** A deferred-activation frame carries what its arguments and captures contribute, an instance's sources among them ([§28.3](#sec-argument-contributions)). A local instance in an `async` body may be live across a suspension: every source it can carry is frame-resident or is one the frame already carries ([§30.1](#sec-async-methods)).
>
> **Staging.** In this revision the rules of this section define the classification the trust-admission report prints ([§28.3](#sec-diagnostics), clause 4) and change no verdict: [K4.3](#sec-kernel-k4)'s field-held, collection-held and comparer-held clauses admit what they admitted.
>
> *Example.* (the `Tag` example of this proposal's §2, with its comments)

**[S2]** *Replace the Staging paragraph with:*

> ▲ **Staging.** In this revision a site this section rejects, and that [K4.3](#sec-kernel-k4)'s field-held, collection-held or comparer-held clause admits, compiles and draws a warning that names this section's rule and its rewrite. A store that [§28.14](#sec-scoped-callback-parameters) admits only since this revision — a class-typed `scoped` argument against a location its bounding forms do not resolve — is bounded by this section in full: for it the rules above are errors. The trust-admission report prints the same classification ([§28.3](#sec-diagnostics), clause 4).

**[S3]** *Delete the Staging paragraph.* Two phrases of the text above also wait for stage 3, because the rules they cite land then: the opening paragraph's "([K2](#sec-kernel-k2) clause 5)", and the property paragraph's last sentence, "A borrow-bearing class is not sendable", which lands with §30.14's (§8.8).

### 8.3 Spec §28.3.1, the kernel [S3]

**K2.3.** *Replace the sentence beginning* `Where they are not, an admission is made only under K4's posture:` *and ending* `[§28.11](#sec-collection-element-ownership)).` *with:*

> ▲ Where they are not — a pending frame's carried sources against what its caller does before settlement ([K6.4](#sec-kernel-k6)) — an admission is made only under K4's posture: a structural, decidable check at the site plus a recorded residual obligation with a named backstop ([§30.1](#sec-async-methods), [§28.14](#sec-scoped-callback-parameters)). A borrow stored in an object is never such an admission: the object's class is borrow-bearing and the instance carries the borrow's sources (clause 5).

**K2, a new clause 5.** *Insert before* `*Example.* A plain return must name a source every caller re-establishes (K2.4)`:

> ▲ 5. **Stored borrows are carried.** A value that stores a borrow carries the borrow's sources as its own source set, and every clause above reads it by that set: a value that holds borrows ([§9.7](#sec-records-and-disposal)), a function value ([§28.3](#sec-borrow-lifetime-rules)'s closure bullet), a frame ([K6](#sec-kernel-k6)), and an instance of a borrow-bearing class ([§28.15](#sec-borrow-bearing-classes)), whose set a mint reads off the constructing call's signature and a later store adds to.

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

> ▲  locals that hold an instance of a borrow-bearing class ([§28.15](#sec-borrow-bearing-classes)), each a live local borrow of every source the instance carries, its release counting as a use where it may run user code,

**K8.4.** *Replace the sentence beginning* `A plain parameter announces none: a callee that stores it holds the borrow under the borrowed-field rule's` *and ending* `at its boundary (K8.1).` *with:*

> ▲ A plain parameter announces none, and its callee may not keep it ([§28.15](#sec-borrow-bearing-classes) *The callee side*) — a constructor's excepted: a constructor call of a borrow-bearing class is a mint, the constructed instance carries the argument, and a fresh unbound owned temporary there is rejected unless the instance is itself consumed within the full expression.

### 8.4 Spec §28.3.5, *The sources an argument contributes*, and a capture's [S3]

*Replace* `1. a class- or interface-typed argument contributes its source set;` *with:*

> ▲ 1. a class- or interface-typed argument contributes its source set, and, where its value is an instance of a borrow-bearing class ([§28.15](#sec-borrow-bearing-classes)), the sources the instance carries;

*Replace* `5. an `owned` argument, and a `null`-literal, other value-typed or other runtime-managed argument, contributes nothing.` *with:*

> ▲ 5. an `owned` argument contributes no borrow of itself; where its value is an instance of a borrow-bearing class it contributes the sources the instance carries ([§28.15](#sec-borrow-bearing-classes) *The source set*, clause 2). A `null`-literal, other value-typed or other runtime-managed argument contributes nothing.

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

> ▲ a closure that captures such a holder, an instance of a borrow-bearing class that carries it ([§28.15](#sec-borrow-bearing-classes)), or an iterator, enumerator or lazy-query frame that carries it

*and append to that sentence's list of uses (*"…enumerating or disposing the frame"*):* ▲ `, releasing an instance whose release is a use`.

**§9.7, the instance-field bullet.** *Replace the sentence beginning* `A class instance's lifetime is not bounded by any borrow source's scope, and a **direct** class-typed borrow field` *and ending* `flag as its backstop.` *with:*

> ▲ A **direct** class-typed borrow field makes its class borrow-bearing, and its instances carry the borrow's sources ([§28.15](#sec-borrow-bearing-classes)); a record-carried borrow stays out of class fields on this section's own posture.

*(This proposal does not lift the instance-field ban for records or spans; `DECLINED.md`'s deferred row for that retirement stands, with its bar.)*

**§30.1.** *Insert before* `*Example.* Awaiting before the source's scope ends discharges the settlement obligation; abandoning the task leaves it to the caller's trust:`:

> ▲ **Instances that carry sources, in a frame (derives: [K6](#sec-kernel-k6) + [K2](#sec-kernel-k2) clause 5).** An argument or capture that is an instance of a borrow-bearing class ([§28.15](#sec-borrow-bearing-classes)) contributes its binding's source and the sources it carries to the frame, and an `owned` one the sources it carries; the obligation above reaches each. A local instance built in an `async` body may be live across a suspension point, whatever it carries: a borrow formed in the body is sourced at a location the frame owns or at one of the frame's own carried sources, so the instance holds nothing the frame does not already hold, and the rule above for a value type that holds borrows is not extended to it. The value an `await` yields, where the task's result type is a borrow-bearing class or interface, carries the task's carried sources.

**§30.14.** *Append to the sendability rules' statement on class fields:* ▲ `A borrow-bearing class ([§28.15](#sec-borrow-bearing-classes)) is not sendable.` (For a class with a borrow field or an `owned` field of a borrow-bearing class the existing rules already say so — the note's probe p16; the sentence makes `where T: sendable` establish R5's requirement for a class that is borrow-bearing only by a store member.)

**Appendix C.3.** *Insert after the bullet beginning* `- A choice that holds borrows ([§35.4](#sec-choice-classification)'s property, whatever the choice's classification) is subject to`:

> ▲ - An instance of a borrow-bearing class carries the sources of the borrows it stores: it is bounded by scope, by the no-temporary-source rule and by the live-local freeze, it is not converted at an owning position to a type that is not borrow-bearing, and a member that is not a constructor keeps a plain parameter only where it is declared `scoped` ([§28.15](#sec-borrow-bearing-classes); [K2](#sec-kernel-k2) clause 5 + [K5.5](#sec-kernel-k5)).

**Stdlib §1 [S1; its sentence on rejection at the site lands at S3, with R5].** *Insert after the sendability-statements paragraph (anchor `sec-sendability-statements`):*

> ▲ `<a id="sec-borrow-bearing-statements"></a>` **Borrow-bearing statements** (language spec [§28.15](Bezel_Language_Spec.md#sec-borrow-bearing-classes)). A standard-library class is a borrow-bearing class as follows. A **borrowing** collection is borrow-bearing at an instantiation whose element type — for a map, its key or value type — is a class or interface type that is not runtime-managed. `Dictionary<K, V>`, `OwningDictionary<K, V>`, `Set<T>` and `OwningSet<T>`, which have a constructor that retains a comparer, are borrow-bearing at every instantiation; an instance built without a comparer and holding no borrowed element carries the empty set. An **owning** collection, `Shared<T>`, `Weak<T>`, `Locked<T>` and `RwLocked<T>` are borrow-bearing at an instantiation whose owned type argument is a borrow-bearing class. No other standard-library class is borrow-bearing, and a standard-library generic instantiated at a borrow-bearing class in a position where it takes `owned T` is rejected at the site unless its entry says otherwise. `Task<T>` and `OwningTask<T>` are not such a case: a task is a frame, and the value an `await` of it yields carries the frame's sources (language spec §28.15). The store positions are those of language spec §28.11's *Value-copy members*.

**Stdlib §4, *A retained comparer is a borrow* [S3].** *Replace from* `**A retained comparer is a borrow.**` *through* `The comparerless constructors retain nothing.` *with:*

> ▲ **A retained comparer is a borrow.** The `comparer` constructors of `Dictionary<K, V>`, `OwningDictionary<K, V>`, `Set<T>` and `OwningSet<T>` ([§4.8](#sec-dictionaryk-v)–[§4.10](#sec-sett-and-ownedsett)) take a plain parameter and retain it: every later lookup consults it. The constructed collection carries the comparer's sources ([§1](#sec-borrow-bearing-statements); language spec [§28.15](Bezel_Language_Spec.md#sec-borrow-bearing-classes)): it may not outlive the comparer, and the comparer may not be transferred while the collection is live. The comparerless constructors retain nothing.

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
> **What was not done.** Lifetime parameters (the precision ceiling; nine measured sites over this design, at the price of a second parameter system), a freeze of explicit `Dispose()` under a live instance (§5.1's recorded boundary stands), a ban on destructors reading a stored borrow (it retires flush-on-dispose and is not complete), and the retirement of the frame-held clause.

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

New rows (`declined-at-proposal`; authoritative record: this proposal, the cited section):

| id | topic | reason | reopening bar |
|---|---|---|---|
| D1 | A per-field marker that keeps the field-held trust admission for that field (§4.3) | It keeps K4.3's clause, and the domain race no flag catches, under another name; every measured store has a static rewrite. | A measured count, from the stage-1 report over programs that are not tests, of stores classified "no static form" that the rewrites of §6 do not express. |
| D2 | A `scoped` parameter that names another parameter as its bounding object (§4.2) | The receiver form, or a store at the caller, expresses every measured site (9), and the marker is the first step of a second parameter system. | A measured helper that must be a static method and whose rewrite as an instance or extension member is not available. |
| D3 | A declared borrow-bearing interface, for interfaces whose implementations store only in constructors (§4.1) | §3.6 rule 1: a derived property is not declared; 0 measured owned upcasts; the concrete return type is the rewrite. | A measured program that must return an owned borrow-bearing instance at an interface with no store member. |
| D4 | Rejecting every generic instantiation at a borrow-bearing class unless the generic opts in (§3.1) | It needs a new constraint form and rejects generics that keep their parameter, which are sound. | —(none stated) |
| D5 | A rule that a destructor may not read a borrow field; a rule that a self-sourced holder be trivial-dispose (§3.2) | The first retires flush-on-dispose and is incomplete; the second rejects every tree with parent pointers and does not bound a subclass. | —(none stated) |
| D6 | Importing §30.1's ban on a holds-borrows local across a suspension for a class instance (§3.3) | The frame already carries every source such an instance can hold; the plain alias beside it is admitted. | The retirement of the frame-held clause itself (a strict mode). |

The note's candidates C (lifetime parameters) and D (freezing explicit disposal) are covered by existing rows — *Compile-time borrow-liveness checking* stands — and need no new row.

### 8.11 Guide 01 [S3; the pin moves to the landing revision]

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
> An explicit `acct.Dispose()` while `stmt` is alive is allowed, as it is under any borrow; a use of `stmt` that always follows it is a compile error, and one that follows it on some paths is checked mode's `DisposedAccessException`. A registry that must outlive the things it refers to does not hold borrows of them: it owns them, keys them, or observes them through `Weak<T>` (spec §28.13).

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

The edits of §8.3–§8.12 cover the defining occurrences. The remaining ones are of four kinds, each with one instruction, and are composed by the stage-3 train against its own head (process §4: a train re-verifies every anchor before composing): (a) a citation of the field-held clause as the reason a class-typed borrow is admitted somewhere — re-cite §28.15; (b) "admitted under the residual" in a worked example (`Keep<Item>`, `Bag<Item>`, `_all.AddRange(items)`) — state the §28.15 verdict: a constructor or `scoped` store is admitted, a plain method store is not; (c) the report's description of what it names — drop the three clauses; (d) §9.7's collection bullet, whose "collection-held clause" for a record-carried borrow in a local collection becomes the collection's carried set. §28.3.9's *Scope* clause 1 and the slot-displaced entry (proposal 0210) are untouched: they keep the domain clause through the re-homed clause 4 of §8.5.

### 8.14 Changelog and Migration (sketch for the stage-3 entry)

- **Newly rejected:** a member that is not a constructor storing a plain class- or interface-typed parameter (write `scoped`); a store into an object the member does not hold (store at the caller, or the receiver form); a transfer or reassignment of a source under a live instance; an instance bound in a wider scope than a source; an instance built over a fresh temporary and bound; an owned conversion of an instance that carries sources to a type that is not borrow-bearing, in a generic body per instantiation; a `throw` of such an instance; a field that borrows a sibling declared after it, where its release may run user code.
- **Newly admitted** (already at stage 2): a class-typed `scoped` argument against a plain `var` holder, a moved holder, and a holder placed in an `owned` field of the bounding object; `scoped` at a parameter declared at a type parameter.
- **Newly defined:** the source set of a class instance; what an awaited result carries.
- **No longer warned:** the report's field-held, collection-held and comparer-held lines.
- **Ramp:** §7.3's paragraph.
- **Spec-ahead-of-impl:** §10's trackers.

---

## 9. Records this proposal answers

A change is a design change the moment it contradicts a recorded decision, and such a decision is reopened only with new evidence against its stated reasons (process §2).

**The callee-side no-store rule** (`DECLINED.md`; rationale §4.2.6; proposal 0146 §3.4). *Stated reasons* (QUOTED): the rule "retires the back-reference idiom", whose named form is `constructor(logger: ILogger) _logger := logger;`, and its reach was "every class-typed-or-interface borrow stored from a parameter across the repository, for a hazard that only a boxed value carries". *New evidence.* (1) The idiom's named form is a constructor, and under R2 a constructor's store has a carrier: it is untouched, with no marker. MEASURED: 93 of the 157 stores are constructor stores; 44 of 58 accepted-tier stores are admitted as written. (2) The hazard is not a box's: at one model bound 2,016 of 26,031 accepted programs read a dead or foreign object through a stored borrow, 540 of them a race no flag catches (MEASURED, §5.2). (3) The cost is measured: 12 accepted-tier signatures take `scoped`, 5 programs are rewritten. The half of that row that keys a rule on an interface's implementers is not reopened.

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
| `scoped` widening | `MethodOwnership::scopedBoundingLocal`, `checkScopedAdmission`, `checkScopedFreeze` | a class-typed argument is no longer rejected when no bounding form resolves; it becomes a later store |

**Cost** (HYPOTHESIS; the compiler was not built). R1 is a per-class fixed point of the kind trivial-dispose and sendable already run. R2–R3 add no pass: they extend the per-body dataflow that already carries source sets for records, closures and frames. R4 replaces an admission with a check in the same visitor. R5's generic half is one recorded pair per conversion and one evaluation per instantiation.

**Prerequisites: gaps r40 has against v0.355** (MEASURED).

1. Proposal 0205's borrow and holder legs are not implemented (the note's p07, p05). R3's dominated-use error is that rule read for an instance.
2. K5.5's frame-local freeze is not implemented (the note's p26). R3's freeze is the same code path for one more holder kind.
3. §28.12's live-instance rule (#7506) is not implemented: the flag is set when `Dispose()` begins (d01–d03, d11). Until it is, every back-pointing child with a destructor that reads its owner faults at teardown in checked mode — today as well as under 0212.
4. Smaller ones met while probing: a `scoped` parameter may be forwarded only to another `scoped` parameter and not passed to a standard collection's store, which §28.14 calls an ordinary borrow argument (q06; the same probe shows `scoped` rejected at a type-parameter-typed parameter, which is the spec's rule today and which §8.7 changes); the replacing `move` is not implemented (g08's first form); an explicit `Dispose()` of a non-nullable owned field in a destructor is rejected as a double free, against §28.8.1 (d02's first form); a member access through a class-typed `scoped` parameter is rejected with the callback rule's message (q05).

**An order for implementation lanes.**

1. The three prerequisites above, each its own lane (0205's legs first: every later step reports through it).
2. R1 and the report's classification (stage 1). No verdict changes; it can be gated on the conformance cases of §7.4 in report mode.
3. The consumer half: R2's mint and R3, as warnings (stage 2's non-store lines). It rejects nothing in the accepted tier (the note, MEASURED: 0 programs kill a source under a live holder).
4. R4's classification as warnings; the `scoped` widening (which only admits more).
5. R5 and R6 as warnings.
6. The flip to errors (stage 3), with the corpus migration of §7.4.

---

## 11. Owner questions

| # | question | recommendation |
|---|---|---|
| Q1 | Accept the direction: a class that stores a borrow is borrow-bearing, its instances carry their sources, and K4.3's field-, collection- and comparer-held clauses are retired through the three-stage ramp? | **Yes.** |
| Q2 | Reverse the callee-side decline in full for members that are not constructors, or keep a per-field opt-out that preserves the trust admission? | **In full, no opt-out** (§4.3). If the owner prefers one: a field modifier that exempts its stores from R4 and its class's instances from carrying what that field holds; the report then lists exactly the marked fields, and the domain clause stays for them. It should be decided now, not after stage 2, because it changes which K4.3 text stage 3 deletes. |
| Q3 | Does the mint read the signature (every argument carried, no marker on constructors), or must each stored parameter be `scoped`? | **The signature.** It needs 12 markers where the explicit form needs 53 (the note, MEASURED). Its price is measured: at one model bound 30 clean programs rejected and 24 certain faults left to the flag that the explicit form would reject (§5.2 items 3–4). |
| Q4 | Is an interface borrow-bearing by derivation from its store members, by declaration, or never? | **By derivation** (§4.1). A declared form is declined with a bar. |
| Q5 | Does a store that relates two parameters get a spelling? | **No**; the storing object is the receiver, or the store is made at the caller (§4.2). |
| Q6 | A class-typed `scoped` argument today freezes explicit disposal of its source (K5.7). Keep that where the bounding object is a `using var` local or `this`, and use the unfrozen posture (K4.2) for the locations 0212 newly admits — or one posture everywhere? | **Keep it where it exists.** No verdict that is static today becomes flag-backstopped. The cost is two postures for one keyword. |
| Q7 | A member that is not a constructor cannot keep an `Object` parameter (`scoped` is not admitted at `Object`). Leave it without a spelling? | **Yes**; declare the parameter at the stored type (2 sites). |
| Q8 | A user generic container's `Push(item: T)` needs `scoped item: T` — which means admitting `scoped` at a type parameter, new in §22.3 and §28.14 (§8.7; r40 rejects it today, probe q06) — and the marker at class instantiations, while a standard collection's `Add` needs neither. Accept the admission and the asymmetry, or make such a parameter marker-free at the call? | **Admit `scoped` at a type parameter; accept the asymmetry for stage 1 and measure.** The corpora name such a container at a class type nowhere (42 stores, all at value instantiations). The marker-free variant is a small follow-up if the report shows the pattern. |
| Q9 | Who gates each stage, on what? | Stage 1 with acceptance; stage 2 after stage 1 and its prerequisites are implemented and the report has been read on the in-tree programs; stage 3 after the corpus migration and a report with no unexpressed "no static form" site (§7.3). |
| Q10 | Should a weak reference over an ordinary owned object (the prior lane's design) be pursued as the companion for registries? | **As a separate proposal, after stage 1's data.** Reordering, owning with keys, and `Weak<T>` over `Shared<T>` cover the measured cases. |
| Q11 | For an instance that carries several sources, should §28.2's dominated-use rule fire when *every* source is disposed (today's letter for a record) or when *any* is? | **Every.** "Any" rejects a clean program that disposes an argument the constructor only read; "every" leaves a certain fault to the flag when the stored one of two is disposed (24 programs at one bound). Both are sound. |
| Q12 | The note's ruling S1 (which bounding form `_child := new Child(scoped this)` resolves). | **Moot under 0212**: a constructor call is a mint for a class-typed argument and resolves no form (§8.7). If 0212 is declined, S1 is still owed. |

---

## 12. Self-critique

Written by the author, not an independent critic. What a critic should attack first, in the author's order of doubt:

1. **R5's list of owning positions.** The soundness of erasure rests on the list being complete. A position this draft may have missed lets a set be dropped silently. Candidates to check: a `yield return` in an owning iterator whose element type is not borrow-bearing; an `out owned` location; a choice payload; `Shared<T>`'s `Clone()`; a task-completion source's result slot.
2. **R4's "purely local" clause.** A body-local instance that receives a plain parameter's borrow and is then stored into `this` by a member that is not a constructor must be caught as a store of a parameter-sourced borrow into `this`. The draft says so ("an instance that carries it"); the check needs the instance's set inside the callee, which is the same dataflow.
3. **Two shapes real code writes and the corpora do not** (HYPOTHESIS). *The decorator over a borrowed inner object returned at its base type*, `static method Wrap(s: Stream): owned Stream => new Buffered(s)`: R5 rejects it unless the base declares a store member; the rewrites are the concrete return type or an owning wrapper. *Index, then move*: `byName.Add(it.Name, it); owning.Add(move it);` with `byName` used later — the transfer is frozen, and the rewrite is the freeze's usual one, move first and index the borrow read back from the new owner. Stage 1's report is what counts both.
4. **The over-approximation on `owned` arguments.** R2 adds an owned instance's sources to the receiver of *any* instance call it is passed to, so passing an owned holder to a method of a borrowed object is rejected even where the callee only consumes it. No corpus site meets it (0 owned-holder arguments, MEASURED). Real code may.
5. **Clause 3 of R1 on classes.** A class is borrow-bearing if it declares a `scoped` class-typed parameter even where it stores nothing. That is conservative and makes `sendable` exclude it by a new sentence in §30.14.
6. **Two postures for `scoped`** (Q6) is a wart the owner may prefer to remove.
7. **The scanner's R5 pass does not follow a forwarded value** into its callee, and reads one unit at a time.
8. **§8.13 is a list, not an edit set.** The defining text is exact and anchored; the consequential mentions are enumerated and classified, not rewritten.
9. **No number here is from real programs.** Every corpus is a test corpus.

**What could not be done.** The compiler was not built or modified, so no 0212 rule was run and its cost is unmeasured. The generics matrix was not run against R5. No independent critique was run. `OwningSlotMap<T>` is not implemented in r40, so that registry rewrite was not probed. The exact text of the consequential mentions (§8.13) and of the revised §28.3.8 examples is left to the stage-3 train.

---

## 13. Files

Under `docs/research/static-ownership-tracking/0212/`:

- `README.md` — an index of this directory.
- `probes/` — 33 programs (`d*`, `g*`, `a*`, `q*`), `run_all.sh`, `results.md` (r40, three modes).
- `model/bzcore_0212.py` — the model; `model/run_sweeps.sh`, `model/breakdown.py`; `model/out/` — the self-tests, every sweep of §5.2, and the per-rule breakdowns.
- `data/scan_0212.py` — the corpus scanner; `data/scan_collection_transfer.py` — one extra count; `data/out/` — `summary_0212.txt`, `collection_transfer.txt`, the per-pass tables, and `base/`, the note's scanner re-run.
- `check_anchors.py` — verifies that each `old` anchor of §8 occurs exactly once at this base, and lists the consequential mentions.
