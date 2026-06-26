// Spec-derived tests for Sema slice DR1 — a value-returning method
// with a block body must return (or otherwise abruptly exit) on every
// path (§21.7). Sound always-exits analysis: return/throw, if-with-
// else-all-exit, `while true`, do-block, exhaustive (`_`) case, and
// try (finally-exits or body+all-catches-exit). Expression-bodied and
// unit methods are exempt.

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

std::string cls(const std::string& member) {
    return std::string("module M; class C ") + member + " end C;";
}

} // namespace

BEZEL_TEST(sema_ret_path, ends_in_return_ok) {
    auto r = parseSemaAll(cls(
        "public method F(): int return 1; end F;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret_path, no_return_diagnoses) {
    auto r = parseSemaAll(cls(
        "public method F(): int var x := 1; end F;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret_path, if_else_both_return_ok) {
    auto r = parseSemaAll(cls(
        "public method F(b: bool): int "
        "  if b then return 1; else return 2; end; "
        "end F;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret_path, if_without_else_falls_through_diagnoses) {
    auto r = parseSemaAll(cls(
        "public method F(b: bool): int "
        "  if b then return 1; end; "
        "end F;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret_path, if_return_then_trailing_return_ok) {
    auto r = parseSemaAll(cls(
        "public method F(b: bool): int "
        "  if b then return 1; end; "
        "  return 2; "
        "end F;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret_path, ends_in_throw_ok) {
    auto r = parseSemaAll(cls(
        "public method F(): int throw new Exception(); end F;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret_path, while_true_with_return_ok) {
    auto r = parseSemaAll(cls(
        "public method F(): int "
        "  while true do return 1; end; "
        "end F;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret_path, expression_body_ok) {
    auto r = parseSemaAll(cls(
        "public method F(): int => 42;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret_path, unit_method_no_return_ok) {
    auto r = parseSemaAll(cls(
        "public method F() var x := 1; end F;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret_path, do_block_returns_ok) {
    auto r = parseSemaAll(cls(
        "public method F(): int "
        "  do return 1; end; "
        "end F;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #1063 / §21.7 / §31.2.2: a try whose body returns and whose catch arm is an
// EXPRESSION-form throw (`catch E => throw;` rethrow, or `catch E => throw X`)
// always exits — the throw-expression diverges. Before the fix these were
// parsed as an ExpressionStmt wrapping a ThrowExpr (not a ThrowStmt), which
// stmtAlwaysExits did not recognize, so they were wrongly flagged "does not
// return a value on every path."
BEZEL_TEST(sema_ret_path, try_return_catch_rethrow_ok) {
    auto r = parseSemaAll(cls(
        "public method F(): int "
        "  try return 1; catch ArgumentException => throw; end; "
        "end F;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ret_path, try_return_catch_throw_new_ok) {
    auto r = parseSemaAll(cls(
        "public method F(): int "
        "  try return 1; "
        "  catch ArgumentException => throw new InvalidOperationException(\"x\"); end; "
        "end F;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Boundary (negative control): a catch arm that genuinely FALLS THROUGH (an
// expression statement that is NOT a throw) must still be diagnosed — the fix
// recognizes only ThrowExpr expression-statements as always-exiting.
BEZEL_TEST(sema_ret_path, try_return_catch_fallthrough_diagnoses) {
    auto r = parseSemaAll(cls(
        "public method F(): int "
        "  try return 1; catch ArgumentException => var z: int := 5; end; "
        "end F;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The block-form throwing catch arm was already accepted; keep it covered so
// the two forms stay in agreement.
BEZEL_TEST(sema_ret_path, try_return_catch_block_throw_ok) {
    auto r = parseSemaAll(cls(
        "public method F(): int "
        "  try return 1; "
        "  catch ArgumentException => do throw new InvalidOperationException(\"x\"); end; end; "
        "end F;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
