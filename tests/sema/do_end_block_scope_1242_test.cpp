// Spec-derived tests for §21.6 — do…end block scope (#1242).
// §21.6, line 3282: "Locals declared in a block are visible only until
// the matching `end`." A `var` declared in a `do…end` statement block
// must be invisible after the closing `end`; any reference past that
// point is an "unresolved identifier" compile error.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Wrap a method body into a minimal class for testing.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; class C public static method Main(): int ")
        + body + " end Main; end C;";
}

} // namespace

// §21.6 Facet A: a var declared inside a do…end block is not visible
// after the matching end — must produce a compile error (#1242).
BEZEL_TEST(sema_do_block_scope_1242, var_after_end_is_unresolved_spec_21_6) {
    auto r = parseSemaAll(wrap(
        "do var secret := 42; end; "
        "return secret; "));  // secret is out of scope here
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §21.6: a var used WITHIN the same do…end block must still resolve
// (no over-rejection).
BEZEL_TEST(sema_do_block_scope_1242, var_used_within_block_compiles_spec_21_6) {
    auto r = parseSemaAll(wrap(
        "do var x := 1; var y := x + 1; end; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §21.6: an OUTER-scope var read inside a do…end block must still
// resolve (the block is transparent to the enclosing scope upward).
BEZEL_TEST(sema_do_block_scope_1242, outer_var_accessible_inside_block_spec_21_6) {
    auto r = parseSemaAll(wrap(
        "var outer := 10; "
        "do var inner := outer + 1; end; "  // outer is visible inside
        "return outer;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §21.6: writing to an outer-scope var from inside a do…end block is
// permitted (upward mutation through the scope chain).
BEZEL_TEST(sema_do_block_scope_1242, outer_var_writable_inside_block_spec_21_6) {
    auto r = parseSemaAll(wrap(
        "var sum := 0; "
        "do sum := sum + 5; end; "
        "return sum;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §21.6: nested do…end blocks each introduce their own scope; the
// inner var is not visible in the outer block after the inner end.
BEZEL_TEST(sema_do_block_scope_1242, nested_blocks_each_scope_spec_21_6) {
    auto r = parseSemaAll(wrap(
        "do "
        "  do var inner := 7; end; "
        "  return inner; "   // inner is out of scope here
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §21.6: a var from an outer do-block is not visible after the outer
// end even when the outer block itself surrounds an inner block.
BEZEL_TEST(sema_do_block_scope_1242, outer_block_var_not_visible_after_outer_end_spec_21_6) {
    auto r = parseSemaAll(wrap(
        "do var x := 1; do var y := 2; end; end; "
        "return x; "));  // x is out of scope here
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §21.6: a local const declared inside a do…end block also goes out
// of scope at the matching end.
BEZEL_TEST(sema_do_block_scope_1242, local_const_after_end_is_unresolved_spec_21_6) {
    auto r = parseSemaAll(wrap(
        "do const K: int := 99; end; "
        "return K; "));  // K out of scope
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
