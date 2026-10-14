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

#include "support/aot_result_asserts.hpp"
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

// fix #6265 (spec §3.4, stdlib §4.4/§4.5, v0.338): the stdlib declares no static
// member on a collection type NAME; the #3345 sugar admits only a well-formed
// `Array.Sort(<Array<T>>)`. Anything else is rejected with the same "has no
// static method or property" diagnostic the other builtin type names use, at
// the call's real line:col. The weak oracle this replaces (`!r.ran ||
// r.exitCode != 0`) was also satisfied by the compiler SEGFAULTING (rc 139):
// `failedAtCompileTimeWithRealLocation` rejects a crash/signal outcome, and
// the needle pins the specific message.
namespace {
bool rejectedNoStatic(const std::string& body, const char* needle) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var x: int := 42; " + body + " return 0; "
        "end Main; end C;");
    return bezel::test::failedAtCompileTimeWithRealLocation(r) &&
           bezel::test::diagnosticMessageContains(r, needle);
}
}  // namespace

BEZEL_TEST(array_sort_static_call_3345, static_sort_non_array_arg_rejected) {
    // x is int, not Array<T>: the issue's literal repro.
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "Array.Sort(x);",
        "'Array' has no static method or property 'Sort' (\xC2\xA7""3.4)"));
}

BEZEL_TEST(array_sort_static_call_3345, static_call_wrong_shape_rejected_not_crash) {
    const char* kSort = "has no static method or property 'Sort'";
    BEZEL_EXPECT_TRUE(rejectedNoStatic("Array.Sort();", kSort));
    BEZEL_EXPECT_TRUE(rejectedNoStatic("Array.Sort(\"s\");", kSort));
    BEZEL_EXPECT_TRUE(rejectedNoStatic("Array.Sort(null);", kSort));
    BEZEL_EXPECT_TRUE(rejectedNoStatic("OwningArray.Sort(x);", kSort));
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "Array.Reverse(x);", "has no static method or property 'Reverse'"));
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "Array.Fill(x, 1);", "has no static method or property 'Fill'"));
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "Array.Foo(x);", "has no static method or property 'Foo'"));
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "Stack.Foo(x);", "'Stack' has no static method or property 'Foo'"));
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "var y := Array.Get(x, 0);", "has no static method or property 'Get'"));
}

// fix #6265 regression guard: a nullable array NARROWED to non-null is a valid
// static-call argument (exprType still carries nullable=true on the narrowed
// identifier, so the sugar must not key on it) — spec §7.4.
BEZEL_TEST(array_sort_static_call_3345, static_call_narrowed_nullable_arg_accepted) {
    auto r1 = runAot(
        "module M; class C public static method Main(): int "
        "  var a: Array<int>? := [3, 1]; "
        "  if a <> null then Array.Sort(a); return a[0]; end; return 9; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r1.ran);
    BEZEL_EXPECT_EQ(r1.exitCode, 1);
    auto r2 = runAot(
        "module M; class C public static method Main(): int "
        "  var a: Array<int>? := [3, 1]; "
        "  if a = null then return 9; end; Array.Sort(a); return a[0]; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r2.ran);
    BEZEL_EXPECT_EQ(r2.exitCode, 1);
    auto r3 = runAot(
        "module M; class C "
        "  public static method F(a: Array<int>?): int "
        "    if a <> null then Array.Reverse(a); return a[0]; end; return 9; "
        "  end F; "
        "  public static method Main(): int "
        "    var a: Array<int> := [3, 1]; return F(a); "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r3.ran);
    BEZEL_EXPECT_EQ(r3.exitCode, 1);
}
