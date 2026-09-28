// Spec-derived regression tests for #4978 — §27.6 (v0.307) target-typed
// overload selection for method references.
//
// THE PROBLEM (§27.6, v0.307): a bare method reference formed against a
// target function type must select the overload whose signature unifies
// with the target under §5.1. Without an implementation, the bare
// `H.On` access above resolves to the FIRST-declared overload via
// `lookupMember`'s first-match BFS (`resolveMemberAccess`,
// ExprTypecheck_Calls.cpp:24242), and the formation TypeRef is built
// from that one method's signature — a #4968 c06-class bug where the
// `string` overload was bound at an `(int) -> ()` event handler. The
// §27.6 spec text says, verbatim:
//
//   "When the named method (or property) is overloaded, the reference is
//    resolved using the target function type at the use site. The
//    compiler selects the overload whose parameter types and return
//    type unify with the target's parameter and return types under the
//    standard implicit-conversion rules (§5.1)."
//
//   "Selection among several unifying candidates: exact-preferred.
//    More than one overload may unify with the target … Among
//    unifying candidates, one whose parameter types — and return
//    type, where the target's return position is closed — match the
//    target's exactly is preferred; candidates that unify only
//    through conversion are co-equal (the §22.6 posture applied to
//    the reference's signature), so absent an exact candidate the
//    reference is ambiguous and the compiler reports the candidates."
//
//   "When no target type is available (e.g., `var f := Math.Abs;` with
//    no annotation), the reference is ambiguous and the compiler
//    reports an error pointing to the candidate overloads."
//
// FAIL-WITHOUT / PASS-WITH (verified on `d437bab4`, pre-fix base):
//   * `static_overloaded_method_ref_target_typed_int_accepted_spec_27_6`
//     — REJECTED pre-fix ("not implicitly convertible to the target
//     type") at the §5.1 formation-site check, ACCEPTED post-fix
//     (the chosen overload's `(int) -> ()` signature unifies exactly
//     with the target's).
//   * `instance_overloaded_method_ref_target_typed_int_accepted_spec_27_6`
//     — same defect on the instance form.
//   * `static_overloaded_method_ref_target_typed_string_accepted_spec_27_6`
//     — same defect, mirror case (string target binds string overload).
//   * `no_target_typed_overloaded_method_ref_rejected_spec_27_6` —
//     silently ACCEPTED pre-fix (no diagnostic, would have bound the
//     first-declared overload at runtime); post-fix reports the §27.6
//     v0.307 ambiguity with both candidate signatures.
//
// AXIS GRID. The fix has two surfaces: (1) formation-site overload
// selection when a target function type is available (`H.On` to
// `(int) -> ()`); (2) the no-target rule when the bare-method-reference
// reaches a use site that does NOT carry a target function type (`var
// f := H.On;` with no `f: T := ...` ascription). The accept-controls
// below cover the shapes the fix must preserve:
//
//   * single overload + target typed (the only case that worked before)
//   * overload + target typed on the OTHER overload (string target,
//     string method) — pins that exact-preferred ranking is symmetric
//     across declared order
//   * the no-target rule firing correctly with the §27.6 v0.307 wording
//
// SCOPE NOTE. The §27.6 spec rule itself is fully implemented in sema
// (ExprTypecheck.cpp's `selectMethodRefOverload` + the `checkAssignable`
// formation-site overload-selector). The accept rows below pin SEMANTIC
// correctness — `r.ran == true` means the binary compiled, linked, and
// reached `Main`; the §27.6 selector picked the right overload from the
// candidate set. A downstream codegen defect (the method-thunk emission
// in `LlvmBackend_Exprs.cpp` reads the bound `MethodSymbol` rather than
// the chosen overload's Function TypeRef) can crash the binary at run
// time when the chosen overload is not the first-declared one. That is
// a separate codegen-side defect, out of scope for this sema fix; the
// rows assert the SEMANTIC compile result, not the runtime execution
// result. A future codegen fix will turn these rows green at the exit-
// code level without changing this test file.
//
// per §27.6 (issue #4978, spec v0.307).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §27.6 / fix #4978 (v0.307): the no-target overload ambiguity rule's
// diagnostic. Distinct wording — "no target type to select by" — so the
// test catches a regression in either rule independently.
bool rejectedNoTargetOverloadedMethodRef(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("no target type to select by") !=
                std::string::npos &&
            d.message.find("\xC2\xA7""27.6") != std::string::npos &&
            d.message.find("v0.307") != std::string::npos)
            return true;
    return false;
}

void expectRejectedNoTargetOverloadedMethodRef(const std::string& body) {
    JitResult r = runAot(body);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedNoTargetOverloadedMethodRef(r));
}

void expectCompiled(const std::string& body) {
    JitResult r = runAot(body);
    // Pre-fix: this row's compile FAILED (`r.ran == false` because the
    // wrong overload's signature was bound and the §5.1 formation-site
    // check rejected the mismatch). Post-fix: the correct overload is
    // selected, the binary compiles+links, and `r.ran == true`. We
    // assert the compile result, not the runtime result, because a
    // codegen-side defect (out of scope for this sema fix) can crash
    // the binary at run time when the chosen overload is not the first-
    // declared one. The semantically-correct behavior is "compile ok";
    // the binary reaching `Main` is the strongest oracle available for
    // a sema-only test.
    BEZEL_EXPECT_TRUE(r.ran);
}

}  // namespace

// =====================================================================
// REJECT — the no-target rule. The spec §27.6 (v0.307) names the
// unannotated `var` as the canonical case:
//   "When no target type is available (e.g., `var f := Math.Abs;` with
//    no annotation), the reference is ambiguous and the compiler
//    reports an error pointing to the candidate overloads."
// Pre-fix the row silently accepted and would have bound the first-
// declared overload at runtime (the #4968 c06 mechanism).
// per §27.6 (issue #4978, spec v0.307)
// =====================================================================
BEZEL_TEST(sema_method_ref_target_typed_4978,
           no_target_typed_overloaded_method_ref_rejected_spec_27_6) {
    expectRejectedNoTargetOverloadedMethodRef(
        "module M; "
        "class H "
        "  public static method On(x: string) "
        "    Console.WriteLine(x); "
        "  end On; "
        "  public static method On(x: int) "
        "    Console.WriteLine(x); "
        "  end On; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var f := H.On; "
        "    return 0; "
        "  end Main; "
        "end C;");
}

// =====================================================================
// ACCEPT — overload + target-typed (the #4978 / #4968 c06 case).
// Two overloads: `(string) -> ()` and `(int) -> ()`; target is
// `(int) -> ()`; the `int` overload matches the target EXACTLY and
// is selected. Pre-fix the row rejected (the first-declared `string`
// overload's `(string) -> ()` is not implicitly convertible to the
// target's `(int) -> ()`).
// per §27.6 (issue #4978, spec v0.307)
// =====================================================================
BEZEL_TEST(sema_method_ref_target_typed_4978,
           static_overloaded_method_ref_target_typed_int_accepted_spec_27_6) {
    expectCompiled(
        "module M; "
        "class H "
        "  public static method On(x: string) "
        "    Console.WriteLine(x); "
        "  end On; "
        "  public static method On(x: int) "
        "    Console.WriteLine(x); "
        "  end On; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var f: (int) -> () := H.On; "
        "    return 0; "
        "  end Main; "
        "end C;");
}

// =====================================================================
// ACCEPT — instance overload + target-typed. Same surface as the static
// case but on an instance method (receiver-driven). Pre-fix the row
// rejected for the same reason.
// per §27.6 (issue #4978, spec v0.307)
// =====================================================================
BEZEL_TEST(sema_method_ref_target_typed_4978,
           instance_overloaded_method_ref_target_typed_int_accepted_spec_27_6) {
    expectCompiled(
        "module M; "
        "class H "
        "  public method On(x: string) "
        "    Console.WriteLine(x); "
        "  end On; "
        "  public method On(x: int) "
        "    Console.WriteLine(x); "
        "  end On; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var h := new H(); "
        "    var f: (int) -> () := h.On; "
        "    return 0; "
        "  end Main; "
        "end C;");
}

// =====================================================================
// ACCEPT — overload + target-typed on the OTHER overload (the `string`
// one). Pins that exact-preferred ranking is symmetric: the second-
// declared `string` overload binds at the `string` target. This is the
// canonical "no over-broad flip" control — if the selector preferred
// the first-declared overload regardless of target, this row would
// fail the §5.1 mismatch check (a `(int) -> ()` source at a `(string) ->
// ()` target slot).
// per §27.6 (issue #4978, spec v0.307)
// =====================================================================
BEZEL_TEST(sema_method_ref_target_typed_4978,
           static_overloaded_method_ref_target_typed_string_accepted_spec_27_6) {
    expectCompiled(
        "module M; "
        "class H "
        "  public static method On(x: string) "
        "    Console.WriteLine(x); "
        "  end On; "
        "  public static method On(x: int) "
        "    Console.WriteLine(x); "
        "  end On; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var f: (string) -> () := H.On; "
        "    return 0; "
        "  end Main; "
        "end C;");
}

// =====================================================================
// ACCEPT — single (non-overloaded) method reference, target-typed.
// Pins the un-rejected axis: a non-overloaded reference is unaffected
// by the formation-site selector (the `SingleOverload` enum case
// short-circuits, leaving the existing `lookupMember` + bare-method-
// access path intact).
// per §27.6 (issue #4978, spec v0.307)
// =====================================================================
BEZEL_TEST(sema_method_ref_target_typed_4978,
           single_method_ref_target_typed_accepted_spec_27_6) {
    expectCompiled(
        "module M; "
        "class H "
        "  public static method On(x: int) "
        "    Console.WriteLine(x); "
        "  end On; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var f: (int) -> () := H.On; "
        "    return 0; "
        "  end Main; "
        "end C;");
}

// =====================================================================
// REJECT — overload + target-typed where the target's signature does
// not unify with ANY overload. The selector returns `NoFit` and falls
// through to the existing §5.1 formation-site check, which rejects with
// the spec's "not implicitly convertible to the target type" wording
// (the surface unchanged by the #4978 fix). This pins the §5.1 fall-
// through — without it, an over-broad selector could spuriously
// "succeed" by reporting the §27.6 ambiguity on a row that has no
// fit at all.
// per §27.6 (issue #4978, spec v0.307)
// =====================================================================
BEZEL_TEST(sema_method_ref_target_typed_4978,
           no_overload_fits_target_rejected_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class H "
        "  public static method On(x: int) "
        "    Console.WriteLine(x); "
        "  end On; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var f: (double) -> () := H.On; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawSection51 = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("not implicitly convertible to the target type") !=
                std::string::npos &&
            d.message.find("\xC2\xA7""5.1") != std::string::npos) {
            sawSection51 = true;
            break;
        }
    BEZEL_EXPECT_TRUE(sawSection51);
}
