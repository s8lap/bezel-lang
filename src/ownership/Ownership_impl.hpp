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

// §28.8.1 (#867): the per-block dataflow lattice for the field-restoration
// check. A `move` of an owned field leaves the field moved-from; the field
// must be RESTORED (reassigned a value, incl. `null` for a nullable owned
// field) on EVERY control-flow path from the move to method exit. Earlier the
// moved-from facts lived in a flat member map mutated linearly as statements
// were visited, so a `move` restored on only SOME paths was wrongly accepted:
// the map is not part of `BlockState`, so it never participated in the CFG
// meet at joins. `FlowState` bundles the locals lattice WITH the field-moved-
// from facts so both flow through `runDataflow`/`computeEntry` together (the
// locals merged via `joinInto`, the fields via `joinFieldsInto`). The field
// facts merge with the OPPOSITE polarity to the locals:
// a field is moved-from at a join iff it is moved-from on ANY incoming path
// (equivalently RESTORED iff restored on ALL paths) — an unrestored field on
// even one path must be reported at exit. // per §28.8.1, #867
using FieldMovedFrom = std::unordered_map<std::string_view, bool>;

// §28.8.1 (#867): the full per-block dataflow state — the locals lattice plus
// the field-moved-from facts, carried together so both participate in the CFG
// meet (the locals via the §28.3 `joinInto`, the fields via the OR-over-paths
// `joinFieldsInto`).
struct FlowState {
    BlockState locals;
    FieldMovedFrom fields;
};

class MethodOwnership {
public:
    MethodOwnership(sema::SemaContext& ctx) : ctx_(ctx) {}

    // Run the linear walk. `body` is the method/ctor/lambda body's
    // statement list. `paramSyms` (slice O10) is the matching list of
    // ParameterSymbols — `owned` params are introduced as Owned locals
    // at method entry per §28.8 and must reach a discharge point on
    // every normal-exit path (same rule as `var x := producer-of-owned;`).
    //
    // Async-readiness 3c: `isAsync` is true when the enclosing method/lambda
    // is declared `async`. It gates the additive at-await-site Pattern A / B
    // checks (§30.5) and the across-suspend SuspendLocal annotation — all of
    // which fire ONLY on async bodies, so a non-async body's diagnostics are
    // byte-for-byte unchanged. // per §30.5, §30.1
    void run(const std::vector<std::unique_ptr<ast::Stmt>>& body,
             const std::vector<sema::Symbol*>* paramSyms = nullptr,
             bool isAsync = false, bool isDestructor = false,
             const ast::Type* returnType = nullptr, bool returnOwned = false);

    // §28.3 / §5.11 (#1007): apply ONLY the borrow-returned-through-owning-type
    // check to an expression-bodied method `M(): T => exprBody` (which the full
    // block dataflow above is not run on). Seeds the parameters so a borrowed
    // param is classified correctly, then runs `checkBorrowReturnedThroughOwning
    // Type(exprBody)` — nothing else, so no unrelated diagnostics are introduced
    // on expression bodies. // per §28.3, §5.11
    void runExprBodyOwningReturnCheck(
        const ast::Expr& exprBody,
        const std::vector<sema::Symbol*>* paramSyms,
        const ast::Type* returnType, bool returnOwned);

    // #926 / §28.6 (v0.153): a property/indexer GETTER may not return a freshly-
    // produced owned value — a getter is a borrow-producing position (properties
    // and indexers do not carry `owned`), so a fresh owned value would escape with
    // no owner and leak. checkGetterFreshOwnedReturn reports when `e` is a fresh
    // owned producer (producerState == Owned). scanGetterReturns / -Stmt walk a
    // block-form getter accessor body for `return <fresh owned>` statements (nested
    // in if/while/for/try/do). These run a NARROW check only (not the full
    // ownership pass), so accessor bodies surface no other new diagnostics.
    void checkGetterFreshOwnedReturn(const ast::Expr& e);
    void scanGetterReturns(const std::vector<std::unique_ptr<ast::Stmt>>& stmts);
    void scanGetterReturnsStmt(const ast::Stmt* s);

    // #1484 / §28.6: a field INITIALIZER (`f: T := E`) that stores a freshly-
    // produced owned value (producerState == Owned) into a NON-`owned` (borrow)
    // field is an error — the field does not own its referent, so the fresh value
    // is left with no owner and leaks. This is the field-initializer counterpart
    // of the constructor-body assignment reject (handleAssignment's
    // targetIsNonOwnedField branch); the two positions enforce the identical
    // §28.6 rule via the same producerState classifier and owned-modifier gate. A
    // NARROW check only (not the full ownership pass), so a legal borrow
    // initializer surfaces no other new diagnostics, and an `owned` field's fresh
    // initializer stays accepted (§28.5). // per §28.6
    void checkFieldInitializer(const ast::FieldDecl& fd);

    // #1066 / §9.7 (v0.153) + §28.3: a borrow-bearing record may not be
    // constructed from a TEMPORARY bound to one of its borrow-carrying fields —
    // the temporary's lifetime ends at the construction expression, so the record
    // immediately outlives that borrow's source (a dangling borrow). Reject at
    // `new Record(...)` when the record is borrow-bearing and a positional arg
    // bound to a borrow-carrying field is a temporary (not a named place). Keeps
    // long-lived-borrow construction (`var xs := […]; new Bag(xs)`) legal.
    void checkRecordFromTemporary(const ast::NewExpr& ne);
    // True when `ft` is a borrow-carrying field type (a class/interface
    // reference — including the BUILTIN heap collections Array/Dictionary/… that
    // carry no user TypeDecl — an inline array of references, or a transitively
    // borrow-bearing record). Used by checkRecordFromTemporary; intentionally
    // distinct from isBorrowBearingRecord's own predicate (which is left
    // unchanged, so the #360 return-escape behaviour does not shift).
    bool fieldTypeIsBorrowCarrying(const sema::TypeRef* ft) const;
    // True when `e` is a temporary (not a named place — identifier / member
    // access / index slot — whose storage can outlive the construction).
    bool exprIsTemporary(const ast::Expr& e) const;

private:
    sema::SemaContext& ctx_;
    std::unordered_map<sema::Symbol*, LocalOwnership> states_;
    // Async-readiness 3c: true while analysing an `async` method/lambda body.
    // Gates the at-await-site Pattern A/B checks and the SuspendLocal
    // annotation so they never affect a synchronous body. // per §30.5
    bool isAsync_ = false;
    // §28.8.1 (#763): true while analysing a DESTRUCTOR body. Inside a
    // destructor the field-restoration rule does NOT require reassigning a
    // moved-from owned field — the object is being torn down and no future read
    // of the field occurs ("the field-restoration rule does not require
    // reassignment inside a destructor"). Gates the field-moved-from-at-exit
    // check so a `move this.f` whose taken value is otherwise discharged is not
    // spuriously rejected. // per §28.8.1
    bool isDestructor_ = false;
    // §28.3 / §28.6 / §5.11 (#1007): the enclosing method's DECLARED return type
    // and whether it was declared `owned`. An owning-collection / owning-wrapper
    // return type (`OwningArray<T>`, `OwningTask<T>`, `Shared<T>`, …) names ELEMENT
    // ownership (§5.11); the CONTAINER-ownership axis is the separate `owned`
    // qualifier. A method declared `: OwningArray<T>` (no `owned`) that hands back
    // a BORROW (a non-owned param, a borrowed local, a plain field read) launders
    // that borrow into a value the caller tracks as owned — a double-free. The
    // return check (handleReturn) consults these to reject that case. Null/false
    // for constructors, destructors, operators, lambdas, and non-owning returns.
    // // per §28.3, §28.6, §5.11
    const ast::Type* returnType_ = nullptr;
    bool returnOwned_ = false;
    // §29.3 / §29.8 (#945): true when this method's body uses `yield` (it is an
    // iterator method). A SYNC iterator (sawYield_ && !isAsync_) whose `owned`
    // parameter is never moved out gets §29.8 state-machine auto-dispose instead
    // of the §28.3 normal-exit drop reject. Set at the start of run().
    bool sawYield_ = false;
    // §29.8 (#945): symbols (params or locals) discharged — moved-from or
    // disposed — on AT LEAST ONE path during the dataflow. Recorded by
    // markMovedByName / markDisposedByName. Used by the end-of-method check to
    // distinguish an iterator's owned parameter that is NEVER discharged (safe to
    // auto-dispose unconditionally) from one discharged on some path (kept as a
    // reject — the state machine cannot statically know whether the body moved it
    // out, so auto-disposing would risk a double-free). Monotone (grows only), so
    // it is fixpoint-safe across the dataflow's block re-evaluations.
    std::unordered_set<sema::Symbol*> everDischarged_;
    // §28.3 (#1257): every plain (non-`using`) `var` local that a LocalDecl
    // transfer classified as Owned at its declaration. Recorded so the end-of-
    // method auto-drop can flag a nested-scope owned local that is NOT present in
    // the function-exit `endState` — one declared in an inner block that exits
    // normally before the function reaches a throw-only exit (its scope ended, so
    // it is not live at the function exit, and when the sole exit is a throw the
    // exit state may be empty entirely). Such a local is never flagged by the
    // endState loop, and codegen's fallback unwind-only disposal cannot fire (the
    // inner scope was popped before the outer throw unwinds) → leak. Monotone
    // (grows only) like `everDischarged_`, so it is fixpoint-safe.
    std::unordered_set<sema::Symbol*> everOwnedPlainLocals_;
    // §29.2 / §28.3 v0.157 (#1329): every owned iteration variable seeded by
    // seedOwnedIterVar for the CURRENT method. Used by the post-endState
    // break-body check: a seeded iter var absent from endState AND never
    // discharged (everDischarged_.count == 0) leaked via a break-only body —
    // the break edge is not a loopEdge so item=Owned is not propagated to the
    // after block via unionKeys=true. Monotone (grows only), fixpoint-safe.
    std::unordered_set<sema::Symbol*> seededIterVarSyms_;
    // §28.3 / #1387: every `out owned var` INLINE local whose declaring call
    // appears in a WHILE-LOOP CONDITION (block.isLoopHeader == true). While-
    // condition items are potentially overwritten on each iteration (the slot is
    // shared across iterations via ensureOutVarSlots' alreadyMapped check), so
    // an intermediate value that was not disposed before the next iteration leaks.
    // Auto-drop can only dispose the FINAL slot value → incomplete. These symbols
    // are excluded from the §28.3 affine auto-drop (undischarged keeps the hard
    // error). If-condition and statement-level `out owned var` locals are NOT in
    // this set — they are written at most once, making auto-drop exactly-once-safe.
    // Monotone (grows only), fixpoint-safe.  // per §28.3, §22.4
    std::unordered_set<sema::Symbol*> loopCondOutOwnedSyms_;
    // Issue #392 / §31.1: the set of catch-bound exception variables in this
    // method (`var ex: T` arm bindings). Per §31.1 the binding is a BORROW of
    // the in-flight exception whose lifetime is the arm body: it may be passed
    // to other methods (a borrow for the call's duration) and read, but may
    // NOT be assigned to an `owned` field, returned as `owned`, or captured by
    // a closure that may outlive the arm — the exception instance cannot
    // escape the arm that caught it (the bare re-throw `throw;` is the one
    // allowed exception). The binding is never declared via a `LocalDecl`, so
    // it lives in this set rather than in `states_`; it carries no discharge
    // obligation (the runtime owns and disposes the exception), so seeding it
    // into `states_` as Owned would mis-fire the end-of-method undischarged
    // check. The escape machinery consults this set via `isCatchBorrow` /
    // `isEscapeBoundLocal` to treat an escape of the binding (or of a local
    // that re-borrows it) exactly as it treats an escape of an owned scope-
    // bound local. Populated at the start of run() by scanning this method's
    // catch arms; symbols are unique per arm, so the set is path-insensitive
    // (a catch binding only appears within its own arm body). // per §31.1
    std::unordered_set<sema::Symbol*> catchBorrows_;
    // §28.3 / §28.9 (#861): block-nesting depth of each local's declaration,
    // keyed by its `LocalDecl` AST node (== `LocalSymbol::decl`). The method
    // body is depth 0; each nested do-end / if-branch / loop / try-arm body adds
    // one. Populated at the start of run(). Used by checkBorrowStoreEscape to
    // decide whether a closure assigned to a plain local escapes: the target
    // outlives the captured source iff its declaring block is strictly
    // shallower (smaller depth) — sound because both are in scope at the
    // assignment, so their blocks lie on a single nesting path.
    std::unordered_map<const ast::Node*, int> localDepth_;
    // Slice O8 / §28.8.1 (#867): the CURRENT block's `move this.field`
    // tracker — the field's name maps to its moved-from state. The transfer
    // functions (recordMovesIn / handleDischarge mark; handleAssignment clear)
    // mutate this map exactly as they mutate `states_` for locals. The CFG
    // driver (runDataflow) seeds it from the block's merged entry field facts
    // before each block and snapshots it into the block's `FlowState` exit
    // after, so the facts participate in the §28.8.1 join (moved-from on ANY
    // path → reported at exit). fieldMoveLoc_ stores the move-site location for
    // diagnostics (path-insensitive: last move site per field name wins; it
    // only fixes the reported error location). // per §28.8.1, #867
    FieldMovedFrom fieldMovedFrom_;
    std::unordered_map<std::string_view, SourceLocation> fieldMoveLoc_;

    // §28.3 / #892: for loop-header Branch blocks, the ownership state
    // BEFORE the condition expression is evaluated. A while condition
    // `s.TryPop(out owned item)` marks `item` Owned in the post-condition
    // exit state (which flows to the body-entry on the true edge). But on
    // the false (skip) exit — when TryPop returns false and `item` was not
    // written — the local should still carry its pre-condition state (e.g.
    // null/Disposed from the prior body iteration), not the Owned state that
    // the post-condition exit has. This map stores, for each loop header,
    // the locals (and fields) state captured just before processBlock calls
    // handleDischarge on the condition so computeEntry can use it when
    // folding the header→after skip edge. Populated by processBlock; consumed
    // by computeEntry's foldPred lambda. // per §28.3, #892
    std::unordered_map<const flow::CFGBlock*, FlowState> loopSkipExitState_;

    // §28.3 / #1407: for each non-loop if/elif Branch block whose condition
    // declares `out owned var` inline locals, the set of those symbols. On the
    // FALSE edge (TryPop returned false → the local was NOT written), those
    // symbols retain their pre-condition state — null/absent, NOT the Owned
    // state handleDischarge marks unconditionally. foldPred in computeEntry
    // demotes them to PolyNull on the false edge so a discharge inside the
    // then-branch alone correctly satisfies the obligation:
    //   then-branch exit: Disposed ⊓ false-edge: PolyNull = Disposed (OK)
    // vs. the pre-fix wrong meet:
    //   then-branch exit: Disposed ⊓ false-edge: Owned = Owned (spurious).
    // Monotone (grows only), fixpoint-safe. // per §28.3, #1407
    std::unordered_map<const flow::CFGBlock*,
                       std::unordered_set<sema::Symbol*>> ifCondOutOwnedSyms_;

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
    // owned-IN transfer based on the callee's formal parameter list — true
    // only for a plain `owned` formal (the argument is consumed). An
    // `out owned` formal is the owned-OUT direction (the argument local
    // RECEIVES ownership) and is NOT an implicit owned sink (#860).
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

    // §15.3 (#1549): a straight-line, program-order pre-pass (mirrors codegen's
    // MoveOnlyArrayTransferWalker's borrow/transfer classification of an
    // identifier occurrence, but for diagnostic purposes rather than
    // disposal-eligibility) that tracks, per move-only inline-array LOCAL
    // name, which slots are provably moved-from at each point in the body.
    // At a whole-value reassignment `arr := newValue;` reachable while some
    // slot is not provably moved-from, emits the §15.3-mandated warning
    // naming the at-risk slots. Runs before the CFG dataflow, like the other
    // syntactic pre-passes below (collectCatchBorrows etc.) — it does not
    // participate in branch-join precision (a conditionally-executed
    // `move arr[i]` is treated as if it always executes, the same
    // conservative simplification codegen's walker already makes for
    // disposal eligibility), so it may under- or over-warn on branchy code;
    // the mandate is a best-effort warning, not a soundness-critical check
    // (#1385 tracks the real fix: disposing the displaced slots so nothing
    // leaks in the first place). // per §15.3
    void moveOnlyArrayLeakScan(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void moveOnlyArrayLeakScanStmt(const ast::Stmt* s);
    void moveOnlyArrayLeakScanExpr(const ast::Expr* e, bool borrow);
    // `move arr[CONST_I]` with a compile-time-constant index proves slot
    // CONST_I moved-from; a non-constant index proves nothing (conservative).
    void moveOnlyArrayNoteSlotMoved(const ast::IndexExpr& ix);
    // A slot store `arr[CONST_I] := RHS;`: `null` proves the slot empty (no
    // owned value to leak — same standing as a proven-moved slot); any other
    // RHS re-populates the slot with a live owned value (at risk again).
    void moveOnlyArrayNoteSlotStore(const ast::AssignmentStmt& as);
    // A bare array-local identifier read in a non-borrow (owned-consuming)
    // position transfers the WHOLE array out of `name` — nothing is left in
    // this binding to leak, so it drops out of tracking.
    void moveOnlyArrayNoteWholeTransfer(std::string_view name);
    // The check point: `arr := newValue;` where `arr` names a move-only
    // inline-array local. Warns when some slot is provably still live, then
    // resets tracking to reflect the fresh (fully-populated) RHS value.
    void moveOnlyArrayCheckReassignment(const ast::AssignmentStmt& as);
    bool isMoveOnlyInlineArrayType(const sema::TypeRef* rt,
                                   long long& sizeOut) const;
    struct MoveOnlyArraySlots {
        long long size = 0;
        std::unordered_set<long long> movedSlots;  // provably moved-from
    };
    std::unordered_map<std::string, MoveOnlyArraySlots> moveOnlyArrayLive_;

    void handleReturn(const ast::Expr& e);

    // §28.3 / §28.6 / §5.11 (#1007): reject returning a BORROW through an
    // owning-collection / owning-wrapper return type (`OwningArray<T>`,
    // `OwningTask<T>`, `Shared<T>`, …) declared WITHOUT `owned`. Such a return
    // type names element ownership, not container ownership: a method that hands
    // back a borrowed value (a non-owned param, a borrowed local, a plain field
    // read) launders it into a value the caller tracks as owned — a double-free
    // (#1007). A fresh producer (`new`, `move`, a call returning owned, an owned
    // local/param being transferred out) is Owned, not a borrow, and is
    // unaffected. Fires from handleReturn; a no-op for non-owning return types.
    void checkBorrowReturnedThroughOwningType(const ast::Expr& e);

    // §28.6 *Borrowed owning collections* (#1008 Part C): reject an ownership-
    // transferring / disposing operation (Dispose / Drain / Pop* / Dequeue /
    // owning Add / Insert / Remove* / Clear) on a BORROW of an owning collection.
    // A borrow grants read access only; the owner disposes the container and its
    // elements, so a borrower's Dispose would double-free it. The current
    // scope's OWNED owning collections — a `this`-field, or a local/param with
    // Owned state — keep their full API.
    void checkBorrowedOwningCollectionCapability(
        const ast::MemberAccessExpr& callee);

    // §28.6 (#1005): reject returning an OWNED FIELD's `move`d value through a
    // method return type that is NOT declared `owned`. The leak is the `move`
    // form: `move this.f` / `move f` transfers the owned field's value out
    // (§28.8.1), nulling the field, so the moved value escapes a non-`owned`
    // (borrow) return with no owner. A BARE read (`return this.f;` / `=> f;`) is
    // NOT flagged: per §28.6's "Methods returning `this`" rule a non-`owned`
    // class return type yields a legal BORROW of the field (the field's owner —
    // the receiver's destructor — still disposes it; the owning-collection
    // `return _transactions` leak is the separate #1007 case). An `owned` return
    // type is the legal transfer and is exempt. Scoped to owned FIELD `move`s
    // only; owned-LOCAL returns (`var r := new T(); return r;`) are a separate
    // concern (#1006a) and are NOT handled here. Fires from handleReturn (block
    // bodies) and runExprBodyOwningReturnCheck (`=>` expression bodies).
    // // per §28.6
    void checkOwnedFieldReturn(const ast::Expr& e);

    // §28.3 / §5.11 (#1007): the name-head of an owning-collection / owning-
    // wrapper type (the set whose value the caller must discharge). True for
    // `OwningArray`/`OwningDictionary`/…/`Shared`/`Weak`/`OwningTask`/etc.
    static bool isOwningTypeNameHead(std::string_view head);
    // §28.6 / §5.11 (#1008 Part C): the owning COLLECTIONS only (OwningArray,
    // OwningDictionary, OwningSet, OwningStack, OwningQueue, OwningLinkedList) — the
    // Part C borrow-return / capability rules apply to these; the owning
    // wrappers keep their consumer-owns semantics.
    static bool isOwningCollectionNameHead(std::string_view head);

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
    // Returns true when it reported a diagnostic, so the owned-field borrow
    // check (issue #402) can skip a borrow source #386 has already flagged and
    // avoid a duplicate diagnostic for the same store. // per §28.3
    bool checkBorrowStoreEscape(const ast::AssignmentStmt& as);

    // §28.3 / §28.5 / §28.8 (issue #402): true when `target` resolves to an
    // `owned` field — an owned storage location. Reuses the owned-field
    // recognizer `exprIsOwnedFieldRead` (a real FieldSymbol carrying the
    // `owned` modifier; a property setter is not a FieldSymbol, so a property
    // target is correctly excluded). Used by handleAssignment to (a) require
    // an OWNED value on the RHS of an owned-field assignment and (b) record the
    // ownership transfer INTO the field (move-out of an owned local, discharge
    // of a consumed owned parameter). // per §28.3, §28.5, §28.8
    bool targetIsOwnedField(const ast::Expr& target) const;

    // §28.11 (issue #780): true when `target` is an owning inline-array slot
    // `a[i]` — an index into a `[N owned]T?` / `[N owned]T` value. Such a slot
    // is an owned storage location with the SAME discipline as an `owned` field:
    // its indexer-set "takes owned" (§28.11 element slot policy), so a borrow
    // RHS is the double-free hazard and a bare owned-local read is a move INTO
    // the slot. Used by handleAssignment to route the slot write through
    // handleOwnedFieldAssignment (with the owning-array-slot diagnostic
    // wording). // per §28.11, §28.3
    bool targetIsOwningInlineArraySlot(const ast::Expr& target) const;

    // #1020 / §28.6 (v0.153): true when `target` resolves to a NON-`owned` field
    // (a FieldSymbol without the `owned` modifier) — a BORROW field that does not
    // own its referent. Used by handleAssignment to reject a freshly-produced
    // owned value (producerState == Owned) stored into it: the field is not an
    // owner, so the fresh value would leak. A bare borrow read of a parameter or
    // local (producerState == Borrowed) is the legitimate back-reference store and
    // is not flagged. The `owned`-field case is handled by targetIsOwnedField.
    bool targetIsNonOwnedField(const ast::Expr& target) const;

    // §28.3 / §28.5 / §28.8 (issue #402): handle an assignment whose LHS is an
    // `owned` field. An owned field may only receive an OWNED value (§28.3
    // producers: `new`, `move`, an owned-returning call, a consumed `owned`
    // parameter). A BORROW RHS — a bare borrow parameter, a borrowed local, a
    // bare (un-`move`d) owned-field read, or `this` — would give the field's
    // owner disposal responsibility for an object whose real owner also
    // disposes it: a double-free. This reports that hazard (the field-write
    // analogue of #395 / #398). When the RHS is a legal owned value, it instead
    // records the transfer IN: a bare owned-LOCAL read moves out of the local
    // (move-on-read into an owned slot, §28.3); a bare owned-PARAMETER read
    // discharges the parameter (the §28.8 `_resource := resource;` pattern).
    // `move` / `new` / call RHS side-effects are already recorded by the
    // preceding recordMovesIn, so this records only the bare-identifier case.
    // `borrowAlreadyFlagged` is the #386 result, so a borrow source already
    // diagnosed there is not re-reported. `intoOwningArraySlot` (issue #780)
    // selects the owning inline-array-slot diagnostic wording (§28.11) instead
    // of the owned-field wording; the ownership semantics are identical, so the
    // borrow-reject and the move-/discharge-recording paths are shared.
    // // per §28.3, §28.5, §28.8, §28.11
    void handleOwnedFieldAssignment(const ast::AssignmentStmt& as,
                                    bool borrowAlreadyFlagged,
                                    bool intoOwningArraySlot = false);

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

    // Issue #392 / §31.1: true when `sym` is a catch-bound exception variable
    // of this method. A catch binding is a borrow of the in-flight exception
    // whose lifetime is the arm body; like an owned scope-bound local, the
    // borrow may not escape the arm.
    bool isCatchBorrow(sema::Symbol* sym) const;

    // True when `sym` is a scope-bound local whose borrow may NOT escape this
    // method body: either an owned scope-bound local (`isOwnedTrackedLocal`)
    // or a catch-bound exception variable (`isCatchBorrow`). The escape checks
    // use this in place of `isOwnedTrackedLocal` so the catch binding's borrow
    // is rejected on every channel that already rejects an owned local's
    // borrow (store-to-field, out/ref param, multi-hop alias, closure
    // capture). // per §28.3, §31.1
    bool isEscapeBoundLocal(sema::Symbol* sym) const;

    // Populate `catchBorrows_` by scanning every catch arm in `body`
    // (recursively, including nested try/if/while/for/do bodies) for a binding
    // pattern (`var ex: T`), resolving the bound LocalSymbol via the arm body
    // node it anchors on. Called once at the start of run(). // per §31.1
    void collectCatchBorrows(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void collectCatchBorrowsStmt(const ast::Stmt& s);

    // §28.3 / §28.9 (#861): populate `localDepth_` with each LocalDecl's
    // block-nesting depth. Mirrors collectCatchBorrowsStmt's statement-form
    // coverage so no block form is missed. Called once at the start of run().
    void collectLocalDepths(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void collectLocalDepthsStmt(const ast::Stmt& s, int depth);

    // Issue #392 / §31.1: a returned bare catch-binding identifier
    // (`return ex;`) lets the in-flight exception instance escape the arm
    // whose body the runtime disposes it after — a use-after-free. Diagnoses
    // that direct form. Indirect returns through a holder local
    // (`var b := ex; return b;`) and returned closures capturing `ex` are
    // handled by the existing escape machinery, which `collectCatchBorrows` /
    // `isEscapeBoundLocal` extend to recognize the catch binding as a source.
    void checkCatchBorrowEscapesOnReturn(const ast::Expr& e);

    // §31.1 / §17.12 (issue #1569, v0.182): the `throw` operand is consumed as
    // an OWNED value transferred to the runtime's exception machinery — so a
    // BORROWED class reference is not a valid operand: a catch-arm binding
    // re-thrown (`throw ex;`), a non-`owned` parameter, a field read, or `this`
    // each still has a live owner, and letting the runtime dispose it after the
    // arm is a use-after-free / double-dispose. `ownedArgSourceIsBorrow`
    // classifies the operand exactly as for an `owned` sink; when it is a
    // borrow, this diagnoses and directs to the bare re-raise `throw;`
    // (§31.2.4) or a fresh cause-chained instance (§31.4). A `new`/call, an
    // owned local/param transferred out, and a `move`d owned value are owned
    // producers and are not flagged. Called from the ThrowStmt handler
    // (`visitStmt`) and the ThrowExpr handler (`handleDischarge`), BEFORE the
    // operand's own discharge walk. // per §31.1, §17.12
    void checkThrowOperandNotBorrowed(const ast::Expr& operand);

    // §31.1 (issue #430): scan the constructor arguments of `ne` for a catch
    // borrow in a NON-OWNED parameter position. Returns the first catch-borrow
    // symbol found, or null. Only BARE identifier arguments are examined (value
    // projections like `ex.Message` are MemberAccessExprs and are skipped).
    // Owned-parameter positions are excluded to avoid double-reporting with the
    // existing #404 `checkOwnedArgSource` check. // per §31.1, §28.3
    sema::Symbol* catchBorrowAmongNonOwnedCtorArgs(const ast::NewExpr& ne) const;

    // §31.1 (issue #430): emit a §31.1 diagnostic when the NewExpr `ne` is in
    // an ESCAPING position (throw or return from a catch arm) and
    // `catchBorrowAmongNonOwnedCtorArgs` finds a catch borrow in a non-owned-
    // param argument. The escaping constructed object may store the borrow in
    // a field, leaving a dangling reference after the arm completes. Called
    // from the throw handler (`visitStmt`) and `handleReturn`. // per §31.1
    void checkCatchBorrowEscapesViaNewExpr(const ast::NewExpr& ne,
                                           SourceLocation loc);

    // §27.5 / §28.6 (#1217): true when `sym` is a plain `ref` or `out`
    // parameter (ParameterSymbol with modifier "ref" or "out", excluding
    // "out owned" — that is an ownership-transfer parameter and is already
    // handled by the owned-parameter escape machinery). A ref/out param is
    // stored as a pointer to the CALLER's stack frame; capturing it in a
    // closure that escapes the method produces a dangling pointer. // per §27.5
    bool isRefOutParam(sema::Symbol* sym) const;

    // §27.5 / §28.6 (#1217): reject a lambda stored to a FIELD or OUT-PARAM
    // (both escaping locations that outlive the method call) when the lambda
    // captures a ref/out parameter. The captured pointer is to the caller's
    // stack frame and is valid only for the duration of the call; storing
    // the closure in a field or through an out-param lets it outlive that
    // frame — a use-after-free. Called from handleAssignment AFTER the
    // existing checkBorrowStoreEscape, so it fires ONLY when the target is a
    // field or out-param (field = any MemberAccessExpr or bare FieldSymbol
    // identifier; out-param = the "p := …" assignment through an out/ref
    // parameter that is itself the destination). // per §27.5, §28.6
    void checkRefOutLambdaCaptureOnStore(const ast::AssignmentStmt& as);

    // §9.7: true when `rt` is a record type that holds at least one
    // borrow-carrying field of a non-runtime-managed type (a class/
    // interface reference field, an inline array of reference elements,
    // or another borrow-bearing record). String/function/boxed and
    // value-typed fields do not make a record borrow-bearing.
    bool isBorrowBearingRecord(const sema::TypeRef* rt) const;

    // §30.1 / §9.7 (v0.179, #1438/#1448): true when `rt` is borrow-bearing for
    // the async-signature and across-suspension rules — a user borrow-bearing
    // record (isBorrowBearingRecord, above) OR a builtin span *view* over an
    // Array / OwningBuffer (`ArraySpan` / `ReadOnlyArraySpan`, stdlib §4.15;
    // `Span` / `ReadOnlySpan`, stdlib §4.14 — each carries a borrow of its
    // backing reference-typed store, but has no user Record TypeDecl so the
    // predicate above misses it). The stdlib-certified `CancellationToken`
    // (§30.8, the single enumerated exception) is a value builtin with no
    // Record decl and is not in the span list, so it is correctly NOT flagged;
    // likewise the string-backed spans `ReadOnlyStringSpan` /
    // `ReadOnlyAsciiByteSpan` (string is runtime-managed, §9.7). // per §30.1
    bool isBorrowBearingRecordOrSpan(const sema::TypeRef* rt) const;

    // §30.1 (v0.179, #1438): an `async` method / async iterator may not declare
    // a parameter, result, or (for an async iterator) element type that is a
    // borrow-bearing record (isBorrowBearingRecordOrSpan). An async method
    // captures its parameters into a heap state-machine frame that outlives the
    // synchronous call and lives until the task settles, so a captured borrow
    // has no stack frame whose unwinding bounds it. Runs once per body at the
    // top of run(), gated on `isAsync_`. `CancellationToken` is exempt (handled
    // by the predicate). // per §30.1
    void checkAsyncSignatureBorrowBearing(
        const std::vector<sema::Symbol*>* paramSyms,
        const ast::Type* returnType);

    // §30.1 / §29.4 (v0.179, #1448): a local of borrow-bearing-record type
    // whose live range crosses a suspension point (`await`, `await using`,
    // `await for`, or `yield return` in an iterator) is rejected unless every
    // borrow it carries has a frame-resident source — an owned local, a
    // `using var`/`await using var`, or an `owned` parameter of the same body
    // (locations the state-machine frame itself owns, which therefore survive
    // every suspension). A caller-side source (a plain parameter, `this`, a
    // field through `this`, or an enclosing capture) does not qualify: its
    // owner may dispose it while the frame is suspended. Runs a source-order
    // walk over the body; gated on `isAsync_ || sawYield_`. // per §30.1, §29.4
    void checkBorrowBearingLocalsAcrossSuspension(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    // Info recorded at a borrow-bearing-record local's declaration for the
    // across-suspension check. `frameResident` is computed once from the
    // initializer's borrow source (an owned/using/owned-param source survives
    // suspension). // per §30.1
    struct SuspCandidate {
        std::string_view name;
        bool frameResident = false;
    };
    // Helpers for checkBorrowBearingLocalsAcrossSuspension. `walkSuspStmts`
    // takes raw statement pointers so a nested body, a `do…end` block, or a
    // single catch-arm body can all be walked through the one function.
    void walkSuspStmts(
        const std::vector<const ast::Stmt*>& stmts,
        std::unordered_map<const sema::Symbol*, const ast::Node*>& crossed);
    void reportSuspCross(const sema::Symbol* sym, const ast::Node* suspNode);
    // True when `init`'s borrow source is a frame-resident owner (per §30.1).
    bool suspSourceIsFrameResident(const ast::Expr& init);
    // Collect the identifiers read anywhere in `e` (no descent into lambdas).
    void collectReadIdentifiers(
        const ast::Expr* e,
        std::vector<const ast::IdentifierExpr*>& out) const;
    // True when `e` (recursively, no lambda descent) contains an `await`.
    bool exprContainsAwait(const ast::Expr* e) const;
    // True when any statement in `stmts` (recursively, incl. nested blocks) is
    // a suspension point: an `await` expr, an `await using` decl, an `await
    // for` loop, or a `yield return`. // per §30.1, §29.4
    bool blockContainsSuspension(
        const std::vector<std::unique_ptr<ast::Stmt>>& stmts) const;
    // The per-body candidate map + de-dup set for the across-suspension walk.
    std::unordered_map<const sema::Symbol*, SuspCandidate> suspCands_;
    std::unordered_set<const sema::Symbol*> suspInScope_;
    std::unordered_set<const sema::Symbol*> suspReported_;

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
    //
    // §28.8.1 (#867): the returned FlowState also carries the field-moved-from
    // facts merged over every path reaching exit (OR-over-paths), which run()
    // reads for the field-restoration check.
    FlowState runDataflow(const flow::CFG& cfg, const FlowState& entry);

    // §28.8.1 (#867): merge `other`'s field-moved-from facts into `into` with
    // OR-over-predecessors — a field moved-from on ANY incoming path is
    // moved-from at the join (restored only when restored on ALL paths). A
    // field absent from one operand is treated as RESTORED (false) on that
    // path: a field that was never moved on a path is not moved-from there.
    void joinFieldsInto(FieldMovedFrom& into, const FieldMovedFrom& other);

    // Process one CFG block's straight-line statements, mutating
    // `states_` (which the caller has set to the block's entry state).
    void processBlock(const flow::CFGBlock& block);

    sema::LocalSymbol* findLocalSymbol(const ast::LocalDecl& ld);

    // §29.2 (#834): seed the iteration variable of `fs` as Owned at the loop
    // body entry when its iterable resolves to an owning enumerable, so the
    // §28.3 discharge check rejects an undischarged iteration variable.
    void seedOwnedIterVar(const ast::ForStmt& fs);

    // §29.2 (#834): true when `fs`'s iterable has static type
    // `IOwningEnumerable<T>` / `IOwningAsyncEnumerable<T>` (owning iteration).
    bool iterableIsOwningEnumerable(const ast::ForStmt& fs);

    // §29.2 (#834): the LocalSymbol ExprTypecheck created for `fs`'s iteration
    // variable (its `decl` is `&fs`); null when none exists.
    sema::LocalSymbol* findForLoopVarSymbol(const ast::ForStmt& fs);

    LocalOwnership* lookupState(std::string_view name);

    void markDisposedByName(std::string_view name, SourceLocation loc);

    void markMovedByName(std::string_view name, SourceLocation loc);

    // §22.3 (v0.169, #1348): true when `e` is a NAMED OWNED BINDING — a bare
    // identifier (other than `this`/`base`) resolving to a tracked owned local
    // or `owned` parameter currently in the Owned state, so passing it into an
    // `owned` sink leaves the binding moved-from. This is the SOLE argument
    // form on which the call-site `owned` marker (`M(owned x)`) is REQUIRED;
    // every other owned-value form (a `new` expression, a call returning
    // `owned T`, an operator result, a `move obj.f`) is a *fresh producer* that
    // invalidates no live binding and stays unmarked. Reuses the existing
    // moved-from state tracking rather than re-classifying the expression.
    bool isNamedOwnedBinding(const ast::Expr& e);

    // §22.3 (v0.169, #1348): report the "missing marker" error — a named owned
    // binding `name` is being transferred into an `owned` sink without the
    // required call-site `owned` marker. Emits the spec-stated fix-it.
    void reportMissingOwnedMarker(std::string_view name, SourceLocation loc);

    // §22.3 (v0.169, #1348): report a "spurious marker" error — an `owned`
    // call-site marker written where it is rejected. `onFreshProducer` selects
    // the fresh-producer wording (`new`/owned-returning call/operator/`move
    // obj.f`); otherwise the non-`owned`-parameter-position wording.
    void reportSpuriousOwnedMarker(SourceLocation loc, bool onFreshProducer);

    // §28.8.1 (#897 #898): record a `move this.field` / `move _field` (qualified
    // OR bare owned-field move). This is the field analogue of markMovedByName
    // for locals: BEFORE marking the field moved-from, it CHECKS the current
    // (path-sensitive, #867) `fieldMovedFrom_` fact — if the field is already
    // moved-from on the path reaching this move site, the move has no ownership
    // to transfer and is a use-after-move; report a §28.8.1 error. Otherwise
    // mark the field moved-from and record the move-site location. The check
    // routes through `report()` so the loop-fixpoint suppression (#867) emits
    // each diagnostic exactly once on the converged final pass — so a loop whose
    // body moves the field but restores it only conditionally is rejected at the
    // move site on the moved-from back-edge path (#898), while a move-restore-
    // move sequence and a loop with unconditional restore are accepted.
    void recordFieldMove(std::string_view name, SourceLocation loc);

    // §28.3 / §28.11 (issue #860): mark a tracked local Owned because it
    // received ownership through an `out owned` argument. `origin` is the
    // AST node the dropped-without-disposal diagnostic points at. // per §28.3
    void markOwnedByName(std::string_view name, SourceLocation loc,
                         const ast::Node* origin);

    // §28.8 owned-sink check (issue #395): true when `argValue` — an
    // argument destined for an `owned` parameter, or the RHS of an
    // owning-collection `set(owned)` indexer assignment — denotes a *borrow*
    // rather than an owned value. Passing a borrow into an `owned` sink lets
    // the sink dispose a value it does not own (a double-dispose with the
    // value's real owner), so it must be rejected.
    //
    // The borrow forms flagged are: a tracked local currently in the Borrowed
    // state; a bare identifier resolving to a non-`owned` parameter (a borrowed
    // parameter, whose source is the caller's frame); and a *bare* read of an
    // `owned` field — `obj.f` / a bare `f` resolving to an owned field — which
    // §28.3 classifies as Borrowed (source: `obj`), the only owned-field read
    // that yields an owned value being `move obj.f` (§28.8.1). A leading `move`
    // is unwrapped first — `move b` of a borrow is no more an owned value than
    // the bare `b`, while `move obj.f` IS the legal owned producer (§28.3 /
    // §28.8.1) and so is NOT flagged: the owned-field branch only fires when no
    // enclosing `move` wrapped the read (issue #398). Every owned-value form
    // (`new T(...)`, a call returning `owned T`, a `move` of an owned local /
    // owned field / owned slot, a bare owned local) is NOT flagged, so legal
    // §28.8 transfers are never over-rejected. Anything the analysis cannot
    // classify (e.g. `this`, an unresolved symbol, a non-`owned` field) is
    // conservatively treated as NOT a borrow, so it is never falsely rejected.
    // // per §28.8, §28.3, §28.8.1
    bool ownedArgSourceIsBorrow(const ast::Expr& argValue);

    // §28.3 (issue #398): true when `e` is a read of an `owned` field — either
    // an explicit member access `obj.f` whose member resolves to an owned
    // FieldSymbol, or a bare identifier resolving to an owned field of the
    // declaring class (implicit `this`). A bare such read is Borrowed per the
    // §28.3 producers table ("Field access `obj.f` where `f` is `owned` |
    // Borrowed (source: `obj`)"); only `move obj.f` reads it as an owned value
    // (§28.8.1). Used by `ownedArgSourceIsBorrow` to reject a bare owned-field
    // read flowing into an owned sink. // per §28.3, §28.8.1
    bool exprIsOwnedFieldRead(const ast::Expr& e) const;

    // §28.6 / §5.11 (#1493): true when `e` is an `owned`-field read AND the
    // receiver is `this` — a bare identifier (implicit `this.f`) or an
    // explicit `this.f`, never another object's field. This is the exact
    // shape `checkBorrowReturnedThroughOwningType`'s owning-WRAPPER branch
    // admits as a borrow (mirroring the owning-collection branch's identical
    // receiver restriction, inlined there); factored out here so the caller-
    // side classifier below (`methodReturnsOnlyWrapperReceiverBorrow`) can
    // reuse the exact same test. // per §28.6, §28.3, §5.11
    bool exprIsReceiverOwnedFieldBorrow(const ast::Expr& e) const;

    // §28.6 / §5.11 (#1493): does every reachable `return` under `s` return
    // the wrapper-receiver-field-borrow shape above? Sets `sawReturn` when at
    // least one `return <expr>;` was visited, and clears `allBorrow` on the
    // first one that is `return;` (no value) or returns anything else. Does
    // NOT descend into nested closures — a lambda's `return` belongs to the
    // lambda, not the enclosing method. Statement coverage mirrors
    // `moveOnlyArrayLeakScanStmt`'s control-flow walk (§17 statement forms
    // that can carry a nested `return`). // per §28.6, §28.3, §5.11
    void scanReturnsForWrapperReceiverBorrow(const ast::Stmt* s,
                                              bool& sawReturn,
                                              bool& allBorrow) const;

    // §28.6 / §5.11 (#1493): true when EVERY return path of `md`'s body is the
    // wrapper-receiver-field-borrow shape (an expression body counts as its
    // single implicit return). `checkBorrowReturnedThroughOwningType`
    // guarantees this is the ONLY borrow shape it admits for a non-`owned`
    // owning-WRAPPER return type (Shared, Weak, ChannelSender, OwningTask,
    // IOwningEnumerable, CancellationTokenSource) — every other admitted
    // return there (`new`, `move`, an owned-returning call, a transferred
    // owned local/param) is a genuine owned producer the caller must still
    // discharge. So "every return matches" is a sound, purely structural test
    // for classifying a CALL to `md` as Borrowed rather than the wrapper's
    // default Owned (consumer-owns) classification in `producerState` below —
    // without it, a legitimate borrow-return (the shape #1493 fixed at the
    // definition site) was still tracked Owned at every call site, triggering
    // a spurious §28.3 auto-drop warning (memory-safe only by Dispose()'s
    // idempotency, §28.2 — not a double-free, but a wrong classification). A
    // body with no return in the walk is conservatively NOT classified
    // Borrowed (returns false). // per §28.6, §28.3, §5.11
    bool methodReturnsOnlyWrapperReceiverBorrow(const ast::MethodDecl& md) const;

    // §28.8: report the owned-sink violation for `argValue` (at `loc`) when
    // `ownedArgSourceIsBorrow` holds. Returns true when it reported (the
    // caller then skips the bogus `markMovedByName` that would silently treat
    // the borrow as consumed). // per §28.8
    bool checkOwnedArgSource(const ast::Expr& argValue, SourceLocation loc);

    // §28.3 / §28.8 (issue #407): an owned-sink argument that is NOT a bare
    // identifier and NOT a bare owned-field read — those two forms have
    // dedicated handling at each call site that also records the move when the
    // source is a legal owned value. This catch-all covers the remaining class-
    // typed borrow forms a non-`move`d argument can take: an indexer-get
    // `coll[i]` (§28.3 / §28.11), a non-`owned` field read `obj.f` (§28.3), and
    // `this` (§28.3 / #403). None is ever a legal owned producer in bare form,
    // so there is nothing to mark moved-from; reports via `checkOwnedArgSource`
    // and returns true when it reported (the caller then treats the arg as
    // consumed). A `move`-wrapped argument is a legal owned producer and is NOT
    // reported (`ownedArgSourceIsBorrow` unwraps the `move` and returns false).
    // // per §28.3, §28.8, §28.11
    bool checkOwnedArgOtherBorrow(const ast::Expr& argValue);

    // §28.11 indexer-set (issue #395): true when `target` is `coll[i]` on an
    // owning collection whose `set(owned)` indexer requires an owned RHS. Per
    // §28.11 "Indexer set `coll[i] := value` requires `value` to be owned."
    // Recognizes a user indexer `PropertyDecl` whose set accessor is declared
    // `set(owned)`, and the built-in owning collections `OwningArray<T>` /
    // `OwningDictionary<K, V>` (stdlib §4.5 / §4.9), whose indexer setter takes
    // owned by definition. // per §28.11
    bool indexerSetterTakesOwned(const ast::Expr& target);

    // §28.8 built-in owned-sink methods (issue #395): true when `callee` is a
    // member access `coll.M` whose receiver `coll` has a *built-in* owning-
    // collection type (which sema resolves to a Named TypeRef with a null
    // declaration, so the user-declaration path in recordMovesInArgs /
    // handleDischarge cannot see the `owned` formal) AND `M` is one of that
    // family's owned-taking methods at argument index `argPos` (stdlib §4.5 /
    // §4.7 / §4.9: `Add` / `Insert` / `AddRange` / `AddFirst` / `AddLast` /
    // `AddBefore` / `AddAfter` / `Push` / `Enqueue`). Gating on the built-in
    // receiver type means it never mis-fires on a user method of the same name
    // (those go through the precise declaration-based path). // per §28.8
    bool builtinOwnedSinkArg(const ast::Expr* callee, std::size_t argPos);

    // §28.3 / §28.11 (issue #860): true when `callee` is a member access
    // `recv.M` whose receiver `recv` has a *built-in* type carrying an
    // `out owned` parameter at argument index `argPos` — the caller-side
    // out-owned transfer that moves an element OUT of an owning collection
    // (or a Channel<T>/Weak<T> handle) into the caller's local. The owned-OUT
    // direction is the mirror of `builtinOwnedSinkArg` (which models owned-IN
    // sinks): there the argument local LOSES ownership, here it RECEIVES it.
    // Recognised: `OwningStack<T>.TryPop`, `OwningQueue<T>.TryDequeue`,
    // `OwningLinkedList<T>.TryPopFirst/TryPopLast`, `OwningSet<T>.TryTake`
    // (argPos 1), `OwningDictionary<K,V>.TryPopValue` (argPos 1),
    // `Channel<T>.TryReceive` (class T only — §6.9 value-T drops the
    // modifiers), and `Weak<T>.TryUpgrade` (stdlib §4.5/§4.7/§4.9/§6.9/§2.16).
    // The borrow forms `TryPop(out item)` / `TryPeek(out item)` carry no
    // `owned` and are NOT recognised. // per §28.3, §28.11
    bool builtinOutOwnedArg(const ast::Expr* callee, std::size_t argPos);

    // §28.3 / §28.11 (issue #860): after a call, mark each argument that is an
    // `out owned` transfer (a call-site `out owned` modifier on a method whose
    // formal is genuinely `out owned`, a user `out owned` parameter, or a
    // built-in out-owned method per `builtinOutOwnedArg`) and is a bare LOCAL
    // identifier as Owned — the local received ownership of the moved-out value
    // and now carries the §28.3 discharge obligation. Marked unconditionally
    // (conservative for the Try* false-return path, where the local is unchanged
    // / null); the canonical null-safe consumer `item?.Dispose()` discharges it.
    // // per §28.3, §28.11
    void markOutOwnedArgs(const std::vector<ast::CallArgument>& args,
                          const ast::Expr* callee);

    // -- Async-readiness 3c: at-await-site Pattern A / B enforcement -----------
    //
    // §30.5 "Consumer-side rules". For `await <task>` the obligations are fixed
    // by the task's STATIC return type, which 3b's await typing makes readable:
    // the await UnaryExpr's operand carries the resolved `Task`/`Task<T>`/
    // `OwningTask<T>` TypeRef. These checks run as a single syntactic walk over
    // the async body (driven from `run()`), independent of the CFG dataflow, so
    // each await is examined once and no diagnostic is duplicated. They are
    // gated on `isAsync_`, so a synchronous body never reaches them. // per §30.5

    enum class AwaitTaskKind {
        NotTask,    // operand not a Task-family type (e.g. async-iterator await,
                    //   or an untyped operand) — no site rule applies here
        UnitTask,   // `await Task` — no result value, no ownership obligation
        PatternA,   // `await Task<T>` for CLASS T — borrow; needs owning location
        PatternB,   // `await OwningTask<T>` for class T — ownership transfers;
                    //   needs an owned-consuming context
        ValueResult,// `await Task<T>` for non-class T — no obligation crosses
    };

    // Classify the await by its operand's resolved task type (§30.3 / §30.5).
    AwaitTaskKind awaitTaskKind(const ast::UnaryExpr& await) const;

    // §30.5 Pattern A: true when `taskExpr` (the await's operand) occupies a
    // syntactically-visible OWNING location whose lifetime bounds the borrowed
    // result — a reference to a `using var`/`await using var`-bound local of
    // `Task<T>`, an `owned` parameter, an `owned` field of `this`, an
    // owning-collection indexer slot, or a borrowing-iteration variable. The
    // list mirrors §30.5's admitted positions; anything else (a bare producer
    // call, an arbitrary expression) is NOT an owning location. // per §30.5
    bool awaitOperandIsOwningLocation(const ast::Expr& taskExpr);

    // Drive the at-await-site checks over the body. For each statement, decide
    // which awaits sit in an owned-consuming context (Pattern B) — passed via
    // `consumingAwaits` collected by `collectOwnedConsumingAwaits` — then walk
    // every await in the statement and diagnose: a Pattern A await whose
    // operand is not an owning location, and a Pattern B await not in an
    // owned-consuming context. // per §30.5
    void checkAwaitSites(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void checkAwaitSitesStmt(
        const ast::Stmt& s,
        const std::unordered_set<const ast::UnaryExpr*>& consumingAwaits);
    void checkAwaitSitesExpr(
        const ast::Expr& e,
        const std::unordered_set<const ast::UnaryExpr*>& consumingAwaits);

    // §30.5 owned-consuming contexts: collect into `out` every `await`
    // UnaryExpr that appears as the IMMEDIATE value of an owned-consuming
    // position — a LocalDecl `:= await` RHS (using/plain/await-using var), an
    // assignment-to-owned-location `target := await` RHS, an `owned` argument
    // `M(await)` / `coll.Add(await)`, an `owned`/`OwningTask` `return await`,
    // and an owned `yield return await`. A Pattern B await NOT in this set is
    // dropped without discharge and is diagnosed. // per §30.5
    void collectOwnedConsumingAwaits(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        std::unordered_set<const ast::UnaryExpr*>& out);
    void collectOwnedConsumingAwaitsStmt(
        const ast::Stmt& s, std::unordered_set<const ast::UnaryExpr*>& out);
    // §30.5: record awaits passed as `owned` arguments (a call's `owned`
    // formal, or a built-in owning-collection owned-sink method) — those are
    // owned-consuming positions. Recurses into the expression. // per §30.5
    void collectOwnedConsumingInExpr(
        const ast::Expr& e, std::unordered_set<const ast::UnaryExpr*>& out);

    // The await UnaryExpr at the root of `e` when `e` is exactly `await X`
    // (the immediate-value form an owned-consuming position admits), else null.
    static const ast::UnaryExpr* awaitRoot(const ast::Expr& e);

    // -- #1530 (§30.3 / §30.5, v0.179): `Task<T>.Result` (Pattern A, class T)
    // owning-location check ------------------------------------------------
    //
    // Reading `.Result` on a completed `Task<T>` whose T is a class is
    // additionally subject to the SAME owning-source rule as `await Task<T>`
    // (§30.3): the read produces a borrow whose source is the task
    // expression's own location, so the receiver must occupy a syntactically-
    // visible owning location exactly as an awaited Pattern A operand must
    // (`awaitOperandIsOwningLocation`, reused verbatim — a `.Result` receiver
    // and an `await` operand are the same "task expression" shape). Unlike
    // `await`, there is no chained-slot admission for a bare producer call
    // (`return await Producer()`'s exception): `.Result` is a synchronous
    // read, not part of an async state machine's return-await chain, so a
    // bare-producer receiver (`SomeProducer().Result`) is unconditionally
    // rejected. Runs UNCONDITIONALLY over every method body (sync and async)
    // — unlike the at-await-site checks above, which are gated on `isAsync_`
    // because `await` only appears in async bodies, `.Result` is the
    // synchronous escape hatch and is typically read from ordinary
    // (non-async) code. // per §30.3, §30.5
    void checkResultSites(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void checkResultSitesStmt(const ast::Stmt& s);
    void checkResultSitesExpr(const ast::Expr& e);

    // -- Async-readiness 3c: SuspendLocal annotation + completion edges --------
    //
    // §30.5 / §30.9. Classify each owned local as suspend-local (live range
    // crosses an await) vs activation-local, recording the result on its
    // LocalOwnership.suspendLocal. Adds synthetic completion edges from each
    // await-bearing CFG block to the exit so the abandonment/cancellation
    // exits give a suspend-local the same frame-disposal treatment as the
    // §28.3 exception-path discharge. See the rationale block in Ownership.cpp.
    // // per §30.5, §30.9, §30.8
    void annotateSuspendLocals(const flow::CFG& cfg);
    void addCompletionEdges(flow::CFG& cfg);

    // Async-readiness 3c: the set of LocalDecls whose owned local is a
    // suspend-local candidate (declared at/before an await in CFG order; the
    // conservative over-approximation `annotateSuspendLocals` computes). Read
    // by handleLocalDecl to set LocalOwnership.suspendLocal when the local is
    // created. Empty for a synchronous body (annotateSuspendLocals is not
    // called) and for an async body with no await. // per §30.5
    std::unordered_set<const ast::Node*> suspendLocalDecls_;

    // Async-readiness 3c (§30.5 line ~6068): the set of `await` UnaryExprs that
    // appear directly as the value of a `return` statement. A Pattern A
    // `return await Producer()` is an explicitly-admitted owning position even
    // though `Producer()` is a bare call — the producer's task is owned by the
    // enclosing state machine and the borrow flows into the outer Pattern A
    // task's result slot. Populated by collectOwnedConsumingAwaitsStmt's
    // ReturnStmt branch (alongside the owned-consuming set) and consulted by
    // the Pattern A site check. // per §30.5
    std::unordered_set<const ast::UnaryExpr*> returnPositionAwaits_;

    // Async-readiness 3c (§30.5 line ~6110): the Pattern B await is
    // owned-consuming WITH RESPECT TO ITS OPERAND — awaiting an owned task
    // local consumes that local. When the await's operand is a bare owned
    // task local, mark it consumed (Disposed) so a straight-line SECOND await
    // of the same bound local is a read-after-consume (the statically-
    // detectable slice of the §30.4 single-consumer rule). Called from the
    // discharge/decl transfer functions on each await operand. // per §30.5
    void recordAwaitOperandConsume(const ast::Expr& awaitOperand,
                                   SourceLocation loc);

    // §30.5 / §30.4 (#834): true when `e`'s static type is `OwningTask<...>`
    // (the single-consumer owning task an `await` consumes); generic-safe
    // (name-head based), unlike awaitTaskKind's result-class gate.
    bool operandIsOwnedTaskType(const ast::Expr& e);

    // -- §28.14 (v0.175, #808): scoped callback parameters -------------------
    //
    // Storage-bounded capture. Two checker halves:
    //   CALL-SITE ADMISSION — a closure argument bound to a `scoped` formal is
    //   admitted iff the call's BOUNDING OBJECT resolves to a `using var` /
    //   `await using var` local L (constructor→constructed, owned-return-as-
    //   initializer→returned, else receiver) and every captured source is
    //   declared BEFORE L; the sources are then FROZEN (no move / dispose /
    //   reassign) until L's scope exits.
    //   CALLEE VERIFICATION — a `scoped` parameter may only be invoked, stored
    //   into a `scoped` field of `this`, or forwarded to a `scoped` parameter
    //   of a call on `this`; a `scoped` FIELD may only be invoked, overwritten
    //   from a `scoped` parameter, cleared to null, or null-compared.
    //
    // Both run as SYNTACTIC pre-passes at the start of run() (before the CFG
    // dataflow), so each diagnostic is emitted exactly once and the freeze map
    // is complete before the dataflow's enforcement hooks fire. // per §28.14

    // Declaration-order index of every local declaration node, assigned in
    // source order by scopedPreScan (which — unlike collectLocalDepths — also
    // walks lambda bodies). Keys: LocalDecl*, ForStmt* (iteration variable),
    // `out owned var` IdentifierExpr*. Parameters and `this` precede every
    // local (they have no entry; absence + ParameterSymbol/this = index -1).
    std::unordered_map<const ast::Node*, int> scopedDeclOrder_;
    int scopedDeclCounter_ = 0;
    // Initializer expression -> its `using var`/`await using var` LocalDecl,
    // for bounding forms 1 (constructor) and 2 (owned-return initializer).
    std::unordered_map<const ast::Expr*, const ast::LocalDecl*>
        scopedInitOfUsingLocal_;
    // The enclosing method's own parameter symbols — distinguishes a real
    // capture of a method parameter from a lambda-internal parameter.
    std::unordered_set<sema::Symbol*> methodParamSet_;
    // §28.14 freeze: source symbol -> {bounding local's name, admission call
    // site}. A frozen source may not be moved from, disposed, or reassigned
    // AFTER the admission site (source-offset ordered; the pre-scan records,
    // the dataflow transfer functions enforce). The freeze nominally ends when
    // L's scope exits; v1 keeps it for the rest of the method — sound (only
    // over-rejects mutation after L's block closed, a rare shape).
    struct ScopedFreeze {
        std::string_view boundingName;
        SourceLocation admissionLoc;
    };
    std::unordered_map<sema::Symbol*, ScopedFreeze> scopedFrozen_;

    // §28.14 verifiable-subset widening (#1420 triage): a local initialized
    // with a LAMBDA LITERAL and never reassigned is a verifiable function
    // value at a scoped call site — its captures are the literal's captures
    // (`var h := (v) => box.Add(v); … evt.Subscribe(h);`). Maps the
    // LocalSymbol to its initializer lambda; consulted only when the symbol
    // is absent from scopedAssignedSyms_.
    std::unordered_map<sema::Symbol*, const ast::LambdaExpr*>
        scopedLambdaLocalInit_;
    // Every symbol that is an assignment target anywhere in the body
    // (including through an `out`/`ref` call argument). A lambda-literal
    // local among them is NOT verifiable (the binding may have been swapped
    // before the call site — the pre-scan is flow-insensitive).
    std::unordered_set<sema::Symbol*> scopedAssignedSyms_;
    // Deferred §28.14 admission work items: the walk indexes declarations and
    // records the maps above; the admission checks run AFTER the full walk so
    // scopedLambdaLocalInit_ / scopedAssignedSyms_ are complete. (Declaration-
    // order comparisons are unchanged: a source declared after the bounding
    // local now carries a LARGER index instead of no index — rejected the
    // same way.)
    struct ScopedAdmission {
        const ast::Expr* callNode;
        const ast::Expr* callee;
        const ast::NewExpr* ne;
        const ast::Expr* arg;
    };
    std::vector<ScopedAdmission> scopedAdmissionQueue_;

    // Walk the body once: assign scopedDeclOrder_, record using-var
    // initializer anchors, then check every call with a `scoped` formal
    // (admission) and every use of a `scoped` param / field (callee rules).
    void scopedPreScan(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void scopedPreScanStmt(const ast::Stmt& s);
    void scopedPreScanExpr(const ast::Expr& e);

    // True when the call's formal at `argPos` is `scoped`. Resolves user
    // methods/ctors via sema symbols, builtin TaskScope.Spawn by receiver
    // type name, and function-VALUE callees via their Function TypeRef.
    bool scopedFormalAt(const ast::Expr* callee, const ast::NewExpr* ne,
                        std::size_t argPos);

    // §24.3 / stdlib §6.5, §6.1 (v0.175 signatures; enforced per #1420): true
    // when `callee` is a builtin subscribe-and-store call whose handler formal
    // at `argPos` is `scoped` — `<event>.Subscribe(handler)` (the target
    // resolves to an EventSymbol, or the receiver's static type is
    // EventSource / AsyncEventSource / IEvent / IAsyncEvent) or
    // `<token>.Register(callback)` (receiver type CancellationToken). Both
    // return `owned Subscription` (stdlib §2.11), so the §28.14 bounding
    // object resolves per form 2 (owned return initializing a `using var`) —
    // see scopedBoundingLocal. The event `Add`/`Remove` (`+=`/`-=`) forms are
    // NOT flipped: §24.3's normative signatures keep `Add(handler: (T) -> ())`
    // plain, so they stay on the §28.14 stdlib-certified legacy path.
    bool builtinScopedCallbackFormal(const ast::Expr* callee,
                                     std::size_t argPos);
    // Name-head matchers for the builtin receiver types above (the same three
    // TypeRef fallbacks as typeRefIsTaskScope).
    bool typeRefIsEventSourceLike(const sema::TypeRef* rt) const;
    bool typeRefIsCancellationToken(const sema::TypeRef* rt) const;

    // §28.14 call-site admission for one argument bound to a scoped formal.
    // `callNode` is the CallExpr / NullCondCallExpr / NewExpr node.
    void checkScopedAdmission(const ast::Expr& callNode,
                              const ast::Expr* callee, const ast::NewExpr* ne,
                              const ast::Expr& arg);

    // Resolve the §28.14 bounding object of a call to a using/await-using
    // LocalDecl, or null when the bounding object is not usable as a bound
    // (plain expression receiver, field, non-initializer owned return, …).
    const ast::LocalDecl* scopedBoundingLocal(const ast::Expr& callNode,
                                              const ast::Expr* callee,
                                              const ast::NewExpr* ne);

    // COMPLETE capture enumeration for a lambda argument (statements,
    // expressions, patterns, nested lambdas; `this` is an IdentifierExpr and
    // is collected like any identifier). Unlike collectLambdaIdentifiers this
    // walks every statement/expression form — a missed capture would be a
    // silently-admitted escape. `declared` accumulates the lambda's own
    // params/locals so uses of lambda-internal names are not captures.
    void scopedCollectCaptures(const ast::LambdaExpr& lam,
                               std::vector<const ast::IdentifierExpr*>& out);
    void scopedCaptureWalkStmt(const ast::Stmt& s,
                               std::vector<const ast::IdentifierExpr*>& out);
    void scopedCaptureWalkExpr(const ast::Expr& e,
                               std::vector<const ast::IdentifierExpr*>& out);

    // §28.14 freeze enforcement: report when `name` resolves to a frozen
    // source and the mutation site `loc` is at-or-after the admission site.
    // `action` names the rejected operation for the diagnostic.
    void checkScopedFreeze(std::string_view name, SourceLocation loc,
                           std::string_view action);

    // §28.14 callee verification pre-pass (see the header comment above).
    void scopedCalleeScan(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void scopedCalleeWalkStmt(const ast::Stmt& s);
    // `allowInvokeOf` lets a CallExpr's callee identifier through (invoke).
    void scopedCalleeWalkExpr(const ast::Expr& e);
    // Report + true when `e` is a bare use of a scoped param / scoped field
    // outside every admitted context.
    bool scopedRestrictedUse(const ast::Expr& e, std::string_view context);
    // True when `e` is an identifier/member-access denoting a `scoped`
    // parameter of THIS method (kind=1) or a `scoped` field (kind=2, bare or
    // this-qualified). Returns 0 otherwise.
    int scopedUseKind(const ast::Expr& e, std::string_view* nameOut = nullptr);

    // -- §28.14 v0.177 (#1419): the call-bounded default ----------------------
    //
    // A PLAIN (no-modifier) function-typed parameter is CALL-BOUNDED in this
    // callee: it may be invoked, bound to locals, and passed to another plain
    // function-typed parameter; it may NOT be stored in a field of any object,
    // stored into a collection reachable beyond the call (parameter-, field-,
    // or borrow-reachable — a purely local collection is a local binding),
    // returned (including a `yield return` or a write through an out/ref
    // parameter, both of which hand the value beyond the call), captured by a
    // closure that itself escapes by these same rules (transitively, followed
    // through local bindings), or forwarded into a `scoped` parameter position
    // (the callee cannot vouch for captures it cannot see). Compiler-verified
    // for user-authored callees — structural sink checks, no interprocedural
    // analysis; builtin callees state their retention in their signatures
    // (`scoped` where they store; §30.11 and the lazy query operators stay on
    // the stdlib-certified legacy path). Runs as a syntactic pre-pass beside
    // scopedCalleeScan, BEFORE scopedPreScan so the alias map is complete when
    // the §28.14 admission consults it for the forward-to-scoped shapes.
    // // per §28.14, §28.3, §22.3 (v0.177)

    // The enclosing method's plain function-typed parameters.
    std::unordered_set<sema::Symbol*> callBoundedParams_;
    // Locals that transitively CARRY a call-bounded parameter: bound from the
    // parameter directly, from a closure capturing one (or capturing another
    // carrier), from a conditional over carriers, or from a collection literal
    // containing one. Maps the carrier to the parameter it carries. Recorded
    // in source order; flow-insensitive (an alias is never un-recorded —
    // conservative, may over-reject a rebound name, never under-rejects).
    std::unordered_map<sema::Symbol*, sema::ParameterSymbol*> callBoundedAlias_;
    // Locals whose initializer is ROOTED at a parameter / field / `this` /
    // call result — a collection reached through one is reachable beyond the
    // call. (Only consulted for collection receivers.)
    std::unordered_set<sema::Symbol*> beyondCallRooted_;

    // The pre-pass driver + walkers (statement coverage mirrors
    // scopedCalleeWalkStmt).
    void callBoundedScan(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void callBoundedWalkStmt(const ast::Stmt& s);
    void callBoundedWalkExpr(const ast::Expr& e);
    // The call-bounded parameter `e` denotes or transitively carries (bare
    // identifier, carrier local, capturing lambda, conditional, collection
    // literal, postfix-bang), or null.
    sema::ParameterSymbol* callBoundedSourceOf(const ast::Expr& e);
    // True when `recv` (a collection receiver / index-store target) is
    // reachable beyond the call: rooted at a parameter, field, `this`, a call
    // result, or a beyond-call-rooted local. Conservative for unresolved
    // roots.
    bool exprReachableBeyondCall(const ast::Expr& recv);
    // True when `callee` is a builtin-collection STORING mutator
    // (Add / Insert / Push / Enqueue / …) — the §28.14 collection-store sink.
    // User methods never match (their formals carry their own obligations).
    bool builtinCollectionStoreCallee(const ast::Expr* callee);
    // Report the §28.14 v0.177 sink violation, naming the parameter and the
    // spec-stated fix.
    void reportCallBounded(SourceLocation loc, std::string_view paramName,
                           std::string_view sink);
    // True when the call's formal at `argPos` resolves to a USER-authored
    // PLAIN function-typed parameter — a position v0.177 verifies callee-side
    // as call-bounded, so the CALLER may pass any capture-bearing closure
    // there (checkClosureEscapesOnReturn skips its legacy rejection for such
    // arguments). Builtin callees (the lazy query operators' storing
    // signatures), function-value callees, and unresolved callees stay on the
    // legacy caller-side rejection — the stdlib-certified path §28.14 v0.177
    // records. // per §28.14, §28.3 (v0.177)
    bool verifiedCallBoundedFormalAt(const ast::Expr* callee,
                                     const ast::NewExpr* ne,
                                     std::size_t argPos);

    // -- stdlib §6.10 (v0.175, #808): TaskScope consumption rules ------------
    // TaskScope may only be consumed by `await using`; a plain `using var`, an
    // undisposed drop, or an explicit .Dispose() is a compile error naming the
    // rule; `await L.Join()` must appear on the normal path (checked
    // syntactically: error only when no such await exists AND the scope local
    // is never delegated as a call argument — the provable subset).
    bool exprTypeIsTaskScope(const ast::Expr& e) const;
    bool typeRefIsTaskScope(const sema::TypeRef* rt) const;
    void checkTaskScopeConsumption(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    // Scan helpers: does `await <name>.Join()` / a bare-`name` argument occur?
    void taskScopeUsageScanStmt(const ast::Stmt& s, std::string_view name,
                                bool& sawAwaitJoin, bool& sawDelegation);
    void taskScopeUsageScanExpr(const ast::Expr& e, std::string_view name,
                                bool& sawAwaitJoin, bool& sawDelegation);
};

} // namespace bezel::ownership
