// Spec-derived tests for async-readiness slice 3a — CFG await suspend-site
// marker.
//
// Basis. §30 (Asynchronous Programming) opens: the compiler "transforms an
// async method's body into a state machine that suspends and resumes at each
// `await`." §30.1 and §30.2 describe the method *suspending* at each await.
// §30.7 says `await for` "desugars analogously to `for`: it acquires an async
// enumerator with `await using`, calls `MoveNextAsync()` with `await` in a
// loop" — so an `await for` carries an implicit per-iteration await at its
// header. §28.12 gives `await using var` an implicit scope-end `DisposeAsync()`
// await. CFG.hpp:24-25 anticipated this: "await-for shares this CFG — the await
// is just a Suspend node decoration."
//
// Slice 3a's contract (purely ADDITIVE):
//   - A block records, in `awaitSites`, the await-bearing AST nodes within it.
//   - An explicit `await` UnaryExpr (§30.2) is recorded at the block holding
//     the statement / condition that contains it.
//   - `await for` (§30.7) records the ForStmt on the loop header.
//   - `await using` (§28.12) records the LocalDecl.
//   - A body with no `await` has no recorded suspend site anywhere.
//   - The marker changes NO edges and NO block kinds: a body's `succs`,
//     `exceptionSuccs`, block count, and edge count are identical with or
//     without the marker. The try/catch case asserts the exception edges
//     survive alongside the new suspend site.

#include "support/cfg_support.hpp"
#include "test_harness.hpp"

using bezel::flow::CFG;
using bezel::flow::CFGBlock;
using bezel::test::parseFirstMethodCFG;

namespace {

// Total await suspend sites recorded across all blocks of the CFG.
std::size_t totalAwaitSites(const CFG& cfg) {
    std::size_t n = 0;
    for (const auto& b : cfg.blocks()) n += b->awaitSites.size();
    return n;
}

// Count blocks that hold at least one await suspend site.
std::size_t blocksWithAwait(const CFG& cfg) {
    std::size_t n = 0;
    for (const auto& b : cfg.blocks()) {
        if (CFG::hasAwaitSites(b.get())) ++n;
    }
    return n;
}

// Total edges in the CFG (sum of successor counts) — used to confirm the
// additive marker leaves graph shape unchanged.
std::size_t totalEdges(const CFG& cfg) {
    std::size_t n = 0;
    for (const auto& b : cfg.blocks()) n += b->succs.size();
    return n;
}

// Total exception edges in the CFG.
std::size_t totalExceptionEdges(const CFG& cfg) {
    std::size_t n = 0;
    for (const auto& b : cfg.blocks()) n += b->exceptionSuccs.size();
    return n;
}

} // namespace

// §30.2 — a single explicit `await` in an async method body is recorded as
// exactly one suspend site, in exactly one block.
BEZEL_TEST(flow_cfg_suspend, single_await_spec_30_2) {
    auto r = parseFirstMethodCFG(
        "module M; class C "
        "public async method M(): Task "
        "  var x: int := await t; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.cfg != nullptr);
    if (r.cfg) {
        BEZEL_EXPECT_EQ(totalAwaitSites(*r.cfg), std::size_t(1));
        BEZEL_EXPECT_EQ(blocksWithAwait(*r.cfg), std::size_t(1));
    }
}

// §30.2 — `await` is a unary-precedence prefix operator (§17.1); an `await`
// nested in a larger expression is still found by the sub-expression walk.
BEZEL_TEST(flow_cfg_suspend, nested_await_in_expression_spec_30_2) {
    // `(await a) + (await b)` — two suspend sites in one statement / block.
    auto r = parseFirstMethodCFG(
        "module M; class C "
        "public async method M(): Task "
        "  var x: int := (await a) + (await b); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.cfg != nullptr);
    if (r.cfg) {
        BEZEL_EXPECT_EQ(totalAwaitSites(*r.cfg), std::size_t(2));
        BEZEL_EXPECT_EQ(blocksWithAwait(*r.cfg), std::size_t(1));
    }
}

// §30.7 — `await for` records one suspend site (the implicit per-iteration
// `MoveNextAsync()` await) on the loop header block.
BEZEL_TEST(flow_cfg_suspend, await_for_records_loop_await_spec_30_7) {
    auto r = parseFirstMethodCFG(
        "module M; class C "
        "public async method M(): Task "
        "  await for var line in src do "
        "    Process(line); "
        "  end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.cfg != nullptr);
    if (r.cfg) {
        BEZEL_EXPECT_EQ(totalAwaitSites(*r.cfg), std::size_t(1));
        // The site is on a loop header (Branch block flagged isLoopHeader).
        bool onHeader = false;
        for (const auto& b : r.cfg->blocks()) {
            if (CFG::hasAwaitSites(b.get())) {
                onHeader = b->isLoopHeader &&
                           b->kind == CFGBlock::Kind::Branch;
            }
        }
        BEZEL_EXPECT_TRUE(onHeader);
    }
}

// A *synchronous* `for` records no suspend site (the loop is not `await for`).
BEZEL_TEST(flow_cfg_suspend, sync_for_records_no_await_spec_30_7) {
    auto r = parseFirstMethodCFG(
        "module M; class C "
        "public method M() "
        "  for var x in src do "
        "    Use(x); "
        "  end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.cfg != nullptr);
    if (r.cfg) BEZEL_EXPECT_EQ(totalAwaitSites(*r.cfg), std::size_t(0));
}

// §28.12 — `await using var` records its scope-end `DisposeAsync()` implicit
// await (the LocalDecl), plus the explicit `await` in its initializer.
BEZEL_TEST(flow_cfg_suspend, await_using_records_dispose_await_spec_28_12) {
    auto r = parseFirstMethodCFG(
        "module M; class C "
        "public async method M(): Task "
        "  await using var c := await ConnectAsync(); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.cfg != nullptr);
    if (r.cfg) {
        // Two sites in the same block: the initializer's explicit await
        // (UnaryExpr) and the implicit dispose await (the LocalDecl).
        BEZEL_EXPECT_EQ(totalAwaitSites(*r.cfg), std::size_t(2));
        BEZEL_EXPECT_EQ(blocksWithAwait(*r.cfg), std::size_t(1));
    }
}

// A method with no `await` records no suspend site anywhere — the marker is
// strictly opt-in to await-bearing constructs.
BEZEL_TEST(flow_cfg_suspend, no_await_no_marker) {
    auto r = parseFirstMethodCFG(
        "module M; class C "
        "public method M() "
        "  var x: int := 1; "
        "  if cond then x := 2; else x := 3; end; "
        "  while go do step(); end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.cfg != nullptr);
    if (r.cfg) {
        BEZEL_EXPECT_EQ(totalAwaitSites(*r.cfg), std::size_t(0));
        BEZEL_EXPECT_EQ(blocksWithAwait(*r.cfg), std::size_t(0));
    }
}

// Additivity (load-bearing): an `await` inside a try body records a suspend
// site AND the F2 exception edges are still present and unchanged. The
// suspend marker must not perturb the exception-edge machinery it mirrors.
BEZEL_TEST(flow_cfg_suspend, await_in_try_keeps_exception_edges) {
    auto r = parseFirstMethodCFG(
        "module M; class C "
        "public async method M(): Task "
        "  try "
        "    var x: int := await t; "
        "  catch var e: E => log(e); "
        "  end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.cfg != nullptr);
    if (r.cfg) {
        // The suspend site was recorded.
        BEZEL_EXPECT_EQ(totalAwaitSites(*r.cfg), std::size_t(1));
        // ...and the exception edge(s) from the try body to the catch
        // dispatcher survive (F2 invariant — additive marker did not touch
        // them).
        BEZEL_EXPECT_TRUE(totalExceptionEdges(*r.cfg) >= std::size_t(1));
        // The catch dispatcher's exception-edge tagging is intact.
        bool dispatchHasExcEdge = false;
        for (const auto& b : r.cfg->blocks()) {
            for (auto* s : b->succs) {
                if (CFG::isExceptionEdge(b.get(), s)) dispatchHasExcEdge = true;
            }
        }
        BEZEL_EXPECT_TRUE(dispatchHasExcEdge);
    }
}

// Additivity (load-bearing): adding `await` to an otherwise-identical body
// changes ONLY the await-site set — block count and edge count are byte-for-
// byte unchanged. The async version differs from the sync version only by the
// `await` keyword; their CFG shapes must match exactly.
BEZEL_TEST(flow_cfg_suspend, marker_does_not_change_graph_shape) {
    auto sync = parseFirstMethodCFG(
        "module M; class C "
        "public method M() "
        "  var x: int := t; "
        "end M; end C;");
    auto async = parseFirstMethodCFG(
        "module M; class C "
        "public async method M(): Task "
        "  var x: int := await t; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(sync.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(async.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sync.cfg != nullptr && async.cfg != nullptr);
    if (sync.cfg && async.cfg) {
        // Same block count, same edge count, same exception-edge count.
        BEZEL_EXPECT_EQ(sync.cfg->size(), async.cfg->size());
        BEZEL_EXPECT_EQ(totalEdges(*sync.cfg), totalEdges(*async.cfg));
        BEZEL_EXPECT_EQ(totalExceptionEdges(*sync.cfg),
                        totalExceptionEdges(*async.cfg));
        // The ONLY difference is the suspend-site set.
        BEZEL_EXPECT_EQ(totalAwaitSites(*sync.cfg), std::size_t(0));
        BEZEL_EXPECT_EQ(totalAwaitSites(*async.cfg), std::size_t(1));
    }
}

// An `await` in an `if` condition is recorded on the condition's branch block
// (§30.2: the condition is evaluated, and suspends, before the branch).
BEZEL_TEST(flow_cfg_suspend, await_in_if_condition_spec_30_2) {
    auto r = parseFirstMethodCFG(
        "module M; class C "
        "public async method M(): Task "
        "  if await ready then act(); end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.cfg != nullptr);
    if (r.cfg) {
        BEZEL_EXPECT_EQ(totalAwaitSites(*r.cfg), std::size_t(1));
        bool onBranch = false;
        for (const auto& b : r.cfg->blocks()) {
            if (CFG::hasAwaitSites(b.get())) {
                onBranch = b->kind == CFGBlock::Kind::Branch;
            }
        }
        BEZEL_EXPECT_TRUE(onBranch);
    }
}
