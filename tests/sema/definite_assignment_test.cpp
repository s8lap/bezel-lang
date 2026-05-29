// Spec-derived tests for Sema slice DA1 — definite-assignment for
// locals. Every read of a local must be preceded on every reaching
// path by at least one write. Reads of an unassigned local are
// diagnosed.

#include "support/da_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckDA;

namespace {

std::string wrap(const std::string& body) {
    return std::string("module M; "
        "class C public method M(cond: bool) ") + body + " end M; end C;";
}

} // namespace

BEZEL_TEST(sema_da, local_with_init_is_assigned) {
    auto r = parseAndCheckDA(wrap("var x := 1; _ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, local_without_init_read_diagnoses) {
    auto r = parseAndCheckDA(wrap("var x: int; _ := x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, local_assigned_before_read_is_clean) {
    auto r = parseAndCheckDA(wrap("var x: int; x := 5; _ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, both_branches_assign_is_clean) {
    auto r = parseAndCheckDA(wrap(
        "var x: int; "
        "if cond then x := 1; else x := 2; end; "
        "_ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, only_one_branch_assigns_diagnoses) {
    auto r = parseAndCheckDA(wrap(
        "var x: int; "
        "if cond then x := 1; end; "
        "_ := x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, elif_branches_all_assign_clean) {
    auto r = parseAndCheckDA(wrap(
        "var x: int; "
        "if cond then x := 1; "
        "elif cond then x := 2; "
        "else x := 3; "
        "end; "
        "_ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, while_body_assigns_doesnt_count_after_loop) {
    auto r = parseAndCheckDA(wrap(
        "var x: int; "
        "while cond do x := 1; end; "
        "_ := x;"));
    // While body may not execute → x is not definitely assigned post.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, while_body_read_after_assign_clean) {
    auto r = parseAndCheckDA(wrap(
        "var x: int; "
        "while cond do x := 1; _ := x; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, param_is_always_assigned) {
    // Parameters are part of the method's entry state — implicit
    // assignment. Slice DA1 doesn't register them in declared_ so
    // reads pass without entering the unassigned-check at all.
    auto r = parseAndCheckDA(wrap("_ := cond;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, field_is_silent_pass_through) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  private _x: int := 42; "
        "  public method M() _ := this._x; end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, local_used_in_initializer_of_another_clean) {
    auto r = parseAndCheckDA(wrap(
        "var x := 1; var y := x + 1; _ := y;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, self_referential_init_reads_undeclared_diagnoses) {
    // `var x := x + 1;` reads x BEFORE its declaration completes →
    // unassigned read.
    auto r = parseAndCheckDA(wrap("var x := x + 1; _ := x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
