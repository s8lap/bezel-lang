# Proposal 0099 — `ScopeGuard`: the errdefer idiom as a library type, and the recorded decline of language-level `defer`/`errdefer`

| | |
|---|---|
| **Status** | accepted — ruled under the owner's 2026-08-18 delegation; awaiting graduation train. Charter ruling C5: **adopt** a stdlib `ScopeGuard` (constructibility verified under §28.14 bounding-object form 1; `Dismiss()` for the errdefer idiom; drafted against the stdlib's naming, Try, and facts disciplines), **adopt** a Guide 1 teaching section, and **record the decline** of language-level `defer`/`errdefer` with the charter's grounds. |
| **Tracking issue** | coordinator files |
| **Target module(s)** | **stdlib** — new **§2.21 `ScopeGuard`** (appended to `System`, after §2.20 `Result<T, E>`), plus one line in §2.10's `IDisposable` neighbourhood cross-referencing it. **spec** — §2.3's always-available-names list if `System`'s auto-import roster is enumerated there (verification item, §6). **rationale** — a new decline record for language-level `defer`/`errdefer` in §17 *Things explicitly rejected*. **guides** — Guide 01 (Memory and Ownership): one teaching section, *Cleanup that must not run on the success path*. |
| **Written against** | spec/stdlib **v0.255** (`git show 5fa5a95b:docs/*.md`) |
| **Currency** | **v0.256** (HEAD `8f41e99d` — the defect train `4130d2de` plus its gate fixes `e8f5387a`): every quoted §28.14/§28.10/§28.4-cleanup/§21.6/stdlib text is unchanged. Adjacent, no conflict — verified: v0.256 added §22.9's destructor-body-never-on-construction-unwind rule and §28.4's **one-resource-per-owning-wrapper** paragraph. Both govern classes releasing a **raw resource in a destructor**; `ScopeGuard` is not that shape — it is an ordinary class whose `scoped`-stored *action* runs at disposal, it holds no raw resource, and inside a constructor body a `using var` guard participates in §22.9's unwinding exactly as any `using var` local does (its disposal — hence its undo — runs on the unwind edges). The new teaching and §5's decline record compose without touching each other. |
| **Origin** | Charter C5. The question the reviews raised was whether Bezel should have a `defer`/`errdefer` statement. The ruling: the *failure-path* half is a real gap, the *value* half is not, and the gap is a library type — not a keyword. |

*Reference convention: unprefixed §-references are language-spec references; stdlib references are marked `stdlib §x`. Every quotation is from the v0.255 snapshot, re-verified unchanged at v0.256. Bold inside a quotation is the source's own unless marked* (emphasis added).

---

## 1. What Bezel already has, and the one thing it does not

**Scope-end cleanup is already automatic and is not the gap.** An owned local's obligation is discharged at scope end (§28.3); `using var` pins a resource to its scope (§28.10); an owning collection's `Dispose()` cascades; a class's owned fields are torn down in reverse declaration order (§28.4); `do … end` blocks (§21.6) exist precisely to bound a local's lifetime tighter than its method's. A `defer` statement over *values* would be redundant machinery over an affine model that already runs cleanup at the right point without being told.

**Failure-only cleanup has no spelling.** The pattern is: perform a side effect (register something, mutate shared state, hand a handle to a foreign library, begin a transaction), then on **success** keep it and on **failure** undo it. `finally` runs on both paths, so the undo needs a flag:

```bezel
var committed := false;
try
  Register(id);
  DoWork();
  committed := true;
finally
  if not committed then Unregister(id); end;
end;
```

That is correct, it composes, and it is the sanctioned spelling today. It is also four extra lines and one mutable boolean at every site, and the boolean's `:= true` has to sit at exactly the right statement — a class of bug that is invisible on the success path.

**`ScopeGuard` is that flag, packaged.** It converts the four lines into one declaration plus one `Dismiss()`, and it makes the "right statement" a call the reader can see.

## 2. The type

```bezel
public class ScopeGuard
  public constructor(scoped action: () -> ());     // runs at disposal unless dismissed
  public method Dismiss();                          // disarms; idempotent
  public readonly property IsDismissed: bool;
  destructor;                                       // invokes action unless dismissed
end ScopeGuard;
```

**Facts** (stdlib §1's statics-and-sharing discipline): `ScopeGuard` declares **no facts** — it is domain-local, mutable, and holds a caller-supplied callback whose captures are bounded to the guard's own scope, so it belongs to none of the three derived sets. *(The line is written explicitly rather than omitted, following §6.9's `Channel<T>` precedent of recording a deliberate absence with its ground; `scripts/check_docs.sh` check 3b reads the fact-line roster.)*

**Every element of the declaration, verified against the rules it depends on:**

- **`scoped` on the constructor parameter is required, not stylistic.** The guard *stores* the callback and invokes it after the constructing call has returned. §28.14's call-bounded default forbids that outright: a function-typed parameter without `scoped` *"may **not** be stored in a field of any object."* `scoped` is the storage-bounded mode, and its callee-side right is exactly what the guard needs: *"The callee may store the callback, but only in `scoped` fields **of the bounding object itself**."* The guard's field is a `scoped` field of the guard.
- **The bounding object resolves by form 1.** §28.14's resolution list, tested in order: *"1. a constructor call → the constructed object."* `new ScopeGuard(…)` is a constructor call, so the bounding object is the guard.
- **Capture-bearing callbacks are admitted, under a stated condition.** §28.14 admits a capture-bearing closure *"iff the bounding object resolves to … a `using var`/`await using var` local `L` (directly, **through the initializer resolution of forms 1/2**, …), where every captured **local** source is declared **before** `L` in the same or an enclosing scope."* So `using var g := new ScopeGuard(lambda() => Unregister(id));` is admitted whenever every source the lambda captures was declared before `g` — which is the natural order anyway: you acquire the thing, then arm the guard that undoes the acquisition. A guard bound with plain `var` rather than `using var` resolves no admitted bound and therefore accepts **capture-free** callbacks only; the type's documentation must say `using var` is the spelling.
- **Quiescence is satisfied by construction.** §28.14 requires that *"Disposal of the bounding object must guarantee that no stored callback is invoked after disposal completes"*, and adds *"A callback that runs **during** the drain may touch the bounding object soundly: logical disposal precedes physical reclamation (§28.12)."* The guard invokes its callback **in its own destructor** — during disposal, never after — so the obligation is discharged by the type's definition rather than trusted to the author. For a compiler-known type the guarantee is *"stated normatively in the standard library"*, which is what §2's declaration does.
- **`Dismiss()` is idempotent**, matching the language-wide idempotent-disposal rule's spirit and `Subscription`'s precedent (stdlib §2.11: *"`Dispose()` is idempotent … the first call removes the registration; subsequent calls are no-ops"*).
- **`IsDismissed` is a `readonly property`, not `TryDismiss`.** stdlib §1's Try convention sanctions exactly four shapes, all of them about *extraction* or *attempt*; dismissal neither extracts a payload nor can fail, so no Try shape applies. The property exists for assertions and tests; it is not needed by the idiom.
- **Naming.** `Dismiss` over `Cancel` (which in this stdlib means cooperative cancellation, §6.1) and over `Disarm`/`Release` (both overloaded elsewhere — `Semaphore.Release`, and *release* is the ownership vocabulary). `Dismiss` appears nowhere else in the document. Verified against the §5(b)-style block-by-name concern of proposal 0098: `ScopeGuard` declares no member colliding with a LINQ operator name, and nothing extends it.
- **Home: `System` (stdlib §2), auto-imported.** The type is general-purpose cleanup vocabulary in the same family as `IDisposable` (§2.10) and `Subscription` (§2.11); `Subscription`'s own placement note is the precedent (*"`Subscription` lives in `System` (not `System.Threading`) and is auto-imported … placing it beside those APIs — rather than behind an explicit import — matches the type's actual usage"*). Numbered **§2.21**, appended after §2.20, so no existing section renumbers.

**No constructor overload taking a "dismissed" initial state, and no `owned`-taking form.** The first is a flag the caller can spell (`g.Dismiss()` on the next line); the second would make the guard own a resource, which is what `using var` on the resource itself already does — the guard's job is the *action*, not the value.

## 3. The idiom, and the one seam it has

**The shape the type is for:**

```bezel
public method Register(id: int)
  _registry.Add(id);
  using var g := new ScopeGuard(lambda() => _registry.Remove(id));
  Validate(id);            // may throw
  Publish(id);             // may throw
  g.Dismiss();             // success: keep the registration
end Register;
```

On any throwing edge the guard's disposal runs the undo (§28.10's `using var` compiles to the try/finally equivalent, and §28.4's cleanup ordering places it on the single establishment-ordered stack). On the success path `Dismiss()` disarms it and the disposal is a no-op.

**The seam: a captured source is frozen for the guard's scope.** §28.14 is explicit — *"Until `L`'s scope exits, the captured local sources are **frozen** (derives: K5.7): they may not be moved from, reassigned, or disposed — the consume-site freeze **plus explicit disposal**. The storage freeze is deliberately *stronger* than the consume-site rule it extends."* So the classic build-guard-commit-**transfer-out** shape does not compile as written:

```bezel
public method Open(): owned Session
  var conn := Connect();
  using var g := new ScopeGuard(lambda() => conn.Rollback());
  Configure(conn);
  g.Dismiss();
  return move conn;        // REJECTED: conn is frozen while g is live (§28.14)
end Open;
```

This is not a defect the proposal introduces — it is §28.14's admission bargain, and it is *why* the capture is sound: a stored callback that could observe a moved-from or disposed source is exactly what the freeze exists to prevent. But it is the first thing a user will hit, so the type's documentation and the guide must lead with the remedy. **The remedy is `do … end` (§21.6)**, which exists for precisely this — bounding a local's lifetime tighter than the method's:

```bezel
public method Open(): owned Session
  var conn := Connect();
  do
    using var g := new ScopeGuard(lambda() => conn.Rollback());
    Configure(conn);
    g.Dismiss();
  end;                     // g's scope ends; the freeze on conn lifts
  return move conn;
end Open;
```

Two further remedies, both already idiomatic: express the action as a **method reference on `this`** (`new ScopeGuard(this.RollbackPending)`), whose capture-set contribution is the receiver — §28.14's forms-3/4 `this` admission — leaving locals unfrozen; or keep the resource in a **field** rather than a local, where the freeze does not reach.

## 4. Guide 01 teaching section (sketch)

Placed after the guide's existing scope-end-disposal material, titled **"Cleanup that must not run on the success path."** Contents, in the guide's example-first register:

1. **The distinction, in two sentences.** Scope-end cleanup is automatic and needs no ceremony — that is what owned locals and `using var` are. What has no automatic form is *undoing a side effect only if the rest of the method fails*, because a side effect is not a value and has no owner.
2. **The flag version first**, in full (the `committed` boolean above), named as correct and as what everyone writes.
3. **The same method with `ScopeGuard`**, side by side, with the `Dismiss()` line called out as the commit point.
4. **The freeze, taught before it bites** — the `Open(): owned Session` example above in both forms, rejected then repaired with `do … end`, with one sentence on why the freeze is the price of a stored callback that is guaranteed never to observe a dead source.
5. **When not to reach for it**: if the thing to undo is a *value*, it has an owner and the owner's disposal is the answer; `ScopeGuard` is for effects — registrations, foreign handles, transactions, counters.
6. **One sentence pointing at the decline** (§5), so a reader arriving from a `defer` language finds out why there is no keyword.

## 5. The recorded decline of language-level `defer`/`errdefer`

Landing in rationale §17 (*Things explicitly rejected*), in that section's register — a short entry with its grounds, and a reopening condition.

> **`defer` / `errdefer` statements.** A statement registering an action to run at scope exit (`defer`), or only on the failure edge (`errdefer` / `scope(failure)`). Declined on three grounds, one per half of the design.
>
> **(1) The value half is redundant.** Bezel is affine: every owned value's disposal is already scheduled by the binding form that introduced it ([§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows), [§28.10](Bezel_Language_Spec.md#sec-using-var)), and the tighter scope a `defer` would emulate is `do … end` ([§21.6](Bezel_Language_Spec.md#sec-do-end-blocks)). Languages that need `defer` for cleanup need it because their cleanup is not attached to values; ours is. Adding `defer` would give every resource two spellings for one obligation — the both-ways surface the marker discipline is built to avoid.
>
> **(2) The ordering half is already provided, on one stack.** `finally` composes with `using var` and with owned-local discharge on a **single establishment-ordered cleanup stack**: a `finally` is *"established at entry to the protected block … on the single establishment-ordered cleanup stack it ranks before anything the block itself establishes"* ([§31.2](Bezel_Language_Spec.md#sec-try-catch-finally)). A `defer` would be a second registration mechanism ranking on the same stack — new syntax for an existing position.
>
> **(3) The failure half needs a primitive the language deliberately lacks.** `errdefer` / `scope(failure)` must ask, at scope exit, *"are we unwinding?"* — D's `scope(failure)` is defined in terms of it and C++'s `std::uncaught_exceptions()` is its canonical spelling. Bezel exposes no such predicate, and adding one would make a program's meaning depend on the ambient unwind state at a point where nothing names an exception — the opposite of the exception model's design, in which the only ways to observe an in-flight exception are `catch` (which names its type) and `finally` (which does not, and therefore cannot branch on it). The **commit-flag guard is the sanctioned spelling**, and the standard library packages it as `ScopeGuard` (stdlib §2.21): the flag is a field, the "are we unwinding?" question is replaced by "was `Dismiss()` reached?", and the answer is a fact about the program's own control flow rather than about the runtime's.
>
> **Reopening condition:** field evidence from external users that the `ScopeGuard` spelling is systematically forgotten or mis-placed in a way a statement form would prevent — together with a proposal for the uncaught-exception predicate that names its interaction with `finally`, nested unwinds, and async continuations. Absent that predicate the failure half is not designable, whatever its ergonomics.

## 6. Verification items for the graduation train

Every one of these was checked while drafting and is listed so the train re-checks it rather than trusting this document.

1. **`scoped` on a *constructor* parameter — ANSWERED, with a mandatory spelling.** The position splits by constructor kind, and both halves are already normative: `scoped` is **rejected on a primary-constructor parameter** — §22.10: *"A `scoped` primary-constructor parameter is rejected in this revision (the header admits plain and `owned` parameters only …): the §28.14 scoped-storage contract needs an explicitly declared field — use a regular constructor. (Recorded v1 bound; liftable.)"* — and **admitted on an explicit constructor**, where §28.14's own text spells it (*"a class- or interface-typed parameter may be declared `scoped` — `constructor(scoped stream: Stream)`"*) and form 1 resolves the bounding object to the constructed instance. §2's declaration is therefore correct as written **and its spelling is mandatory**: the graduation train must **not** respell `ScopeGuard` with a primary constructor — the explicit-constructor form plus the explicitly declared `scoped` field is the only admitted shape until §22.10's recorded bound is lifted.
2. **`() -> ()` is the unit-returning function type** as spelled in §27.1, and a lambda with a unit body infers it (§27's return-type ascription rules — *"A unit ascription (`: ()`) declares a procedure lambda"*).
3. **The auto-import roster.** `Subscription`'s section says the auto-import list lives at spec §2.3; if that list is enumerated normatively, `ScopeGuard` joins it, and `scripts/check_docs.sh` may cross-check the roster.
4. **The `IsDismissed` property's purity.** If the stdlib marks such accessors `pure` (§22.15), decide and state it; the getter reads one `bool` field and qualifies, but the document should not acquire an unmarked outlier beside marked siblings.
5. **Destructor semantics on the throwing-action edge.** If the guard's action itself throws during an exception-path disposal, §28.4's multi-value cleanup rule governs (*"a throwing disposer does not skip the remaining values … the later exception **supersedes** the earlier one"*), and proposal 0097's snapshot records the superseded one. State the composition in one clause so a reader does not have to derive it; it is the guard's most likely surprise.
6. **`AsyncScopeGuard` is deliberately not proposed.** An `await`-ing cleanup would need `IAsyncDisposable` and `await using` (§28.12, stdlib §2.10) and its action would be `() -> Task`, whose invocation from a synchronous disposal path §28.14's call-bounded rule rejects. Recorded as out of scope, not overlooked; the shape is available if an async cleanup case arrives.

## 7. Migration and compatibility (changelog house categories)

- **Newly defined:** stdlib §2.21 `ScopeGuard`, its four members and its no-facts line.
- **Newly rejected:** nothing.
- **Doc-only:** the Guide 01 section and the rationale §17 decline record.
- **Unchanged:** every language rule. `ScopeGuard` is an ordinary class using `scoped`, `destructor`, and `using var` exactly as declared — **no compiler support is required**, which is the proposal's strongest property and should be stated at the top of its stdlib section.

## 8. Implementation sketch

- **Nothing compiler-side.** The type is writable in Bezel today: one `scoped` field, one `bool`, one destructor. The graduation train may implement it in the runtime or in stdlib Bezel source (proposal 0005's axis).
- **Spec-cited regression tests** (`runAot`): the throwing path runs the action; the dismissed path does not; `Dismiss()` twice is a no-op; a capture-bearing lambda under `using var` with sources declared **before** the guard is **accepted**, and the same lambda with a source declared **after** the guard is **rejected** (§28.14's ordering condition); the same lambda under plain `var` is **rejected** (no admitted bound, capture-bearing); a capture-free action under plain `var` is **accepted**; the freeze pin — `move` of a captured source while the guard is live is **rejected**, and the `do … end` repair is **accepted**; the destructor-throws case composing with §28.4 (item 5 above); and an ASan run (`--full`) over a guard whose action disposes an owned local, proving exactly-once.

## 9. Interactions

- **§28.14 / axiom A11** — the whole admission story. This proposal spends none of A11's liftables and relies on form 1 plus the `using var` admission exactly as ratified; it is a *client* of the storage-bounded design, and arguably its first ordinary-user-facing one.
- **stdlib §2.11 `Subscription`** — the nearest sibling (an owned handle whose disposal undoes a registration) and the placement/idempotence precedent. The difference worth one sentence in §2.21: a `Subscription` is *handed to* you by the registrar and always undoes; a `ScopeGuard` is *built by* you and undoes only on failure.
- **Proposal 0097 (this train)** — §6 item 5: a throwing guard action on an exception edge is a **§28.4 supersession the chain records** — the guard's disposal sits in the multi-value cleanup pass, its exception supersedes the in-flight one, and 0097's `SupersededChain` is what keeps the original failure readable. The two proposals cite each other (0097 §8 carries the reciprocal bullet).
- **Guide 02 (Errors and Nullability)** — teaches `finally`; the new Guide 01 section should cross-reference it rather than re-teach it.
- **Rationale §18** — no entry moves; `defer` was never on the deferred list, and after this proposal it is on the rejected one.
