# Proposal 0216 — A box is held where it is stored: runtime-managed values at `Object` and interface locations

| | |
|---|---|
| **Status** | under-review — draft (2026-11-26; the v0.361 changelog lists #9219 under *Not ruled*, an owner decision needing a proposal) |
| **Tracking issue** | [#9219](https://github.com/s8lap/bezel-lang/issues/9219) (found while fixing [#9206](https://github.com/s8lap/bezel-lang/issues/9206)) |
| **Target documents** | **spec** — §28.3.1 K1.6 (one sentence) and K4.1 (one sentence), §28.2 (one clause), §28.3.4 (the runtime-managed producers row, one clause), §28.3.8 field-store bullet clause 3 (one sentence), §28.9 (the boxes bullet, one clause; a new paragraph *Held while referenced*), §28.11 (one paragraph after the operations table; one clause in *Storing a borrow*), §30.14 (two words in an implementation note). **rationale** — §4 (one paragraph). **Guide 01** — §3 (one sentence). **stdlib, code-generation reference, Appendix C** — no edit (§6.9). No grammar change, no new name. |
| **Written against** | spec, stdlib and rationale **v0.360** (`origin/main` `bd12fe038`), with v0.361's text where it touches these sections: §26.7's #8113 ruling, read from `spec/v0361-2026-11` at `c85ce271c`. v0.361 changes none of the sentences edited here. |
| **Origin** | #9219, a `spec-question` filed by lane `r43-m103-9206`; triage classed it DESIGN, and the v0.361 triage note on #8113 records that #8113's reasoning ("runtime-managed values have unbounded lifetime") is the argument for the issue's reading 1. |
| **Change class** | Design change ([`spec-change-process.md`](spec-change-process.md) §2): it states a lifetime guarantee the text does not state, edits a kernel clause (K1.6), and commits every implementation to a run-time cost in unchecked builds. No recorded decline is reopened (§4). |

*Conventions. An unprefixed § is a language-spec section at v0.361. Evidence labels: **MEASURED** (a probe run in this lane, or a count re-runnable from the repository), **QUOTED** (repository text), **HYPOTHESIS** (a judgement). Probes ran on `bezelc` 0.0.0-8ccf7ff06 (2026-11-19; it predates both #9206 commits) with programs under `/var/tmp/bezel-scratch/v361/p-0216/probes/`.*

## 0. Summary

A box (§8.6) reaches storage through a location declared `Object` or an interface type. The ownership rules classify such a location as a borrow, because it may hold a class instance. Two sentences then name K4.1's disposed flag as the safety net for a store through such a borrow — §28.3.8 clause 3 ("a member access through the stored borrow after that point is the backstop's fault") and §28.11 ("with K4.1's backstop as the named safety net"). A box has no disposed flag (§8.7, §15.1.1). Whether a box outlives the local that first held it is not decided anywhere: the issue's lane measured a use-after-free in the default build. Today's compiler leaks the box instead where it cannot prove that nothing keeps it.

The proposal: a runtime-managed value is held by every location that holds it, whatever the location's declared type, and is never reclaimed while one does. Runtime-managed here means a box, or a `string`, `Type` or function value converted to `Object`. The borrow classification of an `Object` or interface location then bounds only the class instances it may hold. The two sentences are narrowed to class instances. This is the issue's reading 1, generalized from collection slots to every location.

The reasons, in short:

- The text already presupposes it. Several admitted shapes are sound only if the storing location holds the box: `a.Add(ih)` for an interface-typed local `ih` that holds a box; `a.Add(i)` into an `Array<Object>`; a collection literal `[1, "a"]` at `Array<Object>`; and §28.14's box at a `scoped` parameter, which "is admitted under every bounding object and freezes nothing". A box carries no source (§28.3.4), so no static rule and no trust clause bounds any of them.
- The corpus already expects it. s17_4_0901–0904 and s28_9_4609 read a box back through a field or a local after its first holder is gone and expect it to print. s17_4_0901–0904 pass in the baseline.
- The compiler already does it for class fields (#2500, #8215, #9061). Borrowing collections and the escape-walk's leak path are the remaining work.

Declined: a released flag on boxes with a checked-mode fault (option B); narrowing the sentences only, with a box residual recorded with no backstop (option C); rejecting the store (option D); leaving the text as is (option E). Six owner questions are in §10.

## 1. The problem

### 1.1 The text today (QUOTED, v0.361)

- §28.3.8, field-store bullet, clause 3 (*The residual*): "with K4.1's backstop as the named safety net — live in the default build (a member access through the stored borrow after the source's disposal raises `DisposedAccessException`, §28.2) … Neither source is lifetime-extended: the temporary is disposed at its K8 boundary and the local at its scope's end, and a member access through the stored borrow after that point is the backstop's fault".
- §28.11, *Storing a borrow*: "The residual is a K4.3-recorded trust clause: whether each admitted element's source actually outlives the collection's use of it is the programmer's obligation, with K4.1's backstop as the named safety net".
- §28.11, slot-policy table: an element type is "runtime-managed reference (`string`, function types, `Type`; … no *declared* element type is a boxed value type, §9.7, so an `Object` or interface element type is classified by the row below)", and the row below is "class / interface type | borrows". The borrow column's store cell reads "stores the borrow; the element's source must outlive the collection's use of it".
- §28.3.4, the runtime-managed producers row: a boxing conversion is "Not applicable — the value is runtime-managed (K1.6): no classification, no obligation, no source, and a read of it is no borrow … A parameter, field, capture, getter or indexer result whose static type is `Object` or an interface is classified by its form — a borrow — whatever it holds at run time."
- K1.6: runtime-managed values "carry no classification and no obligation". K4.1: "in checked disposal mode any later use through any alias faults deterministically as `DisposedAccessException` … This is the runtime backstop every trust admission names." K4.3: "no new admission may be made without recording its clause or its backstop".
- §28.9: "the type's lifetime is managed by the runtime without user involvement. How the runtime manages their lifecycle (reference counting, region allocation, or other) is not part of this specification", and, of boxes, "so a boxed value is an immutable shared reference". §9.7, of `string`, `Type` and function types only: "runtime-managed values have unbounded lifetime by construction".
- §8.7: boxed values have no `Dispose()`. §15.1.1, of an owning inline array's interface slot: "a released box behind such a binding has no flag of its own, and the array's state is the flag". §8.6: "releasing the last reference to the box during the call does not end the borrow".
- §28.2: "no class in the language dangles without this backstop" — true, and silent on boxes.
- §28.14: "any other admitted value's box carries no borrow, so the argument has no borrow source: it is admitted under every bounding object and freezes nothing" (#5559).

### 1.2 The issue's repro, traced

```bezel
module M;
public interface IHx
  readonly property P: (IHx) -> int;
end IHx;
public record Rx: IHx
  public property P: (IHx) -> int => lambda(x: IHx) => 1;
  public constructor() end constructor;
end Rx;
public class Program
  public static method Main(): int
    var t := 0;
    var a := new Array<IHx>();
    for var i in 0 ..< 200 do
      var r := new Rx();
      var ih: IHx := r;
      var f: (IHx) -> int := lambda(x: IHx) => do a.Add(x); return 1; end;
      t := t + f(ih);
    end for;
    Console.WriteLine(a.Count);
    Console.WriteLine(a[0].P(a[0]));   // <- the use-after-free
    return 0;
  end Main;
end Program;
```

Each iteration, step by step against the text:

1. `var r := new Rx();` is a record value: no classification (K1.6).
2. `var ih: IHx := r;` boxes (§8.6). The producers row makes the initializer runtime-managed and `ih` **unclassified** (K1): "no obligation, no source", and "a read of it is no borrow".
3. The lambda captures `a`, a local declared before the loop. Its scope encloses `f`'s, so the closure bullet admits `f`.
4. `f(ih)`: the argument is a read of an unclassified local, so it contributes no source to the invocation (§28.3.8 clause 2's argument contributions).
5. In the lambda, `x` is a plain interface-typed parameter, "classified by its form — a borrow". `a.Add(x)` is a parameter-sourced borrow stored into a borrow slot. §28.11's store rule clause 1 admits it, and the residual paragraph makes it a K4.3 trust admission whose safety net is K4.1.
6. At the end of the iteration `ih` goes out of scope. The text gives this point no meaning for the box: scope end over a runtime-managed value "disposes nothing" (§28.9), and nothing says the slot keeps the box alive. §28.3.8 clause 3's "the local at its scope's end" was written for a classified local.

After the loop, `a.Count` is 200. `a[0]` is an indexer-get on a borrow slot: "borrow out (source: the element's original holder)", and that holder is the per-iteration `ih`, now gone. `.P(a[0])` is an interface member access. In checked mode, §28.2 raises only for "a logically-disposed instance", and a box is never logically disposed: it has no `Dispose()` and no flag. In unchecked mode the access is the same access. So on the one path the program has, in both modes, the text leaves the read undefined. The sentence that names a backstop for it is false for a box.

MEASURED, by the issue's lane (r30-c1 @ `7fe91b2b7` plus #9206's first fix, `--sanitize=address --checked-disposal`): `heap-use-after-free … in main` for this program, and `Unhandled exception: System.DisposedAccessException` for its class twin.

QUOTED, from commit `1300d04de` (the #9206 fix-forward on `origin/main`): the box is now released at the local's scope end only where the function value's body is read and keeps nothing of the parameter. Every opaque callee — the repro's interface property read among them — "keeps the pre-fix leak, because the spec does not decide what protects a box an unreadable callee stores". The 9206 test file parks that program as a corpus candidate "expected after #9219: no leak".

MEASURED (this lane, the probe build, system link, valgrind): the repro prints `200` and `1` with no invalid read, and its boxes are still allocated at exit. The class twin (`c6.bzl`: `using var c := new Cx(i)` passed to the same lambda) prints `faulted` from a `DisposedAccessException` arm.

### 1.3 Simpler shapes the text admits with no clause at all

The repro needs a function value to reach the trust clause. The same hazard appears without one, and then nothing names a safety net, because a box is no borrow:

| Shape | What admits it | Who holds the box after its first holder ends |
|---|---|---|
| `var ih: IHx := new Rx(i); a.Add(ih);`, `a` declared before the loop | §28.11 store rule: clause 2 rejects "a borrow of a scope-bound binding", but a read of an unclassified local "is no borrow" | only the slot |
| `a.Add(i)` for `a: Array<Object>` | the boxing conversion at the parameter; §15.4's fresh-store rule covers class-typed owned temporaries only | only the slot (the box is minted for the call) |
| `var args: Array<Object> := [1, "two"];`, then `sb.AppendFormat("{0} {1}", args)` (stdlib §3.1) | §15.4's literal; a box is no literal-tree owned node | only the slot |
| `h.Keep := ih;` for a plain `Keep: IHx?` property of a longer-lived holder | the field-store bullet: the stored value is no borrow | only the field |
| `new Reader(scoped r)` at an interface-typed `scoped` parameter, `r` a record value | §28.14: "admitted under every bounding object and freezes nothing" | only the bounding object's field |

MEASURED (probe build): `c2.bzl` (the first and fourth rows, plus a `string` at `Array<Object>` and a borrow-out local read after `Clear()`) prints `19900`, `199`, `v199`, `1`, and `c7.bzl` (`a.Add(i)` at `Array<Object>`) prints `19900`, each with no invalid read under valgrind.

Every row is a program that looks like value code. Each is sound only if the storing location holds the box. The spec says so nowhere, and two sentences say a disposed flag stands behind the trust-clause version.

### 1.4 Why this is a design question, not a clarification

The text leans one way. §28.9 calls a box "an immutable shared reference". §8.6 speaks of "the last reference to the box". §28.14 admits a box under every bounding object. §15.1.1 has an owned slot release its box. Rationale §4.1 says that reference counting "reclaims such a value when its last reference goes". But it never states the guarantee, and three choices remain open, each changing what the repro means:

- Option A makes the repro print `200` / `1`, and requires every implementation to hold a runtime-managed occupant of an `Object` or interface location. That is a run-time test at stores into such locations in unchecked builds, a cost no current sentence states (§28.9 leaves the mechanism "not part of this specification").
- Option B makes it fault in checked mode. That needs a release point for a value that K1.6 gives none.
- Option C makes it undefined in both modes.

The choice edits a kernel clause and sets a cost model, so it is a design change (process §2). The v0.361 changelog lists #9219 under *Not ruled (owner decisions, each needing a proposal)*.

## 2. The options

Each option is traced through the repro (§1.2), the §1.3 shapes, and the spec's own statements quoted in §1.1.

### 2.1 Option A — a runtime-managed value is held by every location that holds it (recommended)

**Rule.** Whatever the mechanism, a runtime-managed value is not reclaimed while any location holds it, whatever that location's declared type. The locations are a local, a parameter, a field, a collection or inline-array slot of either family, a capture, a frame-resident binding, a temporary and a static. At an `Object` or interface location the K1 classification (a borrow, by form) bounds the class instances the location may hold, never the runtime-managed value it holds. The guarantee is safety only: whether and when a value no location holds is reclaimed stays the mechanism §28.9 leaves open.

**Repro.** Step 5's store makes the slot a holder of the box, and step 6 releases only `ih`'s hold. `a[0].P(a[0])` reads a live box and prints `1`, in both modes. The residual of §28.11 still covers a class instance passed through `f`, and the class twin still faults (`c6.bzl`).

**§1.3 shapes.** All five print. The `scoped` case is what §28.14 already says: the box has no source and freezes nothing, because the bounding object's field holds it.

**Spec statements.** "An immutable shared reference" (§28.9) becomes a stated guarantee. §8.6's "releasing the last reference" sentence becomes a special case (the call's receiver is a location). §15.1.1's released box behind a borrowing-match binding is unchanged: that binding holds the array, not the box, and the array's disposed state still faults the read before it reaches the slot. §26.7's #8113 carve is unchanged and still keys on the declared head: an extension over `Object` or an interface still may not store `this`, because the receiver may be a class instance. §9.7's "unbounded lifetime by construction" for runtime-managed field types is the same statement at a runtime-managed declared type.

**Soundness.** It closes the hazard for runtime-managed values entirely; nothing needs a flag. The class half is untouched: K4.1, K4.3's recorded clauses and the trust-admission report still stand behind every class instance an `Object` or interface location holds. HYPOTHESIS: holds add no reference cycle that function values cannot already form. A cycle through a box needs a function value in the box's payload whose captures reach the box. A capture-bearing function value makes its record a value that holds borrows, which §8.6 boxes only as a call receiver. Owner question Q6 asks whether this argument needs a check before landing.

**Ergonomics.** Every shape in §1.3 means what it looks like. The rule needs no new spelling, diagnostic or remedy.

**Implementation cost** (read from `src/`, read-only; counts MEASURED by `git grep`):

- Class fields at `Object`/interface types already hold boxes, with a run-time discriminator. Box-only fields retain unconditionally (#2500). Mixed fields test the occupant (`emitObjectSlotBoxOp`, the #8118 discriminator, 14 call sites; #8215). Function values at `Object` fields are handled too (#9061).
- Locals bound from such reads pin the box (`box_borrow_pin_8444`; corpus s28_9_4609).
- Missing: the borrowing collections at an `Object`/interface element or key type. The repro is this case. The element-slot factory `elementPolicyFor` (one central function behind 129 call sites) needs a dynamic hold arm: the store retains a runtime-managed occupant, and `Remove`/`RemoveAt`/`Clear`/overwrite/`Dispose` release it. The runtime's free routines already take an element thunk for the owning family, and the discriminator needs a C-callable form for them.
- Simplified: once every keeping location holds its own reference, the boxing local can always release at its scope end. `BoxEscapeWalker`'s escape-means-leak path (22 references; `boxReleaseEligible_`, 27) becomes an optimization, not a safety device. That retires #9206's opaque-callee leak.
- Run-time cost: one occupant test per store and release at `Object`/interface locations whose producer is not statically known, in every build. Strings already pay a count at every store (rationale §4.1 records elision as implementation latitude).

Estimate: one implementation lane for the collections, one for retiring the escape walk's leak path. Both touch allocation, so each runs the ASan leg (CLAUDE.md).

**Migration.** No verdict changes; the corpus already expects this (§8). The 9206 test file's two `expectMemorySafe` rows (`spec_28_11_box_a_function_value_stores_in_a_collection_stays_alive`, `spec_28_3_8_box_a_function_value_stores_in_a_field_stays_alive`) run leak-lenient "until #9219 is ruled". They strengthen to `expectClean` with the implementation, and its comments citing #9219 as open are rewritten. The leak-floor rows `set_object_element_4642`'s `tryadd_contains_box_residual_floor_spec_8_6` and `dict_object_bare_value_key_indexer_set_4640`'s `dict_object_bare_int_key_indexer_set_leak_floor_spec_8_6` pin representation residuals that the guarantee does not address (a safety rule forbids no leak). They flip only if the implementation retires those residuals, and the fix lane re-points them then.

### 2.2 Option B — a checked-mode backstop for boxes (the issue's reading 3)

**Rule.** A box cell carries a released flag (it already has a count word, so a sentinel value can serve). In checked mode, reclamation of a released box is deferred, as for a disposed class (§28.2's quarantine latitude). A member access through a reference to a released box raises `DisposedAccessException`, as do an unboxing `as`, a narrowed read and a typed-pattern binding over it.

**What it needs first.** A flag faults only after a *release*, so B must say when a box is released. The text gives a box "no classification, no obligation, no source" (§28.3.4), so B has to add a lifetime model for boxes. There are two candidates:

- B1: only the location the boxing conversion initialized (plus an owned slot, per §15.1.1) keeps the box.
- B2: every location keeps it except a borrow slot and a parameter.

**Repro.** Under B1 and B2 the box dies at `ih`'s scope end, and `a[0].P(a[0])` raises `DisposedAccessException` in checked mode. It is undefined behaviour unchecked.

**§1.3 shapes.** Under either variant, `a.Add(i)` at `Array<Object>` and the `Array<Object>` literal fault on the first read. The box is minted for the call or the literal, and only the slot ever holds it. So `StringBuilder.AppendFormat`'s documented argument shape (stdlib §3.1) cannot be built. Under B1 the field rows and §28.14's `scoped` admission fault too, which contradicts §28.14's "freezes nothing" ruling (#5559) and flips s17_4_0901–0904 and s28_9_4609 from `run` to a fault. B2 keeps those but is A with a hole cut at the borrow slot, chosen by the table's label alone.

**Soundness.** It is sound in checked mode for the shapes it rejects at run time, but it still leaves undefined behaviour in unchecked builds for programs with no visible kill.

**Ergonomics.** Poor. The remedy for a fault would be "keep the box in a location that keeps it", a distinction no source spelling shows.

**Cost.** A flag test on every box-read path (box dispatch thunks, `as`, narrowing, patterns, `Object`'s `Equals`/`GetHashCode`/`ToString`). Quarantine of released boxes in the default build, unless escape-proven as `bz_cd_may_free` does for classes; boxes are minted at every boxing conversion. Plus the holder-set rule the compiler must implement anyway.

**Migration.** It flips the corpus rows above (B1), or the borrowing-collection rows (B2). It rewrites the 9206 test file's two stays-alive rows.

### 2.3 Option C — narrow the two sentences to class instances and record the box residual

**Rule.** The two sentences say "for a class instance". A new K4.3 clause records that a runtime-managed value read through a trust-admitted location after its holder's release has no backstop, as the domain clause records a race. This needs the same release model as B, or it leaves the release point unspecified.

**Repro.** Undefined in both modes, so the default build has a use-after-free in code that never names a disposal.

**§1.3 shapes.** The trust clause does not even reach most of them (no trust admission is made: a box is no borrow). C must either extend the report to every store of an `Object`/interface value whose occupant could be a box, which is every such store, or leave them outside any clause. That breaks K4.3's "no new admission may be made without recording its clause or its backstop".

**Soundness.** The weakest option: it turns §28.2's posture (checked mode is the default, and unchecked is a relaxation of it) into undefined behaviour in the default build.

**Ergonomics.** Same as B, without the fault.

**Cost.** None in the compiler: today's leak is conforming, and so would the lane's use-after-free be.

**Migration.** The corpus rows in §2.2 lose their defined output, so they would be deleted.

C1, narrowing the sentences and saying nothing more, is not a different option. It leaves the question exactly where it is today, with the sentences now merely silent.

### 2.4 Option D — reject the store (the issue's reading 2)

The store site cannot see that the source is a box: in the repro it is a plain parameter, and in §1.3 an `Object`-typed value of unknown provenance. Rejecting every store of a plain `Object`/interface parameter would reject `Array<IShape>.Add(shape)` for class instances, the collection form of the back-reference idiom. `DECLINED.md` already rejects that callee-side rule ("A callee-side rule forbidding the store of every plain `Object` or interface parameter …"). It also rejects the slot form: "Rejecting a runtime-managed (boxed) value in an owning inline array's interface-typed slot", because "which value a slot holds is a per-slot runtime fact no static rule sees". Rejecting at the caller would need the callee's body, which `DECLINED.md`'s K8.4 row declines. D is not viable.

### 2.5 Option E — leave the text as is (decline)

The two sentences stay false for boxes, and the §1.3 shapes stay without a stated guarantee. Implementations stay free to leak, as `origin/main` does where its escape walk cannot see the callee. The leak is memory-safe and unbounded (the 9206 file's measured 24 B per boxing). The issue stays open. E is viable only as deferral.

## 3. Recommendation

Adopt option A, scoped to every runtime-managed value at an `Object` or interface location: a box, and a `string`, `Type` or function value converted to `Object`. The reasons:

1. It is the only option under which every shape the text already admits has a meaning (§1.3). B and C turn `a.Add(i)` at `Array<Object>` into a fault or undefined behaviour.
2. It is what the text already leans on (§1.4) and what the corpus already expects (s17_4_0901–0904 pass; s28_9_4609 and s28_9_9601 are written to it). B1 and C would flip or delete those cases.
3. It is mostly implemented: class fields, locals and pins hold boxes today, and the missing piece is the borrowing collections.
4. Its cost is the one strings already pay, at the locations where a box can appear. It adds no ownership kind, because K1.6 keeps runtime-managed values outside the classification.
5. It composes with proposal 0212's stage 3 (§5).

Generalizing to strings, `Type` and function values at `Object` costs nothing extra. Those are the other occupants the #8118 discriminator already tells apart (#9061 is the function-value fix), and a rule for boxes alone would leave a string at an `Object` field under the same false sentence. Owner question Q2 asks about it.

## 4. Records this proposal answers

A change is a design change the moment it contradicts a recorded decision (process §2). This one contradicts none:

- **Rationale §5.4's declines**: implicit handle duplication, reference counting arbitrary classes through an annotation, compiler-inferred sharing by escape analysis, and "sharing as a third reference kind beside owned and borrowed". These are declines about class instances and `Shared<T>` handles. Option A holds no class instance and adds no K1 kind. Runtime-managed values are outside K1 already (K1.6), and their reclamation is "the runtime's business rather than the program's" (rationale §4).
- **`DECLINED.md`, the K4.3 clause for a boxed value that holds borrows**, and the borrow-erasing-position rows (#5645): untouched. A value that holds borrows still reaches no `Object`/interface position but a call receiver, so no such box is ever stored.
- **`DECLINED.md`, the callee-side no-store rule, and the owning-inline-array slot row (#5554)**: kept. They are option D's grounds.
- **#8113 (v0.361)**: kept, keyed on the declared head as triage required. A does not relax §26.7 for an `Object`/interface head.
- **#5559 (§28.14, a box at a `scoped` parameter)**: A is the reading under which that ruling is sound.

## 5. Interaction with proposal 0212

Proposal 0212 is accepted; its stage 1 landed at v0.357 and stages 2–3 are scheduled. At stage 3 it deletes K4.3's field-held and collection-held clauses, the two sentences this proposal narrows, and makes a plain parameter call-bounded for an object borrow: only a constructor, or a `scoped` parameter, may keep one. Two consequences:

- The repro's shape (a lambda keeping its plain parameter) becomes a compile error at stage 3 (§28.15's callee-side rule). Corpus case `s28_9_2161` (§8) flips then. The other cases do not use that shape.
- 0212 reads what an argument contributes from the argument, and a box contributes no source. Its mint, `scoped` and later-store rules therefore admit a box into a constructor-built field, a `scoped` field or a collection with nothing bounding it. That is sound only under option A. Under B or C, stage 3 would keep a box hole outside every model 0212 measured; 0212's model has no runtime-managed values. A is therefore a prerequisite of 0212's stage-3 claim at `Object` and interface positions (Q5). The edits in §6 land before stage 3 and survive it, except the two narrowed sentences, which stage 3 deletes.

## 6. The normative edits

Every *old* string below occurs exactly once in the v0.361 text (MEASURED, `grep -c -F`).

### 6.1 Spec §28.3.1, K1.6

Old:

> Values of runtime-managed type (`string`, `Type`, boxed value types, function values) and of ordinary value type carry no classification and no obligation; `owned` markers over them are inadmissible or erased per instantiation ([§28.1](#sec-where-the-owned-modifier-can-appear)).

New:

> Values of runtime-managed type (`string`, `Type`, boxed value types, function values) and of ordinary value type carry no classification and no obligation; `owned` markers over them are inadmissible or erased per instantiation ([§28.1](#sec-where-the-owned-modifier-can-appear)). A runtime-managed value is held by every location that holds it, whatever that location's declared type and classification, and is never reclaimed while one does ([§28.9](#sec-runtime-managed-types) *Held while referenced*): no source bounds it, and no rule of this chapter needs to.

### 6.2 Spec §28.3.1, K4.1

Old:

> This is the runtime backstop every trust admission names.

New:

> This is the runtime backstop every trust admission names for a class instance; a runtime-managed value held at a trust-admitted location needs none, the location holding it (K1.6).

### 6.3 Spec §28.2, *The compiler-inserted free*

Old:

> no class in the language dangles without this backstop.

New:

> no class in the language dangles without this backstop, and no runtime-managed value dangles at all ([§28.9](#sec-runtime-managed-types) *Held while referenced*).

### 6.4 Spec §28.3.4, the runtime-managed producers row (a table cell; the text has no `|`)

Old:

> A parameter, field, capture, getter or indexer result whose static type is `Object` or an interface is classified by its form — a borrow — whatever it holds at run time.

New:

> A parameter, field, capture, getter or indexer result whose static type is `Object` or an interface is classified by its form — a borrow — whatever it holds at run time: the classification bounds the class instances such a position may hold, and a runtime-managed value it holds is held by it ([§28.9](#sec-runtime-managed-types) *Held while referenced*).

### 6.5 Spec §28.3.8, field-store bullet, clause 3

Old:

> Neither source is lifetime-extended: the temporary is disposed at its [K8](#sec-kernel-k8) boundary and the local at its scope's end, and a member access through the stored borrow after that point is the backstop's fault; a temporary meets the obligation only when the object dies within the same full expression.

New:

> Neither source is lifetime-extended: the temporary is disposed at its [K8](#sec-kernel-k8) boundary and the local at its scope's end, and a member access through the stored borrow after that point is the backstop's fault; a temporary meets the obligation only when the object dies within the same full expression. A runtime-managed argument — a box ([§8.6](#sec-boxing)), or a `string`, `Type` or function value converted to `Object` — is neither: it carries no classification ([K1](#sec-kernel-k1), K1.6), the location the callee stores it in holds it ([§28.9](#sec-runtime-managed-types) *Held while referenced*), and no access through that location meets a reclaimed value, in either disposal mode.

### 6.6 Spec §28.9

(a) The boxes bullet. Old:

> so a boxed value is an immutable shared reference.

New:

> so a boxed value is an immutable shared reference, held by every location that holds it (*Held while referenced*, below).

(b) A new paragraph, inserted immediately before the paragraph that begins "This tier is exhaustive: no runtime lifetime mechanism exists for user-defined classes at all":

> <a id="sec-held-while-referenced"></a>**Held while referenced.** Whatever the mechanism, a runtime-managed value is not reclaimed while any location holds it — a local, a parameter, a field, a slot of a collection of either family or of an inline array, a capture, a frame-resident binding, a temporary or a static — whatever that location's declared type. Where the declared type is itself runtime-managed this is the type's own contract ([§9.7](#sec-records-and-disposal): such values "have unbounded lifetime by construction"). Where it is `Object` or an interface type — a box ([§8.6](#sec-boxing)), or a `string`, `Type` or function value converted to `Object` — the location is classified by its form, a borrow ([§28.3](#sec-producers)'s runtime-managed row), and that classification bounds the class instances the location may hold instead, never the runtime-managed value it holds: a box stored through a plain parameter into a field or a borrowing-collection slot ([§28.3](#sec-borrow-lifetime-rules)'s field-store bullet, [§28.11](#sec-borrowing-collections)'s store rule) lives while that field or slot holds it, a local bound from a read of such a location holds what it read, and no read meets a reclaimed value, in either disposal mode. A runtime-managed value is therefore never the subject of a trust admission's residual, and [K4.1](#sec-kernel-k4)'s disposed flag, which it does not have ([§8.7](#sec-dispose-is-not-on-object)), is not needed for it. The guarantee is about safety only: whether and when an implementation reclaims a value no location holds is the mechanism this section leaves open. *Implementation note (non-normative): a location declared `Object` or an interface type cannot tell a box from a class instance statically, so an implementation that counts references tests the occupant at run time at each store into and release of such a location, and may omit the test where the stored value's producer is statically known; unchecked builds pay that test, the price of the guarantee.*

### 6.7 Spec §28.11

(a) A new paragraph, inserted immediately after the operations table (before "The per-collection clauses in the remainder of this section"):

> A borrow slot whose occupant is **runtime-managed** — a box ([§8.6](#sec-boxing)), or a `string`, `Type` or function value at an `Object` element type; which kind of value a slot holds is a per-slot run-time fact, as [§15.1.1](#sec-owning-inline-arrays)'s slot pass reads it — holds that occupant as a shared-reference slot does: the store holds it, and `Remove`/`RemoveAt`, `Clear()`, an overwriting store and the collection's `Dispose()` release that hold, still with no user-visible work ([§28.9](#sec-runtime-managed-types) *Held while referenced*). A class instance in the same slot is a borrow, as the table states.

(b) *Storing a borrow*, the residual. Old:

> with [K4.1](#sec-kernel-k4)'s backstop as the named safety net; the obligation carries

New:

> with [K4.1](#sec-kernel-k4)'s backstop as the named safety net for a class-instance element — a runtime-managed element is held by its slot (the paragraph after the operations table) and is outside the residual; the obligation carries

### 6.8 Spec §30.14, sendable types (implementation note)

Old:

> the runtime's reclamation bookkeeping for strings, and its materialization

New:

> the runtime's reclamation bookkeeping for strings and boxes, and its materialization

### 6.9 Not edited, with reasons

- **Stdlib.** Its collection clauses are presentations of §28.11's table ("a per-collection statement that diverges from the table is a defect"), so §6.7 covers them. No signature changes.
- **Code-generation reference.** Its §4 rules state the class-instance obligations and impose none on boxes; the guarantee removes a hazard rather than adding a rule a writer must follow. Its maintenance rule admits a new clause only as a table row or a *looks right, is rejected* pair, and there is no rejected shape. Its pin moves with the revision as usual.
- **Appendix C.** The guarantee is a run-time property, not a compile-time invariant.
- **§8.6, §9.7, §15.1.1, §26.7, §28.14.** Each is consistent with A as written (§2.1).

### 6.10 Rationale §4

Insert after the paragraph "Boxed values and `Type` tokens are runtime-managed on the same footing: …":

> **A runtime-managed value is held by every location that holds it, whatever that location's declared type ([#9219](https://github.com/s8lap/bezel-lang/issues/9219); proposal [0216](proposals/0216-runtime-managed-values-held-where-stored.md)).** A box reaches storage through a location declared `Object` or an interface type, which the ownership rules classify as a borrow because it may hold a class instance. Until v0.36x two sentences named K4.1's disposed flag as the safety net for a store through such a location, a flag a box does not have. The plainest shapes — `a.Add(ih)` for an interface-typed local holding a box, `a.Add(i)` into an `Array<Object>`, a collection literal of `Object` elements, a box at a `scoped` parameter — are admitted with no trust clause at all, because a box carries no source. Only a hold by the storing location makes those admissions sound. It is the posture §4.1 already states for strings: reference counting reclaims such a value when its last reference goes. The price is a run-time occupant test at stores into `Object`- and interface-typed locations, in unchecked builds too, which an implementation omits where the producer is known. No class instance is ever held this way and no ownership kind is added (K1.6 keeps runtime-managed values outside the classification), so [§5.4](#sec-no-shared-ownership)'s declines of implicit sharing stand. Declined: a released flag on boxes with a checked-mode fault (it needs a release point for a value the kernel gives none, faults `a.Add(i)` at `Array<Object>` on its first read, and quarantines boxes in every default build); narrowing the two sentences and recording a box residual with no backstop (a use-after-free in the default build in programs that read as value code); rejecting the store (which value a location holds is a run-time fact no static rule sees, the ground of the v0.327 inline-array-slot decline).

### 6.11 Guide 01, §3 (the *stashed reference* paragraph)

After "… as a `DisposedAccessException` in the default build." insert:

> A value boxed at an interface type — a record or an enum stashed in an interface-typed field or collection — is not such a target: a box is runtime-managed, and the field or slot that holds it keeps it alive (spec §28.9).

## 7. Changelog sketch

> **#9219, a runtime-managed value is held where it is stored** (design change; proposal [0216](proposals/0216-runtime-managed-values-held-where-stored.md); spec K1.6, K4.1, §28.2, §28.3.4, §28.3.8 clause 3, §28.9 *Held while referenced* (new), §28.11 (one paragraph, one clause), §30.14 (note); rationale §4; Guide 01 §3).
> - *Decided.* A runtime-managed value — a box, or a `string`, `Type` or function value converted to `Object` — is held by every location that holds it and is never reclaimed while one does, whatever the location's declared type. The borrow classification of an `Object` or interface location bounds the class instances it may hold. The two sentences that named K4.1's flag as the safety net for a trust-admitted store now say so for a class instance; a box needs none.
> - *Declined.* A checked-mode released flag on boxes (it needs a box release point and faults `a.Add(i)` at `Array<Object>`); narrowing the sentences with a box residual recorded with no backstop (undefined behaviour in the default build); rejecting the store (a per-slot run-time fact, as the v0.327 inline-array-slot decline records).
> - **Migration.** No program changes verdict. A program that reads a box back through a field, a borrowing-collection slot or a local after the box's first holder is gone had no stated meaning; it now has the one it reads as having. Implementations: a borrowing collection at an `Object` or interface element or key type holds its runtime-managed occupants and releases them at removal, overwrite, `Clear()` and disposal; a boxing local may then release its own reference at its scope end whatever the callee. Tests: the 9206 file's two stays-alive rows strengthen to `expectClean` with that implementation. Rule corpus: six cases added (§8), none flipped.

## 8. Rule corpus (process §5 item 3c)

New cases (ids proposed; free on `origin/main` and on `spec/v0361-2026-11`). Each verdict comes from §6.6's sentence, not from the compiler. Each case was probed on the probe build for syntax and output.

| Case | Section | Program | Expect |
|---|---|---|---|
| `s28_9_2161` | 28.9 | the issue's repro (§1.2) | `run`, output `200` / `1`. Header note: flips to `reject` at proposal 0212's stage 3 (the lambda keeps a plain parameter) |
| `s28_9_2162` | 28.9 | `var ih: IHx := new Rx(i); a.Add(ih);` over 200 iterations, `a` declared before the loop; sum `e.V()` after | `run`, stdout `19900` |
| `s28_9_2163` | 28.9 | `h.Keep := ih;` each iteration, `using var h` declared before the loop, a plain `Keep: IHx?` property; print `h.Keep!.V()` after | `run`, stdout `199` |
| `s28_9_2164` | 28.9 | `var s: Object := $"v{i}"; b.Add(s);` into `b: Array<Object>`; print `b[199]` after | `run`, stdout `v199` |
| `s28_11_2161` | 28.11 | `a.Add(i)` into `a: Array<Object>` over 200 iterations; sum `(o as int) ?? 0` | `run`, stdout `19900` |
| `s28_11_2162` | 28.11 | a local `var first := a[0];` over boxed elements, then `a.Clear();`, then `first.V() + 1` | `run`, stdout `1` |

Control, the class half unchanged: `s28_11_2163`, the class twin of `s28_9_2161` (`using var c := new Cx(i)` passed to the same lambda; the read is in `try` with a `DisposedAccessException` arm that prints `faulted`) → `run`, stdout `faulted`. That makes seven cases.

Existing cases that A keeps (no edit): s17_4_0901–0904, s28_9_4609, s28_9_9601, s17_5_0015. Under B1 or C, the first five would flip or be deleted (§2.2–§2.3). The 9206 lane's parked candidate `9206-opaque-callee.bzl` becomes `s28_9_2161`'s twin once the implementation lands (its stated expectation, "no leak", is the implementation's, not the corpus's: the corpus has no leak leg).

## 9. Implementation sketch (non-normative)

1. **Borrowing collections at `Object`/interface element or key types.** Add a dynamic hold arm to `elementPolicyFor`'s class/interface × borrowing case: retain a runtime-managed occupant at every store and release it at `Remove`/`RemoveAt`/`Clear`/overwrite/`Dispose`. Use the #8118 discriminator, given a C-callable form so the runtime's free routines can call it as they call owned-element thunks. Covers `Array`, `Dictionary` keys and values, `Set`, `LinkedList`, `Stack`, `Queue`.
2. **The boxing local's release.** Once (1) lands, every location that keeps a box holds its own reference, so a boxing local releases at its scope end unconditionally. `BoxEscapeWalker`'s escape-keeps-it-alive path becomes dead as a safety device. Retire it, or keep it only as an optimization, and the #9206 opaque-callee leak goes with it.
3. **Audit the remaining holders**: statics of `Object`/interface type at the exit pass, capture cells (the runtime header already describes +1-retained box captures), async and iterator frames.
4. **Tests.** Strengthen the 9206 rows (§2.1). Add an ASan leg over the §8 shapes looped 200×, per CLAUDE.md's allocation rule.

## 10. Owner questions

- **Q1. Which option?** Recommended: A (§3). B and C are recorded as declined (§6.10's rationale text; `DECLINED.md` rows below, indexed only on acceptance under the 2026-08-20 scope rule).
- **Q2. Scope.** Boxes only, or every runtime-managed value at an `Object` or interface location (boxes, and `string`, `Type` and function values converted to `Object`)? Recommended: every one. It is the same discriminator (#8118, #9061), and boxes alone would leave a string at an `Object` field under the same false sentence.
- **Q3. Safety only, or reclamation too?** Should *Held while referenced* also say that a value no location holds is reclaimed, which would make today's escape-walk leak nonconforming? Recommended: safety only. §28.9 keeps the mechanism open, and the leaks are quality defects to file, as #9206 already is.
- **Q4. The cost.** Accept a run-time occupant test at stores into, and releases of, `Object`/interface locations in unchecked builds, stated in a non-normative note? Recommended: yes. The representation (a header bit, a tagged pointer) is the implementation's choice.
- **Q5. Proposal 0212.** Record in 0212's stage-3 edit list that a box contributes no source because its storing location holds it (this proposal's §5), and that `s28_9_2161` flips to `reject` then? Recommended: yes.
- **Q6. Cycles.** Accept §2.1's argument (a box that could reach itself needs a capture-bearing function value in its payload, which §8.6 boxes only as a call receiver), or require a model check or a probe sweep before landing? Recommended: accept, with a note on the tracker.

**`DECLINED.md` rows (proposed; indexed only if the proposal is accepted):**

| Item | Class | Where recorded | Reason / reopening bar |
|---|---|---|---|
| A released flag on boxed values with a checked-mode `DisposedAccessException` for an access through a reference to a released box (#9219 reading 3) | `declined-at-proposal` | proposal 0216 §2.2; rationale §4 | It needs a release point for a value K1.6 gives none; under any release model short of a hold by every location it faults `a.Add(i)` at `Array<Object>` and the `Array<Object>` literal on their first read, and it quarantines boxes in every default build. Reopening bar: a runtime-managed value that cannot be held by its location (a representation with no count). |
| Narrowing §28.3.8 clause 3 and §28.11's residual to class instances and recording a box residual with no backstop, as the domain clause is recorded | `declined-at-proposal` | proposal 0216 §2.3; rationale §4 | A use-after-free in the default build in programs that read as value code, most of them outside any trust admission (a box is no borrow), against K4.3's "no new admission … without recording its clause or its backstop". Reopening bar: —(none stated). |
| Rejecting the store of a box into a field or a borrowing-collection slot (#9219 reading 2) | `declined-at-proposal` | proposal 0216 §2.4 | Which value a location holds is a per-slot run-time fact no static rule sees (the v0.327 inline-array-slot row), and the callee-side form is the rejected no-store rule. Reopening bar: that row's. |
