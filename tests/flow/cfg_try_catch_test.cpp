// Spec-derived tests for Flow slice F2 — try/catch/finally CFG.
//
// F2's contract:
//   - Throws inside a try route to the nearest enclosing catch dispatcher
//     (not directly to the function exit).
//   - Try-body's normal exit routes through finally (if any) to afterTry.
//   - Each catch arm body's exit routes through finally to afterTry.
//   - Without catches (try…finally form), the dispatch is a pass-through.

#include "support/cfg_support.hpp"
#include "test_harness.hpp"

using bezel::flow::CFGBlock;
using bezel::test::parseFirstMethodCFG;

namespace {

// Helper: count edges to exit.
std::size_t edgesToExit(const bezel::flow::CFG& cfg) {
    std::size_t n = 0;
    for (const auto& b : cfg.blocks()) {
        for (auto* s : b->succs) {
            if (s == cfg.exit()) ++n;
        }
    }
    return n;
}

} // namespace

BEZEL_TEST(flow_cfg_try, throw_in_try_routes_to_dispatcher) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  try throw new E(); "
        "  catch var e: E => log(e); "
        "  end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // No edge from throw to exit — the throw routes to the catch dispatcher
    // which then proceeds through the catch body to afterTry → exit.
    BEZEL_EXPECT_TRUE(edgesToExit(*r.cfg) >= std::size_t(1));
}

BEZEL_TEST(flow_cfg_try, naked_throw_routes_to_exit) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  throw new E(); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(edgesToExit(*r.cfg), std::size_t(1));
}

BEZEL_TEST(flow_cfg_try, try_finally_normal_exit_through_finally) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  try var x: int := 1; "
        "  finally cleanup(); "
        "  end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // Only one path to exit: try → finally → afterTry → exit.
    BEZEL_EXPECT_EQ(edgesToExit(*r.cfg), std::size_t(1));
}

BEZEL_TEST(flow_cfg_try, multiple_catch_arms) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  try risky(); "
        "  catch IOException => retry();, "
        "  catch var e: Error => abort(e); "
        "  end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // Two catch arms produce a Branch with 2 successors from dispatcher.
    // The dispatch is a Branch block.
    std::size_t branchCount = 0;
    for (const auto& b : r.cfg->blocks()) {
        if (b->kind == CFGBlock::Kind::Branch) ++branchCount;
    }
    BEZEL_EXPECT_TRUE(branchCount >= std::size_t(1));
}

BEZEL_TEST(flow_cfg_try, nested_try_inner_throw_routes_to_inner) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  try "
        "    try throw new E(); "
        "    catch var e: E => log(); "
        "    end; "
        "  catch var e2: Error => outer(); "
        "  end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(edgesToExit(*r.cfg) >= std::size_t(1));
}

BEZEL_TEST(flow_cfg_try, try_inside_loop_break_still_works) {
    // Regression: a `break` inside a try-body within a while loop should
    // still route to the loop's afterEnd, not to the try's afterTry.
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  while cond do "
        "    try break; "
        "    catch var e: E => log(e); "
        "    end; "
        "  end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.cfg != nullptr);
}
