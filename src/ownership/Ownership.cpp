#include "ownership/Ownership.hpp"
#include "ownership/Ownership_impl.hpp"

#include <algorithm>
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

// The MethodOwnership class is declared in Ownership_impl.hpp; its move /
// borrow / discharge check definitions live in Ownership_Checks.cpp. This
// file holds the statement/flow-walking definitions plus the public
// checkOwnership entry point and its supporting free functions.

namespace {
// §29.3 (#945): does this statement list use `yield` (making the method an
// iterator)? Recurses into control-flow block bodies (if/elif/else, while, for,
// do, try body — §29.6 forbids yield in catch/finally). Mirrors
// ExprTC::stmtsContainYield.
bool bodyContainsYield(const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    for (const auto& s : body) {
        if (!s) continue;
        if (dynamic_cast<const ast::YieldStmt*>(s.get())) return true;
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get()))
            if (bodyContainsYield(fs->body)) return true;
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get()))
            if (bodyContainsYield(ws->body)) return true;
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            if (bodyContainsYield(ifs->ifBranch.body)) return true;
            for (const auto& el : ifs->elifBranches)
                if (bodyContainsYield(el.body)) return true;
            if (ifs->hasElse && bodyContainsYield(ifs->elseBody)) return true;
        }
        if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get()))
            if (bodyContainsYield(db->body)) return true;
        if (auto* trys = dynamic_cast<const ast::TryStmt*>(s.get()))
            if (bodyContainsYield(trys->body)) return true;
    }
    return false;
}

// §28.3 v0.150 (#1008 Part B): forward declaration — collect every local that
// is reassigned or `out`-overwritten anywhere in the body (excluded from affine
// auto-drop). Definition lives below with the other body walkers.
void scanReassignStmts(const std::vector<std::unique_ptr<ast::Stmt>>& body,
                       sema::SemaContext& ctx,
                       std::unordered_set<const sema::Symbol*>& out);
}  // namespace

void MethodOwnership::run(const std::vector<std::unique_ptr<ast::Stmt>>& body,
         const std::vector<sema::Symbol*>* paramSyms, bool isAsync,
         bool isDestructor, const ast::Type* returnType, bool returnOwned) {
    isAsync_ = isAsync;
    isDestructor_ = isDestructor;
    returnType_ = returnType;
    returnOwned_ = returnOwned;
    // §29.3 / §29.8 (#945): classify iterator methods up-front so the end-of-
    // method check can apply the state-machine auto-dispose rule to owned params.
    sawYield_ = bodyContainsYield(body);
    // Issue #392 / §31.1: collect this method's catch-bound exception
    // variables before the dataflow runs. Each is a borrow of the in-flight
    // exception whose lifetime is its arm body; the escape machinery consults
    // `catchBorrows_` to reject the binding (or a re-borrow of it) escaping
    // the arm (store-to-owned-field, return-as-owned, escaping closure
    // capture). Done first so the per-statement escape checks see it.
    collectCatchBorrows(body);
    // §28.3 / §28.9 (#861): record each local's block-nesting depth so the
    // borrow-store escape check can tell whether a closure assigned to a plain
    // local outlives the owned local it captures.
    collectLocalDepths(body);
    // Seed the dataflow entry state with `owned` / `out owned` parameters
    // (slice O10): each enters the method in the Owned state and must
    // reach a discharge point on every normal-exit path (§28.8).
    FlowState entry;
    if (paramSyms) {
        for (sema::Symbol* sym : *paramSyms) {
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym);
            if (!ps) continue;
            if (ps->modifier == "owned" ||
                ps->modifier == "out owned") {
                LocalOwnership lo;
                lo.state = OwnershipState::Owned;
                lo.implicitDischarge = false;
                lo.obligationOrigin = ps->decl;
                entry.locals[sym] = lo;
            }
        }
    }
    // Slice O11-real: run the analysis as a forward dataflow over the
    // body's full CFG, including the real F2 try/catch/finally exception
    // edges (`treatTryAsOpaque=false`). The straight-line / if-else / loop /
    // try flow is all driven by the CFG. The returned state is the meet of
    // everything reaching the CFG exit; exception edges discharge their live
    // owned locals implicitly (§28.3 "Exception-path discharge"), so a value
    // that reaches exit only via a throw has been dropped from `endState`
    // and does not trip the undischarged check below.
    auto cfg = flow::buildCFG(body, /*treatTryAsOpaque=*/false);
    // Async-readiness 3c (§30.5 / §30.9): before the dataflow runs, classify
    // each owned local as suspend-local (live range crosses an await) and add
    // the synthetic completion edges (abandonment / cancellation exits) so a
    // suspend-local gets the §30.5 frame-disposal treatment on those edges.
    // Both are additive and gated on `isAsync_`; for a synchronous body the
    // CFG is identical and `annotateSuspendLocals` is a no-op (no await sites).
    if (isAsync_) {
        annotateSuspendLocals(*cfg);
        addCompletionEdges(*cfg);
    }
    FlowState endFlow = runDataflow(*cfg, entry);
    const BlockState& endState = endFlow.locals;
    // §28.3 "Owned-local discharge requirement": an owned local must reach
    // an owned slot (or be disposed) before it goes out of scope on a
    // *normal-exit* path. The exception-path discharge is handled inside the
    // dataflow (runDataflow / computeEntry): a value live-and-owned on an
    // exception edge is auto-disposed by the implicit try/finally during
    // stack unwinding, so the meet at any exception successor — and
    // ultimately at the function exit — sees it Disposed, not Owned. Hence
    // the check here fires only for locals that survive Owned to a
    // normal-exit edge.
    //
    // §28.3 v0.150 (#1008 Part B): the set of locals reassigned or out-
    // overwritten anywhere in the body — excluded from affine auto-drop below
    // (auto-drop disposes only the final slot value; an overwritten local would
    // leak its earlier value).
    std::unordered_set<const sema::Symbol*> reassignedLocals;
    scanReassignStmts(body, ctx_, reassignedLocals);
    for (const auto& [sym, st] : endState) {
        if (st.state == OwnershipState::Owned && !st.implicitDischarge) {
            // §29.8 (#945, #973b): an iterator method's `owned` parameter that the
            // body never moves out is disposed by the state machine at iterator-
            // Dispose. Suppress the §28.3 normal-exit drop reject and flag the
            // parameter for codegen disposal — emitIteratorMethod's Dispose for a
            // SYNC iterator (#945), or emitAsyncIteratorMethod's Dispose for an
            // ASYNC iterator (#973b: emitFreeAndReturn disposes it before freeing
            // the frame, on every Dispose path, null-guarded + nulled). A param
            // discharged on SOME path is in everDischarged_ → kept as a reject (the
            // state machine can't statically tell it was moved out, so auto-
            // disposing would risk a double-free; #973a covers that conditional
            // case via a runtime moved-from flag). Owned LOCALS in an iterator keep
            // the reject (their frame-disposal is a separate piece).
            if (sawYield_) {
                if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym)) {
                    if (ps->modifier == "owned" &&
                        everDischarged_.count(sym) == 0) {
                        ps->iteratorAutoDispose = true;
                        continue;
                    }
                }
            }
            // §29.2 / §28.3 v0.157 (#1329): affine auto-drop for owned ITERATION
            // VARIABLES. A `for var x in IOwningEnumerable<T>` / `await for var x in
            // IOwningAsyncEnumerable<T>` iteration variable whose ownership obligation
            // reaches a body-exit edge (i.e. is still Owned in endState via the
            // loop back-edge / skip-edge) is auto-dropped per-iteration at body-end
            // with the must-use warning, matching the §28.3 auto-drop for regular
            // owned locals.
            //
            // Safety: the iter var's slot is null-guarded (dynamicDispose codegen).
            // `x.Dispose()` already nulls the slot (explicitDisposeOwnedReceiverSlot),
            // and `move x` will be patched to null the slot when this flag is set.
            // Null-guarded auto-drop is therefore exactly-once-safe regardless of
            // whether the var was conditionally discharged (`everDischarged_` check is
            // NOT required here). Applies to BOTH sync and async bodies (not gated on
            // !isAsync_) since the iter var is NOT a suspend-local (annotateSuspendLocals
            // only marks LocalDecl* nodes, not ForStmt-backed iter vars) and therefore
            // is not disposed by the async completion-edge machinery.
            if (auto* ls = dynamic_cast<sema::LocalSymbol*>(sym)) {
                if (auto* fs =
                        dynamic_cast<const ast::ForStmt*>(ls->decl)) {
                    // Confirm it is an owning enumerable loop (not a borrow for-loop).
                    // iterableIsOwningEnumerable() is already used by seedOwnedIterVar.
                    if (iterableIsOwningEnumerable(*fs)) {
                        const_cast<ast::ForStmt*>(fs)->ownedIterVarAutoDropAtBodyEnd =
                            true;
                        ctx_.diagnostics().report(
                            Severity::Warning,
                            st.obligationOrigin ? st.obligationOrigin->span.begin
                                                : SourceLocation{},
                            std::string("owned value in `") +
                            std::string(sym->name) +
                            "` is implicitly disposed at the end of each loop "
                            "iteration (§29.2/§28.3 auto-drop); use `.Dispose()`, "
                            "`move`, `using var`, or `return` to discharge it "
                            "explicitly");
                        continue;
                    }
                }
            }
            // §28.3 v0.150 (#1008 Part B): affine auto-drop. A plain `var` owned
            // LOCAL that is never discharged on ANY path (everDischarged == 0)
            // is, on every path reaching this exit, a never-moved genuine owner:
            // its slot holds a live value and is never transferred out. Disposing
            // it implicitly at scope end is therefore exactly-once-safe. Flag the
            // LocalDecl so codegen routes it through registerUsingDisposal (the
            // same scope-end disposal a `using var` gets) and emit a *warning*,
            // not an error — the must-use lint. A local discharged on SOME path
            // (everDischarged > 0 — a conditional move/dispose) is NOT auto-
            // dropped: plain `var`s are not nulled on move (to keep borrow-after-
            // move legal, e.g. `new T(move n, n)`), so the join retains Owned and
            // auto-disposing would double-free the moved/disposed path. That case
            // keeps the equalize-the-branches error below (§5.6 lattice rule).
            // An `owned` PARAMETER dropped on normal exit auto-drops too (the
            // ParameterSymbol branch below, §28.3 v0.152). // per §28.3, §5.6
            //
            // GATED to synchronous, non-iterator bodies. In an `async` method an
            // owned local that crosses an await is a suspend-local disposed by
            // the state machine on the §30.5 completion edges; in an iterator
            // (`sawYield_`) an owned local is captured into the iterator frame
            // and disposed at iterator-Dispose. Routing either through the
            // scope-end using-disposal would double-dispose. Those bodies keep
            // the discharge error (auto-drop for them is a separate piece). The
            // synchronous-body auto-drop below uses the same everDischarged == 0
            // never-moved invariant as the sync-iterator-param case above.
            if (!isAsync_ && !sawYield_ && !isDestructor_ &&
                everDischarged_.count(sym) == 0 &&
                reassignedLocals.count(sym) == 0) {
                if (auto* ls = dynamic_cast<sema::LocalSymbol*>(sym)) {
                    if (auto* ld =
                            dynamic_cast<const ast::LocalDecl*>(ls->decl)) {
                        const_cast<ast::LocalDecl*>(ld)->autoDropAtScopeEnd = true;
                        ctx_.diagnostics().report(
                            Severity::Warning,
                            st.obligationOrigin ? st.obligationOrigin->span.begin
                                                : SourceLocation{},
                            std::string("owned value in `") +
                            std::string(sym->name) +
                            "` is implicitly disposed at the end of its scope "
                            "(§28.3 auto-drop); declare it `using var` to make "
                            "the scope-bound disposal explicit, or `move` / "
                            "`return` / `.Dispose()` / `_ := …` to discharge it");
                        continue;
                    }
                    // §28.3 / #1387: `out owned var x` inline local — ls->decl is
                    // the IdentifierExpr (not a LocalDecl). Apply affine auto-drop
                    // so an undischarged inline local gets a warning and scope-end
                    // disposal rather than a hard error.
                    // EXCLUDED: while-loop condition items (loopCondOutOwnedSyms_)
                    // share their slot across iterations — auto-drop only covers the
                    // FINAL iteration's value, so earlier values would leak; those
                    // keep the hard §28.3 discharge error.  // per §28.3, #1387
                    if (dynamic_cast<const ast::IdentifierExpr*>(ls->decl) &&
                        loopCondOutOwnedSyms_.count(sym) == 0) {
                        ls->autoDropAtScopeEnd = true;
                        ctx_.diagnostics().report(
                            Severity::Warning,
                            st.obligationOrigin ? st.obligationOrigin->span.begin
                                                : SourceLocation{},
                            std::string("owned value in `") +
                            std::string(sym->name) +
                            "` is implicitly disposed at the end of its scope "
                            "(§28.3 auto-drop); use `item?.Dispose()`, "
                            "`_ := item`, `move`, or `return` to discharge it "
                            "explicitly (§28.3/§28.11)");
                        continue;
                    }
                } else if (auto* ps =
                               dynamic_cast<sema::ParameterSymbol*>(sym)) {
                    // §28.3 v0.152 (#1008 Part B, extended to parameters): an
                    // `owned` parameter the method neither moves/returns/disposes
                    // nor reassigns is a never-moved genuine owner — auto-dropped
                    // at method end (the same implicit disposal the exception edge
                    // already performs on owned parameters), with the must-use
                    // warning. (A SYNC iterator's owned param was handled above
                    // via iteratorAutoDispose; this is the ordinary synchronous
                    // non-iterator method.) Only an `owned` parameter carries a
                    // disposal obligation; a borrow parameter is never Owned here.
                    if (ps->modifier == "owned") {
                        ps->autoDropAtScopeEnd = true;
                        ctx_.diagnostics().report(
                            Severity::Warning,
                            st.obligationOrigin ? st.obligationOrigin->span.begin
                                                : SourceLocation{},
                            std::string("owned parameter `") +
                            std::string(sym->name) +
                            "` is implicitly disposed at the end of the method "
                            "(§28.3 auto-drop); `move` / `return` / `.Dispose()` "
                            "it, or rebind it with `using var`, to discharge it "
                            "explicitly");
                        continue;
                    }
                }
            }
            ctx_.diagnostics().report(
                Severity::Error,
                st.obligationOrigin ? st.obligationOrigin->span.begin
                                    : SourceLocation{},
                std::string("owned value in `") +
                std::string(sym->name) +
                "` would be dropped here without disposal "
                "(§28.3 — consider `using var`, explicit "
                "`.Dispose()`, `move`, or `return`)");
        }
    }
    // §29.2 / §28.3 v0.157 (#1329) addendum — break-only loop bodies.
    // A `for var x in IOwningEnumerable<T>` body that always exits via `break`
    // never propagates item=Owned to the loop's `after` block: the break edge is
    // not a loopEdge (it is not a back-edge or the header's zero-trip skip-edge),
    // so `unionKeys=false` at the `after` block's entry merge prevents new symbols
    // from the break edge from entering the state. `endState` therefore never sees
    // item=Owned, and the `endState` loop above never fires. Detect this case by
    // checking every seeded iter var: seeded-but-not-in-endState AND never
    // discharged (everDischarged_.count == 0) means the value leaked at break.
    // Restricted to `seededIterVarSyms_` (symbols seeded by THIS method's
    // seedOwnedIterVar) so we never mis-flag iter vars from a different method.
    for (sema::Symbol* sym : seededIterVarSyms_) {
        auto* ls = dynamic_cast<sema::LocalSymbol*>(sym);
        if (!ls) continue;
        auto* fs = dynamic_cast<const ast::ForStmt*>(ls->decl);
        if (!fs) continue;
        if (fs->ownedIterVarAutoDropAtBodyEnd) continue; // already set above
        if (everDischarged_.count(sym) != 0) continue;  // discharged on some path
        const_cast<ast::ForStmt*>(fs)->ownedIterVarAutoDropAtBodyEnd = true;
        ctx_.diagnostics().report(
            Severity::Warning, fs->span.begin,
            std::string("owned value in `") + std::string(ls->name) +
            "` is implicitly disposed at the end of each loop "
            "iteration (§29.2/§28.3 auto-drop); use `.Dispose()`, "
            "`move`, `using var`, or `return` to discharge it "
            "explicitly");
    }
    // §28.3 (#1257): a plain-var owned local declared in a NESTED scope that exits
    // normally before the function reaches a throw-only exit never appears in
    // `endState` (its inner scope ended, so it is not live at the function exit;
    // and when the sole exit is a throw the exit state may be empty). The loop
    // above therefore never flags it, and codegen's fallback unwind-only disposal
    // cannot fire (the inner scope was popped before the outer throw unwinds) → the
    // value leaks. Flag every plain owner that was NEVER discharged on any path
    // (everDischarged == 0, !reassigned) and not already flagged above: it is a
    // never-moved genuine owner, so treating it like a `using var` is exactly-once-
    // safe — registerUsingDisposal places the disposal at the local's OWN lexical
    // scope, firing at the inner-block scope end on the normal path (before the
    // throw) and via that block's unwind landing pad otherwise. Gated to
    // synchronous, non-iterator, non-destructor bodies exactly like the function-
    // exit auto-drop above (async suspend-locals / iterator-frame locals dispose
    // via their own machinery; routing them here would double-dispose).
    if (!isAsync_ && !sawYield_ && !isDestructor_) {
        for (sema::Symbol* sym : everOwnedPlainLocals_) {
            if (everDischarged_.count(sym) != 0) continue;
            if (reassignedLocals.count(sym) != 0) continue;
            auto* ls = dynamic_cast<sema::LocalSymbol*>(sym);
            if (!ls) continue;
            auto* ld = dynamic_cast<const ast::LocalDecl*>(ls->decl);
            if (ld) {
                // Skip a local already flagged by the endState loop (it reaches the
                // function exit as Owned) — flagging is idempotent but its warning
                // would duplicate.
                if (ld->autoDropAtScopeEnd) continue;
                const_cast<ast::LocalDecl*>(ld)->autoDropAtScopeEnd = true;
                ctx_.diagnostics().report(
                    Severity::Warning, ld->span.begin,
                    std::string("owned value in `") + std::string(sym->name) +
                    "` is implicitly disposed at the end of its scope "
                    "(§28.3 auto-drop); declare it `using var` to make the scope-bound "
                    "disposal explicit, or `move` / `return` / `.Dispose()` / `_ := …` "
                    "to discharge it");
                continue;
            }
            // §28.3 / #1387: `out owned var x` inline local in a NESTED scope —
            // ls->decl is IdentifierExpr (not LocalDecl). Skip if already flagged by
            // the endState loop above (warning would duplicate). Excluded when the
            // declaring call is in a while-loop condition (loopCondOutOwnedSyms_);
            // those keep the hard §28.3 discharge error (slot is reused each
            // iteration, auto-drop only covers the FINAL value).  // per §28.3, #1387
            if (dynamic_cast<const ast::IdentifierExpr*>(ls->decl) &&
                loopCondOutOwnedSyms_.count(sym) == 0) {
                if (ls->autoDropAtScopeEnd) continue;
                ls->autoDropAtScopeEnd = true;
                ctx_.diagnostics().report(
                    Severity::Warning,
                    ls->decl ? ls->decl->span.begin : SourceLocation{},
                    std::string("owned value in `") + std::string(sym->name) +
                    "` is implicitly disposed at the end of its scope "
                    "(§28.3 auto-drop); use `item?.Dispose()`, "
                    "`_ := item`, `move`, or `return` to discharge it "
                    "explicitly (§28.3/§28.11)");
            }
        }
    }
    // Slice O8 / §28.8.1: a `move this.field` leaves the field in
    // moved-from state. The field must be reassigned (restored) before the
    // surrounding method exits — otherwise the field's invariant is broken on
    // method return. Per §28.8.1 the field must be restored on EVERY path from
    // the move to method exit; "Exception paths are not exempt", so this is NOT
    // covered by the §28.3 exception-path discharge: even when the method exits
    // via a throw, the destructor on `this` would observe the field moved-from
    // during unwinding (the field facts deliberately pass through discharge
    // edges UNTOUCHED — unwinding disposes live locals, it does not restore a
    // moved-from field).
    // §28.8.1 (#867): the field facts are PATH-SENSITIVE — `endFlow.fields` is
    // the OR-over-every-path-reaching-exit merge of the per-block moved-from
    // facts (see runDataflow / joinFieldsInto), so a field restored on only
    // SOME paths is reported here exactly because the unrestored path leaves it
    // moved-from at the merged exit state.
    // §28.8.1 (#763): the field-restoration rule does NOT apply inside a
    // destructor — "the object is being torn down and no future read of the
    // field will occur," so a `move this.f` whose taken value is otherwise
    // discharged (checked by the ordinary owned-local discharge above) leaves
    // the field validly moved-from at destructor exit. Skip the check there.
    if (!isDestructor_) {
        for (const auto& [name, moved] : endFlow.fields) {
            if (moved) {
                ctx_.diagnostics().report(
                    Severity::Error,
                    fieldMoveLoc_.count(name)
                        ? fieldMoveLoc_[name]
                        : SourceLocation{},
                    std::string("field `") + std::string(name) +
                    "` left in moved-from state at method exit "
                    "(§28.8.1 — reassign the field before exiting)");
            }
        }
    }
    // Async-readiness 3c (§30.5 Consumer-side; Appendix C.6): the at-await-site
    // Pattern A / Pattern B enforcement. A single syntactic walk over the body
    // examines each await once (no fixpoint, so no diagnostic duplication) and
    // reports a Pattern A await whose operand is not an admitted owning
    // location, and a Pattern B await not in an owned-consuming context. Gated
    // on `isAsync_` — a synchronous body never reaches it. The dataflow above
    // already enforces the across-suspend NORMAL-path discharge (suspend-locals
    // are ordinary owned locals in the lattice) and the Pattern B operand-
    // consume double-await (recordAwaitOperandConsume runs inside the transfer
    // functions). // per §30.5
    if (isAsync_) {
        checkAwaitSites(body);
    }
}

void MethodOwnership::runExprBodyOwningReturnCheck(
        const ast::Expr& exprBody,
        const std::vector<sema::Symbol*>* paramSyms,
        const ast::Type* returnType, bool returnOwned) {
    returnType_ = returnType;
    returnOwned_ = returnOwned;
    // Seed `owned` / `out owned` parameters as Owned locals (as run() does) so
    // returning an owned param is classified as an owned transfer, not a borrow.
    // Non-owned params stay out of `states_`; the ParameterSymbol branch in
    // ownedArgSourceIsBorrow classifies them as borrows by modifier. // per §28.8
    if (paramSyms) {
        for (sema::Symbol* sym : *paramSyms) {
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym);
            if (!ps) continue;
            if (ps->modifier == "owned" || ps->modifier == "out owned") {
                LocalOwnership lo;
                lo.state = OwnershipState::Owned;
                lo.obligationOrigin = ps->decl;
                states_[sym] = lo;
            }
        }
    }
    checkBorrowReturnedThroughOwningType(exprBody);
    // §28.6 (#1005): the `=>` expression-body sibling of the handleReturn call —
    // rejects `Take(): Cell? => move v;` (the bare-read `=> v;` borrow is
    // legal). // per §28.6
    checkOwnedFieldReturn(exprBody);
}

namespace {

// An edge identified by its endpoints, used to flag CFG back-edges so the
// O3b loop fixpoint can distinguish loop-carried merges (back-edge / zero-
// trip skip) from straight-line / if-else forward merges.
struct Edge {
    const flow::CFGBlock* from;
    const flow::CFGBlock* to;
    bool operator==(const Edge& o) const {
        return from == o.from && to == o.to;
    }
};
struct EdgeHash {
    std::size_t operator()(const Edge& e) const {
        return std::hash<const void*>()(e.from) * 31u +
               std::hash<const void*>()(e.to);
    }
};

// Classify back-edges via a DFS gray/black colouring: an edge to a node
// still on the recursion stack (gray) is a back-edge. Reducible (i.e.
// structured) CFGs have exactly the loop back-edges so classified.
void collectBackEdges(const flow::CFGBlock* b,
                      std::unordered_set<const flow::CFGBlock*>& gray,
                      std::unordered_set<const flow::CFGBlock*>& black,
                      std::unordered_set<Edge, EdgeHash>& backEdges) {
    gray.insert(b);
    for (const flow::CFGBlock* s : b->succs) {
        if (gray.count(s)) {
            backEdges.insert({b, s});
        } else if (!black.count(s)) {
            collectBackEdges(s, gray, black, backEdges);
        }
    }
    gray.erase(b);
    black.insert(b);
}

// True if the edge from `pred` to `block` is a loop header's skip / exit
// edge (header → succs[1], i.e. the zero-trip path into the loop's
// after-block).
bool isLoopSkipEdge(const flow::CFGBlock* pred, const flow::CFGBlock* block) {
    return pred->isLoopHeader && pred->succs.size() >= 2 &&
           pred->succs[1] == block;
}

// Reverse-postorder over the CFG following every forward (non-back) edge.
// O3b uses this as the visitation order for the fixpoint iterations and
// the final pass; back-edges are still excluded from the DFS so the order
// is a true RPO (every block's forward-predecessors precede it), but the
// fixpoint re-runs the whole order until exit states stabilize, so the
// loop-carried state still reaches every block.
void reversePostorder(const flow::CFGBlock* b,
                      const std::unordered_set<Edge, EdgeHash>& backEdges,
                      std::unordered_set<const flow::CFGBlock*>& seen,
                      std::vector<const flow::CFGBlock*>& post) {
    seen.insert(b);
    for (const flow::CFGBlock* s : b->succs) {
        if (backEdges.count({b, s})) continue;
        if (!seen.count(s)) {
            reversePostorder(s, backEdges, seen, post);
        }
    }
    post.push_back(b);
}

// Compare two block states for fixpoint convergence: same key set, and
// the same ownership state + implicit-discharge flag per key. The
// auxiliary fields (obligation origin, borrow source) do not affect any
// later transfer-function decision, so they are excluded — comparing them
// would make pointer churn look like non-convergence.
bool sameState(const BlockState& a, const BlockState& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [sym, la] : a) {
        auto it = b.find(sym);
        if (it == b.end()) return false;
        if (la.state != it->second.state) return false;
        if (la.implicitDischarge != it->second.implicitDischarge) return false;
    }
    return true;
}

// §28.8.1 (#867): fixpoint convergence over the field-moved-from facts. Two
// maps agree when every field present-and-true in one is present-and-true in
// the other (an absent or false entry both mean "restored / never moved").
// The loop fixpoint must consider these alongside the locals so a field fact
// that needs another pass to propagate around a back-edge is not declared
// stable prematurely. // per §28.8.1, #867
bool sameFields(const FieldMovedFrom& a, const FieldMovedFrom& b) {
    auto subsetTrue = [](const FieldMovedFrom& x, const FieldMovedFrom& y) {
        for (const auto& [name, moved] : x) {
            if (!moved) continue;
            auto it = y.find(name);
            if (it == y.end() || !it->second) return false;
        }
        return true;
    };
    return subsetTrue(a, b) && subsetTrue(b, a);
}

// §28.8.1 (#867): same FlowState for the loop fixpoint — both the locals
// lattice and the field-moved-from facts must be stable.
bool sameFlow(const FlowState& a, const FlowState& b) {
    return sameState(a.locals, b.locals) && sameFields(a.fields, b.fields);
}

// Collect every LocalDecl AST node declared (directly or in a nested
// block) within a statement list, into `out`. Used to scope §28.3
// exception-path discharge to exactly the locals whose scope the unwind
// destroys (a try body's own locals), leaving enclosing-scope locals
// untouched — they remain live in the catch and after the try.
//
// §22.4 / v0.160 (#1344, #1305): an `out owned var x` argument inline-
// declares a fresh local whose ownership symbol carries `decl = id` (the
// IdentifierExpr argument node) rather than a LocalDecl. Collect those
// IdentifierExpr nodes too — they must appear in `unwoundDecls` so that
// dischargeOnExceptionEdge correctly recognises them as try-body-scoped
// locals and discharges them on the exception edge (otherwise they survive
// as Owned to the function exit and trigger a spurious §28.3 error).
void collectLocalDecls(const std::vector<std::unique_ptr<ast::Stmt>>& body,
                       std::unordered_set<const ast::Node*>& out);

// §28.3 / #1387: scan `e` for `out owned var` call arguments and insert each
// argument's resolved LocalSymbol into `out`. Used to populate
// loopCondOutOwnedSyms_ for while-loop condition calls. Requires a SemaContext
// to resolve the IdentifierExpr argument to its symbol.
void collectOutOwnedVarSyms(const ast::Expr& e,
                             sema::SemaContext& ctx,
                             std::unordered_set<sema::Symbol*>& out) {
    auto scan = [&](const std::vector<ast::CallArgument>& args) {
        for (const auto& a : args) {
            if (a.modifier == "out owned var" && a.value) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        a.value.get()))
                    if (sema::Symbol* s = ctx.exprSymbol(*id))
                        out.insert(s);
            }
        }
    };
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e))
        scan(ce->args);
    else if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e))
        scan(nc->args);
}

// Scan `e` for any CallExpr / NullCondCallExpr that has an `out owned var`
// argument and insert the argument's IdentifierExpr node into `out`.
void collectOutOwnedVarArgs(const ast::Expr* e,
                            std::unordered_set<const ast::Node*>& out) {
    if (!e) return;
    auto scan = [&](const std::vector<ast::CallArgument>& args) {
        for (const auto& a : args) {
            if (a.modifier == "out owned var") {
                if (auto* id =
                        dynamic_cast<const ast::IdentifierExpr*>(a.value.get()))
                    out.insert(id);
            }
        }
    };
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(e))
        scan(ce->args);
    else if (auto* ncc = dynamic_cast<const ast::NullCondCallExpr*>(e))
        scan(ncc->args);
}

void collectLocalDeclsStmt(const ast::Stmt& s,
                           std::unordered_set<const ast::Node*>& out) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        out.insert(ld);
        // §22.4 / v0.160: also collect `out owned var` inline locals from
        // the initializer, e.g. `var ok := s.TryPop(out owned var item);`.
        collectOutOwnedVarArgs(ld->initializer.get(), out);
        return;
    }
    // Bare call statement: `s.TryPop(out owned var item);`
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        collectOutOwnedVarArgs(es->expr.get(), out);
        return;
    }
    // Assignment RHS: `ok := s.TryPop(out owned var item);`
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        collectOutOwnedVarArgs(as->value.get(), out);
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        // §22.4: the `if` / `elif` condition may itself be the call that
        // declares the inline local, e.g. `if s.TryPop(out owned var item)`.
        collectOutOwnedVarArgs(ifs->ifBranch.condition.get(), out);
        collectLocalDecls(ifs->ifBranch.body, out);
        for (const auto& el : ifs->elifBranches) {
            collectOutOwnedVarArgs(el.condition.get(), out);
            collectLocalDecls(el.body, out);
        }
        collectLocalDecls(ifs->elseBody, out);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        // §22.4: while-condition may declare the inline local.
        collectOutOwnedVarArgs(ws->condition.get(), out);
        collectLocalDecls(ws->body, out);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        collectLocalDecls(fs->body, out);
        return;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        collectLocalDecls(dob->body, out);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        // A nested try's body / catch / finally locals are also destroyed
        // when an exception escapes the *outer* try through them.
        collectLocalDecls(ts->body, out);
        for (const auto& arm : ts->catches) {
            if (arm.body) collectLocalDeclsStmt(*arm.body, out);
        }
        collectLocalDecls(ts->finallyBody, out);
        return;
    }
}

void collectLocalDecls(const std::vector<std::unique_ptr<ast::Stmt>>& body,
                       std::unordered_set<const ast::Node*>& out) {
    for (const auto& s : body) {
        if (s) collectLocalDeclsStmt(*s, out);
    }
}

// §28.3 v0.150 (#1008 Part B, affine auto-drop): collect every LocalSymbol that
// can receive an owned value MORE THAN ONCE over its lifetime — an assignment
// target (`x := …`) or an `out`-argument (`f(out x)` / `out owned`). Affine
// auto-drop disposes only a local's FINAL slot value at scope end; a local that
// is reassigned or out-overwritten while owned would leak its earlier value(s),
// which auto-drop cannot see. Such locals are excluded from auto-drop and keep
// the conservative §28.3 discharge error. A single-assignment owning local
// (`var x := new T();`, never written again) is not collected and is eligible.
void scanReassignExpr(const ast::Expr* e, sema::SemaContext& ctx,
                      std::unordered_set<const sema::Symbol*>& out);

void scanReassignArgs(const std::vector<ast::CallArgument>& args,
                      sema::SemaContext& ctx,
                      std::unordered_set<const sema::Symbol*>& out) {
    for (const auto& a : args) {
        // §28.3 / #1387: `out owned var`, `out using var`, `out var` inline
        // declarations are FRESH locals written AT MOST ONCE per call-site
        // evaluation — not reassignments of an existing local. Excluding them
        // from reassignedLocals enables the §28.3 affine auto-drop for these
        // locals when they are never discharged. (While-loop conditions
        // overwrite the slot on each iteration; those are blocked separately
        // via loopCondOutOwnedSyms_ in the dataflow, not here.)
        // Pre-existing `out` / `out owned` params that refer to a local
        // already declared BEFORE the call ARE reassignments → keep them.
        bool isInlineFresh = (a.modifier == "out owned var" ||
                              a.modifier == "out using var" ||
                              a.modifier == "out var");
        if (!isInlineFresh && a.value &&
            a.modifier.find("out") != std::string_view::npos) {
            if (auto* id =
                    dynamic_cast<const ast::IdentifierExpr*>(a.value.get()))
                if (sema::Symbol* s = ctx.exprSymbol(*id)) out.insert(s);
        }
        if (a.value) scanReassignExpr(a.value.get(), ctx, out);
    }
}

void scanReassignExpr(const ast::Expr* e, sema::SemaContext& ctx,
                      std::unordered_set<const sema::Symbol*>& out) {
    if (!e) return;
    if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
        scanReassignExpr(c->callee.get(), ctx, out);
        scanReassignArgs(c->args, ctx, out);
    } else if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
        scanReassignExpr(nc->callee.get(), ctx, out);
        scanReassignArgs(nc->args, ctx, out);
    } else if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
        scanReassignArgs(n->args, ctx, out);
    } else if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
        scanReassignExpr(m->target.get(), ctx, out);
    } else if (auto* ncm = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
        scanReassignExpr(ncm->target.get(), ctx, out);
    } else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        scanReassignExpr(ix->target.get(), ctx, out);
        scanReassignArgs(ix->indices, ctx, out);
    } else if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
        scanReassignExpr(b->left.get(), ctx, out);
        scanReassignExpr(b->right.get(), ctx, out);
    } else if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
        scanReassignExpr(u->operand.get(), ctx, out);
    } else if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
        scanReassignExpr(p->operand.get(), ctx, out);
    } else if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
        scanReassignExpr(t->operand.get(), ctx, out);
    } else if (auto* co = dynamic_cast<const ast::ConditionalExpr*>(e)) {
        scanReassignExpr(co->thenValue.get(), ctx, out);
        scanReassignExpr(co->condition.get(), ctx, out);
        scanReassignExpr(co->elseValue.get(), ctx, out);
    } else if (auto* th = dynamic_cast<const ast::ThrowExpr*>(e)) {
        scanReassignExpr(th->operand.get(), ctx, out);
    } else if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
        scanReassignExpr(r->lo.get(), ctx, out);
        scanReassignExpr(r->hi.get(), ctx, out);
        scanReassignExpr(r->step.get(), ctx, out);
    } else if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
        for (const auto& el : cl->elements) scanReassignExpr(el.get(), ctx, out);
        for (const auto& k : cl->keys) scanReassignExpr(k.get(), ctx, out);
    }
    // InterpolatedString / WithExpr / LambdaExpr: an `out`-arg essentially never
    // appears there, and a lambda body carries its own ownership scope.
}

void scanReassignStmts(const std::vector<std::unique_ptr<ast::Stmt>>& body,
                       sema::SemaContext& ctx,
                       std::unordered_set<const sema::Symbol*>& out);

void scanReassignStmt(const ast::Stmt& s, sema::SemaContext& ctx,
                      std::unordered_set<const sema::Symbol*>& out) {
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (auto* id =
                dynamic_cast<const ast::IdentifierExpr*>(as->target.get()))
            if (sema::Symbol* sym = ctx.exprSymbol(*id)) out.insert(sym);
        scanReassignExpr(as->target.get(), ctx, out);
        scanReassignExpr(as->value.get(), ctx, out);
    } else if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        // §17.6 / §28.3 (#913): `c++` / `c--` desugars to `c := T.operator++(c)`
        // — a REASSIGNMENT of the target. For an OWNED class local this drops the
        // previous owned value, so (exactly like a direct `c := …`) the local must
        // be excluded from the §28.3 affine auto-drop and explicitly discharged.
        // Marking the identifier target reassigned here routes `var c := new T();
        // c++` (no discharge) to the §28.3 drop-reject instead of silently leaking
        // the old value; a value-type local (int/record — not owned) carries no
        // obligation, so it is never rejected. (The `using var` form is already
        // rejected as non-reassignable, §28.10.)
        if (auto* id =
                dynamic_cast<const ast::IdentifierExpr*>(is->target.get()))
            if (sema::Symbol* sym = ctx.exprSymbol(*id)) out.insert(sym);
        scanReassignExpr(is->target.get(), ctx, out);
    } else if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        scanReassignExpr(ld->initializer.get(), ctx, out);
    } else if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        scanReassignExpr(es->expr.get(), ctx, out);
    } else if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        scanReassignExpr(rs->value.get(), ctx, out);
    } else if (auto* ths = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        scanReassignExpr(ths->value.get(), ctx, out);
    } else if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        scanReassignExpr(ds->value.get(), ctx, out);
    } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        scanReassignExpr(ifs->ifBranch.condition.get(), ctx, out);
        scanReassignStmts(ifs->ifBranch.body, ctx, out);
        for (const auto& el : ifs->elifBranches) {
            scanReassignExpr(el.condition.get(), ctx, out);
            scanReassignStmts(el.body, ctx, out);
        }
        scanReassignStmts(ifs->elseBody, ctx, out);
    } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        scanReassignExpr(ws->condition.get(), ctx, out);
        scanReassignStmts(ws->body, ctx, out);
    } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        scanReassignExpr(fs->iterable.get(), ctx, out);
        scanReassignExpr(fs->withExpr.get(), ctx, out);
        scanReassignStmts(fs->body, ctx, out);
    } else if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        scanReassignStmts(dob->body, ctx, out);
    } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        scanReassignStmts(ts->body, ctx, out);
        for (const auto& arm : ts->catches)
            if (arm.body) scanReassignStmt(*arm.body, ctx, out);
        scanReassignStmts(ts->finallyBody, ctx, out);
    }
}

void scanReassignStmts(const std::vector<std::unique_ptr<ast::Stmt>>& body,
                       sema::SemaContext& ctx,
                       std::unordered_set<const sema::Symbol*>& out) {
    for (const auto& s : body) if (s) scanReassignStmt(*s, ctx, out);
}

// §28.3 "Exception-path discharge": when control transfers along an
// exception (stack-unwinding) edge, every owned local or owned parameter
// whose scope the unwind *destroys* is disposed implicitly by the
// compiler's implicit try/finally. Returns a copy of `pe` with each such
// Owned local *removed* — modelling that the unwind both disposed it (so it
// carries no leak obligation past this edge) and ended its scope (so it
// does not exist at the handler or after the try). Removing rather than
// marking Disposed is deliberate: the value's scope is gone, and a leftover
// Disposed entry would otherwise meet the *normal*-exit path's Owned entry
// for the same local at the post-try join and mask a real normal-path leak
// (§28.6). Dropping the entry lets the normal path's Owned obligation
// survive the join intact (callers fold normal predecessors first so the
// surviving entry is seeded from a normal edge).
//
// `unwoundDecls` is non-null when the edge lands at a try's catch dispatch:
// only locals whose declaration is in that set (declared inside the try
// body the exception is escaping) go out of scope; locals from enclosing
// scopes are still live in the catch and after the try, so they are left
// untouched. When `unwoundDecls` is null the edge leaves the method (an
// uncaught exception), so the whole method unwinds and *all* in-scope owned
// locals are disposed.
//
// The transform applies to both plain `var` and `using var` owned locals:
// "On exception paths, both behave identically — the value is disposed by
// the implicit cleanup pass." It deliberately leaves the states the spec
// lists as NOT auto-disposed untouched: MovedFrom (already moved out),
// Disposed (already disposed), Borrowed (no obligation), Uninitialized /
// PolyNull / Poisoned (no live owned value).
BlockState dischargeOnExceptionEdge(
    const BlockState& pe,
    const std::unordered_set<const ast::Node*>* unwoundDecls) {
    BlockState out = pe;
    for (auto it = out.begin(); it != out.end();) {
        sema::Symbol* sym = it->first;
        const LocalOwnership& st = it->second;
        bool unwind = st.state == OwnershipState::Owned;
        if (unwind && unwoundDecls) {
            // Caught by an enclosing try: only this try body's own locals
            // unwind. A symbol's `decl` is its declaration AST node (the
            // LocalDecl for a local, the parameter for an owned parameter);
            // it is stable across reassignment, unlike the obligation
            // origin. An owned parameter is declared in the method scope,
            // which encloses the try, so its decl is not in `unwoundDecls`
            // and it is NOT disposed here — it stays Owned and must still be
            // discharged after the try.
            if (sym->decl == nullptr ||
                unwoundDecls->count(sym->decl) == 0) {
                unwind = false;
            }
        }
        if (unwind) {
            it = out.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

} // namespace

void MethodOwnership::joinFieldsInto(FieldMovedFrom& into,
                                    const FieldMovedFrom& other) {
    // §28.8.1 (#867): OR-over-predecessors. A field moved-from on the `other`
    // path becomes moved-from at the join (restored only when restored on ALL
    // incoming paths). A field absent from one operand was never moved on that
    // path (i.e. restored), so it does not force the result true. We therefore
    // only need to propagate `other`'s TRUE entries into `into`: any field
    // already true in `into` stays true; a field true in `other` becomes true
    // in `into`; everything else (false/absent in both) stays restored.
    for (const auto& [name, moved] : other) {
        if (moved) into[name] = true;
    }
}

FlowState MethodOwnership::runDataflow(
    const flow::CFG& cfg, const FlowState& entry) {
    // Identify back-edges, then a reverse-postorder over the forward edges.
    std::unordered_set<Edge, EdgeHash> backEdges;
    {
        std::unordered_set<const flow::CFGBlock*> gray, black;
        collectBackEdges(cfg.entry(), gray, black, backEdges);
    }
    std::vector<const flow::CFGBlock*> post;
    {
        std::unordered_set<const flow::CFGBlock*> seen;
        reversePostorder(cfg.entry(), backEdges, seen, post);
        // Safety net: a block unreachable from entry via forward edges
        // (e.g. a loop after-block whose body always returns/breaks, so it
        // is reached only across the back-edge-excluded skip) is appended
        // in allocation order so the fixpoint still processes it.
        for (const auto& bp : cfg.blocks()) {
            if (!seen.count(bp.get())) {
                seen.insert(bp.get());
                post.push_back(bp.get());
            }
        }
    }
    std::reverse(post.begin(), post.end()); // postorder → reverse-postorder

    std::unordered_map<const flow::CFGBlock*, FlowState> exitState;
    std::unordered_set<const flow::CFGBlock*> evaluated;

    // §28.3 exception-path discharge scoping. For an exception edge that
    // lands at a try's catch dispatch, only the locals declared in that try
    // body unwind; for an edge that leaves the method (lands at the exit),
    // the whole method unwinds. Cache, per try AST node, the set of
    // LocalDecls declared in its body. The resolver returns that set for a
    // catch-dispatch target, or nullptr ("unwind everything") for the exit.
    std::unordered_map<const ast::TryStmt*,
                       std::unordered_set<const ast::Node*>> tryUnwound;
    auto unwoundDeclsFor =
        [&](const flow::CFGBlock* target)
        -> const std::unordered_set<const ast::Node*>* {
        const ast::TryStmt* ts = target->tryForDispatch;
        if (!ts) return nullptr;  // exit / non-dispatch → whole-method unwind
        auto it = tryUnwound.find(ts);
        if (it == tryUnwound.end()) {
            std::unordered_set<const ast::Node*> decls;
            collectLocalDecls(ts->body, decls);
            it = tryUnwound.emplace(ts, std::move(decls)).first;
        }
        return &it->second;
    };

    // Compute one block's entry state as the §28.3 meet over its evaluated
    // predecessors' exit states. O3b: every predecessor participates,
    // INCLUDING back-edges and the zero-trip header→after skip edge —
    // that is what makes the analysis sound for the never-executed loop
    // path. Loop-carried merges (back-edge / skip) use union-key meet so a
    // loop-body local that reaches the after-block keeps its discharge
    // obligation, and never emit the §28.3 branch-join clash (only a real
    // if/elif/else Merge block does, and only on the final pass via
    // `emitOnMerge`).
    auto computeEntry = [&](const flow::CFGBlock* b,
                            bool emitOnMerge) -> FlowState {
        BlockState in;
        // §28.8.1 (#867): the field-moved-from facts at this block's entry,
        // merged OR-over-predecessors (a field moved-from on ANY incoming path
        // → moved-from at entry). Unlike the locals, field facts get NO
        // discharge / null-guard transform: unwinding disposes live locals but
        // does NOT restore a moved-from field (§28.8.1 "Exception paths are not
        // exempt"), so they pass through every edge — normal, exception,
        // completion, loop — unchanged and merge purely by OR. The merge is
        // order-independent, so every evaluated predecessor is folded in
        // uniformly below (no normal-first / discharge ordering needed).
        FieldMovedFrom inFields;
        bool haveIn = false;
        if (b == cfg.entry()) {
            in = entry.locals;
            inFields = entry.fields;
            haveIn = true;
        }
        SourceLocation joinLoc =
            b->sourceStmt ? b->sourceStmt->span.begin : SourceLocation{};
        const bool isIfMerge = b->kind == flow::CFGBlock::Kind::Merge;
        // §28.3 exception-path discharge: along an exception (unwinding)
        // edge, the unwound owned locals are auto-disposed by the implicit
        // try/finally and their scope ends, so the predecessor's exit state
        // is fed to the meet with those locals dropped (see
        // dischargeOnExceptionEdge). Normal predecessors are folded FIRST so
        // a local that is still Owned on a normal-exit edge into `b` seeds
        // the meet result and survives the union with the exception edges
        // that dropped it — preserving a real normal-path leak (§28.6).
        //
        // Async-readiness 3c (§30.5 / §30.8): a synthetic COMPLETION edge
        // (abandonment / cancellation) gets the SAME discharge treatment — the
        // state-machine frame disposes every owned suspend-local it holds, so
        // a still-owned local arriving via a completion edge is dropped from
        // the meet (the frame disposed it), exactly as on an exception edge.
        // Both are "discharge edges." The §28.6 ordering still holds: normal
        // predecessors are folded first, so a real NORMAL-path leak survives
        // the union with the completion/exception edges that dropped the local.
        auto isDischargeEdge = [&](const flow::CFGBlock* p) {
            return flow::CFG::isExceptionEdge(p, b) ||
                   flow::CFG::isCompletionEdge(p, b);
        };
        // §28.3/§28.7 (#764): the name of the local that is PROVABLY NULL on the
        // CFG edge p→b, when p is an if/elif condition `x <> null` / `x = null`
        // on a local and b is the null successor (succs[0]=true, succs[1]=false).
        // Empty otherwise (loop headers, non-Branch preds, non-null-check
        // conditions). Condition-sensitive: fires only for a real null check.
        auto nullEdgeName = [](const flow::CFGBlock* p,
                               const flow::CFGBlock* b) -> std::string_view {
            if (!p || p->kind != flow::CFGBlock::Kind::Branch ||
                p->isLoopHeader || p->succs.size() < 2 || !p->condition)
                return {};
            const auto* be = dynamic_cast<const ast::BinaryExpr*>(p->condition);
            if (!be || (be->op != "<>" && be->op != "=")) return {};
            const auto* id =
                dynamic_cast<const ast::IdentifierExpr*>(be->left.get());
            bool nullOther =
                dynamic_cast<const ast::NullLiteralExpr*>(be->right.get())
                    != nullptr;
            if (!id) {  // accept the reversed `null <> x`
                id = dynamic_cast<const ast::IdentifierExpr*>(be->right.get());
                nullOther =
                    dynamic_cast<const ast::NullLiteralExpr*>(be->left.get())
                        != nullptr;
            }
            if (!id || !nullOther) return {};
            // `x <> null`: x is null on the FALSE edge (succs[1]).
            // `x = null` : x is null on the TRUE  edge (succs[0]).
            const flow::CFGBlock* nullSucc =
                (be->op == "<>") ? p->succs[1] : p->succs[0];
            return (b == nullSucc) ? id->name : std::string_view{};
        };
        auto foldPred = [&](const flow::CFGBlock* p, bool dischargeEdge) {
            if (!evaluated.count(p)) return;
            // §28.3 / #892: for a loop-header → after-block skip edge, the
            // condition expression was NOT executed (TryPop returned false and
            // did not write the out-owned local), so the correct state on this
            // edge is the PRE-CONDITION state captured in loopSkipExitState_.
            // Use it when available; otherwise fall back to exitState[p] (which
            // is the post-condition state and would wrongly mark item Owned).
            // For back-edges and all non-skip edges, exitState[p] is correct.
            const bool skipEdge = isLoopSkipEdge(p, b);
            auto skipIt = skipEdge ? loopSkipExitState_.find(p)
                                   : loopSkipExitState_.end();
            const FlowState& predFlow =
                (skipEdge && skipIt != loopSkipExitState_.end())
                    ? skipIt->second
                    : exitState[p];
            // §28.8.1 (#867): merge this predecessor's field-moved-from facts
            // (OR). Done for every evaluated predecessor regardless of edge
            // kind — field facts are not discharged on exception/completion
            // edges (see comment above).
            joinFieldsInto(inFields, predFlow.fields);
            const bool loopEdge =
                backEdges.count({p, b}) != 0 || skipEdge;
            const BlockState& peRaw = predFlow.locals;
            BlockState peDischarged;
            if (dischargeEdge)
                peDischarged =
                    dischargeOnExceptionEdge(peRaw, unwoundDeclsFor(b));
            const BlockState& peBase = dischargeEdge ? peDischarged : peRaw;
            // §28.3/§28.7 (#764): on the edge where x is provably null, x carries
            // no disposal obligation ("disposal of null is a no-op"). Feed x as
            // Disposed into THIS edge's contribution only — the other (non-null)
            // edge keeps x Owned, so a genuine leak there (e.g. a then-branch
            // that never disposes, or a null branch that re-acquires ownership)
            // still surfaces at the merge join.
            BlockState peGuard;
            const BlockState* pePtr = &peBase;
            if (std::string_view nn = nullEdgeName(p, b); !nn.empty()) {
                peGuard = peBase;
                for (auto& [sym, lo] : peGuard)
                    if (sym && sym->name == nn &&
                        lo.state == OwnershipState::Owned)
                        lo.state = OwnershipState::Disposed;
                pePtr = &peGuard;
            }
            const BlockState& pe = *pePtr;
            if (!haveIn) {
                in = pe;
                haveIn = true;
            } else {
                joinInto(in, pe, joinLoc,
                         /*emitClash=*/emitOnMerge && isIfMerge && !loopEdge,
                         /*unionKeys=*/loopEdge);
            }
        };
        for (const flow::CFGBlock* p : b->preds)
            if (!isDischargeEdge(p)) foldPred(p, false);
        for (const flow::CFGBlock* p : b->preds)
            if (isDischargeEdge(p)) foldPred(p, true);
        return FlowState{std::move(in), std::move(inFields)};
    };

    // --- O3b fixpoint phase (diagnostics suppressed) ----------------------
    //
    // Iterate the RPO until a full pass changes no block's exit state. The
    // lattice per local is finite (seven states; PolyNull ⊓ X absorbs, and
    // every disagreeing merge moves to a strictly higher fixed element —
    // Poisoned or Uninitialized — never back down), so each block's exit
    // state ascends a bounded chain and the iteration terminates. The
    // `maxPasses` cap is a defensive backstop, sized to the block count
    // times the lattice height; it should never be hit for a reducible
    // (structured) CFG.
    suppressDiag_ = true;
    const std::size_t maxPasses = (post.size() + 1) * 8 + 4;
    for (std::size_t pass = 0; pass < maxPasses; ++pass) {
        bool changed = false;
        for (const flow::CFGBlock* b : post) {
            // §28.8.1 (#867): seed the per-block working state for BOTH the
            // locals (states_) and the field-moved-from facts (fieldMovedFrom_)
            // from the merged entry. processBlock's transfer functions mutate
            // both members in place; snapshot both into the block's FlowState
            // exit afterward so the field facts participate in the next pass's
            // joins exactly like the locals.
            FlowState fs = computeEntry(b, /*emitOnMerge=*/false);
            states_ = std::move(fs.locals);
            fieldMovedFrom_ = std::move(fs.fields);
            processBlock(*b);
            FlowState exit{states_, fieldMovedFrom_};
            auto it = exitState.find(b);
            if (it == exitState.end() || !sameFlow(it->second, exit)) {
                changed = true;
            }
            exitState[b] = std::move(exit);
            evaluated.insert(b);
        }
        if (!changed) break;
    }

    // --- O3b final emit pass (diagnostics enabled) ------------------------
    //
    // The fixpoint above left `exitState` stable. Walk the RPO one last
    // time with diagnostics on, so each block's reads/moves/disposals and
    // branch-join clashes are reported exactly once, against the converged
    // predecessor exit states.
    // §28.8.1 (#867): `fieldMovedFrom_` is now seeded per block from the merged
    // entry (it is the current-block working map), so it no longer needs a
    // single-walk reset — each block starts from its own merged entry facts.
    // `fieldMoveLoc_` is a global accumulator of move-site locations (used only
    // to place the exit diagnostic); reset it once so it reflects this walk.
    suppressDiag_ = false;
    fieldMoveLoc_.clear();
    for (const flow::CFGBlock* b : post) {
        FlowState fs = computeEntry(b, /*emitOnMerge=*/true);
        states_ = std::move(fs.locals);
        fieldMovedFrom_ = std::move(fs.fields);
        processBlock(*b);
        exitState[b] = FlowState{states_, fieldMovedFrom_};
    }

    // End-of-method state = the meet of every path reaching the CFG exit.
    // emitClash stays false (a clash on two paths to exit was already
    // reported at its originating Merge block; the old walker never met at
    // exit). unionKeys=true so an owned obligation that survives on any
    // NORMAL path — e.g. a loop-body local that reaches exit still Owned —
    // is preserved for the undischarged-owned check in run() (§28.3 /
    // §28.6: an owned value must be discharged on every normal-exit path).
    // A predecessor reaching exit along an exception edge (a naked `throw`
    // with no enclosing handler) contributes its post-unwind state, in
    // which the unwound owned locals have been dropped (§28.3 exception-path
    // discharge), so the throw path never produces a leak diagnostic.
    BlockState endState;
    // §28.8.1 (#867): the field-moved-from facts merged over every path that
    // reaches exit (OR-over-paths). run() reads this for the field-restoration
    // check, so a field left moved-from on ANY exit path is reported. Field
    // facts are NOT discharged on the throw/completion exit edges (unwinding
    // disposes locals but does not restore a moved-from field — §28.8.1
    // "Exception paths are not exempt").
    FieldMovedFrom endFields;
    bool haveEnd = false;
    auto foldExitPred = [&](const flow::CFGBlock* p, bool dischargeEdge) {
        if (!evaluated.count(p)) return;
        joinFieldsInto(endFields, exitState[p].fields);
        const BlockState& peRaw = exitState[p].locals;
        BlockState peDischarged;
        if (dischargeEdge)
            peDischarged =
                dischargeOnExceptionEdge(peRaw, unwoundDeclsFor(cfg.exit()));
        const BlockState& pe = dischargeEdge ? peDischarged : peRaw;
        if (!haveEnd) {
            endState = pe;
            haveEnd = true;
        } else {
            joinInto(endState, pe, SourceLocation{}, /*emitClash=*/false,
                     /*unionKeys=*/true);
        }
    };
    // §28.6 / §30.5: a "discharge edge" to exit is an exception edge (naked
    // throw → whole-method unwind) OR a synthetic completion edge (async
    // abandonment / cancellation → state-machine frame disposes its
    // suspend-locals). Both drop their owned locals. To preserve a real
    // NORMAL-path leak, each UNIQUE predecessor block is folded ONCE: as a
    // normal edge when it has any non-discharge edge to exit (so its
    // still-Owned locals seed the meet and survive the union), and as a
    // discharge edge only when its ONLY edges to exit are discharge edges (a
    // block whose sole exit path is a throw, or an await block whose control
    // flows onward but which the frame could abandon here). This dedup matters
    // for an async fall-off block that has BOTH a normal edge (the real
    // normal-completion path, which must catch an undischarged owned local)
    // and a synthetic completion edge: classifying it as normal preserves the
    // normal-path leak. Normal predecessors are folded first (same §28.6
    // ordering as computeEntry). // per §28.6, §30.5
    auto exitEdgeIsDischargeOnly = [&](const flow::CFGBlock* p) {
        // True when p reaches exit ONLY via discharge edges (no plain edge).
        bool hasNormal = false, hasDischarge = false;
        for (const flow::CFGBlock* s : p->succs) {
            if (s != cfg.exit()) continue;
            if (flow::CFG::isExceptionEdge(p, cfg.exit()) ||
                flow::CFG::isCompletionEdge(p, cfg.exit()))
                hasDischarge = true;
            else
                hasNormal = true;
        }
        return hasDischarge && !hasNormal;
    };
    std::unordered_set<const flow::CFGBlock*> foldedExitPred;
    // Pass 1: blocks that reach exit via a normal edge (fold as normal — seeds
    // and preserves any normal-path Owned obligation).
    for (const flow::CFGBlock* p : cfg.exit()->preds) {
        if (exitEdgeIsDischargeOnly(p)) continue;
        if (!foldedExitPred.insert(p).second) continue;
        foldExitPred(p, /*dischargeEdge=*/false);
    }
    // Pass 2: blocks whose ONLY exit edges are discharge edges (fold as
    // discharge — their owned locals are auto-disposed on that exit).
    for (const flow::CFGBlock* p : cfg.exit()->preds) {
        if (!exitEdgeIsDischargeOnly(p)) continue;
        if (!foldedExitPred.insert(p).second) continue;
        foldExitPred(p, /*dischargeEdge=*/true);
    }
    if (!haveEnd) {
        // No path reaches exit. Fall back to the entry-block exit state.
        auto it = exitState.find(cfg.entry());
        if (it != exitState.end()) {
            endState = it->second.locals;
            endFields = it->second.fields;
        }
    }
    return FlowState{std::move(endState), std::move(endFields)};
}

void MethodOwnership::processBlock(const flow::CFGBlock& block) {
    // §29.2 (#834) "Iterator yield ownership": when this block is the
    // body-entry of a `for`/`await for` loop whose iterable is an owning
    // enumerable (`IOwningEnumerable<T>` / `IOwningAsyncEnumerable<T>`), the
    // iteration variable `x` is a *plain owned local* — "owned and must be
    // discharged inside the loop body. Falling off the body without
    // discharging is a compile error." Seed `x` as Owned at the body entry of
    // EACH iteration (this runs once per fixpoint pass on the body-entry
    // block, overwriting any state carried around the back-edge — the value is
    // freshly bound each iteration per §29.2). The existing §28.3 owned-local
    // discharge machinery then carries it through the body: an undischarged
    // `x` survives Owned to the back-edge / break / continue / return and is
    // reported by the end-of-method undischarged-owned check, exactly as a
    // body-declared owned local is. A BORROWING for-loop (Array<T> /
    // IEnumerable<T>, range, inline array, string) is NOT seeded — its
    // iteration variable is a borrow with no discharge obligation (§29.2:
    // default iteration is borrowing), so ordinary for-loops are unaffected.
    if (block.forBodyEntry) {
        seedOwnedIterVar(*block.forBodyEntry);
    }
    // Each CFG block holds only straight-line statements plus the leaf
    // terminators break / continue / return / throw. The existing
    // per-statement transfer functions handle them. As of O11-real the CFG
    // expands try/catch/finally into real blocks and exception edges, so —
    // like if/while/for/do — a TryStmt is never seen here as a block
    // statement; the if/while/for/do/try branches inside visitStmt are
    // retained but unreachable from a block's statement list (the CFG
    // already extracted that control flow).
    for (const ast::Stmt* s : block.stmts) {
        if (s) visitStmt(*s);
    }
    // §28.3 / #892: process the branch block's CONDITION expression through
    // handleDischarge so that out-owned argument marking (markOutOwnedArgs /
    // §28.3 §28.11 #860) and move recording (recordMovesIn) happen for a
    // call used as an if/while condition.  Without this a Try*(out owned item)
    // call in the condition never invokes markOutOwnedArgs, so the receiving
    // local is never marked Owned and the §28.3 discharge check does not fire
    // when item is later dropped — a silent leak.
    //
    // The condition evaluates on EVERY iteration of a while loop and on the
    // if-test for an if statement. The out-owned marking must propagate into
    // the body (true edge). For the FALSE edge (skip, after the loop):
    // the TryPop call returned false so it did NOT write to `item` — the
    // local carries the same state as BEFORE the condition ran (e.g. null /
    // Disposed from the prior iteration's body). We capture the pre-condition
    // state for loop headers in loopSkipExitState_ so computeEntry can use
    // it when folding the header→after skip edge, giving the false edge the
    // correct pre-condition state instead of the Owned post-condition state.
    // For if/elif conditions (isLoopHeader=false) the false edge IS the else
    // branch and sema already models `item?.Dispose()` covering both paths.
    if (block.kind == flow::CFGBlock::Kind::Branch && block.condition) {
        if (block.isLoopHeader) {
            // Capture the pre-condition state for the skip edge.  Both the
            // locals map and the field-moved-from facts are saved so the
            // skip-edge entry faithfully represents the state when TryPop
            // returned false (did NOT write item). // per §28.3, #892
            loopSkipExitState_[&block] = FlowState{states_, fieldMovedFrom_};
            // §28.3 / #1387: while-condition `out owned var` items share the
            // same slot across ALL iterations (ensureOutVarSlots alreadyMapped).
            // Auto-drop only disposes the FINAL slot value → earlier iterations'
            // values would leak. Track these symbols so the auto-drop check in
            // run() can exclude them (they keep the hard §28.3 discharge error).
            collectOutOwnedVarSyms(*block.condition, ctx_, loopCondOutOwnedSyms_);
        }
        handleDischarge(*block.condition);
    }
}

// §29.2 (#834): seed the iteration variable of `fs` as Owned when the loop's
// iterable resolves to an owning enumerable. The iteration variable's
// LocalSymbol was created by ExprTypecheck with `decl == &fs` (the ForStmt);
// find it and set its dataflow state to Owned with the ForStmt as the
// obligation origin (so the undischarged diagnostic points at the loop). A
// no-op for a borrowing iterable, a value/range iterable, or when no iteration
// variable symbol exists (`for var _ in ...` is rejected earlier; a missing
// symbol simply seeds nothing).
void MethodOwnership::seedOwnedIterVar(const ast::ForStmt& fs) {
    if (!iterableIsOwningEnumerable(fs)) return;
    sema::LocalSymbol* iterSym = findForLoopVarSymbol(fs);
    if (!iterSym) return;
    LocalOwnership lo;
    lo.state = OwnershipState::Owned;
    lo.implicitDischarge = false;
    // The obligation originates at the loop: a fresh owned value is bound to
    // `x` each iteration (§29.2). Anchor the diagnostic at the ForStmt span.
    lo.obligationOrigin = &fs;
    states_[iterSym] = lo;
    // Track every iter var seeded for this method so the post-endState check
    // can detect break-only bodies (§29.2/#1329 addendum — see run()).
    seededIterVarSyms_.insert(iterSym);
}

// §29.2 (#834): true when the for-loop's iterable has static type
// `IOwningEnumerable<T>` or `IOwningAsyncEnumerable<T>` (the owning-enumerator
// desugaring path). Detected by the resolved iterable type's name-head,
// mirroring the owned-call classification at producerState (which name-checks
// `IOwningEnumerable` among the always-owned handle heads). A Named TypeRef
// carries its name either on the AST node (`astNode->name->parts`) or, for an
// imported type, in `importedName`; check both. Any other iterable kind
// (Array<T>/IEnumerable<T> borrowing, range, inline array, string, span)
// returns false, so its iteration variable is left a borrow with no discharge
// obligation.
bool MethodOwnership::iterableIsOwningEnumerable(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    const sema::TypeRef* tt = ctx_.exprType(*fs.iterable);
    if (!tt || tt->kind != sema::TypeRef::Kind::Named) return false;
    std::string_view head;
    if (tt->astNode && tt->astNode->name && !tt->astNode->name->parts.empty())
        head = tt->astNode->name->parts.front();
    else if (!tt->importedName.empty())
        head = tt->importedName;
    return head == "IOwningEnumerable" || head == "IOwningAsyncEnumerable";
}

// §29.2 (#834): find the LocalSymbol ExprTypecheck created for `fs`'s
// iteration variable. Sema anchors that symbol on the ForStmt
// (`lsym->decl = fs`), so it is the LocalSymbol whose `decl` is `&fs`.
sema::LocalSymbol* MethodOwnership::findForLoopVarSymbol(
    const ast::ForStmt& fs) {
    for (const auto& s : ctx_.symbols()) {
        if (auto* ls = dynamic_cast<sema::LocalSymbol*>(s.get())) {
            if (ls->decl == &fs) return ls;
        }
    }
    return nullptr;
}

void MethodOwnership::visitStmt(const ast::Stmt& s) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        handleLocalDecl(*ld);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) handleDischarge(*es->expr);
        return;
    }
    // §19 / §28.8 (#1280): a `case` STATEMENT. The CFG appends a CaseStmt as a
    // single opaque node (flow/CFG.cpp visitStmt has no case handler), so this
    // transfer must walk the scrutinee, each arm's guard, and each arm body
    // itself — otherwise the ownership checks (notably the §28.8 borrow→`owned`
    // argument check) never fire on a call inside a case arm (`Box(a) =>
    // Prog.Sink(a)` handed the borrowed destructure binding `a` to an `owned`
    // sink undetected → double-free). Each arm is a mutually-exclusive branch;
    // visit it from the pre-case state and restore afterward (conservative — the
    // arm-local discharge tracking does not leak into the post-case state).
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        if (cs->scrutinee) checkRead(*cs->scrutinee, /*selfTarget=*/false);
        BlockState saved = states_;
        for (const auto& arm : cs->arms) {
            states_ = saved;
            if (arm.guard) checkRead(*arm.guard, /*selfTarget=*/false);
            if (arm.bodyKind == ast::CaseArmBodyKind::Expression) {
                if (arm.exprBody) handleDischarge(*arm.exprBody);
            } else {
                for (const auto& bs : arm.stmts) if (bs) visitStmt(*bs);
            }
        }
        states_ = std::move(saved);
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) {
            handleDischarge(*ds->value);
            // §17.13 / §28.3 / #1387: `_ := top` where `top` is an `out owned var`
            // inline local (ls->decl is IdentifierExpr) that is currently Owned.
            // Recognize this as an explicit §17.13 discharge: mark `top` as moved-
            // from (so the affine auto-drop path does not fire and emit a warning)
            // and flag the DiscardStmt so codegen emits null-guarded dispose+reclaim
            // of the local's slot at this statement and nulls the slot.
            // Restricted to IdentifierExpr-decl locals (the `out owned var` inline
            // form) — regular LocalDecl locals go through the auto-drop path which
            // is already correct. // per §17.13, §28.3, §28.11
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                    ds->value.get())) {
                auto* dstate = lookupState(id->name);
                if (dstate && dstate->state == OwnershipState::Owned) {
                    // Find the symbol to confirm it is an `out owned var` inline
                    // local (LocalSymbol with IdentifierExpr decl).
                    sema::LocalSymbol* dls = nullptr;
                    for (auto& [sym, st] : states_) {
                        if (sym->name == id->name) {
                            dls = dynamic_cast<sema::LocalSymbol*>(sym);
                            break;
                        }
                    }
                    if (dls && dynamic_cast<const ast::IdentifierExpr*>(
                                   dls->decl)) {
                        markMovedByName(id->name, id->span.begin);
                        const_cast<ast::DiscardStmt*>(ds)->discardOutOwnedVar =
                            true;
                    }
                }
            }
            // §17.13 / §28.3 v0.150 (#1009): a discard `_ := <owned producer>` of
            // a USER class/interface is an *unbound owned temporary* (§28.3) — it
            // is disposed at end-of-statement. Mark it so codegen emits that
            // disposal. producerState == Owned is the sound gate: a BORROW (a
            // field/local read, a borrow-returning collection method, a plain
            // Task) is Borrowed and left unmarked, so codegen never disposes it
            // (no double-free). Restricted to a USER class/interface (declaration
            // is a Class/Interface with a real AST decl, not value-builtin) —
            // builtin handles (Task, Subscription, Shared, Stopwatch, …) resolve
            // to a declaration-less / decl-null TypeSymbol and need their own free
            // paths (deferred); owning collections are Owned* but decl-null too.
            if (producerState(*ds->value) == OwnershipState::Owned) {
                if (const sema::TypeRef* rt = ctx_.exprType(*ds->value))
                    if (rt->kind == sema::TypeRef::Kind::Named && rt->declaration &&
                        (rt->declaration->kind == ast::TypeDeclKind::Class ||
                         rt->declaration->kind == ast::TypeDeclKind::Interface) &&
                        !rt->declaration->isValueBuiltin &&
                        rt->declaration->decl != nullptr)
                        const_cast<ast::DiscardStmt*>(ds)->disposeOwnedValue = true;
            }
            // §17.13 / §15.3 (#1016): a discard `_ := expr` of a move-only
            // owning inline array (`[N owned]T` / `[N owned]T?`). Permitted per
            // §17.13 bullet 2 — disposes each non-null slot at end-of-statement.
            // MoveOnlyArrayTransferWalker walks the DiscardStmt value with
            // borrow=false and marks the source local as transferred, so
            // registerInlineOwnedArrayDisposal skips the scope-end disposal
            // (no double-free). Mark the discard here so codegen emits the
            // slot disposal. // per §17.13, §15.3
            if (const sema::TypeRef* rt = ctx_.exprType(*ds->value))
                if (rt->kind == sema::TypeRef::Kind::InlineArray && rt->arrayOwned)
                    const_cast<ast::DiscardStmt*>(ds)->disposeInlineOwnedArray = true;
        }
        return;
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        handleAssignment(*as);
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value) handleReturn(*rs->value);
        return;
    }
    // §29.6 / §29.8 (#945): walk a `yield return EXPR` operand through the shared
    // arg walker so any owned discharge INSIDE the operand is recorded. Without
    // this the CFG treated yield as an opaque straight-line statement and never
    // walked its operand — so `yield return Consume(c)` (Consume(owned x) consumes
    // the owned param `c`) left `c` un-marked, the iterator owned-param auto-
    // dispose (run() end-of-method check) then wrongly flagged it, and codegen
    // disposed it again at iterator-Dispose: a double-free. handleDischarge marks
    // an owned argument moved inside a call but treats a BARE yielded identifier /
    // member-access as a read (not a discharge), so `yield return c` and
    // `yield return c.V` keep `c` Owned and are correctly auto-disposed. A
    // `yield break` (null value) carries nothing to walk.
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        if (ys->value) handleDischarge(*ys->value);
        return;
    }
    // §31.1 / §28.8 (issue #406): the operand of a `throw` is an ordinary
    // expression evaluated to an owned value that the throw then consumes
    // (§31.1: "`new SomeException(...)` produces an owned class instance that
    // the `throw` statement immediately consumes"). A thrown `new`/call must
    // therefore have its arguments walked through the same owned-argument check
    // (§28.8 transfer) as a `new`/call in any other operand position — a borrow
    // passed to an `owned` constructor/method parameter is a double-free
    // whether or not the `new`/call is the throw operand. `handleDischarge` is
    // the shared arg walker used by expression-statement / decl / assignment
    // operands; routing the thrown value through it closes the throw-operand
    // sink that previously skipped the check. A bare re-throw `throw e;` of a
    // borrow (the in-flight exception, §31.2.4) is a bare identifier, not a
    // `new`/call, so it falls through to `checkRead` and is correctly NOT
    // treated as an owned sink — the throw transfers it back to the runtime,
    // which §31.1 explicitly permits. // per §31.1, §28.8
    if (auto* th = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        // §31.1 (issue #430): check for a catch borrow escaping via a
        // NON-OWNED constructor argument of the thrown object. This is the
        // `throw new Wrap(ex)` pattern where Wrap's ctor stores ex in a
        // non-owned field. The throw escapes the catch arm, so ex may outlive
        // it after the runtime frees the caught exception. // per §31.1
        if (th->value) {
            if (const auto* ne =
                    dynamic_cast<const ast::NewExpr*>(th->value.get())) {
                checkCatchBorrowEscapesViaNewExpr(*ne, ne->span.begin);
            }
            handleDischarge(*th->value);
        }
        return;
    }
    // Slice O4: if-then-else handling with branch-join semantics
    // per §28.3. Each branch is walked with a snapshot of the
    // pre-branch state; the post-branch states are joined into the
    // current state. Owned/Borrowed mismatch at the join is a
    // diagnostic.
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        // Snapshot the state entering the if-statement.
        auto entry = states_;
        // Walk the if-branch.
        for (const auto& bs : ifs->ifBranch.body) if (bs) visitStmt(*bs);
        auto branchEndStates = std::vector{states_};
        // For each elif: restore entry, walk, snapshot.
        for (const auto& el : ifs->elifBranches) {
            states_ = entry;
            for (const auto& bs : el.body) if (bs) visitStmt(*bs);
            branchEndStates.push_back(states_);
        }
        // Else branch (or implicit fall-through == entry).
        if (ifs->hasElse) {
            states_ = entry;
            for (const auto& bs : ifs->elseBody) if (bs) visitStmt(*bs);
            branchEndStates.push_back(states_);
        } else {
            // No else: the false edge falls through with entry state.
            branchEndStates.push_back(entry);
        }
        // Join all branches. Start from the first; reduce in pairs.
        auto joined = branchEndStates.front();
        for (std::size_t i = 1; i < branchEndStates.size(); ++i) {
            joinInto(joined, branchEndStates[i], ifs->span.begin);
        }
        states_ = std::move(joined);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        for (const auto& bs : ws->body) if (bs) visitStmt(*bs);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        for (const auto& bs : fs->body) if (bs) visitStmt(*bs);
        return;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& bs : dob->body) if (bs) visitStmt(*bs);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        for (const auto& bs : ts->body) if (bs) visitStmt(*bs);
        for (const auto& arm : ts->catches) {
            if (arm.body) visitStmt(*arm.body);
        }
        for (const auto& bs : ts->finallyBody) if (bs) visitStmt(*bs);
        return;
    }
}

OwnershipState MethodOwnership::producerState(const ast::Expr& e) {
    // `new T(...)` is the canonical producer of Owned values — but only
    // for reference types (classes). §9: a record is a value type with
    // no disposal obligation (§9.7 forbids record destructors and owned
    // fields), so `new Record(...)` yields a plain value, not an owned
    // resource. (Enums and other value kinds likewise.)
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        {  // §17.7 (#979): a `new(args)` shorthand has a null `ne->type`; sema
           // recorded the construction target as the NewExpr's exprType. Use it
           // so a shorthand `new Record(...)` (value type) is classified
           // Borrowed (no obligation), not wrongly Owned — which would reject
           // the value local as an undisposed owned resource (§28.3).
            if (const sema::TypeRef* rt =
                    ne->type ? ctx_.resolvedType(*ne->type) : ctx_.exprType(e)) {
                if (rt->kind == sema::TypeRef::Kind::Named &&
                    rt->declaration &&
                    (rt->declaration->kind != ast::TypeDeclKind::Class &&
                     rt->declaration->kind != ast::TypeDeclKind::Interface)) {
                    return OwnershipState::Borrowed;  // value type: no obligation
                }
                // §2.5 / §2.6 / §6.1 / §28 (issue #434): builtin value types
                // (DateTime, TimeSpan, Guid, CancellationToken) carry
                // isValueBuiltin=true when the TypeSymbol was created via
                // builtinClassSym. When TypeResolution found the name in the
                // builtinTypeArity table it leaves declaration null and the name
                // is on the ast::Type node; check both paths. // per §28
                if (rt->kind == sema::TypeRef::Kind::Named) {
                    // Path A: declaration set by ExprTypecheck (builtinClassSym).
                    if (rt->declaration && rt->declaration->isValueBuiltin)
                        return OwnershipState::Borrowed;
                    // Path B: TypeResolution left declaration null; read the
                    // type name from the ast::Type to recognise value builtins.
                    if (!rt->declaration && rt->astNode && rt->astNode->name &&
                        rt->astNode->name->parts.size() == 1) {
                        std::string_view n = rt->astNode->name->parts[0];
                        if (n == "DateTime" || n == "TimeSpan" ||
                            n == "Guid"     || n == "CancellationToken" ||
                            n == "DateOnly" || n == "TimeOnly")  // per §2.5 / #1357
                            return OwnershipState::Borrowed;
                    }
                }
            }
        }
        return OwnershipState::Owned;
    }
    // `null` is the polymorphic-null state (§28.3 null-literal row).
    if (dynamic_cast<const ast::NullLiteralExpr*>(&e)) {
        return OwnershipState::PolyNull;
    }
    // Async-readiness 3c (§30.5 Pattern B): `await <OwningTask<T>>` for class T
    // produces an OWNED value — ownership transfers from the task to the
    // awaiter. The standard §28.3 owned-local discharge rules then apply to the
    // binding (§30.5 line ~6148: "`x` is an owned local subject to standard
    // discharge rules"). A Pattern A `await <Task<T>>` (class T) and a value/
    // unit await carry no obligation and fall through to Borrowed. The await is
    // only owned-producing in an async body (3b only types awaits there); the
    // kind classifier reads the operand's resolved task type. // per §30.5
    if (isAsync_) {
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            if (ue->op == "await" &&
                awaitTaskKind(*ue) == AwaitTaskKind::PatternB) {
                return OwnershipState::Owned;
            }
        }
    }
    // §28.6 / §28.11 (#1107 finding D): a `move X` whose value is an OWNING
    // COLLECTION (OwningArray/OwningStack/OwningQueue/OwningSet/OwningDictionary/
    // OwningLinkedList) transfers the CONTAINER ownership the source local holds
    // to the result — the result is Owned (the target must dispose the container
    // and cascade to its elements, §28.11). Without this, `var arr2 := move arr1`
    // falls through to the owning-collection name-head fallback below, which
    // classifies a non-`owned` owning-collection-typed expr as Borrowed (§28.6:
    // the `Owned*` name denotes ELEMENT ownership for method returns) — so arr2
    // is never flagged Owned, never auto-dropped, and its elements leak. A `move`
    // of a plain class already classifies Owned via the fallback (which masked
    // this for classes). The moved-from source is marked moved-from by the
    // LocalDecl move handling, so exactly one owner (the target) disposes — no
    // double-free. // per §28.6, §28.11
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (ue->op == "move") {
            if (const sema::TypeRef* rt = ctx_.exprType(e))
                if (rt->kind == sema::TypeRef::Kind::Named && rt->astNode &&
                    rt->astNode->name && !rt->astNode->name->parts.empty() &&
                    isOwningCollectionNameHead(rt->astNode->name->parts.front()))
                    return OwnershipState::Owned;
        }
    }
    // Slice O13: a call to a method with an `owned T` return type
    // produces an Owned value. The callee MethodSymbol resolution
    // comes from sema; we look at its MethodDecl::returnOwned to
    // decide. A null callee symbol (overload not yet pickable, or
    // a call on a value with no symbol) falls through to the builtin
    // name-head check below. // per §28.3 / §30.14
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (ce->callee) {
            if (auto* sym = ctx_.exprSymbol(*ce->callee)) {
                if (auto* ms = dynamic_cast<sema::MethodSymbol*>(sym)) {
                    auto* md = dynamic_cast<const ast::MethodDecl*>(
                        ms->decl);
                    if (md) {
                        if (md->returnOwned) return OwnershipState::Owned;
                        // §28.6 / §5.11 (#1008 Part C): a USER method returning a
                        // non-`owned` owning COLLECTION (OwningArray, …) yields a
                        // BORROW (the `Owned*` name denotes ELEMENT ownership,
                        // orthogonal to the container ownership the `owned`
                        // qualifier governs). Do NOT fall through to the builtin
                        // name-head fallback below for collections — that would
                        // classify it Owned. The definition-site check
                        // checkBorrowReturnedThroughOwningType guarantees such a
                        // return is a genuine borrow (a bare receiver-field
                        // read), never a fresh/`move`d/owned value, so this never
                        // under-disposes an owned result. Owning WRAPPERS
                        // (OwningTask, Shared, …) keep consumer-owns semantics —
                        // they fall through to the name-head fallback (Owned),
                        // and a borrow through them stays the #1007 reject.
                        if (const sema::TypeRef* rt = ctx_.exprType(e))
                            if (rt->kind == sema::TypeRef::Kind::Named &&
                                rt->astNode && rt->astNode->name &&
                                !rt->astNode->name->parts.empty() &&
                                isOwningCollectionNameHead(
                                    rt->astNode->name->parts.front()))
                                return OwnershipState::Borrowed;
                        // Other non-`owned` user returns: fall through.
                    }
                }
            }
        }
        // #1290 / §27.1 / §28.3: a call through a FUNCTION-TYPED VALUE whose
        // declared return slot carries `owned` (`mk: () -> owned Res; mk()`)
        // produces a fresh owned value, exactly like an owned-returning method
        // call. The callee is an ordinary value (a LocalSymbol / field, not a
        // MethodSymbol), so the returnOwned check above does not fire; classify
        // it Owned from the callee's function type's funcReturnOwned flag.
        // Without this the owned result carried no disposal obligation and
        // leaked even under `using var` (§27.1 admits `owned` in a function-type
        // return slot). (A bare inline lambda with no `owned`-annotated context
        // does not set funcReturnOwned, so its result is not owned here.)
        if (ce->callee) {
            if (const sema::TypeRef* calleeTy = ctx_.exprType(*ce->callee)) {
                if (calleeTy->kind == sema::TypeRef::Kind::Function &&
                    calleeTy->funcReturnOwned)
                    return OwnershipState::Owned;
            }
        }
        // Slice O13 / §28.13 / §30.14 (#693): builtin owned-returning calls.
        // Several stdlib builtin methods return `owned T` but have no user
        // MethodDecl (so returnOwned is unavailable): Shared<T>.Clone(),
        // Shared<T>.Downgrade(), Channel<T>.NewSender(), and the owning-
        // collection factory/drain methods (GetBytes, PopLast, PopFirst, etc.).
        // Mirror the awaitTaskKind pattern: dispatch by the resolved return
        // type's name-head.  A Named TypeRef with no declaration (builtin) or
        // with a Class/Interface declaration whose name-head is one of the
        // always-owned handle types signals an owned-producing call.
        // This is the correct classification for `var clone := sm.Clone()`
        // (Shared<T>), `var sender := ch.NewSender()` (ChannelSender<T>),
        // `var cts := CancellationTokenSource.New()`, etc. // per §28.13, §30.14
        if (const sema::TypeRef* retTy = ctx_.exprType(e)) {
            if (retTy->kind == sema::TypeRef::Kind::Named &&
                retTy->astNode && retTy->astNode->name &&
                !retTy->astNode->name->parts.empty()) {
                std::string_view head = retTy->astNode->name->parts.front();
                if (head == "Shared"          || head == "Weak"            ||
                    head == "ChannelSender"    || head == "OwningTask"       ||
                    head == "OwningArray"       || head == "OwningDictionary" ||
                    head == "OwningSet"         || head == "OwningStack"      ||
                    head == "OwningQueue"       || head == "OwningLinkedList" ||
                    head == "IOwningEnumerable"  ||
                    head == "CancellationTokenSource") {
                    return OwnershipState::Owned;
                }
            }
        }
    }
    // #724 F1 / §28.8.1 / §17.x: `move x` TRANSFERS ownership of x to the binding,
    // so `var s := move x` makes `s` an OWNED local (subject to the standard §28.3
    // discharge rules), exactly as `var s := x` would have made `x`'s obligation
    // s's. Without this the move expression fell through to Borrowed, so `s`
    // carried no disposal obligation: it was neither disposed at scope end nor
    // diagnosed as an undischarged owned local — a silent leak. Owned only when the
    // moved value is a disposable reference type (class/interface); a `move` of a
    // value type (record/enum/value-builtin) is a copy with no obligation and
    // stays Borrowed, mirroring the `new T` classification above.
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (ue->op == "move" && ue->operand) {
            if (const sema::TypeRef* opTy = ctx_.exprType(*ue->operand)) {
                if (opTy->kind == sema::TypeRef::Kind::Named &&
                    opTy->declaration &&
                    (opTy->declaration->kind == ast::TypeDeclKind::Class ||
                     opTy->declaration->kind == ast::TypeDeclKind::Interface) &&
                    !opTy->declaration->isValueBuiltin) {
                    return OwnershipState::Owned;
                }
            }
        }
    }
    // #965 / §25 / §28.3: a user-defined BINARY operator overload declared with an
    // `owned` return (`static method operator +(a, b): owned Cell => new Cell(...)`)
    // produces a FRESH owned value, exactly like an owned-returning method call.
    // sema attaches the operator MethodSymbol to the BinaryExpr (§25 typeBinary)
    // and OperatorDecl::returnOwned is the authority. Without this the operator
    // result fell through to Borrowed, so `var r := a + b` carried no disposal
    // obligation: it was neither auto-dropped (§28.3) nor disposed — a silent leak
    // (#965). Owned only when the result is a disposable class/interface (an
    // `owned`-returning operator on a value type is impossible — §9.7 forbids
    // record destructors). The operands stay borrowed, so there is no double-free.
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (auto* sym = ctx_.exprSymbol(*be))
            if (auto* ms = dynamic_cast<sema::MethodSymbol*>(sym))
                if (auto* od = dynamic_cast<const ast::OperatorDecl*>(ms->decl))
                    if (od->returnOwned)
                        if (const sema::TypeRef* rt = ctx_.exprType(e))
                            if (rt->kind == sema::TypeRef::Kind::Named &&
                                rt->declaration &&
                                (rt->declaration->kind ==
                                     ast::TypeDeclKind::Class ||
                                 rt->declaration->kind ==
                                     ast::TypeDeclKind::Interface) &&
                                !rt->declaration->isValueBuiltin)
                                return OwnershipState::Owned;
        // #1029 / §28.3: built-in `??` (NullCoalescing) with an Owned-producing
        // RHS. The caller owns whichever branch's value is taken: LHS non-null →
        // ownership transfers from the LHS (codegen nulls the LHS slot); LHS null
        // → freshly-allocated RHS. Only fires for a reference-type result (value-
        // type `??` uses the nullable-struct path with no disposal obligation).
        // A user-defined `operator ??` with `owned` return is already handled
        // above (the sym/ms/od chain); we only reach here when there is no such
        // operator symbol attached (the built-in). // per §28.3
        if (be->op == "??" && be->right &&
            producerState(*be->right) == OwnershipState::Owned) {
            if (const sema::TypeRef* rt = ctx_.exprType(e))
                if (rt->kind == sema::TypeRef::Kind::Named && rt->declaration &&
                    (rt->declaration->kind == ast::TypeDeclKind::Class ||
                     rt->declaration->kind == ast::TypeDeclKind::Interface) &&
                    !rt->declaration->isValueBuiltin)
                    return OwnershipState::Owned;
        }
    }
    // Default: Borrowed.
    return OwnershipState::Borrowed;
}

void MethodOwnership::handleLocalDecl(const ast::LocalDecl& ld) {
    // #1066 / §9.7 (v0.153): `var b := new Bag([…])` from a temporary is a
    // dangling borrow. Checked before the lsym early-return so it runs for a
    // value-typed (record) binding too, matching the handleReturn/handleAssignment
    // hooks for the other binding contexts.
    if (ld.initializer)
        if (const auto* ne =
                dynamic_cast<const ast::NewExpr*>(ld.initializer.get()))
            checkRecordFromTemporary(*ne);
    // Find the LocalSymbol that ExprTypecheck created for this decl.
    sema::LocalSymbol* lsym = findLocalSymbol(ld);
    if (!lsym) return;
    // §28.3 (#747 R42): an initializer that READS a moved-from / disposed /
    // uninitialized local is a use-after-move/dispose. The initializer was
    // never run through checkRead, so `move b; var x := b._val;` (a member
    // access on a moved-from local) slipped through. Run checkRead here — but
    // NOT when the initializer is itself a top-level CONSUMING form (`move y` /
    // `await y`): those are handled by the existing, more specific
    // move/await-consume checks below, which emit the precise error; running a
    // broad read-check first would steal that diagnostic (and weaken it to a
    // use-after-dispose warning).
    if (ld.initializer) {
        bool topLevelConsume = false;
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(ld.initializer.get()))
            topLevelConsume = (ue->op == "move" || ue->op == "await");
        if (!topLevelConsume)
            checkRead(*ld.initializer, /*selfTarget=*/false);
    }
    LocalOwnership lo;
    lo.obligationOrigin = &ld;
    if (ld.isUsing) {
        // `using var` is always Owned and implicitly discharged.
        lo.state = OwnershipState::Owned;
        lo.implicitDischarge = true;
        // §28.3 / §28.10 adoption (Guide 1 §7 Pattern B; spec §30.15 example):
        // `using var x := <owned local or owned param>;` is an owned-consuming
        // binding — the using var IS an owned location, so the source's
        // obligation transfers (a move). Without this the source stays Owned
        // and is falsely reported dropped at method exit. Only an Owned
        // source transfers; a borrow RHS keeps §28.10's pin-without-obligation
        // behavior unchanged.
        if (ld.initializer) {
            if (auto* aid = dynamic_cast<const ast::IdentifierExpr*>(
                    ld.initializer.get())) {
                for (auto& [s, st] : states_) {
                    if (s->name == aid->name &&
                        st.state == OwnershipState::Owned) {
                        markMovedByName(aid->name, aid->span.begin);
                        break;
                    }
                }
                // §28.10 (#1285): if the bare-id source resolves to a local (or
                // parameter) that the flow currently holds as a BORROW — a
                // borrow local aliasing a borrow, or a borrow parameter — the
                // using var only pins it and must NOT dispose it at scope end
                // (its real owner does). Record that on the decl so codegen
                // skips the disposal. Only a positive Borrowed classification
                // sets the flag, so an owned transfer (Owned above), a
                // drain-loop element, or an owned parameter (which register
                // real disposals) is untouched.
                if (sema::Symbol* srcSym = ctx_.exprSymbol(*aid)) {
                    auto it = states_.find(srcSym);
                    if (it != states_.end() &&
                        it->second.state == OwnershipState::Borrowed)
                        const_cast<ast::LocalDecl&>(ld)
                            .usingVarSourceIsBorrow = true;
                }
            }
        }
    } else if (ld.initializer) {
        lo.state = producerState(*ld.initializer);
    } else {
        lo.state = OwnershipState::Uninitialized;
    }
    // Async-readiness 3c (§30.5 / §30.9): set the SuspendLocal annotation when
    // this owned local's declaration was classified (by annotateSuspendLocals)
    // as live across an await. Informational for the ownership pass — the
    // recorded contract for the async-codegen milestone (which locals go in the
    // state-machine frame). Only ever set in an async body. // per §30.5
    if (suspendLocalDecls_.count(&ld)) lo.suspendLocal = true;
    states_[lsym] = lo;
    // §28.3 (#1257): record a plain (non-`using`) owned local so the end-of-method
    // auto-drop can flag it even when its nested scope ends before the function
    // exit (so it never reaches `endState`). `using var`s are already Owned +
    // implicitly discharged and need no flag.
    if (!ld.isUsing && lo.state == OwnershipState::Owned)
        everOwnedPlainLocals_.insert(lsym);
    // Async-readiness 3c (§30.5 line ~6110): a `using var`/`var` initializer
    // `:= await <bound owned task local>` consumes that operand task local
    // (the Pattern B await is owned-consuming with respect to its operand).
    // Record the consume so a straight-line second await of the same local is
    // a read-after-consume. Done before the source-tracking below so the
    // operand's state reflects the consume. // per §30.5
    if (isAsync_ && ld.initializer) {
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(ld.initializer.get())) {
            if (ue->op == "await" && ue->operand &&
                awaitTaskKind(*ue) == AwaitTaskKind::PatternB) {
                recordAwaitOperandConsume(*ue->operand, ue->span.begin);
            }
        }
    }
    // Slice O8: if the initializer involves `move this.field`, mark
    // the field as moved-from (the move side-effect happens at
    // initializer evaluation time, regardless of the local's `using`
    // status). This lets `using var old := move this._log;` correctly
    // record that `_log` is now empty.
    if (ld.initializer) {
        recordMovesIn(*ld.initializer);
        // §28.9 / §9.7 indirect-escape tracking (issue #360, second slice):
        // when the initializer binds a borrow of an owned scope-bound local
        // (a re-borrow `r`/`r.f`, a borrow-bearing record `new Holder(r)`, or
        // a closure capturing `r`), record that owned source on this local so
        // a later `return b;` can be diagnosed as a borrow escape. Computed
        // AFTER recordMovesIn so a `move`d-out source (now MovedFrom, not
        // Owned) is correctly NOT treated as a live borrow source. A `using
        // var` is itself Owned, not a re-borrow, so it never gets a source.
        if (!ld.isUsing) {
            if (sema::Symbol* src = borrowSourceForExpr(*ld.initializer)) {
                BorrowSource bs;
                bs.sym = src;
                states_[lsym].source = bs;
            }
        }
    }
}

void MethodOwnership::handleAssignment(const ast::AssignmentStmt& as) {
    // Slice O4: `x := EXPR` where x is a tracked local updates x's
    // state to match the producer of EXPR.
    if (as.op != ":=") return;
    if (!as.target || !as.value) return;
    // Slice O15: side-effects in the RHS — `x := move y` should
    // transition y to MovedFrom. recordMovesIn handles `move local`,
    // `move this.field`, and `f(owned z)` in any position.
    recordMovesIn(*as.value);
    // #1066 / §9.7 (v0.153): `x := new Bag([…])` / `this.f := new Bag([…])`
    // from a temporary is a dangling borrow.
    if (const auto* ne = dynamic_cast<const ast::NewExpr*>(as.value.get()))
        checkRecordFromTemporary(*ne);
    // §28.3 (#386 cases 1+2): storing a borrow of an owned scope-bound local
    // into a field of an object, or writing it through an out/ref parameter,
    // lets the borrow outlive the local (use-after-dispose). Run AFTER
    // recordMovesIn so a `move`d source is no longer Owned and the transfer
    // form is correctly not flagged. checkBorrowStoreEscape itself skips the
    // legal bare-owned-LOCAL transfer into an OWNED field (a move-on-read per
    // §28.3, not a borrow store); it still catches catch-bound-exception borrows
    // (§31.1) and re-borrows of owned locals stored into any field. Its result
    // feeds the #402 owned-field check below so the same store is not reported
    // by both. // per §28.3, §31.1
    bool flagged386 = checkBorrowStoreEscape(as);
    // §27.5 / §28.6 (#1217): a lambda stored into a field or written through an
    // out/ref parameter that captures a ref/out parameter — the captured pointer
    // is to the caller's stack frame and is valid only for the duration of the
    // call; storing the closure in an escaping location lets it dereference a
    // dangling pointer after the call returns (use-after-free). Runs after
    // checkBorrowStoreEscape so any earlier §28.3 / §31.1 diagnostic is not
    // duplicated (checkBorrowStoreEscape returns true = error already reported,
    // but ref/out captures are a SEPARATE source not covered by §28.3 scope-
    // bound locals, so we always run this check regardless of flagged386). // per §27.5, §28.6
    checkRefOutLambdaCaptureOnStore(as);
    // §28.3 / §28.5 / §28.8 (issue #402): an `owned` FIELD is an owned storage
    // location and may only receive an OWNED value. A BORROW RHS is the
    // double-free hazard (the field's owner and the borrow's real owner both
    // dispose the value); a legal OWNED RHS records the ownership transfer INTO
    // the field (move-out of a bare owned local, discharge of a consumed owned
    // parameter). Runs after checkBorrowStoreEscape so #386's owned-local-borrow
    // and §31.1 catch-borrow diagnoses are not duplicated. // per §28.3, §28.5
    if (targetIsOwnedField(*as.target)) {
        handleOwnedFieldAssignment(as, flagged386);
    } else if (targetIsOwningInlineArraySlot(*as.target)) {
        // §28.11 (#780): an owning inline-array slot `a[i]` (a `[N owned]T?` /
        // `[N owned]T` slot) is an owned sink with the SAME discipline as an
        // `owned` field — its indexer-set "takes owned" (§28.11 slot policy).
        // A borrow RHS is the double-free hazard; a bare owned-LOCAL read is a
        // move INTO the slot (so a `using var` source is correctly rejected as
        // scope-bound, §28.3, and a plain owned local is recorded moved-from
        // rather than spuriously flagged as dropped-without-disposal). This is
        // the index-target analogue of the owned-field path above and of the
        // owning-collection indexer check below. `checkBorrowStoreEscape`
        // returns early for an index target, so `flagged386` is false here and
        // no diagnostic is duplicated. // per §28.11, §28.3
        handleOwnedFieldAssignment(as, flagged386,
                                   /*intoOwningArraySlot=*/true);
    } else if (!flagged386 && targetIsNonOwnedField(*as.target) &&
               producerState(*as.value) == OwnershipState::Owned) {
        // #1020 / §28.6 (v0.153): a freshly-produced owned value — a `new`, an
        // owned-returning call, a `move`, or an owned operator result
        // (producerState == Owned) — stored into a non-`owned` (borrow) field
        // has no owner and leaks: the field does not own its referent. The
        // storing-side counterpart of the non-`owned`-return rule (§28.6). A
        // bare borrow read of a parameter/local (the legitimate back-reference
        // store) is producerState == Borrowed and is correctly not flagged.
        // `flagged386` guards against double-reporting a borrow-of-owned-local
        // store that checkBorrowStoreEscape already flagged. // per §28.6
        std::string fieldName;
        if (auto* ma =
                dynamic_cast<const ast::MemberAccessExpr*>(as.target.get()))
            fieldName = std::string(ma->member);
        else if (auto* id =
                     dynamic_cast<const ast::IdentifierExpr*>(as.target.get()))
            fieldName = std::string(id->name);
        report(Severity::Error, as.value->span.begin,
               std::string("a freshly-produced owned value cannot be stored "
                           "into the non-`owned` field") +
                   (fieldName.empty() ? std::string()
                                      : " `" + fieldName + "`") +
                   " — the field does not own it, so the value would leak; "
                   "declare the field `owned` (§28.5), or hand it to a method "
                   "that takes it `owned` (§28.6)");
    }
    // §28.11 (#395): an owning-collection `set(owned)` indexer assignment
    // `coll[i] := value` requires `value` to be owned — "Indexer set
    // `coll[i] := value` requires `value` to be owned." A borrow handed to the
    // owned slot would be disposed by the slot (and by its real owner): a
    // double-dispose. Run after recordMovesIn so a `move`d-owned RHS has
    // already transferred and is correctly NOT seen as a borrow.
    if (indexerSetterTakesOwned(*as.target)) {
        // §28.11 (#395/#1167): the owning-collection indexer setter TAKES
        // ownership of the RHS exactly like `Add` — so a bare owned LOCAL must
        // be CONSUMED (marked moved-from), or it is disposed TWICE: once by the
        // slot and again by its scope-end auto-drop (the #1167 double-free
        // SIGSEGV). Mirror `recordMovesInArgs`' owned-sink consumption: reject a
        // borrow flowing into the owned slot, else record the implicit move of a
        // bare owned-local source. A fresh `new` / owned-returning call has no
        // local to consume; a `move`d local was already moved by recordMovesIn
        // above (and is no longer a bare identifier here).
        if (const auto* id =
                dynamic_cast<const ast::IdentifierExpr*>(as.value.get())) {
            if (!checkOwnedArgSource(*as.value, id->span.begin))
                markMovedByName(id->name, id->span.begin);
        } else {
            checkOwnedArgSource(*as.value, as.value->span.begin);
        }
    }
    if (const auto* id =
            dynamic_cast<const ast::IdentifierExpr*>(as.target.get())) {
        // §28.9 / §9.7 indirect-escape tracking: recompute the borrow source
        // for the reassigned local. Computed before the states_ scan so it
        // observes the pre-reassignment Owned states of any source local.
        // Set when the RHS re-borrows / aliases an owned scope-bound local,
        // and CLEARED otherwise so a stale source does not linger across a
        // rebind to a non-borrowing value (e.g. `b := null;`).
        sema::Symbol* src = borrowSourceForExpr(*as.value);
        for (auto& [sym, st] : states_) {
            if (sym->name != id->name) continue;
            // §22.3 + §28.8: assigning an owned value to an `out owned`
            // parameter transfers ownership OUT through the out-location to
            // the caller, who becomes responsible for discharging it. The
            // assignment is therefore a discharge point on the callee side —
            // both for the RHS's owned obligation (a fresh `new` / owned-
            // returning call, or a `move`d local whose move-from `recordMovesIn`
            // above already recorded) and for the `out owned` parameter's own
            // seeded obligation (run() seeds `out owned` params as Owned). We
            // mark the parameter Disposed (discharged), mirroring a `return`
            // under an `owned` return type: the value is now the caller's
            // responsibility and must not be re-flagged as undischarged at
            // method exit (the over-rejection of issue #390). Definite
            // assignment of the `out` parameter on every path (§7.9) is a
            // separate concern not handled here.
            if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym)) {
                if (ps->modifier == "out owned") {
                    st.state = OwnershipState::Disposed;
                    st.obligationOrigin = &as;
                    st.source.reset();
                    return;
                }
            }
            st.state = producerState(*as.value);
            st.obligationOrigin = &as;
            if (src && src != sym) {
                BorrowSource bs;
                bs.sym = src;
                st.source = bs;
            } else {
                st.source.reset();
            }
            return;
        }
        // §28.8.1 / #825: name not found in local states — this bare identifier
        // may resolve to an owned FIELD (implicit `this`). If so, clear the
        // field's moved-from mark: `_field := value` is the unqualified
        // restoration form, symmetric to `this._field := value` (below). We
        // check this here (inside the IdentifierExpr branch) because the
        // outer IdentifierExpr block returns before reaching the named-field
        // clear that follows. exprIsOwnedFieldRead confirms the sema symbol is
        // a FieldSymbol with the `owned` modifier. // per §28.8.1, #825
        if (exprIsOwnedFieldRead(*as.target)) {
            fieldMovedFrom_[id->name] = false;
        }
        return;
    }
    // Slice O8: `this.field := value` clears the moved-from mark
    // on the field. (We track only `this.field` shape — `other.f`
    // and indexer-set are sema's broader concern.)
    if (const auto* ma =
            dynamic_cast<const ast::MemberAccessExpr*>(as.target.get())) {
        if (const auto* tid =
                dynamic_cast<const ast::IdentifierExpr*>(
                    ma->target.get())) {
            if (tid->name == "this") {
                fieldMovedFrom_[ma->member] = false;
            }
        }
    }
}

void MethodOwnership::handleReturn(const ast::Expr& e) {
    // §28.9 / §9.7 escape checks (issue #360). These run before the
    // bare-identifier discharge fast-path below so that a returned
    // closure or borrow-bearing record is examined even when it is the
    // whole returned expression. Neither check marks any local disposed;
    // they only diagnose. The discharge handling below is unchanged.
    checkClosureEscapesOnReturn(e);       // D1: §28.9 closure capture escape
    checkRecordBorrowEscapesOnReturn(e);  // D2: §9.7 borrow-bearing record
    // #1066 / §9.7 (v0.153): `return new Bag([…])` from a temporary is a
    // dangling borrow exactly as a local-decl initializer is.
    if (const auto* ne = dynamic_cast<const ast::NewExpr*>(&e))
        checkRecordFromTemporary(*ne);
    checkIndirectEscapeOnReturn(e);       // §28.9/§9.7 indirect-via-local
    checkCatchBorrowEscapesOnReturn(e);   // §31.1 caught-exception escape
    // §31.1 (issue #430): `return new Wrap(ex)` where Wrap's ctor stores
    // ex in a non-owned field. The returned object escapes the catch arm;
    // same use-after-free hazard as the throw case above. // per §31.1
    if (const auto* ne = dynamic_cast<const ast::NewExpr*>(&e))
        checkCatchBorrowEscapesViaNewExpr(*ne, ne->span.begin);
    // §28.3 / §28.6 / §5.11 (#1007): reject laundering a borrow into an owned
    // alias through an owning-collection / owning-wrapper return type. Run BEFORE
    // the bare-identifier discharge fast-path below (which returns early for a
    // bare `return x;` and so would otherwise hide this case) and before
    // handleDischarge, so the (rejected) returned local is examined in its borrow
    // state. A no-op for non-owning return types. // per §28.3, §28.6, §5.11
    checkBorrowReturnedThroughOwningType(e);
    // §28.6 (#1005): reject returning an owned field's `move`d value
    // (`move this.f` / `move f`) through a non-`owned` return type — the moved
    // field value escapes with no owner (leak). A bare field read is a legal
    // borrow and is not flagged. Exempt when the return type is `owned` (the
    // legal transfer). // per §28.6
    checkOwnedFieldReturn(e);
    // For slice O1, treat `return x;` (bare identifier) as a
    // discharge. The "is the method's return type owned T?" check
    // is sema's; here we just satisfy the discharge requirement
    // pessimistically.
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        // §28.3 / §28.10 (#762): a `using var` is owned but scope-bound — per
        // §28.3 "it cannot be moved out of, only borrowed from." Returning it by
        // bare identifier transfers it out of its scope: for an `owned` return
        // type it is a move-on-read into the owned return slot, and for a borrow
        // return type it is a borrow that escapes the scope that disposes the
        // value — either way the value would be disposed both by the caller (or
        // the dangling borrow's reader) and by the using-var's scope-bound
        // discharge, a double-free / use-after-free. The bare-identifier
        // fast-path here ran before both the owned-move-out and borrow-escape
        // checks and silently marked the local discharged; reject instead,
        // mirroring the `markMovedByName` scope-bound guard. // per §28.3, §28.10
        if (auto* st = lookupState(id->name)) {
            if (st->implicitDischarge) {
                report(
                    Severity::Error, id->span.begin,
                    std::string("cannot return `using var` `") +
                    std::string(id->name) +
                    "`; a `using var` is scope-bound — it cannot be moved out of "
                    "(only borrowed from, and a borrow of it may not escape its "
                    "scope) (§28.3 / §28.10)");
                st->state = OwnershipState::Poisoned;
                return;
            }
        }
        markDisposedByName(id->name, id->span.begin);
        return;
    }
    // Slice O7: borrow-escape check. `return localRef.M(...)` /
    // `return localRef.f` returns a borrow whose source is the
    // local. If the local is a `using var` (scope-bound), the
    // borrow outlives its source — error per §28.3.
    checkBorrowEscapesOnReturn(e);
    // For `move x` return arm: covered by handleDischarge inside
    // the expression — but we also call it here.
    handleDischarge(e);
}

// §28.3 / §5.11 (#1007): the owning-collection / owning-wrapper type heads. A
// return type whose name-head is one of these denotes ELEMENT ownership (the
// `Owned*` type), not CONTAINER ownership (the separate `owned` qualifier,
// §5.11). The set mirrors the `producerState` owned-returning-call fallback so
// the two stay aligned. // per §28.3, §5.11
bool MethodOwnership::isOwningTypeNameHead(std::string_view head) {
    return head == "Shared"          || head == "Weak"            ||
           head == "ChannelSender"    || head == "OwningTask"       ||
           head == "OwningArray"       || head == "OwningDictionary" ||
           head == "OwningSet"         || head == "OwningStack"      ||
           head == "OwningQueue"       || head == "OwningLinkedList" ||
           head == "IOwningEnumerable"  ||
           head == "CancellationTokenSource";
}

// §28.6 / §5.11 (#1008 Part C): the owning *collections* — the borrow-return and
// capability rules of Part C apply to these. The owning *wrappers* (OwningTask,
// Shared, Weak, ChannelSender, IOwningEnumerable, CancellationTokenSource) have
// consumer-owns semantics Part C does not change: a non-`owned` wrapper return
// stays an owned producer (a borrow through it is the #1007 double-free hazard,
// still rejected), and the read-only capability restriction does not apply.
bool MethodOwnership::isOwningCollectionNameHead(std::string_view head) {
    return head == "OwningArray"  || head == "OwningDictionary" ||
           head == "OwningSet"    || head == "OwningStack"      ||
           head == "OwningQueue"  || head == "OwningLinkedList";
}

// §28.3 / §28.6 / §5.11 (#1007): a method declared `: OwningArray<T>` (or any
// owning-collection / owning-wrapper return type) WITHOUT the `owned` qualifier
// hands the caller a value the caller tracks as OWNED — the caller must
// discharge it (a `using var` binding disposes it; a plain `var` errors if it
// is dropped). If the returned expression is a BORROW (a non-`owned` parameter,
// a borrowed local, or a plain field read), that borrow is laundered into a
// second owner: both the original owner and the caller dispose the same value,
// a double-free (#1007). The element-ownership the `OwningArray` type name
// denotes (§5.11) is a SEPARATE axis from the container ownership the `owned`
// qualifier governs — so a bare (non-`owned`) owning-collection return that
// returns a borrow must be rejected, exactly as a borrowed `Array<T>` return is
// rejected by the §28.3 borrow-lifetime rule. A genuine owned producer (`new`,
// `move`, a call returning owned, an owned local/param transferred out) is NOT
// a borrow (ownedArgSourceIsBorrow returns false) and still compiles; an `owned`
// return type also exempts the method (the container-ownership transfer is then
// explicit). This is the definition-site rejection that closes the laundering
// hole. // per §28.3, §28.6, §5.11
void MethodOwnership::checkBorrowReturnedThroughOwningType(const ast::Expr& e) {
    // Only owning-collection / owning-wrapper return types declared WITHOUT
    // `owned` are at risk; everything else keeps its existing behaviour.
    if (returnOwned_) return;
    if (!returnType_ || returnType_->kind != ast::TypeKind::Named ||
        !returnType_->name || returnType_->name->parts.empty())
        return;
    std::string head(returnType_->name->parts.front());
    if (!isOwningTypeNameHead(head)) return;
    if (!isOwningCollectionNameHead(head)) {
        // Owning WRAPPER (OwningTask, Shared, Weak, ChannelSender,
        // IOwningEnumerable, CancellationTokenSource): consumer-owns semantics are
        // unchanged by Part C — a non-`owned` wrapper return is an OWNED producer
        // (the caller disposes it), and a BORROW returned that way is laundered
        // into a second owner (the #1007 double-free). Reject the borrow; allow
        // every genuine owned producer (`new`, `move`, a call returning owned).
        // This is the pre-Part-C #1007 behaviour, retained for the wrappers.
        bool isBorrow = ownedArgSourceIsBorrow(e);
        if (!isBorrow) {
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
                if (dynamic_cast<sema::FieldSymbol*>(ctx_.exprSymbol(*ma)))
                    isBorrow = true;
            } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
                if (dynamic_cast<sema::FieldSymbol*>(ctx_.exprSymbol(*id)))
                    isBorrow = true;
            } else if (dynamic_cast<const ast::IndexExpr*>(&e)) {
                isBorrow = true;
            }
        }
        if (!isBorrow) return;
        report(
            Severity::Error, e.span.begin,
            std::string("a borrow may not be returned through the owning-type "
                        "return `") + head +
            "<…>` — the caller would track the returned value as owned and "
            "dispose it, double-freeing the borrow's actual owner (§28.3 / "
            "§5.11); return a freshly-produced or `move`d owned value, declare "
            "the return type `owned`, or use a borrowing return type");
        return;
    }
    // §28.6 / §5.11 (#1008 Part C): a non-`owned` owning-collection return is a
    // BORROW (the `Owned*` name denotes ELEMENT ownership, orthogonal to the
    // container ownership the `owned` qualifier governs). The one admissible
    // borrow form is a bare read of an `owned` owning-collection field of THE
    // RECEIVER (`this`) — `return this` for a field, source the receiver, which
    // the caller holds across the call. The borrowed result carries the read-
    // only capability restriction ([§28.6](#sec-borrowed-owning-collections)) at
    // its use sites. A field of ANOTHER object (`obj._items`, `local._items`)
    // has a source that need not outlive the call (a scope-bound local would
    // dangle — a use-after-free); the borrow-lifetime checker does not catch
    // this for the owning-collection builtins (no `declaration`), so it is
    // rejected here. Everything else is rejected below. // per §28.6, §28.3, §5.11
    if (exprIsOwnedFieldRead(e)) {
        // Restrict the allowance to a RECEIVER field: a bare identifier
        // (implicit `this.f`) or an explicit `this.f`.
        if (dynamic_cast<const ast::IdentifierExpr*>(&e)) return;  // implicit this.f
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e))
            if (auto* tid =
                    dynamic_cast<const ast::IdentifierExpr*>(ma->target.get()))
                if (tid->name == "this") return;  // this.f
        // A field of another object — fall through to the borrow rejection.
    }
    // `move field` is the field-TRANSFER form — diagnosed by
    // checkOwnedFieldReturn (#1005, "declare the return type `owned`"); skip
    // here to avoid double-reporting it.
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e))
        if (un->op == "move") return;
    // A borrowed PARAMETER or LOCAL, or a field of another object — a borrow
    // whose source does not outlive the call
    // (`Identity(a: OwningArray<T>): OwningArray<T> => a`, or `return local._items`).
    if (ownedArgSourceIsBorrow(e) || exprIsOwnedFieldRead(e)) {
        report(
            Severity::Error, e.span.begin,
            std::string("a borrowed `") + head +
            "<…>` value may not be returned — its source does not outlive the "
            "call; return a bare owning-collection field of the receiver "
            "(`this`), or declare the return type `owned` to transfer ownership "
            "of a freshly-produced value (§28.3 / §28.6 / §5.11)");
        return;
    }
    // Otherwise: a freshly-produced value (`new`, an owned-returning call)
    // returned through a non-`owned` owning return. #1008 Part C tightening —
    // the caller cannot distinguish a fresh owned value from a borrow by the
    // return signature, so a non-`owned` owning-collection return ALWAYS denotes
    // a borrow; a fresh owning value must be returned `owned` (without it the
    // caller would treat the fresh value as a borrow and never dispose it — a
    // leak). // per §28.6, §5.11
    report(
        Severity::Error, e.span.begin,
        std::string("a freshly-produced `") + head +
        "<…>` value must be returned through an `owned` return type (declare `: "
        "owned " + head +
        "<…>`); a non-`owned` owning-collection return is a borrow of a field, "
        "which the caller may not dispose (§28.6 / §5.11)");
}

// §28.6 *Borrowed owning collections* (#1008 Part C). A BORROW of an owning
// collection grants read access only: the ownership-transferring and disposing
// operations are unavailable, because the borrower is not the container's owner
// and must not dispose it (a double-free of the owner's container) nor change
// which elements the owner will dispose. The current scope's OWNED owning
// collections keep their full API — a `this`-field (the owner managing its own
// field, §28.7), or a local / parameter currently in the Owned state. // per §28.6
void MethodOwnership::checkBorrowedOwningCollectionCapability(
    const ast::MemberAccessExpr& callee) {
    // The one MEMORY-unsafe operation on a borrowed owning collection is
    // `Dispose`: it frees the container and cascades to its elements, which the
    // actual owner ALSO frees at scope/destructor end — a double-free. The
    // mutation / transfer operations (`Add`, `Drain`, `Pop*`, `Remove*`,
    // `Clear`, indexer-set) are memory-safe on a borrow — they preserve
    // dispose-exactly-once (a transferred element is owned by its new holder;
    // the owner then disposes a smaller/empty container) — and Bezel already
    // admits them on a borrowed collection (the pass-and-fill `a.Add(...)`
    // pattern where `a` is a borrowed `OwningArray<T>` parameter, §28.8). So only
    // `Dispose` is restricted here; the rest follow normal borrowed-collection
    // semantics. // per §28.6
    if (callee.member != "Dispose") return;
    const ast::Expr* recv = callee.target.get();
    if (!recv) return;
    // The receiver must statically be an owning-collection type (OwningArray, …);
    // the same operations on a plain Array<T>/List<T> or a borrowed plain class
    // are unaffected.
    const sema::TypeRef* rt = ctx_.exprType(*recv);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->astNode ||
        !rt->astNode->name || rt->astNode->name->parts.empty())
        return;
    if (!isOwningCollectionNameHead(rt->astNode->name->parts.front())) return;
    // Owned by the current scope → full API.
    sema::Symbol* rsym = ctx_.exprSymbol(*recv);
    if (dynamic_cast<sema::FieldSymbol*>(rsym)) {
        // A field accessed through the CURRENT object — bare `_items` (implicit
        // `this`) or `this._items` — is the owner's own field (§28.7). A field
        // of ANOTHER object (`obj._items`) is an external borrow → reject.
        if (dynamic_cast<const ast::IdentifierExpr*>(recv)) return;
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(recv))
            if (auto* tid =
                    dynamic_cast<const ast::IdentifierExpr*>(ma->target.get()))
                if (tid->name == "this") return;
    } else if (rsym) {
        // An owned LOCAL (`using var` / `var a := new OwningArray()`) or an
        // `owned` PARAMETER is in the Owned state in `states_`; a borrowed
        // local/param (e.g. `var items := h.GetItems()`) is Borrowed → reject.
        auto it = states_.find(rsym);
        if (it != states_.end() && it->second.state == OwnershipState::Owned)
            return;
    }
    // else: a call result (`h.GetItems().Drain()`) or other expression — a
    // borrow with no owner in this scope → reject.
    report(
        Severity::Error, callee.span.begin,
        "cannot call `Dispose` on a borrowed owning collection — its owner "
        "disposes the container and its elements; disposing it here would "
        "double-free. The owner (a `using var`, an owned local, or the object "
        "whose field this is) disposes it (§28.6 Borrowed owning collections).");
}

// §28.6 (#1005): "An owned field's value may not be returned from a method
// unless the return type is also declared `owned`" (Bezel_Language_Spec.md
// L5066; canonical error at L5071, `return _transactions; // ERROR: cannot
// leak owned field`). The leak is the `move` form:
//
//   `move this.f` / `move f` — `move` reads the owned field's *value* as an
//   owned value (§28.8.1, "the only way to read an owned field's value as an
//   owned value") and nulls the field, so the enclosing object's destructor
//   disposes nothing. Returned through a NON-`owned` (borrow) return type, the
//   moved value escapes the body with no owner — a leak (the #1005 repro
//   `Take(): Cell? => move v;`).
//
// SCOPED TO THE `move` FORM. A BARE read of an owned field (`return this.f;` /
// `=> f;`) is NOT caught here: per §28.6's "Methods returning `this`" rule a
// method with a non-`owned` class return type returns a BORROW of the field —
// "a reference whose lifetime is bounded by the receiver's lifetime" — which the
// caller may not assign to an owned field or return as owned, and which the
// field's owner (the receiver's destructor) still disposes. That is a legal,
// double-free-free borrow return (codified by the cross-module `t3` canary
// `GetConn(): Connection => Conn`, which returns 55 ASan-clean). The §28.6
// example's `return _transactions` leak is the OWNING-COLLECTION case (the field
// type is an owning collection / wrapper), already rejected at
// checkBorrowReturnedThroughOwningType (#1007); a bare class-type field read is
// a borrow, not a leak. So only the `move` form — which actually transfers the
// value out — is the §28.6 violation handled here.
//
// An `owned` return type is the explicit, legal ownership-transfer form (§28.8)
// and is exempt. Scoped to owned FIELD `move`s only: an owned LOCAL return
// (`var r := new T(); return r;`) is a separate concern (#1006a, with
// materializer-interaction risk) and is deliberately NOT handled here — `move`
// of a local resolves to a LocalSymbol, not a FieldSymbol, so it does not match.
// // per §28.6
void MethodOwnership::checkOwnedFieldReturn(const ast::Expr& e) {
    // An `owned` return type is the explicit, legal transfer — exempt.
    if (returnOwned_) return;
    // Require the `move` form: `move this.f` / `move f` is a UnaryExpr("move").
    // A bare field read (no `move`) is a legal borrow return (see above) and is
    // intentionally not flagged.
    auto* un = dynamic_cast<const ast::UnaryExpr*>(&e);
    if (!un || un->op != "move" || !un->operand) return;
    // The moved operand must be a read of an `owned` FIELD of the enclosing
    // class. `exprIsOwnedFieldRead` resolves a MemberAccess `obj.f` or a bare
    // identifier `f` (implicit `this.f`) to its FieldSymbol and checks the
    // `owned` modifier; a method-scope local/parameter is not a FieldSymbol, so
    // `move localOwned` (the #1006a concern) does not match. // per §28.6
    if (!exprIsOwnedFieldRead(*un->operand)) return;
    report(
        Severity::Error, e.span.begin,
        "cannot return an owned field's `move`d value through a non-`owned` "
        "return (§28.6); `move` transfers the field's value out, leaving it with "
        "no owner — declare the return type `owned` (e.g. `: owned T`) to "
        "transfer ownership to the caller");
}

namespace {

// Top-level walker — visits every method/ctor body in the source.
void checkTopDecl(const ast::TopDecl& d, sema::SemaContext& ctx);

// Find the sema symbol that wraps a given member AST and return its
// param-list, or nullptr if the member doesn't have one (or wasn't
// matched by symbol collection).
const std::vector<sema::Symbol*>* paramSymsOf(
    const ast::Member& m, sema::SemaContext& ctx) {
    for (const auto& s : ctx.symbols()) {
        if (s->decl != &m) continue;
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(s.get()))
            return &ms->params;
        if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(s.get()))
            return &cs->params;
    }
    return nullptr;
}

// Async-readiness 3c (§30.1): true when a method's `static async ...`
// modifier list contains `async`. The parser records the literal lexemes in
// MethodDecl::modifiers; sema validates the legal combinations. Constructors,
// destructors, and operators cannot be `async` (§30.1), so only MethodDecl is
// asked. // per §30.1
bool methodIsAsync(const ast::MethodDecl& md) {
    for (std::string_view mod : md.modifiers)
        if (mod == "async") return true;
    return false;
}

void checkMember(const ast::Member& m, sema::SemaContext& ctx) {
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
        if (md->bodyKind == ast::MethodBodyKind::Block) {
            MethodOwnership mo(ctx);
            mo.run(md->stmts, paramSymsOf(m, ctx), methodIsAsync(*md),
                   /*isDestructor=*/false, md->returnType.get(),
                   md->returnOwned);
        } else if (md->bodyKind == ast::MethodBodyKind::Expression &&
                   md->exprBody) {
            // §28.3 / §5.11 (#1007): an expression-bodied method `M(): T => E`
            // is exactly `return E` for ownership purposes — but the full block
            // dataflow is not run on it. Run the narrow borrow-returned-through-
            // owning-type check so `Identity(a): OwningArray<T> => a` (the #1007
            // double-free) is rejected at its definition site, the same as the
            // block-body `return a;` form. Seed the params so a borrowed param /
            // owned param is classified correctly, then apply only this check
            // (not the full handleReturn, which would introduce unrelated new
            // diagnostics on expression bodies). // per §28.3, §5.11
            MethodOwnership mo(ctx);
            mo.runExprBodyOwningReturnCheck(
                *md->exprBody, paramSymsOf(m, ctx),
                md->returnType.get(), md->returnOwned);
        }
    } else if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(&m)) {
        MethodOwnership mo(ctx);
        mo.run(cd->body, paramSymsOf(m, ctx));
    } else if (auto* dd = dynamic_cast<const ast::DestructorDecl*>(&m)) {
        MethodOwnership mo(ctx);
        // §28.8.1 (#763): analyse the destructor body with isDestructor=true so
        // the field-restoration check is suppressed (a torn-down object's owned
        // fields may be left moved-from after a discharged `move this.f`).
        mo.run(dd->body, /*paramSyms=*/nullptr, /*isAsync=*/false,
               /*isDestructor=*/true);
    } else if (auto* od = dynamic_cast<const ast::OperatorDecl*>(&m)) {
        if (od->bodyKind == ast::MethodBodyKind::Block) {
            MethodOwnership mo(ctx);
            mo.run(od->stmts);
        }
    } else if (auto* nt = dynamic_cast<const ast::NestedTypeMember*>(&m)) {
        if (nt->decl) {
            for (const auto& innerM : nt->decl->members) {
                if (innerM) checkMember(*innerM, ctx);
            }
        }
    } else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(&m)) {
        // #926 / §28.6 (v0.153): a property/indexer GETTER may not return a
        // freshly-produced owned value (a getter is a borrow-producing position).
        // This is the only ownership check run on accessors today — a NARROW
        // getter-return check, not the full pass, so existing getters/setters
        // surface no unrelated new diagnostics. The expression-bodied read-only
        // property (`property X: T => E`) and each block-form `get` accessor
        // (`get => E` / `get => do … end`) are covered. // per §28.6
        if (pd->bodyKind == ast::PropertyBodyKind::Expression && pd->exprBody) {
            MethodOwnership mo(ctx);
            mo.checkGetterFreshOwnedReturn(*pd->exprBody);
        } else if (pd->bodyKind == ast::PropertyBodyKind::Block) {
            for (const auto& acc : pd->accessors) {
                if (acc.isSet) continue;  // getters only
                MethodOwnership mo(ctx);
                if (acc.bodyKind == ast::PropertyAccessorBodyKind::Expression &&
                    acc.exprBody) {
                    mo.checkGetterFreshOwnedReturn(*acc.exprBody);
                } else if (acc.bodyKind ==
                           ast::PropertyAccessorBodyKind::Block) {
                    mo.scanGetterReturns(acc.stmts);
                }
            }
        }
    }
}

// ---------- Slice O9: owned-field definite-assignment (§7.9) --------
//
// Per §7.9 / Appendix C.1, every non-nullable field — which owned
// class-typed fields are — must be assigned by a field initializer or
// in every constructor. This slice enforces that for `owned` fields
// specifically. A `this(...)` chained constructor transfers the
// responsibility to the chained target. `base(...)` chaining does not
// — the derived class's fields are still the derived ctor's job.
//
// This is a syntactic over-approximation: we accept any
// `this.<field> := …` assignment anywhere in the body (not necessarily
// on every CFG path). The full DA check awaits the CFG-based ownership
// refactor; for now a constructor that initializes only on some paths
// passes here but would fail under the stricter pass once landed.

// §7.9: a field of non-nullable *reference* type (class, interface,
// string, function, non-nullable Object) has no default value and must
// be assigned by a field initializer or in every constructor. Value
// types (records, enums, primitives other than string/Object, inline
// arrays, unit) have well-defined defaults and are exempt. A static
// field is exempt here (static-init rules, §22.13). Generic-parameter
// and unresolved types are conservatively treated as non-reference
// (not flagged). `owned` fields are a subset (class-typed) and are
// covered by this rule.
bool isNonNullableReferenceField(const ast::FieldDecl& f,
                                 sema::SemaContext& ctx) {
    for (auto m : f.modifiers) {
        if (m == "static") return false;
    }
    if (!f.type) return false;
    auto* rt = ctx.resolvedType(*f.type);
    if (!rt || rt->nullable) return false;
    switch (rt->kind) {
        case sema::TypeRef::Kind::Primitive:
            return rt->primitive == sema::PrimitiveType::String ||
                   rt->primitive == sema::PrimitiveType::Object;
        case sema::TypeRef::Kind::Function:
            return true;
        case sema::TypeRef::Kind::Named:
            if (!rt->declaration) return false;  // built-in/generic — skip
            return rt->declaration->kind == ast::TypeDeclKind::Class ||
                   rt->declaration->kind == ast::TypeDeclKind::Interface;
        default:
            return false;  // Unit, InlineArray, Error → value/unknown
    }
}

// True if `e` assigns a field of the current instance — either
// `this.<member>` (explicit) or a bare `<field>` that resolves to a field
// of the declaring class (implicit `this`). §7.9 requires the field to be
// "assigned"; it does not mandate `this.` qualification, and the bare form
// resolves to the field in sema and codegen alike.
bool isDirectThisFieldRef(const ast::Expr& e, std::string_view& outName,
                          sema::SemaContext& ctx) {
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
        if (!id || id->name != "this") return false;
        outName = ma->member;
        return true;
    }
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        if (auto* fs =
                dynamic_cast<sema::FieldSymbol*>(ctx.exprSymbol(*id))) {
            outName = fs->name;
            return true;
        }
    }
    return false;
}

// Returns true if the constructor body's first statement is a
// `this(...)` call (chained-ctor delegation). Per spec the chained
// target carries the field-initialization burden.
bool ctorChainsToThis(const ast::ConstructorDecl& cd) {
    if (cd.body.empty() || !cd.body.front()) return false;
    auto* es = dynamic_cast<const ast::ExpressionStmt*>(cd.body.front().get());
    if (!es || !es->expr) return false;
    auto* call = dynamic_cast<const ast::CallExpr*>(es->expr.get());
    if (!call || !call->callee) return false;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(call->callee.get());
    return id && id->name == "this";
}

// Walk the body collecting every owned-field name that gets an
// outright assignment `this.<f> := …` somewhere in it. This walks
// recursively into control-flow blocks; over-approximates by counting
// any path-assignment as definitely-assigned.
void collectThisFieldAssignments(
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    std::unordered_set<std::string_view>& assigned, sema::SemaContext& ctx);

void collectThisFieldAssignmentsStmt(
    const ast::Stmt& s,
    std::unordered_set<std::string_view>& assigned, sema::SemaContext& ctx) {
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->op == ":=" && as->target) {
            std::string_view name;
            if (isDirectThisFieldRef(*as->target, name, ctx)) {
                assigned.insert(name);
            }
        }
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        collectThisFieldAssignments(ifs->ifBranch.body, assigned, ctx);
        for (const auto& el : ifs->elifBranches)
            collectThisFieldAssignments(el.body, assigned, ctx);
        collectThisFieldAssignments(ifs->elseBody, assigned, ctx);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        collectThisFieldAssignments(ws->body, assigned, ctx);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        collectThisFieldAssignments(fs->body, assigned, ctx);
        return;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        collectThisFieldAssignments(dob->body, assigned, ctx);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        collectThisFieldAssignments(ts->body, assigned, ctx);
        for (const auto& arm : ts->catches) {
            if (arm.body)
                collectThisFieldAssignmentsStmt(*arm.body, assigned, ctx);
        }
        collectThisFieldAssignments(ts->finallyBody, assigned, ctx);
        return;
    }
}

void collectThisFieldAssignments(
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    std::unordered_set<std::string_view>& assigned, sema::SemaContext& ctx) {
    for (const auto& s : body) {
        if (s) collectThisFieldAssignmentsStmt(*s, assigned, ctx);
    }
}

void checkOwnedFieldDA(const ast::TypeDecl& td, sema::SemaContext& ctx) {
    // Gather non-nullable reference fields without initializers (§7.9).
    // Owned fields are a subset.
    std::vector<const ast::FieldDecl*> ownedNoInit;
    for (const auto& m : td.members) {
        auto* f = dynamic_cast<const ast::FieldDecl*>(m.get());
        if (!f) continue;
        if (!isNonNullableReferenceField(*f, ctx)) continue;
        if (f->initializer) continue;
        ownedNoInit.push_back(f);
    }
    if (ownedNoInit.empty()) return;
    // Collect ctors.
    std::vector<const ast::ConstructorDecl*> ctors;
    for (const auto& m : td.members) {
        if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(m.get())) {
            if (!cd->isStatic) ctors.push_back(cd);
        }
    }
    // No ctors at all: each owned-without-init field is reported at
    // its declaration site as needing initialization.
    if (ctors.empty()) {
        for (auto* f : ownedNoInit) {
            ctx.diagnostics().report(
                Severity::Error, f->span.begin,
                std::string("non-nullable field `") + std::string(f->name) +
                "` needs an initializer or a constructor that assigns "
                "it (§7.9)");
        }
        return;
    }
    // Per-ctor: walk body and check assignments cover ownedNoInit.
    for (auto* cd : ctors) {
        if (ctorChainsToThis(*cd)) continue; // delegated
        std::unordered_set<std::string_view> assigned;
        collectThisFieldAssignments(cd->body, assigned, ctx);
        for (auto* f : ownedNoInit) {
            if (!assigned.count(f->name)) {
                ctx.diagnostics().report(
                    Severity::Error, cd->span.begin,
                    std::string("constructor does not initialize "
                                "non-nullable field `") +
                    std::string(f->name) +
                    "` (§7.9 — every constructor's normal-exit path "
                    "must assign it, or chain to `this(…)`)");
            }
        }
    }
}

void checkTopDecl(const ast::TopDecl& d, sema::SemaContext& ctx) {
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(&d)) {
        // O9: per-class owned-field DA check (classes only).
        if (td->kind == ast::TypeDeclKind::Class) {
            checkOwnedFieldDA(*td, ctx);
        }
        for (const auto& m : td->members) {
            if (m) checkMember(*m, ctx);
        }
    } else if (auto* ext = dynamic_cast<const ast::ExtensionDecl*>(&d)) {
        for (const auto& m : ext->members) {
            if (m) checkMember(*m, ctx);
        }
    }
}

} // namespace

bool checkOwnership(const ast::SourceFile& src, sema::SemaContext& ctx) {
    for (const auto& d : src.topDecls) {
        if (d) checkTopDecl(*d, ctx);
    }
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::ownership
