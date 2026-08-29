// Codegen — issue #3587 (§19.1 / §19.4 / §30.2 / §30.7, v0.286): the
// STATEMENT-position sibling of case_expr_await_uncounted_2251_test.cpp.
//
// `ast::CaseStmt` (`case EXPR of ARM, ... end;` in statement position, A.6)
// was entirely ABSENT — no `dynamic_cast<const ast::CaseStmt*>` arm at all —
// from both statement-level "does this contain an await?" walkers in
// src/codegen/LlvmBackend_Async.cpp: `Emitter::containsAwait` (the
// `std::vector<std::unique_ptr<ast::Stmt>>` overload) and the file-static
// `countAwaits`. #2251 flagged this as one of "two same-family holes
// surfaced by the same audit" alongside the (now separately-filed, #3622)
// `ForStmt::iterable` gap. This file MEASURES it live and pins the fix.
//
// MECHANISM: identical to the CaseExpr sibling — `countAwaits` sizes the
// async resume switch; an await inside an uncounted CaseStmt arm leaves the
// switch without a state for it, so a REAL cross-thread resume (forced with
// `Task.Delay(40)`) falls to `default: return` and the outer Task never
// completes. The #441/#2226 hang shape.
//
// fail-without/pass-with: the repro below HUNG under the pre-fix compiler
// (MEASURED: a standalone `timeout 10` probe binary, exit 124, stable across
// 3 repeats) and returns the expected value with the fix.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// `total` accumulates across 3 loop iterations, with the ONLY `await` in the
// program sitting inside a statement-`case`'s Statement-form arm body
// (`total := total + await Slow();`, A.6's "reads as a single statement
// whose terminating `;` ends the arm body"), reached only when `i == 1`. Per
// iteration: i=0 -> total=1 (case `_` arm, no-op); i=1 -> total=2, then the
// `1` arm runs `total := total + await Slow()` = 2+5=7; i=2 -> total=8
// (case `_` arm again). Final: 8.
BEZEL_TEST(case_stmt_await_3587, await_in_statement_form_arm_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static async method Slow(): Task<int> "
        "    await Task.Delay(40); "
        "    return 5; "
        "  end Slow; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var i in 0..<3 do "
        "      total := total + 1; "
        "      case i of "
        "        1 => total := total + await Slow();, "
        "        _ => (), "
        "      end; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        8);
}

// Same shape, but the await sits in the CaseStmt's Block-form (`do … end`)
// arm body instead of the Statement-form — the other bodyKind
// `forEachAwaitSiteInCaseArm` deliberately does NOT cover (it is Stmt-level,
// handled by the `arm.bodyKind == ast::CaseArmBodyKind::Block` branch this
// fix adds directly to `containsAwait`/`countAwaits`). Same arithmetic: 8.
BEZEL_TEST(case_stmt_await_3587, await_in_block_form_arm_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static async method Slow(): Task<int> "
        "    await Task.Delay(40); "
        "    return 5; "
        "  end Slow; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var i in 0..<3 do "
        "      total := total + 1; "
        "      case i of "
        "        1 => do total := total + await Slow(); end, "
        "        _ => (), "
        "      end; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        8);
}
