// Spec-derived tests for Ownership slice O2 — diagnosing reads of
// moved-from or disposed locals, and re-disposal / re-move (§28.3).
//
//   "Owned value in `x` was already moved out at line N."
//   "Read of disposed local `x`."
//   "Cannot move out of `using var` `x`; `using var` is scope-bound."

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string wrap(const std::string& body, const std::string& sig = "M()") {
    return std::string("module M; "
        "class T end T; class U end U; class Source end Source; "
        "class C "
        "  public source: Source := null; "
        "  public method ") + sig + " " + body + " end; "
        "end C;";
}

} // namespace

BEZEL_TEST(own_read, read_of_disposed_local_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); x.Dispose(); _ := x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_read, read_of_moved_local_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); _ := move x; _ := x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_read, double_dispose_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); x.Dispose(); x.Dispose();"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_read, double_move_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); _ := move x; _ := move x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_read, dispose_after_move_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); _ := move x; x.Dispose();"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_read, move_out_of_using_var_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "using var x := new T(); _ := move x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_read, normal_use_before_dispose_clean) {
    // A normal read of `x` before disposing it is clean. (Originally this read
    // `x.Length`, but `T` has no `Length` member; issue #414 now diagnoses
    // unknown members on a fully-resolved user type, so the read must name a
    // real member. `x.ToString()` is the universal §8.1 Object member — a valid
    // read that exercises the same use-before-dispose ownership path.)
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); _ := x.ToString(); x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_read, return_after_move_uses_moved_local_diagnosed) {
    // `var x := new T(); _ := move x; return x;` — the `return x` is
    // a read of a moved-from local.
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); _ := move x; return x;", "Make(): T"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
