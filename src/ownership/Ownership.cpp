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

void MethodOwnership::run(const std::vector<std::unique_ptr<ast::Stmt>>& body,
         const std::vector<sema::Symbol*>* paramSyms) {
    // Seed the dataflow entry state with `owned` / `out owned` parameters
    // (slice O10): each enters the method in the Owned state and must
    // reach a discharge point on every normal-exit path (§28.8).
    BlockState entry;
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
                entry[sym] = lo;
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
    BlockState endState = runDataflow(*cfg, entry);
    // §28.3 "Owned-local discharge requirement": an owned local must reach
    // an owned slot (or be disposed) before it goes out of scope on a
    // *normal-exit* path. The exception-path discharge is handled inside the
    // dataflow (runDataflow / computeEntry): a value live-and-owned on an
    // exception edge is auto-disposed by the implicit try/finally during
    // stack unwinding, so the meet at any exception successor — and
    // ultimately at the function exit — sees it Disposed, not Owned. Hence
    // the check here fires only for locals that survive Owned to a
    // normal-exit edge.
    for (const auto& [sym, st] : endState) {
        if (st.state == OwnershipState::Owned && !st.implicitDischarge) {
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
    // Slice O8 / §28.8.1: a `move this.field` leaves the field in
    // moved-from state. The field must be reassigned before the
    // surrounding method exits — otherwise the field's invariant
    // is broken on method return. Per §28.8.1 "Exception paths are not
    // exempt", this is NOT covered by the §28.3 exception-path discharge:
    // even when the method exits via a throw, the destructor on `this`
    // would observe the field moved-from during unwinding, so the check
    // fires unconditionally (path-insensitively) here.
    for (const auto& [name, moved] : fieldMovedFrom_) {
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

// Collect every LocalDecl AST node declared (directly or in a nested
// block) within a statement list, into `out`. Used to scope §28.3
// exception-path discharge to exactly the locals whose scope the unwind
// destroys (a try body's own locals), leaving enclosing-scope locals
// untouched — they remain live in the catch and after the try.
void collectLocalDecls(const std::vector<std::unique_ptr<ast::Stmt>>& body,
                       std::unordered_set<const ast::Node*>& out);

void collectLocalDeclsStmt(const ast::Stmt& s,
                           std::unordered_set<const ast::Node*>& out) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        out.insert(ld);
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        collectLocalDecls(ifs->ifBranch.body, out);
        for (const auto& el : ifs->elifBranches) collectLocalDecls(el.body, out);
        collectLocalDecls(ifs->elseBody, out);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
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

BlockState MethodOwnership::runDataflow(
    const flow::CFG& cfg, const BlockState& entry) {
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

    std::unordered_map<const flow::CFGBlock*, BlockState> exitState;
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
                            bool emitOnMerge) -> BlockState {
        BlockState in;
        bool haveIn = false;
        if (b == cfg.entry()) {
            in = entry;
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
        auto foldPred = [&](const flow::CFGBlock* p, bool excEdge) {
            if (!evaluated.count(p)) return;
            const bool loopEdge =
                backEdges.count({p, b}) != 0 || isLoopSkipEdge(p, b);
            const BlockState& peRaw = exitState[p];
            BlockState peDischarged;
            if (excEdge)
                peDischarged =
                    dischargeOnExceptionEdge(peRaw, unwoundDeclsFor(b));
            const BlockState& pe = excEdge ? peDischarged : peRaw;
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
            if (!flow::CFG::isExceptionEdge(p, b)) foldPred(p, false);
        for (const flow::CFGBlock* p : b->preds)
            if (flow::CFG::isExceptionEdge(p, b)) foldPred(p, true);
        return in;
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
            states_ = computeEntry(b, /*emitOnMerge=*/false);
            processBlock(*b);
            auto it = exitState.find(b);
            if (it == exitState.end() || !sameState(it->second, states_)) {
                changed = true;
            }
            exitState[b] = states_;
            evaluated.insert(b);
        }
        if (!changed) break;
    }

    // --- O3b final emit pass (diagnostics enabled) ------------------------
    //
    // The fixpoint above left `exitState` stable. Walk the RPO one last
    // time with diagnostics on, so each block's reads/moves/disposals and
    // branch-join clashes are reported exactly once, against the converged
    // predecessor exit states. Reset the per-method field-move tracker so
    // it reflects exactly this single walk (the suppressed fixpoint passes
    // toggled it repeatedly).
    suppressDiag_ = false;
    fieldMovedFrom_.clear();
    fieldMoveLoc_.clear();
    for (const flow::CFGBlock* b : post) {
        states_ = computeEntry(b, /*emitOnMerge=*/true);
        processBlock(*b);
        exitState[b] = states_;
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
    bool haveEnd = false;
    auto foldExitPred = [&](const flow::CFGBlock* p, bool excEdge) {
        if (!evaluated.count(p)) return;
        const BlockState& peRaw = exitState[p];
        BlockState peDischarged;
        if (excEdge)
            peDischarged =
                dischargeOnExceptionEdge(peRaw, unwoundDeclsFor(cfg.exit()));
        const BlockState& pe = excEdge ? peDischarged : peRaw;
        if (!haveEnd) {
            endState = pe;
            haveEnd = true;
        } else {
            joinInto(endState, pe, SourceLocation{}, /*emitClash=*/false,
                     /*unionKeys=*/true);
        }
    };
    // Normal-exit predecessors first (same rationale as computeEntry: a
    // normal-path Owned obligation must seed and survive the union with the
    // exception edges that dropped the local).
    for (const flow::CFGBlock* p : cfg.exit()->preds)
        if (!flow::CFG::isExceptionEdge(p, cfg.exit())) foldExitPred(p, false);
    for (const flow::CFGBlock* p : cfg.exit()->preds)
        if (flow::CFG::isExceptionEdge(p, cfg.exit())) foldExitPred(p, true);
    if (!haveEnd) {
        // No path reaches exit. Fall back to the entry-block exit state.
        auto it = exitState.find(cfg.entry());
        if (it != exitState.end()) endState = it->second;
    }
    return endState;
}

void MethodOwnership::processBlock(const flow::CFGBlock& block) {
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
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) handleDischarge(*ds->value);
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
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type)) {
                if (rt->kind == sema::TypeRef::Kind::Named &&
                    rt->declaration &&
                    rt->declaration->kind != ast::TypeDeclKind::Class &&
                    rt->declaration->kind != ast::TypeDeclKind::Interface) {
                    return OwnershipState::Borrowed;  // value type: no obligation
                }
            }
        }
        return OwnershipState::Owned;
    }
    // `null` is the polymorphic-null state (§28.3 null-literal row).
    if (dynamic_cast<const ast::NullLiteralExpr*>(&e)) {
        return OwnershipState::PolyNull;
    }
    // Slice O13: a call to a method with an `owned T` return type
    // produces an Owned value. The callee MethodSymbol resolution
    // comes from sema; we look at its MethodDecl::returnOwned to
    // decide. A null callee symbol (overload not yet pickable, or
    // a call on a value with no symbol) falls through to Borrowed.
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (ce->callee) {
            if (auto* sym = ctx_.exprSymbol(*ce->callee)) {
                if (auto* ms = dynamic_cast<sema::MethodSymbol*>(sym)) {
                    auto* md = dynamic_cast<const ast::MethodDecl*>(
                        ms->decl);
                    if (md && md->returnOwned) {
                        return OwnershipState::Owned;
                    }
                }
            }
        }
    }
    // Default: Borrowed.
    return OwnershipState::Borrowed;
}

void MethodOwnership::handleLocalDecl(const ast::LocalDecl& ld) {
    // Find the LocalSymbol that ExprTypecheck created for this decl.
    sema::LocalSymbol* lsym = findLocalSymbol(ld);
    if (!lsym) return;
    LocalOwnership lo;
    lo.obligationOrigin = &ld;
    if (ld.isUsing) {
        // `using var` is always Owned and implicitly discharged.
        lo.state = OwnershipState::Owned;
        lo.implicitDischarge = true;
    } else if (ld.initializer) {
        lo.state = producerState(*ld.initializer);
    } else {
        lo.state = OwnershipState::Uninitialized;
    }
    states_[lsym] = lo;
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
    // §28.3 (#386 cases 1+2): storing a borrow of an owned scope-bound local
    // into a field of an object, or writing it through an out/ref parameter,
    // lets the borrow outlive the local (use-after-dispose). Run AFTER
    // recordMovesIn so a `move`d source is no longer Owned and the transfer
    // form is correctly not flagged.
    checkBorrowStoreEscape(as);
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
    checkIndirectEscapeOnReturn(e);       // §28.9/§9.7 indirect-via-local
    // For slice O1, treat `return x;` (bare identifier) as a
    // discharge. The "is the method's return type owned T?" check
    // is sema's; here we just satisfy the discharge requirement
    // pessimistically.
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
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

void checkMember(const ast::Member& m, sema::SemaContext& ctx) {
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
        if (md->bodyKind == ast::MethodBodyKind::Block) {
            MethodOwnership mo(ctx);
            mo.run(md->stmts, paramSymsOf(m, ctx));
        }
    } else if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(&m)) {
        MethodOwnership mo(ctx);
        mo.run(cd->body, paramSymsOf(m, ctx));
    } else if (auto* dd = dynamic_cast<const ast::DestructorDecl*>(&m)) {
        MethodOwnership mo(ctx);
        mo.run(dd->body);
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
    }
    // PropertyDecl accessors and other forms join in later O slices.
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
