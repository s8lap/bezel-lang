// Spec-derived tests for fix #3103 — `step` in a slice index rejected at
// compile time (§15.6, v0.255, option 1).
//
// Authority: §15.6 "Range slicing `arr[a..b]`, `arr[..b]`, `arr[a..]`,
// `arr[..]` is supported on `Array<T>`. Slicing produces a new `Array<T>`
// containing copies of the references (or values) in the range; mutating
// the slice does not affect the original."
//
// §15.6's slice form is stepless by definition: slicing is a contiguous
// range. A `step` in a slice index has no meaning, parallel to §16.4's
// restriction "step is not legal in `case` patterns" (sema/
// ExprTypecheck.cpp's `validateCaseArms`).
//
// Pre-fix (codegen/LlvmBackend_Assign.cpp's `lowerArrayRangeSlice`):
// only `re->lo`, `re->hi`, and `re->inclusive` were read; `re->step` was
// silently dropped.  Two consequences:
//   1. §16.4's runtime zero-step fault did not fire for
//      `arr[a..b step z]` with a computed `z == 0` — the step was never
//      lowered, so no `OverflowException` was raised.
//   2. Side effects in the step expression were skipped entirely — a
//      function call in `arr[0..2 step Next()]` was never made.
// Post-fix (this change): `lowerArrayRangeSlice` reports a compile error
// when `re->step` is non-null, parallel to the case-pattern diagnostic.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Compile a .bzl source; return the JitResult so tests can inspect
// `r.diagnostics.hasErrors()` and `r.ran`.
JitResult compile(const std::string& src) {
    return runAot(src);
}

}  // namespace

// ── §15.6 / #3103: a `step` in a slice index is rejected at compile time ──

// #3103 / §15.6: `arr[0..2 step z]` with a runtime-typed step is rejected.
// Pre-fix: the step was silently dropped, and the probe returned 5
// (the post-slice assignment).
// Post-fix: a compile error cites §15.6.
BEZEL_TEST(slice_step_3103, step_with_runtime_typed_index_rejected) {
    JitResult r = compile(
        "module M; class C "
        "  public static method Main(): int "
        "    var z: int := 0; "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "    using var s := a[0..2 step z]; "
        "    return s.Count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// #3103 / §15.6: the `step` clause on a slice index can be on a literal
// non-zero expression — still rejected, because the slice form is stepless
// by definition. (The §16.4 zero-step case is rejected earlier by sema;
// this is the *non-zero* case the codegen silent-drop covered.)
BEZEL_TEST(slice_step_3103, step_with_non_zero_constant_rejected) {
    JitResult r = compile(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "    using var s := a[0..2 step 2]; "
        "    return s.Count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// #3103 / §15.6: a side-effecting step expression is rejected at compile
// time, so the side effect is *not* silently dropped. Pre-fix, the
// side effect was never executed (the step was never lowered). Post-fix,
// the compile error prevents the program from running at all — the side
// effect being unreached is now explicit, not silent. The step is a
// static-method call that increments a module-level counter; without
// the fix neither call happens, the counter stays at 0, and the
// program runs to completion, returning `s.Count == 5` (a[0..4] =
// elements at 0..4 inclusive = 5 elements).
BEZEL_TEST(slice_step_3103, step_with_side_effecting_call_rejected) {
    JitResult r = compile(
        "module M; class C "
        "  public static method Bump(): int "
        "    return 1; "
        "  end Bump; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(10); a.Add(20); a.Add(30); a.Add(40); a.Add(50); "
        "    using var s := a[0..4 step Bump()]; "
        "    return s.Count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// #3103 / §15.6: a stepless slice on a step-bearing Range<T> value
// stays legal. The step FIELD of the `Range<T>` struct is ignored by
// the slice lowering (it only reads lo/hi/flags), which is the
// documented behaviour for `Array<T>.Slice(r)`. The restriction is on
// the `step` CLAUSE in the source syntax, not on Range<T> values.
BEZEL_TEST(slice_step_3103, stepless_slice_of_step_bearing_range_var_ok) {
    JitResult r = compile(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(10); a.Add(20); a.Add(30); a.Add(40); a.Add(50); "
        "    var r: Range<int> := 1..4 step 2; "
        "    using var s := a[r]; "
        "    return s.Count; "   // lo=1, hi=4 inclusive -> 4 elements
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// #3103 / §15.6: a plain stepless slice still works (regression guard
// — the new diagnostic must not over-fire).
BEZEL_TEST(slice_step_3103, plain_stepless_slice_still_works) {
    JitResult r = compile(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(10); a.Add(20); a.Add(30); a.Add(40); a.Add(50); "
        "    using var s := a[1..3]; "
        "    return s.Count; "   // elements at 1,2,3 -> Count = 3
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// #3103 / §15.6: a `for var i in a..b step z` iteration still works
// (a `step` is legal in a for-in range — the restriction is slice-only,
// parallel to case-pattern's slice-only restriction).
BEZEL_TEST(slice_step_3103, for_in_range_with_step_still_works) {
    JitResult r = compile(
        "module M; class C "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..10 step 2 do s := s + i; end; "
        "    return s; "   // 0+2+4+6+8+10 = 30
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 30);
}
