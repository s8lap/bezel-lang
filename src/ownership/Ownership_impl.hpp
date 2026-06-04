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
    //
    // Async-readiness 3c: `isAsync` is true when the enclosing method/lambda
    // is declared `async`. It gates the additive at-await-site Pattern A / B
    // checks (§30.5) and the across-suspend SuspendLocal annotation — all of
    // which fire ONLY on async bodies, so a non-async body's diagnostics are
    // byte-for-byte unchanged. // per §30.5, §30.1
    void run(const std::vector<std::unique_ptr<ast::Stmt>>& body,
             const std::vector<sema::Symbol*>* paramSyms = nullptr,
             bool isAsync = false);

private:
    sema::SemaContext& ctx_;
    std::unordered_map<sema::Symbol*, LocalOwnership> states_;
    // Async-readiness 3c: true while analysing an `async` method/lambda body.
    // Gates the at-await-site Pattern A/B checks and the SuspendLocal
    // annotation so they never affect a synchronous body. // per §30.5
    bool isAsync_ = false;
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
    // diagnosed there is not re-reported. // per §28.3, §28.5, §28.8
    void handleOwnedFieldAssignment(const ast::AssignmentStmt& as,
                                    bool borrowAlreadyFlagged);

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

    // Issue #392 / §31.1: a returned bare catch-binding identifier
    // (`return ex;`) lets the in-flight exception instance escape the arm
    // whose body the runtime disposes it after — a use-after-free. Diagnoses
    // that direct form. Indirect returns through a holder local
    // (`var b := ex; return b;`) and returned closures capturing `ex` are
    // handled by the existing escape machinery, which `collectCatchBorrows` /
    // `isEscapeBoundLocal` extend to recognize the catch binding as a source.
    void checkCatchBorrowEscapesOnReturn(const ast::Expr& e);

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
    // `set(owned)`, and the built-in owning collections `OwnedArray<T>` /
    // `OwnedDictionary<K, V>` (stdlib §4.5 / §4.9), whose indexer setter takes
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

    // -- Async-readiness 3c: at-await-site Pattern A / B enforcement -----------
    //
    // §30.5 "Consumer-side rules". For `await <task>` the obligations are fixed
    // by the task's STATIC return type, which 3b's await typing makes readable:
    // the await UnaryExpr's operand carries the resolved `Task`/`Task<T>`/
    // `OwnedTask<T>` TypeRef. These checks run as a single syntactic walk over
    // the async body (driven from `run()`), independent of the CFG dataflow, so
    // each await is examined once and no diagnostic is duplicated. They are
    // gated on `isAsync_`, so a synchronous body never reaches them. // per §30.5

    enum class AwaitTaskKind {
        NotTask,    // operand not a Task-family type (e.g. async-iterator await,
                    //   or an untyped operand) — no site rule applies here
        UnitTask,   // `await Task` — no result value, no ownership obligation
        PatternA,   // `await Task<T>` for CLASS T — borrow; needs owning location
        PatternB,   // `await OwnedTask<T>` for class T — ownership transfers;
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
    // `M(await)` / `coll.Add(await)`, an `owned`/`OwnedTask` `return await`,
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
};

} // namespace bezel::ownership
