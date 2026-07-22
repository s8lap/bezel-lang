// Codegen — positional record construction with named arguments (§9.1 / §22.5,
// issue #540). `new R(name := x, age := y)` should store x into the `name`
// field and y into the `age` field regardless of argument order, matching
// the behaviour of class constructors with named args (already tested in
// ctor_args_test.cpp). Named→positional reordering must be applied before
// the field stores, so `new R(age := 30, name := "Bob")` produces the same
// result as `new R("Bob", 30)`.
//
// Spec refs: §9.1 (record positional parameters), §22.5 (named arguments).
// Regression: before this fix, named args on a positional record were silently
// dropped / the value fell through with nullptr → SIGSEGV or garbage.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// §9.1 / §22.5 / #540: named args in declaration order.
// `new Point(x := 3, y := 4)` → x=3, y=4.
BEZEL_TEST(codegen_record_named_args_540, named_args_in_declaration_order) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(x := 3, y := 4); return p.x * 10 + p.y; "
        "end Main; end C;"), 34);
}

// §9.1 / §22.5 / #540: named args in reversed order.
// `new Point(y := 4, x := 3)` must produce the same result as `new Point(3, 4)`.
// This is the primary regression case — before the fix, x and y were swapped.
BEZEL_TEST(codegen_record_named_args_540, named_args_reversed_order) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(y := 4, x := 3); return p.x * 10 + p.y; "
        "end Main; end C;"), 34);
}

// §9.1 / §22.5 / #540: two-field record with string and int.
// Named args in reversed order: age first, name second.
// `new Person(age := 7, name := "Bob")` → name="Bob", age=7.
// Both fields are read back to prove each landed in the right slot:
// name.Length (3) + age (7) = 10. If the reorder were missing, the string
// "Bob" would be stored into the int `age` slot (and 7 into `name`), which
// either crashes on `r.name.Length` or yields the wrong total.
BEZEL_TEST(codegen_record_named_args_540, named_args_string_int_reversed) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Person(name: string, age: int); "
        "class C public static method Main(): int "
        "  var r := new Person(age := 7, name := \"Bob\"); "
        "  return r.name.Length + r.age; "
        "end Main; end C;"), 10);
}

// §9.1 / §22.5 / #540: three-field record with named args in scrambled order.
// `new Triple(c := 3, a := 1, b := 2)` → a=1, b=2, c=3.
BEZEL_TEST(codegen_record_named_args_540, named_args_three_fields_scrambled) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Triple(a: int, b: int, c: int); "
        "class C public static method Main(): int "
        "  var t := new Triple(c := 3, a := 1, b := 2); "
        "  return t.a * 100 + t.b * 10 + t.c; "
        "end Main; end C;"), 123);
}

// §9.1 / §22.5 / #540: all-int fields, reversed pair.
// `new Pair(second := 7, first := 5)` → first=5, second=7.
BEZEL_TEST(codegen_record_named_args_540, named_args_all_int_reversed) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(first: int, second: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(second := 7, first := 5); "
        "  return p.first * 10 + p.second; "
        "end Main; end C;"), 57);
}

// §9.1 / §22.5 / #540: record-typed field with named args.
// Inner record constructed positionally; outer record uses named args.
BEZEL_TEST(codegen_record_named_args_540, named_args_record_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Inner(v: int); "
        "record Outer(n: int, inner: Inner); "
        "class C public static method Main(): int "
        "  var i := new Inner(9); "
        "  var o := new Outer(inner := i, n := 5); "
        "  return o.n * 10 + o.inner.v; "
        "end Main; end C;"), 59);
}

// CONTROL: §9.1 / #540: positional construction (no named args) still works.
BEZEL_TEST(codegen_record_named_args_540, positional_construction_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(3, 4); return p.x * 10 + p.y; "
        "end Main; end C;"), 34);
}

// CONTROL: §22.4 / #540: class constructor with named args still works.
BEZEL_TEST(codegen_record_named_args_540, class_ctor_named_args_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public X: int := 0; public Y: int := 0; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "end P; "
        "class C public static method Main(): int "
        "  var p := new P(y := 4, x := 3); return p.X * 10 + p.Y; "
        "end Main; end C;"), 34);
}

// CONTROL: §9.5 / #540: record `with`-expression still works after the fix.
BEZEL_TEST(codegen_record_named_args_540, with_expression_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new Point(3, 4); "
        "  var b := a with(x := 9); "
        "  return b.x * 10 + b.y; "
        "end Main; end C;"), 94);
}
