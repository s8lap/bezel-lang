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

// fix #747 R43 / §22.3: a pre-declared local passed as `out x` is an OUTPUT
// location — "the variable need not be initialized before the call", and the
// callee assigns it on every exit path — so it is definitely assigned after the
// call. The DA pass previously treated every call argument as a read, so a
// `var v: int; M(out v); _ := v;` drew a false "read of unassigned" both at the
// argument and at the post-call read.
BEZEL_TEST(sema_da, out_arg_predeclared_local_is_assigned_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method GetIt(out v: int): bool v := 42; return true; end GetIt; "
        "  public static method Main(): int "
        "    var v: int; "
        "    var ok := GetIt(out v); "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL §22.3: a genuinely unassigned local read (no `out` write) still
// diagnoses — the out-arg relaxation must not mask a real unassigned read.
BEZEL_TEST(sema_da, plain_unassigned_read_still_diagnoses_after_out_fix) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var v: int; "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// CONTROL: a `ref` argument is read-write — the variable must be assigned
// BEFORE the call, so an unassigned `ref v` still diagnoses (only `out` is a
// pure write).
BEZEL_TEST(sema_da, ref_arg_unassigned_still_diagnoses_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Inc(ref v: int) v := v + 1; end Inc; "
        "  public static method Main(): int "
        "    var v: int; "
        "    Inc(ref v); "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- fix #925: callee-side out-parameter definite-assignment (§22.3) -------

// §22.3 / #925: a callee that never assigns its out-parameter on any exit path
// must be rejected with a compile error naming the unassigned param.
BEZEL_TEST(sema_da, callee_out_never_assigned_rejects_spec_22_3_925) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method NeverAssigns(out x: int) "
        "    // x never assigned "
        "  end NeverAssigns; "
        "  public static method Main(): int "
        "    var v: int := 42; "
        "    C.NeverAssigns(out v); "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.3 / #925: a callee that assigns the out-param only in the `if` branch
// (no `else`) fails the §22.3 check — the path where the condition is false
// exits without assigning.
BEZEL_TEST(sema_da, callee_out_partial_if_no_else_rejects_spec_22_3_925) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method MaybeAssign(cond: bool, out x: int) "
        "    if cond then x := 7; end; "
        "    // x not assigned on the !cond fall-through path "
        "  end MaybeAssign; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.3 / #925: a callee that returns early (before assigning the out-param)
// fails the check — the early return is an exit path where x is unassigned.
BEZEL_TEST(sema_da, callee_out_early_return_before_assign_rejects_spec_22_3_925) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method EarlyReturn(cond: bool, out x: int) "
        "    if cond then return; end; "   // x not assigned at this return
        "    x := 7; "
        "  end EarlyReturn; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.3 / #925: a callee that assigns the out-param on every path compiles
// cleanly — no error, and the callee-assigned value is observable.
BEZEL_TEST(sema_da, callee_out_assigns_all_paths_is_clean_spec_22_3_925) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method GetIt(out x: int) x := 7; end GetIt; "
        "  public static method Main(): int "
        "    var v: int := 0; "
        "    C.GetIt(out v); "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.3 / #925: assigning the out-param in BOTH branches of an if/else before
// fall-off satisfies the check.
BEZEL_TEST(sema_da, callee_out_if_else_both_assign_is_clean_spec_22_3_925) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Choose(cond: bool, out x: int) "
        "    if cond then x := 1; else x := 2; end; "
        "  end Choose; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.3 / #925: a `ref` parameter that the callee reads but does not
// re-assign must still compile cleanly — `ref` starts assigned (the caller
// passes an initialised lvalue), so no out-param check applies.
BEZEL_TEST(sema_da, callee_ref_param_not_reassigned_is_clean_spec_22_3_925) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method ReadOnly(ref n: int): int return n; end ReadOnly; "
        "  public static method Main(): int "
        "    var v: int := 42; "
        "    return C.ReadOnly(ref v); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
