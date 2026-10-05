// fix #5392 (§11.2/§12.1, v0.314): an implementing member or override must
// declare exactly as many type parameters of its own as the matched declaration.
// The two lists correspond by position; type parameters at the same position
// are considered matching regardless of name.
//
// ROOT CAUSE
// ───────────
// Two bugs, one in interface implementation and one in override validation:
//
// Interface implementation: `paramsMatchObligation` used `typesMatch` which
// compares by spelling. A renamed `Show<V>(x: V)` impl vs `Show<U>(x: U)`
// iface was falsely rejected because `typesMatch` saw V vs U as different.
//
// Override validation: `overrideParamsMatch` had no generic-param count check.
// A count-mismatched override (e.g., `Do<U,V>(x: U)` vs `Do<T>(x: T)`) was
// silently accepted because the parameter comparison failed but returned false
// (no match found) rather than rejecting the override outright.
//
// THE FIX
// ───────
// 1. `overrideParamsMatch`: added genericParams.size() equality check.
//    Without this, permuted and count-mismatched overrides were silently
//    accepted because overrideParamsMatch returned false only on param type
//    mismatch; a different type-parameter count means there is no positional
//    correspondence to compare under.
//
// 2. `paramsMatchObligation` (interface impl candidate filter): when both
//    the interface's substituted and the impl's parameter types are
//    declaration-less Named refs (method-level type params, identified by
//    declaration == nullptr), accept regardless of spelling. This implements
//    positional correspondence for renamed interface implementations.
//
// 3. `checkAbstractImplementations`: added explicit count check for the
//    impl's genericParam count vs the interface member's count. Reports a
//    clear diagnostic when counts differ.
//
// 4. `checkAbstractImplementations` parameter verification: same positional
//    correspondence check for method-level type params in the actual
//    parameter-type comparison (not just in the candidate filter).

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"       // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ════════════════════════════════════════════════════════════════════════════
// ACCEPT cases — renamed implementations and overrides must be accepted
// ════════════════════════════════════════════════════════════════════════════

// ── Interface implementation: same-name type parameter ────────────────────

BEZEL_TEST(interface_impl_type_param_correspondence_5392,
           same_name_interface_impl_accepted) {
    // `Show<U>(x: U)` impl vs `Show<U>(x: U)` iface — same name, trivially
    // corresponding.
    auto r = runAot(
        "module M; "
        "interface IShow "
        "  method Show<U>(x: U): int; "
        "end IShow; "
        "class C : IShow "
        "  public method Show<U>(x: U): int return 0; end Show; "
        "end C; "
        "class Cm public static method Main(): int return 0; end Main; end Cm;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── Interface implementation: renamed type parameter ───────────────────────

BEZEL_TEST(interface_impl_type_param_correspondence_5392,
           renamed_interface_impl_accepted) {
    // fix #5392: `Show<V>(x: V)` correctly implements `Show<U>(x: U)`.
    // The two type-parameter lists correspond by position regardless of name.
    auto r = runAot(
        "module M; "
        "interface IShow "
        "  method Show<U>(x: U): int; "
        "end IShow; "
        "class C : IShow "
        "  public method Show<V>(x: V): int return 0; end Show; "
        "end C; "
        "class Cm public static method Main(): int return 0; end Main; end Cm;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── Override: renamed type parameter ─────────────────────────────────────

BEZEL_TEST(interface_impl_type_param_correspondence_5392,
           renamed_override_accepted) {
    // `Echo<U>(x: U)` correctly overrides `Echo<T>(x: T)` — positional
    // correspondence holds regardless of the name.
    auto r = runAot(
        "module M; "
        "class Base "
        "  public virtual method Echo<T>(x: T): string return x.ToString(); "
        "  end Echo; "
        "end Base; "
        "class Der : Base "
        "  public override method Echo<U>(x: U): string "
        "    return \"d\" + x.ToString(); "
        "  end Echo; "
        "end Der; "
        "class C public static method Main(): int "
        "  using var d := new Der(); "
        "  var b: Base := d; "
        "  return b.Echo(7).Length + b.Echo(\"ab\").Length; "  // "d7" + "dab" = 2+3=5
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ════════════════════════════════════════════════════════════════════════════
// REJECT cases — count-mismatched and non-generic implementations
// ════════════════════════════════════════════════════════════════════════════

// ── Interface implementation: count-mismatched (too many) ──────────────────

BEZEL_TEST(interface_impl_type_param_correspondence_5392,
           count_mismatch_too_many_impl_rejected) {
    // `Show<V,W>(x: V)` does NOT implement `Show<U>(x: U)` — the interface
    // declares 1 type parameter, the impl declares 2. No positional
    // correspondence exists.
    auto r = runAot(
        "module M; "
        "interface IShow "
        "  method Show<U>(x: U): int; "
        "end IShow; "
        "class C : IShow "
        "  public method Show<V,W>(x: V): int return 0; end Show; "
        "end C; "
        "class Cm public static method Main(): int return 0; end Main; end Cm;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the interface member declares 1 type parameter"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "implementing method declares 2"));
}

// ── Interface implementation: count-mismatched (too few) ───────────────────

BEZEL_TEST(interface_impl_type_param_correspondence_5392,
           count_mismatch_too_few_impl_rejected) {
    // `Show(x: T)` on `class C<T>` does NOT implement `Show<U>(x: U)` —
    // the impl declares 0 type parameters, the interface member declares 1.
    // The impl uses the class's own type parameter T, which is NOT
    // positionally corresponding to the interface's U.
    auto r = runAot(
        "module M; "
        "interface IShow "
        "  method Show<U>(x: U): int; "
        "end IShow; "
        "class C<T> : IShow "
        "  public method Show(x: T): int return 0; end Show; "
        "end C; "
        "class Cm public static method Main(): int return 0; end Main; end Cm;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the interface member declares 1 type parameter"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "implementing method declares 0"));
}

// ── Override: count-mismatched (too many) ─────────────────────────────────

BEZEL_TEST(interface_impl_type_param_correspondence_5392,
           count_mismatch_override_too_many_rejected) {
    // `Do<U,V>(x: U)` does NOT correctly override `Do<T>(x: T)` — the base
    // declares 1 type parameter, the override declares 2. Positional
    // correspondence cannot be established.
    auto r = runAot(
        "module M; "
        "abstract class Base "
        "  public abstract method Do<T>(x: T): int; "
        "end Base; "
        "class Derived : Base "
        "  public override method Do<U,V>(x: U): int return 0; end Do; "
        "end Derived; "
        "class Cm public static method Main(): int return 0; end Main; end Cm;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "does not match"));
}

// ── Override: count-mismatched (too few) ─────────────────────────────────

BEZEL_TEST(interface_impl_type_param_correspondence_5392,
           count_mismatch_override_too_few_rejected) {
    // A non-generic override does NOT correctly override a generic virtual.
    auto r = runAot(
        "module M; "
        "abstract class Base "
        "  public abstract method Do<T>(x: T): int; "
        "end Base; "
        "class Derived : Base "
        "  public override method Do(x: int): int return 0; end Do; "
        "end Derived; "
        "class Cm public static method Main(): int return 0; end Main; end Cm;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "does not match"));
}
