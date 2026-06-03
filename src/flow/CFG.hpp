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
#include <unordered_set>
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

    // Optional back-pointer to the AST statement that originated this
    // block. Populated additively (the F1/F2 builder fills it for Merge
    // blocks with the originating `IfStmt`) so analyses that need the
    // source node for diagnostics can recover it — the ownership pass
    // uses it to locate the §28.3 branch-join diagnostic at the if's
    // span. Null when no single statement is responsible.
    const ast::Stmt* sourceStmt = nullptr;

    // True for a loop's condition Branch (the header of a while/for).
    // A back-edge targets this block, and its false successor (succs[1])
    // is the loop-skip / exit edge. Additive: lets a forward analysis
    // distinguish loop headers from if-condition branches without a
    // separate dominator computation.
    bool isLoopHeader = false;

    // F2 / O11-real: successor targets reached only along an *exception*
    // (stack-unwinding) edge from this block — an explicit `throw`, or the
    // over-approximated "a throwable call inside the try body" edge from a
    // try-body end to its catch dispatcher. An analysis can ask, for a
    // given (from → to) edge, whether it is an exception edge by testing
    // whether `to` is in `from->exceptionSuccs`. The ownership pass uses
    // this to model §28.3 "Exception-path discharge": owned locals live on
    // an exception edge are auto-disposed by the implicit unwind, so they
    // are not reported as leaked on that edge. A target may appear in both
    // `succs` (normal) and `exceptionSuccs` (exceptional) when distinct
    // edges to it exist; the set membership marks only the exceptional one.
    std::unordered_set<const CFGBlock*> exceptionSuccs;

    // O11-real: when this block is a try's catch-dispatch (the landing pad
    // for exception edges out of that try's body), `tryForDispatch` points
    // at the originating `TryStmt`. An analysis modelling §28.3
    // exception-path discharge uses it to scope the auto-dispose to exactly
    // the locals whose declaration lives in that try's body — locals
    // declared in *enclosing* scopes remain live in the catch and after the
    // try, so they are not disposed by this unwind. Null for every other
    // block (and for the function exit, where an uncaught exception unwinds
    // the whole method, so *all* in-scope owned locals are disposed).
    const ast::TryStmt* tryForDispatch = nullptr;
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

    // F2 / O11-real: add an exception (stack-unwinding) edge from `from`
    // to `to`. Maintains the same succ/pred lists as a normal edge, and
    // additionally records the edge in `from->exceptionSuccs` so an
    // analysis can distinguish exceptional from normal control transfer.
    void addExceptionEdge(CFGBlock* from, CFGBlock* to);

    // True when the (from → to) edge was created as an exception edge.
    static bool isExceptionEdge(const CFGBlock* from, const CFGBlock* to) {
        return from->exceptionSuccs.count(to) != 0;
    }

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
//
// `treatTryAsOpaque` (additive, default false): when true, a `TryStmt`
// is NOT expanded into the F2 try/catch/finally exception-edge shape —
// it is emitted as a single opaque statement in the current linear
// block, exactly like an unknown statement kind. This was used by the
// ownership pass during slices O3a/O11-simple while it still walked
// try/catch structurally. As of O11-real the ownership pass consumes the
// real exception-edge expansion (`treatTryAsOpaque=false`, the default)
// and models §28.3 exception-path discharge per `exceptionSuccs`. The
// flag is retained for any consumer that still wants the opaque shape.
std::unique_ptr<CFG> buildCFG(
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    bool treatTryAsOpaque = false);

} // namespace bezel::flow
