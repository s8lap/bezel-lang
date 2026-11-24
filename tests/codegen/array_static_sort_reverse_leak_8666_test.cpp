// Regression for #8666 -- the static sugar `Array.Sort(a)` / `Array.Reverse(a)`
// leaked the receiver array's backing (header + buffer) on every evaluation,
// while the instance spelling `a.Sort()` / `a.Reverse()` did not.
//
// Root cause: the borrowing-Array escape walk (StringEscapeWalker in
// borrowingArrayMode) saw the local passed as an ARGUMENT of the builtin
// `Array.Sort` / `Array.Reverse` and classified it ESCAPED, so the scope-end
// `bz_array_free` never armed. The static sugar is the instance call on its
// first argument (#3345; lowered by the isStaticArrayCall arm of
// tryLowerCollectionInstanceCall): an in-place pass that never stores the
// array, i.e. a call-bounded borrow. Fix: isBuiltinReaderCollectionArg
// recognizes `Array.Sort` / `Array.Reverse` at parameter 0.
//
// per §28.3 (borrow for the call), §28.9 (the backing is released with the
// array's owner), stdlib §4.4 (Array<T>.Sort / Reverse), v0.359.
//
// Each row loops 200 times: a one-shot leak is a vacuous oracle.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// A leak (LSan) or crash flips r.ran to false -> -999.
int run(const std::string& body, const std::string& after) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var s: int := 0; var i: int := 0; "
        "  while i < 200 do "
        "    var a: Array<int> := [3, 1, 2]; " + body +
        "    s := s + " + after + "; i := i + 1; "
        "  end while; "
        "  return s mod 256; "
        "end Main; end C;");
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// [3,1,2] -> Sort -> a[0] == 1; 200 * 1 = 200.
BEZEL_TEST(array_static_sort_reverse_leak_8666, static_sort_does_not_leak) {
    BEZEL_EXPECT_EQ(run("Array.Sort(a); ", "a[0]"), 200);
}

// [3,1,2] -> Reverse -> a[0] == 2; 200 * 2 = 400 mod 256 = 144.
BEZEL_TEST(array_static_sort_reverse_leak_8666, static_reverse_does_not_leak) {
    BEZEL_EXPECT_EQ(run("Array.Reverse(a); ", "a[0]"), 144);
}

// Sort then Reverse -> [3,2,1]; a[0] == 3; 600 mod 256 = 88.
BEZEL_TEST(array_static_sort_reverse_leak_8666, static_sort_then_reverse_does_not_leak) {
    BEZEL_EXPECT_EQ(run("Array.Sort(a); Array.Reverse(a); ", "a[0]"), 88);
}

// The generic spelling `Array<int>.Sort(a)` (#7662's second spelling).
BEZEL_TEST(array_static_sort_reverse_leak_8666, generic_static_sort_does_not_leak) {
    BEZEL_EXPECT_EQ(run("Array<int>.Sort(a); ", "a[0]"), 200);
}
