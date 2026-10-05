// Spec-derived regression test for issue #2742 — the #2713 MISATTRIBUTION
// defect in the deferred per-instantiation obligation recorders.
//
// §14.1 *Instantiation and its site* + §3.6 rule 2 "Per-instantiation
// evaluation for generics", v0.306. Family I1 (docs/proposals/
// defect-campaign-2026-09.md §2): the same "N copies of one predicate, one of
// them later taught something the others were not" mechanism the #4212 anchor
// unified on the TypeRef-walker side, here at the *site recorder* side.
//
// THE SHAPE. A rule that cannot be decided at a generic declaration (is this
// element type hashable? is it move-only?) is RECORDED there as a bare
// parameter NAME plus the enclosing type, and re-decided in a Pass 2 that
// resolves that name against `s.enclosingType->genericParams` and walks
// `instantiations_`. A generic METHOD may declare a type parameter that
// SHADOWS one of the enclosing type's — `class Box<T>` with `method M<T>()` —
// and nothing forbids it: Appendix A.3's `GenericParam ::= Variance? Identifier`
// imposes only the variance restriction, no §13/§14/§28.1 rule imposes name
// uniqueness, and there is no shadowing diagnostic anywhere in sema or the
// parser (not even for `method M<T, T>()`). Inside such a method the name
// denotes the METHOD's parameter; Pass 2 checks it against the TYPE's.
//
// #2713 fixed its own recorder (`owningElementSites_`) with a guard consulting
// `currentMethodGenericParams_`, and that guard was then hand-copied to
// `owningBufferSpanMoveOnlySites_` and `asTargetSites_`. It was never added to
// `hashContainerSites_` (this issue) or to `moveOnlyElementSites_` (the same
// defect, unreported until this lane measured it). Three copies, two omissions.
//
// ⚠ #2742 was filed "suspected pre-existing, NOT reproduced" — its author
// wrote a caveat saying a failing program had not been built and that whoever
// picked it up should "build the repro first and confirm the direction (false
// reject vs missed reject)". MEASURED on integration HEAD 00e10c0e: it is a
// FALSE REJECT. `Box2742<Unhashable2742>` calling `M2742<int>()` over a body
// holding `new Set<T>()` is rejected with
//
//   `Set` requires a hashable element type, but the type argument
//   `Unhashable2742` is not hashable (§3.6/§8.3): ...
//
// naming the ENCLOSING type's binding, while the `Set`'s actual element type is
// the method's `int` — which is hashable. A legal program is rejected. The
// sibling recorder behaves identically for the §28.11 move-only row.
//
// THE RESIDUAL, recorded rather than silently absorbed. Skipping the record
// means a generic METHOD's own binding is not re-checked at all — an
// ill-formed one is admitted. That is NOT a new hole: generic-method
// instantiations were never in `instantiations_` to begin with, so a
// NON-shadowing method parameter (`class Box<T>` + `method M<S>()`) already
// fell through Pass 2's `paramIdx` lookup and was skipped. #2713 chose exactly
// this trade-off on the record ("a false reject would be strictly worse than
// carrying it") and filed the residual as #2736(a); this fix extends the same
// trade-off to the two recorders that never got it. The residual is the same
// issue, not a new one.
//
// ⚠ OVER-SUPPRESSION is the hazard of a guard, so the controls below outnumber
// the bug rows and pin every neighbouring spelling that must STILL be rejected:
// a concrete non-hashable key (Pass 1), the same body in a NON-generic method
// (no shadowing — Pass 2's real job), and both of those for the move-only row.

#include "support/aot_support.hpp"
#include "test_harness.hpp"
#include "support/aot_result_asserts.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// `record Fld(...)` in FIELD form is not auto-hashable (§9.2) — it neither
// synthesizes nor declares equality. The POSITIONAL form `record Pos(a: int);`
// is. Both spellings are used below so the controls key on the real §9.2
// property rather than on the record's name.
const char* kUnhashable =
    "record Unhashable2742 "
    "  public a: int := 0; "
    "end Unhashable2742; ";

// §35.4 rule 1: a choice is OWNING if any case payload is `owned`. §15.3 makes
// an owning-classified choice one of the two move-only value kinds, which
// §28.11's element-slot-policy table rejects at instantiation.
const char* kOwningChoice =
    "class Cell2742 public tag: int := 3; end Cell2742; "
    "choice Own2742 Has(owned c: Cell2742), None; end Own2742; ";

bool rejectsMentioning(const JitResult& r, const char* needle) {
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ===========================================================================
// THE BUG, hash-container recorder (`hashContainerSites_` — #2742 proper).
// The method's `T` is `int`; `Box`'s `T` is a non-hashable record. The `Set`'s
// element type is the METHOD's `T`, so the program is legal and must run.
// ===========================================================================
// FIX #5393 (§14.1, v0.314): `M2742<T>()` inside `Box2742<T>` is now
// rejected at the type-parameter list. These programs no longer compile.
BEZEL_TEST(generic_method_param_shadowing_2742, hash_container_false_reject_spec_3_6) {
    auto r = runAot(
        std::string("module M; ") + kUnhashable +
        "class Box2742<T> "
        "  public method M2742<T>(): int "
        "    using var s := new Set<T>(); "
        "    return s.Count; "
        "  end M2742; "
        "end Box2742; "
        "class P "
        "  public static method Main(): int "
        "    using var b := new Box2742<Unhashable2742>(); "
        "    return b.M2742<int>(); "
        "  end Main; "
        "end P;");
    // #3892: a bare !r.ran cannot distinguish a real rejection from a
    // crash/signal/timeout/ASan report. Assert the diagnostic itself.
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "reuses the name of a type parameter of"));
}

// The `Dictionary` KEY slot reaches the same recorder through the other
// `hashRole` spelling, so it is pinned separately — one arm could be guarded
// and the other not.
BEZEL_TEST(generic_method_param_shadowing_2742, dictionary_key_false_reject_spec_3_6) {
    auto r = runAot(
        std::string("module M; ") + kUnhashable +
        "class Box2742<T> "
        "  public method M2742<T>(): int "
        "    using var d := new Dictionary<T, int>(); "
        "    return d.Count; "
        "  end M2742; "
        "end Box2742; "
        "class P "
        "  public static method Main(): int "
        "    using var b := new Box2742<Unhashable2742>(); "
        "    return b.M2742<int>(); "
        "  end Main; "
        "end P;");
    // #3892: a bare !r.ran cannot distinguish a real rejection from a
    // crash/signal/timeout/ASan report. Assert the diagnostic itself.
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "reuses the name of a type parameter of"));
}

// ===========================================================================
// THE SIBLING, element-category recorder (`moveOnlyElementSites_`). Same
// mechanism, §28.11 / §15.3 instead of §3.6/§8.3. Unreported before this lane;
// it is fixed by the same shared predicate, which is the whole point of
// hoisting rather than patching the one issue that happened to be filed.
// ===========================================================================
// FIX #5393 (§14.1, v0.314): `M2742<T>()` inside `Box2742<T>` is now
// rejected at the type-parameter list. These programs no longer compile.
BEZEL_TEST(generic_method_param_shadowing_2742, move_only_element_false_reject_spec_28_11) {
    auto r = runAot(
        std::string("module M; ") + kOwningChoice +
        "class Box2742<T> "
        "  public method M2742<T>(): int "
        "    using var a := new Array<T>(2); "
        "    return 100 + a.Count * 10 + a.Capacity; "
        "  end M2742; "
        "end Box2742; "
        "class P "
        "  public static method Main(): int "
        "    using var b := new Box2742<Own2742>(); "
        "    return b.M2742<int>(); "
        "  end Main; "
        "end P;");
    // #3892: a bare !r.ran cannot distinguish a real rejection from a
    // crash/signal/timeout/ASan report. Assert the diagnostic itself.
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "reuses the name of a type parameter of"));
}

// ===========================================================================
// REJECT CONTROLS — everything the guard must NOT suppress. Over-suppression
// is the failure mode of a fix like this: the guard keys on the NAME, and a
// name that the method does not bind must still reach Pass 2.
// ===========================================================================

// Pass 1, untouched: a CONCRETE non-hashable key at a plain call site. No
// generic method anywhere.
BEZEL_TEST(generic_method_param_shadowing_2742, control_concrete_unhashable_key_still_rejected) {
    auto r = runAot(
        std::string("module M; ") + kUnhashable +
        "class P "
        "  public static method Main(): int "
        "    using var s := new Set<Unhashable2742>(); "
        "    return s.Count; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(rejectsMentioning(r, "requires a hashable"));
    BEZEL_EXPECT_TRUE(rejectsMentioning(r, "Unhashable2742"));
}

// Pass 2's actual job, and the row that would go green if the guard were
// over-broad: the SAME body, in a NON-generic method, so `T` is unambiguously
// `Box`'s. The enclosing instantiation binds it to a non-hashable record and
// the program must still be rejected at the instantiation site.
BEZEL_TEST(generic_method_param_shadowing_2742, control_no_shadowing_still_rejected_spec_3_6) {
    auto r = runAot(
        std::string("module M; ") + kUnhashable +
        "class Box2742<T> "
        "  public method Plain(): int "
        "    using var s := new Set<T>(); "
        "    return s.Count; "
        "  end Plain; "
        "end Box2742; "
        "class P "
        "  public static method Main(): int "
        "    using var b := new Box2742<Unhashable2742>(); "
        "    return b.Plain(); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(rejectsMentioning(r, "requires a hashable"));
    BEZEL_EXPECT_TRUE(rejectsMentioning(r, "Unhashable2742"));
}

// The move-only row's two controls, matching the pair above.
BEZEL_TEST(generic_method_param_shadowing_2742, control_concrete_move_only_still_rejected) {
    auto r = runAot(
        std::string("module M; ") + kOwningChoice +
        "class P "
        "  public static method Main(): int "
        "    using var a := new Array<Own2742>(2); "
        "    return a.Length; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(rejectsMentioning(r, "owning-classified choice"));
}

BEZEL_TEST(generic_method_param_shadowing_2742, control_move_only_no_shadowing_still_rejected) {
    auto r = runAot(
        std::string("module M; ") + kOwningChoice +
        "class Box2742<T> "
        "  public method Plain(): int "
        "    using var a := new Array<T>(2); "
        "    return a.Length; "
        "  end Plain; "
        "end Box2742; "
        "class P "
        "  public static method Main(): int "
        "    using var b := new Box2742<Own2742>(); "
        "    return b.Plain(); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(rejectsMentioning(r, "Own2742"));
    BEZEL_EXPECT_TRUE(rejectsMentioning(r, "move-only"));
}

// ACCEPT CONTROL: a genuinely hashable key through the enclosing type's own
// parameter, with no generic method in sight. This passed before and must keep
// passing — it pins that the recorder still records, i.e. the guard did not
// disable the whole pass.
BEZEL_TEST(generic_method_param_shadowing_2742, control_hashable_enclosing_param_accepted) {
    auto r = runAot(
        "module M; "
        "record Pos2742(a: int); "
        "class Box2742<T> where T: hashable "
        "  public method Plain(): int "
        "    using var s := new Set<T>(); "
        "    return s.Count; "
        "  end Plain; "
        "end Box2742; "
        "class P "
        "  public static method Main(): int "
        "    using var b := new Box2742<Pos2742>(); "
        "    return b.Plain(); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
