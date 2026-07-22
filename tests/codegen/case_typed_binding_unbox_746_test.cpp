// Spec-derived tests for fix #746 (R41 cluster A) — a `case` typed binding
// `var n: T` over a BOXED `Object?` scrutinee unboxes the value (and does a
// runtime type-test), instead of binding the raw box pointer.
//
// Authority: §19.3 (typed binding `var n: T` is a type-test + binding), §19.5
// (typed bindings never match null), §8.6 (unboxing).
//
// Pre-fix the binding `n` was the box `ptr`: arithmetic/branch/field access on
// it emitted ptr-vs-value IR (IR_FAIL), and the primitive type-test was
// hardcoded `true` so `var n: int` matched ANY boxed value (a string, a null)
// and then unboxed garbage / dereferenced null.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §19.3/§8.6: int typed binding over Object? — unboxed and used in arithmetic.
BEZEL_TEST(case_typed_binding_746, int_binding_unboxed) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: Object? := 42; "
        "  return (case x of var n: int => n + 1, _ => 0); "  // 43
        "end Main; end C;"), 43);
}

// §19.3: bool typed binding over Object? — used as a branch condition.
BEZEL_TEST(case_typed_binding_746, bool_binding_unboxed) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: Object? := true; "
        "  return (case x of var n: bool => (if n then 1 else 0), _ => 99); "
        "end Main; end C;"), 1);
}

// §19.3: long typed binding over Object? — full-width arithmetic.
BEZEL_TEST(case_typed_binding_746, long_binding_unboxed) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: Object? := 100L; "
        "  return (case x of var n: long => int(n mod 256L), _ => 0); "
        "end Main; end C;"), 100);
}

// §19.4: the bound name is usable in a when-guard.
BEZEL_TEST(case_typed_binding_746, int_binding_in_when_guard) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: Object? := 42; "
        "  return (case x of var n: int when n > 10 => 1, _ => 0); "
        "end Main; end C;"), 1);
}

// §19.3: a record typed binding over Object? — field access on the unboxed value.
BEZEL_TEST(case_typed_binding_746, record_binding_unboxed) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(px: int, py: int); "
        "class C public static method Main(): int "
        "  var p := new Point(3, 7); "
        "  var q: Object? := p; "
        "  return (case q of var pt: Point => pt.px + pt.py, _ => 0); "  // 10
        "end Main; end C;"), 10);
}

// §19.3/§19.5: a typed binding is a TYPE-TEST — a boxed string does not match
// `var n: int`; the arm is skipped (no crash) and `_` is taken.
BEZEL_TEST(case_typed_binding_746, int_binding_does_not_match_boxed_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: Object? := \"hello\"; "
        "  return (case x of var n: int => n + 1, _ => 77); "
        "end Main; end C;"), 77);
}

// §19.5: a typed binding never matches null — `_` is taken (no deref).
BEZEL_TEST(case_typed_binding_746, int_binding_does_not_match_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: Object? := null; "
        "  return (case x of var n: int => n + 1, _ => 55); "
        "end Main; end C;"), 55);
}

// §19.3: a record typed binding does not match a boxed primitive — `_` taken.
BEZEL_TEST(case_typed_binding_746, record_binding_does_not_match_boxed_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(px: int, py: int); "
        "class C public static method Main(): int "
        "  var q: Object? := 5; "
        "  return (case q of var pt: Point => pt.px, _ => 88); "
        "end Main; end C;"), 88);
}
