// Regression tests for #1091 (§8.1/§14.2): GetHashCode/Equals on a bare
// unconstrained generic type parameter `T` returned 0 (type-erasure
// miscompile). tryLowerValueSemanticsCall tested the receiver for a CONCRETE
// primitive/record/enum before resolving the type parameter through the active
// monomorphization, so `x.GetHashCode()` / `x.Equals(y)` with `x: T` (Named, no
// declaration) fell through to a 0 stub — while ToString on the same
// unconstrained T already worked (it resolves T→concrete via genericSubst).
// The fix mirrors the ToString path: resolve T→concrete and route to
// lowerValueSemMethodOn (primitive/record/enum) or lowerObjectGetHashCode/
// lowerObjectEquals (class/interface/string/Object). Found by sweep #9.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1091: GetHashCode on an unconstrained `T` bound to int returns the value's
// hash (the value itself), not 0.
BEZEL_TEST(generic_unconstrained_objmethod_1091, int_gethashcode) {
    BEZEL_EXPECT_EQ(run(
        "module M; class U public static method H<T>(x: T): int => x.GetHashCode(); end U; "
        "class C public static method Main(): int return U.H(7); end Main; end C;"), 7);
}

// #1091: GetHashCode on T bound to string returns a non-zero content hash.
BEZEL_TEST(generic_unconstrained_objmethod_1091, string_gethashcode_nonzero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class U public static method H<T>(x: T): int => x.GetHashCode(); end U; "
        "class C public static method Main(): int "
        "  var h := U.H(\"hi\"); return (1 if h <> 0 else 0); "
        "end Main; end C;"), 1);
}

// #1091: GetHashCode on T bound to a record returns a non-zero structural hash.
BEZEL_TEST(generic_unconstrained_objmethod_1091, record_gethashcode_nonzero) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(X: int, Y: int); "
        "class U public static method H<T>(x: T): int => x.GetHashCode(); end U; "
        "class C public static method Main(): int "
        "  var h := U.H(new Pt(3, 4)); return (1 if h <> 0 else 0); "
        "end Main; end C;"), 1);
}

// #1091: Equals on an unconstrained T (value binding) compares by value.
BEZEL_TEST(generic_unconstrained_objmethod_1091, value_equals) {
    BEZEL_EXPECT_EQ(run(
        "module M; class U public static method Eq<T>(x: T, y: T): bool => x.Equals(y); end U; "
        "class C public static method Main(): int "
        "  return (1 if U.Eq(5, 5) else 0) + (10 if U.Eq(5, 9) else 0); "  // 5==5 →1, 5==9 →0
        "end Main; end C;"), 1);
}

// #1091: Equals on T bound to a class uses reference identity (no override).
BEZEL_TEST(generic_unconstrained_objmethod_1091, class_equals_ref_identity) {
    BEZEL_EXPECT_EQ(run(
        "module M; class K public _v: int := 0; end K; "
        "class U public static method Eq<T>(x: T, y: T): bool => x.Equals(y); end U; "
        "class C public static method Main(): int "
        "  using var a := new K(); using var b := new K(); "
        "  return (10 if U.Eq(a, a) else 0) + (1 if U.Eq(a, b) else 0); "  // self →10, distinct →0
        "end Main; end C;"), 10);
}

// Regression guard: ToString on an unconstrained T (the path that already
// worked) is unchanged.
BEZEL_TEST(generic_unconstrained_objmethod_1091, tostring_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class U public static method S<T>(x: T): int => x.ToString().Length; end U; "
        "class C public static method Main(): int return U.S(42); end Main; end C;"), 2);
}
