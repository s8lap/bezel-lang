// fix #922 / §16.5 / §9.8 — explicit `Range<int>` type annotation resolves
// to the same compiler-intrinsic record as the inferred form `var r := a..b`.
//
// Before the fix, `var r: Range<int> := 1..5` produced:
//   "unresolved type name `Range`"
//
// The fix adds Range to builtinTypeArity (arity 1) and wires
// declaration = ctx_.builtinClassSym("Range") so isRangeType / .Start/.End /
// for-in-over-Range codegen recognise annotated Ranges identically to
// inferred ones (§16.5 normative syntax).
//
// Tests cover:
//   1. Annotated .Start + .End access (was: return 0; now: returns correct sum)
//   2. Annotated for-in == inferred for-in (same iteration value)
//   3. Inferred form regression (must still work after the fix)
//   4. Parameter-position Range<int> (method(r: Range<int>): int)
//   5. Half-open annotated range for-in

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

// §16.5 / §9.8 / #922: annotated .Start + .End — `r.Start + r.End` for
// `var r: Range<int> := 3..9` must return 12.
// Before fix: "unresolved type name `Range`".
BEZEL_TEST(codegen_range_annotated_922, annotated_start_plus_end) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 3..9; "
        "  return r.Start! + r.End!; "
        "end Main; end C;"), 12);
}

// §16.5 / §9.8 / #922: annotated for-in must iterate identically to the
// inferred form.  `var r: Range<int> := 1..5` → for-in sums 1+2+3+4+5 = 15.
BEZEL_TEST(codegen_range_annotated_922, annotated_for_in_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 1..5; "
        "  var s := 0; "
        "  for var i in r do s := s + i; end; "
        "  return s; "
        "end Main; end C;"), 15);
}

// §16.5 / §9.8 / #922: inferred form must still produce the same value so we
// can confirm annotated == inferred.  `var r := 1..5` for-in sum → 15.
BEZEL_TEST(codegen_range_annotated_922, inferred_for_in_sum_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..5; "
        "  var s := 0; "
        "  for var i in r do s := s + i; end; "
        "  return s; "
        "end Main; end C;"), 15);
}

// §16.5 / §9.8 / #922: annotated == inferred — one method uses the annotated
// form and the other uses the inferred form; both must return the same sum.
BEZEL_TEST(codegen_range_annotated_922, annotated_equals_inferred) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r1: Range<int> := 2..6; "
        "  var r2 := 2..6; "
        "  var s1 := 0; var s2 := 0; "
        "  for var i in r1 do s1 := s1 + i; end; "
        "  for var j in r2 do s2 := s2 + j; end; "
        "  if s1 <> s2 then return 99; end; "
        "  return s1; "
        "end Main; end C;"), 20);  // 2+3+4+5+6 = 20
}

// §16.5 / §9.8 / #922: parameter-position Range<int> annotation.
// A method that accepts Range<int> and iterates it with for-in.
BEZEL_TEST(codegen_range_annotated_922, parameter_position_range) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Sum(r: Range<int>): int "
        "    var s := 0; "
        "    for var i in r do s := s + i; end; "
        "    return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    return Sum(1..4); "
        "  end Main; "
        "end C;"), 10);  // 1+2+3+4 = 10
}

// §16.5 / §9.8 / #922: annotated .Start and .End individually.
BEZEL_TEST(codegen_range_annotated_922, annotated_start_member) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 7..11; "
        "  return r.Start!; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_range_annotated_922, annotated_end_member) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 7..11; "
        "  return r.End!; "
        "end Main; end C;"), 11);
}

// §16.5 / §9.8 / #922: annotated half-open range for-in.
// `var r: Range<int> := 1..<5` iterates 1,2,3,4 → sum = 10.
BEZEL_TEST(codegen_range_annotated_922, annotated_halfopen_for_in) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 1..<5; "
        "  var s := 0; "
        "  for var i in r do s := s + i; end; "
        "  return s; "
        "end Main; end C;"), 10);
}

// §16.5 / §9.8 / #922: inferred .Start/.End regression — must still work.
BEZEL_TEST(codegen_range_annotated_922, inferred_start_end_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 3..9; "
        "  return r.Start! + r.End!; "
        "end Main; end C;"), 12);
}
