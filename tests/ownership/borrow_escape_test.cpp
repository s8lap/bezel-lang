// Spec-derived tests for Ownership slice O7 — borrow lifetime escape
// through return (§28.3).
//
// A returned borrow's source must outlive the caller's frame. A
// `using var` local is scope-bound — returning a borrow rooted at a
// using-var leaks the source.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string wrap(const std::string& method) {
    return std::string("module M; "
        "class T end T; class View end View; "
        "class Connection "
        "  public method View(): View => null; "
        "end Connection; "
        "class C ") + method + " end C;";
}

} // namespace

BEZEL_TEST(own_escape, return_member_call_on_using_var_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Leak(): View "
        "  using var c := new Connection(); "
        "  return c.View(); "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_escape, return_field_access_on_using_var_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "public method LeakField(): View "
        "  using var c := new Connection(); "
        "  return c.Inner; "
        "end LeakField;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_escape, return_member_call_on_param_clean) {
    // Param `c` outlives the caller's frame, so returning c.View() is
    // fine (the caller's `c` source persists past this method's exit).
    auto r = parseAndCheckOwnership(wrap(
        "public method View(c: Connection): View "
        "  return c.View(); "
        "end View;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_escape, return_bare_local_not_borrow_escape) {
    // `return c;` where c is a plain `var` is a discharge (the local's
    // ownership transfers to the caller); not a borrow escape.
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(): Connection "
        "  var c := new Connection(); "
        "  return c; "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A *value-typed* projection of a `using var` (an int field or an
// int-returning method) is a copy, not a borrow — returning it cannot
// outlive the source, so it is not a borrow-escape (§28.3 governs
// borrows). The reference-typed cases above remain diagnosed.
BEZEL_TEST(own_escape, return_value_field_on_using_var_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class R public V: int := 0; end R; "
        "class C public method Get(): int "
        "  using var r := new R(); return r.V; "
        "end Get; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_escape, return_value_method_on_using_var_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class R public V: int := 0; "
        "  public method Size(): int => V; end R; "
        "class C public method Get(): int "
        "  using var r := new R(); return r.Size(); "
        "end Get; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
