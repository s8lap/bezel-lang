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

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
bool rejected(const std::string& prog) {
    auto r = runAot(prog);
    return !r.ran && r.diagnostics.hasErrors();
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
        "  return (if a.Equals(a) then 10 else 0) + (if a.Equals(b) then 1 else 0); "  // 10
        "end Main; end C;"), 10);
}

// #1106 defect 2: `a = b` on a class with an overridden Equals dispatches the
// override (was: raw pointer compare → false for distinct equal instances).
BEZEL_TEST(class_object_method_dispatch_1106, eq_operator_uses_override) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kP +
        "class C public static method Main(): int "
        "  using var a := new P(5); using var b := new P(5); using var c := new P(9); "
        "  return (if a = b then 10 else 0) + (if a <> c then 1 else 0); "  // 5==5 →10, 5<>9 →1 = 11
        "end Main; end C;"), 11);
}

// #1106 defect 2 (negative): `a = c` with the override is false when values
// differ; `<>` is true.
BEZEL_TEST(class_object_method_dispatch_1106, eq_operator_override_unequal) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kP +
        "class C public static method Main(): int "
        "  using var a := new P(5); using var c := new P(9); "
        "  return (if a = c then 1 else 0) + (if a <> c then 10 else 0); "  // 0 + 10 = 10
        "end Main; end C;"), 10);
}

// Regression guard: `=` on a class with NO override keeps reference identity
// (a = a true, a = b false).
BEZEL_TEST(class_object_method_dispatch_1106, eq_operator_no_override_ref_identity) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Plain public _v: int := 0; end Plain; "
        "class C public static method Main(): int "
        "  using var a := new Plain(); using var b := new Plain(); "
        "  return (if a = a then 10 else 0) + (if a = b then 1 else 0); "  // 10
        "end Main; end C;"), 10);
}

// Regression guard: a direct `a.Equals(b)` call to a user override is unchanged
// (this path already worked — the override resolves a MethodSymbol).
BEZEL_TEST(class_object_method_dispatch_1106, direct_equals_override_call) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kP +
        "class C public static method Main(): int "
        "  using var a := new P(5); using var b := new P(5); "
        "  return (if a.Equals(b) then 1 else 0); "             // 1
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

// #1106 defect 6 (§12.3): `base.M()` where M is declared only on the calling
// class (its base is Object, which has no M) is rejected — was silently
// accepted and returned 0 (mis-lowered to a stub).
BEZEL_TEST(class_object_method_dispatch_1106, base_call_own_method_rejected) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; "
        "class A "
        "  public virtual method UserM(): int => 42; "
        "  public method CallBase(): int => base.UserM(); "  // UserM is A's own
        "end A; "
        "class C public static method Main(): int "
        "  using var a := new A(); return a.CallBase(); "
        "end Main; end C;"));
}

// Regression guard: `base.M()` to a method the explicit base provides is
// accepted and dispatches the base implementation non-virtually.
BEZEL_TEST(class_object_method_dispatch_1106, base_call_real_base_method_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B public virtual method RealM(): int => 7; end B; "
        "class A: B public override method RealM(): int => base.RealM() + 5; end A; "
        "class C public static method Main(): int "
        "  using var a := new A(); return a.RealM(); "     // base 7 + 5 = 12
        "end Main; end C;"), 12);
}

// Regression guard: `base.M()` to an inherited (non-overridden) base method is
// accepted.
BEZEL_TEST(class_object_method_dispatch_1106, base_call_inherited_method_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B public virtual method Helper(): int => 9; end B; "
        "class A: B public method Use(): int => base.Helper(); end A; "
        "class C public static method Main(): int "
        "  using var a := new A(); return a.Use(); "       // 9
        "end Main; end C;"), 9);
}

// Regression guard: `base.ToString()` (a universal Object member) is NOT
// over-rejected by the §12.3 base-member check.
BEZEL_TEST(class_object_method_dispatch_1106, base_call_universal_object_member_ok) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A public override method ToString(): string => base.ToString(); end A; "
        "class C public static method Main(): int "
        "  using var a := new A(); return a.ToString().Length; "  // "A" → 1
        "end Main; end C;"), 1);
}

// ---- #1111 (§12.3 / §8.3): base.Equals / base.GetHashCode non-virtual dispatch

// #1111: base.Equals(o) inside an overriding class where base=Object → §8.3
// reference-identity (same object → true).  Was: vtable dispatch re-entered the
// override → wrong value (returned false / infinite recursion).
BEZEL_TEST(class_object_method_dispatch_1106, base_equals_object_same_ref_true_1111) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A "
        "  public override method Equals(o: Object?): bool => base.Equals(o); "
        "  public override method GetHashCode(): int => 1; "
        "end A; "
        "class C public static method Main(): int "
        "  using var a := new A(); "
        "  return (if a.Equals(a) then 1 else 0); "  // ref-identity true → 1
        "end Main; end C;"), 1);
}

// #1111: base.Equals(o) is false for distinct instances (reference-identity).
// (same-ref=1, distinct-ref=0 → sum=1; verifies both directions in one shot.)
BEZEL_TEST(class_object_method_dispatch_1106, base_equals_object_distinct_false_1111) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A "
        "  public override method Equals(o: Object?): bool => base.Equals(o); "
        "  public override method GetHashCode(): int => 1; "
        "end A; "
        "class C public static method Main(): int "
        "  using var a := new A(); using var b := new A(); "
        "  return (if a.Equals(a) then 1 else 0) + (if a.Equals(b) then 10 else 0); "
        "end Main; end C;"), 1);
}

// #1111: base.GetHashCode() where base=Object → identity hash is consistent
// (calling twice on the same object gives the same value).
BEZEL_TEST(class_object_method_dispatch_1106, base_gethashcode_object_consistent_1111) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A "
        "  public override method GetHashCode(): int => base.GetHashCode(); "
        "end A; "
        "class C public static method Main(): int "
        "  using var a := new A(); "
        "  var h1 := a.GetHashCode(); var h2 := a.GetHashCode(); "
        "  return (if h1 = h2 then 1 else 0); "
        "end Main; end C;"), 1);
}

// #1111: base.Equals(o) where the explicit base has its own Equals override →
// calls that base override non-virtually (§12.3), not the most-derived.
// (Here `base` = B with a user Equals; calleeSym is resolved by sema, so the
// non-virtual dispatch goes through the ms != null path with baseQualified=true.)
BEZEL_TEST(class_object_method_dispatch_1106, base_equals_explicit_base_override_1111) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B "
        "  public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public override method Equals(o: Object?): bool "
        "    var q := o as B; if q = null then return false; end; "
        "    return _v = q!._v; "
        "  end Equals; "
        "  public override method GetHashCode(): int => _v; "
        "end B; "
        "class A: B "
        "  public constructor(v: int) base(v); end constructor; "
        "  public override method Equals(o: Object?): bool => base.Equals(o); "
        "  public override method GetHashCode(): int => base.GetHashCode(); "
        "end A; "
        "class C public static method Main(): int "
        "  using var a := new A(5); using var b := new A(5); "
        "  return (if a.Equals(b) then 1 else 0); "
        "end Main; end C;"), 1);
}

// #1111: base.GetHashCode() where explicit base has override → calls base override.
BEZEL_TEST(class_object_method_dispatch_1106, base_gethashcode_explicit_base_override_1111) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B "
        "  public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public override method GetHashCode(): int => _v * 3; "
        "end B; "
        "class A: B "
        "  public constructor(v: int) base(v); end constructor; "
        "  public override method GetHashCode(): int => base.GetHashCode(); "
        "end A; "
        "class C public static method Main(): int "
        "  using var a := new A(7); "
        "  return a.GetHashCode(); "
        "end Main; end C;"), 21);
}
