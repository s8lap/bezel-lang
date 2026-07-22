// Codegen tests for issue #1579 (v0.182) — §8.4 / §8.6:
// `Object.ReferenceEquals(a, b)` boxes value-typed arguments before comparing
// identity. A NULL nullable-value argument (`T?` currently `null`) boxes to
// the null `Object` reference (§8.6 "Nullable value-type boxing": "If the
// value is `null`, the result is a null `Object` reference"), so
// `ReferenceEquals(x, null)` is `true` for a null `int?` `x` — §8.4: "a
// **null** nullable-value argument boxes to the null reference ([§8.6]), so
// `ReferenceEquals(x, null)` is `true` for a null `int?` `x`."
//
// Before this fix, codegen short-circuited ANY non-pointer (i.e. any
// value-typed, including nullable-value) operand straight to `false` — the
// pre-v0.182 wording the spec's changelog explicitly flags as a
// compiler-observable behavior flip. A present (non-null) value-typed
// argument still boxes to a FRESH heap object on every call and therefore
// still compares unequal to every other argument, including another
// occurrence of an equal value or itself — that part is unchanged by v0.182
// and is covered here as a regression guard.
//
// runJit returns the raw exit code (0/1 encode bool results in these tests).

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

// §8.4/§8.6: a null `int?` compared against the `null` literal is TRUE — the
// null nullable value boxes to the null Object reference, same identity as
// any other null reference.
BEZEL_TEST(reference_equals_null_nullable_1579, null_int_nullable_vs_null_literal_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: int? := null; "
        "  return (if Object.ReferenceEquals(x, null) then 1 else 0); "
        "end Main; end C;"), 1);
}

// §8.4/§8.6: the null-literal argument order is symmetric.
BEZEL_TEST(reference_equals_null_nullable_1579, null_literal_vs_null_int_nullable_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: int? := null; "
        "  return (if Object.ReferenceEquals(null, x) then 1 else 0); "
        "end Main; end C;"), 1);
}

// §8.4/§8.6: two DIFFERENT null nullable-value locals both box to the null
// reference — still identity-equal (both are "the null reference").
BEZEL_TEST(reference_equals_null_nullable_1579, two_null_nullables_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: int? := null; "
        "  var y: double? := null; "
        "  return (if Object.ReferenceEquals(x, y) then 1 else 0); "
        "end Main; end C;"), 1);
}

// §8.4: a PRESENT (non-null) nullable-value argument still boxes to a FRESH
// object — comparing it against `null` is false (unaffected by v0.182).
BEZEL_TEST(reference_equals_null_nullable_1579, present_nullable_vs_null_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: int? := 5; "
        "  return (if Object.ReferenceEquals(x, null) then 1 else 0); "
        "end Main; end C;"), 0);
}

// §8.4: two present nullable-value locals holding the SAME underlying value
// still compare false — each occurrence boxes independently to a fresh,
// never-aliasing object (unaffected by v0.182).
BEZEL_TEST(reference_equals_null_nullable_1579, two_present_equal_nullables_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: int? := 5; "
        "  var y: int? := 5; "
        "  return (if Object.ReferenceEquals(x, y) then 1 else 0); "
        "end Main; end C;"), 0);
}

// §8.4: a plain non-nullable value argument (`5`) is still always false
// against `null` — the pre-existing "always false" behavior for a
// non-nullable value type is unaffected by v0.182.
BEZEL_TEST(reference_equals_null_nullable_1579, nonnullable_value_vs_null_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (if Object.ReferenceEquals(5, null) then 1 else 0); "
        "end Main; end C;"), 0);
}

// §8.4: `Object.ReferenceEquals(null, null)` (two `null` literals, no
// nullable-value type at all) remains true — the baseline reference case.
BEZEL_TEST(reference_equals_null_nullable_1579, two_null_literals_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (if Object.ReferenceEquals(null, null) then 1 else 0); "
        "end Main; end C;"), 1);
}

// §8.4: two references to the SAME class instance remain identity-equal
// (the reference-type path is untouched by the nullable-value fix).
BEZEL_TEST(reference_equals_null_nullable_1579, same_class_instance_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N public v: int := 0; end N; "
        "class C public static method Main(): int "
        "  using var a := new N(); "
        "  return (if Object.ReferenceEquals(a, a) then 1 else 0); "
        "end Main; end C;"), 1);
}
