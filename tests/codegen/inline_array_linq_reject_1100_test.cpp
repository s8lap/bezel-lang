// Regression tests for #1100 (§15.1.2): an inline array `[N]T` exposes ONLY
// `.Length`, `.ToArray()`, `.Contains(v)`, the indexer `arr[i]`, and iteration
// via `for var x in arr`.  It is a value type that does NOT implement
// `IEnumerable<T>` and CANNOT be extended (§26.2), so general LINQ / IEnumerable
// operators — `.Count()`, `.Sum()`, `.Where()`, `.All()`, … — have no member to
// resolve on it.  (Note: `.ToArray()` and `.Contains()` ARE now supported as
// §15.1.2 intrinsics per fix #1342 — they are NOT LINQ operators.)
//
// These previously slipped past every gate: resolveMemberAccess derives no
// `targetType` for an InlineArray receiver, so the `knownCompleteUserType`
// "no member" diagnostic never fired, the call typed to nothing, and codegen
// defaulted the result (Count → 0, Sum → garbage, a bool result in a branch →
// IR crash). Sema now rejects any member access on an InlineArray receiver
// other than `.Length`, the intrinsics (`.ToArray()`/`.Contains()`), and the
// universal Object members (§8.1). Found by adversarial discovery sweep #8.
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
// True iff some diagnostic cites the §15.1.2 inline-array member rule.
bool has1512Error(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("\xC2\xA7""15.1.2") != std::string::npos ||
            d.message.find("inline array") != std::string::npos)
            return true;
    return false;
}
}  // namespace

// #1100: `arr.Count()` on `[5]int` was silently accepted and returned 0 — now a
// compile error citing §15.1.2.
BEZEL_TEST(inline_array_linq_reject_1100, count_rejected) {
    auto r = runJit(
        "module M; class C public static method Main(): int "
        "  var arr: [5]int := [1,2,3,4,5]; "
        "  return arr.Count(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(has1512Error(r));
}

// #1100: `.Sum()` likewise rejected.
BEZEL_TEST(inline_array_linq_reject_1100, sum_rejected) {
    auto r = runJit(
        "module M; class C public static method Main(): int "
        "  var arr: [5]int := [10,20,30,40,50]; "
        "  return arr.Sum(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(has1512Error(r));
}

// NOTE: `.ToArray()` and `.Contains()` are now §15.1.2 intrinsics (#1342) and
// are NOT rejected — they are tested in inline_array_intrinsics_1342_test.cpp.

// #1100: a predicate LINQ operator (`.All`) is rejected — its bool result fed a
// branch and previously crashed IR verification.
BEZEL_TEST(inline_array_linq_reject_1100, all_predicate_rejected) {
    auto r = runJit(
        "module M; class C public static method Main(): int "
        "  var arr: [4]int := [2,4,6,8]; "
        "  if arr.All((x: int) => x mod 2 = 0) then return 1; else return 0; end "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(has1512Error(r));
}

// #1100: `.Where(...)` (a deferred lazy operator) is rejected.
BEZEL_TEST(inline_array_linq_reject_1100, where_rejected) {
    auto r = runJit(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  var arr: [5]int := [1,2,3,4,5]; "
        "  using var r: Array<int> := arr.Where((x: int) => x > 2).ToArray(); "
        "  return r.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(has1512Error(r));
}

// #1100: a completely unknown member (`.Frobnicate`) is also rejected — the
// reject is general to the inline-array surface, not a LINQ allow-list.
BEZEL_TEST(inline_array_linq_reject_1100, unknown_member_rejected) {
    auto r = runJit(
        "module M; class C public static method Main(): int "
        "  var arr: [3]int := [1,2,3]; "
        "  return arr.Frobnicate(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(has1512Error(r));
}

// Regression guard: `.Length` (the array's own property, §15.1.2) still works.
BEZEL_TEST(inline_array_linq_reject_1100, length_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [5]int := [1,2,3,4,5]; "
        "  return arr.Length; "                            // 5
        "end Main; end C;"), 5);
}

// Regression guard: indexing `arr[i]` (an IndexExpr, not a member access) is
// unaffected.
BEZEL_TEST(inline_array_linq_reject_1100, indexing_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [5]int := [7,14,21,28,35]; "
        "  return arr[2]; "                                // 21
        "end Main; end C;"), 21);
}

// Regression guard: `for var x in arr` iteration (the spec-blessed traversal,
// not a member access) is unaffected.
BEZEL_TEST(inline_array_linq_reject_1100, for_in_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [5]int := [1,2,3,4,5]; "
        "  var s := 0; for var x in arr do s := s + x; end; "
        "  return s; "                                     // 15
        "end Main; end C;"), 15);
}

// Regression guard: a user method on a class whose receiver happens to be named
// like a collection is unaffected (the reject keys on the InlineArray RECEIVER,
// not the method name) — a class method `Sum()` driven off `_arr[i]` works.
BEZEL_TEST(inline_array_linq_reject_1100, class_method_named_sum_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Holder "
        "  public _arr: [3]int; "
        "  public constructor() _arr := [10, 20, 30]; end constructor; "
        "  public method Sum(): int => _arr[0] + _arr[1] + _arr[2]; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  return h.Sum(); "                               // 60
        "end Main; end C;"), 60);
}
