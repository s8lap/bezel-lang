# Proposal 0142 — A `static readonly` field is assigned only in static initialization: the instance-constructor site is withdrawn

| | |
|---|---|
| **Status** | graduated-v0.327 — superseded — graduated v0.327 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option B; critique round 1 applied, 3 blocking and 9 non-blocking findings resolved, shared with the #5598 and #5603 rulings; landed with the v0.327 changelog entry) |
| **Tracking issue** | [#5577](https://github.com/s8lap/bezel-lang/issues/5577) (the question); the compiler tracker is filed at the v0.327 landing (see §7) |
| **Target module(s)** | **spec** — §22.8 (the placement sentence, the §7.4 cross-reference sentence, the checkable form's last sentence), Appendix C's §22.8 single-assignment row; **rationale** — §9.16 (one new paragraph); **code-generation reference** — §4.2's `ref`/`out` paragraph (its `static readonly` clause corrected in place). No change to §7.4's text, §22.12, §22.13 or §23.1, which already state the rule this proposal restores for fields. |
| **Written against** | spec **v0.325** at `b27728a` (main). Line numbers below are at that commit. |
| **Origin** | Proposal 0136 §2.3 filed §22.8's thrice-stated "not decided here" aside as one question ([#5577](https://github.com/s8lap/bezel-lang/issues/5577), v0.323). The owner's 2026-10-08 direction — "resolve and decide all known spec related issues; do proposals for issues that need them" — delegates the decision. The issue's own recommendation is **no**; this proposal adopts it. |

*Reference convention: unprefixed §-references are language-spec references at `b27728a` (v0.325). Quotations are verbatim. ▲ marks proposed text, landed nowhere. The programs below use only members the stdlib companion declares; none invents an API.*

---

## 0. Thesis

§22.8's placement sentence admits an assignment to a `static readonly` field "in a constructor body of the declaring type", which includes every *instance* constructor. Two rules already built on the narrower reading — §7.4's narrowing licence for `TypeName._s` and §22.8's own single-assignment rule, whose unit for a static is "one type initialization" — are unsound or undefined under the wider one. §23.1 already confines a `static readonly` **auto-property** to "the declaring type's own static initialization". This proposal makes the field rule say the same: a `static readonly` field is assigned only in its own static field initializer or in the static constructor. It is a tightening (process §2: a design change), and it is cheap: two test controls flip, no corpus case, no guide and no stdlib listing writes the withdrawn form, and the one reference text that states it (the code-generation reference's `ref`/`out` paragraph) is corrected.

## 1. Ground truth

### 1.1 The rule today (§22.8, line 4908)

> "a `static readonly` field is assignable only in its own static field initializer or in a constructor body of the declaring type, spelled as the bare name or `TypeName.F`."

and, same paragraph:

> "The placement rule above admits an assignment to a `static readonly` field in an *instance* constructor of the declaring type, which makes a plain `new C()` a writer of the `TypeName._s` shape §7.4 also narrows; whether `readonly` should instead confine such a field to its static field initializer and static constructor — and so what once-ness would mean across the several instances such a constructor could build — is an open question (#5577)."

The two rules the wide reading breaks:

- **§7.4 (line 983):** "A nullable **`readonly`** field — instance or `static` — participates in flow narrowing, because the blanket refusal's premise (unseen writers) does not exist for it: outside its construction window (§22.8's single-assignment rule) a `readonly` field has no writer on any path, through any alias". The tracked shape includes `TypeName._s`. Under the wide reading `if C._s <> null then new C(); C._s.M(); end` narrows across a writer: `new C()` may store `null` into `_s`.
- **§22.8's single-assignment rule (line 4910):** "For a `static readonly` field the unit is one type initialization: at most one assignment executes on any path through the static field initializer and the static constructor body (§22.13)." An instance-constructor store is outside that unit, so once-ness across the several instances one program constructs is undefined — a checkable form for it would have to count executions of a constructor over the program's whole run, which no body-local check can do (proposal 0105 §1's locality classes).

The rule the narrow reading already has for the sibling member kind — **§23.1 (line 5474):** a `readonly` auto-property's assignment "is admitted only through `this` for an instance auto-property, or in the declaring type's own static initialization for a `static readonly` one."

And the statics discipline it composes with — **§22.13:** "Once initialization begins for a type, all of its static fields are initialized before any of its methods can run on behalf of an outside observer"; the static constructor "runs after all static field initializers complete". Every rule about static storage keys on that one window.

### 1.2 The compiler today (`src/sema/ExprTypecheck.cpp` at `b27728a`)

The per-instance narrowing helper `isThisRootedLValueTarget` (line 7421) records the static case as deliberately untouched: "Static readonly carve-outs are NOT narrowed by this helper — the per-instance ruling does not reach them, and v0.270 records the 'instance constructor may assign a `static readonly` field' case as open. The four call sites handle the static-vs-instance split themselves (`isStatic ? inConstructor_ : inConstructor_ && thisRooted`)." The sites are the `:=` target check (`:7763`, `:8117`), the `ref`/`out` operand checks (`:8459`/`:8492`, `:8708`) and the field-symbol context test at `:23186`; a static readonly field is assignable in **any** constructor body of the declaring type (`inConstructor_` is true in instance and static constructors alike). The single-assignment pass (`static_field_no_ctor_definite_assignment_4015_test.cpp`'s commentary) counts a static field's assignments over the static constructor only, so an instance-constructor store is admitted and never counted — the undefined once-ness of §1.1 in practice.

Whether `bezelc` narrows `C._s` across `new C()` could not be probed in this environment (no build directory). It is not needed for the decision: §7.4's text licenses the narrowing, so a conforming compiler may do it, and the hole is in the text.

Two tests pin the admitted form as controls, in `tests/sema/readonly_per_instance_3316_test.cpp`: test 8 (`C._s := 7` in `C`'s instance constructor, expected to compile) and test 9 (bare `_s := 7`, same). Their own comment calls the carve-out "preserved" because "the case is recorded as open in v0.270". No other test, corpus case, guide fence, stdlib listing or code-generation-reference example writes a `static readonly` field from an instance constructor (scan of `tests/`, `tests/conformance/` and `docs/`: the only matches outside those two controls are mutable statics, static-constructor stores, or an instance field named like a static).

## 2. Options

### 2.1 Option A — status quo

Keep the placement sentence; patch §7.4 to exclude `TypeName._s` narrowing across any call that can run an instance constructor of `C`. Rejected: that exclusion is undecidable in practice (any call may construct a `C`, directly or through a factory, a virtual, a closure), so §7.4's static narrowing would be withdrawn in all but name; and the once-ness unit stays undefined. The issue's "alternative: yes" is this option.

### 2.1′ Option A′ — keep the instance-constructor site, drop `TypeName._s` narrowing outright

The honest form of A: admit the store and withdraw §7.4's narrowing for `static readonly` fields altogether, so no exclusion has to be computed. Sound, but rejected. It takes narrowing from **every** static field (two lit accept cases pin it today: `readonly_field_narrowing_2085__static_field_ne_null_accepted.bzl` and `readonly_field_narrowing_2116__bare_static_field_accepted.bzl`) to keep a store that only two test controls write. Its once-ness unit stays undefined, as in A, and field and auto-property still disagree (§23.1). Its only gain is a per-instance write to type-wide storage, which a `static constructor()` or a mutable static under §22.12 rule 2 already expresses.

### 2.2 Option B — confine to static initialization (recommended)

A `static readonly` field is assignable only in its own static field initializer or in the declaring type's static constructor body. An instance constructor is not an assignment site; the assignment there is rejected with §22.8's placement diagnostic, which names the static constructor as the remedy. §7.4's premise is restored verbatim (the construction window of a static field *is* one type initialization), and the single-assignment rule's unit already names exactly this window. Field and auto-property agree (§23.1).

### 2.3 Option C — admit the instance-constructor site, define once-ness as "the first instance construction"

A `static readonly` field assigned in an instance constructor would be assigned by the first constructor run and the assignment skipped afterwards. Rejected: it is a runtime check with a race (two domains constructing concurrently, §30.13), it contradicts "the field holds its one assigned value" for the narrowing licence (the field changes from the null watermark to a value at an unbounded moment), and nothing in C#, Java or Swift has the form — all three confine static readonly / final static / static let to static initialization.

## 3. Evaluation

| | A (status quo) | A′ (admit, drop static narrowing) | B (static initialization only) | C (first-construction once) |
|---|---|---|---|---|
| Soundness (§7.4 narrowing of `TypeName._s`) | hole, or narrowing withdrawn | sound (no narrowing) | restored | hole (unbounded assignment moment) |
| Once-ness defined | no | no | yes (the existing unit) | runtime, racy |
| Simplicity | one more exclusion in §7.4 | §7.4 loses a root shape | one sentence narrowed; one sentence deleted | a new runtime mechanism |
| Consistency with §23.1 (auto-property) | disagrees | disagrees | agrees | disagrees |
| Migration reach (tests / corpus / docs) | none | two lit accept cases (static narrowing) flip | two test controls flip; no corpus case; no doc example; one code-generation-reference clause corrected | runtime semantics change |
| Precedent | — | — | C# `static readonly`, Java `static final`, Swift `static let` | none |

## 4. Decision

**Option B.** The ruling, in one sentence: *a `static readonly` field is assignable only in its own static field initializer or in the declaring type's static constructor body; an instance constructor of the declaring type is not an assignment site for it.*

- **Kernel classification (process §9):** no contact with §28/§30.5; this is a §22 placement rule.
- **Body-locality (proposal 0105 §1):** **local** — the check reads the enclosing member's kind (static constructor, static field initializer, or neither) and the target's `static readonly` declaration, as today's placement check does.
- **Diagnostic:** the existing placement rejection ("cannot assign to readonly field `_s` …"), now also at an instance constructor of the declaring type, naming the static constructor as the remedy; `scripts/pregate_stale_needles.py` finds no needle keyed on the admitted form.
- **Remedies for a program that wrote the withdrawn form:** move the assignment into `static constructor()`; or, where the intent is per-instance bookkeeping on a type-wide counter, declare the static **mutable** under §22.12 rule 2 (which warns at the declaration for the value-typed form (iii), exactly as it does for every mutable static today).

## 5. The exact normative edits (▲)

1. **§22.8, the placement sentence** (line 4908): "a `static readonly` field is assignable only in its own static field initializer or in a constructor body of the declaring type, spelled as the bare name or `TypeName.F`." → ▲ "a `static readonly` field is assignable only in its own static field initializer or in the declaring type's **static** constructor body, spelled as the bare name or `TypeName.F` — an instance constructor of the declaring type is not an assignment site for it, so no `new C()` writes `C._s`."
2. **§22.8, the §7.4 cross-reference sentence and the open-question sentence** (same line): both replaced by ▲ "This is what §7.4's narrowing licence assumes when it states that a `readonly` field, instance or `static`, has no writer on any path, through any alias, outside its construction window: that window is one instance construction for an instance field and one type initialization (§22.13) for a static one, and no site outside it — for a static field, an instance constructor of the declaring type included — can write the field."
3. **§22.8, the checkable form's last sentence** (line 4912): "It does not reach the open question above (an instance constructor's assignment to a `static readonly` field)." → deleted.
4. **Appendix C, the §22.8 single-assignment row** (line 11899): after "through one type initialization" ▲ "— and, for a `static readonly` field, only in its own static field initializer or the declaring type's static constructor, never in an instance constructor".
5. **Rationale §9.16**: ▲ one paragraph, *A `static readonly` field is assigned only in static initialization (v0.327)*, recording options A and C and the C#/Java/Swift precedent.
6. **Code-generation reference §4.2, the `ref`/`out` paragraph** (the clause that already states the `static readonly` context): "its context is its own static initializer or a constructor body of the declaring type" → ▲ "its context is its own static initializer or the declaring type's `static constructor()`, never an instance constructor". A correction of an existing clause, not a new one, so the reference's maintenance rule (no clause appended to prose) is not engaged, and no duplicate table row is added.

The §7.4 text is unchanged: its exclusion list ("the declaring type's constructors, field initializers, static constructor, or static field initializers") stays a superset of the static field's window, which is sound and conservative; narrowing `C._s` inside an instance constructor of `C` is not newly admitted by this proposal (recorded as liftable in §7 below).

## 6. Declined alternatives and `DECLINED.md` rows

| Topic | Verdict class | Authoritative record | Reopening bar |
|---|---|---|---|
| An instance constructor of the declaring type as an assignment site for a `static readonly` field (§22.8's placement rule read per-type), with or without §7.4's `TypeName._s` narrowing | `declined-at-proposal` | proposal 0142 §2.1, §2.1′, §2.3; changelog v0.327 (#5577) | A body-local checkable form of once-ness across instance constructions, and a §7.4 narrowing exclusion for `TypeName._s` that is decidable at the use site — neither exists; §23.1 already confines the auto-property form, so reopening must move both member kinds together. |
| Runtime "first construction assigns" once-ness for a `static readonly` field (option C) | `declined-at-proposal` | proposal 0142 §2.3 | A race-free, narrowing-compatible definition of the assignment moment under §30.13's parallel domains; no precedent language has one. |

## 7. Migration, corpus and trackers

- **Newly rejected (spec-ahead-of-impl):** an assignment, compound assignment, `++`/`--`, or `ref`/`out` binding of a `static readonly` field inside an **instance** constructor of the declaring type (`public constructor() C._s := 7; end constructor;` and the bare-name spelling). The compiler admits it today (`ExprTypecheck.cpp:7421–7425` and the four call sites); the tracker is filed at landing. Tests that flip, to be named in the fix commit: `readonly_per_instance_3316_test.cpp`'s two controls `ctor_type_qualified_static_readonly_assignment_accepted_spec_22_8` and `ctor_bare_static_readonly_assignment_accepted_spec_22_8` (the "static readonly carve-out preserved" cases), which become rejections with the §22.8 placement diagnostic.
- **Newly admitted:** none. (Liftable, recorded: narrowing `C._s` inside an instance constructor of `C`, which §7.4's exclusion list still refuses although no writer exists there any more.)
- **Rule corpus (`tests/conformance/rules/`):** no `s22_8_*` or `s7_4_*` case states the withdrawn form (`s22_8_0001`–`0008`, `s7_4_0001`–`0010` re-read); nothing to fix. Coverage gap recorded, not written under landing pressure: a reject case for the instance-constructor store and an accept case for the static-constructor store (the latter is pinned by `readonly_single_assignment_2187 / static_single_assign` and the lit `readonly_field_single_assignment_2187__static_single_accepted.bzl`).
- **Doc-example corpus:** no fence changes.
- **Code-generation reference:** its §4.2 `ref`/`out` paragraph stated the withdrawn site ("a constructor body of the declaring type") and is corrected (§5 item 6).
- **Test commentary:** `tests/sema/readonly_derived_ctor_reassignment_4276_test.cpp` quotes the old phrase in a comment (no needle); refresh it with the tracker's fix.
- **Guides:** no guide writes the withdrawn form (Guide 04's statics all carry initializers); no guide edit.
- **Related, unchanged:** #2114 (static readonly narrowing across generic instantiations) is a separate question and is not touched.

## 8. Self-critique (adversarial pass)

- *Does any stdlib or compiler-known type rely on an instance constructor seeding a static?* No: stdlib listings declare `static readonly` properties and fields with initializers or none; no listing shows a constructor body (stdlib bodies are not shown), and the semantics stated for `Environment`, `Console`, `CancellationToken.None` are static-initialization semantics.
- *Does the ruling reach a `this(...)`-chained or `base(...)` instance constructor?* Yes: every instance constructor, chained or not, is outside the static window. The static constructor can itself construct instances (`new C()` in `static constructor()`); their instance constructors still may not write `_s` — the write must be lexically in the static window, which is the body-local form.
- *Does it reach a static constructor of a **record**, **enum** or **choice**?* An enum has no static constructor (§10.5's list) and a choice declares what an enum does (§35.2), so for them the only site is the initializer — unchanged. A record's static constructor is admitted as a class's (`static_field_no_ctor_definite_assignment_4015_test.cpp` pins the double-assign rejection there).
- *Can a closure carry a store out of the window?* A lambda written in a static constructor body that assigns `_s` and is invoked after type initialization would write outside the window. This is not created by this proposal and is shared with instance fields (the compiler's `inConstructor_` flag is not reset for a lambda body, `ExprTypecheck.cpp:17831`); whether "in a constructor body" includes a nested lambda body is filed as a separate question at landing, not decided here.
- *Is there history narration in the proposed normative text?* No: the landed sentences state the current rule; the story is in this file and the changelog.
- *Does §7.4's exclusion list now contradict §22.8?* No; it is wider than the window and says so by listing sites, not by naming the window.
