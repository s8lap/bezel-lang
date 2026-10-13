# Proposal 0177 — The destructor takes its fields as any other body does: K9.3's bare take of a non-nullable field is retired

| | |
|---|---|
| **Status** | graduated-v0.337 — superseded — graduated v0.337 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option B — K9.3's destructor exemption from the required replacement is retired: a destructor body reads and takes the fields of the object it tears down under the rule every other body takes (`move x.f with e`, the bare take at a nullable field), releases one its own class declares early with the field's explicit `Dispose()` (§28.6's early release point, the cascade's later call the no-op) and hands one onward through a nullable declaration; K9.2's no-hole invariant holds without exception (#5913, both faces); critique round 1 applied (the early close scoped to a field the destructor's own class declares, an inherited field being a borrow receiver; a base destructor's read stated as a live value, `null` or a logically disposed value under K4.1's backstop; `s28_8_1_0027`'s `issue:` header; #1230's quotation made verbatim); landed with the v0.337 changelog entry) |
| **Tracking issue** | [#5913](https://github.com/s8lap/bezel-lang/issues/5913) (both faces: the call on `this` in the same body, and the base destructor reading an inherited `protected owned` field after a derived destructor's take); compiler tracker filed at the v0.337 landing (the required-`with` diagnostic in destructor bodies; the #1230 diagnostic on an explicit field `Dispose()` in a destructor body withdrawn) |
| **Target module(s)** | **spec**: §28.3 (K1 item 3; K9 items 2 and 3), §28.8.1 (the opening paragraph, *The replacing `move`*'s required-clause sentence and no-hole paragraph, *Constructors and destructors* rewritten), §28.6 (*Who may call `Dispose()`*, the idiom clause), §28.4 (step 2, editorial), Appendix A.7 (the `MoveExpr` comment), Appendix C.3 (the replacing-`move` bullet). **rationale**: §5.10 (one dated paragraph). **code-generation reference**: §4.2 (one sentence deleted), §7.2 (one *Looks right, is rejected* pair). **No stdlib, guide or grammar-production change.** |
| **Written against** | spec/stdlib **v0.336** at `e6241b23d` (main). Section numbers are v0.336's. |
| **Origin** | The v0.336 train's `Y1` critique (finding F3, residual Q1 → #5913) and the v0.336 disposal-matrix re-run (`docs/research/disposal-matrix/runs/v0.336.json`, route R4/C5·dtor-bare-take, which added the inherited-field face in #5913's comment). Decided by lane Z1 of the v0.337 train under the 2026-10-08 delegation. |

*Reference convention: unprefixed §-references are language-spec references at v0.336; "stdlib §" references are to the standard library companion; K tags are §28.3's kernel ids. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API. The former name of the language is not used anywhere in this document.*

---

## 0. Summary

Proposal 0174 (v0.336) made every field or slot `move` the replacing `move loc with e`, so that "an owned location never holds a hole" (K9.2), with one residue it recorded rather than closed: inside a destructor body, K9.3 still lets a non-nullable field of the object being torn down be taken **bare** — `using var t := move _r;` — the field "hold[ing] no value for the rest of the body". K1's straight-line rejection covers a later read through the field's own spelling only. #5913 names the first escape: a call on `this` later in the same body (`this.Flush()`, where `Flush` reads `_r`) reads the emptied field through another spelling and meets `null` typed non-null — no disposed object stands behind it, so K4.1's flag never fires and no K4.3 clause covers it. The v0.336 matrix re-run found a second escape that no per-body rule can reach: a **derived** destructor takes an inherited `protected owned` field bare, and the **base** destructor, which §28.4 runs afterwards, reads the field through its own spelling in another body.

This proposal retires the exemption. A destructor body takes its fields under the rule every other body takes: `move x.f with e` exchanges, `move x.f` at a nullable field stores `null`, and a non-nullable field is not taken bare. The two needs the bare take served have spellings the text already admits: a field released *before* the rest of the body is disposed in place (`_r.Dispose();`, §28.6's early release point — the field keeps its logically disposed value, the cascade's later call is §28.2's no-op), and a value handed *onward* from a destructor takes a nullable declaration, the remedy 0174 gives everywhere else. K9.2 then holds without exception, K1's *moved-from* is a state of bindings only, with no residue, and the cascade meets live values and nulls only at every level of the hierarchy, whatever a derived destructor did first.

Measured: twelve bare-take sites in ten test programs and two rule cases, every one disposing the taken value at the body's end — which the cascade does unprompted, and which is exactly how the compiler lowers the take today (inside a destructor body it neither nulls the field nor disposes the `using var`; the cascade is the sole disposal site, `src/codegen/LlvmBackend_Exprs2.cpp:852–866`). No document, guide, corpus program or stdlib sketch uses the shape. Nothing is newly admitted. This is a §10 subtraction, defended by no site.

---

## 1. The problem, in the text

### 1.1 The residue, as v0.336 states it

K9.3 (v0.336): "a destructor body may take a field of the object it tears down bare — `move x.f` with no replacement, whatever the field's nullability: teardown reads it no more and the cascade skips the emptied field". §28.8.1 *Constructors and destructors*: "at a non-nullable field the field holds no value for the rest of the body — a later read of it there through the field's own spelling is a read of a moved-from location ([K1](#)) and is rejected until the field is reassigned." K1 item 3: "the binding — or a field a destructor body took bare ([K9.3](#)) — is unreadable until reassigned". K9.2, §28.8.1's no-hole paragraph and Appendix C.3 each carry "a destructor body's bare take aside".

"Teardown reads it no more" is the claim the two faces refute.

### 1.2 The two faces

**Face 1 — a call on `this` in the same body** (#5913):

```bezel
destructor
  using var r := move _r;     // `owned _r: Res`, non-nullable: the field holds nothing now
  this.Flush();               // `Flush` reads `_r` through its own spelling, in another body
end destructor;
```

K1's rejection is spelled over `_r` in *this* body. `Flush` is checked once at its declaration (proposal 0105's body-locality), where `_r` is an ordinary live field. The read meets `null` at a non-nullable type. There is no disposed object behind it, so K4.1's backstop has nothing to test, and K4.3's "recorded trust, or nothing" records nothing for it.

**Face 2 — a base destructor after a derived destructor's take** (#5913's comment; route R4/C5·dtor-bare-take):

```bezel
class Base
  protected owned _r: Res := new Res();
  destructor
    _r.Flush();                // runs after Derived's destructor (§28.4: subclass-to-base order)
  end destructor;
end Base;
class Derived: Base
  destructor
    using var r := move _r;    // K9.3: "a field of the object it tears down" — nothing says "a field its class declares"
  end destructor;              // r disposed here
end Derived;
```

The read is in another body, after the taking body has completed. No rule over the taking body — #5913's readings 1 and 2 included — can see it.

### 1.3 Why a per-body rule cannot close the hole

The hole is a state of the **object** (a non-nullable field holding nothing), not of the body that made it. Every rule that keys on the taking body leaves a reader outside it:

- a base destructor (face 2);
- a callee that does not take `this` but reaches the object through a stored borrow — a registry holding a borrow of the object under §28.3's field-held clause, calling `obj.Flush()` when the destructor notifies it;
- a closure that captured `this` before the take and is invoked after it (the invocation is a use of a local, not of `this`);
- a virtual or interface call on another object that re-enters this one through either path above.

Each of these reads the field while the destructor body is still running, before logical disposal has completed, so the flag (set at the beginning of `Dispose()`, faulting "from its completion", Appendix C.3) does not stand behind the read either. Restricting the take to fields the destructor's own class declares (the re-run's reading 4) closes face 2 alone. A run-time representation of the emptied field — a sentinel the cascade skips and every read tests — would be a second null-like state at a non-nullable location, tested at every read of every owned field in both build modes; the v0.223 #1814 decline already recorded that "moved-from is per-method static state with no runtime representation for a non-nullable field".

### 1.4 What the compiler does today (source read at `e6241b23d`; no build)

- The ownership pass exempts destructor bodies from the (now-deleted) restoration analysis: `src/ownership/Ownership.cpp:1530–1545` (the at-exit check, "#763: the field-restoration rule does NOT apply inside a destructor") and `src/ownership/Ownership_Checks.cpp:19312–19316` (`checkThrowableWhileFieldMovedFrom`, `if (isDestructor_) return;`). 0174's required-`with` diagnostic is not implemented anywhere yet (tracker #5923), so today every bare take compiles, in every body.
- The lowering treats a destructor body's bare take as an **alias of the field**: the null-store that a bare `move _f` performs elsewhere is gated on `!emittingDisposerBody_` (`src/codegen/LlvmBackend_Exprs2.cpp:852–866`, the #899 fix; the qualified form `:880–896`, #897/#898), and the `using var`'s scope-end disposal is suppressed inside destructor bodies ("disposer locals: field pass owns them", `src/codegen/LlvmBackend_Disposal.cpp:6846`, `:7004`; `LlvmBackend_Setup.cpp:23202`). The cascade (`emitOwnedFieldDisposals`) is the sole disposal site. So at run time `using var t := move _r;` in a destructor is today exactly what writing nothing is, and face 1 reads a live value — which is why the twelve sites run; the spec's state (field empty, `t` the owner) is not the one the compiler implements.
- The ownership pass **rejects** an explicit `_f.Dispose()` or `this._f.Dispose()` inside a destructor body (`src/ownership/Ownership_Checks.cpp:2120–2140`, the #1230 fix: "explicit `.Dispose()` of an owned field inside a destructor double-frees … (§28.4)"), pinned by `tests/lit/ownership/destructor_owned_field_dispose_1230__bare_field_dispose_in_destructor_rejected.bzl` and `__explicit_this_field_dispose_in_destructor_rejected.bzl`. The spec admits the call: §28.6's receiver list names "an `owned` field … read by code of the owning object's declaring type", its idiom clause names "the destructor idioms of §28.4/§28.8.1", and the v0.303 #4366 ruling makes the call an early release point whose "second call is §28.2's no-op" at the cascade. The diagnostic is a pre-existing compiler over-rejection; under this proposal it stands in the way of the destructor's early-release spelling and goes to the tracker (§8).

---

## 2. Options

| | Option | Shape |
|---|---|---|
| A | **Status quo** (#5913 reading 3) | Keep the exemption; record the two faces as a residual. |
| B | **Retire the exemption** (this proposal) | A destructor body takes its fields as any other body does. Early release: the field's explicit `Dispose()` (§28.6). Transfer out: a nullable declaration. |
| C | **A per-body `this` rule** (#5913 readings 1 and 2) | Reject a use of `this`, other than another field's own spelling, after a bare take of a non-nullable field; or require the take to follow the body's last use of `this`. |
| D | **Own-class restriction, with C** (the re-run's reading 4) | The bare take of a non-nullable field is admitted only at fields the destructor's own class declares; C covers the same-body face. |
| E | **A run-time moved-from representation** | A sentinel at the emptied field, skipped by the cascade and tested by every read, so the take could stay. |

---

## 3. Evaluation

| Criterion | A | B | C | D | E |
|---|---|---|---|---|---|
| **Soundness** | open: both faces read `null` typed non-null with no flag and no clause | closed: no location is ever empty; the cascade meets live values and nulls only at every level | open: face 2 (another body), the stored-borrow callee, the captured `this` | open: C's own faces | closed, at a cost in every read |
| **Simplicity** | one exemption, one residue clause in four places | zero exemptions; K1 item 3, K9.2, §28.8.1 and C.3 each lose a clause | a third apparatus around the take (a `this`-use analysis per destructor body) | C plus an access category §28.6 deliberately does not draw ("by declaring type, matching §13.1's class-level privacy") | a second null-like state at non-nullable locations, in both build modes |
| **Consistency** | K9.2 "holds a live value at every instant" is false in one place | K9.2 holds without exception; 0174's rule is uniform across bodies | the take stays an exception to 0174's rule | likewise | contradicts the v0.223 #1814 ground |
| **Migration reach** (§4) | none | twelve take sites, each a no-op relative to the cascade; zero documents, guides, corpus programs, stdlib sketches | none, if the twelve sites are clean (they are) | none | none |
| **Expressiveness** | the bare take in destructors | lost: a *bare* transfer out of a non-nullable field inside a destructor (zero sites); kept through a nullable declaration | kept, bounded | kept, bounded | kept |

**Decision: B.** It is the only sound option that adds no rule, and it is the direction the owner's standing preference names — narrowing a feature with negligible measured reach beats adding rules around it. The three points the owner decided for 0174 (the spelling `move loc with e`; a bare `move` at a nullable location stores `null`; §9.5's tie-break kept) are untouched; the destructor exemption was 0174's own drafting choice (its §6 item 1: "item 3 keeps the destructor's bare take"), carried from v0.335's K9.3 because the eleven sites it counted used it, and recorded as a residue by its critic. The v0.336 changelog lists no owner decision on it.

What B does not change: the bare take at a **nullable** field in a destructor is the ordinary bare take (stores `null`; a base destructor reading it meets `null` at a nullable type, which the type admits); the constructor half of K9.3 (§7.9's definite assignment); `move x.f with e` in a destructor body, through `this` or any other receiver; and the explicit disposal of a field or local-array slot in a destructor body, which §28.6 already admits by declaring type.

---

## 4. Measurement

**Method.** Every `destructor … end destructor` body in `tests/` (gtest C++ program strings, lit `.bzl`), both conformance corpora, `docs/` and the stdlib sketches was scanned for a `move` (`scratch/v0337/work-Z1`, the scan over `tests/**/*.cpp`, `tests/**/*.bzl`, `docs/*.md`); each hit was read. The v0.336 lane's list (`scratch/v0336/Y1.md`, *Destructor bare take (K9.3, unchanged) — 11 site(s)*) was re-derived at HEAD, which added one rule case (`s28_8_1_0027`, written at v0.336).

**Newly rejected — twelve take sites, ten test programs and two rule cases, all of a non-nullable `owned` field of `this`:**

| Site | Take | Later use of `this` or of the field | Inheritance | Verdict today | Respelling |
|---|---|---|---|---|---|
| `tests/codegen/destructor_field_move_runtime_763_test.cpp:37–39` | `var taken := move this._inner; taken.Dispose();` | none | none | run (count 1) | `this._inner.Dispose();` — same count, same order |
| `…_763_test.cpp:58` | `using var u := move this._inner;` | none | none | run | delete the line (the cascade disposes once), or `this._inner.Dispose();` |
| `tests/codegen/dtor_bare_move_dispose_899_test.cpp:46` | `using var taken := move _res;` | none | none | run | as above |
| `…_899_test.cpp:70` | `using var taken := move this._res;` | none | none | run | as above |
| `…_899_test.cpp:150`, `:157` (two classes, one test) | `using var taken := move _res;` / `move this._res;` | none | none | run | as above |
| `tests/ownership/move_field_throwable_exception_edge_3080_test.cpp:231` | `using var t := move _r; var q := Dangerous();` | none | none | sema-only accept (the exception-edge rule's destructor exemption; the rule itself was deleted by 0174) | retire: the subject is gone twice over |
| `tests/lit/ownership/destructor_field_move_763__destructor_move_then_dispose_accepted.bzl:13` | `var taken := move this._inner; taken.Dispose();` | none | none | accept | `this._inner.Dispose();` |
| `…763__destructor_move_into_using_var_accepted.bzl:12` | `using var u := move this._inner;` | none | none | accept | delete the line |
| `tests/lit/ownership/destructor_owned_field_dispose_1230__move_then_dispose_in_destructor_accepted.bzl:14` | `var taken := move _sub; taken.Dispose();` | none | none | accept | `_sub.Dispose();` — the shape its two sibling cases pin as rejected (below) |
| `tests/conformance/rules/cases/s28_8_1_0011.bzl:35` (**banked** `pass`, `run`) | `using var taken := move _resource;` then `Console.WriteLine("holder teardown")` | none (`Console` is static) | none | run: `holder teardown` / `dispose inner` | `Console.WriteLine("holder teardown"); _resource.Dispose();`, or delete the take — the same output either way; **for the tracker** (banked) |
| `tests/conformance/rules/cases/s28_8_1_0027.bzl` (unbanked) | `using var t := move _r; Console.WriteLine(_r.Name);` | the field's own spelling (rejected today as `moved-from`) | none | reject | stays `reject`; the ground and the diagnostic needle move to `with` (header refreshed in `Z1.corpus.json`) |

Every site takes a field of its own class with no later use of `this`, no inheritance, and discharges the taken value at the body's end: the respelling to the early close, or to nothing, keeps every verdict, count and output line. The respelled `_f.Dispose();` is spec-admitted (§28.6) and rejected by the compiler's #1230 diagnostic (§1.4), so the two #1230 lit pins `bare_field_dispose_in_destructor_rejected` and `explicit_this_field_dispose_in_destructor_rejected` **flip to accept** when the diagnostic is withdrawn — a behaviour flip the implementing commit names (§8).

**Zero sites elsewhere.** No code block in the specification, stdlib, rationale, guides or code-generation reference takes a field bare in a destructor (the code-generation reference's §4.2 states the exemption in one sentence, deleted here); no doc-corpus case mirrors one; no stdlib sketch (`OwningArray<T>`, `OwningDictionary<K, V>`, the span types) has a destructor that moves a field. `scratch/v0336/Y1.md`'s "11 sites" and this table's twelve differ only in counting the `899:150/:157` pair as two takes and in the v0.336 rule case.

**Newly admitted: nothing.** The explicit field `Dispose()` in a destructor body was already admitted by §28.6; the compiler's rejection of it is pre-existing (§1.4).

**Words.** Specification: about 230 removed, about 190 added (net about −40); rationale +230 (one dated paragraph); code-generation reference −25 +120 (one pair).

---

## 5. Soundness, and the cells

**Argument.** After this proposal, no body — a destructor's included — ever leaves an owned location holding nothing: the replacing `move` stores in the step it takes, the bare take stores `null` at a nullable location only, and explicit disposal leaves the location holding its logically disposed value, flag set. Every reader of a field during teardown therefore meets a live value, `null` at a nullable type, or a flagged object: the first is ordinary, the second is what the type admits, the third is K4.1's backstop (`DisposedAccessException` in checked mode). This holds for a call on `this` in the same body, for a base destructor in another body, for a callee reaching the object through a stored borrow, and for a closure that captured `this` — the faces §1.3 lists — because the invariant is on the object. K9.2's "a non-nullable owned field or owning slot holds a live value at every instant" is now true at every instant, the cascade "meets live values and nulls only" at every level, and K1's *moved-from* has no location-level instance.

**Exactly-once.** The early close disposes the field's value at the call and the cascade's call is the no-op on the set flag followed by the block's free at the holder's release point (§28.2's compiler-inserted free, §28.6's early-release paragraph) — the posture §28.6 already states for a method body, applied in a destructor body, where the containing object's teardown is the current statement's own continuation. A value handed onward through a nullable field leaves the field `null`, which the cascade skips (§28.4 step 2's null check).

**Cells** (`docs/research/disposal-matrix/runs/v0.336.json`):

- **Route R4/C5·dtor-bare-take** (#5913; open, both faces): **closed** — its subject does not exist. The counterexample programs are rejected at the `move` (`with` required; the diagnostic names `_r.Dispose()`), and their sound respellings (`move _r with fresh`, `_r.Dispose()`, `owned _r: Res?`) each leave the base destructor's read meeting a live value, a flagged object or `null`.
- **R4/C5** (decided, conditional): the "open residue" note loses its subject; the three sub-shapes (exchange, nullable bare take, non-nullable bare take rejected) are now uniform across bodies.
- **R4/C1** and **R4/C12** (decided): "a destructor body's bare take is skipped by the cascade" becomes "an early-closed field's cascade call is the no-op"; verdicts unchanged (once-backstopped; once).
- **Route R4/C3·re-entrant** (decided): "the destructor's bare take is the one residue" — no residue.
- **Neighbours re-walked, no change:** R4/C4 (a constructor body's `move`: §7.9 before first assignment, the replacing `move` after); R4/C9 (explicit disposal of a field: unchanged, and now the destructor's early-release spelling); R4/C14 (`DisposeAsync` supersession: the destructor body is not run on the `await using` path, so no take happens there either way); R6/C1 and R6/C9 (a local owning inline array in a destructor body: its slots are taken bare only at a nullable slot type, as elsewhere); R9 (statics: K9.4 unchanged).

**Newly opened: none.** The one admission this proposal relies on — an explicit `Dispose()` of a field inside a destructor body — is §28.6's existing admission, by declaring type, with the cascade's no-op already stated. A derived destructor cannot dispose an inherited field explicitly (§28.6: "an owned-field read outside the declaring class" is a borrow receiver), which is right: the base's cascade disposes it, and a derived destructor that needs the base's resource released first exchanges it (`move _r with fresh`, admitted "at the site" where the field is accessible) or asks the base for a method that does.

---

## 6. The normative edits

Exact `old → new` pairs are `scratch/v0337/Z1.json`, ids `Z1-0177-*`. In prose:

1. **K1 item 3** ▲ "the binding — or a field a destructor body took bare (K9.3) — is unreadable until reassigned" → "the binding is unreadable until reassigned".
2. **K9 item 2** ▲ "item 3's destructor take aside" deleted. **K9 item 3** ▲ becomes the constructor's definite-assignment clause plus: "a destructor body reads and takes the fields of the object it tears down as any other body does — the replacing `move`, or the bare take at a nullable field — and may dispose one its own class declares early (§28.6), the cascade's later call being the no-op; no body takes a non-nullable owned location bare (§28.8.1)". K9's numbering is kept, so every K9.3 citation stays valid.
3. **§28.8.1** ▲ the opening paragraph's "a destructor body's bare take of its own object's non-nullable field aside" deleted; the required-clause sentence gains the destructor's third remedy ("in a destructor body, at a field the destructor's class declares, the error names the field's explicit `Dispose()` as well") and states "The rule has no exemption"; the no-hole paragraph's "with the one residue of a destructor body's bare take" deleted; *Constructors and destructors*' destructor paragraph rewritten (the fields are owned locations like any other; the early close, at a field the destructor's own class declares (an inherited field is a borrow receiver under §28.6); the nullable declaration; the cascade meets live values and nulls only at every level; a base destructor's read of its `protected` field meets a live value, `null` at a nullable field, or a logically disposed value under K4.1's backstop — never an empty non-nullable field; other objects' fields and owning slots take the same rules).
4. **§28.6** ▲ "the destructor idioms of §28.4/§28.8.1 unchanged" → "the destructor body's early close of a field (§28.8.1 *Constructors and destructors*) unchanged".
5. **§28.4 step 2** ▲ editorial: "a null or moved-from field disposes nothing" → "a null field disposes nothing" (no field is ever moved-from, Appendix C.3).
6. **Appendix A.7** ▲ the `MoveExpr` comment's "a destructor body's bare take of its own object's field, K9.3, is the one exemption" → "no body, a destructor's included, takes a non-nullable location bare, K9.3". **Appendix C.3** ▲ the replacing-`move` bullet loses "outside a destructor body's bare take of its own object's field (K9.3)" and reads "a bare `move` at a non-nullable location is rejected in every body, a destructor's included (K9.3)".
7. **Rationale §5.10** ▲ one dated paragraph (v0.337): the residue, the two faces, why no per-body rule closes it, the retirement, the two spellings, the declines.
8. **Code-generation reference** ▲ §4.2's "Only a `destructor` body takes a field of its own object bare whatever its nullability (the cascade skips it)." deleted; §7.2 gains one *Looks right, is rejected* pair (the destructor's `using var t := move _resource;` → ERROR naming `_resource.Dispose();`, `move _resource with fresh` and the nullable declaration; the early-close block). Per the document's maintenance rule, no sentence is appended to prose and no pin is added.

Passages checked and left unchanged: §28.4's example destructor (`FileStream`, no `move`); §28.6's receiver list (already names the field read by its declaring type); §28.11 (no destructor clause); §15.1.1/§15.3 (a local array's slots in a destructor body are ordinary); stdlib §4.5/§4.9 sketches (no destructor moves a field); Guides 01 and 05 (neither teaches the destructor take; no pin moves); the rationale's v0.336 paragraph ("The destructor keeps K9.3's bare take") stays as dated history.

---

## 7. Declined alternatives (rows for `DECLINED.md`)

| Topic | Verdict | Reason | Reopening bar |
|---|---|---|---|
| Recording the destructor's bare take of a non-nullable field as a residual, left to K1's own-spelling rejection (#5913 reading 3; the v0.336 status quo) | `declined-at-proposal` | the read through another spelling meets `null` typed non-null with no flag (K4.1) and no recorded clause (K4.3), which K4.3's "recorded trust, or nothing" does not admit as a residual; the second face is in another body, outside any per-body record | —(none stated) |
| Rejecting a use of `this` other than another field's own spelling after the take, or requiring the take to follow the body's last use of `this` (#5913 readings 1 and 2) | `declined-at-proposal` | per-body rules over a hole that is in the object: a base destructor, a stored-borrow callee and a captured `this` are outside their reach; a third apparatus around the take with faces still open, protecting nothing a site uses | a measured destructor that must take a non-nullable field bare and use `this` afterwards |
| Confining the take to fields the destructor's own class declares, with reading 1 (reading 4) | `declined-at-proposal` | closes the inherited face only; keys admission on which class declared the field, an access category §28.6 deliberately does not draw; adds a rule where removing the exemption removes the hole | —(none stated) |
| A run-time representation of a moved-from non-nullable field (a sentinel the cascade skips and every read tests) | `declined-at-proposal` | a second null-like state at a non-nullable location, tested at every read of every owned field in both build modes, for a take no site uses for anything the cascade does not do; K9.2's invariant is simpler and stronger; the v0.223 #1814 ground | —(none stated) |

The exemption itself is indexed as a **`retired`** row (the destructor body's bare take of a non-nullable field, v0.87's #763 ruling through v0.336), with the reopening bar "a measured destructor that must transfer a non-nullable field's value out and cannot declare the field nullable, with a representation of the emptied field that every reader — a base destructor included — tests".

---

## 8. Changelog, migration, trackers

- **Class.** Design change (a stated admission narrowed: the bare take of a non-nullable field inside a destructor body is withdrawn; nothing added; the exemption retired under process §10's bar — no site defends it), graduated in v0.337. Kernel classification: K9.3 restated as a derivation of K9.1 + K9.2 + K3.9 (no axiom touched; K1 item 3 loses its one location-level instance). Body-locality (proposal 0105 §1): **local** — the rule reads the operand's declared type and the body kind, nothing across bodies; it is the rule 0174 already states, with its exemption removed.
- **Migration.** *Newly rejected (spec-ahead-of-impl):* a bare `move` of a non-nullable owned field inside a destructor body — twelve take sites in ten test programs (§4's table, each respelled to the early close `_f.Dispose();` or deleted, verdicts, counts and output kept) and two rule cases (`s28_8_1_0011`, banked `pass`/`run`, flips to `reject` and goes to the tracker; `s28_8_1_0027`, unbanked, keeps `reject` with its header and needle refreshed in this train). *Newly admitted:* nothing (the explicit field `Dispose()` in a destructor body was admitted at v0.303, #4366). *Behaviour flips the implementing commit names:* the two #1230 lit pins `destructor_owned_field_dispose_1230__bare_field_dispose_in_destructor_rejected` and `__explicit_this_field_dispose_in_destructor_rejected` (reject → accept, once the #1230 diagnostic is withdrawn in favour of §28.6's admission — the cascade's call on the flagged field is the no-op, so the double-free the diagnostic guarded against is §28.2's idempotence at work; the implementation must make the cascade honour the flag, which #1230's own report named as the other acceptable fix). *Doc-only:* K1, K9, §28.8.1's opening and no-hole sentences, §28.6, §28.4 step 2, Appendix A.7, C.3, the rationale paragraph, the code-generation reference.
- **Trackers (filed at landing, `spec-tracking`):** one tracker for this proposal — (i) the required-`with` diagnostic inside destructor bodies, naming `_f.Dispose()` beside `move _f with fresh` and the nullable declaration (an item of #5923's diagnostic, extended); (ii) withdrawal of the #1230 diagnostic on `_f.Dispose()` / `this._f.Dispose()` in a destructor body (`src/ownership/Ownership_Checks.cpp:2120–2140`) and the cascade's flag-honouring no-op on an explicitly disposed field (the generated `Dispose()` already null-guards; it must also read the flag, which §28.2's idempotence states), with the two #1230 lit pins flipped; (iii) deletion of the destructor-body special cases in the lowering (`LlvmBackend_Exprs2.cpp:852–866`, `:880–896`) and of `isDestructor_`'s field-moved-from exemptions (`Ownership.cpp:1535`, `Ownership_Checks.cpp:19316`) with the rest of 0174's analysis; (iv) the test migration (§4's table, one commit); (v) the rule-corpus sweep (`s28_8_1_0011` flips to `reject`, needle `with`). A note on #5830 (explicit disposal marks, the holder frees — the destructor-body case is now the early-release spelling the text names). *(Composed at v0.337: the tracker also carries the doc corpus's blocked-on row for the code-generation reference's new §7.2 early-close block — `_resource.Dispose();` in a destructor, which today's compiler rejects under #1230's diagnostic — the banked `s28_8_1_0011`'s `rule:` header, refreshed with its flip, and the banked `s11_5_0008` (`_cached.Dispose();` in a destructor, rejected on the `owned` method modifier), re-verified at the #1230 withdrawal; a note goes on #5923.)*
- **Corpus cases to write after landing** (coverage gaps recorded, not written under landing pressure): reject — `using var t := move _r;` in a destructor at `owned _r: Res` (diagnostic names `Dispose`); run — a derived destructor's `move _r with fresh` of an inherited `protected owned` field, the base destructor then reading `fresh`; run — a destructor's `_r.Dispose();` followed by a `this` call that reads `_r` (faults `DisposedAccessException` in checked mode, exit code and stdout asserted); run — a derived destructor's bare take of an inherited **nullable** field, the base destructor reading `null`.

---

## 9. Self-critique (adversarial pass)

- *Is this a design change or a ruling?* A narrowing of a stated admission ("may be taken bare whatever its nullability") is a design change under process §2, and a removal is a §10 subtraction; both gates are taken here. The owner's three 0174 decisions are untouched (§3).
- *Does the early close really replace the take?* For the measured sites, yes (each discharges at the body's end, which the cascade does). The one shape the take expressed that the early close does not is a *transfer out* of a non-nullable field from a destructor — zero sites; the nullable declaration spells it, which is 0174's own remedy for the same shape in every other body. A reader who objects that "a destructor should be able to return a connection to its pool without declaring the field nullable" is asking for the hole this proposal removes; the row's reopening bar names the representation such a program would need.
- *Is the compiler's #1230 diagnostic evidence that the early close is unsafe?* No: the diagnostic predates the v0.303 #4366 ruling that made the explicit call an early release point with the cascade's call the no-op; #1230's own report offered "make the auto owned-field cleanup honor the per-object disposed flag" as the alternative fix, and §28.2's idempotence is that fix stated. It is implementation catch-up, tracked.
- *Does face 2 have a nullable twin that stays open?* A derived destructor's bare take of an inherited **nullable** field leaves `null`, which the base destructor's read meets at a nullable type — the state the declaration admits, and the same state after `_r := null;` in any method. Not a hole.
- *Timing.* With `using var t := move _r;` the value was disposed at the body's end, before the cascade's other fields; with the early close at the body's end, at that statement; with the line deleted, inside the cascade in reverse declaration order. For a single owned field the three are indistinguishable; for several, the early close reproduces the take's order exactly (dispose at the point where `t` would have gone out of scope). No measured site has more than one owned field.
- *Anchors.* Every `old` in `Z1.json` was read off HEAD by the build script and is unique there; the lane's edits sit inside §28.8.1's destructor paragraph, K1/K9 items and the one-sentence clauses listed in §6, none of which Z2 or Z3 touch (their sections are §3.6, §9, §25.2, §35.7, §27.6, §22.13, §14.2, §11.8, §28.11's warning and §28.3's field-store bullet).
