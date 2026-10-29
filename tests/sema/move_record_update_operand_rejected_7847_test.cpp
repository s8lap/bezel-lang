// fix #7847 (spec §28.8.1 *The `with` clause beside the other two uses of
// `with`*, §9.5 tie-break, Appendix A.7 MoveExpr, v0.350): a `with` followed by
// `(` and `Identifier :=` after a `move` operand is the record-update suffix of
// the operand, so `move r with(X := 1)` is `move (r with(X := 1))` -- a `move`
// of a record value. A.7 admits no such operand; the diagnostic names the
// replacing form with its replacement parenthesized, `move loc with (e)`.
// Before the fix the program compiled and ran (the parse was right, sema had no
// check on the operand).
//
// Rows:
//   T1  corpus case s28_8_1_0024's program (`var t := move r with(X := 1);`)
//   T2  the explicitly parenthesized spelling `move (r with(X := 1))`, the same
//       tree
//   T3  at an owned-argument position (the rule is not local-binding specific)
//   T4  ACCEPT control: a plain record update (no `move`) is untouched.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

BEZEL_TEST(sema_move_record_update_operand_7847,
           move_r_with_assign_list_is_rejected_naming_replacing_form) {
    auto r = runAot(
        "module M7847A; "
        "record Pt(X: int, Y: int); "
        "class P public static method Main(): int "
        "  var r := new Pt(0, 2); "
        "  var t := move r with(X := 1); "
        "  return t.X; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "move loc with (e)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "record-update"));
}

BEZEL_TEST(sema_move_record_update_operand_7847,
           parenthesized_spelling_is_the_same_tree_and_rejected) {
    auto r = runAot(
        "module M7847B; "
        "record Pt(X: int, Y: int); "
        "class P public static method Main(): int "
        "  var r := new Pt(0, 2); "
        "  var t := move (r with(X := 1)); "
        "  return t.X; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "move loc with (e)"));
}

BEZEL_TEST(sema_move_record_update_operand_7847,
           argument_position_is_rejected_too) {
    auto r = runAot(
        "module M7847C; "
        "record Pt(X: int, Y: int); "
        "class P "
        "  public static method Use(p: Pt): int return p.X; end Use; "
        "  public static method Main(): int "
        "    var r := new Pt(0, 2); "
        "    return P.Use(move r with(X := 3)); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "move loc with (e)"));
}

// ACCEPT control: the plain record update (no `move`) is untouched.
BEZEL_TEST(sema_move_record_update_operand_7847,
           plain_record_update_without_move_still_accepted) {
    auto r = runAot(
        "module M7847D; "
        "record Pt(X: int, Y: int); "
        "class P public static method Main(): int "
        "  var r := new Pt(0, 2); "
        "  var t := r with(X := 1); "
        "  return t.X + r.X; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
