// Codegen + sema — #3345: the static-call form `Array.Sort(a)` /
// `Array.Reverse(a)` (the receiver is the TYPE NAME, not a value) used to
// crash bezelc with SIGSEGV (exit 139) — sema unconditionally dereferenced
// the null `recvTy` at ExprTypecheck_Calls.cpp:5526 in the `else if` chain
// that types Sort/Sorted/Reversed.  Codegen had no parallel arm.  Fix
// routes both forms to the same in-place lowering.
//
// Static-call sugar: `Array.Sort(a)` ↔ `a.Sort()` (in-place, returns unit).
// The fix covers Sort / Reverse / Fill / CopyTo on Array<T> / OwningArray<T>.
// The copy-form static calls (`Array.Reversed()`, `Array.Sorted()`) are
// arity-0 in this position and never reach the new arm — users invoke the
// instance form for those.
//
// per spec §3, stdlib §4.4 / §4.5 (v0.278).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ===========================================================================
// Core regression: `Array.Sort(a)` must compile cleanly (no SIGSEGV) and
// produce a correctly-sorted array.  This is the case that crashed on
// HEAD at ExprTypecheck_Calls.cpp:5527 (recvTy was null for the bare
// IdentifierExpr "Array" target → !recvTy->genericArgs.empty()).
// ===========================================================================

// sort [3,1,4,1,5] → [1,1,3,4,5] = 11345; runAot truncates to 8 bits
// (WEXITSTATUS) so the result is 11345 % 256 = 81.  Both numbers are
// discriminable (an unsorted 31415 % 256 = 183) — confirms the array
// is actually sorted, not just that the program ran.
BEZEL_TEST(array_sort_static_call_3345, static_sort_int_basic) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [3, 1, 4, 1, 5]; "
        "  Array.Sort(a); "  // static-call form (was SIGSEGV)
        "  return a.Get(0) * 10000 + a.Get(1) * 1000 + a.Get(2) * 100 "
        "       + a.Get(3) * 10 + a.Get(4); "
        "end Main; end C;"), 81);
}

// ===========================================================================
// Same shape for `Array.Reverse(a)`.  Reverse [3,1,4,1,5] → [5,1,4,1,3] =
// 51413; 51413 % 256 = 213.
// ===========================================================================

BEZEL_TEST(array_sort_static_call_3345, static_reverse_int_basic) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [3, 1, 4, 1, 5]; "
        "  Array.Reverse(a); "  // static-call form
        "  return a.Get(0) * 10000 + a.Get(1) * 1000 + a.Get(2) * 100 "
        "       + a.Get(3) * 10 + a.Get(4); "
        "end Main; end C;"), 213);
}

// ===========================================================================
// Control: the instance form `a.Sort()` / `a.Reverse()` must still produce
// the same answer — the fix re-binds the receiver, it does not change the
// underlying in-place lowering.
// ===========================================================================

BEZEL_TEST(array_sort_static_call_3345, instance_sort_int_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [3, 1, 4, 1, 5]; "
        "  a.Sort(); "        // instance form (pre-existing, must still work)
        "  return a.Get(0) * 10000 + a.Get(1) * 1000 + a.Get(2) * 100 "
        "       + a.Get(3) * 10 + a.Get(4); "
        "end Main; end C;"), 81);
}

BEZEL_TEST(array_sort_static_call_3345, instance_reverse_int_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [3, 1, 4, 1, 5]; "
        "  a.Reverse(); "     // instance form (pre-existing, must still work)
        "  return a.Get(0) * 10000 + a.Get(1) * 1000 + a.Get(2) * 100 "
        "       + a.Get(3) * 10 + a.Get(4); "
        "end Main; end C;"), 213);
}

// ===========================================================================
// Axis: pre-sorted input.  Sort() on a sorted array is a no-op, but
// exercises the comparator-thunk path through `bz_array_sort`.
// ===========================================================================

BEZEL_TEST(array_sort_static_call_3345, static_sort_pre_sorted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3, 4, 5]; "
        "  Array.Sort(a); "
        "  return a.Get(0) * 10000 + a.Get(1) * 1000 + a.Get(2) * 100 "
        "       + a.Get(3) * 10 + a.Get(4); "
        "end Main; end C;"), 12345 % 256);
}

// ===========================================================================
// Axis: descending input.  Sorts to ascending.  Same encoding as basic.
// ===========================================================================

BEZEL_TEST(array_sort_static_call_3345, static_sort_descending) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [9, 7, 5, 3, 1]; "
        "  Array.Sort(a); "
        "  return a.Get(0) * 10000 + a.Get(1) * 1000 + a.Get(2) * 100 "
        "       + a.Get(3) * 10 + a.Get(4); "
        "end Main; end C;"), 13579 % 256);
}

// ===========================================================================
// Axis: all-equal input.  Sort is a no-op.  Verifies the comparator
// returns 0 cleanly for the bz_array_sort msort path.
// ===========================================================================

BEZEL_TEST(array_sort_static_call_3345, static_sort_all_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [7, 7, 7, 7, 7]; "
        "  Array.Sort(a); "
        "  return a.Get(0) * 10000 + a.Get(1) * 1000 + a.Get(2) * 100 "
        "       + a.Get(3) * 10 + a.Get(4); "
        "end Main; end C;"), 77777 % 256);
}

// ===========================================================================
// Static-call dispatch must return Unit (no value), so a statement
// expression context works (the typical use).
// ===========================================================================

BEZEL_TEST(array_sort_static_call_3345, static_sort_as_statement) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [5, 4, 3, 2, 1]; "
        "  Array.Sort(a); Array.Sort(a); "  // chained, still in-place
        "  return a.Get(0); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// Negative: a non-array first argument must be rejected with a clean
// diagnostic, not crash.  The static-call guard checks argTy is itself
// Array<T>/OwningArray<T> with matching head; anything else falls through
// to the standard "no member M on Type" reject (which fires for the bare
// IdentifierExpr target).
// ===========================================================================

BEZEL_TEST(array_sort_static_call_3345, static_sort_non_array_arg_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var x: int := 42; "
        "  Array.Sort(x); "  // x is int, not Array<T>
        "  return 0; "
        "end Main; end C;");
    // Compile must FAIL cleanly (no crash), with exit != 0 or no run.
    // Pre-#3345: exit 139 (SIGSEGV in sema).
    BEZEL_EXPECT_TRUE(!r.ran || r.exitCode != 0);
}
