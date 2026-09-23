// Spec-derived tests for fix #746 (R41 clusters E and G) — four missing
// diagnostics that were silently accepted (FALSE_ACCEPT) or reached codegen
// (IR_FAIL):
//   E: range slicing on an inline array `[N]T` (§15.6) / on `OwningArray<T>`
//      (§15.6) must be rejected.
//   G: a `try` with neither catch nor finally (§31.2) / an empty interpolation
//      hole `{}` (§18.5) must be rejected.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasMsg(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(std::string(needle)) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §15.6 (E): an inline-array range slice is rejected.
BEZEL_TEST(r41_diagnostics_746, inline_array_range_slice_rejected) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var ia: [5]int := [1, 2, 3, 4, 5]; "
        "  var s := ia[1..3]; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "inline array"));
}

// §15.6 (E): an OwningArray<T> range slice is rejected.
BEZEL_TEST(r41_diagnostics_746, owned_array_range_slice_rejected) {
    auto r = parseSemaAll(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var oa := new OwningArray<int>(); "
        "  oa.Add(10); "
        "  using var s := oa[0..1]; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "OwningArray"));
}

// §15.6: a plain (non-range) index on those types is NOT rejected.
BEZEL_TEST(r41_diagnostics_746, plain_index_not_rejected) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var ia: [5]int := [1, 2, 3, 4, 5]; "
        "  return ia[2]; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "§15.6"));
}

// §31.2 (G): a try with neither catch nor finally is rejected.
//
// ORACLE STRENGTHENED (v0.307): this used to assert only that SOME diagnostic
// mentioned "§31.2". That is the #4716 vacuous-pass shape — a bare section
// number is short enough to appear in a neighbouring diagnostic's text, so the
// row would stay green if an EARLIER check started rejecting this program for
// an unrelated reason. It also could not see whether the diagnostic NAMES the
// rule it enforced. §31.2 (v0.307) states the rule as "At least one of `catch`
// or `finally` must be present"; the diagnostic now carries that clause
// verbatim and this asserts it, which is what rule-corpus case `s31_2_0001`
// (`//! diagnostic: must be present`) measures.
BEZEL_TEST(r41_diagnostics_746, try_without_catch_or_finally_rejected) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  try "
        "    var x := 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "§31.2"));
    BEZEL_EXPECT_TRUE(
        hasMsg(r, "at least one of `catch` or `finally` must be present"));
}

// §31.2: a try WITH a catch is accepted.
BEZEL_TEST(r41_diagnostics_746, try_with_catch_ok) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  try var x := 1; catch Exception => return 1; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "§31.2"));
}

// §18.5 (G): an empty interpolation hole `{}` is rejected.
BEZEL_TEST(r41_diagnostics_746, empty_interpolation_hole_rejected) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var s := $\"a{}b\"; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "empty interpolation hole"));
}

// §18.5: a non-empty interpolation hole is accepted.
BEZEL_TEST(r41_diagnostics_746, nonempty_interpolation_hole_ok) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n := 7; "
        "  var s := $\"a{n}b\"; "
        "  return s.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "empty interpolation hole"));
}
