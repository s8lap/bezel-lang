// Codegen AOT — `x ??= expr` (non-nullable RHS) establishes §7.4 narrowing.
//
// §7.4 "How narrowing is established" (v0.210, issue #2104):
//   "assignment: after `x := expr;` where `expr`'s static type is non-nullable
//    … `x` is narrowed until invalidated; likewise after `x ??= expr;` with
//    such an `expr` (on every path `x` then holds a non-null value, §7.7)."
//
// §7.7: `x ??= expr` assigns `expr` into `x` only when `x` is currently null;
// on the was-null path `x` becomes `expr`, on the was-non-null path `x` is
// unchanged and already non-null — so with a NON-NULLABLE `expr`, `x` holds a
// non-null value on EVERY path after the statement, exactly as a plain
// `x := expr;` of a non-nullable value would establish (issue #418).
//
// Before the fix, the compiler established narrowing only for the plain `:=`
// operator, so these programs were wrongly REJECTED at `s.Length`. These
// programs must COMPILE (sema accepts the narrowed access) and produce the
// spec-stated value at runtime; the corresponding NULLABLE-RHS non-narrowing
// and the still-firing invalidation edge are pinned in the lit reject track
// (tests/lit/coalesce_assign_narrow_reject_2104.bzl).

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §7.4 / §7.7 (#2104): the canonical repro. `s` is null; `s ??= "hello"`
// assigns the non-nullable literal on the was-null path, so `s` is non-null
// afterward and `s.Length` (== 5) reads without a further narrowing form.
BEZEL_TEST(codegen_coalesce_assign_narrow, nonnull_rhs_narrows_after_stmt_spec_7_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string? := null; "
        "  s ??= \"hello\"; "
        "  return s.Length; "
        "end Main; end C;"), 5);
}

// §7.7 (#2104): the was-non-null path leaves `x` unchanged (no reassignment),
// while the was-null path takes the RHS — both paths end non-null, so narrowing
// holds regardless of which path ran. `F(true)`: `s` stays null then coalesces
// to "def" (Length 3); `F(false)`: `s` is "ab", `??=` is a no-op, keeps "ab"
// (Length 2). Both branches reach `s.Length` with no narrowing form. 3*10+2=32.
BEZEL_TEST(codegen_coalesce_assign_narrow, both_paths_end_nonnull_spec_7_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(useNull: bool): int "
        "    var s: string? := null; "
        "    if not useNull then s := \"ab\"; end; "
        "    s ??= \"def\"; "
        "    return s.Length; "
        "  end F; "
        "  public static method Main(): int "
        "    return C.F(true) * 10 + C.F(false); "
        "  end Main; end C;"), 32);
}

// §7.4 (#2104): the narrowing is established inside a branch and usable within
// it — `??=` follows the same flow-sensitive establishment as `:=`. Called with
// `true`, the then-block runs: `s ??= "hi"` narrows and `s.Length` (== 2) reads.
BEZEL_TEST(codegen_coalesce_assign_narrow, narrows_inside_branch_spec_7_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method InBranch(cond: bool): int "
        "    var s: string? := null; "
        "    if cond then "
        "      s ??= \"hi\"; "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end InBranch; "
        "  public static method Main(): int "
        "    return C.InBranch(true); "
        "  end Main; end C;"), 2);
}

// §7.4 (#2104): the narrowing established by `??=` is usable for the remainder
// of the loop body it appears in. Each iteration coalesces `s` to a
// non-nullable literal and reads `s.Length` (== 4) with no narrowing form;
// three iterations sum to 12.
BEZEL_TEST(codegen_coalesce_assign_narrow, narrows_within_loop_body_spec_7_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method InLoop(n: int): int "
        "    var s: string? := null; "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < n do "
        "      s ??= \"abcd\"; "
        "      total := total + s.Length; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end InLoop; "
        "  public static method Main(): int "
        "    return C.InLoop(3); "
        "  end Main; end C;"), 12);
}
