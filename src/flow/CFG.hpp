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
//
// Async-readiness 3a — await suspend-site marker
// ----------------------------------------------
//
// Realising the "the await is just a Suspend node decoration" note above
// (line 24-25): this slice adds a purely ADDITIVE per-block record of the
// `await` suspend sites within each block (`CFGBlock::awaitSites`, populated
// via `CFG::recordAwaitSite`). Per §30, an async method "suspends and resumes
// at each `await`"; this marks where those suspensions are so a later
// async-ownership pass can find them. The marker mirrors the F2 exception-edge
// machinery (`exceptionSuccs` / `addExceptionEdge` / `isExceptionEdge`): it is
// metadata only — no edges change, no block kinds change, no existing
// traversal is affected. The CFG is NOT yet split into await-bounded blocks
// (that is the codegen state-machine milestone, slice 3c).

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

    // §29.2 (#834): when this Linear block is the BODY-ENTRY of a `for`/`await
    // for` loop, `forBodyEntry` points at the originating `ForStmt`. The
    // ownership pass uses it to seed the iteration variable as Owned at the
    // body entry of EACH iteration when the loop's iterable is an owning
    // enumerable (`IOwningEnumerable<T>` / `IOwningAsyncEnumerable<T>`), so the
    // standard §28.3 owned-local discharge check rejects an undischarged
    // iteration variable ("Iterator yield ownership", §29.2). Null for every
    // other block (including a borrowing for-loop's body entry — the seeding
    // is gated on the iterable type, not on this pointer alone). Additive;
    // populated only by the CFG builder's visitFor.
    const ast::ForStmt* forBodyEntry = nullptr;

    // #2109 (§30.7 / §28.3): when a `for`/`await for` loop originates at
    // this block (i.e. its iterable and optional `with` cancellation-token
    // expression are evaluated in this block, before the for-header Branch),
    // `forOrigins` lists every such ForStmt. The ownership pass iterates this
    // list once per block visit and runs `recordMovesIn` on each loop's
    // iterable/with expression — the ONLY walk that ever reached for-loop
    // arguments, because the iterable is NOT part of any block's `stmts`.
    // Using `recordMovesIn` (move-recording only) rather than `handleDischarge`
    // is intentional: the iterable's RESULT is consumed by the enumerator
    // frame (§29.8 disposes it), so attempting to discharge the call's result
    // here would conflict with the enumerator-disposal path. Without this
    // field a named-local owned-form iterable
    // (`Task.WhenEachOwned(owned tasks)`) is recorded as a plain read of
    // `tasks`, leaving it Owned at scope end — the §28.3 rejection the issue
    // pins. Empty for every block that does not originate a for/await-for.
    // Additive; populated only by the CFG builder's visitFor.
    std::vector<const ast::ForStmt*> forOrigins;

    // #764: for an if/elif condition Branch block, the condition expression —
    // so a forward analysis can null-discriminate the successors (`x <> null`
    // makes x non-null on succs[0] and null on succs[1]; `x = null` the reverse).
    // Null for loop-header Branches and non-Branch blocks. Additive; populated by
    // the CFG builder's visitBranch.
    const ast::Expr* condition = nullptr;

    // #1591 (§31.1 / §31.2.1): for a catch arm's Linear entry block, the arm's
    // optional `when` guard expression. A guard is an ordinary expression
    // position and may itself embed a `throw` (§31.2.1: a guard that diverges
    // supersedes the in-flight exception and propagates), so it must run
    // through the same ownership transfer (`handleDischarge`, which enforces
    // §31.1's throw-operand owned-value rule among other owned-argument-
    // transfer checks) as every other expression position. Before this field
    // existed the guard was never represented in the CFG at all — `visitTry`
    // built the arm's entry block from `arm.body` only — so a guard's throw
    // operand ownership went unchecked. Null when the arm has no guard, and
    // for every non-catch-arm block. Additive; populated by the CFG builder's
    // `visitTry`.
    const ast::Expr* catchGuard = nullptr;

    // fix #4146 (§19, §28.2, §28.3 K1.4, v0.298): for a `case` STATEMENT's
    // entry block (the block the case's scrutinee is evaluated in, before
    // the arm Branch blocks fan out), the scrutinee Expr. A `case` has no
    // condition at the Branch block level — the pattern match itself is
    // codegen-only and Pattern is not an Expr — so a separate back-pointer
    // to the scrutinee is needed for the ownership pass's K8 full-
    // expression boundary mark (`disposeOwnedScrutinee`, §17.13 / #1795)
    // and the §28.8 borrow→`owned` argument check on the scrutinee itself
    // (#4116's own commit message names this as the reason the per-method
    // CaseStmt walker had to walk the scrutinee by hand). Null for every
    // non-case-statement block, and for a case statement whose scrutinee
    // is omitted (the `when` statement form, parseWhenStmt in
    // Parser_Stmts.cpp, parses into CaseStmt with a null scrutinee).
    // Additive; populated only by the CFG builder's `visitCase`.
    const ast::Expr* caseScrutinee = nullptr;

    // fix #4146 (§19, §28.2, §28.3 K1.4, v0.298): for a `case` STATEMENT's
    // expression-form arm Branch block, the arm body's expression
    // (`case EXPR of P1 => EXPR1, P2 => EXPR2, ... end`). Block-form arms
    // (`do … end`) hold their stmts as a real CFG block sequence via the
    // arm-entry→body→merge edges built by `visitCase`; expression-form arms
    // have no body block sequence and need this back-pointer so the
    // ownership pass can fire `handleDischarge` on the expression
    // (mirrors what `visitTry` does for a catch arm with no `body` field).
    // Null for every non-expression-form-arm block. Additive; populated
    // only by the CFG builder's `visitCase`.
    const ast::Expr* caseArmExprBody = nullptr;

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

    // Async-readiness 3a: the set of `await` suspend sites that occur within
    // this block. Per §30, the compiler "transforms an async method's body
    // into a state machine that suspends and resumes at each `await`" (§30
    // intro), and §30.1/§30.2 describe the method *suspending* at each await.
    // A suspend point is therefore a *position within a block* (an `await`
    // is mid-expression — §17.1 makes it a unary-precedence prefix operator),
    // not a block boundary. For slice 3a it is sufficient to record, per
    // block, the set of await-bearing AST nodes it contains, so a later
    // async-ownership pass can find every suspend point and slice 3c can
    // split/annotate the block around each. This deliberately does NOT
    // restructure the CFG into await-split blocks — that is the codegen
    // milestone. See the file-header note: "await-for shares this CFG — the
    // await is just a Suspend node decoration" (CFG.hpp:24-25).
    //
    // What is recorded as an await site:
    //   - an explicit `await` UnaryExpr (§30.2): the UnaryExpr node whose
    //     `op == "await"` reachable from a statement appended to this block;
    //   - an `await using var` LocalDecl (§28.12): the implicit await of the
    //     scope-end `DisposeAsync()` — recorded as the LocalDecl node, since
    //     no UnaryExpr exists for the implicit await (the initializer's own
    //     explicit awaits, if any, are recorded as their UnaryExpr nodes too);
    //   - an `await for` ForStmt (§30.7): the implicit per-iteration await of
    //     `MoveNextAsync()` — recorded as the ForStmt node on the loop's
    //     header (Branch) block, which the desugaring re-evaluates each pass.
    //
    // Purely ADDITIVE: this field is only populated by CFG construction and
    // only read by future async-aware code. `succs` (normal) and
    // `exceptionSuccs` (exceptional) and every existing traversal are
    // unchanged. Empty for every block in a body with no `await`.
    std::unordered_set<const ast::Node*> awaitSites;

    // Async-readiness 3c: synthetic *completion* edges. An async method's
    // state machine has completion exits BEYOND the synchronous ones: besides
    // normal return and fault/exception (the latter already modelled by
    // `exceptionSuccs`), the task can be **abandoned** (dropped without being
    // awaited to completion — §30.5 "Disposing an unconsumed `OwningTask<T>`")
    // or **cancelled** (§30.8 — observed at an `await` as
    // `OperationCanceledException`). On those completion edges the state-
    // machine frame disposes every owned local stored in it (every
    // *suspend-local* — one live across an `await`), exactly as the implicit
    // try/finally disposes owned locals on an exception edge. This set tags a
    // (from → to) transfer as such a synthetic completion edge so the async
    // ownership analysis treats a suspend-local as discharged along it (the
    // §30.5 frame-disposal view) rather than reporting it as leaked.
    //
    // The edge is "synthetic" because abandonment/cancellation are not
    // statements in the body; the async ownership pass adds an edge from each
    // await-bearing block to the function exit to represent "the task could
    // complete here via abandonment/cancellation, and the frame disposes its
    // suspend-locals." Mirrors `exceptionSuccs` / `addExceptionEdge`: metadata
    // only — it is added to `succs`/`preds` as a real transfer but tagged so
    // an analysis can give it the frame-disposal treatment. Empty for every
    // block in a non-async body and for blocks with no await sites.
    std::unordered_set<const CFGBlock*> completionSuccs;
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

    // Async-readiness 3a: record that the given await-bearing AST node is a
    // suspend site located within `block` (§30). Purely additive — it adds
    // `node` to `block->awaitSites` and touches nothing else (no edges, no
    // block kinds). Mirrors the exception-edge machinery: just as
    // `addExceptionEdge` tags a control transfer for a later analysis,
    // `recordAwaitSite` tags a suspend position for the future async-ownership
    // pass. Idempotent (set insertion); ignores a null node defensively.
    void recordAwaitSite(CFGBlock* block, const ast::Node* node) {
        if (node) block->awaitSites.insert(node);
    }

    // True when `block` holds at least one recorded await suspend site.
    static bool hasAwaitSites(const CFGBlock* block) {
        return !block->awaitSites.empty();
    }

    // Async-readiness 3c: add a synthetic *completion* edge from `from` to
    // `to` (typically the function exit). Like `addExceptionEdge` it is a real
    // control transfer in `succs`/`preds`, additionally tagged in
    // `from->completionSuccs` so an analysis can apply the §30.5 state-machine
    // frame-disposal treatment to a suspend-local along it (abandonment /
    // cancellation completion of the task). Idempotent for the tag; the
    // succ/pred lists may gain a duplicate edge to a target that is already a
    // normal successor, which the analysis tolerates (set membership marks
    // only the synthetic one). The CFG builder does NOT add these — the async
    // ownership pass adds them after deciding which blocks bear awaits.
    void addCompletionEdge(CFGBlock* from, CFGBlock* to) {
        from->succs.push_back(to);
        to->preds.push_back(from);
        from->completionSuccs.insert(to);
    }

    // True when the (from → to) edge was created as a synthetic completion
    // edge (abandonment / cancellation; §30.5 / §30.8).
    static bool isCompletionEdge(const CFGBlock* from, const CFGBlock* to) {
        return from->completionSuccs.count(to) != 0;
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
