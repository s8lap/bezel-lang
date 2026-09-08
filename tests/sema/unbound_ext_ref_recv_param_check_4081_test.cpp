// Spec-derived regression tests for fix #4081 — §27.6 / §5.1 (v0.294):
// an UNBOUND extension-method reference (`Ext.Method`, §27.6, type-name
// qualifier) assigned to an explicitly-typed function local whose
// PARAMETER TYPE does not match the extension's actual receiver type is
// accepted with NO diagnostic and SIGSEGVs at runtime when the method
// body reads the mismatched receiver (`this`).
//
// §27.6, verbatim: "The reference's signature must be compatible with the
// target function type: its parameter and return types unify with the
// target's under the standard implicit-conversion rules (§5.1)". `int`
// is not implicitly convertible to a user class type (`ExtTargetErrU4070`
// in the issue repro, any user `class C` here) — there is no such §5.1
// conversion — so this assignment must be rejected at compile time.
//
// Pre-fix (verified on `511e576`, the v0.294 baseline): every ACCEPT row
// compiled clean (the body-free shape hid the gap; the reference's
// `() -> int` type was accepted against a `(int) -> int` target because
// the formation-site shape check was entirely skipped — `skipShapeCheck =
// freshReferenceFormation || freshLambda` admitted every pair), and every
// REJECT row compiled clean too (no diagnostic, no compile error) and
// crashed with SIGSEGV at the receiver parameter when called. With the
// fix each REJECT row reports its spec-cited §5.1 diagnostic, and every
// ACCEPT row stays pinned by exit-code.
//
// AXIS GRID. The formation-site shape check operates on (a) parameter
// slots contravariantly (target_param → source_param must be §5.1-
// convertible) and (b) the return slot covariantly (source_return →
// target_return must be §5.1-convertible). The accept controls cover the
// four un-rejected shapes each axis must preserve:
//
//   * Identity parameter slot: a method whose receiver is the same type
//     as the target's parameter is accepted (the §27.6 identity case).
//   * Primitive widening parameter slot: a method whose receiver is
//     `int` formed against a `(long) -> int` target — the target's `long`
//     implicitly narrows to source's `int` only when the source is
//     WIDER than the target; here the source `int` is NARROWER than the
//     target `long`, so contravariance rejects (a control: the negative
//     arm of the primitive widening axis).
//   * Subtype receiver parameter slot: a method on a base-class receiver
//     formed against a derived-class parameter — accepted via contravariance
//     (a `(Animal) -> ()` method formed as `(Dog) -> ()` because Dog is a
//     subtype of Animal and the target's Dog can be passed where the
//     source's Animal is expected).
//   * Widening return slot: a method `(int) -> int` formed against
//     `(int) -> long` — accepted via covariance (the int return widens
//     to long).
//
// per §27.6 / §5.1 / §27.1.1 (issue #4081, spec v0.294).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// §5.1 ("... is not implicitly convertible to the target type (§5.1)").
// The formation-site shape check reuses the existing §5.1 reject message
// from `ExprTC::checkAssignable`'s reportNotConvertible() helper, so the
// wording is the same as every other §5.1 reject site.
bool hasNotImplicitlyConvertible(
    const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

void expectRejectedByNotImplicitlyConvertible(const std::string& src) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

void expectAcceptedByValue(const std::string& src, int expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

}  // namespace

// REJECT (cx1, §4081 repro): an unbound extension-method reference whose
// declared receiver type (a user class `ExtTargetErrU4081`) is assigned
// to a function local whose parameter type is `int`. `int` is NOT
// implicitly convertible to `ExtTargetErrU4081` (§5.1 has no path between
// a primitive and a user-declared class), so the §27.6 formation-site
// shape check (§5.1 unification) must reject. Pre-fix: compiles clean,
// runs to SIGSEGV at `f(1)` when the body reads `this` (it dereferences
// `int` bits as if they were an `ExtTargetErrU4081`).
// // per §27.6 / §5.1 (issue #4081)
BEZEL_TEST(unbound_ext_ref_recv_param_check_4081,
           class_recv_against_prim_param_rejected_spec_27_6_5_1) {
    expectRejectedByNotImplicitlyConvertible(
        "module M; "
        "public class ExtTargetErrU4081 "
        "  public n: int := 0; "
        "  public constructor(n: int) this.n := n; end constructor; "
        "  destructor end destructor; "
        "end ExtTargetErrU4081; "
        "extension ExtTargetErrU4081 "
        "  public method Val(): int => this.n; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (int) -> int := ExtTargetErrU4081.Val; "
        "    return f(1); "
        "  end Main; "
        "end C;");
}

// REJECT (cx2, symmetry): the reverse mismatch — `int` is the receiver
// type, `ExtTargetErrU4081b` is the parameter slot. `ExtTargetErrU4081b`
// is also not implicitly convertible from `int` (no §5.1 path between
// primitive and user class in either direction). Pins the formation-site
// shape check works in both contravariant directions, not just the one
// the issue repro exercises.
// // per §27.6 / §5.1 (issue #4081)
BEZEL_TEST(unbound_ext_ref_recv_param_check_4081,
           prim_recv_against_class_param_rejected_spec_27_6_5_1) {
    expectRejectedByNotImplicitlyConvertible(
        "module M; "
        "public class ExtTargetErrU4081b "
        "  public n: int := 0; "
        "  public constructor(n: int) this.n := n; end constructor; "
        "  destructor end destructor; "
        "end ExtTargetErrU4081b; "
        "extension int "
        "  public method Zero(): int => 0; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (ExtTargetErrU4081b) -> int := int.Zero; "
        "    using var t := new ExtTargetErrU4081b(7); "
        "    return f(t); "
        "  end Main; "
        "end C;");
}

// ACCEPT (ax1, identity): the method reference's receiver type matches
// the target's parameter slot exactly. §5.1 identity is the formation
// site's accept case; the §5.1 unification predicate recognises the
// structural equality (functionSlotTypeRefEqual) and accepts.
// // per §27.6 (issue #4081)
BEZEL_TEST(unbound_ext_ref_recv_param_check_4081,
           class_recv_class_param_identity_accepted_spec_27_6) {
    expectAcceptedByValue(
        "module M; "
        "public class ExtTargetOKU4081 "
        "  public n: int := 0; "
        "  public constructor(n: int) this.n := n; end constructor; "
        "  destructor end destructor; "
        "end ExtTargetOKU4081; "
        "extension ExtTargetOKU4081 "
        "  public method Val(): int => this.n; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (ExtTargetOKU4081) -> int := ExtTargetOKU4081.Val; "
        "    using var t := new ExtTargetOKU4081(42); "
        "    return f(t); "
        "  end Main; "
        "end C;", 42);
}

// ACCEPT (ax2, contravariance): a method on a base-class receiver formed
// against a derived-class parameter. A `(Animal) -> ()` method is
// accepted where `(Dog) -> ()` is expected because Dog IS-A Animal
// (every Dog can be passed where an Animal is expected), the standard
// contravariance of function-type parameter slots. §5.1's reference
// conversion path covers this — Animal is a supertype of Dog, so
// `implicitlyConvertible(Dog, Animal)` returns true at the contravariant
// parameter slot.
// // per §27.6 / §5.1 / §27.1.1 contravariance (issue #4081)
BEZEL_TEST(unbound_ext_ref_recv_param_check_4081,
           base_recv_derived_param_contravariance_accepted_spec_27_6_27_1_1) {
    expectAcceptedByValue(
        "module M; "
        "public class AnimalU4081 "
        "  public n: int := 7; "
        "  public constructor(n: int) this.n := n; end constructor; "
        "  destructor end destructor; "
        "end AnimalU4081; "
        "public class DogU4081: AnimalU4081 "
        "  public constructor() base(11); end constructor; "
        "  destructor end destructor; "
        "end DogU4081; "
        "extension AnimalU4081 "
        "  public method N(): int => this.n; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (DogU4081) -> int := AnimalU4081.N; "
        "    using var d := new DogU4081(); "
        "    return f(d); "
        "  end Main; "
        "end C;", 11);
}

// ACCEPT (ax3, return covariance widening): a `(int) -> int` method
// reference formed against a `(int) -> long` target. The int return
// widens to long at the formation site (§5.1's primitive widening
// table: int → long is implicit). The parameter slots are identical
// (int ↔ int), so only the return-slot axis is exercised — pins that
// the formation-site shape check applies the covariant direction
// (source_return → target_return) and accepts primitive widening.
// // per §27.6 / §5.1 / §27.1.1 covariance (issue #4081)
BEZEL_TEST(unbound_ext_ref_recv_param_check_4081,
           int_return_long_param_return_widening_accepted_spec_27_6_27_1_1) {
    expectAcceptedByValue(
        "module M; "
        "extension int "
        "  public method Doubled4081(): int => this * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (int) -> long := int.Doubled4081; "
        "    return int(f(21)); "
        "  end Main; "
        "end C;", 42);
}

// REJECT (ax4, parameter contravariance narrowing): the formation site's
// parameter slot MUST be contravariant — a `(int) -> int` method formed
// against a `(long) -> int` target is REJECTED. The source's `int` is
// NARROWER than the target's `long`; contravariance requires the target's
// parameter type to convert to the source's (`implicitlyConvertible
// (long, int)` is false — narrowing is not a §5.1 conversion). This is
// the negative arm of the primitive axis so a future loosening cannot
// accidentally accept primitive narrowing at formation.
// // per §27.6 / §5.1 / §27.1.1 contravariance (issue #4081)
BEZEL_TEST(unbound_ext_ref_recv_param_check_4081,
           int_recv_long_param_narrowing_rejected_spec_27_6_27_1_1) {
    expectRejectedByNotImplicitlyConvertible(
        "module M; "
        "extension int "
        "  public method Doubled4081b(): int => this * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (long) -> int := int.Doubled4081b; "
        "    return 0; "
        "  end Main; "
        "end C;");
}
