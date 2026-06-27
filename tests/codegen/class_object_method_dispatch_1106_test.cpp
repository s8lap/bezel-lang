// Regression tests for #1106 part L1a (§8.3 / §8.4): Object-method dispatch on
// CLASS-typed receivers/operands.
//
// Three defects, all from the same gap — the codegen paths that route Object
// members (Equals/GetHashCode) and the `=`/`<>` operator only handled Object-
// and interface-typed receivers, not concrete class-typed ones:
//   - defect 1: the DEFAULT Object.Equals on a class with no override (no
//     MethodSymbol) fell through the lowerObjectEquals gate and mis-lowered to
//     a bare field load → returned false for `a.Equals(a)` (same reference).
//   - defect 3: that same i32 field-load result fed an `if` → "Branch condition
//     is not 'i1' type!" IR-verification crash.
//   - defect 2: `a = b` on a class did a raw ICmp pointer compare, bypassing
//     the class's `Equals` override (§8.4: for classes, `=` always invokes
//     Equals). Two distinct-but-equal instances compared unequal.
//
// Fixes (codegen): extend the Equals/GetHashCode Object-member fallback gate to
// named class receivers (mirroring the ToString gate), and route `=`/`<>` on
// user-class operands through lowerObjectEquals (vtable Equals-slot dispatch —
// the override, or the default reference-identity thunk; null-safe). Found by
// adversarial discovery sweep #9 (#1106).
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
// A class with a value-based Equals/GetHashCode override.
const char* kP =
    "class P "
    "  public _v: int := 0; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public override method Equals(o: Object?): bool "
    "    var q := o as P; if q = null then return false; end; "
    "    return _v = q!._v; "
    "  end Equals; "
    "  public override method GetHashCode(): int => _v; "
    "end P; ";
}  // namespace

// #1106 defect 1+3: the default Object.Equals on a class with NO override —
// `a.Equals(a)` (same reference) bound to a var then used in `if` compiles (was
// a `br i32` crash) and is true.
BEZEL_TEST(class_object_method_dispatch_1106, default_equals_self_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Plain public _v: int := 0; end Plain; "
        "class C public static method Main(): int "
        "  using var a := new Plain(); "
        "  var r := a.Equals(a); "                         // default ref-equality
        "  if r then return 1; else return 0; end; "       // 1 (was crash)
        "end Main; end C;"), 1);
}

// #1106 defect 1: default Object.Equals distinguishes distinct instances by
// reference (no override → reference identity).
BEZEL_TEST(class_object_method_dispatch_1106, default_equals_distinct_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Plain public _v: int := 0; end Plain; "
        "class C public static method Main(): int "
        "  using var a := new Plain(); using var b := new Plain(); "
        "  return (10 if a.Equals(a) else 0) + (1 if a.Equals(b) else 0); "  // 10
        "end Main; end C;"), 10);
}

// #1106 defect 2: `a = b` on a class with an overridden Equals dispatches the
// override (was: raw pointer compare → false for distinct equal instances).
BEZEL_TEST(class_object_method_dispatch_1106, eq_operator_uses_override) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kP +
        "class C public static method Main(): int "
        "  using var a := new P(5); using var b := new P(5); using var c := new P(9); "
        "  return (10 if a = b else 0) + (1 if a <> c else 0); "  // 5==5 →10, 5<>9 →1 = 11
        "end Main; end C;"), 11);
}

// #1106 defect 2 (negative): `a = c` with the override is false when values
// differ; `<>` is true.
BEZEL_TEST(class_object_method_dispatch_1106, eq_operator_override_unequal) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kP +
        "class C public static method Main(): int "
        "  using var a := new P(5); using var c := new P(9); "
        "  return (1 if a = c else 0) + (10 if a <> c else 0); "  // 0 + 10 = 10
        "end Main; end C;"), 10);
}

// Regression guard: `=` on a class with NO override keeps reference identity
// (a = a true, a = b false).
BEZEL_TEST(class_object_method_dispatch_1106, eq_operator_no_override_ref_identity) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Plain public _v: int := 0; end Plain; "
        "class C public static method Main(): int "
        "  using var a := new Plain(); using var b := new Plain(); "
        "  return (10 if a = a else 0) + (1 if a = b else 0); "  // 10
        "end Main; end C;"), 10);
}

// Regression guard: a direct `a.Equals(b)` call to a user override is unchanged
// (this path already worked — the override resolves a MethodSymbol).
BEZEL_TEST(class_object_method_dispatch_1106, direct_equals_override_call) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kP +
        "class C public static method Main(): int "
        "  using var a := new P(5); using var b := new P(5); "
        "  return (1 if a.Equals(b) else 0); "             // 1
        "end Main; end C;"), 1);
}

// GetHashCode override on a class receiver dispatches the override.
BEZEL_TEST(class_object_method_dispatch_1106, gethashcode_override) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kP +
        "class C public static method Main(): int "
        "  using var a := new P(42); return a.GetHashCode(); "  // 42
        "end Main; end C;"), 42);
}
