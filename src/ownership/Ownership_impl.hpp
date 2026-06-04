#pragma once

// Private implementation header for the per-method ownership analyzer
// (cf. codegen/LlvmBackend_impl.hpp).
//
// Declares the `MethodOwnership` worker class whose method definitions are
// distributed across Ownership.cpp and Ownership_Checks.cpp. This header is
// included ONLY by those implementation files; the public surface stays in
// Ownership.hpp (checkOwnership). Keeping the class out of the public header
// preserves the encapsulation the previous anonymous-namespace placement
// gave, while allowing the definitions to live in separately-compiled units.

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/Node.hpp"
#include "flow/CFG.hpp"
#include "ownership/OwnershipState.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"

namespace bezel::ownership {

// Per-method ownership analyzer.
//
// Slice O3a: the analysis runs as a forward dataflow over the method
// body's control-flow graph (`bezel::flow::CFG`) rather than the
// original syntactic statement walker. The per-statement transfer
// functions (handleLocalDecl / handleDischarge / handleAssignment /
// handleReturn and the move/dispose/read helpers in Ownership_Checks.cpp)
// are unchanged; the driver feeds each CFG basic block's straight-line
// statement sequence through them, and merges predecessor exit states at
// block entry using the §28.3 asymmetric meet (`joinInto`).
//
//   - if/elif/else joins flow through real CFG merge blocks (the merge
//     carries the originating `IfStmt` so the clash diagnostic lands at
//     the if's span);
//   - loops are iterated to a fixpoint (O3b): back-edges and the zero-trip
//     header→after skip edge participate in the meet, so the analysis is
//     sound for the never-executed loop path;
//   - try/catch/finally is expanded into the real F2 exception-edge CFG
//     (O11-real, `treatTryAsOpaque=false`). The driver applies §28.3
//     "Exception-path discharge" per edge: when state flows along an
//     exception (unwinding) edge — a `throw`, or a throwable call inside a
//     try body — every live owned local is auto-disposed by the implicit
//     try/finally, so it is not reported as leaked on that edge.
using BlockState = std::unordered_map<sema::Symbol*, LocalOwnership>;

class MethodOwnership {
public:
    MethodOwnership(sema::SemaContext& ctx) : ctx_(ctx) {}

    // Run the linear walk. `body` is the method/ctor/lambda body's
    // statement list. `paramSyms` (slice O10) is the matching list of
    // ParameterSymbols — `owned` params are introduced as Owned locals
    // at method entry per §28.8 and must reach a discharge point on
    // every normal-exit path (same rule as `var x := producer-of-owned;`).
    void run(const std::vector<std::unique_ptr<ast::Stmt>>& body,
             const std::vector<sema::Symbol*>* paramSyms = nullptr);

private:
    sema::SemaContext& ctx_;
    std::unordered_map<sema::Symbol*, LocalOwnership> states_;
    // Slice O8: per-method tracker for `move this.field` — the field's
    // name maps to its moved-from state. fieldMoveLoc_ stores the
    // move-site location for diagnostics.
    std::unordered_map<std::string_view, bool> fieldMovedFrom_;
    std::unordered_map<std::string_view, SourceLocation> fieldMoveLoc_;

    // Slice O3b: the loop fixpoint re-processes loop bodies until the
    // per-block states stabilize. The transfer functions emit diagnostics
    // as a side effect; running them while iterating to the fixpoint would
    // emit each diagnostic once per pass. So the fixpoint iterations run
    // with diagnostics suppressed, and a single final pass
    // (suppressDiag_ == false) over the stabilized entry states emits each
    // diagnostic exactly once. Every transfer-function report routes
    // through `report()` so the flag is honoured in one place.
    bool suppressDiag_ = false;
    void report(Severity sev, SourceLocation loc, std::string msg) {
        if (suppressDiag_) return;
        ctx_.diagnostics().report(sev, loc, std::move(msg));
    }

    void visitStmt(const ast::Stmt& s);

    // Categorize the initializer expression to determine the local's
    // initial ownership state (slice O1 producer table — minimal).
    OwnershipState producerState(const ast::Expr& e);

    void handleLocalDecl(const ast::LocalDecl& ld);

    // Walk an expression and side-effect any `move` operations it
    // contains. Used by handleLocalDecl so `move`-in-initializer
    // properly transitions the source's state.
    void recordMovesIn(const ast::Expr& e);

    void recordMovesInArgs(const std::vector<ast::CallArgument>& args,
                           const ast::Expr* callee = nullptr);

    // Detect `local.Dispose()` patterns and mark the local as Disposed.
    // Also detects `local` consumed as a `return` value (handleReturn
    // path) and `move local` (UnaryExpr op=move on bare identifier).
    //
    // Slice O2: before marking, check the current state — re-disposing
    // or re-moving an already-Disposed/MovedFrom local is a diagnostic
    // (the §28.3 "read of moved-from / disposed" rules).
    // §28.8: check if argument at position `pos` in `args` is an implicit
    // ownership transfer based on the callee's formal parameter list.
    bool isImplicitOwned(const std::vector<ast::CallArgument>& args,
                         std::size_t pos, const ast::Expr* callee);

    void handleDischarge(const ast::Expr& e);

    // Recursively descend an expression and, at each IdentifierExpr that
    // names a tracked local, verify the local's ownership state is
    // readable. `selfTarget=true` for the `x` in `x.Dispose()` — we
    // tolerate that as the discharge use site; the post-marking handles
    // it. For everything else, MovedFrom / Disposed is a diagnostic.
    void checkRead(const ast::Expr& e, bool selfTarget);

    void handleAssignment(const ast::AssignmentStmt& as);

    void handleReturn(const ast::Expr& e);

    // A returned value is only a *borrow* of its local root when it has
    // reference type; a value-typed projection (e.g. `r.V` where V is an
    // int, or `r.Size()` returning int) is a copy and cannot dangle.
    // Generic type parameters (Named with no declaration) are treated as
    // non-borrowable: the ownership checker runs on generic code without
    // knowing the concrete instantiation (e.g. Box<int>.Value has sema type
    // T which looks like an unresolved Named), and conservatively treating T
    // as borrowable produces false positives for value-type instantiations.
    // A missed borrow for T=class is acceptable at this stage (§14).
    static bool isBorrowableType(const sema::TypeRef* rt);

    // §28.3 borrow-escape gate: true when the returned expression could
    // be a borrow aliasing into its identifier root (so it must be checked
    // against `using var` scope). False when the returned value is a
    // value-typed copy (an int/bool/enum/record-by-value projection),
    // which can never outlive its source. Prefers sema's resolved expr
    // type (`isBorrowableType`); for a bare property/field access that
    // sema left untyped — most notably a built-in collection's value-
    // typed `Length`/`Count`/`IsEmpty` projection, which sema types only
    // on the call form — it falls back to recognizing the spec/stdlib
    // value-typed projection by member name (still a copy, never a borrow).
    bool returnedExprMayBorrow(const ast::Expr& e) const;

    // Walk the return value's expression and, for each path that ends
    // in a local-identifier root, verify the root isn't a `using var`.
    void checkBorrowEscapesOnReturn(const ast::Expr& e);

    // §28.9 closure-escape (issue #360, D1): a returned lambda whose
    // captured set includes an owned (`using var` or owned-producing)
    // method-scope local of reference type. Capturing such a local does
    // not transfer ownership; the capture is a borrow whose source is the
    // local's enclosing scope, and the returned closure outlives that
    // scope — a use-after-dispose. Walks the returned expression for any
    // LambdaExpr and diagnoses each offending capture.
    void checkClosureEscapesOnReturn(const ast::Expr& e);

    // Free-variable walk of a lambda body. Appends to `out` every
    // IdentifierExpr in the body (and in nested lambdas) — sema has
    // already attached each identifier's resolved Symbol via exprSymbol,
    // and a captured method-scope local resolves to the same LocalSymbol
    // key the dataflow tracks in `states_`, so resolving against `states_`
    // by symbol identifies exactly the captured method-scope locals.
    void collectLambdaIdentifiers(
        const ast::LambdaExpr& lam,
        std::vector<const ast::IdentifierExpr*>& out) const;
    void collectIdentifiersIn(
        const ast::Expr& e,
        std::vector<const ast::IdentifierExpr*>& out) const;
    void collectIdentifiersInStmt(
        const ast::Stmt& s,
        std::vector<const ast::IdentifierExpr*>& out) const;

    // §9.7 borrow-bearing-record escape (issue #360, D2): a returned
    // expression whose static type is a borrow-bearing record that
    // carries a borrow of a scope-bound (`using var` / owned) local.
    // Handles the direct `return new Rec(...localUsingVar...)` form.
    void checkRecordBorrowEscapesOnReturn(const ast::Expr& e);

    // §28.9 / §9.7 INDIRECT escape (issue #360, second slice): a returned
    // *bare local* whose tracked borrow-source aliases a still-owned
    // scope-bound local. This covers the tractable indirect forms where the
    // alias was bound into a local one statement earlier:
    //   var b := usingVarLocal;            return b;   // direct re-borrow
    //   var b := usingVarLocal.refField;   return b;   // field-access borrow
    //   var h := new Holder(usingVarLocal); return h;  // record holder
    //   var f := () => usingVarLocal.V;     return f;  // closure holder
    // In each case `b`/`h`/`f`'s LocalOwnership.source was populated at the
    // declaration/assignment site by borrowSourceForExpr (below), pointing at
    // the owned source local; returning the holder lets the borrow outlive
    // that source. Deeper data-flow (store-to-field of an escaping object,
    // multi-hop alias chains) is deferred. Only fires when the source local
    // is still in the Owned state at the return point.
    void checkIndirectEscapeOnReturn(const ast::Expr& e);

    // §28.3 (#386 cases 1+2) INDIRECT escape through an assignment. When the
    // assignment target is a *field of an object* (`obj.f := …`, including
    // `this.f := …`) or an *out / ref / out-owned parameter* (`p := …`), and
    // the RHS borrows an owned scope-bound LOCAL, the borrow is stored into a
    // location that outlives that local — a use-after-dispose:
    //   - A field of any object outlives a method-scope local; §28.3 forbids
    //     assigning a borrow to any field ("class-typed fields that hold a
    //     borrow are not expressible in this version").
    //   - An out / ref parameter's storage is the caller's frame, which
    //     outlives this method; writing a borrow of a local through it lets the
    //     borrow outlive its source (§28.3 borrow-lifetime rules; §22.3 `out`
    //     produces a borrow location for the caller).
    // The source is restricted to an owned *local* (not an owned parameter):
    // assigning an owned parameter into an owned field is the §28.8 ownership-
    // transfer pattern, not a borrow store, so it must not be flagged here.
    // Borrows whose source is a plain parameter are likewise not flagged — the
    // parameter's source outlives this method (the "borrow in, borrow out"
    // pattern). Called from handleAssignment before the RHS state transition.
    void checkBorrowStoreEscape(const ast::AssignmentStmt& as);

    // Like the owned-source half of borrowSourceForExpr but restricted to an
    // owned scope-bound *local* (a `using var` or `var x := <owned producer>`),
    // excluding owned parameters. Returns that local, or null. Used by
    // checkBorrowStoreEscape to flag exactly the use-after-dispose hazard
    // (a borrow of a value that dies at this scope's end) while leaving the
    // §28.8 owned-parameter-into-owned-field transfer alone. // per §28.3
    sema::Symbol* ownedLocalBorrowSource(const ast::Expr& rhs);

    // Compute the owned scope-bound source local that the initializer/RHS
    // expression `rhs` borrows from, or null if it borrows nothing owned.
    // Used to populate LocalOwnership.source so an aliasing local can be
    // recognized as an escape when it is later returned. Recognizes exactly
    // the three borrow-binding shapes the indirect check above relies on:
    //   - a re-borrow rooted at an Owned local (`r` or `r.f` / `r[i]`),
    //   - a borrow-bearing-record construction whose arg aliases an Owned
    //     local (`new Holder(r)`),
    //   - a closure capturing an Owned local (`() => r.V`).
    // Returns the FIRST owned source found. A value-typed result (e.g. an
    // `int` re-borrow `var n := r.V;`) is never reported because the source
    // is only taken when the alias carries a reference/record/closure borrow
    // — the per-shape gating below ensures a value-typed local is never
    // flagged. Conservative: returns null when it cannot prove an owned
    // source, so it never manufactures a false escape.
    sema::Symbol* borrowSourceForExpr(const ast::Expr& rhs);

    // True when `sym` is a tracked local currently in the Owned state — the
    // scope-bound discharge obligation that a borrow may not outlive. Used by
    // borrowSourceForExpr to decide whether a re-borrow / capture aliases an
    // owned source. (A parameter without `owned` enters Borrowed, never Owned,
    // so plain-parameter borrows are correctly not treated as escapes — see
    // the §9.7 param-borrow control.)
    bool isOwnedTrackedLocal(sema::Symbol* sym) const;

    // §9.7: true when `rt` is a record type that holds at least one
    // borrow-carrying field of a non-runtime-managed type (a class/
    // interface reference field, an inline array of reference elements,
    // or another borrow-bearing record). String/function/boxed and
    // value-typed fields do not make a record borrow-bearing.
    bool isBorrowBearingRecord(const sema::TypeRef* rt) const;

    // Find the identifier at the root of a chain like `x.f.g[i].M(...)`.
    // Returns null if the root isn't a bare identifier.
    const ast::IdentifierExpr* identifierRoot(const ast::Expr& e);

    // §28.3 join. Per the spec the join is asymmetric: matching states
    // carry forward (with their auxiliary data, e.g. Owned's discharge
    // obligation), but mismatched owned-vs-borrowed is rejected (the
    // spec's "silently demoting an owned obligation to a borrowed state
    // at the join would leak the resource"). Mutates `into` in place.
    //
    // `emitClash` controls whether the owned-vs-borrowed mismatch reports
    // a diagnostic. It is true for the CFG merge-block meet (reproducing
    // the old branch-join diagnostic) and false for the synthetic meet at
    // the function exit used only to derive the end-of-method
    // undischarged-owned state (the old walker never met at exit, so
    // emitting there would introduce a new diagnostic — O3a forbids that).
    //
    // Slice O3b: `unionKeys` controls the domain. When false (the if/elif/
    // else merge), only locals present in `into` are met — a local declared
    // inside one branch is out of scope after the merge and is dropped.
    // When true (the loop back-edge and zero-trip skip merges), locals
    // present in only one operand are carried into the result, preserving a
    // §28.3 discharge obligation that exists on one incoming path but not
    // the other (the zero-trip leak case). The carried-in state is the
    // operand's state met against "absent" — Owned/Uninitialized survive so
    // the end-of-method / read checks still see the hazard.
    void joinInto(std::unordered_map<sema::Symbol*, LocalOwnership>& into,
                  const std::unordered_map<sema::Symbol*,
                                           LocalOwnership>& other,
                  SourceLocation joinLoc, bool emitClash = true,
                  bool unionKeys = false);

    // Slice O3a/O3b CFG driver: run the forward dataflow over `cfg`,
    // seeding the entry block with `entry`. Loops are iterated to a
    // fixpoint (O3b): the back-edge and the zero-trip header→after skip
    // edge participate in the meet, so the analysis is sound for the
    // never-executed loop path (§28). Returns the meet of all states
    // reaching the CFG exit (used as the end-of-method undischarged-owned
    // check). Diagnostics for reads/moves/disposals and branch-join
    // clashes are emitted on the single final (non-suppressed) pass.
    BlockState runDataflow(const flow::CFG& cfg, const BlockState& entry);

    // Process one CFG block's straight-line statements, mutating
    // `states_` (which the caller has set to the block's entry state).
    void processBlock(const flow::CFGBlock& block);

    sema::LocalSymbol* findLocalSymbol(const ast::LocalDecl& ld);

    LocalOwnership* lookupState(std::string_view name);

    void markDisposedByName(std::string_view name, SourceLocation loc);

    void markMovedByName(std::string_view name, SourceLocation loc);
};

} // namespace bezel::ownership
