# Proposal 0144 — `Object` and every interface join §30.7's conservative `yield return` key

| | |
|---|---|
| **Status** | graduated-v0.328 — superseded — graduated v0.328 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option (b); filed by lane L2's critique round 1 (B1) and revised by round 2, whose 2 blocking and 4 non-blocking findings are resolved; landed with the v0.328 changelog entry; at landing its §30.7 class clause and Appendix C.6 row read *without a synchronous fallback*, the same train's per-level ruling, which also decides §5's question (ii); §1 *Scope*'s question is decided by proposal 0145, under which §1's `TaskScope` held at `Object` is rejected at its initializer as well) |
| **Tracking issue** | none filed before this proposal: the question is the v0.328 lane L2's *New questions surfaced* under item (3) (`scratch/v0328/L2.md`), sharpened by its critic (`scratch/v0328/L2.critic.md`, finding B1); one `spec-tracking` compiler tracker is filed at landing (§5) |
| **Target module(s)** | **spec** — §30.7 (the `await using var` across-`yield return` rule: its interface parenthetical, and one sentence after "the defined degraded behavior"), §28.12 (the v0.328 *The `DisposeAsync` awaited is the run-time object's* paragraph's last sentences, landed by the same lane), Appendix C.6 (the §30.7 row); **`DECLINED.md`** — two `declined-at-proposal` rows |
| **Written against** | spec **v0.327** (HEAD `478a6de`; drafted at the WIP `3ebd81e`, whose `docs/` and `src/` content `478a6de` carries unchanged for every passage cited here) plus lane L2's §28.12 ruling (v0.328). Section numbers are v0.327's. |
| **Origin** | Lane L2 ruled that `await using` awaits the run-time object's `DisposeAsync` whatever the binding's static type. Under the static reading v0.327 left open, a `TaskScope` behind an `Object`- or `IDisposable`-typed `await using var` skipped its drain on **every** path; under the ruling it awaits the drain on the normal and exception paths and skips it only on abandonment at a `yield return`, the one face §30.7 exists to close and does not, as written, reach. |

*Reference convention: unprefixed §-references are language-spec references at v0.327 as composed. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares.*

---

## 1. Problem

§30.7 rejects an `await using var` live across a `yield return` in an async iterator when consumer abandonment would silently void cleanup the type states normatively. Its key is the binding's static type: "An `await using var` whose static type is such a class [one that implements `IAsyncDisposable` and declares no destructor] (or is of **interface** static type — `IAsyncDisposable` or any interface deriving from it: an interface cannot promise a destructor, so the runtime type's fallback posture is statically unknowable and interface-typed bindings are conservatively included; a recorded liftable bound) therefore **may not be live across a `yield return`**".

Under the v0.328 §28.12 ruling the path is the object's. So a `TaskScope` (stdlib §6.10: no destructor by design, "no synchronous escape is sound") held at a static type that is neither `TaskScope` nor an `IAsyncDisposable`-derived interface awaits its drain on the normal path and, on abandonment, takes the synchronous fallback, which skips the drain. Its children keep running with borrows of a reclaimed frame (§28.14's quiescence). Two such static types exist:

- **`Object`**: every class reaches it.
- **`IDisposable`**: every class implicitly implements it (§28.2; §14.2's conformance rule 1).

```bezel
public async method Pages(): IAsyncEnumerable<int>
  await using var s: Object := new TaskScope();     // admitted by §30.7 as written
  Fan.Start(s);                                       // user helper: `if o is TaskScope ts then ts.Spawn(…); end;`
  yield return 1;                                     // consumer abandons here: the drain is skipped
  await Fan.Finish(s);                                // user helper: `if o is TaskScope ts then await ts.Join(); end;`
end Pages;
```

(`Fan.Start` and `Fan.Finish` are the program's own helpers taking a plain `Object` parameter; the `is` test is §12.7's. Passing `s` as a bare argument is a delegation, so the stdlib §6.10 missing-`Join` check — Appendix C's provable subset — does not fire. Without a delegation or an `await s.Join()`, which `Object` cannot spell, that check rejects the method on its own ground, so the hazard needs a program shaped like this one.)

Stdlib §6.10 already says it object-centrally: "inside an async iterator, a `TaskScope` may not be held across a `yield return`". §30.7 is the rule that checks this, and with the path now run-time it misses these two spellings. `TaskScope` admits no subclass (stdlib §1 *Extensibility*), so no other static type can hide one: only its own type, its interfaces, and `Object`.

**Scope.** This is one face of a wider pre-existing gap that this proposal does not close: stdlib §6.10's "consumption is `await using` only" rule is checkable only while the static type is `TaskScope`. A `TaskScope` converted to `Object` or an interface can reach a plain `using var`, a container cascade or an auto-drop, each of which takes §28.12's implicit synchronous path and skips the drain, with no `yield return` involved (for instance a method returning `owned Object` that returns `new TaskScope()`, bound by `using var o := Make();`). Whether a `TaskScope` converts to a supertype at all is a separate question, filed at landing (§5); a rule that forbids the conversion would close every face at once, this one included. This proposal decides only §30.7's key, which the v0.328 ruling makes the one place the run-time path, rather than the static type, now determines whether the drain runs.

## 2. Options

- **(a) Status quo.** Leave the key as written. The hole in §1 stays open, and it is a soundness loss, not a quality loss.
- **(b) Add `Object` and every interface to the conservative inclusion.** An `await using var` whose static type is `Object` or any interface may not be live across a `yield return`. The interface clause already rests on the ground "an interface cannot promise a destructor", which is true of every interface and of `Object`. Every other class static type keeps today's verdict.
- **(c) Add every non-`sealed` class static type as well.** Any binding whose static type admits a subclass that might implement `IAsyncDisposable` without a destructor would be rejected.
- **(d) Track no-destructor `IAsyncDisposable` objects through upcasts.** This would be a flow analysis over conversions to a supertype. It is a real design with no other client.
- **(e) Key the path on the static type at yield-crossing bindings only.** An `await using var` live across a `yield return` would take the synchronous path whenever its static type declares no `DisposeAsync`, and the run-time path elsewhere.

## 3. Evaluation

- **Soundness.** (b) closes §1's `yield return` face for `TaskScope`, because `TaskScope` is closed to subclassing; the wider conversion gap of §1 *Scope* is untouched by every option here. The remaining run-time-path case is an open **class** base (one that is not `Object`) holding a program's own subclass that implements `IAsyncDisposable` with no destructor. It is the degraded path §28.12 already accepts for that subclass: the object's synchronous disposal runs (its base levels' destructors, then physical reclamation), its asynchronous cleanup is skipped, and §28.12's no-destructor warning has already named that at the subclass's declaration. The same skip happens on every implicit synchronous discharge of such an object (a container cascade, an auto-drop), held at its own type or not, so the open base adds no path the program could not already take. Where the subclass is a §28.14 bounding object whose quiescence rests on its `DisposeAsync`, that quiescence is the author's recorded residual (§28.14 *Quiescence*: "the temporal guarantee is the author's"), on this path as on those. (c) would buy nothing further for soundness.
- **(e).** Not sound and not simpler. A `TaskScope` behind `Object` live across a `yield return` would skip its drain on the normal path as well as on abandonment, which is the static reading's full hole restored at exactly the bindings §30.7 polices. The path would also depend on whether a `yield return` falls inside the binding's live range, which a refactor that moves a yield would change silently. It is the static keying the v0.328 `rejected` row declines, restricted to one context.
- **Simplicity.** Under (b) the key is "a no-destructor `IAsyncDisposable` class, any interface, or `Object`". That is one ground ("cannot promise a destructor") applied uniformly, where the current text applies it to a subset of interfaces.
- **Over-rejection (measured).**
  - `tests/`: no async-iterator body holds an `Object`- or non-`IAsyncDisposable`-interface-typed `await using var` live across a `yield return`. The interface-typed heads in `await_using_iterator_format_disposal_3437_test.cpp:501,527` and `vacuous_using_var_iterator_2357_test.cpp:193` are in consumers, not in the iterator holding them.
  - `docs/`: no typed `await using var` head.
  - Rule corpus: `s30_7_0006` rejects a `TaskScope` and is unaffected.
  - Under (c), every typed or inferred `await using var` over an ordinary non-`sealed` class in an async iterator would be rejected (`await using var x := new Plain();` included), to guard a warned, program-local degraded path. That is the over-rejection lane L2 measured as the reason to keep the bound.
- **Consistency with the compiler.** `src/ownership/Ownership_Checks.cpp:20025` `awaitUsingYieldSensitive` returns `false` for every type ref that is not `Kind::Named`, and otherwise rejects every named static type except a class whose base chain declares a destructor (`:19990`). Every interface is a named type ref, so every interface is already rejected. `Object` is **not**: `sema/TypeResolution.cpp:127` resolves it to `PrimitiveType::Object`, a `Kind::Primitive` ref (`sema/TypeRef.hpp:30,36`), so the check admits an `Object`-typed `await using var` across a `yield return`. Under (b) that shape is spec-ahead-of-impl. This is source-read at `478a6de`, not run (no `build/`). The compiler is *stricter* than (b) for a class with no destructor that does not implement `IAsyncDisposable`, and for a compiler-known head with no class declaration; it is *more permissive* than §30.7's "declares no destructor" where a class implementing `IAsyncDisposable` only inherits one, because its walk counts base destructors. Those divergences predate this proposal and are reported in §5.
- **Migration reach.** At spec level, (b) newly rejects an `Object`- or non-`IAsyncDisposable`-interface-typed `await using var` live across a `yield return` in an async iterator. There are zero sites in tests, docs and corpus. No compiler verdict moves for the interface shapes; the `Object` shape is newly rejected by the spec and still admitted by the compiler (spec-ahead-of-impl, §5).

## 4. Decision

**(b).** Declined, recorded in `DECLINED.md` as `declined-at-proposal`:

- **(a)**, because it leaves the `TaskScope` quiescence hole open. Reopening bar: a check of form (d) that lands, or a rule that keeps a `TaskScope` from converting to `Object` or an interface (§1 *Scope*).
- **(c)**, because it rejects ordinary class bindings for a case that is a program's own warned degraded path. Reopening bar: a library class with no destructor that is opened for subclassing, or a program whose subclass's skipped asynchronous cleanup is a soundness loss rather than a skipped cleanup.

Declined without a row of its own:

- **(e)**: it is the static keying that the v0.328 `rejected` row for §28.12 (*Selecting `await using`'s logical-disposal path by the binding's static type*) declines, applied to one context, and that row's reopening bar governs it.

(d) is not declined. It is the liftable bound's lift, beside the async unwind driver §30.7 already names.

## 5. Normative edits (in `scratch/v0328/L2.json`, ids `L2-3007-1`, `L2-3007-2`, `L2-3007-3`, `L2-3007-4`, and `L2-5592-1`'s last sentences)

- **§30.7.** The parenthetical becomes ▲ "(or is of **interface** static type — any interface, `IDisposable`, `IAsyncDisposable` and the interfaces deriving from it among them — or is `Object`: neither an interface nor `Object` can promise a destructor, and the disposal path is the run-time object's ([§28.12](#sec-asynchronous-disposal)), so the runtime type's fallback posture is statically unknowable and such bindings are conservatively included; a recorded liftable bound)".
- **§30.7.** After "For types **with** a destructor, the synchronous-fallback path above remains the defined degraded behavior, unchanged." add ▲ the open-base sentence: a binding whose static type is any other class is admitted, and the case of a subclass that implements `IAsyncDisposable` without a destructor is the degraded path §28.12's warning names at the subclass. The library's one such class, `TaskScope`, admits no subclass.
- **Appendix C.6's §30.7 row.** "(or of interface static type)" → ▲ "(or of interface or `Object` static type)".
- **§28.12, v0.328 paragraph.** Its closing sentences state that §30.7 reads the static type for what is known before run time, and list the three rejected static-type shapes.

**Compiler.** The interface shapes flip no verdict (§3). The `Object` shape is spec-ahead-of-impl: the check skips every non-`Named` type ref, and `Object` is a primitive one. File one `spec-tracking` tracker at landing, "§30.7 across-yield check: `Object` head admitted, and static types the rule admits rejected", carrying three items, each confirmed by a probe before filing:

1. `await using var s: Object := …;` live across a `yield return` in an async iterator is not rejected (`Ownership_Checks.cpp:20025–20028`; `TypeResolution.cpp:127`). This is the new verdict 0144 lands.
2. Pre-existing over-rejection: a class static type with no destructor that does not implement `IAsyncDisposable`, and a compiler-known head with no class declaration, are rejected (`:19990–20028`). Both are admitted by §30.7 as written and by this proposal.
3. Pre-existing divergence, cross-referenced to the spec question below: the destructor walk counts a base's destructor, so a class that implements `IAsyncDisposable` and only inherits a destructor is admitted.

**Filed questions (spec-question, at landing; not decided here).** (i) Whether a `TaskScope` converts to `Object` or an interface at all (§1 *Scope*): stdlib §6.10's consumption rule is checkable only at its own static type. (ii) Whether §30.7's and §28.12's "declares no destructor" / "without a destructor" read a destructor that a class only inherits; §30.7's ground ("no synchronous fallback at all") argues for "declares", since the inherited destructor is the base's cleanup, not the subclass's. The compiler reads "declares or inherits".
