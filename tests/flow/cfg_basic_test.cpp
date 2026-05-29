// Spec-derived tests for the Flow slice F1 — CFG infrastructure.
//
// F1's contract: build a CFG over a method body containing linear stmts,
// if/elif/else, while, for, do/end, return, throw, break, continue. The
// CFG must:
//   - have a single Entry block (no preds) and single Exit block
//     (no succs).
//   - represent each Branch with multiple successors.
//   - close every reachable path to Exit.
//   - route break/continue to the enclosing loop's exit/header.

#include "support/cfg_support.hpp"
#include "test_harness.hpp"

using bezel::flow::CFGBlock;
using bezel::test::parseFirstMethodCFG;

namespace {

// Count successors of a block whose target is the CFG's exit block.
std::size_t edgesToExit(const bezel::flow::CFG& cfg) {
    std::size_t n = 0;
    for (const auto& b : cfg.blocks()) {
        for (auto* s : b->succs) {
            if (s == cfg.exit()) ++n;
        }
    }
    return n;
}

// Convenience: total branch blocks.
std::size_t branchBlocks(const bezel::flow::CFG& cfg) {
    std::size_t n = 0;
    for (const auto& b : cfg.blocks()) {
        if (b->kind == CFGBlock::Kind::Branch) ++n;
    }
    return n;
}

} // namespace

BEZEL_TEST(flow_cfg, empty_body_entry_to_exit) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.cfg != nullptr);
    if (r.cfg) {
        // Entry → some-block → Exit: at minimum the entry has a successor.
        BEZEL_EXPECT_TRUE(r.cfg->entry()->preds.empty());
        BEZEL_EXPECT_TRUE(r.cfg->exit()->succs.empty());
        BEZEL_EXPECT_EQ(edgesToExit(*r.cfg), std::size_t(1));
    }
}

BEZEL_TEST(flow_cfg, linear_body_one_thread) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  var x: int := 1; "
        "  var y: int := 2; "
        "  var z: int := 3; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(branchBlocks(*r.cfg), std::size_t(0));
    BEZEL_EXPECT_EQ(edgesToExit(*r.cfg), std::size_t(1));
}

BEZEL_TEST(flow_cfg, return_terminates_path) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M(): int "
        "  return 1; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(edgesToExit(*r.cfg), std::size_t(1));
}

BEZEL_TEST(flow_cfg, if_else_two_paths_join) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M(): int "
        "  if cond then return 1; else return 2; end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // Both arms return; the entry → if-branch is the only non-trivial.
    // The implicit fall-through edge into the merge isn't taken (both
    // arms exit early); the merge block still exists for join hygiene.
    BEZEL_EXPECT_TRUE(branchBlocks(*r.cfg) >= std::size_t(1));
    BEZEL_EXPECT_TRUE(edgesToExit(*r.cfg) >= std::size_t(2));
}

BEZEL_TEST(flow_cfg, if_no_else_two_paths_meet) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  if cond then var x: int := 1; end; "
        "  var y: int := 2; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(branchBlocks(*r.cfg), std::size_t(1));
    // Only one tail edge to exit (the merge → after → exit).
    BEZEL_EXPECT_EQ(edgesToExit(*r.cfg), std::size_t(1));
}

BEZEL_TEST(flow_cfg, while_loop_has_back_edge) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  while cond do var x: int := 1; end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(branchBlocks(*r.cfg), std::size_t(1));
    // Header has 2 preds (entry + back-edge from body end).
    std::size_t header_preds = 0;
    for (const auto& b : r.cfg->blocks()) {
        if (b->kind == CFGBlock::Kind::Branch) header_preds = b->preds.size();
    }
    BEZEL_EXPECT_EQ(header_preds, std::size_t(2));
}

BEZEL_TEST(flow_cfg, for_loop_back_edge) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  for var x in src do var y: int := x; end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(branchBlocks(*r.cfg), std::size_t(1));
}

BEZEL_TEST(flow_cfg, break_routes_to_loop_after) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  while cond do "
        "    if other then break; end; "
        "  end; "
        "  var done: int := 1; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // Two branches: while header + inner if. The break in the inner-if's
    // then-branch jumps out of the while.
    BEZEL_EXPECT_EQ(branchBlocks(*r.cfg), std::size_t(2));
}

BEZEL_TEST(flow_cfg, continue_routes_to_loop_header) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  while cond do "
        "    if other then continue; end; "
        "    var x: int := 1; "
        "  end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The continue gives an extra predecessor to the loop header.
    std::size_t header_preds = 0;
    for (const auto& b : r.cfg->blocks()) {
        if (b->kind == CFGBlock::Kind::Branch && !b->succs.empty() &&
            b->succs.size() == 2) {
            if (b->preds.size() > header_preds) {
                header_preds = b->preds.size();
            }
        }
    }
    // Header preds: entry edge + body fall-through edge + continue edge.
    BEZEL_EXPECT_TRUE(header_preds >= std::size_t(3));
}

BEZEL_TEST(flow_cfg, do_end_block_inlines) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  do var x: int := 1; var y: int := 2; end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(branchBlocks(*r.cfg), std::size_t(0));
}

BEZEL_TEST(flow_cfg, elif_chain_one_merge) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  if a then var x: int := 1; "
        "  elif b then var y: int := 2; "
        "  elif c then var z: int := 3; "
        "  else var w: int := 4; "
        "  end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // Three condition branches.
    BEZEL_EXPECT_EQ(branchBlocks(*r.cfg), std::size_t(3));
    BEZEL_EXPECT_EQ(edgesToExit(*r.cfg), std::size_t(1));
}

BEZEL_TEST(flow_cfg, throw_terminates_path) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  throw new Exception(); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(edgesToExit(*r.cfg), std::size_t(1));
}

BEZEL_TEST(flow_cfg, nested_if_in_while) {
    auto r = parseFirstMethodCFG(
        "module M; class C public method M() "
        "  while cond do "
        "    if a then var x: int := 1; "
        "    else var y: int := 2; "
        "    end; "
        "  end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(branchBlocks(*r.cfg), std::size_t(2));
}
