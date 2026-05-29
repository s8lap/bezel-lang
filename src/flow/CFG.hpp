#pragma once

// Control-Flow Graph infrastructure for method-body analyses (slice F1).
//
// A CFG represents one method body / property accessor / constructor /
// destructor / lambda body as a graph of basic blocks. Each block holds a
// straight-line sequence of statements; control flow between blocks is the
// successor / predecessor edge list. Branches and loops produce multi-
// successor / multi-predecessor blocks; the merge of a branch is a fresh
// block that the successors point at.
//
// CFG users (Ownership, DefiniteAssignment, Throwability) consume one CFG
// per analysed body and run dataflow over it. The CFG is read-only after
// construction.
//
// What's in slice F1
// ------------------
//
//   - Linear blocks (sequences of LocalDecl, ExpressionStmt, AssignmentStmt,
//     LocalConstDecl, DiscardStmt, IncrementStmt, ReturnStmt — anything
//     without branching).
//   - IfStmt with elif + else, building a fresh merge block.
//   - WhileStmt with a back-edge from the body to the condition.
//   - ForStmt with the same back-edge shape (sync form; await-for shares
//     this CFG — the await is just a Suspend node decoration).
//   - DoEndBlock — pure statement-sequencing block.
//   - ReturnStmt — terminator edge to the function exit.
//   - BreakStmt / ContinueStmt — terminator edges to the nearest enclosing
//     loop's exit / header.
//
// What's deferred to F2/F3
// ------------------------
//
//   - TryStmt + catch + finally + exception edges (F2).
//   - YieldStmt iterator-body shape (F3).
//   - Lambda bodies + accessor bodies (F3).
//   - `case` statement-body dispatch (F2 — once exception edges are in,
//     the case has a similar branching shape).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ast/Node.hpp"

namespace bezel::flow {

struct CFGBlock {
    enum class Kind {
        Entry,       // unique synthetic entry; one per CFG
        Exit,        // unique synthetic exit; one per CFG
        Linear,      // body holds a straight-line statement sequence
        Branch,      // 1 predecessor, 2+ successors (if / while header /
                     // for header — guarded transfer)
        Merge,       // 2+ predecessors, 1 successor (after if/elif/else)
    };
    Kind kind = Kind::Linear;

    // Statements held by this block. For Branch blocks, the `condition`
    // is the only stmt represented; for Linear blocks it's the straight-
    // line sequence; for Merge blocks it's empty.
    std::vector<const ast::Stmt*> stmts;

    // Successors / predecessors in graph order. The semantics depends on
    // the block's kind: for Branch, succs[0] is the true edge and
    // succs[1] is the false / else edge (or the next-elif for chained
    // conditions). The CFG construction documents the convention.
    std::vector<CFGBlock*> succs;
    std::vector<CFGBlock*> preds;

    // ID for diagnostics — stable across rebuilds for the same body.
    std::uint32_t id = 0;
};

class CFG {
public:
    CFG();

    CFGBlock* entry() const { return entry_; }
    CFGBlock* exit() const { return exit_; }

    // Total block count, including entry and exit. Useful in tests.
    std::size_t size() const { return blocks_.size(); }

    // Iteration in allocation order.
    const std::vector<std::unique_ptr<CFGBlock>>& blocks() const {
        return blocks_;
    }

    // Allocate a fresh block. Used by the builder; tests can construct
    // CFGs manually for unit-testing the dataflow infrastructure.
    CFGBlock* makeBlock(CFGBlock::Kind k);

    // Add an edge from `from` to `to`. Maintains both successor and
    // predecessor lists.
    void addEdge(CFGBlock* from, CFGBlock* to);

private:
    std::vector<std::unique_ptr<CFGBlock>> blocks_;
    CFGBlock* entry_ = nullptr;
    CFGBlock* exit_ = nullptr;
    std::uint32_t nextId_ = 0;
};

// Build a CFG for a method-body-shape sequence of statements. The
// returned CFG is owned by the caller. Slice F1 supports the statement
// kinds documented in the file-header comment; encountering an unknown
// shape produces a single Linear block containing the statement (so
// later analyses see it as opaque rather than crashing).
std::unique_ptr<CFG> buildCFG(
    const std::vector<std::unique_ptr<ast::Stmt>>& body);

} // namespace bezel::flow
