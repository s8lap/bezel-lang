// Spec-derived regression test for #2674 — the ownership pass's
// end-of-statement reclaim flags (DiscardStmt::disposeOwnedValue /
// disposeOwnedCollectionValue / disposeInlineOwnedArray / discardOutOwnedVar)
// are never set inside a lambda `do` body, because the per-method dataflow
// driven by MethodOwnership::run walks only the METHOD body's statement list
// — the lambda body statements inside `lam->stmts` are reached only by
// narrow, purpose-built checks (checkLambdaTaskFamilyBorrowReturns… and
// friends, never by visitStmt).
//
// Before this fix the leak is mechanical: the #1009 / #1909 / #1016 /
// #1387/#1651 flag-setting code lives in MethodOwnership::visitStmt's
// DiscardStmt arm, and a `DiscardStmt` inside a lambda body is never seen by
// visitStmt — so `disposeOwnedValue` (and the three siblings) stays false,
// and codegen emits no end-of-statement free for the discarded owned
// producer. ASan/LSan catches the leak at exit.
//
// #2655's ExpressionStmt fix is set in sema (not the ownership pass), and
// sema's visitStmt does descend into lambda bodies via
// ExprTypecheck_Exprs.cpp:8066 (`for (const auto& s : lam->stmts) if (s)
// visitStmt(*s);`). That fix therefore reaches the lambda body. The
// ExpressionStmt case is pinned as the §28.3 "free path already wired"
// control — it should remain leak-free across this change.
//
// Each test uses runAotChecked (the AOT + ASan/LSan oracle, support/aot_support.hpp):
// a leak at exit surfaces as `r.ran == false` with the LSan report in
// diagnostics. A clean program reports `r.ran == true` with the program's
// exit code.
//
// per §28.3 "unbound owned temporary", §17.13, §28.9 (v0.282)

#include <string>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// §28.3 / #1009 / #2674: a discard `_ := new Box(9);` inside a block-bodied
// lambda body is an "unbound owned temporary" (§28.3) and §17.13's cosmetic
// `_ := …` discard of a fresh owned producer reclaims at end-of-statement.
// The fix sets DiscardStmt::disposeOwnedValue inside lambda bodies via a
// structural pre-pass parallel to applyUsingVarBorrowPinInLambdas, so the
// codegen emits the reclaim exactly as it does for the same discard at
// method-body scope. Without the fix ASan/LSan report a 24-byte leak (Box
// holds one int). The lambda is invoked locally so a non-trivial return path
// or capture-escape concern does not muddy the test.
BEZEL_TEST(lambda_body_discard_reclaim_2674, discard_new_box_in_block_lambda_clean) {
    std::string src =
        "module M; "
        "class Box(v: int) "
        "  public V: int := v; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      _ := new Box(9); "
        "      return 7; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §28.3 / #1909 / #2674: a discard `_ := ownarray;` of an owning collection
// inside a lambda body must reclaim the collection at end-of-statement. The
// OwningArray element type is a trivial-dispose class so the inner-element
// reclaim is a no-op, but the collection's own storage must be freed. The
// §17.13 cosmetic-discard flag here is disposeOwnedCollectionValue (set by
// the ownership pass at visitStmt's DiscardStmt arm). Without the fix this
// leaks OwningArray's header + element array.
BEZEL_TEST(lambda_body_discard_reclaim_2674, discard_owning_array_in_block_lambda_clean) {
    std::string src =
        "module M; "
        "class Cell(v: int) "
        "  public V: int := v; "
        "end Cell; "
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var own := new OwningArray<Cell>(); "
        "      own.Add(new Cell(1)); own.Add(new Cell(2)); "
        "      _ := own; "
        "      return 7; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §28.3 / #2655 / #2674: an ExpressionStmt `new Box(9);` inside a lambda
// body is the SAME unbound-owned-temporary at end-of-statement as the
// `_ := …` discard above — §28.3's "in an expression statement, 'end of
// the full expression' and 'end of the statement' coincide (§21.9)". This
// flag is set by sema (ExprTypecheck.cpp:12286 / :12842), and sema's
// visitStmt DOES descend into lambda bodies — so this test pins the
// §28.3 control: the program must already pass on the pre-fix parent
// (the leak is structural to the OWNERSHIP-PASS-set flags only). It is
// kept here so any future widening of the fix does not silently regress
// the ExpressionStmt path.
BEZEL_TEST(lambda_body_discard_reclaim_2674, bare_new_box_in_block_lambda_clean) {
    std::string src =
        "module M; "
        "class Box(v: int) "
        "  public V: int := v; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      new Box(9); "
        "      return 7; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §28.3 / #2674: invoke the lambda multiple times to amplify the leak
// signal — a single iteration leaks 24 B, which is the boundary case for
// LSan (it can pass on the last allocation if rss accounting rolls over
// at exit). 16 iterations is well above that noise floor. Each iteration
// must reclaim its `new Box(9);` discard. Note Bezel's `a..b` range
// iteration is INCLUSIVE of both endpoints, so `0..15` visits i=0..15
// (16 iterations).
BEZEL_TEST(lambda_body_discard_reclaim_2674, discard_in_invoked_lambda_loop_clean) {
    std::string src =
        "module M; "
        "class Box(v: int) "
        "  public V: int := v; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      _ := new Box(9); "
        "      return 7; "
        "    end; "
        "    var sum := 0; "
        "    for var i in 0..15 do "
        "      sum := sum + f(); "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7 * 16);
}

}  // namespace
