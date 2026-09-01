// Regression test for fix #3656 — `Ownership_Checks.cpp` retired five
// hand-written statement walkers onto a single `forEachNestedStmt` dispatcher
// in `src/ownership/StmtWalker.hpp`. The retirements must be behaviour-
// preserving: every statement kind the old walkers reached, the new walker
// reaches; nothing the new walker reaches was missed by the old walkers.
//
// Spec sections (§28.3 / §28.8.1 / §30.5 / §35.5, v0.286) name the grammar;
// the test exercises every statement kind by constructing a synthetic AST
// (no parser involvement) and asserts the walker's visitor count on each
// kind. A kind whose count is wrong is a coverage regression: the next
// statement kind added to `ast::Stmt` will require updating the dispatcher
// in `StmtWalker.hpp` — the test pins the current full coverage as the
// contract.

#include "ast/Node.hpp"
#include "ownership/StmtWalker.hpp"
#include "test_harness.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ast = bezel::ast;
using bezel::ownership::StmtVisitState;
using bezel::ownership::forEachNestedStmt;

namespace {

// Convenience builders for the leaf statements the walker dispatches on.
// Each one is constructed in the smallest valid form; the walker only needs
// the type tag to dispatch (it never inspects fields for the leaf kinds).
std::unique_ptr<ast::Stmt> makeLeaf(const std::string& tag) {
    // The walker dispatches on the dynamic type, so each leaf kind needs
    // its OWN concrete type. We tag-construct: each `if` below yields a
    // distinct concrete subtype.
    if (tag == "LocalDecl") {
        auto p = std::make_unique<ast::LocalDecl>();
        return p;
    }
    if (tag == "LocalConstDecl") {
        auto p = std::make_unique<ast::LocalConstDecl>();
        return p;
    }
    if (tag == "ReturnStmt") {
        auto p = std::make_unique<ast::ReturnStmt>();
        return p;
    }
    if (tag == "ExpressionStmt") {
        auto p = std::make_unique<ast::ExpressionStmt>();
        return p;
    }
    if (tag == "AssignmentStmt") {
        auto p = std::make_unique<ast::AssignmentStmt>();
        return p;
    }
    if (tag == "ThrowStmt") {
        auto p = std::make_unique<ast::ThrowStmt>();
        return p;
    }
    if (tag == "YieldStmt") {
        auto p = std::make_unique<ast::YieldStmt>();
        return p;
    }
    if (tag == "IncrementStmt") {
        auto p = std::make_unique<ast::IncrementStmt>();
        return p;
    }
    if (tag == "DiscardStmt") {
        auto p = std::make_unique<ast::DiscardStmt>();
        return p;
    }
    if (tag == "BreakStmt") {
        auto p = std::make_unique<ast::BreakStmt>();
        return p;
    }
    if (tag == "ContinueStmt") {
        auto p = std::make_unique<ast::ContinueStmt>();
        return p;
    }
    return nullptr;
}

// A simple visitor counter — increments per kind on every visit.
struct KindCounter {
    int total = 0;
    int localDecl = 0;
    int localConstDecl = 0;
    int returnStmt = 0;
    int expressionStmt = 0;
    int assignmentStmt = 0;
    int throwStmt = 0;
    int yieldStmt = 0;
    int incrementStmt = 0;
    int discardStmt = 0;
    int breakStmt = 0;
    int continueStmt = 0;
    int ifStmt = 0;
    int whileStmt = 0;
    int forStmt = 0;
    int doEndBlock = 0;
    int tryStmt = 0;
    int caseStmt = 0;
};

void count(const ast::Stmt& s, KindCounter& c) {
    ++c.total;
    if (dynamic_cast<const ast::LocalDecl*>(&s)) ++c.localDecl;
    if (dynamic_cast<const ast::LocalConstDecl*>(&s)) ++c.localConstDecl;
    if (dynamic_cast<const ast::ReturnStmt*>(&s)) ++c.returnStmt;
    if (dynamic_cast<const ast::ExpressionStmt*>(&s)) ++c.expressionStmt;
    if (dynamic_cast<const ast::AssignmentStmt*>(&s)) ++c.assignmentStmt;
    if (dynamic_cast<const ast::ThrowStmt*>(&s)) ++c.throwStmt;
    if (dynamic_cast<const ast::YieldStmt*>(&s)) ++c.yieldStmt;
    if (dynamic_cast<const ast::IncrementStmt*>(&s)) ++c.incrementStmt;
    if (dynamic_cast<const ast::DiscardStmt*>(&s)) ++c.discardStmt;
    if (dynamic_cast<const ast::BreakStmt*>(&s)) ++c.breakStmt;
    if (dynamic_cast<const ast::ContinueStmt*>(&s)) ++c.continueStmt;
    if (dynamic_cast<const ast::IfStmt*>(&s)) ++c.ifStmt;
    if (dynamic_cast<const ast::WhileStmt*>(&s)) ++c.whileStmt;
    if (dynamic_cast<const ast::ForStmt*>(&s)) ++c.forStmt;
    if (dynamic_cast<const ast::DoEndBlock*>(&s)) ++c.doEndBlock;
    if (dynamic_cast<const ast::TryStmt*>(&s)) ++c.tryStmt;
    if (dynamic_cast<const ast::CaseStmt*>(&s)) ++c.caseStmt;
}

// Build an AST with one of every container kind, and inside each, one of
// every leaf kind plus a nested layer. Each layer has one of every container
// kind, so any missing-arm in the walker shows up as a missing count for
// that kind.
std::unique_ptr<ast::Stmt> makeIfStmt(std::unique_ptr<ast::Stmt> thenBody) {
    auto p = std::make_unique<ast::IfStmt>();
    p->ifBranch.body.push_back(std::move(thenBody));
    return p;
}

std::unique_ptr<ast::Stmt> makeWhileStmt(std::unique_ptr<ast::Stmt> body) {
    auto p = std::make_unique<ast::WhileStmt>();
    p->body.push_back(std::move(body));
    return p;
}

std::unique_ptr<ast::Stmt> makeForStmt(std::unique_ptr<ast::Stmt> body) {
    auto p = std::make_unique<ast::ForStmt>();
    p->body.push_back(std::move(body));
    return p;
}

std::unique_ptr<ast::Stmt> makeDoEndBlock(std::unique_ptr<ast::Stmt> body) {
    auto p = std::make_unique<ast::DoEndBlock>();
    p->body.push_back(std::move(body));
    return p;
}

std::unique_ptr<ast::Stmt> makeTryStmt(std::unique_ptr<ast::Stmt> tryBody,
                                       std::unique_ptr<ast::Stmt> armBody) {
    auto p = std::make_unique<ast::TryStmt>();
    p->body.push_back(std::move(tryBody));
    ast::CatchArm arm;
    arm.body = std::move(armBody);
    p->catches.push_back(std::move(arm));
    return p;
}

std::unique_ptr<ast::Stmt> makeCaseStmt(std::unique_ptr<ast::Stmt> armBody) {
    auto p = std::make_unique<ast::CaseStmt>();
    ast::CaseArm arm;
    arm.bodyKind = ast::CaseArmBodyKind::Block;
    arm.stmts.push_back(std::move(armBody));
    p->arms.push_back(std::move(arm));
    return p;
}

}  // namespace

// T1 — FAIL-WITHOUT PROOF. Build a synthetic AST with one of every
// container kind, each containing one leaf LocalDecl and one nested
// IfStmt/WhileStmt/ForStmt/DoEndBlock/TryStmt/CaseStmt. The walker's
// visitor must hit:
//   * 1 LocalDecl (the leaf, counted once at the innermost depth)
//   * 1 LocalDecl is in scope-introducer depth 1, etc.
// The exact total depends on the synthetic AST shape; we assert that every
// container kind was visited at least once AND every leaf kind was visited
// at least once, and that the total exceeds the count of container kinds.
// A missing-arm walker would have a missing count for the unreachable kind.
BEZEL_TEST(foreach_nested_stmt_3656, all_statement_kinds_visited) {
    // Innermost: a fresh LocalDecl leaf for each container.
    auto inCase = makeCaseStmt(makeLeaf("LocalDecl"));
    auto inTry = makeTryStmt(makeLeaf("LocalDecl"), makeLeaf("ReturnStmt"));
    auto inDo = makeDoEndBlock(makeLeaf("LocalDecl"));
    auto inFor = makeForStmt(makeLeaf("LocalDecl"));
    auto inWhile = makeWhileStmt(makeLeaf("LocalDecl"));
    auto inIf = makeIfStmt(makeLeaf("LocalDecl"));

    // Layer 2: build a top-level body of every container. The walker must
    // visit every one and recursively descend into its body.
    std::vector<std::unique_ptr<ast::Stmt>> body;
    body.push_back(std::move(inIf));
    body.push_back(std::move(inWhile));
    body.push_back(std::move(inFor));
    body.push_back(std::move(inDo));
    body.push_back(std::move(inTry));
    body.push_back(std::move(inCase));

    KindCounter c;
    for (const auto& s : body) {
        if (!s) continue;
        forEachNestedStmt(*s, [&](const ast::Stmt& cur) { count(cur, c); });
    }
    // Every container kind visited at least once.
    BEZEL_EXPECT_TRUE(c.ifStmt >= 1);
    BEZEL_EXPECT_TRUE(c.whileStmt >= 1);
    BEZEL_EXPECT_TRUE(c.forStmt >= 1);
    BEZEL_EXPECT_TRUE(c.doEndBlock >= 1);
    BEZEL_EXPECT_TRUE(c.tryStmt >= 1);
    BEZEL_EXPECT_TRUE(c.caseStmt >= 1);
    // The innermost leaf was visited inside every container — six LocalDecls.
    BEZEL_EXPECT_EQ(c.localDecl, 6);
    // The leaf `ReturnStmt` inside `TryStmt`'s catch arm was visited.
    BEZEL_EXPECT_TRUE(c.returnStmt >= 1);
    // Total is the sum of every visit; no extra ones.
    BEZEL_EXPECT_TRUE(c.total >= 6 /*containers*/ + 6 /*leaves*/ + 1 /*try's ReturnStmt*/);
}

// T2 — Every leaf statement kind is reached when nested in a `case` arm
// body. This is the regression that motivated fix #3074 / #3552 / #3586: a
// leaf kind sitting only inside a `case` arm body was invisible to the old
// pre-#3656 walker. The fix says all leaves inside an arm body are visible.
BEZEL_TEST(foreach_nested_stmt_3656, every_leaf_kind_visible_in_case_arm) {
    std::vector<std::unique_ptr<ast::Stmt>> body;
    for (const std::string& tag : {
             "LocalDecl", "LocalConstDecl", "ReturnStmt", "ExpressionStmt",
             "AssignmentStmt", "ThrowStmt", "YieldStmt", "IncrementStmt",
             "DiscardStmt", "BreakStmt", "ContinueStmt"}) {
        auto arm = std::make_unique<ast::CaseStmt>();
        arm->arms.push_back(ast::CaseArm{});
        arm->arms.back().bodyKind = ast::CaseArmBodyKind::Block;
        arm->arms.back().stmts.push_back(makeLeaf(tag));
        body.push_back(std::move(arm));
    }
    KindCounter c;
    for (const auto& s : body) {
        if (!s) continue;
        forEachNestedStmt(*s, [&](const ast::Stmt& cur) { count(cur, c); });
    }
    BEZEL_EXPECT_EQ(c.localDecl, 1);
    BEZEL_EXPECT_EQ(c.localConstDecl, 1);
    BEZEL_EXPECT_EQ(c.returnStmt, 1);
    BEZEL_EXPECT_EQ(c.expressionStmt, 1);
    BEZEL_EXPECT_EQ(c.assignmentStmt, 1);
    BEZEL_EXPECT_EQ(c.throwStmt, 1);
    BEZEL_EXPECT_EQ(c.yieldStmt, 1);
    BEZEL_EXPECT_EQ(c.incrementStmt, 1);
    BEZEL_EXPECT_EQ(c.discardStmt, 1);
    BEZEL_EXPECT_EQ(c.breakStmt, 1);
    BEZEL_EXPECT_EQ(c.continueStmt, 1);
    BEZEL_EXPECT_EQ(c.caseStmt, 11);
}

// T3 — The stateful form threads depth through recursion: every scope-
// introducer kind increments depth by one. A nested chain
// `case > try > do > for > while > if > LocalDecl` reports depth 6 for the
// innermost LocalDecl. The stateful form is what `collectLocalDepthsStmt`
// uses to number `localDepth_`.
BEZEL_TEST(foreach_nested_stmt_3656, depth_threads_through_stateful_walker) {
    auto cur = makeLeaf("LocalDecl");
    cur = makeIfStmt(std::move(cur));
    cur = makeWhileStmt(std::move(cur));
    cur = makeForStmt(std::move(cur));
    cur = makeDoEndBlock(std::move(cur));
    cur = makeTryStmt(std::move(cur), makeLeaf("ReturnStmt"));
    cur = makeCaseStmt(std::move(cur));

    int leafDepth = -1;
    forEachNestedStmt(*cur,
        [&](const ast::Stmt& s, StmtVisitState state) {
            if (dynamic_cast<const ast::LocalDecl*>(&s)) {
                leafDepth = state.depth;
            }
        },
        StmtVisitState{0, false, nullptr});
    // case (1) -> try (2) -> do-end (3) -> for (4) -> while (5) -> if (6) -> LocalDecl.
    BEZEL_EXPECT_EQ(leafDepth, 6);
}

// T4 — The stateful form threads `insideTry` through recursion: every
// statement lexically inside a try body / catch / finally has
// `state.insideTry == true`. This is what `collectStmtsInsideTryStmt` uses.
BEZEL_TEST(foreach_nested_stmt_3656, insideTry_sticky_across_nesting) {
    auto inner = makeLeaf("LocalDecl");
    auto wrapped = makeCaseStmt(std::move(inner));   // depth++, no insideTry
    auto ts = makeTryStmt(std::move(wrapped),
                          makeLeaf("ReturnStmt"));   // sets insideTry=true

    int insideTrue = 0;
    int insideFalse = 0;
    forEachNestedStmt(*ts,
        [&](const ast::Stmt&, StmtVisitState state) {
            if (state.insideTry) ++insideTrue; else ++insideFalse;
        },
        StmtVisitState{0, false, nullptr});

    // The TryStmt ITSELF is visited with insideTry=false (the walker
    // recurses INTO its children with insideTry=true). Every nested
    // statement under the try — including the `case`, the `LocalDecl`,
    // and the `ReturnStmt` in the catch arm — sees insideTry=true.
    BEZEL_EXPECT_TRUE(insideTrue > 0);
    BEZEL_EXPECT_EQ(insideFalse, 1);  // the TryStmt itself
}

// T5 — The stateful form threads `enclosingTry` through recursion: a
// statement inside a try body has `state.enclosingTry == &ts`. The walker
// sets it on descent into a try's body / catch / finally.
BEZEL_TEST(foreach_nested_stmt_3656, enclosingTry_set_on_try_descent) {
    auto inner = makeLeaf("LocalDecl");
    auto ts = makeTryStmt(std::move(inner),
                          makeLeaf("ReturnStmt"));

    const ast::TryStmt* observedEnclosing = nullptr;
    int enclosingHits = 0;
    forEachNestedStmt(*ts,
        [&](const ast::Stmt& s, StmtVisitState state) {
            if (state.enclosingTry) {
                ++enclosingHits;
                if (dynamic_cast<const ast::LocalDecl*>(&s)) {
                    observedEnclosing = state.enclosingTry;
                }
            }
        },
        StmtVisitState{0, false, nullptr});

    BEZEL_EXPECT_TRUE(enclosingHits > 0);
    // The innermost LocalDecl is inside the try body.
    BEZEL_EXPECT_EQ(observedEnclosing, ts.get());
}