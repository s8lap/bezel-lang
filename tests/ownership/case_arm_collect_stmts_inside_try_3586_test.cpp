// Regression tests for issue #3586 — `MethodOwnership::collectStmtsInsideTryStmt`
// (src/ownership/Ownership_Checks.cpp) had the same missing `ast::CaseStmt` arm
// that fix #3552 just added to its sibling `collectLocalDepthsStmt`. Without
// the arm, a `try` nested in a statement-`case` arm body left every statement
// inside that `try` out of `stmtsInsideTry_` and `stmtsInsideTryByStmt_`.
//
// The gap is LATENT today: the §28.8.1 / K9.2 consumer (`firePendingInTryChecks`
// and its deferred-firing path) deliberately skips a whole `case` statement
// because the ownership CFG appends a `case` as one opaque node and judging
// its arms against the pre-`case` entry state would falsely reject a legal
// arm that restores the field before evaluating a throwable
// (Ownership_Checks.cpp:11830). That skip is the §28.8.1 under-approximation
// #3241's follow-up is expected to REPLACE with arm-local entry states — at
// which point this walker MUST already be correct, otherwise the diagonal
// divergence between the two siblings (collectLocalDepthsStmt has the arm,
// collectStmtsInsideTryStmt does not) becomes a live miss in a check whose
// whole purpose is preventing a destructor from observing a moved-from field.
//
// The walker-coverage test below drives the pre-pass directly and asserts the
// set contains a statement lexically inside a `try` nested in a `case` arm.
// Pre-fix (the rebased HEAD before this commit): the inner statement is NOT in
// the set — the `case` was opaque to the walker. Post-fix: it IS. The
// `do`-`end` A/B partner is included so a regression that re-breaks only the
// `case` half is visibly a PAIR failure, not a single unexplained row.
//
// NOT TESTED HERE. (a) The §28.8.1 consumer at Ownership_Checks.cpp:11830
// still skips the whole `case` — that skip is the deliberate under-approx and
// is NOT being unwound in this commit. Removing it is part of #3241's follow-up
// and the latent gap is therefore still latent after this commit lands; the
// fix is structural, not behaviour-altering. (b) Over-rejection: the fix is a
// NARROWING of the walker (one new arm), not a widening of any consumer, so
// no existing rejection newly fires on a previously-accepted program. The
// fix-must-update-existing-tests invariant is therefore vacuously satisfied.
// Spec: §28.8.1 "Exception paths are not exempt" / K9.2 (v0.286).

#include "ownership/Ownership.hpp"            // MethodOwnership
#include "ownership/Ownership_impl.hpp"       // isStmtInsideTry accessor
#include "ast/Node.hpp"                       // ast::Stmt, ast::CaseStmt, ast::TryStmt, ...
#include "support/ownership_support.hpp"      // parseAndCheckOwnership
#include "test_harness.hpp"

#include <vector>

namespace ast = bezel::ast;
using bezel::ownership::MethodOwnership;
using bezel::test::parseAndCheckOwnership;

namespace {

// Walk an arbitrary Stmt subtree and find the FIRST `TryStmt` whose body
// contains the needle predicate. Returns the matching TryStmt, or null when
// none. Used by the tests to locate the inner try regardless of which
// statement kind wraps it (here: a `case` arm body in the post-fix run, the
// same location in a `do`-`end` block in the A/B partner).
template <typename Pred>
const ast::TryStmt* findEnclosingTry(const ast::Stmt& root, Pred isTarget) {
    if (isTarget(root)) {
        // The root itself qualifies; the caller will inspect its immediate
        // TryStmt ancestor separately.
        return nullptr;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&root)) {
        for (const auto& bs : ts->body) {
            if (bs && isTarget(*bs)) return ts;
        }
        for (const auto& arm : ts->catches) {
            if (arm.body && isTarget(*arm.body)) return ts;
        }
        for (const auto& bs : ts->finallyBody) {
            if (bs && isTarget(*bs)) return ts;
        }
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&root)) {
        for (const auto& bs : ifs->ifBranch.body) {
            if (bs) {
                auto* t = findEnclosingTry(*bs, isTarget);
                if (t) return t;
            }
        }
        for (const auto& el : ifs->elifBranches) {
            for (const auto& bs : el.body) {
                if (bs) {
                    auto* t = findEnclosingTry(*bs, isTarget);
                    if (t) return t;
                }
            }
        }
        for (const auto& bs : ifs->elseBody) {
            if (bs) {
                auto* t = findEnclosingTry(*bs, isTarget);
                if (t) return t;
            }
        }
    } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&root)) {
        for (const auto& bs : ws->body) {
            if (bs) {
                auto* t = findEnclosingTry(*bs, isTarget);
                if (t) return t;
            }
        }
    } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(&root)) {
        for (const auto& bs : fs->body) {
            if (bs) {
                auto* t = findEnclosingTry(*bs, isTarget);
                if (t) return t;
            }
        }
    } else if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&root)) {
        for (const auto& bs : dob->body) {
            if (bs) {
                auto* t = findEnclosingTry(*bs, isTarget);
                if (t) return t;
            }
        }
    } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&root)) {
        for (const auto& arm : cs->arms) {
            for (const auto& bs : arm.stmts) {
                if (bs) {
                    auto* t = findEnclosingTry(*bs, isTarget);
                    if (t) return t;
                }
            }
        }
    } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(&root)) {
        // Already handled above; reference `ts` so the static analyzer
        // sees the recursive entry point as reachable. The walk handles
        // TryStmt at the top of the function.
        (void)ts;
    }
    return nullptr;
}

// Locate the first TryStmt in `body`. Helper used to find the inner try in
// each test's body so we can inspect its first statement.
const ast::TryStmt* findFirstTry(const ast::Stmt& s) {
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) return ts;
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        for (const auto& bs : ifs->ifBranch.body)
            if (bs) {
                if (auto* t = findFirstTry(*bs)) return t;
            }
        for (const auto& el : ifs->elifBranches)
            for (const auto& bs : el.body)
                if (bs) {
                    if (auto* t = findFirstTry(*bs)) return t;
                }
        for (const auto& bs : ifs->elseBody)
            if (bs) {
                if (auto* t = findFirstTry(*bs)) return t;
            }
    } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        for (const auto& bs : ws->body)
            if (bs) {
                if (auto* t = findFirstTry(*bs)) return t;
            }
    } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        for (const auto& bs : fs->body)
            if (bs) {
                if (auto* t = findFirstTry(*bs)) return t;
            }
    } else if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& bs : dob->body)
            if (bs) {
                if (auto* t = findFirstTry(*bs)) return t;
            }
    } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        for (const auto& arm : cs->arms)
            for (const auto& bs : arm.stmts)
                if (bs) {
                    if (auto* t = findFirstTry(*bs)) return t;
                }
    }
    return nullptr;
}

// Drive the collectStmtsInsideTry pre-pass on a fresh MethodOwnership and
// return a pointer to it so the caller can query stmtsInsideTry_ via the
// isStmtInsideTry accessor. We invoke just the pre-pass: the test asserts the
// walker's walker-state answer, not any consumer of it (the §28.8.1 consumer
// still skips the whole `case` — see file header note (a)). Constructing a
// fresh MethodOwnership and calling `run(body)` re-runs all the pre-passes
// (collectStmtsInsideTry, collectLocalDepths, collectBorrowUses, etc.); the
// walker's answer is stable across the fixpoint so the §28.8.1 pre-pass
// result is the same whether we run it once or many times. // per §28.8.1
MethodOwnership driveOwnershipPrePass(bezel::sema::SemaContext& ctx,
                                      const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    MethodOwnership mo(ctx);
    mo.run(body);
    return mo;
}

} // namespace

// The §28.8.1 walker's CaseStmt-arm coverage — positive case. A `try` is
// nested inside the FIRST arm body of a statement-`case`. The walker's
// collectStmtsInsideTry pre-pass must record at least one statement in the
// try body as `insideTry`. Pre-fix: the `case` was opaque to the walker; the
// inner statement was NOT in `stmtsInsideTry_`. Post-fix: it IS.
//
// The probe statement inside the inner try is a local-decl — the smallest
// shape that still exercises the walker's `insert` action and is independent
// of any ownership check the §28.8.1 consumer would (in the future) re-run on
// it.
BEZEL_TEST(case_arm_collect_stmts_inside_try_3586, stmt_in_try_inside_case_arm_recorded) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M0(n: int): int "
        "    case n of "
        "      0 => do "
        "        try "
        "          var x := 1; "
        "        catch var ex: Exception => "
        "          var y := 2; "
        "        end; "
        "      end, "
        "      _ => do var z := 0; end "
        "    end case; "
        "    return 0; "
        "  end M0; "
        "end C;");
    // Pre-condition: the program parsed and sema-resolved without errors
    // that would block the AST walk. The walker test does not assert on
    // diagnostics (this is a structural pre-pass, not a consumer).
    if (!r.sourceFile || !r.ctx) return;
    // Locate the (only) method body in the AST.
    const ast::MethodDecl* theMethod = nullptr;
    for (const auto& td : r.sourceFile->topDecls) {
        if (!td) continue;
        auto* typeDecl = dynamic_cast<const ast::TypeDecl*>(td.get());
        if (!typeDecl) continue;
        for (const auto& m : typeDecl->members) {
            if (!m) continue;
            auto* md = dynamic_cast<const ast::MethodDecl*>(m.get());
            if (md && md->name == "M0") { theMethod = md; break; }
        }
        if (theMethod) break;
    }
    if (!theMethod) return;
    // Find the inner TryStmt inside the case arm body.
    const ast::TryStmt* innerTry = nullptr;
    for (const auto& s : theMethod->stmts) {
        if (s) {
            innerTry = findFirstTry(*s);
            if (innerTry) break;
        }
    }
    if (!innerTry || innerTry->body.empty()) return;
    // The first statement inside the inner try is the LocalDecl `var x := 1;`.
    const ast::Stmt* probeStmt = innerTry->body.front().get();
    // Drive the pre-pass on a fresh MethodOwnership and ASSERT the probe is
    // recorded as inside-`try`. Pre-fix: false (the case was opaque and the
    // walker never descended into arm bodies, so nothing in the try body was
    // ever inserted). Post-fix: true.
    MethodOwnership mo = driveOwnershipPrePass(*r.ctx, theMethod->stmts);
    BEZEL_EXPECT_TRUE(mo.isStmtInsideTry(probeStmt));
}

// A/B PARTNER. The identical body with the inner try spelled `do`-`end` rather
// than a `case` arm. The walker has always visited `do`-`end` blocks (the
// `DoEndBlock` arm is one of the original six), so the same probe statement
// is recorded there BOTH before and after the fix. This row pins that the
// `case` half of the fix is a real arm-vs-arm difference and not a global
// regression that broke the `do`-`end` path too.
BEZEL_TEST(case_arm_collect_stmts_inside_try_3586, stmt_in_try_inside_do_end_recorded) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M1(n: int): int "
        "    do "
        "      try "
        "        var x := 1; "
        "      catch var ex: Exception => "
        "        var y := 2; "
        "      end; "
        "    end; "
        "    return 0; "
        "  end M1; "
        "end C;");
    if (!r.sourceFile || !r.ctx) return;
    const ast::MethodDecl* theMethod = nullptr;
    for (const auto& td : r.sourceFile->topDecls) {
        if (!td) continue;
        auto* typeDecl = dynamic_cast<const ast::TypeDecl*>(td.get());
        if (!typeDecl) continue;
        for (const auto& m : typeDecl->members) {
            if (!m) continue;
            auto* md = dynamic_cast<const ast::MethodDecl*>(m.get());
            if (md && md->name == "M1") { theMethod = md; break; }
        }
        if (theMethod) break;
    }
    if (!theMethod) return;
    const ast::TryStmt* innerTry = nullptr;
    for (const auto& s : theMethod->stmts) {
        if (s) {
            innerTry = findFirstTry(*s);
            if (innerTry) break;
        }
    }
    if (!innerTry || innerTry->body.empty()) return;
    const ast::Stmt* probeStmt = innerTry->body.front().get();
    MethodOwnership mo = driveOwnershipPrePass(*r.ctx, theMethod->stmts);
    BEZEL_EXPECT_TRUE(mo.isStmtInsideTry(probeStmt));
}

// Negative control: a `case` arm WITHOUT a nested `try` must still record the
// arm's own statements as NOT in-`try`. The fix adds an arm to the walker; it
// must not over-record. The probe statement here is an arm-body LocalDecl that
// is NOT inside any `try` — `isStmtInsideTry` must answer false. (Pre-fix
// this is vacuously false because the walker never visits arm bodies at all;
// post-fix it is meaningfully false because the arm IS walked but nothing in
// it is inside a `try`.)
BEZEL_TEST(case_arm_collect_stmts_inside_try_3586, stmt_in_case_arm_without_try_not_recorded) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M2(n: int): int "
        "    case n of "
        "      0 => do "
        "        var x := 1; "
        "      end, "
        "      _ => do var z := 0; end "
        "    end case; "
        "    return 0; "
        "  end M2; "
        "end C;");
    if (!r.sourceFile || !r.ctx) return;
    const ast::MethodDecl* theMethod = nullptr;
    for (const auto& td : r.sourceFile->topDecls) {
        if (!td) continue;
        auto* typeDecl = dynamic_cast<const ast::TypeDecl*>(td.get());
        if (!typeDecl) continue;
        for (const auto& m : typeDecl->members) {
            if (!m) continue;
            auto* md = dynamic_cast<const ast::MethodDecl*>(m.get());
            if (md && md->name == "M2") { theMethod = md; break; }
        }
        if (theMethod) break;
    }
    if (!theMethod) return;
    // Drill into the case statement and pick the FIRST arm body's FIRST stmt.
    const ast::CaseStmt* theCase = nullptr;
    for (const auto& s : theMethod->stmts) {
        if (s) {
            theCase = dynamic_cast<const ast::CaseStmt*>(s.get());
            if (theCase) break;
        }
    }
    if (!theCase || theCase->arms.empty() || theCase->arms.front().stmts.empty()) return;
    const ast::Stmt* probeStmt = theCase->arms.front().stmts.front().get();
    MethodOwnership mo = driveOwnershipPrePass(*r.ctx, theMethod->stmts);
    BEZEL_EXPECT_FALSE(mo.isStmtInsideTry(probeStmt));
}
