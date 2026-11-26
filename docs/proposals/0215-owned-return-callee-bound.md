# Proposal 0215 — The callee's bound: an owned-return instance method stores a `scoped` argument only where both of its call-site bounds sever it

| | |
|---|---|
| **Status** | under-review — draft (2026-11-26), written from the v0.361 L2 lane's design finding on #9275; no critique pass yet |
| **Tracking issue** | [#9275](https://github.com/s8lap/bezel-lang/issues/9275); related: [#8397](https://github.com/s8lap/bezel-lang/issues/8397) (ruled v0.361, the registration-lifetime residual), [#9311](https://github.com/s8lap/bezel-lang/issues/9311) (ruled v0.361, bound 6 stays deferred) |
| **Target documents** | spec §28.14 (*The `scoped` modifier*, the callee bullet's first sentence; *The registration-lifetime residual*, one sentence; a new paragraph *The callee's bound* before *Quiescence*; *Class- and interface-typed `scoped` parameters*, the callee-side bullet, one sentence), §28.3 axiom register (row A11), Appendix C.3 (one bullet). stdlib §6.1 (`Register`, one sentence), §6.5 (*Subscribing*, one sentence). rationale §5.26 (the A11 entry of *The axioms' reasons*, one paragraph). Guide 1 §9 (one sentence). Code-generation reference §4 (*`scoped`*, one sentence) and §7.2 (one entry). No grammar change, no new keyword, no interface-artifact (§2.8.2) change. |
| **Written against** | `origin/main` `bd12fe038` (v0.360) and the v0.361 ruling text on `spec/v0361-2026-11` at `c85ce271c`, whose §28.14 already carries the #8397 and #9311 rulings; every quoted anchor below is v0.361's text. Section numbers are v0.361's. |
| **Origin** | The v0.361 L2 lane (`/var/tmp/bezel-scratch/v361/lanes/l2.md`, the #9275 section) classified #9275 as a design question, not a ruling, and recommended a callee-side rule; critic A confirmed nothing of it slipped into v0.361 (`critic-a.md`, L2). |
| **Change class** | Design change ([`spec-change-process.md`](spec-change-process.md) §2): it newly rejects callee bodies the text and the compiler admit, and it decides what "the bounding object" means at a callee, which no text states. It is an extension of axiom A11, not a ratchet. |

*Conventions. An unprefixed § is a language-spec section at v0.361; stdlib, rationale and guide sections are marked. Evidence labels: MEASURED (a probe run or a scripted count, reproducible from §11), QUOTED (repository text at the cited place), HYPOTHESIS (reasoning nothing here measures). ▲ marks proposed normative text. "The forms" are §28.14's bounding-object forms 1–5. "Severed by X" means: after X's teardown completes, the stored callback is never invoked again (for a class-typed borrow: the storing object is disposed no later than X).*

## 0. Summary

§28.14 states the callee's storage obligation against "the bounding object", and resolves the bounding object at each call site by the forms. At an instance method whose return is `owned`, call sites resolve two different objects: the returned object (forms 2 and 3) or the receiver (forms 4 and 5). The text never says which of them the callee's body is checked against. The checker uses `this`, so a method that stores the callback in a `scoped` field of `this` and returns an unrelated handle is admitted, and a caller that bounds its captures by the handle (`using var tk := k.Keep(lambda … c …)`) gets a use-after-free (MEASURED: SEGV). The same hole is reachable through interface dispatch, where no caller can see the body: a user implementation of `IEvent<T>.Subscribe` that keeps the handler on `this` breaks stdlib §6.5's observer pattern (MEASURED: SEGV).

Every caller-side repair either rejects programs the text admits and stdlib §6.5 calls normative, or needs a body-derived fact carried in the artifact interface, which `DECLINED.md` records against. The repair belongs at the callee, and it has one sound shape: the callee is checked against every object its call sites can name. This proposal names that set the callee's bound:

| callee | call-site forms that can resolve | the callee's bound |
|---|---|---|
| constructor | 1 | the constructed object (`this`) |
| instance method, return not `owned` | 4, 5 | the receiver (`this`) |
| static method (return `owned`, required) | 2, 3 | the returned object |
| instance method, return `owned` | 2, 3 and 4, 5 | both the receiver and the returned object |

Each store and each forward must be severed by every object of the bound. At an owned-return instance method that leaves exactly one shape: a forward that is the method's returned value, on `this` or on a member chain rooted at `this` through `owned` fields. That is the documented hub idiom, `OnChanged(scoped h): owned Subscription => _changed.Subscribe(h);`, and it stays admitted at every caller form. The `Keep` shape, the interface variant, the class-typed variant and the instance-method factories are rejected at the callee. No caller verdict changes.

The L2 lane's wording ("may store the callback only as a forward whose result is the value it returns") is this rule's return half. Read alone it is unsound at forms 4 and 5 (§3.3 shows two counterexamples), so the recommendation keeps the existing forward clause's `this` half beside it.

A scan of the 12,084 tracked `.bzl` files and the tracked C++ sources finds 33 owned-return instance methods with a `scoped` parameter. None stores the parameter in a way this rule rejects (MEASURED, §5.1). The core rule flips no test. The optional static half (§4.2, Q2) flips one gtest row from reject to accept, and the liftable bound in §4.1 decides one corpus case (Q3).

## 1. The problem

### 1.1 The text

QUOTED, §28.14 *The `scoped` modifier* (v0.361):

> The callee may store the callback, but only in `scoped` fields of the bounding object itself (below), or forward it to another `scoped` parameter whose bounding object is the same object; it may invoke it. It may not return it, move it into non-`scoped` storage, or store it in any other object. […] These obligations are compiler-verified for user-authored callees. A `scoped` parameter on a static method is admitted only when the method's return is `owned`, so that a call site can resolve bounding form 2 or 3 […]

> The forms are tested in order; the first that resolves determines the bounding object

The bounding object is a fact of a call site. The callee's obligation uses the same words, "the bounding object itself", but a callee body has no call site. For a constructor (form 1) and for an instance method with no `owned` return (forms 4/5 only), every call site names `this`, so the reading is forced. For a static method, every call site that admits anything names the returned object (forms 2/3), and the text says so. For an instance method with an `owned` return, call sites name either, and the text is silent.

QUOTED, the checker (`src/ownership/Ownership_Checks.cpp`, `scopedCalleeWalkStmt` and `scopedCalleeWalkExpr` at `bd12fe038`): a store `_h := h` into a `scoped` field is admitted when the target is a field of `this` ("Store INTO a scoped field of `this`: RHS must be a scoped parameter of this method"), and a forward is admitted when the inner call's bounding object, resolved by the forms inside the callee, is `this` (the #9274 forward test). So the checker reads the callee's bounding object as `this` for every instance method, owned return or not.

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

Each of these was probed with the r42 build (§11). The verdict column is what r42 does. The HEAD column comes from reading `bd12fe038`'s checker.

| probe | shape | r42 | HEAD (reading) | dangling path |
|---|---|---|---|---|
| `p2_oncancel` | instance `OnCancel(tok, scoped cb): owned Subscription => tok.Register(cb)` | compiles; prints `hit 32566` (a read of freed memory), then aborts | HYPOTHESIS: rejected by the #9274 forward test (the inner call resolves `tok`, not `this`) | form 5: `keep := r.OnCancel(src.Token, lambda() => c.Hit())` bounds by `r`; `keep` outlives `r` and `c`; `src.Cancel()` fires into `c` |
| `p3_iface` | `class Ticker: IEvent<double>` whose `Subscribe` stores the handler in a `scoped` field of `this` and returns `_dummy.Subscribe(lambda(x) => ())` | compiles; SEGV, both modes | HYPOTHESIS: admitted (the same store path as `Keep`) | form 2 through the interface face: `using var sub := face.Subscribe(lambda(t) => c.Hit(t))` |
| `p5_classtyped` | `Hold(scoped src: Item): owned Token2` storing `_src := src` into a plain field of `this` | compiles; default mode exits 0, checked mode `DisposedAccessException` | HYPOTHESIS: admitted (`exprIsReceiverPlainFieldOfThis`) | form 2: `using var tk := sh.Hold(scoped it)`; `sh.Read()` after `it` is disposed |
| `p9_instance_factory` | instance `Make(scoped on): owned Button => new Button(on)` | compiles; SEGV, both modes | HYPOTHESIS: rejected by the forward test (form 1 resolves the constructed object, not `this`) | form 5: `keep := f.Make(lambda() => c.Hit())` bounds by `f`; `keep` outlives `c` |

r42 (`8ccf7ff06`, 2026-11-19) predates the #9201 fix that walks expression bodies, which is why `p2` and `p9` compile there. On HEAD the two live holes are the stores, `p1`/`p3` (callbacks) and `p5` (a class-typed borrow). The forwards `p2`/`p9` are rejected on HEAD only because the checker reads the callee's bound as `this`, and the text does not say that either.

The interface variant (`p3`) is the decisive one. Stdlib §6.5 makes the observer pattern normative: "Storing a `Subscription` in an `owned` field of `this` resolves spec §28.14's bounding object to `this` (form 3), so a handler capturing `this` … is admitted *regardless of how the publisher was reached* — a parameter, a field, an interface face" (QUOTED). With `ticks: IEvent<TickEvent>` a parameter, the caller sees only the interface. If a user implementation may keep the handler on itself, that sentence is false, and no caller-side rule can repair it, because the body is not visible at the call.

### 1.4 Why this is design-level

A clarification would state a rule the text already implies. Here every candidate rule changes a verdict. Reading the callee's bound as `this` (the checker) leaves the use-after-free. Reading it as the returned object rejects the hub idiom's sibling shapes and, read alone, is unsound at forms 4 and 5 (§3.3). Any caller-side reconciliation rejects caller programs the text admits. The L2 lane reached the same conclusion, and the triage's own draft ruling called its callee summary "the new mechanism".

## 2. What a sound rule must satisfy

A call site's admission is sound when every stored invocation ends before the bound the call site chose is torn down. The call site cannot see the body, so the guarantee must come from the callee. The callee cannot see the call site, so it must give the guarantee for every object a call site might choose.

- An instance method with an `owned` return is called at forms 2/3 (the bound is the returned object `R`, or the caller's `this`, which holds `R` in an `owned` field) and at forms 4/5 (the bound is the receiver, or the chain head the receiver is reached from through `owned` fields). Its storage must be severed by `R`'s teardown and by the receiver's teardown.
- A static method is called only at forms 2/3. Forms 4/5 name no receiver, and a capture-free callback is all the text admits without one. Its storage must be severed by `R`.
- A constructor is called at form 1, and an instance method without an `owned` return only at forms 4/5. Their storage must be severed by `this`.

This is the issue's interpretation (c), the intersection, made exact: "both" applies only where the call sites can actually name both objects. The rest of the proposal is how to check it structurally, with no knowledge of any call site and no new interface fact.

## 3. The options

### 3.1 A — Leave as is

Traces: `p1`, `p3` and `p5` stay admitted with a use-after-free. Soundness: broken at a form the text names as "the tightest bound". Cost: nothing. Not viable. It is listed because "leave as is" must be weighed, and because declining it is itself recorded.

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

The callee is checked against the bound of the §0 table, and each store and forward must be severed by every object in it:

- A store into a `scoped` field of `this` (for a class-typed parameter, into a plain field of `this`) is severed by `this`.
- A forward whose inner call resolves `this` by the forms inside the callee (a call on `this`, form 5, or on a member chain rooted at `this` through `owned` fields, form 4) is severed by `this`. This is the existing forward clause and the #9274 checker test.
- A forward whose call is the value the callee returns, with an `owned` result, is severed by the returned object. The forwarded callee's own bound contains its result: a constructor's constructed object, a static method's returned object, and an owned-return instance method's returned object.

At an owned-return instance method a store is therefore never admitted, and a forward is admitted when it is both: the returned value, on `this` or on an `owned` chain from `this`.

Traces:

- Keeper (`p1`): the store is severed by `this` only, so it is rejected at the callee. Remedy: drop the `owned` return (`p7_keeper_unit`, MEASURED: runs, `hit 1`). Call sites then resolve the receiver, which the store is severed by.
- Hub idiom (`p4`, `p8`): `=> _changed.Subscribe(h)`. The inner call is on an `owned` field of `this`, so it is severed by `this`, and it is the returned value, so it is severed by the `Subscription`. Admitted, and sound at every caller form. Form 2 is bounded by `sub`, and disposing it removes the handler (stdlib §2.11). Form 3 is the observer pattern. Forms 4/5 are bounded by the hub, whose `_changed` destructor severs the subscriber list (stdlib §6.5).
- `p2`, instance `OnCancel`: severed by the `Subscription` only, so rejected. Remedy: make it static (§4.2).
- `p3`, `IEvent<T>` user implementation: the implementation is an owned-return instance method, so the store is rejected. The pattern-shaped implementation, `public method Subscribe(scoped handler: (double) -> ()): owned Subscription => _src.Subscribe(handler);` over `private owned readonly _src: EventSource<double>`, is admitted. Stdlib §6.5's "regardless of how the publisher was reached" becomes true for every implementation.
- `p5`, class-typed: the store is severed by `this` only, so rejected. A storing object has one owner, so no field store is severed by both objects of the bound. The spellings that store are a static factory (`p11` in the static-ownership research, `=> new Account(scoped c)`) or a method without an `owned` return.
- `p9`, instance factory: severed by the returned `Button` only, so rejected. Remedy: a static factory.

The spec's own examples (traced in §4.4) keep their verdicts.

Soundness: §4.5. Ergonomics: the rejected shapes have one-line remedies, and the dominant shape (the hub idiom, every owned-return instance method with a `scoped` parameter in the corpora) is untouched. Cost: callee-local, in the walk that already exists (§5.3). Migration: zero test flips for the core rule (§5.1).

### 3.5 E — The triage carve: a caller co-bound unless the callee is "severed by return"

Each callee would compute a per-parameter fact, "every store or forward is severed by the returned value", export it in the artifact interface (§2.8.2), and the caller would co-bound forms 2/3 by the receiver when the fact is absent.

Traces: Keeper is co-bounded and rejected at form 2. The hub carries the fact and is admitted. `p3` reaches the callee through an interface, so there is no body and no fact, and the call is either co-bounded (breaking §6.5, as B does) or not (leaving the hole). `p2`/`p9` are severed-by-return, so they are admitted, and they dangle at form 5 as under C.

Soundness: as C at forms 4/5, and as A or B at interface dispatch. Design: the caller's verdict would depend on another declaration's body. `DECLINED.md` records that against a callee-body retention summary at the call (`rejected`, "the caller's verdict would depend on another declaration's body, which no per-body fact carries … and would change when the callee became virtual, an interface member, or imported", spec §28.3, rationale §5.24) and against a body-derived fact carried in the interface for diagnostics (`deferred`, proposal 0132 §7). E would carry one for verdicts. It reopens a `rejected` row with no new evidence against its reasons. Declined.

### 3.6 F — A recorded trust clause for implementations

Stdlib §6.5's *Subscribing* contract ("disposing the subscription removes the handler") would become a K4.3 trust clause on every implementation of an owned-return `scoped` interface method, named by the trust-admission report.

This covers `p3` only, by trust. The direct `Keeper` (no interface) needs the static rule anyway, and once D holds, F's clause is decidable, so recording it as trust would contradict §28.14's "These obligations are compiler-verified for user-authored callees". Declined.

### 3.7 Comparison

| | Keeper `p1` | interface `p3` | class-typed `p5` | `p2`/`p9` at form 5 | hub idiom, all forms | stdlib §6.5 observer | caller verdicts change | new interface fact |
|---|---|---|---|---|---|---|---|---|
| A | UAF | UAF | dangles | UAF on r42 | admitted | holds by trust only | no | no |
| B | rejected | UAF or §6.5 broken | rejected | UAF | rejected at form 2 | broken | yes | no |
| C | rejected | rejected | rejected | UAF | admitted | holds | no | no |
| D | rejected | rejected | rejected | rejected | admitted | holds | no | no |
| E | rejected | UAF or §6.5 broken | rejected | UAF | admitted | broken or held by trust | yes | yes |
| F | UAF | trust clause | dangles | UAF | admitted | holds by trust | no | no |

## 4. The recommendation

Adopt D. Land the static half (§4.2) and the class-typed half (§4.3) with it.

### 4.1 The rule

The callee's bound is the §0 table. Every store and every forward of a `scoped` parameter must be severed by each object of the bound, under the three severance facts of §3.4. Its v1 shape is purely syntactic:

- "the value the callee returns" means the forward's call is the whole expression body, or the whole operand of a `return` statement. A binding of the call returned later (`var s := _changed.Subscribe(h); return s;`) and a conditional over two such calls are not read through. Both are recorded *liftable*.
- A `return` inside a nested closure returns that closure's value, not the callee's, so a forward there is never severed by the callee's returned object. *v1 admission bounds* item 6 rejects every callee-side use inside a nested closure in any case (v0.361, #9311). So bound 6 stays deferred and composes: a future lift of bound 6 would not reach this rule.
- A store into a `scoped` field of a local the callee constructs and returns (`s28_14_0017`'s static shape) is not a v1 shape (Q3). The remedy is to forward to a `scoped` constructor parameter (`=> new Registry(cb)`).

The rule binds every user-authored body of an owned-return instance method: an `override`, and an implementation of an interface method whose return is `owned`. A caller resolving either object through an interface face therefore relies on a body that has met both. No interface or artifact fact is added. §11.2's exact-match rule already keeps the `owned` return and the `scoped` modifier identical between the interface and the implementation.

### 4.2 The static half (D2)

For a static method, the text already makes the returned object the bound ("so that a call site can resolve bounding form 2 or 3"). What it lacks is the fact that a forward whose call is the returned value is severed by it. Without that fact a static method can forward to nothing: the 9201 gtest row reads it so ("a static method has no `this`, so NOTHING is the same bounding object — every forward of its own `scoped` parameter is off-object"). Two other readers took the opposite reading: the static-ownership research's `p11` (`static Open(scoped c: Customer): owned Account => new Account(scoped c)`, "EXPECT (spec §28.14): compiles; no report line; static bound") and corpus case `s28_14_0017` (a store into the returned local's `scoped` field, `expect: run`, baseline `fail divergence`). D2 settles the question with the same severance fact the instance rule needs. It admits `=> new Account(scoped c)`, `=> tok.Register(cb)` (`p6`) and `=> new Sink().Make(q)`, and it is the stated remedy for the rejected instance shapes. If the owner declines D2, the instance rule still stands. Its remedy text then names only "drop the `owned` return".

### 4.3 The class-typed half

Scoped construction applies "the same two-sided contract to a passive borrow", and its callee-side bullet uses the same words, "the bounding object". `p5` is the same hole. The severance fact for a borrow is "the storing object is disposed no later than X". A field store in `this` is disposed with `this`, and the object behind a returned forward is the returned object. No field store is disposed by two owners, so at an owned-return instance method no class-typed store is admitted, directly or through a forward. The rule needs no separate text. The proposed sentence in the scoped-construction bullet states the consequence and the two spellings that store.

### 4.4 The spec's and the library's examples, re-traced

- §28.14 `Button` (a constructor, bound `this`): `_onClick := onClick` is unchanged.
- §28.14 `Reader` (a constructor, class-typed): `_src := src` is unchanged.
- Guide 1 §9 `Timer.OnTick` (instance, no `owned` return): the store into `_onTick` is unchanged.
- stdlib §6.5 the event pattern's `_clicked += h` (a forward in a non-owned-return member) is unchanged. `EventSource<T>.Subscribe` and `Add` are compiler-known, and their bounds are the stated ones: `Subscribe` is severed by the destructor ("severs the subscriber list before reclamation") and by the `Subscription` ("disposing the subscription removes the handler"), so both. `Add` is severed by `this`.
- stdlib §6.5 TickObserver: holds through any implementation (§3.4).
- stdlib §6.1 `CancellationToken.Register`: an owned-return instance method whose stated bound is the returned `Subscription` alone ("the registration stores the callback bounded by the returned `Subscription`", QUOTED). Its receiver is a token value whose teardown severs nothing. HYPOTHESIS: no form 4/5 resolution of a token receiver reaches an admitted object today. A token is not `IDisposable`, so it is never a `using var` local, and a token field is not an `owned`-field hop. The proposed stdlib sentence states this as the rule rather than as a coincidence: a `Register` call whose bounding object resolves to the receiver admits capture-free callbacks only. That is a clarification of a compiler-known callee, which the spec's new paragraph generalizes ("a standard-library callee's bound is the one its section states").
- stdlib §6.10 `TaskScope.Spawn` (no `owned` return), stdlib §2.21 `ScopeGuard` (a constructor): unchanged.
- §28.14 form 3 via #8397's form-1 composition (`_child := new Child(scoped this)`): a constructor, bound `this`, unchanged.
- Function values: a method reference over an owned-return method keeps `scoped` in its type (§27.1). A call through it resolves forms 2/3 only, since there is no receiver, and both kinds of owned-return callee have the returned object in their bound. No new hole.

### 4.5 Soundness (HYPOTHESIS, argued by induction on forward depth)

Claim: at every call site that admits a capture-bearing callback, every invocation of the stored callback happens before the bound the call site resolved is torn down.

- Base: a store into a `scoped` field of `this` is invoked only by the declaring type's bodies through `this`, so it ends with `this` (*Quiescence*, K2.3's residual for user types, K4.1 as the backstop). A compiler-known callee severs as its section states.
- Step: a forward on `this` or on an `owned` chain from `this` reaches an inner callee whose bound includes its receiver (items 1, 2 and 4 of the table). That receiver is disposed no later than `this`, so the inner storage is severed by `this`. A forward that is the returned value reaches an inner callee whose bound includes its result (items 1, 3 and 4). That result is the outer returned object, so the inner storage is severed by it.
- Call site: forms 2/3 resolve the returned object `R` (form 2: `L` is `R`; form 3: `R` sits in an `owned` field of the caller's `this`), and every callee whose call sites can resolve forms 2/3 has `R` in its bound. Forms 4/5 resolve the receiver or the chain head it is reached from through `owned` fields, and every callee whose call sites can resolve forms 4/5 has the receiver in its bound. Form 1 resolves the constructed object, which is the constructor's bound.

The residuals are the ones §28.14 already records, now named once (Interaction 1 of the L2 lane, with #8397):

- Form 2 carries none, because a `using var` cannot be moved out (§28.10, A5).
- Form 3 is the handle-holding `owned` field moved out of the caller's `this`.
- Forms 4/5 are "a storing owned child" moved out of the callee: `_changed` moved out of the hub. This needs a `move` of an `owned` field, and the event pattern's field is `readonly`.

All three are *The registration-lifetime residual*. The proposal adds one sentence there so that the form-2 case is stated.

## 5. Measurement, migration and implementation

### 5.1 The corpora (MEASURED, `scan2.py`, §11)

Over every tracked `.bzl` file (12,084: the rule corpus, `tests/conformance/cases`, `tests/lit`, the research and ergonomics programs) and the tracked `.cpp` sources (gtest and `src/`), with C++ string literals joined, the scan finds 44 methods with a `scoped` parameter and an `owned` return. 33 are instance methods and 11 are static. (The normative documents were scanned separately: their only such declarations are the stdlib's `Register` and `Subscribe`.)

- Instance methods (33):
  - stdlib signature mirrors (`Register`, `Subscribe` in `tests/conformance/cases`) and one parser-test interface declaration: 14. Two more hits are text inside `src/` comments.
  - The hub idiom `=> _changed.Subscribe(handler)`: 8 (four in the 9056 ownership and module tests; the ergonomics programs `n17-observer/v2` and `v3`; `repros/NB13`; one bug-report note). Admitted under D.
  - `=> this.Changed.Subscribe(handler)`: 1 (9056). A getter hop. It is rejected today, and under D it is rejected for the same reason.
  - Bodies that never store the parameter (`=> new Sub()`, `h(); return new Sub();`, `return new Sink();`): 8 rows across the 3195 (5), 3189 (2) and 9201 (1) tests. Admitted under D.
  - Keeper-shaped stores: 0. Implementations of `IEvent<T>` or `IAsyncEvent<T>` outside the stdlib: 0 (`git grep` for a class implementing either).
- Static methods (11). Six `lit/sema` `Take` rows that only invoke: unchanged. `9201` `S(scoped s): owned Sink => s`: a return sink, rejected, unchanged. `9274` `On(…): owned Tk  _bus.Add(handler); return new Tk();`: rejected because a static field is not a `this` hop, and under D also not the returned value, unchanged. Three are touched only by Q2 and Q3:
  - `9201` `S(scoped q): owned Sink => new Sink().Make(q)`: today rejected. Under D2 it is admitted, because the call is the returned value and `Make`'s bound contains its result.
  - `p11_scoped_factory_form2` (research probe, not a test): admitted under D2, as the research expected.
  - `s28_14_0017`: decided by Q3.

### 5.2 Tests that flip

| test | today | under D | under D + D2 | under Q3 (ii) |
|---|---|---|---|---|
| `tests/ownership/expr_body_scoped_param_9201_test.cpp` `static_expression_body_forwards_scoped_param_off_object_rejected_spec_28_14` | reject (`kScopedForward`) | reject | accept: the row is rewritten as the admitted D2 shape, and a reject twin over an instance method is added | — |
| `tests/conformance/rules/cases/s28_14_0017.bzl` | `run` (baseline `fail divergence`) | unchanged | unchanged | `reject`, re-headed with the v1 bound; the remedy `=> new Registry(cb)` gets its own `run` case |

No other gtest, lit or corpus verdict flips. The diagnostic texts in `scopedRestrictedUse` ("… or forwarded to a `scoped` parameter of a call on `this`") stay true for the old shapes. The new rejections get a new diagnostic (§7.1). A landing must still run `scripts/pregate_stale_needles.py` over any text it renames.

### 5.3 Implementation sketch (non-normative; `src/` read at `bd12fe038`)

All of it is in `MethodOwnership`'s callee walk (`src/ownership/Ownership_Checks.cpp`). There is no caller-side change, no `.bezel.meta` change and no module-boundary work, because an implementation is checked in its own module.

1. A per-body flag, set where the walk starts (`scopedCalleeScan` for block bodies; the expression-body entry in `src/ownership/Ownership.cpp` beside the #9201 walk): the enclosing callable is an instance method (not a constructor) whose declared return is `owned`.
2. In `scopedCalleeWalkStmt`, under that flag, the two admitted store arms report the new diagnostic instead of returning: the kind-1 store into a `scoped` field of `this`, and the kind-3 store into a plain field of `this`.
3. In `scopedCalleeWalkExpr`'s forward arm (kinds 1 and 3), under that flag, the admitted forward additionally requires the call to be in returned position. That position is set by the `ReturnStmt` case and the expression-body entry, and only when the returned expression is the call node itself. For D2, at a static owned-return method, a forward in returned position whose callee's result is `owned` is admitted where today it reaches the off-object report.
4. Diagnostics (proposed wording): "`h` is a `scoped` parameter of a method whose return is `owned`: callers may bound its captures by the returned object or by the receiver, so it may be kept only by a forward that is the method's returned value, called on `this` or on an `owned` field of `this` (§28.14 *The callee's bound*)". For a class-typed parameter: "… a method whose return is `owned` cannot store a `scoped` borrow parameter: the storing object would be disposed by only one of the returned object and the receiver; make the method `static`, or return no `owned` value (§28.14)".

Estimate (HYPOTHESIS): 80–150 lines plus two diagnostics, one Sonnet lane. Pins: `p1`, `p3`, `p5`, `p2`, `p9` reject; `p4`, `p8`, `p7` and a pattern-shaped `IEvent<T>` implementation run; with D2, `p6` and `p11` run. A runtime leg loops each admitted shape (the corpus cases of §9 do so).

A side observation, not this proposal's question: on r42 every program that forwards a closure through a user wrapper into `Subscribe` or `Register` aborts with "refcount over-release (closure)" (`p4`, `p6`, `p8`). The direct `src.Subscribe(lambda …)` twin runs clean (`q1`, `q2`). r42 predates the #9201/#9274/#9310 fixes. The landing lane should re-probe the admitted shapes on HEAD before banking their `run` rows, and file this if it reproduces.

## 6. Declined, and the `DECLINED.md` row text

Per `DECLINED.md`'s scope rule (2), the rows are indexed only once this proposal is accepted. The proposed text, class `declined-at-proposal`, record "proposal 0215 §3":

1. *A caller-side co-bound at bounding forms 2/3 on a call of a user-authored instance method whose return is `owned` (captures tested against the receiver as well as the returned object; #9275 interpretation (a)).* Reopening bar: it rejects the stdlib §6.5 observer pattern wherever the publisher is a parameter, a getter-faced property or an interface face, rejects the hub idiom's short-lived-capture form at form 2, and cannot reach interface dispatch. Reopened only by a caller-visible retention announcement that does not exist today.
2. *Reading the bound of an owned-return instance method as the returned object alone (#9275 interpretation (b)).* Unsound at forms 4/5: a returned forward over a non-`this` receiver (`=> tok.Register(cb)`) or a returned constructed holder (`=> new Button(on)`) dangles when the caller bounds by the receiver. Reopening bar: none while forms 4/5 resolve the receiver at an owned-return call.
3. *A per-parameter "severed by the returned value" summary computed from the callee's body and exported in the artifact interface, keying a caller-side co-bound (the v0.361 triage carve).* The same grounds as the `rejected` row on a callee-body retention summary at the call. The caller's verdict would depend on another declaration's body, it is unavailable at interface dispatch, and it adds nothing the callee-side rule does not already give. Reopening bar: that row's.
4. *The stdlib §6.5 subscription contract as a recorded trust clause on implementations of owned-return `scoped` interface methods.* The obligation is decidable at the implementation, and §28.14 compiler-verifies user-authored callees. Reopening bar: a shape the callee-side rule cannot decide.

And one `deferred` row, record "spec §28.14 *The callee's bound* (v1 bounds), proposal 0215 §4.1": *The returned forward read through a binding returned on every path, or through a conditional over returned forwards; a store into a `scoped` field of an object the callee constructs and returns.* Bar: more analysis, as for the other *liftable* bounds.

## 7. Normative text

Every `old` anchor below occurs once in the v0.361 text at `c85ce271c` (checked with `grep -c`).

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
> 2. an instance method whose return is not `owned`: the receiver, `this` (forms 4/5);
> 3. a static method, whose return is `owned` (above): the returned object (forms 2/3);
> 4. an instance method whose return is `owned`: **both** the receiver and the returned object — a caller's `using var t := k.M(…)` resolves the returned object (form 2) and a caller's `x := k.M(…)` the receiver (forms 4/5), so storage severed by only one of them outlives the other's bound.
>
> A store into a `scoped` field of `this`, and a forward whose bounding object, resolved in the callee's body by the forms above, is `this`, are severed by `this`. A forward whose call is the value the callee returns — the whole expression body, or the whole operand of a `return` — and whose result is `owned` is severed by the returned object, which the forwarded callee's own bound contains (items 1, 3 and 4). So an instance method whose return is `owned` keeps the callback only by a forward that is both — the returned call, on `this` or on a member chain rooted at `this` through `owned` fields — and a static method only by a returned forward:
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
>   private scoped _h: ((double) -> ())?;
>   public method Keep(scoped h: (double) -> ()): owned Token
>     _h := h;                                  // error: severed by `this` alone, but `using var t := k.Keep(…)` bounds the captures by `t`
>     return new Token();
>   end Keep;
>   public method Watch(tok: CancellationToken, scoped cb: () -> ()): owned Subscription => tok.Register(cb);
>                                               // error: severed by the returned `Subscription` alone, but `x := k.Watch(…)` bounds the captures by `k`
>   public static method Watching(tok: CancellationToken, scoped cb: () -> ()): owned Subscription => tok.Register(cb);
>                                               // admitted: a static method's bound is the returned object
> end Keeper;
> ```
>
> The rule binds every user-authored body of such a method — an `override`, and an implementation of an interface method whose return is `owned` (stdlib [§6.5](Bezel_Standard_Library.md#sec-events-ievent-eventsource)'s `IEvent<T>.Subscribe`) included — so a call through an interface face resolves either object against a body that has met both. The returned forward is read only as the returned call itself: a binding of it returned later, a conditional over two such calls, and a store into a `scoped` field of an object the callee constructs and returns are not read through (*liftable*); a `return` inside a nested closure returns that closure's value, not the callee's (*v1 admission bounds* item 6 rejects such a use in any case). A standard-library callee's bound is the one its section states; where that bound omits the receiver, a call whose bounding object resolves to the receiver (forms 4/5) admits capture-free callbacks only.

If the owner declines D2 (Q2), drop item 3's "only by a returned forward" clause and the `Watching` line, and item 3 keeps today's reading. If Q3 goes (i), drop "and a store into a `scoped` field of an object the callee constructs and returns" from the liftable list.

### 7.4 Spec §28.14, *Class- and interface-typed `scoped` parameters*, the callee-side bullet

old:

> and may forward it only to another `scoped` parameter with the same bounding object. It may not return it or store it anywhere else;

▲ new:

> and may forward it only to another `scoped` parameter with the same bounding object — the callee's bound (*The callee's bound* above), where a store or forward is severed by an object when it leaves the storing object disposed no later than that object; a storing object has one owner, so no store is severed by both objects of item 4's bound, and an instance method whose return is `owned` stores such a parameter neither directly nor through a forward (a static factory, `public static method Open(scoped c: Customer): owned Account => new Account(scoped c);`, or a method without an `owned` return, is the spelling that stores). It may not return it or store it anywhere else;

### 7.5 Spec §28.3, the axiom register, row A11

old:

> | A11 | The [§28.14](#sec-scoped-callback-parameters) bounding-object resolution forms (1–5) and the ratified v1 admission bounds |

▲ new:

> | A11 | The [§28.14](#sec-scoped-callback-parameters) bounding-object resolution forms (1–5), the callee's bound, and the ratified v1 admission bounds |

### 7.6 Spec Appendix C.3, one bullet after the `scoped`-declaration bullet

old:

> - `scoped` is declared only on a function-typed parameter or field, or on a class- or interface-typed parameter whose declared type, nullability stripped, is not `Object` ([§22.3](#sec-parameter-modifiers), [§28.14](#sec-scoped-callback-parameters)).

▲ new (the old bullet, then):

> - Every store and forward of a `scoped` parameter is severed by each object of the callee's bound; an instance method whose return is `owned` — an override and an interface implementation included — keeps a `scoped` callback only by a forward that is its returned call on `this` or on a member chain rooted at `this` through `owned` fields, and stores no class- or interface-typed `scoped` parameter ([§28.14](#sec-scoped-callback-parameters) *The callee's bound*; A11).

### 7.7 Stdlib §6.1, `Register`

old:

> the registration stores the callback bounded by the returned `Subscription` (the owned-return bounding form — `using var reg := token.Register(...)`), and disposal severs the registration, which is the quiescence guarantee.

▲ new:

> the registration stores the callback bounded by the returned `Subscription` (the owned-return bounding form — `using var reg := token.Register(...)`), and disposal severs the registration, which is the quiescence guarantee. That `Subscription` is `Register`'s whole bound (spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters), *The callee's bound*): the receiver is a token value whose teardown severs nothing, so a `Register` call whose bounding object resolves to the receiver admits capture-free callbacks only, and a user method that forwards to `Register` is a `static` method returning the call (`=> tok.Register(cb)`).

### 7.8 Stdlib §6.5, *Subscribing*, one sentence

old:

> so a handler capturing `this` (or state reached through it) is admitted *regardless of how the publisher was reached* — a parameter, a field, an interface face.

▲ new:

> so a handler capturing `this` (or state reached through it) is admitted *regardless of how the publisher was reached* — a parameter, a field, an interface face. A user implementation of `IEvent<T>` or `IAsyncEvent<T>` keeps that true: its `Subscribe` is an instance method whose return is `owned`, so spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters) (*The callee's bound*) lets it keep the handler only by returning a forward on a source it owns — `public method Subscribe(scoped handler: (T) -> ()): owned Subscription => _source.Subscribe(handler);` over a `private owned readonly _source: EventSource<T>` — never in a `scoped` field of its own.

### 7.9 Rationale §5.26, *The axioms' reasons*, the A11 entry, one paragraph appended

old (the entry's last sentence):

> so the registration's extension past the bounding object is recorded as the same residual-trust shape as quiescence, with the checked-mode backstop.

▲ new:

> so the registration's extension past the bounding object is recorded as the same residual-trust shape as quiescence, with the checked-mode backstop. The callee's bound (spec §28.14) is the clause that ties the callee side to the forms ([proposal 0215](proposals/0215-owned-return-callee-bound.md), [#9275](https://github.com/s8lap/bezel-lang/issues/9275)). The forms are per call site and a callee has none, so the callee is checked against every object its call sites can name: both the receiver and the returned object at an instance method whose return is `owned`. The repair sits at the callee because each caller-side alternative fails on a recorded ground. A co-bound by the receiver rejects the stdlib §6.5 observer pattern wherever the publisher is a parameter, a getter-faced property or an interface face. A co-bound keyed on a body-derived summary makes a caller's verdict depend on another declaration's body (§5.24, *No caller-side warning keyed on the callee's body*). Reading the bound as the returned object alone leaves a forward over another receiver dangling at forms 4/5. The cost is that a method that keeps a callback on itself and returns an unrelated handle has no spelling; it returns no `owned` value, or the handle it returns is the one that severs.

### 7.10 Guide 1 §9, one sentence

old:

> On the callee side the rules are the mirror image: store only in a `scoped` field of the same object, or forward to a `scoped` parameter on `this`; never return it, bind it to a local, or store it in another object.

▲ new:

> On the callee side the rules are the mirror image: store only in a `scoped` field of the same object, or forward to a `scoped` parameter on `this`; never return it, bind it to a local, or store it in another object. A method that returns an `owned` handle, such as a `Subscribe` of your own, is stricter, because a caller may bound its captures by the handle or by your object: it may only return the forward, `=> _changed.Subscribe(h)`, and never keep `h` in a field of its own (spec §28.14, *The callee's bound*).

The guide's header pin moves to the landing revision.

### 7.11 Code-generation reference

§4 *`scoped`*, old:

> the callee may store the callback in a `scoped` field of the bounding object, forward it to a same-bound `scoped` parameter, or invoke it.

▲ new:

> the callee may store the callback in a `scoped` field of the bounding object, forward it to a same-bound `scoped` parameter, or invoke it. A method with an `owned` return (an `IEvent<T>.Subscribe` implementation included) keeps it only as the returned call on an `owned` field, `=> _changed.Subscribe(h)`, never in a `scoped` field of `this` (§28.14 *The callee's bound*).

§7.2, one entry appended at the end of the section, in the section's form:

> ```bezel
> class Keeper
>   private scoped _h: ((double) -> ())?;
>   public method Keep(scoped h: (double) -> ()): owned Subscription
>     _h := h;                                         // ERROR: an `owned`-return method's callers may bound the captures by the returned handle, which does not sever `_h` (§28.14)
>     return _ticks.Subscribe(lambda(t) => ());
>   end Keep;
> end Keeper;
> ```
>
> > §28.14 *The callee's bound*: a method that returns an `owned` handle keeps a callback only by returning the forward that the handle severs — `=> _changed.Subscribe(h)` over an `owned` source field — or returns no `owned` value and stores in a `scoped` field of `this`. To register on a token or a publisher it does not own, write a `static` method that returns the registering call.

(`_ticks` is left undeclared in the fragment, as the section's other fragments leave context. A landing that makes the fragment self-contained adds `private owned readonly _ticks: EventSource<double> := new();`.) The header pin moves to the landing revision, which `check_docs.sh`'s PIN check requires within three revisions.

### 7.12 Proposals folder

`README.md` gets a *Pending decision* row (this commit). On acceptance, the rows of §6 go into `DECLINED.md`.

## 8. Changelog sketch

> **v0.NNN — The callee's bound (§28.14; proposal 0215; #9275).** Design change; axiom extension (A11). Decided: §28.14's callee-side obligation is checked against the callee's bound, every object its call sites can resolve — the constructed object for a constructor, the receiver for an instance method without an `owned` return, the returned object for a static method, and both the receiver and the returned object for an instance method whose return is `owned`. Every store and forward must be severed by each. At an owned-return instance method that leaves one shape, the returned forward on `this` or on an `owned` chain from it (the hub idiom `=> _changed.Subscribe(h)`). A store into a `scoped` (or, class-typed, a plain) field of `this` there is rejected, and so is a returned forward over another receiver. A static method keeps a callback by a returned forward. The rule binds overrides and interface implementations, so stdlib §6.5's observer pattern holds through a user `IEvent<T>`. Stdlib §6.1 states that `Register`'s bound is its `Subscription` alone. Declined (proposal 0215 §3): a caller co-bound by the receiver, a returned-object-only bound, a body-derived severed-by-return summary in the artifact interface, and a trust clause on implementations. Recorded *liftable*: a returned binding, a conditional of returned forwards, and a store into a constructed, returned local.
>
> **Migration.** A method declared `…(scoped h: F): owned H` that stored `h` in a field of `this` is now rejected. Either return no `owned` value (callers then bound by the receiver) or return the forward whose handle severs the registration. A method that returned a forward over a token or a publisher it does not own (`=> tok.Register(cb)`), or a constructed holder (`=> new Button(on)`), becomes `static`. A class- or interface-typed `scoped` parameter of such a method is no longer stored; use a static factory. No caller's verdict changes. The tree held no rejected shape at the revision (proposal 0215 §5.1). Flipped: gtest `expr_body_scoped_param_9201` `static_expression_body_forwards_scoped_param_off_object_rejected_spec_28_14` (reject → accept, rewritten as the static shape; an instance-method reject twin added); corpus `s28_14_0017` (per Q3).

## 9. Proposed rule-corpus cases

IDs are assigned in the landing revision's band. Each fixture declares `Counter2` (a class whose `Hit` increments and prints a count), as the probes do. Each `run` case loops its admitted shape 200 times and prints a total, so a dangling read cannot pass by luck (the one-shot trap, CLAUDE.md).

| case | expect | rule sentence (▲ §7.3 unless noted) | program |
|---|---|---|---|
| A | `reject` | "an instance method whose return is `owned`: both the receiver and the returned object" | the issue's `Keeper` repro (`p1`) |
| B | `run` | item 2 (the receiver bound) | `Keep` with no `owned` return, caller bound by `k` (`p7`); `output:` the count |
| C | `run` | "a forward that is both — the returned call, on `this` …" | the hub idiom with captures shorter-lived than the hub, form 2 in a loop (`p4`) |
| D | `run` | the same, at forms 4/5 | `keep := hub.OnChanged(…)` with `c` declared before `hub`; `keep` disposed after the block (`p8`) |
| E | `run` | stdlib §6.5 + ▲ §7.8 | a pattern-shaped user `IEvent<double>` implementation and a TickObserver over a parameter-typed face |
| F | `reject` | ▲ §7.8 "never in a `scoped` field of its own" | the `IEvent<double>` implementation that stores in `this` (`p3`) |
| G | `reject` | "severed by the returned `Subscription` alone" | instance `OnCancel(…) => tok.Register(cb)` (`p2`) |
| H | `run` | item 3 + the returned forward (D2) | static `OnCancel` (`p6`), registered and cancelled in a loop |
| I | `reject` | item 4 | instance `Make(scoped on): owned Button => new Button(on)` (`p9`) |
| J | `run` | item 3 + the returned forward (D2) | `p11`'s static `Open(scoped c) => new Account(scoped c)` |
| K | `reject` | ▲ §7.4 | `Hold(scoped src: Item): owned Token2` storing `_src := src` (`p5`) |
| L | `reject` | "a binding of it returned later … not read through (*liftable*)" | `var s := _changed.Subscribe(h); return s;` |
| M | `reject` | ▲ §7.7 | a `Register` call whose bounding object is a token-typed receiver at form 5, capture-bearing |

## 10. Owner questions

1. **Q1, the rule.** Adopt D, the callee's bound with "both" at owned-return instance methods, as the answer to #9275? Recommended. It is the only option that closes `p1`, `p3`, `p5`, `p2` and `p9` without changing a caller's verdict or adding an interface fact.
2. **Q2, the static half (D2).** Land the returned-forward severance at static methods together with Q1? Recommended. It is the same fact the instance rule needs, it gives the rejected instance shapes their remedy, and it settles the reading on which the 9201 gtest and the `p11`/`s28_14_0017` readers disagree. Cost: one gtest row flips from reject to accept.
3. **Q3, `s28_14_0017`.** A static `Create` that constructs `r`, stores `r._cb := cb` and returns `r`. (i) Admit it as a v1 shape: needs flow facts, namely that `r` is returned on every path and not otherwise transferred. (ii) Reject it in v1 as *liftable*, flipping the case from `run` to `reject`, with the forward to a `scoped` constructor parameter as the remedy. Recommended: (ii). The compiler already rejects it (baseline `fail divergence`), so no program changes verdict, and the remedy is one line.
4. **Q4, the class-typed half.** Land §7.4 in the same revision? Recommended. The scoped-construction bullet uses the same term, and `p5` is the same hole. Declining leaves it open with a stated gap.
5. **Q5, the ramp.** Go straight to an error, with no warn-first stage? Recommended. The rejected shapes are use-after-frees, and the measured tree holds none of them (§5.1). A ramp would be a Ratchet with nothing to ramp.
6. **Q6, `Register`.** Confirm §7.7's sentence as a clarification of a compiler-known callee. It adds a rejection only if some form 4/5 resolution of a token receiver reaches an admitted object today, which §4.4 believes none does (HYPOTHESIS).

## 11. Files and probes

Probes are in `/var/tmp/bezel-scratch/v361/p-0215/probes/`, compiled with `/var/tmp/bezel-scratch/r42/x/bezelc-0.0.0-8ccf7ff0-linux-x86_64/bin/bezelc` (`8ccf7ff06`), each in the default mode and with `--checked-disposal` (`--sanitize=address` could not link on this host: the ASan runtime archives are absent). Run logs sit beside the sources (`*.default.run.txt`, `*.checked.run.txt`).

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

Scans: `/var/tmp/bezel-scratch/v361/p-0215/scan2.py` (output `scan2_out.tsv`), run from the worktree root. The v0.361 documents used for the anchors were extracted from `c85ce271c` and `6c125097d`, which differ only in the version headers.
