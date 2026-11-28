# Proposal 0216 — A box is held where it is stored: runtime-managed values at `Object` and interface locations

| | |
|---|---|
| **Status** | accepted — accepted (2026-11-26, owner: all recommended answers); critiqued (pc-0216, SOUND-WITH-FIXES), revised |
| **Tracking issue** | [#9219](https://github.com/s8lap/bezel-lang/issues/9219) (found while fixing [#9206](https://github.com/s8lap/bezel-lang/issues/9206)) |
| **Target documents** | **spec** — §28.3.1 K1.6 (one sentence) and K4.1 (one sentence), §28.2 (one clause), §28.3.4 (the runtime-managed producers row, one clause), §28.3.8 field-store bullet clause 3 (one sentence), §28.9 (the boxes bullet, one clause; a new paragraph *Held while referenced*), §28.11 (one paragraph after the operations table; one clause in *Storing a borrow*), §30.14 (two words in an implementation note). **rationale** — §4 (one paragraph). **Guide 01** — §3 (one sentence). **stdlib, code-generation reference, Appendix C** — no edit (§6.9). No grammar change, no new name. |
| **Written against** | spec, stdlib, rationale and guides **v0.361** (`origin/main` `cdd2022c7`). Every *old* string in §6 was re-verified there (MEASURED, `grep -c -F`: each occurs exactly once). `src/` and test anchors were read at `bd12fe038` and re-checked at `cdd2022c7`. |
| **Origin** | #9219, a `spec-question` filed by lane `r43-m103-9206`. Triage classed it DESIGN. Its note on #8113 records that #8113's reasoning ("runtime-managed values have unbounded lifetime") is the argument for the issue's reading 1. |
| **Change class** | Design change ([`spec-change-process.md`](spec-change-process.md) §2): it states a lifetime guarantee the text does not state at `Object` and interface locations, edits a kernel clause (K1.6), and commits every implementation to a run-time cost in unchecked builds. No recorded decline is reopened (§4). |

*Conventions. An unprefixed § is a language-spec section at v0.361. Evidence labels: **MEASURED** (a probe run in this lane or by the critic, or a count re-runnable from the repository), **QUOTED** (repository text), **HYPOTHESIS** (a judgement). Probes ran on `bezelc` 0.0.0-8ccf7ff06 (2026-11-19), which predates both #9206 commits, linked with `--link=system` and run under valgrind. The lane's probes are in `/var/tmp/bezel-scratch/v361/p-0216/probes/`; the critic's are in `/var/tmp/bezel-scratch/v361/pc-0216/probes/`.*

## 0. Summary

A box (§8.6) reaches storage through a location declared `Object` or an interface type. The ownership rules classify such a location as a borrow, because it may hold a class instance. Two sentences name K4.1's disposed flag as the safety net for a store through such a borrow:

- §28.3.8 clause 3: "a member access through the stored borrow after that point is the backstop's fault";
- §28.11: "with K4.1's backstop as the named safety net".

A box has no disposed flag (§8.7, §15.1.1). The spec states that a runtime-managed value outlives the location that first held it only where the location's declared type is runtime-managed (§26.7, §28.11's copy-out cell). At an `Object` or interface location it says nothing. The issue's lane measured a use-after-free on #9206's first fix, which was rejected and released the box at scope end whatever the callee did. Today's compiler leaks the box instead wherever the callee may keep it.

The proposal: a runtime-managed value is not reclaimed while any live location refers to it, whatever that location's declared type. This covers a box, and a `string`, `Type` or function value converted to `Object` or to an interface type it implements. The borrow classification of an `Object` or interface location then bounds only the class instances it may hold, and the two sentences are narrowed to class instances. This is the issue's reading 1, generalized from collection slots to every live location.

The reasons, in short:

- The text already depends on it. Several shapes are admitted with no trust clause at all, because a box carries no source (§28.3.4), and each is sound only if the storing location keeps the box: `a.Add(ih)` for an interface-typed local `ih` that holds a box; `a.Add(i)` into an `Array<Object>`; a collection literal at `Array<Object>`; and §28.14's box at a `scoped` parameter, which "is admitted under every bounding object and freezes nothing".
- The corpus already expects it. s17_4_0901–0904 pass in the baseline, and s28_9_4609 is written to it.
- The v0.361 #8113 ruling says the same at runtime-managed declared types: "the stored value living as long as its holder".
- The compiler already does it for some class fields. It does not yet do it for the borrowing collections, or for the `Object`/interface fields its per-module pre-pass excludes. Those must hold before the #9206 leak path can be retired (§9).

The cost is a run-time occupant test at stores into and releases of `Object` and interface locations, in every build, borne by class instances at those locations too (§2.1, Q4). Cycles among runtime-managed values can form through a function value's capture cell, as recursive closures already form them, and under counting they leak. The guarantee is about safety only (Q3, Q6).

Declined: a released flag on boxes with a checked-mode fault (option B); narrowing the sentences, with a box residual recorded with no backstop (option C); rejecting the store (option D); leaving the text as is (option E). Six owner questions are in §10.

## 1. The problem

### 1.1 The text today (QUOTED, v0.361)

- §28.3.8, field-store bullet, clause 3 (*The residual*): "with K4.1's backstop as the named safety net — live in the default build (a member access through the stored borrow after the source's disposal raises `DisposedAccessException`, §28.2) … Neither source is lifetime-extended: the temporary is disposed at its K8 boundary and the local at its scope's end, and a member access through the stored borrow after that point is the backstop's fault".
- §28.11, *Storing a borrow*: "The residual is a K4.3-recorded trust clause: whether each admitted element's source actually outlives the collection's use of it is the programmer's obligation, with K4.1's backstop as the named safety net".
- §28.11, the slot-policy table:
  - An element type is "runtime-managed reference (`string`, function types, `Type`; … no *declared* element type is a boxed value type, §9.7, so an `Object` or interface element type is classified by the row below)". The row below is "class / interface type | borrows", and the borrow column's store cell reads "stores the borrow; the element's source must outlive the collection's use of it".
  - The copy-out cell says that "a runtime-managed reference the copy holds — a `DictionaryEntry<string, V>`'s `Key` — keeps §28.9's unbounded lifetime through every later mutation or disposal of the collection".
- §28.3.4, the runtime-managed producers row: a boxing conversion is "Not applicable — the value is runtime-managed (K1.6): no classification, no obligation, no source, and a read of it is no borrow … A parameter, field, capture, getter or indexer result whose static type is `Object` or an interface is classified by its form — a borrow — whatever it holds at run time."
- The kernel:
  - K1.6: runtime-managed values "carry no classification and no obligation".
  - K4.1: "in checked disposal mode any later use through any alias faults deterministically as `DisposedAccessException` … This is the runtime backstop every trust admission names."
  - K4.3: "no new admission may be made without recording its clause or its backstop".
- §28.9 says "the type's lifetime is managed by the runtime without user involvement. How the runtime manages their lifecycle (reference counting, region allocation, or other) is not part of this specification", and, of boxes, "so a boxed value is an immutable shared reference".
- The two places where the holder rule is stated, both at runtime-managed **declared** types:
  - §9.7, of `string`, `Type` and function types: "runtime-managed values have unbounded lifetime by construction".
  - §26.7 (v0.361, #8113), of `this` in an extension over `string` or `Type`: storing it "is the store of a runtime-managed value … the stored value living as long as its holder (runtime-managed values have unbounded lifetime by construction, §9.7)". The v0.361 changelog adds: "The issue's use-after-free is a codegen obligation".
- §8.7: boxed values have no `Dispose()`. §15.1.1, of an owning inline array's interface slot: "a released box behind such a binding has no flag of its own, and the array's state is the flag". §8.6: "releasing the last reference to the box during the call does not end the borrow".
- §28.2: "no class in the language dangles without this backstop", which is true and silent on boxes.
- §28.14: "any other admitted value's box carries no borrow, so the argument has no borrow source: it is admitted under every bounding object and freezes nothing" (#5559; Appendix C.3 restates it).

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

Each iteration, against the text:

1. `var r := new Rx();` is a record value with no classification (K1.6).
2. `var ih: IHx := r;` boxes (§8.6). The producers row makes the initializer runtime-managed and `ih` **unclassified** (K1): "no obligation, no source", and "a read of it is no borrow".
3. The lambda captures `a`, a local declared before the loop. Its scope encloses `f`'s, so the closure bullet admits `f`.
4. `f(ih)`: the argument is a read of an unclassified local, so it contributes no source to the invocation.
5. In the lambda, `x` is a plain interface-typed parameter, "classified by its form — a borrow". `a.Add(x)` stores a parameter-sourced borrow into a borrow slot. §28.11's store rule clause 1 admits it, and the residual paragraph makes it a K4.3 trust admission whose safety net is K4.1.
6. At the end of the iteration `ih` goes out of scope. The text gives this point no meaning for the box. Scope end over a runtime-managed value "disposes nothing" (§28.9), and nothing says the slot keeps the box alive. §28.3.8 clause 3's "the local at its scope's end" was written for a classified local.

After the loop, `a.Count` is 200. `a[0]` is an indexer-get on a borrow slot: "borrow out (source: the element's original holder)". That holder is the per-iteration `ih`, now gone. `.P(a[0])` is an interface member access. In checked mode §28.2 raises only for "a logically-disposed instance", and a box is never logically disposed: it has no `Dispose()` and no flag. In unchecked mode the access is the same access. So on the program's one path, in both modes, the text leaves the read undefined, and the sentence that names a backstop for it is false for a box.

MEASURED, by the issue's lane: `heap-use-after-free … in main` for this program under `--sanitize=address --checked-disposal`, and `Unhandled exception: System.DisposedAccessException` for its class twin. The build was #9206's first fix, `e07b4b5b0`, which released the box at the local's scope end whatever the callee did. Review rejected it on this measurement. It landed reworded as `narrow #9206` (`80f66d91b`) together with its direct child, the fix-forward `1300d04de`, so `origin/main` has carried the fix-forward since. The issue's text attributes the measurement to "r30-c1 @ `7fe91b2b7`"; the fix-forward's message says r30-c1 "only leaked".

QUOTED, from `1300d04de` and its test file. The box is released at the local's scope end only where the function value's body is read and keeps nothing of the parameter. This repro's callee is read: a never-written `var` bound to a lambda literal. But its body stores the parameter (`a.Add(x)`, an escape), so the commit's "every storing callee" row keeps the pre-fix leak. Every opaque callee — a parameter, a field, or a property read such as #9206's own `ih.P(ih)` — keeps it too. The commit's stated interpretation is that "the spec does not say what happens to a box an OPAQUE function value … stores: that is #9219", and the test file says the same of "a box an unreadable callee stores". The 9206 file parks #9206's `ih.P(ih)` program as corpus candidate `9206-opaque-callee.bzl`, "expected after #9219: no leak".

MEASURED, on the probe build:

- The repro prints `200` and `1` with no invalid read; its boxes are still allocated at exit.
- The class twin `c6.bzl` (`using var c := new Cx(i)` passed to the same lambda) prints `faulted` from a `DisposedAccessException` arm with `--checked-disposal`.
- The critic measured that the probe build's default build SEGVs on `c6.bzl` (two invalid reads under valgrind). That build's default diverges from §28.2's checked default.

### 1.3 Simpler shapes the text admits with no clause at all

The repro needs a function value to reach the trust clause. The same hazard appears without one, and then nothing names a safety net, because a box is no borrow:

| Shape | What admits it | Who holds the box after its first holder ends |
|---|---|---|
| `var ih: IHx := new Rx(i); a.Add(ih);`, with `a` declared before the loop | §28.11 store rule: clause 2 rejects "a borrow of a scope-bound binding", but a read of an unclassified local "is no borrow" | only the slot |
| `a.Add(i)` for `a: Array<Object>` | the boxing conversion at the parameter; §15.4's fresh-store rule covers class-typed owned temporaries only | only the slot (the box is minted for the call) |
| `var args: Array<Object> := [1, "two"];`, then `sb.AppendFormat("{0} {1}", args)` (stdlib §3.1) | §15.4's literal; a box is no literal-tree owned node; corpus `s17_5_0015` runs the shape | only the slot |
| `h.Keep := ih;` for a plain `Keep: IHx?` property of a longer-lived holder | the field-store bullet: the stored value is no borrow | only the field |
| `Tag(s, new R(i + 1000))` storing its `o: Object` parameter into `s.saved` (corpus s17_4_0901) | the field-store bullet's parameter-sourced admission | only the field (the box is minted for the call) |
| `new Reader(scoped r)` at an interface-typed `scoped` parameter, `r` a record value | §28.14: "admitted under every bounding object and freezes nothing" | only the bounding object's field |

MEASURED, on the probe build, re-run by the critic: `c2.bzl` covers the first and fourth rows, plus a `string` at `Array<Object>` and a borrow-out local read after `Clear()`, and prints `19900`, `199`, `v199`, `1`. `c7.bzl` (`a.Add(i)` at `Array<Object>`) prints `19900`. Neither has an invalid read; both leak.

Every row is a program that reads as value code, and each is sound only if the storing location keeps the box. The spec says so for no `Object` or interface location, and two sentences say a disposed flag stands behind the trust-clause version.

### 1.4 Why this is a design question, not a clarification

The text leans one way:

- §28.9 calls a box "an immutable shared reference".
- §8.6 speaks of "the last reference to the box".
- §28.14 admits a box under every bounding object.
- §15.1.1 has an owned slot release its box.
- Rationale §4.1 says reference counting "reclaims such a value when its last reference goes".

But it states the guarantee only at runtime-managed declared types (§26.7 v0.361: "the stored value living as long as its holder"; §28.11's copy-out cell), never at an `Object` or interface location. Three choices remain open, and each changes what the repro means:

- **Option A** makes the repro print `200` / `1`. It requires every implementation to keep a runtime-managed occupant of a live `Object` or interface location, at a run-time cost in unchecked builds that no current sentence states (§28.9 leaves the mechanism "not part of this specification").
- **Option B** makes the repro fault in checked mode. It needs a release point for a value K1.6 gives none.
- **Option C** makes the repro undefined in both modes.

The choice edits a kernel clause and sets a cost model, so it is a design change (process §2). The v0.361 changelog lists #9219 under *Not ruled (owner decisions, each needing a proposal)*.

## 2. The options

Each option is traced through the repro (§1.2), the §1.3 shapes and the statements quoted in §1.1.

### 2.1 Option A — a live location keeps the runtime-managed value it refers to (recommended)

**Rule.** Whatever the mechanism, a runtime-managed value is not reclaimed while any **live** location refers to it, whatever that location's declared type. A location is live while its holder is:

- a local in scope;
- a parameter of a running activation;
- a field of an instance not yet disposed;
- a collection or inline-array slot whose holder has not released it;
- a capture;
- a frame-resident binding;
- a temporary before its K8 boundary;
- a static.

At an `Object` or interface location the K1 classification (a borrow, by form) bounds the class instances the location may hold, never the runtime-managed value it refers to. A read through a location that has itself ended remains that holder's case: K4.1's flag or §15.1.1's disposed state in checked mode, undefined behaviour unchecked. The guarantee is about safety only. Whether and when a value no live location refers to is reclaimed stays the mechanism §28.9 leaves open.

**Repro.** Step 5's store makes the slot refer to the box, and the slot is live as long as `a` is. Step 6 ends only `ih`. `a[0].P(a[0])` reads a live box and prints `1`, in both modes. §28.11's residual still covers a class instance passed through `f`, and the class twin still faults (`c6.bzl`, checked).

**§1.3 shapes.** All six print. The `scoped` case is what §28.14 already says: the box has no source and freezes nothing, because the bounding object's field keeps it.

**Spec statements.**

- "An immutable shared reference" (§28.9) becomes a stated guarantee.
- §8.6's "releasing the last reference" sentence becomes a special case: the call's receiver is a live location.
- §15.1.1's released box behind a borrowing-match binding is unchanged. That binding holds the array, not the box, so it reads through a location whose holder's pass has ended it, and the array's disposed state faults the read before it reaches the slot.
- §26.7's #8113 carve is unchanged and still keys on the declared head: an extension over `Object` or an interface still may not store `this`, because the receiver may be a class instance.
- §9.7's and §26.7's "unbounded lifetime by construction" is the same statement at a runtime-managed declared type.

**Soundness.** A closes the hazard for runtime-managed values at live locations; nothing needs a flag. The class half is untouched: K4.1, K4.3's recorded clauses and the trust-admission report still stand behind every class instance an `Object` or interface location holds.

MEASURED by the critic (`cyc.bzl`, `cyc2.bzl`): a box can reach itself. A function-typed component does not make a record borrow-bearing (§9.7), and a closure whose captures carry no borrow has an empty source set, so it is admitted at a record component that is then boxed. A capture cell that later refers to the box closes the cycle, either directly (`cyc.bzl`: box → closure → `cell: Object?` → box, prints `400`) or through a class instance's `Object` field (`cyc2.bzl`: box → closure → cell holding a static `Hub` → `Hub.keep` → box, prints `2`). The cycle is of the kind §27.5's recursive closure (`fact := lambda …`) already forms. No class instance is counted in it, so it extends no class's lifetime (§28.2's compiler-inserted free still runs at the class's release point). Under a counting mechanism only its runtime-managed members leak. A safety-only guarantee permits that (Q3), as rationale §5.21's "accepted hazard" accepts for `Shared<T>` cycles (Q6).

**Ergonomics.** Every shape in §1.3 means what it looks like. No new spelling, diagnostic or remedy is needed.

**Implementation cost.** Read from `src/`, read-only; counts MEASURED by `git grep` at `cdd2022c7`.

- **Class fields hold boxes only where the per-module pre-pass sees a box written into them.** The pre-pass is `collectBoxStorageFields`. Box-only fields retain unconditionally (#2500). Mixed fields test the occupant at run time (#8215/#8118, `emitObjectSlotBoxOp`, 14 call sites). Function values at `Object` fields are handled by #9061. Some fields take no hold today: a field written only from an `Object` or interface parameter, from a source of unknown type, or with an `Object`-typed initializer; an `owned` field; a field of an imported class. The pre-pass comment says these "keep the old (excluded, leaking) verdict". Their boxes survive only because the caller's `BoxEscapeWalker` treats the call as an escape and leaks the box. This covers the lane's `field.bzl` (`Keep(x: IHx) this.o := x`) and s17_4_0901's `s.saved`.
- **Locals and arguments bound from such reads pin the box** (`box_borrow_pin_8444`; corpus s28_9_4609).
- **The borrowing collections at an `Object` or interface element or key type take no hold** (this is the repro). The element-slot factory `elementPolicyFor` is one central function; 129 lines in `src/codegen` contain `elementPolicyFor(`. It needs a dynamic hold arm: the store retains a runtime-managed occupant, and every removing member releases it or hands it to its result (§6.7(a)). The runtime's free routines already take an element thunk for the owning family, and the discriminator needs a C-callable form for them.
- **The escape walk becomes an optimization.** `BoxEscapeWalker` has 22 references and `boxReleaseEligible_` 27. Once every keeping location holds its own reference, the boxing local can release at its scope end, and the escape-means-leak path stops being a safety device. That retires #9206's leaks. This step is safe only after the previous three are complete (§9's ordering).
- **Run-time cost, in every build:**
  - one occupant test per store into and release of an `Object` or interface location whose producer is not statically known, which class instances at those locations pay too;
  - a conditional retain where a local or argument is bound from a read of such a field (the 8444 family);
  - for `Clear()` and `Dispose()` of a borrowing collection at an `Object` or interface element type, a per-element walk, unless a per-collection flag records that no runtime-managed occupant was ever stored.

  Strings already pay a count at every store, and rationale §4.1 records elision as implementation latitude.

Estimate: three implementation lanes — fields made universal, collections, then the escape walk's retirement. Each touches allocation, so each runs the ASan leg (CLAUDE.md).

**Migration.** No verdict changes; the corpus already expects this (§8).

- **The 9206 test file.** Its two `expectMemorySafe` rows, `spec_28_11_box_a_function_value_stores_in_a_collection_stays_alive` and `spec_28_3_8_box_a_function_value_stores_in_a_field_stays_alive`, run leak-lenient "while #9219 is open". They strengthen to `expectClean` with the implementation, and the comments that cite #9219 as open are rewritten.
- **Four `src/` comments cite #9219 as open:** `LlvmBackend_Assign.cpp` near lines 22219 and 22243, and `LlvmBackend_Setup.cpp` near 54885 and 54912. They are rewritten with the implementation.
- **The leak-floor rows** `set_object_element_4642`'s `tryadd_contains_box_residual_floor_spec_8_6` and `dict_object_bare_value_key_indexer_set_4640`'s `dict_object_bare_int_key_indexer_set_leak_floor_spec_8_6` pin representation residuals the guarantee does not address: a safety rule forbids no leak. They flip only if the implementation retires those residuals, and the fix lane re-points them then.

### 2.2 Option B — a checked-mode backstop for boxes (the issue's reading 3)

**Rule.** A box cell carries a released flag; its count word can hold a sentinel. In checked mode, reclamation of a released box is deferred, as for a disposed class (§28.2's quarantine latitude). A member access through a reference to a released box raises `DisposedAccessException`, as do an unboxing `as`, a narrowed read and a typed-pattern binding over it.

**What it needs first.** A flag faults only after a *release*, so B must say when a box is released. The text gives a box "no classification, no obligation, no source" (§28.3.4), so B has to add a lifetime model. There are two candidates:

- **B1:** only the location the boxing conversion initialized (plus an owned slot, per §15.1.1) keeps the box.
- **B2:** every location keeps it except a borrow slot and a parameter.

**Repro.** Under B1 and B2 the box dies at `ih`'s scope end. `a[0].P(a[0])` raises `DisposedAccessException` in checked mode and is undefined behaviour unchecked.

**§1.3 shapes.** Under either variant, `a.Add(i)` at `Array<Object>` faults on the first read: the box is minted for the call, and only the slot ever holds it. Under B2 the `Array<Object>` literal does too, so `StringBuilder.AppendFormat`'s documented argument shape (stdlib §3.1) cannot be built. Under B1 the literal's element is the location its boxing initializes, so the literal and `s17_5_0015` survive. But B1 faults the field rows and §28.14's `scoped` admission, which contradicts §28.14's "freezes nothing" ruling (#5559) and flips s17_4_0901–0904 and s28_9_4609 from `run` to a fault. B2 keeps those cases, but it is A with a hole cut at the borrow slot, chosen by the table's label alone.

**Soundness.** Sound in checked mode for the shapes it rejects at run time. Unchecked builds keep undefined behaviour for programs with no visible kill.

**Ergonomics.** Poor. The remedy for a fault would be "keep the box in a location that keeps it", a distinction no source spelling shows.

**Cost.** A flag test on every box-read path: box dispatch thunks, `as`, narrowing, patterns, and `Object`'s `Equals`/`GetHashCode`/`ToString`. Quarantine of released boxes in the default build unless escape-proven, as `bz_cd_may_free` does for classes; boxes are minted at every boxing conversion. And the holder-set rule, which the compiler must implement anyway.

**Migration.** It flips the corpus rows above (B1) or the borrowing-collection rows (B2), and it rewrites the 9206 test file's two stays-alive rows.

### 2.3 Option C — narrow the two sentences to class instances and record the box residual

**Rule.** The two sentences say "for a class instance". A new K4.3 clause records that a runtime-managed value read through a trust-admitted location after its holder's release has no backstop, as the domain clause records a race. C needs the same release model as B, or else it leaves the release point unspecified.

**Repro.** Undefined in both modes: a use-after-free in the default build in code that never names a disposal.

**§1.3 shapes.** The trust clause does not reach most of them, because no trust admission is made (a box is no borrow). C must either extend the report to every store of an `Object` or interface value whose occupant could be a box, which is every such store, or leave those stores outside any clause. The second breaks K4.3's "no new admission may be made without recording its clause or its backstop".

**Soundness.** The weakest option. It turns §28.2's posture (checked mode is the default, unchecked its relaxation) into undefined behaviour in the default build.

**Ergonomics.** As B, without the fault.

**Cost.** None in the compiler: today's leak is conforming, and so would #9206's first fix have been.

**Migration.** The corpus rows of §2.2 lose their defined output and are deleted.

A variant, C1, narrows the sentences and says nothing more. It is not a different option: it leaves the question where it is today, with the sentences now merely silent.

### 2.4 Option D — reject the store (the issue's reading 2)

The store site cannot see that the source is a box: in the repro it is a plain parameter, and in §1.3 an `Object`-typed value of unknown provenance. Rejecting every store of a plain `Object` or interface parameter is the callee-side no-store rule that `DECLINED.md` records as rejected. Accepted proposal 0212 reverses that row for members that are not constructors at its stage 3 (0212 §4.3), and that stage will reject the repro's storing lambda for class and box arguments alike. But no stage of 0212 rejects the caller-side shapes of §1.3: `a.Add(ih)`, `a.Add(i)`, the literal, a property store, a constructor store, a `scoped` argument.

Rejecting those would need the run-time kind of the value. `DECLINED.md`'s owning-inline-array slot row (#5554) declines that: "which value a slot holds is a per-slot runtime fact no static rule sees". Rejecting at the caller from the callee's body is what `DECLINED.md`'s K8.4 row declines. D is not viable.

### 2.5 Option E — leave the text as is (decline)

The two sentences stay false for boxes, and the §1.3 shapes stay without a stated guarantee. Implementations stay free to leak, as `origin/main` does wherever its escape walk sees a storing or opaque callee: memory-safe, but unbounded (the 9206 file measured 24 B per boxing). The issue stays open. E is viable only as a deferral.

## 3. Recommendation

Adopt option A, scoped to every runtime-managed value at an `Object` or interface location: a box, and a `string`, `Type` or function value converted to `Object` or to an interface type it implements. The reasons:

1. **It is the only option under which every shape the text already admits has a meaning** (§1.3). B and C turn `a.Add(i)` at `Array<Object>` into a fault or undefined behaviour.
2. **It is what the text already leans on, and states at runtime-managed declared types** (§1.4: §9.7, §26.7, §28.11's copy-out cell). The corpus already expects it: s17_4_0901–0904 pass, and s28_9_4609 is written to it. B1 and C would flip or delete those cases.
3. **It is partly implemented.** Some class fields, the locals and the pins hold boxes today. The missing pieces are the borrowing collections and every `Object` or interface field the pre-pass excludes.
4. **Its cost is the one strings already pay, at the locations where a box can appear.** It adds no ownership kind, because K1.6 keeps runtime-managed values outside the classification, and it counts no class instance (§4, Q4).
5. **It composes with proposal 0212's stage 3** (§5).

The generalization to strings, `Type` and function values is free. `string` implements `IEnumerable<char>`, `IComparable<string>` and `IEquatable<string>` (stdlib §9.6; spec §4.6), so an `Array<IComparable<string>>` holds strings at interface-typed borrow slots. The #8118 discriminator already tells those occupants apart at `Object` slots (#9061 is the function-value fix). A rule for boxes alone would leave a string at an `Object` or interface field under the same false sentence (Q2). The implementation must confirm that the discriminator recognizes strings at interface-typed slots too.

## 4. Records this proposal answers

A change is a design change the moment it contradicts a recorded decision (process §2). This one contradicts none:

- **Rationale §5.4's declines** — implicit handle duplication, reference counting arbitrary classes through an annotation, compiler-inferred sharing by escape analysis, and "sharing as a third reference kind beside owned and borrowed". These are declines for class instances and `Shared<T>` handles. A keeps no class instance alive and adds no K1 kind: runtime-managed values are already outside K1 (K1.6), and their reclamation is "the runtime's business rather than the program's" (rationale §4).
- **Rationale §5.28.1**, the owner's structural objection to "a full reference-counting implementation — cycle policy, atomic counts at domain crossings, … — … a hidden per-type runtime" for trivial-dispose classes. A counts no class instance. Its run-time cost is an occupant test at `Object` and interface locations, which class instances at those locations bear too (§2.1). Its cycles are runtime-managed only (§2.1), and it adds cross-domain box sharing to the bookkeeping §30.14 already asks to tolerate for strings (§6.8). These are the costs §5.28.1 names. A buys them for a safety guarantee over values the language mints implicitly, not for ceremony, but the posture is the owner's to weigh: Q4 asks for that judgement explicitly.
- **`DECLINED.md`, the K4.3 clause for a boxed value that holds borrows, and the borrow-erasing-position rows (#5645).** Untouched: a value that holds borrows still reaches no `Object` or interface position but a call receiver, so no such box is ever stored.
- **`DECLINED.md`, the callee-side no-store rule, and the owning-inline-array slot row (#5554).** The first is reversed for members that are not constructors at 0212's stage 3, which then rejects the repro's storing lambda. The second stands. Option D's grounds stand for the caller-side shapes of §1.3, which no stage of 0212 rejects (§2.4).
- **#8113 (v0.361).** Kept, keyed on the declared head as triage required. A does not relax §26.7 for an `Object` or interface head.
- **#5559 (§28.14, a box at a `scoped` parameter).** A is the reading under which that ruling is sound.

## 5. Interaction with proposal 0212

Proposal 0212 is accepted; stage 1 landed at v0.357, and stages 2–3 are scheduled. Stage 3 replaces the field-store bullet and *Storing a borrow* wholesale (0212 §8.5, §8.6, "No residual"). It also makes a plain parameter call-bounded for an object borrow, so that only a constructor or a `scoped` parameter may keep one. Three consequences:

- **The repro's shape becomes a compile error at stage 3.** A lambda keeping its plain parameter falls under §28.15's callee-side rule, whatever the argument. Corpus cases `s28_9_2161` and its class control `s28_11_2163` (§8) flip then; the other cases do not use that shape.
- **0212 relies on A.** It reads what an argument contributes from the argument, and a box contributes no source. Its mint, `scoped` and later-store rules therefore admit a box into a constructor-built field, a `scoped` field or a collection with nothing bounding it. That is sound only under option A. Under B or C, stage 3 would keep a box hole outside every model 0212 measured (0212's model has no runtime-managed values). A is therefore a prerequisite of 0212's stage-3 claim at `Object` and interface positions (Q5).
- **Which of 0216's edits survive stage 3.** The edits in §6 land before stage 3. They survive it, except §6.5's sentence and §6.7(b)'s clause, which live in the clauses stage 3 replaces. 0212's stage-3 train should restate §6.5's runtime-managed sentence in its replacement field-store bullet (clause 3, "No residual") or cite §28.9 *Held while referenced* there (Q5).

## 6. The normative edits

Every *old* string below occurs exactly once in the v0.361 text at `origin/main` `cdd2022c7` (MEASURED, `grep -c -F`).

### 6.1 Spec §28.3.1, K1.6

Old:

> Values of runtime-managed type (`string`, `Type`, boxed value types, function values) and of ordinary value type carry no classification and no obligation; `owned` markers over them are inadmissible or erased per instantiation ([§28.1](#sec-where-the-owned-modifier-can-appear)).

New:

> Values of runtime-managed type (`string`, `Type`, boxed value types, function values) and of ordinary value type carry no classification and no obligation; `owned` markers over them are inadmissible or erased per instantiation ([§28.1](#sec-where-the-owned-modifier-can-appear)). A runtime-managed value is not reclaimed while any live location refers to it, whatever that location's declared type and classification ([§28.9](#sec-runtime-managed-types) *Held while referenced*): no source bounds it, and no rule of this chapter needs to.

### 6.2 Spec §28.3.1, K4.1

Old:

> This is the runtime backstop every trust admission names.

New:

> This is the runtime backstop every trust admission names for a class instance; a runtime-managed value a live trust-admitted location refers to needs none (K1.6).

### 6.3 Spec §28.2, *The compiler-inserted free*

Old:

> no class in the language dangles without this backstop.

New:

> no class in the language dangles without this backstop, and no runtime-managed value is reclaimed while a live location refers to it ([§28.9](#sec-runtime-managed-types) *Held while referenced*).

### 6.4 Spec §28.3.4, the runtime-managed producers row

This is a table cell; the new text contains no `|`.

Old:

> A parameter, field, capture, getter or indexer result whose static type is `Object` or an interface is classified by its form — a borrow — whatever it holds at run time.

New:

> A parameter, field, capture, getter or indexer result whose static type is `Object` or an interface is classified by its form — a borrow — whatever it holds at run time: the classification bounds the class instances such a position may hold, and a runtime-managed value it refers to is not reclaimed while the position is live ([§28.9](#sec-runtime-managed-types) *Held while referenced*).

### 6.5 Spec §28.3.8, field-store bullet, clause 3

Old:

> Neither source is lifetime-extended: the temporary is disposed at its [K8](#sec-kernel-k8) boundary and the local at its scope's end, and a member access through the stored borrow after that point is the backstop's fault; a temporary meets the obligation only when the object dies within the same full expression.

New:

> Neither source is lifetime-extended: the temporary is disposed at its [K8](#sec-kernel-k8) boundary and the local at its scope's end, and a member access through the stored borrow after that point is the backstop's fault; a temporary meets the obligation only when the object dies within the same full expression. A runtime-managed argument — a box ([§8.6](#sec-boxing)), or a `string`, `Type` or function value converted to `Object` or to an interface type it implements — is neither: it carries no classification ([K1](#sec-kernel-k1), K1.6), the location the callee stores it in keeps it from reclamation ([§28.9](#sec-runtime-managed-types) *Held while referenced*), and no access through that location, while the location itself is live, meets a reclaimed value, in either disposal mode.

### 6.6 Spec §28.9

(a) The boxes bullet. Old:

> so a boxed value is an immutable shared reference.

New:

> so a boxed value is an immutable shared reference, not reclaimed while any live location refers to it (*Held while referenced*, below).

(b) A new paragraph, inserted immediately before the paragraph that begins "This tier is exhaustive: no runtime lifetime mechanism exists for user-defined classes at all":

> <a id="sec-held-while-referenced"></a>**Held while referenced.** Whatever the mechanism, a runtime-managed value is not reclaimed while any live location refers to it — a local in scope, a parameter of a running activation, a field of an instance not yet disposed, a slot of a collection of either family or of an inline array whose holder has not released it, a capture, a frame-resident binding, a temporary before its [K8](#sec-kernel-k8) boundary, or a static — whatever that location's declared type. Where the declared type is itself runtime-managed this is the type's own contract ([§9.7](#sec-records-and-disposal): such values "have unbounded lifetime by construction"; [§26.7](#sec-ownership-in-extensions): "the stored value living as long as its holder"). Where it is `Object` or an interface type — a box ([§8.6](#sec-boxing)), or a `string`, `Type` or function value converted to `Object` or to an interface type it implements — the location is classified by its form, a borrow ([§28.3](#sec-producers)'s runtime-managed row), and that classification bounds the class instances the location may hold instead, never the runtime-managed value it refers to: a box stored through a plain parameter into a field or a borrowing-collection slot ([§28.3](#sec-borrow-lifetime-rules)'s field-store bullet, [§28.11](#sec-borrowing-collections)'s store rule) lives while that field or slot is live and refers to it, a local bound from a read of such a location keeps what it read, and no read through a live location meets a reclaimed value, in either disposal mode. A read through a location that has itself ended — a field of a disposed instance, a slot of an owning inline array its holder's pass has released ([§15.1.1](#sec-owning-inline-arrays)) — is that holder's case: [K4.1](#sec-kernel-k4)'s flag or the array's disposed state in checked mode, undefined behaviour in unchecked mode ([§28.2](#sec-checked-disposal)). A runtime-managed value is therefore never the subject of a trust admission's residual, and K4.1's disposed flag, which it does not have ([§8.7](#sec-dispose-is-not-on-object)), is not needed for it. The guarantee is about safety only: whether and when an implementation reclaims a value no live location refers to is the mechanism this section leaves open, and the members of a reference cycle among runtime-managed values — a box whose payload's function value captures a variable that refers to the box ([§27.5](#sec-closures)) — may never be reclaimed. *Implementation note (non-normative): a location declared `Object` or an interface type cannot tell a box from a class instance statically, so an implementation that counts references tests the occupant at run time at each store into and release of such a location, and may omit the test where the stored value's producer is statically known, and a collection that has never stored a runtime-managed occupant need walk no slot at its release; unchecked builds pay that test, the price of the guarantee.*

### 6.7 Spec §28.11

(a) A new paragraph, inserted immediately after the operations table (before "The per-collection clauses in the remainder of this section"):

> A borrow slot whose occupant is **runtime-managed** — a box ([§8.6](#sec-boxing)), or a `string`, `Type` or function value at an `Object` or interface element type; which kind of value a slot holds is a per-slot run-time fact, as [§15.1.1](#sec-owning-inline-arrays)'s slot pass reads it — holds that occupant as a shared-reference slot does: the store holds it; every member that removes the occupant without returning it — `Remove`/`RemoveAt`, `RemoveFirst`/`RemoveLast`, `Clear()`, an overwriting store and the collection's `Dispose()` among them — releases that hold, and a member that removes the occupant and returns it — `Pop`, `Dequeue` and their `Try*` forms — hands the hold to its result or `out` location; none of this is user-visible work ([§28.9](#sec-runtime-managed-types) *Held while referenced*). A class instance in the same slot is a borrow, as the table states.

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

- **Stdlib.** Its collection clauses are presentations of §28.11's table ("a per-collection statement that diverges from the table is a defect"), so §6.7 covers them, `Stack<T>.Pop`, `Queue<T>.Dequeue` and `LinkedList<T>.RemoveFirst`/`RemoveLast` among them. No signature changes.
- **Code-generation reference.** Its §4 rules state the class-instance obligations and impose none on boxes; the guarantee removes a hazard rather than adding a rule a writer must follow. Its maintenance rule admits a new clause only as a table row or a *looks right, is rejected* pair, and A has no rejected shape. Its pin moves with the revision as usual.
- **Appendix C.** The guarantee is a run-time property, not a compile-time invariant. C.3's `scoped` bullet stays true.
- **§8.6, §9.7, §15.1.1, §26.7, §28.14.** Each is consistent with A as written (§2.1).

### 6.10 Rationale §4

Insert after the paragraph "Boxed values and `Type` tokens are runtime-managed on the same footing: …":

> **A runtime-managed value is not reclaimed while any live location refers to it, whatever that location's declared type ([#9219](https://github.com/s8lap/bezel-lang/issues/9219); proposal [0216](proposals/0216-runtime-managed-values-held-where-stored.md)).** A box reaches storage through a location declared `Object` or an interface type, which the ownership rules classify as a borrow because it may hold a class instance. Until v0.36N (the landing revision) the spec stated the holder rule only at runtime-managed declared types (§9.7, §26.7), and two sentences named K4.1's disposed flag as the safety net for a store through an `Object` or interface location, a flag a box does not have. The plainest shapes — `a.Add(ih)` for an interface-typed local holding a box, `a.Add(i)` into an `Array<Object>`, a collection literal of `Object` elements, a box at a `scoped` parameter — are admitted with no trust clause at all, because a box carries no source. Only a hold by the storing location makes those admissions sound. It is the posture §4.1 already states for strings: reference counting reclaims such a value when its last reference goes. The price is a run-time occupant test at stores into and releases of `Object`- and interface-typed locations, in unchecked builds too, borne by class instances at those locations as well, which an implementation omits where the producer is known. No class instance is ever kept alive this way and no ownership kind is added (K1.6 keeps runtime-managed values outside the classification), so [§5.4](#sec-no-shared-ownership)'s declines of implicit sharing stand. A box can reach itself through a function value's capture cell, as a recursive closure does; under counting such a cycle leaks, which the safety-only guarantee permits, as [§5.21](#sec-shared-t) accepts for `Shared<T>`. Declined: a released flag on boxes with a checked-mode fault (it needs a release point for a value the kernel gives none, faults `a.Add(i)` at `Array<Object>` on its first read, and quarantines boxes in every default build); narrowing the two sentences and recording a box residual with no backstop (a use-after-free in the default build in programs that read as value code); rejecting the store (which value a location holds is a run-time fact no static rule sees, the ground of the v0.327 inline-array-slot decline).

### 6.11 Guide 01, §3 (the *stashed reference* paragraph)

After "… as a `DisposedAccessException` in the default build." insert:

> A value boxed at an interface type — a record or an enum stashed in an interface-typed field or collection — is not such a target: a box is runtime-managed, and the field or slot that refers to it keeps it alive (spec §28.9).

## 7. Changelog sketch

> **#9219, a runtime-managed value is not reclaimed while a live location refers to it** (design change; proposal [0216](proposals/0216-runtime-managed-values-held-where-stored.md); spec K1.6, K4.1, §28.2, §28.3.4, §28.3.8 clause 3, §28.9 *Held while referenced* (new), §28.11 (one paragraph, one clause), §30.14 (note); rationale §4; Guide 01 §3).
> - *Decided.* A runtime-managed value is not reclaimed while any live location refers to it, whatever the location's declared type. This covers a box, and a `string`, `Type` or function value converted to `Object` or to an interface type it implements. The borrow classification of an `Object` or interface location bounds the class instances it may hold. A read through a location that has itself ended stays that holder's case. The two sentences that named K4.1's flag as the safety net for a trust-admitted store now say so for a class instance; a box needs none. The guarantee is safety only: cycles among runtime-managed values may leak.
> - *Declined.* A checked-mode released flag on boxes: it needs a box release point and faults `a.Add(i)` at `Array<Object>`. Narrowing the sentences with a box residual recorded with no backstop: undefined behaviour in the default build. Rejecting the store: a per-slot run-time fact, as the v0.327 inline-array-slot decline records.
> - **Migration.**
>   - No program changes verdict. A program that reads a box back through a live field, a borrowing-collection slot or a local after the box's first holder is gone had no stated meaning; it now has the one it reads as having.
>   - Implementations: every `Object` or interface field holds its runtime-managed occupant, imported classes' fields and generic fields at such a substitution included. A borrowing collection at an `Object` or interface element or key type holds its runtime-managed occupants: it releases them at every removing member and hands them to `Pop`/`Dequeue`'s result. Only then may a boxing local release its own reference at its scope end whatever the callee.
>   - Tests: the 9206 file's two stays-alive rows strengthen to `expectClean` with that implementation, and the four `src/` comments that cite #9219 as open are rewritten.
>   - Rule corpus: seven cases added (§8), none flipped. `s28_9_2161` and `s28_11_2163` flip to `reject` at proposal 0212's stage 3.

## 8. Rule corpus (process §5 item 3c)

New cases. The ids are proposed and are free on `origin/main` `cdd2022c7`. Each verdict comes from §6.6's sentence, not from the compiler. Each case was probed on the probe build for syntax and output.

| Case | Section | Program | Expect |
|---|---|---|---|
| `s28_9_2161` | 28.9 | the issue's repro (§1.2) | `run`, output `200` / `1`. Header note: flips to `reject` at proposal 0212's stage 3 (the lambda keeps a plain parameter) |
| `s28_9_2162` | 28.9 | `var ih: IHx := new Rx(i); a.Add(ih);` over 200 iterations, `a` declared before the loop; sum `e.V()` after | `run`, stdout `19900` |
| `s28_9_2163` | 28.9 | `h.Keep := ih;` each iteration, with `using var h` declared before the loop and a plain `Keep: IHx?` property; print `h.Keep!.V()` after | `run`, stdout `199` |
| `s28_9_2164` | 28.9 | `var s: Object := $"v{i}"; b.Add(s);` into `b: Array<Object>`; print `b[199]` after | `run`, stdout `v199` |
| `s28_11_2161` | 28.11 | `a.Add(i)` into `a: Array<Object>` over 200 iterations; sum `(o as int) ?? 0` | `run`, stdout `19900` |
| `s28_11_2162` | 28.11 | a local `var first := a[0];` over boxed elements, then `a.Clear();`, then `first.V() + 1` | `run`, stdout `1` |
| `s28_11_2163` | 28.11 | control, the class half unchanged: the class twin of `s28_9_2161` (`using var c := new Cx(i)` passed to the same lambda; the read is in `try` with a `DisposedAccessException` arm that prints `faulted`); header `//! flags: --checked-disposal`, as the 28 existing checked-fault cases carry. Header note: flips to `reject` at proposal 0212's stage 3 | `run`, stdout `faulted` |

Existing cases A keeps, with no edit: s17_4_0901–0904, s28_9_4609, s28_9_9601, s17_5_0015. Under B1 or C, s17_4_0901–0904 and s28_9_4609 would flip or be deleted (§2.2–§2.3). s28_9_9601 is neutral: its box is held by its own local for the whole call.

The 9206 lane's parked candidate `9206-opaque-callee.bzl` is #9206's `ih.P(ih)` program, whose callee keeps nothing. Its "no leak" expectation follows implementation step 4 (§9). The corpus has no leak leg, so it cannot state that expectation.

## 9. Implementation sketch (non-normative)

The steps are ordered. Step 4 is safe only after steps 1–3, and that ordering is the acceptance condition for retiring the #9206 leak: **no release at a boxing local's scope end until every `Object` or interface holder holds.**

1. **Every `Object` or interface field.** Give the dynamic hold arm (the #8118 discriminator, `emitObjectSlotBoxOp`) to every such field unconditionally, not only to fields where `collectBoxStorageFields` sees a box written. That includes fields written from parameters or sources of unknown type, fields with an `Object`-typed initializer, imported classes' fields, generic fields at an `Object` or interface substitution, and statics. No holder may depend on the caller's leak.
2. **The borrowing collections at `Object` or interface element or key types.** Add a dynamic hold arm to `elementPolicyFor`'s class/interface × borrowing case. The store retains a runtime-managed occupant. Every removing member releases it, and `Pop`/`Dequeue`/`TryPop`/`TryDequeue` hand it to the result or `out` location (§6.7(a)). Use the discriminator in a C-callable form so the runtime's free routines can call it as they call owned-element thunks. Optionally keep a per-collection "may hold a runtime-managed occupant" flag, so that `Clear()` and `Dispose()` skip the walk for a collection that never stored one. This covers `Array`, `Dictionary` keys and values, `Set`, `LinkedList`, `Stack` and `Queue`. Confirm that the discriminator recognizes strings at interface-typed slots (`IEnumerable<char>`, `IComparable<string>`, `IEquatable<string>`).
3. **Audit the remaining holders:** capture cells (the runtime header already describes +1-retained box captures), async and iterator frames, and locals and arguments bound from `Object` or interface reads (the 8444 pins).
4. **Release at scope end.** Once steps 1–3 hold, a boxing local releases at its scope end unconditionally. Retire `BoxEscapeWalker`'s escape-keeps-it-alive path, or keep it only as an optimization. #9206's leaks go with it.
5. **Tests.** Strengthen the 9206 rows (§2.1), rewrite the four `src/` comments, and add an ASan leg over the §8 shapes looped 200×, per CLAUDE.md's allocation rule.

## 10. Owner decisions (2026-11-26)

On 2026-11-26 the owner accepted this proposal with every recommended answer ("I accept all"). The decisions are recorded below; the questions follow as they were put, with their reasoning.

- **Q1 — A.** A live location keeps the runtime-managed value it refers to (§2.1, §3). B, C and D are declined (the `DECLINED.md` rows below, now indexed).
- **Q2 — every runtime-managed value** at an `Object` or interface location: a box, and a `string`, `Type` or function value converted to `Object` or to an interface type it implements.
- **Q3 — safety only.** *Held while referenced* states no reclamation obligation; the leaks stay quality defects.
- **Q4 — yes.** The cost is accepted in every build, unchecked included, with the representation (a header bit, a tagged pointer, the flag) left to the implementation. This is the owner's answer against rationale §5.28.1.
- **Q5 — yes.** (a)–(c) are recorded in proposal 0212's stage-3 edits ([0212 §8.15](0212-borrow-bearing-classes.md)).
- **Q6 — the cycle leak is accepted** as outside the guarantee; no cycle rule is required.

**The questions, as asked.**

- **Q1. Which option?** Recommended: A (§3). B and C are recorded as declined (§6.10's rationale text; the `DECLINED.md` rows below, indexed only on acceptance under the 2026-08-20 scope rule).
- **Q2. Scope.** Boxes only, or every runtime-managed value at an `Object` or interface location: boxes, and `string`, `Type` and function values converted to `Object` or to an interface type they implement (`string` at `IEnumerable<char>`, `IComparable<string>`, `IEquatable<string>`)? Recommended: every one. It is the same discriminator (#8118, #9061), and boxes alone would leave a string at an `Object` or interface field under the same false sentence.
- **Q3. Safety only, or reclamation too?** Should *Held while referenced* also say that a value no live location refers to is reclaimed? That would make today's escape-walk leak, and any cycle (Q6), nonconforming. Recommended: safety only. §28.9 keeps the mechanism open, and the leaks are quality defects to file, as #9206 already is.
- **Q4. The cost, against rationale §5.28.1.** In every build, unchecked included, A costs:
  - an occupant test at each store into and release of an `Object` or interface location, borne by class instances there too;
  - a conditional retain where a local or argument is bound from a read of such a field;
  - a per-element walk at `Clear()` and `Dispose()` of a borrowing collection at such an element type, unless a per-collection "never stored a runtime-managed occupant" flag lets it skip the walk.

  §5.28.1 records the owner's objection to "a hidden per-type runtime" with "cycle policy, atomic counts at domain crossings". A counts no class, but it does carry a hidden test on class-instance paths through interfaces, a cycle hazard (Q6) and cross-domain box bookkeeping (§6.8). Accept that price for the guarantee? Recommended: yes, with the representation (a header bit, a tagged pointer, the flag) left to the implementation.
- **Q5. Proposal 0212's stage 3.** Record the following in 0212's stage-3 edit list? Recommended: yes.
  - (a) A box contributes no source because its storing location keeps it (§5).
  - (b) `s28_9_2161` and `s28_11_2163` flip to `reject` then.
  - (c) The replacement field-store bullet (clause 3, "No residual") and the replacement *Storing a borrow* restate §6.5's runtime-managed sentence and §6.7(b)'s clause, or cite §28.9 *Held while referenced*, since stage 3 deletes the clauses that carry them.
- **Q6. Cycles.** A box can reach itself through a function value's capture cell (MEASURED, §2.1: `cyc.bzl`, `cyc2.bzl`). Under counting the cycle leaks, as a recursive closure's does today (§27.5). Should that leak be accepted as outside the guarantee (recommended), or should the spec require a cycle rule? A cycle rule would need the "small collector" that rationale §4 permits and rationale §1's "nothing runs in the background" resists.

**`DECLINED.md` rows (indexed at acceptance, 2026-11-26):**

| Item | Class | Where recorded | Reason / reopening bar |
|---|---|---|---|
| A released flag on boxed values with a checked-mode `DisposedAccessException` for an access through a reference to a released box (#9219 reading 3) | `declined-at-proposal` | proposal 0216 §2.2; rationale §4 | It needs a release point for a value K1.6 gives none. Under any release model short of a hold by every live location, `a.Add(i)` at `Array<Object>` faults on its first read, and under B2 the `Array<Object>` literal does too. It also quarantines boxes in every default build. Reopening bar: a measured unchecked-build cost of the occupant test that the owner judges excessive, with a release model under which `a.Add(i)` at `Array<Object>` does not fault. |
| Narrowing §28.3.8 clause 3 and §28.11's residual to class instances and recording a box residual with no backstop, as the domain clause is recorded | `declined-at-proposal` | proposal 0216 §2.3; rationale §4 | A use-after-free in the default build in programs that read as value code, most of them outside any trust admission (a box is no borrow), against K4.3's "no new admission … without recording its clause or its backstop". Reopening bar: —(none stated). |
| Rejecting the store of a box into a field or a borrowing-collection slot (#9219 reading 2) | `declined-at-proposal` | proposal 0216 §2.4 | Which value a location holds is a per-slot run-time fact no static rule sees (the v0.327 inline-array-slot row). The callee-side form is the no-store rule, which 0212's stage 3 adopts for members that are not constructors but which reaches none of the caller-side shapes. Reopening bar: the inline-array-slot row's. |

## 11. Critique record

Critique round 1 (pc-0216, independent non-author, 2026-11-26) returned SOUND-WITH-FIXES with twelve findings, and all are folded in:

- **F1.** The guarantee is scoped to live locations (§2.1, §6.3, §6.5, §6.6(b)).
- **F2.** The false no-cycle argument is replaced by the measured cycles and the leak posture (§2.1, Q6).
- **F3.** The removing and returning members are added (§6.7(a)).
- **F4.** Strings at interface types are added (§3, §6, Q2).
- **F5.** The account of `1300d04de`, the parked candidate and the issue's measurement is corrected (§0, §1.2, §8).
- **F6.** The field pre-pass gap and the implementation ordering are added (§2.1, §3, §9).
- **F7.** The full cost and the rationale §5.28.1 answer are added (§2.1, §4, §6.6(b), Q4).
- **F8.** The stage-3 composition is corrected: `s28_11_2163`, the wholesale replacement, and the reversed `DECLINED.md` row (§2.4, §4, §5, Q5).
- **F9.** The v0.361 §26.7, #8113-changelog and §28.11 copy-out support is cited (§1.1, §1.4, §6.6(b)).
- **F10.** `s28_11_2163` gets its flags header, the c6 default-build SEGV is recorded, s28_9_9601 is marked neutral, and the B1 literal trace is corrected (§1.2, §2.2, §3, §8).
- **F11.** Row 1's reopening bar is rewritten.
- **F12.** The editorial fixes are applied: the K1.6 wording, "v0.36N", the method behind the counts, the `src/` comments, and the status.
