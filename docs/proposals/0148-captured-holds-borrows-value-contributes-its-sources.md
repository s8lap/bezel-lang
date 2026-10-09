# Proposal 0148 — A captured value that holds borrows contributes the sources of the borrows it carries

| | |
|---|---|
| **Status** | graduated-v0.331 — superseded — graduated v0.331 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option B, the carried-source count already used for the same value as an argument and at the consume-site freeze, with the two "contributes nothing" exemptions narrowed to values that carry no borrow; critique round 1 applied, 3 blocking and 8 non-blocking findings resolved, shared with the #5675 ruling; that round recorded the four sink-route test flips, counted a method reference bound to such a receiver, and closed the generic `as T` twin of #5675; landed with the v0.331 changelog entry, beside the composition check's statement of the count in stdlib §6.5, Guides 01 and 04 and K6's carried set) |
| **Tracking issue** | [#5677](https://github.com/s8lap/bezel-lang/issues/5677) (borrow-flow matrix root cause B4; `needs-owner-decision`, decided under the delegation); one `spec-tracking` compiler tracker is filed at landing (§6.4) |
| **Target module(s)** | **spec** — §9.7 (the closure-capture bullet, one clause), §28.3 (the closure bullet's provenance clauses 1 and 2 and its method-reference bullet, one sentence; *Deferred-activation callees*' capture-set contribution, two parentheses), §28.14 (*Class- and interface-typed `scoped` parameters*' captured-source sentence; *v1 admission bounds* item 1, two sentences), Appendix C.3 (one new bullet); **rationale** — §5.25 (one paragraph); **code-generation reference** — §7.2 (one *Looks right, is rejected* pair); **`DECLINED.md`** — three `rejected` rows |
| **Written against** | spec **v0.330** at `1ec0886` (main). The stdlib companion, rationale and code-generation reference are at the same commit. |
| **Origin** | The v0.330 borrow-flow matrix audit (`scratch/matrix/`, root cause B4 = X2's RC1): fifteen cells — R3–R6 × C6, R3–R6 × C9, R3–R5 × C10, R3–R6 × C13 — are open because a closure's source set, read through provenance clause 1, omits a captured local or parameter of a value type that holds borrows, and two passages say such a capture "contributes nothing". |

*Reference convention: unprefixed §-references are language-spec references at `1ec0886` (v0.330); K tags are §28.3's kernel ids; rationale, stdlib, code-generation-reference and proposal references are marked. Quotations are verbatim. ▲ marks proposed text, landed nowhere. The programs below use only members the stdlib companion declares (`ScopeGuard` is stdlib §2.21, spec-ahead-of-implementation #4669); none invents an API.*

---

## 0. Thesis

A closure is "a **borrow-carrying value**", and "whether it may outlive a source is decided by its **source set**, read off the value's provenance" (§28.3, the closure bullet). Provenance clause 1 lists what a lambda literal contributes: "a captured borrowed local or parameter that binding's source; a captured owned local its enclosing scope …; a captured `ref` or `out` parameter … the declaring body's scope as well; a captured function value its own set, transitively". A captured local of a value type that holds borrows — a span, `record Pair(Item: Item)`, `[2]Item`, a choice holding a class payload — is in no item. Two passages then say what the silence means: *Deferred-activation callees* counts "a value-typed capture contributes nothing, living in its heap capture cell", and §28.14's *v1 admission bounds* item 1 exempts "a captured value-typed local or parameter" at both admission arms, its closed list of tested captures naming "class- and interface-typed ones, `owned` bindings, plain function-typed parameters … and `ref` and `out` parameters".

The same value, passed **directly**, is counted: "an argument of a value type that holds borrows … contributes the sources of the borrows it carries" (*The sources an argument contributes*, item 3), and a live local of such a type "counts as a live local borrow of every held borrow's source" at the consume-site freeze (§28.3, "stated explicitly, because stdlib §4.19's construction-time soundness is load-bearing on it"). Two recorded declines presuppose the captured count too: `DECLINED.md` row 129 rejects an interface-typed `scoped` admission "its carried sources tested and frozen as a closure's captures are", and row 181 keeps the invocation count over a lambda returning a borrow-bearing record because "the invocation must count the function value's set in any case".

The gap is reachable on every route that reads the set rather than the closure's escape. §9.7's capture bullet ("may not be captured by a closure that escapes its source's scope") bounds the closure's own escape, and X2 confirms it decides C7. It never fires when the closure stays home and its **set** leaves: through an invocation result (`var f := lambda() => s; return f();` — the result "carries the union row 4 states for a function-value invocation: the function value's source set and what every plain argument contributes", *Concrete-slot function bodies*), through a deferred-activation frame ("frame residence is **not** an escape sink", §28.14), and through §28.14's storage-bounded admission, which freezes only tested captures. For a span the escape has no backstop at all: "a raw span window has no disposal-tracked object through which checked mode could catch the dangle" (§30.1).

This proposal adds the missing item to provenance clause 1 with the count item 3 already gives, and narrows both exemptions to captures that carry no borrow. Nothing else moves: the §28.14 bound keeps its headline ("A capture contributing no borrow source is exempt"), §9.7's capture bullet keeps its verdict, and the empty-set admission for `int`, `string` and null-source captures is untouched.

**Measured reach: four pinned accepts flip, all on the sink route** (§2): a closure that captures such a value is now capture-bearing even when every carried borrow is a parameter's, so the sink rule rejects returning it, as it has rejected returning a closure over a class-typed parameter since v0.307 (rule case `s27_5_0007`). The four tests pinned the unsound `Make(a: Array<int>): () -> int` shape, whose caller receives a function value carrying the empty set and can carry it past `a`. Nothing flips in the doc examples, the guides or the rule corpus. A method reference bound to such a value (`pair.Show`) is counted the same way: formation captures the receiver by copy (§27.6).

---

## 1. Ground truth

### 1.1 The rule today

- **§28.3, closure bullet, provenance clause 1** (line 6742): "A **lambda literal** contributes the sources of the borrows it captures — a captured borrowed local or parameter that binding's source; a captured owned local its enclosing scope (…); a captured `ref` or `out` parameter, whatever its type, the declaring body's scope as well (§27.5); a captured function value its own set, transitively — where a lambda's captures include the free bindings of every lambda nested in its body that resolve outside that body." No item names a value-typed capture.
- **§28.3, *Deferred-activation callees*** (line 6816): a plain function-typed argument "contributes its captured borrows' sources (a value-typed capture contributes nothing, living in its heap capture cell — a `ref` or `out` parameter, which has none, contributes the declaring body's scope, §27.5; a runtime-managed capture contributes only what this rule assigns it — a `string` nothing, a function value or frame its sources)".
- **§28.14, *Class- and interface-typed `scoped` parameters*** (line 7666): "*Captured source* is §28.3's notion here as everywhere: a capture that contributes no source — a value-typed local or parameter, or a `string` — is not a captured source and does not bar the admission (*v1 admission bounds* item 1 below)."
- **§28.14, *v1 admission bounds* item 1** (line 7697): "**A capture contributing no borrow source is exempt, at both admission arms**: a captured value-typed local or parameter (a `ref` or `out` parameter excepted, §27.5) lives in its heap capture cell, which the closure keeps alive, and a runtime-managed capture that contributes no source … has unbounded lifetime by construction; neither carries a lifetime hazard. … The captures that *are* tested are those contributing a source: class- and interface-typed ones, `owned` bindings, plain function-typed parameters (K2.4), and `ref` and `out` parameters of any type (§27.5); a **move-only** value's capture is §28.9's borrow of the enclosing scope and is governed there, not by the value-typed exemption of this bound."
- **The direct count** — *The sources an argument contributes* item 3 (line 6616): "an argument of a value type that holds borrows (…) contributes the sources of the borrows it carries"; and the consume-site freeze (line 6765): "A live local of a **value type that holds borrows** … counts as a live local borrow of every held borrow's source".
- **§9.7's capture bullet** (line 1461): "A borrow-bearing record may not be captured by a closure that escapes its source's scope." §15.1.2 (inline arrays), §35.4 and Appendix C.3 (choices), stdlib §4.16 (spans, "capture it in a closure that escapes") say the same.
- **§9.7's classification paragraph** (line 1450): a value declared `IReadOnlySpan<TElement>` "is **read as holding borrows** — those of the span it boxes … every rule of this specification that reads a value, parameter, local or substitution that holds borrows … reads one declared at that interface type as it reads the span".
- **The invocation read** — *Concrete-slot function bodies* (line 6672): "The result of invoking a function value whose return type, after substitution and with nullability stripped, is such a type carries the union row 4 states for a function-value invocation: the function value's source set and what every plain argument contributes". A lambda at such a slot "admits on each return path a value whose every carried borrow is capture-sourced, plain-parameter-sourced or static storage".

### 1.2 The compiler today (`src/` at `1ec0886`; read from source, no build was run by this lane)

- `MethodOwnership::isBorrowableType` (`src/ownership/Ownership_Checks.cpp:3719`) answers **false** for every record, inline array, choice and bare type parameter (`default: return false; // Unit, InlineArray, enum, record`; a null-declaration `Named` is "NOT borrowable" unless a builtin exception).
- That predicate is the capture gate at both set-reading sites: `deferredActivationCaptureSources` (`Ownership_Checks.cpp:17398`, "Value-typed captures live in their heap capture cell — no borrow, no lifetime hazard") and `checkScopedAdmission` (`Ownership_Checks.cpp:21540`, "Value-typed captures live in their (heap) capture box … Only a capture of a class/interface reference can dangle"). The compiler therefore implements the silent reading at the frame and the `scoped` admission: a captured span, record, inline array or choice contributes nothing, and a generic `T` capture contributes nothing at every instantiation.
- The returned-lambda escape is implemented and pinned: `tests/lit/ownership/record_closure_capture_escape_2144__*.bzl` (span local, span field record, nested record, generic record, catch-source record; field, collection and outer-local stores), `choice_closure_capture_escape_2180__*.bzl` and `closure_capture_escape_2181__*.bzl` reject `return lambda() => s[0];` and its kin with "a borrow-bearing record may not be captured by a closure that escapes its source's scope (§9.7 closure-capture rule".
- Whether `return f();` over `var f := lambda() => s;` is rejected is **not settled by reading**: `checkTransitiveCapturesDisposed` (`Ownership_Checks.cpp:4301`, `4724`) walks a traced lambda local's captures at an invocation, but for the disposed-flag warning, not for the return's source set. The tracker names the probe (§6.4).

---

## 2. Measurement

Searched for a closure that captures a value that holds borrows and (a) returns it through an invocation that escapes, (b) rides a deferred-activation frame that escapes, or (c) is held by a `scoped` holder while a carried source is killed.

| Corpus | Method | Sites that flip |
|---|---|---|
| `tests/` gtest | files containing `AsSpan()` and `lambda`: `tests/sema/instance_field_borrow_lambda_3701_test.cpp` (the record is built inside the lambda body), `tests/sema/static_readonly_span_field_4679_test.cpp` (static field); files containing `record Pair`/`record Ref`/`record Bag` and `lambda` with `ScopeGuard`, `Select(`, `Where(`, `yield return`, `async method` or `scoped`: `vacuous_pin_lint_2073`, `domain_spawn_lambda_choice_arg_3237`, `domain_spawn_choice_valuearg_3181`, `lambda_test`, `case_arm_lambda_capture_2193` — each inspected; `case_arm_lambda_capture_2193`'s `var f := lambda(): int => p.X; return f();` is over `record Pair(X: int)`, which holds no borrow | **0** |
| `tests/lit` | every lambda-over-span/record/choice case (2144, 2180, 2181, 861, 3564, 3743): the scope-bound returned-lambda and store shapes are rejected today and after; `choice_closure_capture_escape_2180__arm_binding_param_source_accepted.bzl` returns a closure over an arm binding whose carried source is the parameter `a` | **1** (critic) |
| `tests/` gtest, the sink route (critic) | returned closures over a value whose carried borrows are a parameter's: `record_closure_capture_escape_2144.param_source_capture_returned_accepted`, `closure_capture_escape_2181.param_source_capture_in_if_block_lambda_returned_accepted`, `choice_closure_capture_escape_2180.param_source_choice_local_returned_accepted` — each `Make(a: Array<int>): () -> int` returning a lambda over `new Holder(a.AsSpan())` (or a choice over it); the closure's set is `{a}`, non-empty, and the sink rule rejects the return. Every other `return lambda`, field, collection and holder store in `tests/` was inspected: none captures such a value | **3** |
| method references | no `tests/`, doc or corpus method reference binds a record, inline-array, choice or span receiver at a set-reading or sink position | **0** |
| rule corpus (`tests/conformance/rules/`) | `manifest.tsv` and `cases/*.bzl` grepped for "contributes nothing", "value-typed local or parameter", "contributing no borrow source", "not a captured source": only `s19_6_0009` and `s35_6_0002` (exhaustiveness, unrelated) | **0**; no `rule:` header quotes the edited text |
| doc examples (`tests/conformance/cases/`), spec, stdlib, guides | lambdas over `AsSpan`/`Pair`: none in the guides; the spec's only such examples are §9.7's and §28.3's rejected ones | **0** |
| `ScopeGuard` users | `tests/conformance/env.bzl` only (Guide 1 §10's `Register`/`Unregister`, over a class) | **0** |

---

## 3. Options

### 3.1 Option A — status quo

Keep clause 1 silent and both exemptions as written. Unsound on fifteen cells: `Make(): ArraySpan<int>` below returns a window over a freed array with no flag; a record's dangling borrow is flag-only; the `scoped` admission's "fully static" promise (K5.7) is broken for a captured span.

```bezel
class Maker
  public method Make(): ArraySpan<int>
    var xs: Array<int> := [1, 2, 3];
    var s := xs.AsSpan();
    var f := lambda() => s;   // capture-sourced return path: admitted
    return f();               // carries f's set, which is {} under the silent clause
  end Make;                   // xs freed; the caller reads a freed window
end Maker;
```

### 3.2 Option B — count the carried sources in clause 1; narrow the exemptions (**recommended**)

Provenance clause 1 gains the item: a captured local or parameter of a value type that holds borrows (or declared `IReadOnlySpan<TElement>`) contributes the sources of the borrows it carries — item 3's count, read per instantiation in a generic body. Clause 2 and the method-reference bullet count a receiver of such a type the same way, since formation captures it by copy (§27.6). The frame contribution and §28.14's exemption are narrowed to captures that carry no borrow. The sink rule, the frame-return check (*Returning a deferred-activation frame*), §30.1's settlement obligation and §28.14's order test and freeze then read the sources they already read for a class-typed capture. A captured `CancellationToken` is excepted at the frame exactly as an argument one is ("a `CancellationToken` excepted (its source obligation is §30.8's)"). Null-source values and value types that hold no borrows still contribute nothing.

### 3.3 Option C — reject every closure capture of a value that holds borrows

Simplest statement. Over-rejects the in-place invocation `var f := lambda() => p.Item.N(); return f();` and every `xs.Where(lambda(x) => x > s[0])` enumerated in the span's scope, which §9.7's capture bullet admits today ("escapes" its source's scope); and a span over an owned buffer is the stdlib's stated idiom for reading a window inside a callback.

### 3.4 Option D — enumerate the three routes in §9.7's capture bullet

Keep clause 1 silent and extend §9.7's bullet: "… or whose invocation result, frame or storage-bounded holder escapes it". A second rule for one value: the closure bullet says "Every rule in this list then applies to the function value **by that set**, with no closure-specific list", and a route list is exactly the closure-specific list it forbids; the next route (a `ref` location, a `with` copy) reopens the gap.

### 3.5 Option E — count the carried sources at the closure's escape sinks only

Make the sink rule read the capture (so a field store or return of the closure is rejected by the set, as by §9.7's bullet) but keep the invocation result and the frame contribution empty. The counterexample of §3.1 is unchanged; declined as unsound.

---

## 4. The options side by side

| | Soundness | Simplicity | Consistency | Migration reach |
|---|---|---|---|---|
| A status quo | fails (15 cells; spans flag-less) | — | contradicts item 3, the freeze, rows 129 and 181 | — |
| **B count in clause 1** | **holds**: every set-reading rule sees the sources | one item added, two exemptions narrowed | the same count as item 3 and the freeze | **0 sites** |
| C reject the capture | holds | one sentence | splits closure captures from argument passing | rejects in-place invocations and in-scope lazy queries (0 sites today; the idiom is documented) |
| D enumerate routes | holds until the next route | a route list beside the set | against the closure bullet's "no closure-specific list" | 0 sites |
| E sinks only | fails (C6, C9, C10, C13) | — | — | — |

---

## 5. Decision (delegated ruling, 2026-10-08)

### 5.1 The rule

▲ **Provenance clause 1.** A lambda literal contributes, besides the items already listed, for *a captured local or parameter of a value type that holds borrows* (§3.6's holds-borrows property; one declared `IReadOnlySpan<TElement>` is read as holding the span's borrows, §9.7) *the sources of the borrows it carries*, the count *The sources an argument contributes* item 3 gives the same value as an argument; in a generic body the item is read at each instantiation at which the type, after substitution, holds borrows (§3.6 rule 2). A captured value that carries no borrow — a value type that holds none, a null-source value, a `string` — contributes nothing and lives in its heap capture cell.

▲ **Provenance clause 2 and the method-reference bullet.** A method reference bound to a receiver of a value type that holds borrows (or declared `IReadOnlySpan<TElement>`), captured by copy (§27.6), contributes the sources of the borrows the receiver carries, as clause 1 counts the same capture. Row 181 of `DECLINED.md` presupposes this count ("A method reference to an instance member returning a receiver-sourced value already hands back borrows of its bound receiver"); without it `var f := pair.First; return f();` over `public method First(): Item => Item` returns a borrow of a dead `Item`.

▲ **The capture-set contribution** (*Deferred-activation callees*) counts the same items, for a lambda's capture and a method reference's receiver, a captured `CancellationToken` excepted as an argument one is.

▲ **§28.14.** A capture of a value type that holds borrows, or declared `IReadOnlySpan<TElement>`, is a captured source for each borrow it carries, tested at both admission arms and frozen by those borrows' sources; the exemption of bound 1 is for a local or parameter of a value type that holds no borrows. The bound's headline is unchanged.

▲ **§9.7's capture bullet** states the mechanism in one clause: the capture contributes the carried sources to the closure's set, and the closure, its invocation results and the frames it rides are bounded by them. **Appendix C.3** gains the bullet.

Consequences, by route (each is the existing rule reading the now-counted sources):

- *The sink rule.* A closure over such a value carries a non-empty set, so it is not returned, stored in a field or any collection (a local one included), written through `out` or `ref`, or passed to a set-erasing position, whatever the carried sources are — a parameter's included, as for a closure over a class-typed parameter (`s27_5_0007`). `Make(a: Array<int>): () -> int => lambda() => new Holder(a.AsSpan()).S[0]` is the edge: the closure's set is `{a}`; the caller's `var f := C.Make(arr);` carries the empty set (clause 4: a method's function-typed result), so admitting the return would let the caller carry `f` past `arr`. Four pinned accepts flip (§2).

- *Invocation.* `return f();` over `var f := lambda() => s;` carries `s`'s sources (`xs`, scope-bound) and is rejected at the return by §9.7's return bullet, as `return s;` is. `var w := f(); Console.WriteLine(w[0]);` inside `xs`'s scope is admitted.
- *Frames.* `return r.RunAsync(lambda() => p.Item.N());` and `return Gen.Repeat(lambda() => a[0].N());` carry `it`'s scope and are rejected at the return by *Returning a deferred-activation frame*; `await r.RunAsync(lambda() => p.Item.N());` inside `it`'s scope is admitted under §30.1's settlement obligation, as the direct `await r.UseAsync(p)` is.
- *The `scoped` admission.* `using var g := new ScopeGuard(lambda() => Console.WriteLine(s[0]));` tests `xs`: declared before `g`, admitted and frozen, so a later `xs.Dispose()` is rejected under K5.7; a `using var cts` declared after the bounding local and captured through its token is rejected at the admission, as a class-typed capture declared after it is.

### 5.2 Kernel classification (spec-change-process §9)

**Derivation** — K2.2's union under composition over K2.4's source announcement: the closure's set is the union of what its captures carry, and a value that holds borrows carries its borrows' sources (the same derivation item 3 and the freeze rest on). No new axiom; **A11 is amended, not extended**: its bound 1 keeps its posture (a capture contributing no borrow source is exempt) and its enumeration is corrected, a narrowing of a ratified bound, which is why the change takes this proposal rather than a ruling.

### 5.3 Body-locality (proposal 0105 §1)

**Local.** The set is read off the body's own flow analysis; a generic body reads the item per instantiation exactly where §9.7's generic-body paragraph already reads its bullets.

### 5.4 Implementation sketch (`1ec0886`)

Replace the `isBorrowableType(ls->declType)` gate at `Ownership_Checks.cpp:17398` and `:21540` with a predicate that, for a holds-borrows value type (and `IReadOnlySpan<TElement>`), yields the captured value's carried sources — the reduction the direct-argument count and the consume-site freeze already perform for a local of that type — and treats a bare `T` per instantiation. The invocation route (`return f()`) needs the return check to read a traced lambda local's set through the same reduction. Probe first (§6.4).

### 5.5 Tests that flip

Four, all on the sink route, all pinning the unsound parameter-sourced return (§2, §5.1 *The sink rule*): gtest `record_closure_capture_escape_2144.param_source_capture_returned_accepted`, `closure_capture_escape_2181.param_source_capture_in_if_block_lambda_returned_accepted`, `choice_closure_capture_escape_2180.param_source_choice_local_returned_accepted`, and lit `choice_closure_capture_escape_2180__arm_binding_param_source_accepted.bzl`. Each flips to a rejection naming the sink rule when the tracker lands; until then the compiler accepts them and they stay as they are (spec-ahead-of-implementation; a banked verdict is never changed by a doc landing). The scope-bound lit cases 2144/2180/2181 keep their verdict and diagnostic.

### 5.6 `DECLINED.md` rows (entered at graduation)

Three `rejected` rows, beside row 129: option C (rejecting every such capture), option D (a route list in §9.7's bullet) and option E (sinks-only count). Exact text in `scratch/v0331/T1.json`.

---

## 6. Edits at graduation (each `old` unique at `1ec0886`; the exact JSON is `scratch/v0331/T1.json`)

Spec: provenance clauses 1 and 2 (lines 6742–6743) and the method-reference bullet (line 6748); *Deferred-activation callees* (line 6816, two parentheses); §28.14's captured-source sentence (line 7666) and bound 1 (line 7697, two sentences); §9.7's capture bullet (line 1461); Appendix C.3 (a bullet after the method-reference row, line 11862). Rationale §5.25 (a paragraph before "Two postures were declined by name"). Code-generation reference §7.2 (a pair after the `Runner` block). `DECLINED.md` (three rows after row 129). The README row for this proposal (Live queue, after 0132).

### 6.1 Changelog

Drafted in `scratch/v0331/T1.md`.

### 6.2 Migration

*Newly rejected (spec-ahead-of-impl):* the four invocation shapes (R3–R6 × C6), the frame-return shapes (R3–R6 × C9, R3–R5 × C10) and the `scoped`-held kill (R3–R6 × C13) of §5.1; a capture, through a token, of a `CancellationTokenSource` declared after the bounding local, and a `this`-arm admission (forms 3/4) whose lambda captures a record, span or token parameter not originating from `this`; by the sink rule, returning or storing a closure over such a value whatever its carried sources, a parameter's included; and the same for a method reference bound to such a receiver. Measured: four pinned accepts flip (§5.5), all on the parameter-sourced return; zero sites elsewhere. *Newly admitted:* nothing. *Doc-only:* the §9.7 clause, the C.3 bullet, the rationale paragraph, the code-generation pair.

### 6.3 Corpus

No case's `rule:` header quotes the edited text. Cases to write after landing (§28.3 and §28.14 numbering continues the manifest): reject `return f();` over a captured span, record, inline array and choice; reject a returned task frame and a returned iterator frame over a lambda capturing a record; reject `xs.Dispose()` under a `scoped` holder of a lambda capturing a span over `xs`; accept the in-place invocation, the frame awaited in scope, and a captured null-source `CancellationToken.None`.

### 6.4 Trackers to file at landing

One `spec-tracking` issue: "§28.3 provenance clause 1, §28.14 bound 1 (v0.331): a captured value that holds borrows contributes its carried sources — the `isBorrowableType` capture gate (`Ownership_Checks.cpp:17398`, `:21540`) drops it; probe `return f()` over a lambda returning a captured span" (#5677 → the compiler delta). Composition note for #5568 (a value-typed argument at an interface-typed `scoped` parameter): its borrow-bearing arm now has the captured twin here.

---

## 7. Self-critique (adversarial pass)

- *Does counting the capture reject the in-place idiom?* No: a non-empty set rejects a function value only at the sinks and set-erasing positions; binding, invoking in scope and passing to a call-bounded parameter stay admitted, as for a class-typed capture.
- *Does it double-count?* The capture contributes the value's **sources**, not the value; a closure capturing both `xs` and `s := xs.AsSpan()` carries `{xs}` once (K2.2's union).
- *Is `CancellationToken` consistent?* At the frame, excepted as an argument one is; at the `scoped` admission, tested by the token's source, which is the new rejection the Migration note names; at a return through invocation, §9.7's return bullet, as a direct return of a local-sourced token.
- *A `T`-typed capture.* Read per instantiation; at a class substitution it is the existing class-typed capture, at `int` nothing, at `Pair` the carried sources. The compiler's gate treats a bare `T` as non-borrowable at every instantiation, so the generic form is in the tracker.
- *Is the sink-route flip an over-rejection?* No: it is the rejection a closure over a class-typed parameter already takes (v0.307, `s27_5_0007`), and the accepted shape was unsound, since a method's function-typed result carries the empty set at its caller (clause 4). The writer's remedy is the one for the class case: return the value, not a closure over it, or take a `scoped` parameter.
- *Generic bodies.* The item is read per instantiation, as §9.7's generic-body paragraph reads its bullets. *The sources an argument contributes* item 4 counts an argument of type-parameter type once at the declaration, as the union over the admitted substitutions. The two readings agree at every concrete substitution. They differ only under `where T: value`, where item 4 rejects at the declaration what this item rejects at a holds-borrows instantiation. That is a pre-existing asymmetry between the argument count and §9.7's bullets, and it is filed as a question rather than settled here (critic).
- *Seam with T2 (#5679, #5680).* The §28.14 bound-1 wording is edited here (two sentences in item 1 and the captured-source sentence); the admission bounds' other items, K5, K6.4 and the scoped residual are untouched. T2's K5(ii)/K6.4 span backstop should cover a captured span exactly as an argument span, since both are now the same contribution; R6/C10's flag-less residual stays T2's.
- *History narration.* None: the new text states the rule, the changelog carries the story.
