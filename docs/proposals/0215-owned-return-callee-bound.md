# Proposal 0215 — The callee's bound: an owned-return instance method stores a `scoped` argument only where both of its call-site bounds sever it

| | |
|---|---|
| **Status** | under-review — critiqued (pc-0215, SOUND-WITH-FIXES), revised: drafted 2026-11-26 from the v0.361 L2 lane's design finding on #9275; critique pass run (pc-0215, 2026-11-26), both blocking findings and the ten others applied (§12); owner decision pending |
| **Tracking issue** | [#9275](https://github.com/s8lap/bezel-lang/issues/9275); related: [#8397](https://github.com/s8lap/bezel-lang/issues/8397) (ruled v0.361, the registration-lifetime residual and form 1 composing with form 3), [#9311](https://github.com/s8lap/bezel-lang/issues/9311) (ruled v0.361, bound 6 stays deferred) |
| **Target documents** | spec §28.14 (*The `scoped` modifier*, the callee bullet's first sentence; *The registration-lifetime residual*, one sentence; a new paragraph *The callee's bound* before *Quiescence*; *Class- and interface-typed `scoped` parameters*, the callee-side bullet, one sentence), §28.3 axiom register (row A11), Appendix C.3 (one bullet). stdlib §6.1 (`Register`, one sentence), §6.5 (*Subscribing*, one sentence). rationale §5.26 (the A11 entry of *The axioms' reasons*, one paragraph). Guide 1 §9 (one sentence). Code-generation reference §4 (*`scoped`*, one sentence) and §7.2 (one entry). No grammar change, no new keyword, no interface-artifact (§2.8.2) change. |
| **Written against** | spec, stdlib and rationale **v0.361**, `origin/main` at `cdd2022c7` (v0.361 landed). Every `old` anchor in §7 was re-verified there: each occurs exactly once. The first draft was written against the v0.361 branch at `c85ce271c`; the documents differ from `cdd2022c7` in none of the anchored passages. Section numbers are v0.361's. |
| **Origin** | The v0.361 L2 lane (`/var/tmp/bezel-scratch/v361/lanes/l2.md`, the #9275 section) classified #9275 as a design question, not a ruling, and recommended a callee-side rule; critic A confirmed nothing of it slipped into v0.361 (`critic-a.md`, L2). The non-author critique pc-0215 (`/var/tmp/bezel-scratch/v361/pc-0215.md`) found two blocking defects in the first draft, both fixed here. |
| **Change class** | Design change ([`spec-change-process.md`](spec-change-process.md) §2): it newly rejects callee bodies the text and the compiler admit, and it decides what "the bounding object" means at a callee, which no text states. It is an extension of axiom A11, not a ratchet. |

*Conventions. An unprefixed § is a language-spec section at v0.361; stdlib, rationale and guide sections are marked. Evidence labels: MEASURED (a probe run or a scripted count, reproducible from §11), QUOTED (repository text at the cited place), HYPOTHESIS (reasoning nothing here measures). ▲ marks proposed normative text. "The forms" are §28.14's bounding-object forms 1–5. "Severed by X" means: after X's teardown completes, the stored callback is never invoked again (for a class-typed borrow: the storing object is disposed no later than X). "Mint-announcing" is §28.3.5's term: a `Task`-family or iterable-family declared return type.*

## 0. Summary

§28.14 states the callee's storage obligation against "the bounding object", and resolves the bounding object at each call site by the forms. At an instance method whose return is `owned`, call sites resolve two different objects: the returned object (forms 2 and 3) or the receiver (forms 4 and 5). The text never says which of them the callee's body is checked against. The checker uses `this`, so a method that stores the callback in a `scoped` field of `this` and returns an unrelated handle is admitted, and a caller that bounds its captures by the handle (`using var tk := k.Keep(lambda … c …)`) gets a use-after-free (MEASURED: SEGV). The same hole is reachable through interface dispatch, where no caller can see the body: a user implementation of `IEvent<T>.Subscribe` that keeps the handler on `this` breaks stdlib §6.5's observer pattern (MEASURED: SEGV). It is also reachable in the stdlib's own producer spelling, `_changed += h; return new Token();` (HYPOTHESIS by reading HEAD; pc-0215).

Every caller-side repair either rejects programs the text admits and stdlib §6.5 calls normative, or needs a body-derived fact carried in the artifact interface, which `DECLINED.md` records against. The repair belongs at the callee, and it has one sound shape: the callee is checked against every object its call sites can name. This proposal names that set the callee's bound:

| callee | call-site forms that can resolve | the callee's bound |
|---|---|---|
| constructor | 1 | the constructed object (`this`) |
| instance or extension method, return not `owned` | 4, 5 | the receiver (`this`; for an extension member, the extended value) |
| static method (return `owned`, required) | 2, 3 | the returned object |
| lambda with a `scoped` parameter | 2, 3 (a function-value invocation names no receiver) | the returned object |
| instance or extension method, return `owned` | 2, 3 and 4, 5 | both the receiver and the returned object |

The bound is read at the member's own declaration and at every declaration it implements or overrides, as §28.3.5 reads the announcement table (F2 of the critique). Each store and each forward must be severed by every object of the bound. A callee whose declared return is mint-announcing (`async`, iterator) returns a frame, so nothing in its body is severed by "the returned object" (F1).

At an owned-return instance method that leaves exactly one shape: a forward that is the method's returned value, on `this` or on a member chain rooted at `this` through `owned` fields. That is the documented hub idiom, `OnChanged(scoped h): owned Subscription => _changed.Subscribe(h);`, and it stays admitted at every caller form. The `Keep` shape, its `+=` and `.Add` spellings, the interface variant, the class-typed variant, the instance-method factories and every `async`/iterator owned-return callee that keeps a callback are rejected at the callee. No caller verdict changes.

The cost is stated plainly: a user type can no longer implement `IEvent<T>` with subscriber storage of its own, even behind a handle that severs it. It wraps an owned `EventSource<T>` instead (§3.6, Q1).

The L2 lane's wording ("may store the callback only as a forward whose result is the value it returns") is this rule's return half. Read alone it is unsound at forms 4 and 5 (§3.3 shows two counterexamples), so the recommendation keeps the existing forward clause's `this` half beside it.

A scan of the 12,084 tracked `.bzl` files and the tracked C++ sources finds 37 owned-return instance methods with a `scoped` parameter (MEASURED, §5.1; pc-0215's `scan3.py` corrects the first draft's 33). None stores the parameter in a way this rule rejects. The core rule flips no test. The optional static half (§4.2, Q2) flips one gtest row from reject to accept, and the liftable bound in §4.1 decides one corpus case (Q3).

## 1. The problem

### 1.1 The text

QUOTED, §28.14 *The `scoped` modifier* (v0.361):

> The callee may store the callback, but only in `scoped` fields of the bounding object itself (below), or forward it to another `scoped` parameter whose bounding object is the same object; it may invoke it. It may not return it, move it into non-`scoped` storage, or store it in any other object. […] These obligations are compiler-verified for user-authored callees. A `scoped` parameter on a static method is admitted only when the method's return is `owned`, so that a call site can resolve bounding form 2 or 3 […]

> The forms are tested in order; the first that resolves determines the bounding object

The bounding object is a fact of a call site. The callee's obligation uses the same words, "the bounding object itself", but a callee body has no call site. For a constructor (form 1) and for an instance method with no `owned` return (forms 4/5 only), every call site names `this`, so the reading is forced. For a static method, every call site that admits anything names the returned object (forms 2/3), and the text says so. For an instance method with an `owned` return, call sites name either, and the text is silent.

QUOTED, the checker (`src/ownership/Ownership_Checks.cpp`, `scopedCalleeWalkStmt` and `scopedCalleeWalkExpr` at `bd12fe038`; confirmed by pc-0215's reading of HEAD):

- a store `_h := h` into a `scoped` field is admitted when the target is a field of `this` ("Store INTO a scoped field of `this`: RHS must be a scoped parameter of this method");
- a forward is admitted when the inner call's bounding object, resolved by the forms inside the callee, is `this` (the #9274 forward test);
- #9274's `+=` arm admits `lhs += h` whenever the desugared call's chain head is `this`;
- every forward into a constructor is rejected (the `NewExpr` arm: "Forwarding into a CONSTRUCTOR is never same-object").

None of these tests the callee's kind or return. So the checker reads the callee's bounding object as `this` for every instance method, owned return or not.

### 1.2 The repro, traced on every caller path

The issue's program (probe `p1_keeper.bzl`, §11):

```bezel
public class Keeper
  private scoped _h: ((double) -> ())?;
  public method Keep(scoped h: (double) -> ()): owned Token2
    _h := h;                 // admitted today: a scoped field of `this`
    return new Token2();
  end Keep;
  public method Fire(t: double)
    _h?(t);
  end Fire;
end Keeper;
```

Callee side: `_h := h` is a store into a `scoped` field of `this`. Under the checker's reading it is admitted. Under a reading where the callee's bound is the returned object, it is rejected. The text does not choose.

Caller side, one call per form:

- Form 2, `using var k := new Keeper(); do using var c := new Counter2(); using var tk := k.Keep(lambda(t) => c.Hit(t)); end; k.Fire(2.0);`. The bound is `tk`, and `c` is declared before `tk`, so the call is admitted. At `end`, `tk` then `c` are disposed. `k._h` still holds the closure, and `k.Fire(2.0)` invokes it over the freed `c`. MEASURED: SEGV in both disposal modes (r42 build, `p1_keeper`).
- Form 3, `_tk := k.Keep(lambda(t) => this.Hit(t))` in a member of `Owner`, with `k` a parameter or a borrowed field. The bound is `Owner`'s `this`, and the only capture is `this`, so the call is admitted. When `Owner` is disposed, `_tk` goes with it, but `k._h` keeps the closure over the freed `Owner`. HYPOTHESIS: the same use-after-free as form 2. It is not probed, because it is the same storage fact.
- Forms 4/5, `using var c := new Counter2(); using var k := new Keeper(); var t := k.Keep(lambda(t) => c.Hit(t));`. The bound is `k`, and `c` is declared before `k`. `k` is disposed before `c`, and the closure dies with `k._h`. Sound.

So the store is sound exactly where the call site names the receiver, and unsound where it names the returned object.

### 1.3 The mirrors

Each of these was probed with the r42 build (§11) unless marked. The verdict column is what r42 does. The HEAD column comes from reading `bd12fe038`'s checker, confirmed by pc-0215.

| probe | shape | r42 | HEAD (reading) | dangling path |
|---|---|---|---|---|
| `p2_oncancel` | instance `OnCancel(tok, scoped cb): owned Subscription => tok.Register(cb)` | compiles; prints `hit 32566` (a read of freed memory), then aborts | rejected by the #9274 forward test (the inner call resolves `tok`, not `this`), pinned by `forwarded_lambda_to_token_register_spec_28_14` | form 5: `keep := r.OnCancel(src.Token, lambda() => c.Hit())` bounds by `r`; `keep` outlives `r` and `c`; `src.Cancel()` fires into `c` |
| `p3_iface` | `class Ticker: IEvent<double>` whose `Subscribe` stores the handler in a `scoped` field of `this` and returns `_dummy.Subscribe(lambda(x) => ())` | compiles; SEGV, both modes | admitted (the same store path as `Keep`) | form 2 through the interface face: `using var sub := face.Subscribe(lambda(t) => c.Hit(t))` |
| `p5_classtyped` | `Hold(scoped src: Item): owned Token2` storing `_src := src` into a plain field of `this` | compiles; default mode exits 0, checked mode `DisposedAccessException` | admitted (`exprIsReceiverPlainFieldOfThis`) | form 2: `using var tk := sh.Hold(scoped it)`; `sh.Read()` after `it` is disposed |
| `p9_instance_factory` | instance `Make(scoped on): owned Button => new Button(on)` | compiles; SEGV, both modes | rejected (every constructor forward is) | form 5: `keep := f.Make(lambda() => c.Hit())` bounds by `f`; `keep` outlives `c` |
| (pc-0215, not probed) | `Keep(scoped h): owned Token  _changed += h; return new Token(); end`, and the `_changed.Add(h);` twin | rejected (r42 predates #9274) | admitted (the #9274 `+=` arm and the `CallExpr` forward arm) | form 2, as for `p1` |
| `m2_marked` (pc-0215) | `Watch(scoped h): owned IEnumerable<int>` storing `_h := h; yield return 1;` | compiles; SEGV | admitted (the store path) | form 2: `using var it := tk.Watch(lambda … c …)` |
| (pc-0215, HYPOTHESIS) | `async SubAsync(scoped h): owned OwningTask<Subscription>  return _changed.Subscribe(h);` | rejected (pre-#9274) | admitted (the #9274 form-4 forward test never checks the method kind) | form 2 binds by the task `t` (MEASURED, `a1_async_form2`); `keep := await t;` moves the `Subscription` out of `t` (Pattern B, §30.5), and the handler outlives `t` and `c` |

r42 (`8ccf7ff06`, 2026-11-19) predates the #9201 fix that walks expression bodies, which is why `p2` and `p9` compile there. On HEAD the live holes are the stores (`p1`, `p3`, `p5`, `m2`), the `+=`/`.Add` forwards, and possibly the `async` returned forward. The forwards `p2`/`p9` are rejected on HEAD only because the checker reads the callee's bound as `this`, and the text does not say that either.

The interface variant (`p3`) is the decisive one. Stdlib §6.5 makes the observer pattern normative: "Storing a `Subscription` in an `owned` field of `this` resolves spec §28.14's bounding object to `this` (form 3), so a handler capturing `this` … is admitted *regardless of how the publisher was reached* — a parameter, a field, an interface face" (QUOTED). With `ticks: IEvent<TickEvent>` a parameter, the caller sees only the interface. If a user implementation may keep the handler on itself, that sentence is false, and no caller-side rule can repair it, because the body is not visible at the call.

### 1.4 Why this is design-level

A clarification would state a rule the text already implies. Here every candidate rule changes a verdict. Reading the callee's bound as `this` (the checker) leaves the use-after-free. Reading it as the returned object rejects the hub idiom's sibling shapes and, read alone, is unsound at forms 4 and 5 (§3.3). Any caller-side reconciliation rejects caller programs the text admits. The L2 lane reached the same conclusion, and the triage's own draft ruling called its callee summary "the new mechanism".

## 2. What a sound rule must satisfy

A call site's admission is sound when every stored invocation ends before the bound the call site chose is torn down. The call site cannot see the body, so the guarantee must come from the callee. The callee cannot see the call site, so it must give the guarantee for every object a call site might choose.

- An instance method with an `owned` return is called at forms 2/3 (the bound is the returned object `R`, or the caller's `this`, which holds `R` in an `owned` field) and at forms 4/5 (the bound is the receiver, or the chain head the receiver is reached from through `owned` fields). Its storage must be severed by `R`'s teardown and by the receiver's teardown.
- A static method, or a lambda (whose calls are function-value invocations), is called only at forms 2/3. Forms 4/5 name no receiver, and a capture-free callback is all the text admits without one. Its storage must be severed by `R`.
- A constructor is called at form 1, and an instance method without an `owned` return only at forms 4/5. Their storage must be severed by `this`.
- The bound must be read where call sites read the callee. A call through an interface face or a base type resolves forms 2/3 by the declaration it calls (the announcement table's *Inheritance* rule, §28.3.5), so an implementation or override is bound by that declaration's return as well as its own.

This is the issue's interpretation (c), the intersection, made exact: "both" applies only where the call sites can actually name both objects. The rest of the proposal is how to check it structurally, with no knowledge of any call site and no new interface fact.

## 3. The options

### 3.1 A — Leave as is

Traces: `p1`, `p3`, `p5` and `m2` stay admitted with a use-after-free. Soundness: broken at a form the text names as "the tightest bound". Cost: nothing. Not viable. It is listed because "leave as is" must be weighed, and because declining it is itself recorded.

### 3.2 B — Caller co-bound (the issue's interpretation (a))

At forms 2/3 on a call of a user-authored owned-return instance method, the captures must also satisfy the receiver's resolution (forms 4/5).

Traces:

- Keeper, form 2: `c` is declared after `k`, so the call is rejected. Fixed.
- Hub idiom, form 2, with captures shorter-lived than the hub (the idiom's purpose; stdlib §6.5 "for handlers that must not capture `this`, `using var sub := …` remains the scope-bound alternative"): `using var hub := new Hub(); while … do using var c := …; using var sub := hub.OnChanged(lambda(t) => c.Hit(t)); … end;`. `c` is declared after `hub`, so the call is rejected. The idiom is broken.
- TickObserver (stdlib §6.5, normative example) over a user `IEvent<T>` implementation: the receiver `ticks` is a parameter, which resolves no admitted object, so the call is rejected. The stdlib text is broken. An exemption for compiler-known callees does not help, because the callee behind `IEvent<T>` is not known at the call.
- `using var sub := button.Clicked.Subscribe(…)` over a user implementation: the receiver is a getter (form 4 stops at a getter), so the call is rejected.
- `p2`/`p9` at form 5: B adds nothing at form 5, so the hole stays unless B also co-bounds forms 4/5 by the returned object, which rejects further admitted programs.

Soundness: incomplete without the form-5 half. Ergonomics: rejects the observer pattern. Cost: caller-side, plus the callee-kind test at every call. Migration: unmeasured caller rejections, including normative stdlib examples. Declined.

### 3.3 C — The callee's bound is the returned object alone (the issue's interpretation (b))

At an owned-return instance method, a store or forward is admitted only where it is severed by the returned object: a forward whose call is the returned value. This is the L2 lane's wording, read without the existing forward clause's `this` half.

Traces: Keeper store, rejected. Hub idiom, admitted. `p3` store, rejected. `p5` store, rejected. But:

- `p2`, instance `OnCancel(…) => tok.Register(cb)`: the forward's result is the returned value, so it is admitted. At form 5 the caller bounds by the receiver `r`, which severs nothing. MEASURED on r42: a read of freed memory (`hit 32566`).
- `p9`, instance `Make(scoped on): owned Button => new Button(on)`: the returned `Button` holds the callback, so it is admitted. At form 5, `keep := f.Make(…)` bounds by `f`. MEASURED: SEGV.

Soundness: broken at forms 4/5 unless the caller side also stops resolving the receiver at owned-return calls (C′), which newly rejects admitted callers such as `var s := hub.OnChanged(lambda … c …)` with `c` declared before `hub` (sound today). Declined as stated. C′ is declined for the same reason as B: it moves the repair to the caller.

### 3.4 D — The callee's bound (interpretation (c), structural): recommended

The callee is checked against the bound of the §0 table, read at its own declaration and at every declaration it implements or overrides. Each store and forward must be severed by every object in it, under three severance facts:

- A store into a `scoped` field of `this` (for a class-typed parameter, into a plain field of `this`) is severed by `this`.
- A forward whose inner call resolves `this` by the forms inside the callee is severed by `this`. There are two routes:
  - through the receiver: a call on `this` (form 5), or on a member chain rooted at `this` through `owned` fields (form 4), including the `+=` desugaring. The inner bound contains the receiver, which is disposed no later than `this`.
  - through the result: form 3, or form 1 composed with form 3's placement (v0.361, #8397), as in `_sub := src.OnChanged(h)`, `_reg := tok.Register(cb)` and `_child := new Child(h)`. The inner bound contains its result, which is held in an `owned` field of `this`.

  This is the existing forward clause, extended to the form-1 composition the v0.361 text already states for the admission arm (Q8).
- A forward whose call is the value the callee returns, with an `owned` result, is severed by the returned object. The forwarded callee's own bound contains its result: a constructor's constructed object, a static method's or a lambda's returned object, an owned-return instance method's returned object. This fact does not hold when the outer callee's return is mint-announcing. An `async` callee returns a task, whose `OwningTask<T>` result leaves it at `await` (§30.5), and an iterator returns a frame, so the forwarded call's result is not the returned object.

At an owned-return instance method a store is therefore never admitted. A forward is admitted when it is both: the returned value, on `this` or on an `owned` chain from `this`. An owned-return `async` or iterator method keeps no callback at all.

Traces:

- Keeper (`p1`), and its `_changed += h; return new Token();` and `_changed.Add(h); return new Token();` spellings: severed by `this` only, so rejected at the callee. Remedy: drop the `owned` return (`p7_keeper_unit`, MEASURED: runs, `hit 1`). Call sites then resolve the receiver, which the store is severed by.
- Hub idiom (`p4`, `p8`): `=> _changed.Subscribe(h)`, and its block twin `return _changed.Subscribe(h);` (a pinned accept row in the 9274 test). The inner call is on an `owned` field of `this`, so it is severed by `this`, and it is the returned value, so it is severed by the `Subscription`. Admitted, and sound at every caller form. Form 2 is bounded by `sub`, and disposing it removes the handler (stdlib §2.11). Form 3 is the observer pattern. Forms 4/5 are bounded by the hub, whose `_changed` destructor severs the subscriber list (stdlib §6.5).
- `p2`, instance `OnCancel`: severed by the `Subscription` only, so rejected. Remedy: store the `Subscription` in an `owned` field of `this` in a member without an `owned` return (`_reg := tok.Register(cb)`, form 3, pinned in the 9274 test), or, under D2, a static method returning the call.
- `p3`, `IEvent<T>` user implementation: the implementation is an owned-return instance method, so the store is rejected. The pattern-shaped implementation, `public method Subscribe(scoped handler: (double) -> ()): owned Subscription => _src.Subscribe(handler);` over `private owned readonly _src: EventSource<double>`, is admitted. Stdlib §6.5's "regardless of how the publisher was reached" becomes true for every implementation.
- `m1`/`m2`, iterator `Watch(scoped h): owned IEnumerable<int>` storing in `this`: item 4 by its own spelling (`m2`), or by the interface declaration it implements (`m1`, whose implementation spells the return unmarked, which §11.2 lets match). The store is rejected either way, and the returned frame severs nothing. Remedy: a method without an `owned` return that stores in a `scoped` field and is not mint-announcing, plus an iterator that only invokes.
- `async SubAsync(…): owned OwningTask<Subscription>  return _changed.Subscribe(h);`: the return is mint-announcing, so the returned forward is not severed by the task, and it is rejected.
- `p5`, class-typed: the store is severed by `this` only, so rejected. A storing object has one owner, so no field store is severed by both objects of the bound. The spellings that store are a method without an `owned` return or, under D2, a static factory (`p11` in the static-ownership research, `=> new Account(scoped c)`).
- `p9`, instance factory: severed by the returned `Button` only, so rejected. Remedy: under D2, a static factory. Without D2, a method without an `owned` return that stores the holder in an `owned` field of `this` (form 1 composed with form 3, Q8).

The spec's own examples (traced in §4.4) keep their verdicts.

Soundness: §4.5. Ergonomics: the rejected shapes have one-line remedies, and the dominant shape (the hub idiom, every owned-return instance method with a `scoped` parameter in the corpora) is untouched. The one shape with no direct spelling is a user event source that keeps its own subscriber storage behind a severing handle (§3.6). Cost: callee-local, in the walk that already exists (§5.3). Migration: zero test flips for the core rule (§5.1).

### 3.5 E — The triage carve: a caller co-bound unless the callee is "severed by return"

Each callee would compute a per-parameter fact, "every store or forward is severed by the returned value", export it in the artifact interface (§2.8.2), and the caller would co-bound forms 2/3 by the receiver when the fact is absent.

Traces: Keeper is co-bounded and rejected at form 2. The hub carries the fact and is admitted. `p3` reaches the callee through an interface, so there is no body and no fact, and the call is either co-bounded (breaking §6.5, as B does) or not (leaving the hole). `p2`/`p9` are severed-by-return, so they are admitted, and they dangle at form 5 as under C.

Soundness: as C at forms 4/5, and as A or B at interface dispatch. Design: the caller's verdict would depend on another declaration's body. `DECLINED.md` records that against a callee-body retention summary at the call (`rejected`, "the caller's verdict would depend on another declaration's body, which no per-body fact carries … and would change when the callee became virtual, an interface member, or imported", spec §28.3, rationale §5.24; reopening bar "—(none stated)") and against a body-derived fact carried in the interface for diagnostics (`deferred`, proposal 0132 §7). E would carry one for verdicts. It reopens a `rejected` row with no new evidence against its reasons. Declined.

### 3.6 F — A recorded trust clause for implementations

Stdlib §6.5's *Subscribing* contract ("disposing the subscription removes the handler") would become a K4.3 trust clause on every implementation of an owned-return `scoped` interface method, named by the trust-admission report.

F would admit one shape D rejects: a user event source with its own subscriber storage, whose `Subscribe` stores the handler in `this` and returns a handle whose disposal clears the slot. That is the same quiescence-by-trust that `EventSource<T>` itself rests on, so F is not unsound for that shape, and D's rejection of it is a real cost. D rejects it anyway, for three reasons:

- The wrap-an-`EventSource<T>` spelling gives the same behaviour and needs no trust.
- F leaves the direct `Keeper` (no interface) needing the static rule anyway.
- §28.14 compiler-verifies user-authored callees wherever a structural check exists, and here one does.

Declined, with that cost carried into Q1.

### 3.7 Comparison

| | Keeper `p1` (+ `+=`, `.Add`) | interface `p3` | class-typed `p5` | `p2`/`p9` at form 5 | `async`/iterator store or forward | hub idiom, all forms | stdlib §6.5 observer | caller verdicts change | new interface fact |
|---|---|---|---|---|---|---|---|---|---|
| A | UAF | UAF | dangles | UAF on r42 | UAF | admitted | holds by trust only | no | no |
| B | rejected | UAF or §6.5 broken | rejected | UAF | rejected at form 2 | rejected at form 2 | broken | yes | no |
| C | rejected | rejected | rejected | UAF | `async` forward UAF | admitted | holds | no | no |
| D | rejected | rejected | rejected | rejected | rejected | admitted | holds | no | no |
| E | rejected | UAF or §6.5 broken | rejected | UAF | `async` forward UAF | admitted | broken or held by trust | yes | yes |
| F | UAF | trust clause | dangles | UAF | UAF | admitted | holds by trust | no | no |

## 4. The recommendation

Adopt D. Land the static half (§4.2) and the class-typed half (§4.3) with it.

### 4.1 The rule

The callee's bound is the §0 table, read at the member's own declaration and at every declaration it implements or overrides. A body is in the both-row when its own declaration, or any declaration it implements or overrides, is an instance method whose return is `owned`. For a mint-announcing return, where §11.2 lets either spelling match ("either spelling matches, in either direction", QUOTED), the `owned` spelling on either declaration puts the body there. This replaces the first draft's appeal to §11.2's exact match, which does not hold for a mint-announcing return (pc-0215 F2). The keying is the announcement table's *Inheritance* rule: "A member implementing or overriding a declaration classifies at the declaration it implements, at every call site, direct or dispatched" (§28.3.5, QUOTED). Whether form 2's "a call whose return is `owned`" also covers an unmarked mint-announcing return is Q7. The bound follows whatever reading Q7 settles.

Every store and every forward of a `scoped` parameter must be severed by each object of the bound, under §3.4's severance facts. The v1 shape is purely syntactic:

- "the value the callee returns" means the forward's call is the whole expression body, or the whole operand of a `return` statement, of a callee whose declared return is not mint-announcing. At an `async` or iterator callee nothing is severed by the returned object. That callee kind is *liftable* for a `Task<T>` result, which the task disposes.
- A binding of the call returned later (`var s := _changed.Subscribe(h); return s;`) and a conditional over two such calls are not read through. Both are recorded *liftable*.
- A `return` inside a nested closure returns that closure's value, not the callee's, so a forward there is never severed by the callee's returned object. *v1 admission bounds* item 6 rejects every callee-side use inside a nested closure in any case (v0.361, #9311). The normative text also states the constraint on a future lift: a revision lifting item 6 admits a closure-body use only where it is severed by every object of the callee's bound. So bound 6 stays deferred and composes.
- A store into a `scoped` field of a local the callee constructs and returns (`s28_14_0017`'s static shape) is not a v1 shape (Q3, after Q2). Under D2 the remedy is to forward to a `scoped` constructor parameter (`=> new Registry(cb)`).

The rule binds every user-authored body: an `override`, an extension member, and an implementation of an interface method. A caller resolving either object through an interface face or a base type therefore relies on a body that has met both. No interface or artifact fact is added. The keying reads only declarations the implementing module already sees.

### 4.2 The static half (D2)

For a static method, the text already makes the returned object the bound ("so that a call site can resolve bounding form 2 or 3"). What it lacks is the fact that a forward whose call is the returned value is severed by it. Without that fact a static method can forward to nothing. HEAD reads it so: every constructor forward is rejected, and so is every static forward. The 9201 gtest row says "a static method has no `this`, so NOTHING is the same bounding object — every forward of its own `scoped` parameter is off-object".

Two other readers took the opposite reading:

- the static-ownership research's `p11` (`static Open(scoped c: Customer): owned Account => new Account(scoped c)`, "EXPECT (spec §28.14): compiles; no report line; static bound");
- corpus case `s28_14_0017` (a store into the returned local's `scoped` field, `expect: run`, baseline `fail divergence`, and itself defective: §5.2).

D2 settles the question with the same severance fact the instance rule needs. It admits `=> new Account(scoped c)`, `=> tok.Register(cb)` (`p6`) and `=> new Sink().Make(q)`, all synchronous. It is the stated remedy for several rejected instance shapes.

If the owner declines D2, the instance rule still stands, and §7.3's trailer lists every sentence that changes. The remedy everywhere is then "return no `owned` value: store the handle in an `owned` field of `this` (form 3), or the callback in a `scoped` field of `this`".

### 4.3 The class-typed half

Scoped construction applies "the same two-sided contract to a passive borrow", and its callee-side bullet uses the same words, "the bounding object". `p5` is the same hole. The severance fact for a borrow is "the storing object is disposed no later than X". A field store in `this` is disposed with `this`, and the object behind a returned forward is the returned object. No field store is disposed by two owners, so at an owned-return instance method no class-typed store is admitted, directly or through a forward. The rule needs no separate text. The proposed sentence in the scoped-construction bullet states the consequence and the spellings that store.

### 4.4 The spec's and the library's examples, re-traced

- §28.14 `Button` (a constructor, bound `this`): `_onClick := onClick` is unchanged.
- §28.14 `Reader` (a constructor, class-typed): `_src := src` is unchanged.
- Guide 1 §9 `Timer.OnTick` (instance, no `owned` return): the store into `_onTick` is unchanged.
- stdlib §6.5 the event pattern's `_clicked += h` (a forward in a non-owned-return member) is unchanged. `EventSource<T>.Subscribe` and `Add` are compiler-known, and their bounds are the stated ones: `Subscribe` is severed by the destructor ("severs the subscriber list before reclamation") and by the `Subscription` ("disposing the subscription removes the handler"), so both. `Add` is severed by `this`.
- stdlib §6.5 TickObserver: holds through any implementation (§3.4), the bound being read at `IEvent<T>.Subscribe`'s declaration as well as the implementation's.
- stdlib §6.1 `CancellationToken.Register`: an owned-return instance method whose stated bound is the returned `Subscription` alone ("the registration stores the callback bounded by the returned `Subscription`", QUOTED). Its receiver is a token value whose teardown severs nothing.
  - This is a compiler-known callee whose bound omits the receiver, so §4.5's step through the receiver does not reach it. A forward to `Register` on `this` (from a user `extension CancellationToken` member) is not severed by `this`, and the text says so.
  - HYPOTHESIS, confirmed by pc-0215's reading: no form 4/5 resolution of a token receiver reaches an admitted object today. A token is not `IDisposable`, so it is never a `using var` local. A `.Token` hop is a getter. A plain-local or parameter root is not admitted. A direct `this` receiver is capture-free by the checker.
  - So the stdlib sentence §7.7 adds is a register obligation that records this, not a new caller rule (Q6).
- stdlib §6.10 `TaskScope.Spawn` (no `owned` return), stdlib §2.21 `ScopeGuard` (a constructor): unchanged.
- §28.14 form 3 via #8397's form-1 composition (`_child := new Child(scoped this)`) at a caller: a constructor, bound `this`, unchanged. The same composition inside a callee (`_child := new Child(h)`) is §3.4's result route (Q8).
- Function values: a method reference over an owned-return method keeps `scoped` in its type (§27.1). A call through it resolves forms 2/3 only, since there is no receiver, and every callee whose call sites can resolve forms 2/3 has the returned object in its bound. A lambda with a `scoped` parameter (`LambdaParam` admits `scoped` through `FuncParamMods`) is bound by its returned object. MEASURED (pc-0215, `l1_lambda_scoped`): r42 rejects the lambda store and does not resolve form 2 at a function-value call, so neither is a live hole.

### 4.5 Soundness (HYPOTHESIS, argued by induction on forward depth)

Claim: at every call site that admits a capture-bearing callback, every invocation of the stored callback happens before the bound the call site resolved is torn down.

- Base: a store into a `scoped` field of `this` is invoked only by the declaring type's bodies through `this`, so it ends with `this` (*Quiescence*, K2.3's residual for user types, K4.1 as the backstop). A compiler-known callee severs exactly as its section states, and its bound may be narrower than the table's row for its kind. `CancellationToken.Register`'s bound omits the receiver. Every standard-library callee whose return is `owned` states a bound that contains its returned object.
- Step, three routes:
  - A forward whose inner call resolves `this` through the receiver (form 5 on `this`, or form 4 on an `owned` chain from `this`) reaches an inner callee whose bound includes its receiver. That holds for table items 1, 2 and 4, and for a standard-library callee only where its section's bound does. The receiver is disposed no later than `this`.
  - A forward whose inner call resolves `this` by form 3, or by form 1 composed with form 3's placement (v0.361), reaches an inner callee whose bound includes its result (items 1, 3, 4, the lambda row, and every owned-return standard-library callee). That result is held in an `owned` field of `this` and disposed with it, under the registration-lifetime residual.
  - A forward that is the returned value of a callee whose return is not mint-announcing reaches an inner callee whose bound includes its result, which is the outer returned object.
- Call site: forms 2/3 resolve the returned object `R` (form 2: `L` is `R`; form 3: `R` sits in an `owned` field of the caller's `this`). Every callee whose call sites can resolve forms 2/3 has `R` in its bound, the bound being read at the declaration the call resolves (the *Inheritance* keying), and has met it by the step. Forms 4/5 resolve the receiver, or the chain head it is reached from through `owned` fields. Every user-authored callee whose call sites can resolve forms 4/5 has the receiver in its bound. A standard-library callee whose bound omits the receiver is one whose receiver no form 4/5 resolution admits (§4.4, the register obligation of §7.7). Form 1 resolves the constructed object, which is the constructor's bound.

The residuals are the ones §28.14 already records, now named once (Interaction 1 of the L2 lane, with #8397):

- Form 2 carries none, because a `using var` cannot be moved out (§28.10, A5).
- Form 3, and the callee-side result route, is the handle-holding `owned` field moved out of `this`.
- Forms 4/5 are "a storing owned child" moved out of the callee: `_changed` moved out of the hub. This needs a `move` of an `owned` field, and the event pattern's field is `readonly`.

All three are *The registration-lifetime residual*. The proposal adds one sentence there so that the form-2 case is stated.

## 5. Measurement, migration and implementation

### 5.1 The corpora (MEASURED, pc-0215 `scan3.py`, §11)

The first draft's `scan2.py` stripped `"\n` before joining split C++ string literals, which left a stray quote inside split signatures and lost four methods (pc-0215 F8). `scan3.py` joins correctly.

It scans every tracked `.bzl` file (12,084: the rule corpus, `tests/conformance/cases`, `tests/lit`, the research and ergonomics programs) and the tracked `.cpp` sources (gtest and `src/`). Excluding this proposal's own file, it finds 48 methods with a `scoped` parameter and an `owned` return: 37 instance and 11 static. The normative documents were scanned separately: their only such declarations are the stdlib's `Register` and `Subscribe`.

- Instance methods (37):
  - stdlib signature mirrors (`Register`, `Subscribe` in `tests/conformance/cases`) and one parser-test interface declaration: 14. Two more hits are text inside `src/` comments.
  - The hub idiom `_changed.Subscribe(handler)` as the returned value: 9. That is the arrow form in four 9056 ownership and module rows, the ergonomics programs `n17-observer/v2` and `v3`, `repros/NB13` and one bug-report note, plus the block form `return _changed.Subscribe(handler)` in the 9274 test (accept; it needs §5.3's `ReturnStmt` returned-position flag). Admitted under D.
  - `this.Changed.Subscribe(handler)`: 2 (9056 arrow, 9274 block). A getter hop. It is rejected today, and under D it is rejected for the same reason.
  - `OnCancel => tok.Register(cb)`: 2 (the 9201 and 9056 tests). Rejected today, and rejected under D: severed by the returned `Subscription` only.
  - Bodies that never store the parameter (`=> new Sub()`, `h(); return new Sub();`, `return new Sink();`): 8 rows across the 3195 (5), 3189 (2) and 9201 (1) tests. Admitted under D.
  - Keeper-shaped stores, and their `+=`/`.Add` spellings: 0.
  - Implementations of `IEvent<T>` or `IAsyncEvent<T>` outside the stdlib: 0 (`git grep` for a class implementing either; confirmed by pc-0215).
- Mint-announcing returns with a `scoped` parameter: 0 marked `owned`. Two are unmarked, and both only invoke: `3195`'s `Subscribe(…): IEnumerable<int>` and `1527`'s `Install(…): Task`. So F1's exclusion and Q7 flip nothing.
- Static methods (11):
  - six `lit/sema` `Take` rows that only invoke: unchanged;
  - `9201` `S(scoped s): owned Sink => s`: a return sink, rejected, unchanged;
  - `9274` `On(…): owned Tk  _bus.Add(handler); return new Tk();`: rejected because a static field is not a `this` hop, and under D also not the returned value, unchanged.

  Three are touched only by Q2 and Q3:
  - `9201` `S(scoped q): owned Sink => new Sink().Make(q)`: today rejected. Under D2 it is admitted, because the call is the returned value and `Make`'s bound contains its result.
  - `p11_scoped_factory_form2` (research probe, not a test): admitted under D2, as the research expected.
  - `s28_14_0017`: decided by Q3.

### 5.2 Tests that flip

| test | today | under D | under D + D2 | under Q3 (ii) |
|---|---|---|---|---|
| `tests/ownership/expr_body_scoped_param_9201_test.cpp` `static_expression_body_forwards_scoped_param_off_object_rejected_spec_28_14` | reject (`kScopedForward`) | reject | accept: the row is rewritten as the admitted D2 shape, and a reject twin over an instance method is added | — |
| `tests/conformance/rules/cases/s28_14_0017.bzl` | `run` (baseline `fail divergence`) | unchanged | unchanged | `reject`, after the repair below |

`s28_14_0017` is defective as written (MEASURED, pc-0215 `c17.bzl`). Its first diagnostic is `constructor does not initialize non-nullable field _cb` (§7.9), and the §28.14 store rejection only follows it. So its `fail divergence` is partly a case bug, and a `reject` re-head would pass vacuously on the §7.9 error. Repair it first: `private scoped _cb: (() -> ())?;`, and `_cb?();` in `Fire`. Its v0.361 verdict is then `run`. The v0.361 text admits the shape: a static method's bounding object is the returned object, and `r` is that object. So Q3 (ii) narrows a text-admitted shape to a *liftable* v1 bound. It is re-headed `reject` with `diagnostic:` on the new §28.14 text, and its remedy twin `=> new Registry(cb)` (D2 only) is a `run` case.

No other gtest, lit or corpus verdict flips. The existing reject rows keep their diagnostics: the new text fires only for shapes that are admitted today, so no existing needle loses its text. A landing must still run `scripts/pregate_stale_needles.py` over any text it renames.

### 5.3 Implementation sketch (non-normative; `src/` read at `bd12fe038`)

All of it is in `MethodOwnership`'s callee walk (`src/ownership/Ownership_Checks.cpp`). There is no caller-side change, no `.bezel.meta` change and no module-boundary work: an implementation is checked in its own module against declarations that module already sees.

1. A per-body flag, set where the walk starts (`scopedCalleeScan` for block bodies; the expression-body entry in `src/ownership/Ownership.cpp` beside the #9201 walk). It is set when the enclosing callable is an instance or extension method, not a constructor, and its own declaration or any declaration it implements or overrides has an `owned` return. A second flag records whether the callable's own declared return is mint-announcing.
2. In `scopedCalleeWalkStmt`, under the first flag, the two admitted store arms report the new diagnostic instead of returning: the kind-1 store into a `scoped` field of `this`, and the kind-3 store into a plain field of `this`.
   - 2b. In `scopedCalleeWalkStmt`'s #9274 `+=` arm, under the first flag, report the new diagnostic instead of admitting. A `+=` statement is never in returned position.
3. In `scopedCalleeWalkExpr`'s forward arm (kinds 1 and 3), under the first flag, the admitted forward additionally requires the call to be in returned position, and the callable's return not to be mint-announcing. Returned position is set by the `ReturnStmt` case and the expression-body entry, and only when the returned expression is the call node itself. For D2: at a static owned-return method, a forward in returned position whose callee's result is `owned` is admitted where today it reaches the off-object report. D2 must also change the `NewExpr` arm's constructor-forward rejection to admit a returned constructor call. For Q8, the same arm admits a constructor call directly assigned to an `owned` field of `this` at item 1 and 2 callees.
4. Diagnostics (proposed wording):
   - "`h` is a `scoped` parameter of a method whose return is `owned`: callers may bound its captures by the returned object or by the receiver, so it may be kept only by a forward that is the method's returned value, called on `this` or on an `owned` field of `this` (§28.14 *The callee's bound*)".
   - For a mint-announcing return: "… an `async` or iterator method whose return is `owned` returns a frame, which severs nothing it forwards: keep no `scoped` callback here (§28.14)".
   - For a class-typed parameter: "… a method whose return is `owned` cannot store a `scoped` borrow parameter: the storing object would be disposed by only one of the returned object and the receiver; return no `owned` value (or, under D2, make the method `static`) (§28.14)".

Estimate (HYPOTHESIS): 100–180 lines plus three diagnostics, one Opus lane, because the implemented-declaration lookup crosses into sema. Pins:

- reject: `p1`, its `+=` and `.Add` twins, `p3`, `p5`, `p2`, `p9`, `m1`, `m2`, `SubAsync`;
- run: `p4`, `p8`, `p7`, the 9274 block row, and a pattern-shaped `IEvent<T>` implementation;
- run, with D2: `p6` and `p11`.

A runtime leg loops each admitted shape (the corpus cases of §9 do so).

Side observations and spin-off issues, not this proposal's question:

- On r42 every program that forwards a closure through a user wrapper into `Subscribe` or `Register` aborts with "refcount over-release (closure)" (`p4`, `p6`, `p8`). The direct `src.Subscribe(lambda …)` twin runs clean (`q1`, `q2`). r42 predates the #9201/#9274/#9310 fixes. The landing lane should re-probe on HEAD before banking the `run` rows, and file this if it reproduces.
- The compiler's §11.2 implementation check is stricter than the spec for mint-announcing returns (`TypeResolution_Checks.cpp` compares `implRetOwned != imRetOwned` with no carve; MEASURED, pc-0215 `m1`). That is a separate spec divergence to file. Bringing it to spec is safe only once §5.3 item 1's keying lands.
- The `SubAsync` shape should be probed on HEAD as a possible live hole (pc-0215 F1).

## 6. Declined, and the `DECLINED.md` row text

Per `DECLINED.md`'s scope rule (2), the rows are indexed only once this proposal is accepted. The proposed text, class `declined-at-proposal`, record "proposal 0215 §3":

1. *A caller-side co-bound at bounding forms 2/3 on a call of a user-authored instance method whose return is `owned` (captures tested against the receiver as well as the returned object; #9275 interpretation (a)).* It rejects the stdlib §6.5 observer pattern wherever the publisher is a parameter, a getter-faced property or an interface face, rejects the hub idiom's short-lived-capture form at form 2, and cannot reach interface dispatch. Reopening bar: a caller-visible retention announcement, which does not exist today.
2. *Reading the bound of an owned-return instance method as the returned object alone (#9275 interpretation (b)).* Unsound at forms 4/5: a returned forward over a non-`this` receiver (`=> tok.Register(cb)`) or a returned constructed holder (`=> new Button(on)`) dangles when the caller bounds by the receiver. Reopening bar: none while forms 4/5 resolve the receiver at an owned-return call.
3. *A per-parameter "severed by the returned value" summary computed from the callee's body and exported in the artifact interface, keying a caller-side co-bound (the v0.361 triage carve).* The same grounds as the `rejected` row on a callee-body retention summary at the call. The caller's verdict would depend on another declaration's body, it is unavailable at interface dispatch, and it adds nothing the callee-side rule does not already give. Reopening bar: —(none stated), as that row.
4. *The stdlib §6.5 subscription contract as a recorded K4.3 trust clause on implementations of owned-return `scoped` interface methods.* F would admit a user source with its own subscriber storage under a trust clause. D rejects that shape instead, because the wrap-an-`EventSource<T>` spelling needs no trust, and §28.14 compiler-verifies user-authored callees where a structural check exists. Reopening bar: a measured need for user subscriber storage that an owned `EventSource<T>` cannot serve.

And one `deferred` row, record "spec §28.14 *The callee's bound* (v1 bounds), proposal 0215 §4.1": *The returned forward read through a binding returned on every path, or through a conditional over returned forwards; a returned forward at an `async` callee whose result is a `Task<T>` (which the task disposes); a store into a `scoped` field of an object the callee constructs and returns.* Bar: more analysis, as for the other *liftable* bounds.

## 7. Normative text

Every `old` anchor below occurs once in the v0.361 text at `origin/main` `cdd2022c7` (checked with a scripted exact-string count).

### 7.1 Spec §28.14, *The `scoped` modifier*, the callee bullet's first sentence

old:

> - **The callee may store the callback**, but only in `scoped` fields **of the bounding object itself** (below), or forward it to another `scoped` parameter whose bounding object is the same object; it may invoke it.

▲ new:

> - **The callee may store the callback**, but only in `scoped` fields **of the bounding object itself** (below), or forward it to another `scoped` parameter whose bounding object is the same object — at the callee, the bounding object is the callee's **bound** (*The callee's bound* below), every object its call sites can resolve; it may invoke it.

### 7.2 Spec §28.14, *The registration-lifetime residual*, one sentence appended

old (the paragraph's last sentence):

> why such fields are not frozen against `move` is recorded in the rationale ([§5.26](Bezel_Design_Rationale.md#sec-the-ownership-kernel), A11).

▲ new:

> why such fields are not frozen against `move` is recorded in the rationale ([§5.26](Bezel_Design_Rationale.md#sec-the-ownership-kernel), A11). At a call of a method whose return is `owned`, *The callee's bound* below makes the returned object sever the storage: under form 2 the `using var` cannot be moved out ([§28.10](#sec-using-var)), so that form carries no such obligation; under form 3 the returned handle is the handle-holding field above; and where an instance method's forward reached its storing member through `owned` fields of its receiver, that member is the storing owned child above.

### 7.3 Spec §28.14, a new paragraph before *Quiescence*

old:

> **Quiescence.** Disposal of the bounding object must guarantee that no stored callback is invoked after disposal completes

▲ new (the new paragraph, then the old text unchanged):

> **The callee's bound** (axiom [A11](#sec-axiom-register)). A callee cannot see which form its call sites resolve, so its obligations above are checked against its **bound**, the objects a call site's resolution can name, and every store and every forward must be **severed by** each of them (after the object's teardown completes, the stored callback is never invoked again):
>
> 1. a constructor: the constructed object, `this` (form 1);
> 2. an instance or extension method whose return is not `owned`: the receiver, `this` (forms 4/5; for an extension member, the extended value);
> 3. a static method, whose return is `owned` (above), and a lambda: the returned object (forms 2/3; a function-value invocation names no receiver);
> 4. an instance or extension method whose return is `owned`: **both** the receiver and the returned object — a caller's `using var t := k.M(…)` resolves the returned object (form 2) and a caller's `x := k.M(…)` the receiver (forms 4/5), so storage severed by only one of them outlives the other's bound.
>
> The bound is read, like the [announcement table](#sec-announcement-table)'s rows, at **the declaration a member implements or overrides** as well as at its own: a body is in item 4 when its own declaration or any declaration it implements or overrides has an `owned` return (for a mint-announcing return, where [§11.2](#sec-interface-declaration) lets either spelling match, the `owned` spelling of either puts it there). So every user-authored body of such a method — an `override`, and an implementation of an interface method whose return is `owned` (stdlib [§6.5](Bezel_Standard_Library.md#sec-events-ievent-eventsource)'s `IEvent<T>.Subscribe`) included — meets both, and a call through an interface face resolves either object against a body that has met both.
>
> A store into a `scoped` field of `this`, and a forward whose bounding object, resolved in the callee's body by the forms above, is `this` — a call on `this` or on a member chain rooted at `this` through `owned` fields (form 4, the `+=` spelling included), or a call whose result is directly assigned to an `owned` field of `this` (form 3, or form 1 composed with its placement) — are severed by `this`. A forward whose call is the value the callee returns — the whole expression body, or the whole operand of a `return` — and whose result is `owned` is severed by the returned object, which the forwarded callee's own bound contains (items 1, 3 and 4). A callee whose declared return type is mint-announcing ([§28.3.5](#sec-announcement-table)) — an `async` or iterator callee — returns a frame, not the forwarded call's result (an `OwningTask<T>`'s result leaves it at `await`, [§30.5](#sec-result-lifetime-and-task-disposal)), so no forward in its body is severed by the returned object: such a callee whose return is `owned` keeps no callback (*liftable*: a `Task<T>` result, which the task disposes). So an instance method whose return is `owned` keeps the callback only by a forward that is both — the returned call, on `this` or on a member chain rooted at `this` through `owned` fields — and a static method only by a returned forward:
>
> ```bezel
> class Token
>   public constructor()
>   end constructor;
> end Token;
>
> class Hub
>   private owned readonly _changed: EventSource<double> := new();
>   public method OnChanged(scoped h: (double) -> ()): owned Subscription => _changed.Subscribe(h);   // severed by the returned `Subscription` and by `_changed`, disposed with `this`
> end Hub;
>
> class Keeper
>   private owned readonly _changed: EventSource<double> := new();
>   private scoped _h: ((double) -> ())?;
>   public method Keep(scoped h: (double) -> ()): owned Token
>     _h := h;                                  // error: severed by `this` alone, but `using var t := k.Keep(…)` bounds the captures by `t`
>     return new Token();
>   end Keep;
>   public method KeepAll(scoped h: (double) -> ()): owned Token
>     _changed += h;                            // error: the same — a `+=` statement is never the returned value
>     return new Token();
>   end KeepAll;
>   public method Watch(tok: CancellationToken, scoped cb: () -> ()): owned Subscription => tok.Register(cb);
>                                               // error: severed by the returned `Subscription` alone, but `x := k.Watch(…)` bounds the captures by `k`
>   public static method Watching(tok: CancellationToken, scoped cb: () -> ()): owned Subscription => tok.Register(cb);
>                                               // admitted: a static method's bound is the returned object
> end Keeper;
> ```
>
> The returned forward is read only as the returned call itself: a binding of it returned later, a conditional over two such calls, and a store into a `scoped` field of an object the callee constructs and returns are not read through (*liftable*); a `return` inside a nested closure returns that closure's value, not the callee's (*v1 admission bounds* item 6 rejects such a use in any case, and a revision lifting item 6 admits a closure-body use only where it is severed by every object of the callee's bound). A standard-library callee's bound is the one its section states: a forward to such a callee whose bound omits the receiver is not severed by `this` (stdlib [§6.1](Bezel_Standard_Library.md#sec-cancellationtoken-and-cancellationtokensource)'s `Register`, reached on `this` from an extension member); every standard-library callee whose return is `owned` states a bound that contains its returned object; and one whose bound omits the receiver is a callee whose receiver no form 4/5 resolution admits, a register obligation its section records.

If the owner declines D2 (Q2), drop the closing clause "and a static method only by a returned forward" and the `Watching` line (item 3, lambda included, keeps the returned object as its bound), the static factory in ▲§7.4's parenthesis, the static spelling in ▲§7.7 and in ▲§7.11, and §8's static remedies. The remedy everywhere is then "return no `owned` value: store the handle in an `owned` field of `this` (form 3), or the callback in a `scoped` field of `this`". Q3's remedy exists only under D2. With D2 declined, Q3 is (ii) without a remedy, or deferred. If Q3 goes (i), drop "and a store into a `scoped` field of an object the callee constructs and returns" from the liftable list. If Q8 is declined, drop "or form 1 composed with its placement" and add the form-1 composition to the liftable list.

### 7.4 Spec §28.14, *Class- and interface-typed `scoped` parameters*, the callee-side bullet

old:

> and may forward it only to another `scoped` parameter with the same bounding object. It may not return it or store it anywhere else;

▲ new:

> and may forward it only to another `scoped` parameter with the same bounding object — the callee's bound (*The callee's bound* above), where a store or forward is severed by an object when it leaves the storing object disposed no later than that object; a storing object has one owner, so no store is severed by both objects of item 4's bound, and an instance method whose return is `owned` stores such a parameter neither directly nor through a forward (a method without an `owned` return, or a static factory, `public static method Open(scoped c: Customer): owned Account => new Account(scoped c);`, is the spelling that stores). It may not return it or store it anywhere else;

### 7.5 Spec §28.3, the axiom register, row A11

old:

> | A11 | The [§28.14](#sec-scoped-callback-parameters) bounding-object resolution forms (1–5) and the ratified v1 admission bounds |

▲ new:

> | A11 | The [§28.14](#sec-scoped-callback-parameters) bounding-object resolution forms (1–5), the callee's bound, and the ratified v1 admission bounds |

### 7.6 Spec Appendix C.3, one bullet after the `scoped`-declaration bullet

old:

> - `scoped` is declared only on a function-typed parameter or field, or on a class- or interface-typed parameter whose declared type, nullability stripped, is not `Object` ([§22.3](#sec-parameter-modifiers), [§28.14](#sec-scoped-callback-parameters)).

▲ new (the old bullet, then):

> - Every store and forward of a `scoped` parameter is severed by each object of the callee's bound, read at the member's own declaration and at every declaration it implements or overrides; an instance or extension method whose return is `owned` — an override and an interface implementation included — keeps a `scoped` callback only by a forward that is its returned call on `this` or on a member chain rooted at `this` through `owned` fields, keeps none where its return is mint-announcing, and stores no class- or interface-typed `scoped` parameter ([§28.14](#sec-scoped-callback-parameters) *The callee's bound*; A11).

### 7.7 Stdlib §6.1, `Register`

old:

> the registration stores the callback bounded by the returned `Subscription` (the owned-return bounding form — `using var reg := token.Register(...)`), and disposal severs the registration, which is the quiescence guarantee.

▲ new:

> the registration stores the callback bounded by the returned `Subscription` (the owned-return bounding form — `using var reg := token.Register(...)`), and disposal severs the registration, which is the quiescence guarantee. That `Subscription` is `Register`'s whole bound (spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters), *The callee's bound*): the receiver is a token value whose teardown severs nothing, and no bounding form resolves a token receiver to an admitted object (a token is not `IDisposable`, so never a `using var` local, and `Token` is a getter), so a `Register` call bounded by its receiver admits capture-free callbacks only; a user method forwards to `Register` either by storing the returned `Subscription` in an `owned` field of `this` (`_reg := tok.Register(cb);`, form 3, in a member whose return is not `owned`) or as a `static` method returning the call (`=> tok.Register(cb)`).

### 7.8 Stdlib §6.5, *Subscribing*, one sentence

old:

> so a handler capturing `this` (or state reached through it) is admitted *regardless of how the publisher was reached* — a parameter, a field, an interface face.

▲ new:

> so a handler capturing `this` (or state reached through it) is admitted *regardless of how the publisher was reached* — a parameter, a field, an interface face. A user implementation of `IEvent<T>` or `IAsyncEvent<T>` keeps that true: its `Subscribe` implements a method whose return is `owned`, so spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters) (*The callee's bound*) lets it keep the handler only by returning a forward on a source it owns — `public method Subscribe(scoped handler: (T) -> ()): owned Subscription => _source.Subscribe(handler);` over a `private owned readonly _source: EventSource<T>` — never in storage of its own, even behind a handle that severs it.

### 7.9 Rationale §5.26, *The axioms' reasons*, the A11 entry, one paragraph appended

old (the entry's last sentence):

> so the registration's extension past the bounding object is recorded as the same residual-trust shape as quiescence, with the checked-mode backstop.

▲ new:

> so the registration's extension past the bounding object is recorded as the same residual-trust shape as quiescence, with the checked-mode backstop. The callee's bound (spec §28.14) is the clause that ties the callee side to the forms ([proposal 0215](proposals/0215-owned-return-callee-bound.md), [#9275](https://github.com/s8lap/bezel-lang/issues/9275)). The forms are per call site and a callee has none, so the callee is checked against every object its call sites can name: both the receiver and the returned object at an instance method whose return is `owned`. The bound is read at the declaration a member implements or overrides, as the announcement table is, because that is the declaration a dispatched call resolves. The repair sits at the callee because each caller-side alternative fails on a recorded ground. A co-bound by the receiver rejects the stdlib §6.5 observer pattern wherever the publisher is a parameter, a getter-faced property or an interface face. A co-bound keyed on a body-derived summary makes a caller's verdict depend on another declaration's body (§5.24, *No caller-side warning keyed on the callee's body*). Reading the bound as the returned object alone leaves a forward over another receiver dangling at forms 4/5. The cost is that a method that keeps a callback on itself and returns an unrelated handle has no spelling: it returns no `owned` value, or the handle it returns is the one that severs. So a user type implementing `IEvent<T>` keeps its subscribers in an owned `EventSource<T>`, never in storage of its own, even behind a handle that severs it; the alternative, a trust clause on such implementations, was declined because the wrapping spelling needs no trust.

### 7.10 Guide 1 §9, one sentence

old:

> On the callee side the rules are the mirror image: store only in a `scoped` field of the same object, or forward to a `scoped` parameter on `this`; never return it, bind it to a local, or store it in another object.

▲ new:

> On the callee side the rules are the mirror image: store only in a `scoped` field of the same object, or forward to a `scoped` parameter on `this`; never return it, bind it to a local, or store it in another object. A method that returns an `owned` handle, such as a `Subscribe` of your own, is stricter, because a caller may bound its captures by the handle or by your object: it may only return the forward, `=> _changed.Subscribe(h)`, and never keep `h` in a field of its own, not even with `_changed += h` (spec §28.14, *The callee's bound*).

The guide's header pin moves to the landing revision.

### 7.11 Code-generation reference

§4 *`scoped`*, old:

> the callee may store the callback in a `scoped` field of the bounding object, forward it to a same-bound `scoped` parameter, or invoke it.

▲ new:

> the callee may store the callback in a `scoped` field of the bounding object, forward it to a same-bound `scoped` parameter, or invoke it. A method with an `owned` return (an `IEvent<T>.Subscribe` implementation included) keeps it only as the returned call on an `owned` field, `=> _changed.Subscribe(h)`, never in a `scoped` field of `this` and never by `+=`; an `async` or iterator method with an `owned` return keeps none (§28.14 *The callee's bound*).

§7.2, one entry appended at the end of the section, in the section's form:

> ```bezel
> class Keeper
>   private owned readonly _ticks: EventSource<double> := new();
>   private scoped _h: ((double) -> ())?;
>   public method Keep(scoped h: (double) -> ()): owned Subscription
>     _h := h;                                         // ERROR: an `owned`-return method's callers may bound the captures by the returned handle, which does not sever `_h` (§28.14)
>     return _ticks.Subscribe(lambda(t) => ());
>   end Keep;
> end Keeper;
> ```
>
> > §28.14 *The callee's bound*: a method that returns an `owned` handle keeps a callback only by returning the forward that the handle severs — `=> _changed.Subscribe(h)` over an `owned` source field — or returns no `owned` value and stores in a `scoped` field of `this`. To register on a token or a publisher it does not own, store the returned `Subscription` in an `owned` field of `this` (`_reg := tok.Register(cb);`) in a member that returns no `owned` value, or write a `static` method that returns the registering call.

The header pin moves to the landing revision, which `check_docs.sh`'s PIN check requires within three revisions.

### 7.12 Proposals folder

`README.md` carries the *Pending decision* row. On acceptance, the rows of §6 go into `DECLINED.md`.

## 8. Changelog sketch

> **v0.NNN — The callee's bound (§28.14; proposal 0215; #9275).** Design change; axiom extension (A11).
>
> Decided: §28.14's callee-side obligation is checked against the callee's bound, every object its call sites can resolve:
> - the constructed object for a constructor;
> - the receiver for an instance or extension method without an `owned` return;
> - the returned object for a static method or a lambda;
> - both the receiver and the returned object for an instance or extension method whose return is `owned`.
>
> The bound is read at the member's own declaration and at every declaration it implements or overrides, as the announcement table is. Every store and forward must be severed by each object of the bound. A callee with a mint-announcing return (`async`, iterator) is never severed by its returned object.
>
> At an owned-return instance method that leaves one shape, the returned forward on `this` or on an `owned` chain from it (the hub idiom `=> _changed.Subscribe(h)`). Rejected there: a store into a `scoped` (or, class-typed, a plain) field of `this`, a `+=` or `.Add` forward that is not the returned value, and a returned forward over another receiver. An owned-return `async` or iterator method keeps no callback. A static method keeps a callback by a returned forward.
>
> The rule binds overrides, extension members and interface implementations, so stdlib §6.5's observer pattern holds through a user `IEvent<T>`. Stdlib §6.1 states that `Register`'s bound is its `Subscription` alone. The form-3 forward inside a callee (`_reg := tok.Register(cb)`, and form 1 composed with it) is severed by `this`.
>
> Declined (proposal 0215 §3): a caller co-bound by the receiver; a returned-object-only bound; a body-derived severed-by-return summary in the artifact interface; a trust clause on implementations, whose cost is that a user event source keeps its subscribers in an owned `EventSource<T>`. Recorded *liftable*: a returned binding, a conditional of returned forwards, a `Task<T>` result at an `async` callee, and a store into a constructed, returned local.
>
> **Migration.** A method declared `…(scoped h: F): owned H` that stored `h` in a field of `this`, or forwarded it with `+=` or `.Add` before returning something else, is now rejected. Either return no `owned` value (callers then bound by the receiver), or return the forward whose handle severs the registration. Rewrite a method that returned a forward over a token or a publisher it does not own (`=> tok.Register(cb)`) in one of two ways: store the returned `Subscription` in an `owned` field of `this` in a member that returns no `owned` value (`_reg := tok.Register(cb);`), or make it a `static` method returning the call. A method that returned a constructed holder (`=> new Button(on)`) becomes a `static` factory. A class- or interface-typed `scoped` parameter of such a method is no longer stored: use a static factory, or a method without an `owned` return. An `async` or iterator method with an `owned` return keeps no `scoped` callback. No caller's verdict changes. The tree held no rejected shape at the revision (proposal 0215 §5.1).
>
> Flipped:
> - gtest `expr_body_scoped_param_9201` `static_expression_body_forwards_scoped_param_off_object_rejected_spec_28_14`: reject → accept, rewritten as the static shape, with an instance-method reject twin added;
> - corpus `s28_14_0017`: repaired for §7.9, then decided per Q3.

(The static remedies are D2's. §7.3's trailer lists what changes if D2 is declined.)

## 9. Proposed rule-corpus cases

IDs are assigned in the landing revision's band. Each fixture declares `Counter2` (a class whose `Hit` increments and prints a count), as the probes do. Each `run` case loops its admitted shape 200 times and prints a total, so a dangling read cannot pass by luck (the one-shot trap, CLAUDE.md). Each `reject` case names its `diagnostic:` on the new §28.14 text, so it cannot pass on an unrelated error.

| case | expect | rule sentence (▲ §7.3 unless noted) | program |
|---|---|---|---|
| A | `reject` | "an instance or extension method whose return is `owned`: both the receiver and the returned object" | the issue's `Keeper` repro (`p1`) |
| B | `run` | item 2 (the receiver bound) | `Keep` with no `owned` return, caller bound by `k` (`p7`); `output:` the count |
| C | `run` | "a forward that is both — the returned call, on `this` …" | the hub idiom with captures shorter-lived than the hub, form 2 in a loop (`p4`) |
| D | `run` | the same, at forms 4/5 | `keep := hub.OnChanged(…)` with `c` declared before `hub`; `keep` disposed after the block (`p8`) |
| E | `run` | stdlib §6.5 + ▲ §7.8 | a pattern-shaped user `IEvent<double>` implementation and a TickObserver over a parameter-typed face |
| F | `reject` | ▲ §7.8 "never in storage of its own" | the `IEvent<double>` implementation that stores in `this` (`p3`) |
| G | `reject` | "severed by the returned `Subscription` alone" | instance `OnCancel(…) => tok.Register(cb)` (`p2`) |
| H | `run` | item 3 + the returned forward (D2) | static `OnCancel` (`p6`), registered and cancelled in a loop |
| I | `reject` | item 4 | instance `Make(scoped on): owned Button => new Button(on)` (`p9`) |
| J | `run` | item 3 + the returned forward (D2) | `p11`'s static `Open(scoped c) => new Account(scoped c)` |
| K | `reject` | ▲ §7.4 | `Hold(scoped src: Item): owned Token2` storing `_src := src` (`p5`) |
| L | `reject` | "a binding of it returned later … not read through (*liftable*)" | `var s := _changed.Subscribe(h); return s;` |
| M | `reject` | ▲ §7.7 | a `Register` call whose bounding object is a token-typed receiver at form 5, capture-bearing |
| N | `reject` | "the `+=` spelling included … severed by `this`" + item 4 | `Keep(scoped h): owned Token  _changed += h; return new Token(); end` |
| O | `reject` | the same | `Keep(scoped h): owned Token  _changed.Add(h); return new Token(); end` |
| P | `run` | "the whole operand of a `return`" | the 9274 block shape `return _changed.Subscribe(h);` (the `ReturnStmt` returned-position pin) |
| Q | `reject` | "such a callee whose return is `owned` keeps no callback" | pc-0215's `SubAsync(scoped h): owned OwningTask<Subscription>  return _changed.Subscribe(h);` |
| R | `reject` | "the bound is read … at the declaration a member implements" | `m1`: an interface `Watch(scoped h): owned IEnumerable<int>;` and an implementation spelling the return unmarked that stores `_h := h` (once the compiler implements §11.2's carve; until then the case rejects on the §11.2 mismatch, so its `diagnostic:` must name the §28.14 text) |
| S | `run` | "form 1 composed with its placement" (Q8) | a constructor `_child := new Child(h)` with `Child(scoped h)` storing it, the owner disposing `_child` in its cascade |

## 10. Owner questions

1. **Q1, the rule.** Adopt D, the callee's bound with "both" at owned-return instance methods, read at the implemented or overridden declaration, with no returned-object severance at an `async` or iterator callee, as the answer to #9275? Recommended. It is the only option that closes `p1` (with its `+=` and `.Add` spellings), `p3`, `p5`, `m2`, `p2`, `p9` and the `async` shape without changing a caller's verdict or adding an interface fact. The cost: a user type can no longer implement `IEvent<T>` with its own subscriber storage, even behind a handle that severs it. It must wrap an owned `EventSource<T>`. The measured tree has no such implementation.
2. **Q2, the static half (D2).** Land the returned-forward severance at static methods, and the returned constructor forward it needs, together with Q1? Recommended.
   - It is the same fact the instance rule needs.
   - It settles the reading on which the 9201 gtest and the `p11`/`s28_14_0017` readers disagree.
   - Several remedies depend on it: §7.4's static factory, §7.7's and §7.11's static spelling, §8's "becomes `static`", and Q3's `=> new Registry(cb)`. Without D2 the remedy everywhere is form 3 or "return no `owned` value" (§7.3's trailer).
   - Cost: one gtest row flips from reject to accept.
3. **Q3, `s28_14_0017` (decide after Q2).** A static `Create` that constructs `r`, stores `r._cb := cb` and returns `r`. The case first needs its §7.9 initialization error repaired (§5.2). Its v0.361 verdict is then `run`: the text admits the shape.
   - (i) Admit it as a v1 shape. That needs flow facts: `r` is returned on every path and not otherwise transferred.
   - (ii) Narrow it to a *liftable* v1 bound, re-headed `reject` with a `diagnostic:`, with the remedy twin `=> new Registry(cb)` as `run` (D2 only). This narrows a text-admitted shape. No program's compiled verdict changes, because the compiler already rejects it.
   - Recommended: (ii) if Q2 is yes. If Q2 is no, defer the case, since (ii) would leave the shape with no remedy.
4. **Q4, the class-typed half.** Land §7.4 in the same revision? Recommended. The scoped-construction bullet uses the same term, and `p5` is the same hole. Declining leaves it open with a stated gap.
5. **Q5, the ramp.** Go straight to an error, with no warn-first stage? Recommended. The rejected shapes are use-after-frees, and the measured tree holds none of them (§5.1). A ramp would be a Ratchet with nothing to ramp.
6. **Q6, `Register`.** Confirm §7.7's sentence as a standard-library register obligation. It records that no form 4/5 resolution of a token receiver admits a capture-bearing callback, which §4.4 and pc-0215 believe holds today (HYPOTHESIS). It is not a new caller rule.
7. **Q7, form 2's "a call whose return is `owned`".** Does it include an unmarked mint-announcing return, which the announcement table classifies as "Owned frame" (row 2) and §11.2 lets match an `owned` spelling? The checker reads the marker: `m3_unmarked` binds by the receiver, `m2_marked` by the returned frame. The bound follows the same reading.
   - If yes, every instance callee with a mint-announcing return is in item 4, and by the F1 sentence keeps no callback.
   - If no, an unmarked one is in item 2 unless a declaration it implements or overrides is marked.
   - Recommended: no (the marker), for this revision, since it is what call sites read today. The question of aligning form 2 with the announcement table goes to a separate issue. The measured tree has two unmarked mint-announcing callees with a `scoped` parameter, and both only invoke, so neither reading flips anything.
8. **Q8, the form-3 forward inside a callee.** Admit `_child := new Child(h)` (form 1 composed with form 3's placement, v0.361's #8397 composition) as a forward severed by `this` at a constructor or an item-2 method? Recommended: yes. It is sound by §4.5's result route, and v0.361 already states the composition for the admission arm. HEAD rejects every constructor forward, so this widens the checker. The `Register` and `Subscribe` form-3 forwards are already admitted and pinned.

## 11. Files and probes

The proposal's probes are in `/var/tmp/bezel-scratch/v361/p-0215/probes/`. They were compiled with `/var/tmp/bezel-scratch/r42/x/bezelc-0.0.0-8ccf7ff0-linux-x86_64/bin/bezelc` (`8ccf7ff06`), each in the default mode and with `--checked-disposal`. `--sanitize=address` could not link on this host, because the ASan runtime archives are absent. Run logs sit beside the sources (`*.default.run.txt`, `*.checked.run.txt`). The critique's probes (`a1_async_form2`, `a2_async_order`, `c17`, `l1_lambda_scoped`, `m1_mint_iface`, `m2_marked`, `m3_unmarked`) are in `/var/tmp/bezel-scratch/v361/pc-0215/probes/`.

| probe | compile | run (default / checked) |
|---|---|---|
| `p1_keeper` | ok | SEGV / SEGV |
| `p2_oncancel` | ok | `hit 32566`, then abort "refcount over-release (closure)" / abort |
| `p3_iface` | ok | SEGV / SEGV |
| `p4_hub` | ok | `hit 1`, then abort (the r42 forward defect, §5.3) |
| `p5_classtyped` | ok | exit 0 / `DisposedAccessException` |
| `p6_static_oncancel` | ok | abort (the r42 forward defect) |
| `p7_keeper_unit` | ok | `hit 1` / `hit 1` |
| `p8_hub_form5` | ok | `hit 1`, then abort (the r42 forward defect) |
| `p9_instance_factory` | ok | SEGV / SEGV |
| `q1_direct_subscribe`, `q2_capfree_subscribe` | ok | clean (`hit 1` ×3, `done`) |
| `q3_hub_block` (`return _changed.Subscribe(h);`) | rejected by r42's pre-#9274 forward test | — |
| `q4_add_forward` (`_changed += h;`) | rejected by r42's pre-#9274 forward test | — |
| `pc-0215/m2_marked` | ok | SEGV (form 2 binds by the frame) |
| `pc-0215/m3_unmarked` | rejected (form 2 not resolved; bound by the receiver) | — |
| `pc-0215/a1_async_form2` | ok | runs (form 2 binds by the task) |

Scans: `/var/tmp/bezel-scratch/v361/pc-0215/tools/scan3.py` (output `pc-0215/scan3.tsv`) supersedes the first draft's `/var/tmp/bezel-scratch/v361/p-0215/scan2.py`.

## 12. Critique record

### 12.1 Round 1 — pc-0215, an independent adversarial critic, on the first draft (`c37239b57`)

Verdict: SOUND-WITH-FIXES. Every finding is applied in this revision:

- **F1 (blocking).** The returned-forward severance was unsound at an `async` callee, whose returned object is the task. The mint-announcing sentence is in ▲§7.3, §3.4, §4.1, ▲C.3, §8, and case Q.
- **F2 (blocking).** The first draft relied on §11.2 keeping `owned` identical across implementations, which §11.2 does not do for mint-announcing returns. The bound is now read at the implemented or overridden declaration (▲§7.3, §4.1, §5.3 item 1, case R, Q7).
- **F3.** The induction now covers the form-3 forward and the #8397 composition (§4.5, Q8). Its base is narrowed for standard-library callees whose bound omits the receiver. Lambdas and extension members are added to the table.
- **F4.** The #9274 `+=` arm is item 2b of §5.3. Cases N, O and P are added.
- **F5.** The D2-decline fallback in §7.3's trailer is complete. The `NewExpr` arm is in §5.3 item 3. Q3 is conditional on Q2.
- **F6.** ▲§7.7, ▲§7.11 and §8 name both spellings: the pinned form-3 spelling and the static one.
- **F7.** `s28_14_0017`'s §7.9 defect is stated and repaired first. Q3 (ii) is labelled a narrowing of a text-admitted shape.
- **F8.** The counts are 37 instance methods and 11 static (§0, §5.1), with the four rows the first scan missed listed.
- **F9.** The `IEvent<T>` cost is stated in §0, §3.6, ▲§7.8, ▲§7.9 and Q1. F's decline reason is softened.
- **F10.** The caller-side sentence is now a standard-library register obligation (▲§7.3, ▲§7.7, Q6).
- **F11.** The bound-6 constraint is in ▲§7.3's normative text.
- **F12.** The Status prose is updated.

### 12.2 What a further critic should attack first

- The *Inheritance* keying across modules: an implementation in one module of an interface declared in another reads the interface's return from its imported metadata, which already carries it. Confirm it on a cross-module probe.
- Q7's interaction with §11.2 once the compiler implements §11.2's carve.
- Whether any standard-library callee other than `Register` has a stated bound that omits the receiver.
