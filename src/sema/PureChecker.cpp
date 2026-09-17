#include "sema/PureChecker.hpp"

#include <string>
#include <vector>

#include "common/BuiltinRegistry.hpp"
#include "common/OwnershipFacts.hpp"  // fix #3588: the ONE §28.9 trivial-dispose walk
#include "sema/NestedTypeWalk.hpp"  // fix #3597: the shared §11.6 nested-type walk
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "support/Diagnostic.hpp"

namespace bezel::sema {

namespace {

// §22.15 (v0.202, #2052) — the closed-body checker. A `PureBodyWalker` is
// constructed per pure member and walks its statements/expressions.
//
// Slice 2 covers the load-bearing rules (frame-local mutation root; calls
// to non-pure members; class construction outside the throw carve-out; the
// consuming discard; IEnumerable for-source rejection). Lambda bodies are
// NOT checked (§22.15 rule 4: a lambda body defined inside a pure body is
// a separate function whose later invocation happens at the caller's
// effect discipline).
class PureBodyWalker {
public:
    PureBodyWalker(SemaContext& ctx, MemberSymbol& member,
                   const std::string& memberLabel, bool isConstructor)
        : ctx_(ctx), member_(member), memberLabel_(memberLabel),
          isConstructor_(isConstructor) {}

    void checkStmts(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void checkOneStmt(const ast::Stmt* s);
    void checkExpr(const ast::Expr* e);

    // §22.15 "Pure constructors" (v0.202, #2052, #2061): a pure constructor's
    // explicit `base(…)`/`this(…)` chain call (§11.1) must target a pure
    // constructor — the chain target's purity propagates upward. The chain
    // surfaces as the body's first ExpressionStmt whose expression is a
    // CallExpr on a `this` or `base` identifier. We resolve the target
    // constructor by CLASS IDENTITY (the same class for `this(…)`; the
    // class-typed first base for `base(…)`) and arity — there is no
    // ambiguity in the pure-classification call: the class narrows the
    // search, the call's argument count picks the single overload. Reports
    // a §22.15 diagnostic when the resolved target is not pure-declared.
    void checkPureCtorChainPurity(const ast::ConstructorDecl& cd,
                                  const ConstructorSymbol& cs);

    // §22.16 (v0.203, #2053): walk each contract clause's expression under
    // the §22.15 pure-body rules APPLIED EXPRESSION-WISE, with the clause-
    // specific amendments: `throw` is rejected (the carve-out's reverse —
    // the mechanism, message, and blame of a violation belong to §22.16's
    // machinery, not to a clause's own throw), and `await` is rejected on
    // every member (a clause is not part of an async body's statement
    // sequence). Purity's other rules (frame-local mutation, calls to
    // pure members, etc.) apply unchanged. Stores and owned-consuming
    // operations cannot be spelled in an expression, so the borrow-only
    // guarantee is the sum of these classifications (§22.16 "Clause typing:
    // bool, pure, borrow-only").
    void checkContractClauses(
        const std::vector<ast::ContractClause>& clauses);

    // §22.16: walk an `invariant` member's expression under the same clause
    // rules (throw/await rejected, pure-body rules expression-wise).
    void checkInvariantExpr(const ast::Expr& e);

private:
    SemaContext& ctx_;
    MemberSymbol& member_;
    std::string memberLabel_;       // "method `M`" / "property `P`" / etc.
    bool isConstructor_;            // §22.15 birth-store rule admissions
    // §22.16 (v0.203, #2053): when true, this walker is checking a contract
    // clause / invariant expression — `throw` is rejected (the carve-out does
    // NOT apply to clauses), and the violation's `operation` prefix is
    // "a contract clause's expression" rather than "a pure body".
    bool clauseMode_ = false;

    // §22.15 "the closed list's contrapositive": any store whose target
    // root leaves the frame is rejected (instance/static field, indexer
    // slot, dereferenced class reference). In a pure constructor the
    // birth-store rule admits stores to fields of `this`.
    void checkAssignmentTarget(const ast::Expr& target);

    // Walk an access-path root: returns the Symbol* the path is anchored on
    // (LocalSymbol / ParameterSymbol / FieldSymbol / etc.), or nullptr if
    // the path doesn't have an identifier-anchored root.
    Symbol* targetRootSymbol(const ast::Expr& target);

    void reportPureViolation(SourceLocation loc,
                             const std::string& operation,
                             const std::string& detail);

    // §22.15 throw carve-out: `new E(args)` admitted iff E's invoked ctor
    // is pure AND E is trivial-dispose. Returns true iff admitted.
    bool throwCarveOutAdmits(const ast::NewExpr& ne);

    // §22.15 trivial-dispose — the exact inverse of
    // `common::isNonTrivialDisposeClass`, which is what it now calls (#3588).
    // A class is trivial-dispose iff it has no destructor, no owning inline
    // array (§15.1.1), no captured `owned` primary-ctor parameter (§22.10),
    // and no non-static `owned` field OF NON-TRIVIAL-DISPOSE TYPE (§28.9's
    // recursive clause, fix #3647), anywhere in its base chain.
    bool isTrivialDisposeClass(const TypeRef* rt) const;

    // §22.15 rule 6: a call to a member not pure-declared / pure-classified
    // is rejected. Returns true iff admitted.
    bool callTargetIsPure(Symbol* calleeSym);

    // narrow #3610 (§22.15 rule 6, v0.286): rule 6's admitted call forms name
    // "method, **operator**, accessor read, or constructor" — the operator
    // form was the one this walker never asked about. A §25 user-declared
    // overload is a call to a `MethodSymbol`, and `ExprTC::typeBinary` /
    // `typeUnary` ALREADY record that symbol on the operator node
    // (`setExprSymbol(&be, opm)` — ExprTypecheck_Exprs.cpp:4688, :4809, :5894),
    // so the question is answerable from a fact the compiler already computes.
    // Routed through `callTargetIsPure`, the one authority fix #3609 left
    // behind, rather than a sixth spelling of "is this member pure".
    //
    // `opSym` is null for every BUILT-IN operator on primitives, enums, and
    // `string` (`findBinaryOperator`/`findUnaryOperator` resolve no symbol
    // there) — rule 3 material, not a call, and deliberately untouched.
    void checkOperatorOverloadPurity(Symbol* opSym, std::string_view opTok,
                                     SourceLocation loc);

    // §22.15 "Members classified by rule" (#2061, item 2): a record's
    // synthesized Equals/GetHashCode/ToString are pure-classified iff every
    // field's equality dispatch target is pure (auto-hashable form).
    // The synthesized-positional-ctor / with-copy forms are matched by
    // `recordConstructionIsPure`. Returns true when the call IS to a
    // synthesized record member AND the record is auto-hashable
    // (class-identity check); false otherwise. Sets
    // *outIsSynthesizedRecordMember to true when the call resolves to a
    // record-synthesized member regardless of purity (so the caller can
    // distinguish "non-record call — fall through" from "record-synth
    // but not pure-classified — report").
    bool recordSynthesizedCallIsPure(const ast::CallExpr& ce,
                                     bool* outIsSynthesizedRecordMember);

    // §22.15 "Pure constructors" + "Members classified by rule" (#2061,
    // item 2): record construction `new R(args)` is admitted in a pure body
    // iff the invoked constructor is compiler-synthesized-pure (a POSITIONAL
    // record — class-identity check, not arity-based) OR a user-declared
    // pure constructor with matching arity (body records have no synthesized
    // ctor). Returns true iff the construction is provably pure-classified;
    // returns false on a record construction that does not satisfy this
    // rule (caller may then report).
    bool recordConstructionIsPure(const ast::NewExpr& ne);

    // §22.15 rule 5: a `for` loop source must not construct an enumerator.
    // Range<T> of primitive steppable type, inline arrays, spans, and
    // value-typed OwningBuffer<T> are admitted; IEnumerable<T> (including
    // Array<T> and string) is rejected.
    bool forSourceAdmitsEnumerator(const ast::Expr& iterable) const;

    // §22.15 rule 5: is `rt` an IEnumerable<T>-typed source that the `for`
    // desugaring expands into an enumerator-constructing loop?
    bool isEnumeratorConstructingSource(const TypeRef* rt) const;

    // fix #3279 (v0.263, stdlib §9.8 + spec §22.15): the `in` membership
    // operator (§17.5) is semantically `r.Contains(value)`. This helper
    // reports whether the receiver `r`'s `Contains` member is pure-
    // classified for the purposes of §22.15's calls-to-pure-members rule.
    // Admitted shapes: Range<T> (intrinsic-purity contract; its Contains is
    // pure-declared in stdlib §9.8), string and the various ReadOnlySpan /
    // ArraySpan variants (Contains is pure-declared in stdlib §3.3 and
    // §4.14), and any user-declared type whose `Contains(value: T)` member
    // carries the `pure` modifier (looked up on the receiver's member
    // list). All other receivers (Array<T>, OwningArray<T>, user-defined
    // non-pure Contains, unresolved) are rejected — the operator is
    // semantically an impure call and §22.15 admits it only when its
    // callee is pure-classified.
    bool inOperatorCalleeIsPure(const ast::BinaryExpr& be) const;
};

bool PureBodyWalker::isTrivialDisposeClass(const TypeRef* rt) const {
    // fix #3588 (§22.15 / §28.9 / §2.8.2, v0.286): ONE body, in `common/`.
    //
    // What stood here was a second, hand-maintained copy of
    // `common::isNonTrivialDisposeClass` written inverted — same cycle guard,
    // same 4096-iteration cap, same member scan (destructor / owning inline
    // array §15.1.1 / captured `owned` primary-ctor parameter §22.10 /
    // non-static `owned` field), same base-chain step, same carried-verdict
    // short-circuit. A normalizing diff of the two bodies at 11920b8 found NO
    // classification arm that differed: every hunk was `return false` where the
    // shared predicate writes `reject = true; break;`. The comment that used to
    // stand on that copy claimed it was "kept in sync with the predicate's
    // shape by construction" — it was kept in sync by hand, and the two edits
    // that had to touch it (#2666's inline-array arm, #3223's carried verdict)
    // are the record of that. Delegation makes the claim true.
    //
    // §22.15's throw carve-out wants the CARRIED imported verdict — that is
    // fix #3223's whole subject (an imported destructor-bearing exception type
    // must not be admitted at `throw new E()`), and it is the shared
    // predicate's default.
    //
    // fix #3647 (§28.9 / §3.6 rule 2, v0.286): the TypeRef+SemaContext
    // overload — the sole caller (`throwCarveOutAdmits`) already has the
    // resolved TypeRef (`rt`, from `ctx_.resolvedType(*ne.type)`) in scope,
    // so this passes it straight through rather than fabricating a synthetic
    // one; `ctx_` is this walker's own SemaContext member.
    return !common::isNonTrivialDisposeClass(rt, ctx_);
}

bool PureBodyWalker::callTargetIsPure(Symbol* calleeSym) {
    // §22.15 rule 6: a call is admitted iff the declared member on the
    // receiver's static type is pure-declared or pure-classified. For
    // Slice 2 we check the explicit `pure` marker (declared) —
    // pure-CLASSIFICATION (auto-getters, record-synthesized members) lands
    // in a later slice.
    if (!calleeSym) return false;  // conservatively reject unknown
    // fix #3609 (§22.15, v0.286): through the one predicate. This arm was the
    // WIDEST of the five spellings of "is this member pure" — symbol field, the
    // `ast::OperatorDecl` bool, and the modifier scan — so `memberIsPure` is it
    // verbatim plus a `ConstructorDecl` arm that a `MethodSymbol` can never
    // reach. No admitted program changes verdict here.
    if (auto* ms = dynamic_cast<MethodSymbol*>(calleeSym))
        return memberIsPure(ms);
    // fix #3174 (§22.15, v0.254): through the one predicate. This site was
    // already both-sources; it is routed here so the chain-purity site below
    // cannot drift away from it again.
    if (auto* cs = dynamic_cast<ConstructorSymbol*>(calleeSym))
        return constructorIsPure(cs);
    // Property/indexer accessor reads — §22.15 rule 6's accessor-read arm. Tight
    // before #3630 this arm admitted EVERY property read unconditionally, on
    // the rationale that pure-classification (auto-getters, record-synthesized
    // members) lands later. With #3609 the declared-purity check
    // (`propertyGetterIsPure` — both spellings: property-level `pure`, the
    // accessor-level `pure get`) is now both-sources — local AST via the
    // `pd->accessors` walk, imported via the new `ps->accessorIsPure` slot
    // populated by `ModuleLoader` from `MetaMember::accessorIsPure` (#3618) —
    // so the declared side of rule 6 can fire here without depending on the
    // still-deferred classification side. The MemberAccessExpr site in
    // `checkExpr` is the OTHER arm (the property READ spelling — `c.Value`
    // inside a pure body); together they close the accessor-read arm on
    // every spelling of an accessor reach.
    if (auto* ps = dynamic_cast<PropertySymbol*>(calleeSym))
        return propertyGetterIsPure(ps);
    // Intrinsic / builtin calls (no AST decl) are admitted — §22.15
    // "Intrinsic purity (the stdlib contract)" trusts stdlib markers, and
    // Slice 4 covers the stdlib annotation wave.
    return true;
}

// narrow #3610 (§22.15 rule 6, v0.286): the operator arm of rule 6.
//
// WHAT WAS MISSING. Rule 6 admits "a call — method, **operator**, accessor
// read, or constructor in the admitted positions — … iff the member declared
// on the receiver's static type … is `pure`-declared or pure-classified".
// `checkExpr`'s BinaryExpr arm implemented rule 3 (built-in operators on
// primitives, enums and `string`) by walking the operands, and the only
// operator it ever classified was `in` — which is not an overload at all but
// the §17.5 `Contains` call. A §25 user-declared `operator -` therefore ran
// inside a pure body, locally and across the artifact boundary alike.
//
// WHY THIS IS NOT A SIXTH PREDICATE. The overload is already resolved and
// already recorded: `ExprTC::typeBinary` calls `ctx_.setExprSymbol(&be, opm)`
// for a named-type overload and for a `where T: I<T>` constraint static
// operator, and `ExprTC::typeUnary` does the same for `&ue`. Nothing else
// ever puts a symbol on a BinaryExpr/UnaryExpr node, so "the node carries a
// MethodSymbol" IS "this operator is a user-declared overload". The purity
// question then goes to `callTargetIsPure`, unchanged.
//
// NARROW, DELIBERATELY. This covers the operator EXPRESSION forms only.
// The STATEMENT forms — `a += b` and `t++` through a §25 overload — record no
// symbol anywhere (their overload is re-looked-up by hand at five separate
// sites), so closing them needs the resolution recorded first: #3628.
// Rule 6's "accessor read" arm stays as it was — `callTargetIsPure` still
// admits every `PropertySymbol` unconditionally — because an accessor-level
// `pure get` has no `MetaMember` slot (#3618), so tightening it today would
// reject a cross-module property read whose local twin compiles: #3630.
void PureBodyWalker::checkOperatorOverloadPurity(Symbol* opSym,
                                                 std::string_view opTok,
                                                 SourceLocation loc) {
    auto* ms = dynamic_cast<MethodSymbol*>(opSym);
    if (!ms) return;   // rule 3: a built-in operator resolves no symbol.
    if (callTargetIsPure(ms)) return;
    std::string owner = ms->parent ? std::string(ms->parent->name)
                                   : std::string("<type>");
    reportPureViolation(
        loc, std::string("call to `operator ") + std::string(opTok) + "`",
        std::string("\xC2\xA7""25's `") + std::string(opTok) +
        "` on this operand type is the call `" + owner + "::operator " +
        std::string(opTok) + "`, and \xC2\xA7""22.15's calls-to-pure-members rule "
        "admits an operator call only when the resolved overload is "
        "pure-declared \xE2\x80\x94 declare it `pure`, or use a `pure` overload");
}

// §22.15 "Members classified by rule" (#2061, item 2): a record's
// synthesized Equals/GetHashCode/ToString are pure-classified iff the
// record class is auto-hashable — every constituent operation
// (field read + the dispatched equality/hash/ToString on each field's
// type) is itself pure. The §9.2/§9.3 admissibility helpers in
// SymbolCollection.cpp already implement this recursive field rule;
// PureChecker reuses them rather than duplicating the walk.
//
// Match BY RECORD CLASS IDENTITY — not by arity. A record's synthesized
// Equals/GetHashCode/ToString exist for the WHOLE record class (every
// positional or body record has them); the receiver's static type alone
// determines whether the call targets a synthesized record member. The
// arity of the call is irrelevant (these members take `Equals(R)` /
// `int GetHashCode()` / `string ToString()`, all fixed signatures).
//
// Returns true when the call IS to a record-synthesized member AND the
// record is auto-hashable. Sets *outIsSynthesizedRecordMember
// to true when the call resolves to a synthesized record member
// REGARDLESS of purity, so the caller can distinguish "non-record call —
// fall through" from "record-synthesized but not pure-classified — report".
bool PureBodyWalker::recordSynthesizedCallIsPure(
    const ast::CallExpr& ce, bool* outIsSynthesizedRecordMember) {
    if (outIsSynthesizedRecordMember) *outIsSynthesizedRecordMember = false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get());
    if (!ma || !ma->target) return false;
    // §22.15: synthesized set here is Equals/GetHashCode/ToString. The
    // synthesized positional ctor is reached via `new R(...)` (handled
    // by recordConstructionIsPure) and the with-copy via `r with { ... }`
    // (a pure-body WithExpr walks its source + values recursively).
    if (ma->member != "Equals" && ma->member != "GetHashCode" &&
        ma->member != "ToString")
        return false;
    const TypeRef* recvTy = ctx_.exprType(*ma->target);
    if (!recvTy || recvTy->kind != TypeRef::Kind::Named ||
        !recvTy->declaration) return false;
    TypeSymbol* rec = recvTy->declaration;
    // Class-identity check: a synthesized record member exists for the
    // WHOLE record class (pos or body); the receiver's record-typing
    // confirms the call shape.
    if (rec->kind != ast::TypeDeclKind::Record) return false;
    if (outIsSynthesizedRecordMember) *outIsSynthesizedRecordMember = true;
    // fix #3631 (§9.1 / §9.2, v0.286): route through the ONE both-sources
    // positional-record accessor (see its doc comment in Symbol.hpp) rather
    // than re-deriving the `hasPositionalFields` / AST-decl-fallback shape
    // here. BEHAVIOR-FREE: this site already checked `hasPositionalFields`
    // first with the same `ast::TypeDecl` fallback, so `recordIsPositional`
    // admits exactly what the inline check admitted, nothing more — pure
    // mechanical dedup, verified byte-for-byte against the old inline form
    // before routing (§6b).
    if (recordIsPositional(rec))
        return isPositionalRecordEqualityBearing(rec, ctx_);
    return isAutoEqualityBearingBodyRecord(rec, ctx_);
}

// Resolve a body record's user-declared constructor of matching arity, or
// a positional record's user-declared override of the synthesized ctor.
// Returns nullptr when no ctor of that arity exists OR when the overload
// set is ambiguous (handled by the overload-resolution pass, not here).
// The pure-classification rule keys on existence + purity, not on
// overload tie-breaking.
//
// fix #3219 (§22.15, v0.254): the candidate walk used to `continue` on any
// `ConstructorSymbol` whose `decl` was null — fine for a local record, but
// FAILS for an imported record whose ctor names a SIBLING type:
// `parseAndAttachCtorBody` re-runs `resolveTypes` over a synthetic source that
// declares only the one type (#2290 bug 2 / #710), so a sibling-naming ctor's
// `decl` is left null and this walk would drop it silently. The drop produced
// two measured defects:
//
//   * a body record with a `pure` ctor naming a sibling  — FALSE REJECT
//     (the ctor is `pure` but the walk misses it, so `recordConstructionIsPure`
//      returns false and §22.15 fires).
//   * a positional record with an impure shadowing ctor naming a sibling —
//     FALSE ACCEPT (§22.15 soundness: the synthesized positional ctor's
//     auto-hashable classification is used instead of the impure user-declared
//     shadow).
//
// (For the positional case, an arity-mismatch with the synthesized positional
// ctor also fails the §22.5 arity check at the call site — `checkNewExprArgs`
// does not consult user-declared ctors on a positional record — so the second
// shape is now unreachable for the positional case. The widening below closes
// it wherever it IS reachable, e.g. body records, future shapes that route
// through this walk.)
//
// The widening is GATED: when any decl-attached candidate of matching arity
// exists, the decl-attached path takes precedence so every program the old
// code answered keeps that answer. When all decl-attached candidates are
// absent, the symbol-side reading (`constructorIsPure`) is consulted;
// `applyBehavior` sets `cs->isPure` on every imported ctor (ModuleLoader.cpp,
// family M8). The single new admission is: "a matching, non-static ctor
// SYMBOL with no attached declaration".
//
// The matching concern that previously kept this walk unconverted — widening
// could turn a single resolved match into an ambiguity — is real but narrow:
// `matching` exceeds 1 only when a record declares two same-arity ctors,
// which is already an overload ambiguity.
static const ConstructorSymbol* findUserCtorByArity(
    const TypeSymbol* rec, std::size_t arity) {
    int matching = 0;
    const ConstructorSymbol* declMatch = nullptr;
    int symMatching = 0;
    const ConstructorSymbol* symMatch = nullptr;
    for (Symbol* m : rec->members) {
        auto* cs = dynamic_cast<ConstructorSymbol*>(m);
        if (!cs || cs->isStatic) continue;
        if (cs->params.size() != arity) continue;
        if (dynamic_cast<const ast::ConstructorDecl*>(cs->decl)) {
            ++matching;
            declMatch = cs;
        } else {
            ++symMatching;
            symMatch = cs;
        }
    }
    if (matching > 0) return matching == 1 ? declMatch : nullptr;
    return symMatching == 1 ? symMatch : nullptr;
}

// §22.15 "Members classified by rule" (#2061, item 2): record construction
// `new R(args)` is admitted in a pure body iff the invoked constructor is
// compiler-synthesized-pure (positional record — class-identity check) OR
// `pure`-declared. The synthesized positional ctor copies args into
// positional fields — a birth-store, pure for the same reason a pure
// ctor's field stores are — and is pure-classified iff the record class
// is auto-hashable. A body record has no synthesized ctor: its
// authored ctor of matching arity must carry `pure`. The arity match here
// is for resolving WHICH user-declared ctor is invoked (a class-identity
// question per record class); the PURITY of the construction is the
// CLASS's classification, not an arity-dependent verdict.
//
// Returns true iff the NewExpr constructs a record AND the invoked ctor is
// pure-classified; false otherwise (caller may then report a §22.15
// violation).
bool PureBodyWalker::recordConstructionIsPure(const ast::NewExpr& ne) {
    if (!ne.type) return false;
    const TypeRef* rt = ctx_.resolvedType(*ne.type);
    if (!rt || rt->kind != TypeRef::Kind::Named || !rt->declaration)
        return false;
    TypeSymbol* rec = rt->declaration;
    if (rec->kind != ast::TypeDeclKind::Record) return false;
    // CLASS-IDENTITY: a positional record has a synthesized ctor whose
    // purity is the record-class's auto-hashable classification.
    //
    // fix #3631 (§9.1 / §9.2, v0.286): route through the ONE both-sources
    // positional-record accessor rather than re-deriving the
    // `hasPositionalFields` / AST-decl-fallback shape here (see Symbol.hpp's
    // doc comment on `recordIsPositional`). BEHAVIOR-FREE: this site already
    // read `hasPositionalFields` first with the identical `ast::TypeDecl`
    // fallback, so this is a pure mechanical dedup, not a widening (§6b).
    const bool isPositional = recordIsPositional(rec);
    const std::size_t arity = ne.args.size();
    const ConstructorSymbol* userCtor =
        findUserCtorByArity(rec, arity);
    if (isPositional) {
        // When a user declared a ctor with matching arity, it shadows the
        // synthesized positional one (§9.5); that ctor must carry `pure`.
        // Otherwise the synthesized positional ctor is invoked — its
        // purity follows from the record's CLASS identity, not from any
        // arity-specific check.
        if (userCtor) return constructorIsPure(userCtor);
        return isPositionalRecordEqualityBearing(rec, ctx_);
    }
    // Body record: there is no synthesized ctor, so the invoked user-
    // declared ctor must be `pure`-declared.
    if (!userCtor) return false;
    return constructorIsPure(userCtor);
}

// §22.15 "Pure constructors" (#2061, item 1): a pure ctor's explicit
// `base(...)`/`this(...)` chain target must itself be pure (§22.15:
// "A pure constructor's `base(…)`/`this(…)` chain target must itself be
// pure"). The chain call surfaces as the body's first ExpressionStmt whose
// expression is a CallExpr on `this` or `base` (Node.hpp CtorChain).
//
// Resolution narrows by class: `this(...)` targets a ctor in the SAME
// class as this ctor; `base(...)` targets a ctor in the first class base.
// Within the narrowed class, the call's argument count picks the single
// overload — there is no further overload ambiguity in the chain call.
void PureBodyWalker::checkPureCtorChainPurity(
    const ast::ConstructorDecl& cd, const ConstructorSymbol& cs) {
    if (cd.body.empty() || !cd.body.front()) return;
    auto* es = dynamic_cast<const ast::ExpressionStmt*>(cd.body.front().get());
    if (!es || !es->expr) return;
    auto* call = dynamic_cast<const ast::CallExpr*>(es->expr.get());
    if (!call || !call->callee) return;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(call->callee.get());
    if (!id) return;
    const bool isThisChain = (id->name == "this");
    const bool isBaseChain = (id->name == "base");
    if (!isThisChain && !isBaseChain) return;  // not a chain call
    // CLASS-IDENTITY narrowing: `this(args)` → same class (cs.parent);
    // `base(args)` → first class base (mirrors codegen's chain target
    // resolution). The implicit Object base has no constructor symbols —
    // admit silently when no explicit base exists.
    TypeSymbol* targetClass = nullptr;
    if (isThisChain) {
        targetClass = cs.parent;
    } else {
        for (TypeSymbol* b : cs.parent->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) {
                targetClass = b;
                break;
            }
    }
    if (!targetClass) return;
    const std::size_t arity = call->args.size();
    // fix #3174 (§22.15, v0.254 — family M8 of umbrella #3149/#3152): resolve
    // the chain target to its SYMBOL, not to an `ast::ConstructorDecl*`.
    //
    // `ModuleLoader` attaches a re-parsed `ast::ConstructorDecl` only for a
    // GENERIC type or a RECORD, so for a non-generic imported base class every
    // candidate's `decl` is null; the old `if (!cd2) continue;` therefore
    // dropped the entire candidate set and the "no matching ctor" early-out
    // below admitted the program silently. MEASURED on `ec6a02f`: a local
    // `pure constructor` chaining `base(n)` to an imported IMPURE base ctor
    // compiled with exit 0, while the character-identical single-module twin
    // was rejected with this function's own diagnostic.
    //
    // ⚠ THE WIDENING, EXACTLY. `declTarget` keeps the OLD selection verbatim —
    // last matching candidate that carries an `ast::ConstructorDecl` — and is
    // preferred whenever it exists, so every program in which the old code
    // found a target keeps that target and that verdict. `symTarget` is the
    // strictly larger set (it drops only the `decl != nullptr` filter), and it
    // is consulted ONLY when the old selection found nothing. So the single new
    // admission is: "a matching, non-static, non-self constructor SYMBOL with
    // no attached declaration" — which is exactly the imported non-generic
    // class, the case this fix exists for. In particular the old code's
    // last-match-wins tie-break is untouched wherever it had a match to break.
    const ast::ConstructorDecl* declTarget = nullptr;
    const ConstructorSymbol* symTarget = nullptr;
    for (Symbol* m : targetClass->members) {
        auto* ct = dynamic_cast<ConstructorSymbol*>(m);
        if (!ct || ct->isStatic) continue;
        if (ct == &cs) continue;  // `this(...)` cannot recurse to itself
        if (ct->params.size() != arity) continue;
        symTarget = ct;
        if (const auto* cd2 =
                dynamic_cast<const ast::ConstructorDecl*>(ct->decl))
            declTarget = cd2;
    }
    if (!symTarget) {
        // No matching ctor — primary-ctor class chain (no
        // ConstructorSymbol: the parser synthesizes the chain's body),
        // or an arity mismatch already caught by sema. Admit silently:
        // the chain target's purity is provable by the primary-ctor
        // birth-store rule, and an arity mismatch is not a §22.15 defect.
        //
        // ⚠ This branch's own comment used to justify an outcome it did not
        // describe: before #3174 it also ran for every imported base class,
        // which is neither a primary-ctor chain nor an arity mismatch. It now
        // means what it says.
        return;
    }
    if (declTarget) {
        if (declTarget->isPure) return;  // chain target is pure — OK
    } else if (constructorIsPure(symTarget)) {
        return;  // imported chain target is pure — OK
    }
    reportPureViolation(
        call->span.begin, "constructor chain call",
        std::string(isThisChain ? "this(" : "base(") + "...) target is "
        "not pure — a pure constructor's chain target must itself be "
        "declared `pure`");
}

bool PureBodyWalker::isEnumeratorConstructingSource(
    const TypeRef* rt) const {
    // §22.15 rule 5: IEnumerable<T> sources construct an enumerator class
    // instance — Array<T>, string, IEnumerable<T>, and the stdlib
    // collection wrappers. Range<T>, inline arrays, spans, and value-typed
    // OwningBuffer<T> use direct counted loops.
    if (!rt) return false;
    std::string_view head = rt->headName();
    if (head == "IEnumerable" || head == "Array" || head == "string" ||
        head == "List" || head == "OwningList" || head == "OwningArray" ||
        head == "OwningDictionary" || head == "OwningSet" ||
        head == "Dictionary" || head == "Set" || head == "Stack" ||
        head == "Queue" || head == "LinkedList" || head == "OwningStack" ||
        head == "OwningQueue" || head == "OwningLinkedList")
        return true;
    return false;
}

// fix #3279 (v0.263, stdlib §9.8 + spec §22.15): the `in` membership
// operator's callee is `r.Contains(value)` (spec §17.5). The
// calls-to-pure-members rule (spec §22.15) admits the operator iff the
// callee is pure-classified.
//
// Pure-classified callees:
//   - Range<T>: pure-declared on the stdlib intrinsic (stdlib §9.8;
//     carried by the `pure` modifier on the Range declaration).
//   - string: pure-declared on `string.Contains` (stdlib §9.6).
//   - ReadOnlyStringSpan: pure-declared on `Contains` for the three
//     `ReadOnlyStringSpan` overloads (stdlib §3.3).
//   - User-declared types: pure iff a member named `Contains` exists on
//     the receiver's symbol-side member list AND that member is
//     pure-declared (carries `pure` in its modifiers list — the same
//     check `callTargetIsPure` runs at a CallExpr to `Contains`).
//
// Rejected:
//   - Array<T>, OwningArray<T> (Contains not pure-declared in stdlib).
//   - Any user-declared type whose `Contains` does NOT carry `pure`.
//   - Unresolved receivers (the resolution error was reported elsewhere;
//     this check does not double-report).
//
// The helper does NOT descend into codegen's `in`-operator lowering
// (which is inlined to a range-membership scan in LlvmBackend_Exprs.cpp).
// The §22.15 rule fires on the SEMANTIC callee — what the operator means,
// not how codegen lowers it — so a future lowering change cannot
// silently relax this check.
bool PureBodyWalker::inOperatorCalleeIsPure(const ast::BinaryExpr& be) const {
    if (!be.right) return true;  // resolution error elsewhere; no double-report
    const TypeRef* recvTy = ctx_.exprType(*be.right);
    if (!recvTy) return true;
    // The receiver's identity for stdlib-type membership testing. Builtin
    // TypeRefs constructed in-place (RangeExpr's Range, the string
    // builtin, ReadOnlyStringSpan) have no astNode and no importedName,
    // so headName() returns empty and we fall back to declaration->name.
    // headName() is the alias-transparent spelling per TypeRef.hpp, which
    // suffices for user-declared Named types and alias-resolved heads.
    std::string_view head = recvTy->headName();
    if (head.empty() && recvTy->declaration)
        head = recvTy->declaration->name;
    // Special cases by receiver head — these are the stdlib-declared
    // pure-Contains types whose purity is a documentation fact, not a
    // symbol-table lookup (the members may not exist as MemberSymbols on
    // the builtin TypeSymbol after ModuleLoader's import-side filtering
    // — see the #3152 umbrella, family F11/F13).
    //
    // Only types whose Contains is `public pure method Contains(...)`
    // in stdlib belong in this list. As of v0.277 the set is exactly
    // {Range<T>, string, ReadOnlyStringSpan}. The Span<T> / ReadOnlySpan<T>
    // family (§4.14) and ArraySpan<T> / ReadOnlyArraySpan<T> (§4.15)
    // declare Contains as a plain `method`, NOT `pure`, so they are
    // EXCLUDED from this list — `x in spanVal` over those receivers is
    // REJECTED by §22.15's calls-to-pure-members rule (the same way
    // `x in arr` over Array<T> is rejected). The BitReader/BitWriter
    // views declare no Contains member at all; their `in` operator is
    // rejected earlier by §17.5's structural lookup, so adding them to
    // this list would be unreachable in practice and misleading.
    if (head == "Range") return true;            // stdlib §9.8
    if (head == "string") return true;           // stdlib §9.6
    if (head == "ReadOnlyStringSpan") return true; // stdlib §3.3
    // Generic user-declared types: walk the receiver's symbol-side member
    // list looking for a `Contains` member and check its `pure` modifier.
    // This matches what `callTargetIsPure` does for an explicit
    // `r.Contains(x)` CallExpr, so the two paths agree (fix #3279's
    // stated invariant: "the marker is what admits a range test inside a
    // contract clause; the same marker admits `r.Contains(x)` in the
    // same position").
    if (recvTy->kind == TypeRef::Kind::Named && recvTy->declaration) {
        for (Symbol* m : recvTy->declaration->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (!ms) continue;
            if (ms->name != "Contains") continue;
            // fix #3609 (§22.15, v0.286): the same predicate as
            // callTargetIsPure's MethodSymbol arm — now literally the same
            // function, which is what the comment here used to claim. It had
            // dropped the `ast::OperatorDecl` arm; that is unreachable behind
            // the `name != "Contains"` filter (an operator symbol is named for
            // its token), so restoring it admits nothing new.
            return memberIsPure(ms);
        }
    }
    // No matching Contains found, or non-Named receiver. Conservatively
    // reject: the operator's semantic callee is unprovably pure.
    return false;
}

bool PureBodyWalker::forSourceAdmitsEnumerator(
    const ast::Expr& iterable) const {
    // Admitted if NOT enumerator-constructing. Range<T>, inline arrays,
    // spans, and value-typed buffers are admitted.
    const TypeRef* rt = ctx_.exprType(iterable);
    return !isEnumeratorConstructingSource(rt);
}

bool PureBodyWalker::throwCarveOutAdmits(const ast::NewExpr& ne) {
    // §22.15 throw carve-out: (a) E's invoked constructor is pure, and
    // (b) E is trivial-dispose. Slice 2 enforces both.
    if (!ne.type) return false;
    const TypeRef* rt = ctx_.resolvedType(*ne.type);
    if (!rt || rt->kind != TypeRef::Kind::Named || !rt->declaration) {
        // Built-in exception types — admitted (§22.15 "Intrinsic purity":
        // every built-in exception's ()/(message) constructors are pure).
        // Conservatively admit unknown new-targets at the throw position so
        // the diagnostic blames the right line; the rule fires elsewhere.
        return true;
    }
    TypeSymbol* targetDecl = rt->declaration;
    if (targetDecl->kind != ast::TypeDeclKind::Class) return false;
    // fix #3647: pass the resolved TypeRef `rt` itself (not just its
    // declaration `targetDecl`), so §3.6 rule 2's per-instantiation
    // substitution can run.
    if (!isTrivialDisposeClass(rt)) return false;
    // (a) the invoked constructor must be pure. For a `new E(args)` form
    // the constructor is resolved by the typecheck pass; we look it up by
    // walking targetDecl's constructors and matching arity. Slice 2 admits
    // conservatively when no pure ctor is provable (the diagnostic still
    // names the rule). A pure ctor present anywhere on the chain admits.
    for (Symbol* m : targetDecl->members) {
        auto* cs = dynamic_cast<ConstructorSymbol*>(m);
        if (!cs) continue;
        // fix #3174 (§22.15, v0.254): through the one predicate — same three
        // sources in the same order this loop already spelled inline.
        if (constructorIsPure(cs)) return true;
    }
    return false;
}

void PureBodyWalker::reportPureViolation(SourceLocation loc,
                                         const std::string& operation,
                                         const std::string& detail) {
    std::string msg = operation + " is not admitted in a pure body";
    if (!detail.empty()) msg += " — " + detail;
    msg += " (\xC2\xA7""22.15)";
    ctx_.diagnostics().report(Severity::Error, loc, msg);
}

Symbol* PureBodyWalker::targetRootSymbol(const ast::Expr& target) {
    // Walk the access path: peel `.member` / `[index]` / `!` to find the
    // identifier-rooted base. Mirrors checkLValueChainNotCallBased.
    const ast::Expr* e = &target;
    for (;;) {
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            e = ma->target.get();
            continue;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            e = ix->target.get();
            continue;
        }
        if (auto* bang = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            e = bang->operand.get();
            continue;
        }
        break;
    }
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e))
        return ctx_.exprSymbol(*id);
    return nullptr;
}

void PureBodyWalker::checkAssignmentTarget(const ast::Expr& target) {
    // §22.15 rule 2 + contrapositive: the target root must be a local
    // variable or by-value parameter, and the access path must traverse
    // only value-typed storage. The first class-typed dereference leaves
    // the frame.
    //
    // Slice 2 implementation: classify by the root symbol's kind. A
    // LocalSymbol or ParameterSymbol root is admitted (frame-local). A
    // FieldSymbol root is admitted only in a pure constructor (birth-store
    // rule, §22.15 "Pure constructors"); otherwise it is a store whose
    // target root is an instance/static field — rejected.
    Symbol* rootSym = targetRootSymbol(target);
    if (!rootSym) {
        // `this.field := ...` is parsed as MemberAccess(IdentifierExpr this,
        // field) — the root is whatever `this` resolves to. Slice 2 keeps
        // this conservative: an unresolvable root in a non-ctor pure body
        // is rejected; the rule blame is on the assignment site.
        if (isConstructor_) return;  // birth-store admission
        reportPureViolation(
            target.span.begin, "assignment",
            "the target root is not a frame-local of this body");
        return;
    }
    if (dynamic_cast<LocalSymbol*>(rootSym)) return;
    if (dynamic_cast<ParameterSymbol*>(rootSym)) return;
    if (auto* fs = dynamic_cast<FieldSymbol*>(rootSym)) {
        // §22.15 birth-store rule: pure-constructor stores to fields of
        // `this` are admitted; static-field stores and instance-field
        // stores outside a pure ctor are rejected.
        if (isConstructor_) {
            bool isStatic = false;
            for (auto mod : fs->modifiers)
                if (mod == "static") isStatic = true;
            if (!isStatic) return;
        }
        reportPureViolation(
            target.span.begin, "assignment",
            std::string("the target root `") + std::string(fs->name) +
            "` is a field (a store outside the frame; pure-constructor "
            "birth-store admits only fields of `this`)");
        return;
    }
    if (auto* ps = dynamic_cast<PropertySymbol*>(rootSym)) {
        // §22.15 birth-store rule (fix #3208): the rule is stated over the
        // target ROOT, not over its spelling — `this.N := n;` and `N := n;`
        // are the same store (§2.7), and the spec admits both spellings of
        // a property write under the same rationale (the property's backing
        // storage is a field of `this`). The previous birth-store arm
        // recognised only FieldSymbol roots, so the bare property spelling
        // fell through to the "not a frame-local" reject even though the
        // `this.`-qualified spelling was silently admitted via the
        // unresolved-root branch above. Admit the PropertySymbol root under
        // the same gate the FieldSymbol arm uses.
        if (isConstructor_) {
            bool isStatic = false;
            for (auto mod : ps->modifiers)
                if (mod == "static") isStatic = true;
            if (!isStatic) return;
        }
        reportPureViolation(
            target.span.begin, "assignment",
            std::string("the target root `") + std::string(ps->name) +
            "` is a property (a store outside the frame; pure-constructor "
            "birth-store admits only properties of `this`)");
        return;
    }
    reportPureViolation(
        target.span.begin, "assignment",
        "the target root is not a frame-local of this body");
}

void PureBodyWalker::checkStmts(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    for (const auto& s : body) {
        if (s) checkOneStmt(s.get());
    }
}

void PureBodyWalker::checkOneStmt(const ast::Stmt* s) {
    if (!s) return;
    // Recurse through control flow.
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
        checkAssignmentTarget(*as->target);
        checkExpr(as->value.get());
        // §22.15 contrapositive: set(owned) stores — `owned` value
        // marker — are disposal-running.
        if (as->valueHasOwnedMarker) {
            reportPureViolation(
                as->span.begin, "owned-consumption assignment",
                "an `owned` marker on the RHS is a transfer (disposal-"
                "running)");
        }
        // fix #3628 (§22.15 rule 6 / §25.1, v0.286): §25.1, verbatim — "the
        // compound forms desugar mechanically to read-modify-write using the
        // underlying binary operator's overload." `t += b` therefore IS the
        // call `T::operator +`, and rule 6 governs it exactly as it governs
        // the spelled-out `t := t + b` (narrow #3610). `ExprTypecheck.cpp`'s
        // compound-assignment existence checks already resolve that overload
        // (including through inheritance and either operand's declaring
        // type, §25.2) and publish it via `setStmtOperatorSymbol` — read here
        // rather than re-resolved, so this is not a sixth predicate. Null for
        // every compound form that resolves no user overload: `:=`, `??=`,
        // a primitive/primitive pair, and an event `+=`/`-=` (§24 — not a
        // real compound assignment, resolves no 2-param `operator`).
        if (Symbol* opSym = ctx_.stmtOperatorSymbol(*as)) {
            std::string_view underlying = as->op;
            if (!underlying.empty() && underlying.back() == '=')
                underlying.remove_suffix(1);  // "+=" -> "+", "mod=" -> "mod"
            checkOperatorOverloadPurity(opSym, underlying, as->span.begin);
        }
        return;
    }
    if (auto* is = dynamic_cast<const ast::IncrementStmt*>(s)) {
        checkAssignmentTarget(*is->target);
        // fix #3628 (§22.15 rule 6 / §17.6, v0.286): §17.6, verbatim — "the
        // statement `x++` is equivalent to `x := T.operator++(x)`" — a call,
        // exactly as rule 6 governs the binary/unary EXPRESSION operator
        // forms (narrow #3610). `ExprTypecheck.cpp`'s IncrementStmt overload-
        // existence check already resolves the `operator ++`/`operator --`
        // and publishes it via `setStmtOperatorSymbol`; null when the target
        // is a built-in numeric (§17.6 category (2) — no overload resolved).
        if (Symbol* opSym = ctx_.stmtOperatorSymbol(*is))
            checkOperatorOverloadPurity(opSym, is->op, is->span.begin);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
        checkExpr(es->expr.get());
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s)) {
        if (rs->value) checkExpr(rs->value.get());
        return;
    }
    if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
        // §22.16 (v0.203, #2053): `throw` is REJECTED in a contract clause
        // (clauseMode_), even when the operand would otherwise satisfy the
        // §22.15 throw carve-out — the carve-out remains available to
        // *callees* of the clause, not to the clause itself.
        if (clauseMode_) {
            reportPureViolation(
                ts->span.begin, "`throw`",
                "a contract clause states a proposition — the mechanism, "
                "message, and blame of a violation belong to §22.16's "
                "machinery, not to the clause's own throw (§22.16)");
            if (ts->value) checkExpr(ts->value.get());
            return;
        }
        // §22.15 throw carve-out: `new E(args)` admitted iff trivial-
        // dispose + pure ctor. Any other throw operand is admitted
        // (bare re-raise, a caught binding read).
        if (ts->value) {
            if (auto* ne = dynamic_cast<ast::NewExpr*>(ts->value.get())) {
                if (!throwCarveOutAdmits(*ne)) {
                    reportPureViolation(
                        ts->span.begin,
                        "class construction at `throw`",
                        "the throw carve-out admits only `new E(...)` "
                        "with a pure constructor of a trivial-dispose "
                        "class");
                }
                // The new-expression's args still need to be checked.
                for (const auto& a : ne->args)
                    if (a.value) checkExpr(a.value.get());
                return;
            }
            checkExpr(ts->value.get());
        }
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
        // §22.15 contrapositive: the consuming discard is rejected
        // (disposal-running / marker-free-position consume).
        reportPureViolation(
            ds->span.begin, "the consuming discard",
            "`_ := expr` is a marker-free consume (§28.3)");
        if (ds->value) checkExpr(ds->value.get());
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s)) {
        if (ifs->ifBranch.condition)
            checkExpr(ifs->ifBranch.condition.get());
        checkStmts(ifs->ifBranch.body);
        for (const auto& el : ifs->elifBranches) {
            if (el.condition) checkExpr(el.condition.get());
            checkStmts(el.body);
        }
        if (ifs->hasElse) checkStmts(ifs->elseBody);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
        if (ws->condition) checkExpr(ws->condition.get());
        checkStmts(ws->body);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
        // §22.15 rule 5: a `for` loop's source must not construct an
        // enumerator. The awaited form is excluded by `pure`+`async`
        // already.
        if (fs->iterable &&
            !forSourceAdmitsEnumerator(*fs->iterable)) {
            reportPureViolation(
                fs->span.begin, "`for` loop source",
                "the source constructs an enumerator (an "
                "IEnumerable<T>-typed source including Array<T> and "
                "string); use an index loop `for var i in 0..<xs.Count`");
        }
        if (fs->iterable) checkExpr(fs->iterable.get());
        if (fs->withExpr) checkExpr(fs->withExpr.get());
        checkStmts(fs->body);
        return;
    }
    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s)) {
        checkStmts(db->body);
        return;
    }
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
        // §22.15 rule 2: local declarations are admitted. The
        // initializer expression still needs to be checked.
        if (ld->initializer) checkExpr(ld->initializer.get());
        // §22.15 contrapositive: `using var` (sync) is disposal-running.
        if (ld->isUsing && !ld->isAwaitUsing) {
            reportPureViolation(
                ld->span.begin, "`using var`",
                "scope-end disposal is disposal-running user code");
        }
        return;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
        if (lc->value) checkExpr(lc->value.get());
        return;
    }
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
        // §22.15 "Where pure is rejected": iterator methods (yield)
        // cannot be pure — Slice 1 already rejected at signature level.
        // A yield surviving into the body check is unreachable in a
        // well-formed program.
        if (ys->value) checkExpr(ys->value.get());
        return;
    }
    if (auto* trys = dynamic_cast<const ast::TryStmt*>(s)) {
        checkStmts(trys->body);
        for (const auto& arm : trys->catches) {
            if (arm.guard) checkExpr(arm.guard.get());
            // CatchArm::body is a single Stmt (usually a DoEndBlock);
            // unwrap one level so its inner statements are visited.
            if (arm.body) {
                if (auto* db = dynamic_cast<ast::DoEndBlock*>(
                        arm.body.get()))
                    checkStmts(db->body);
                else
                    checkOneStmt(arm.body.get());
            }
        }
        if (trys->hasFinally) checkStmts(trys->finallyBody);
        return;
    }
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
        if (cs->scrutinee) checkExpr(cs->scrutinee.get());
        for (const auto& arm : cs->arms)
            checkStmts(arm.stmts);
        return;
    }
    // Break / Continue / etc. — no effect.
}

void PureBodyWalker::checkExpr(const ast::Expr* e) {
    if (!e) return;
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(e)) {
        // §22.15 rule 6 + contrapositive: invoke of function-typed value
        // is prohibited. Detect by: callee has function-typed exprType but
        // no resolved member symbol (i.e. a local/parameter holding a
        // function value).
        Symbol* calleeSym = ctx_.exprSymbol(*ce->callee);
        const TypeRef* calleeTy = ctx_.exprType(*ce->callee);
        const bool isFunctionTypedValue =
            !dynamic_cast<MethodSymbol*>(calleeSym) &&
            !dynamic_cast<ConstructorSymbol*>(calleeSym) &&
            calleeTy && calleeTy->kind == TypeRef::Kind::Function;
        if (isFunctionTypedValue) {
            reportPureViolation(
                ce->span.begin, "invocation of a function-typed value",
                "the value's type carries no effect information");
        } else if (!calleeSym) {
            // §22.15 "Members classified by rule" (#2061, item 2): the
            // callee resolves to no symbol when the call targets a record's
            // synthesized member — codegen synthesizes the body, so sema
            // leaves no MethodSymbol on the callee. Classify the call by
            // the receiver's record-class identity (the synthesized
            // member's purity is a class property, NOT an arity match):
            //   - positional record: synthesized Equals/GetHashCode/
            //     ToString are pure iff the record is auto-hashable
            //     (every field's dispatch is pure under §22.15 rules 1+
            //     3+6, via the §9.2/§9.3 admissibility helpers).
            //   - non-record calls or unresolved callees of other kinds
            //     (codegen fallthrough / member-not-found) are admitted:
            //     the typecheck pass already reported the lookup failure,
            //     and the pure-body checker does not double-report.
            bool isSynthRecMember = false;
            const bool recSynthPure =
                recordSynthesizedCallIsPure(*ce, &isSynthRecMember);
            if (isSynthRecMember && !recSynthPure) {
                reportPureViolation(
                    ce->span.begin,
                    "call to a record-synthesized member",
                    "the receiver record is not auto-hashable — "
                    "a class-typed field's virtual dispatch disqualifies "
                    "the synthesized Equals/GetHashCode/ToString; declare "
                    "`=`/`<>` with `pure` overrides, or simplify the "
                    "record to the auto-hashable form (§22.15 "
                    "\"Members classified by rule\")");
            }
        } else if (!callTargetIsPure(calleeSym)) {
            // §22.15 rule 6: a call to a member not pure-declared or pure-
            // classified. Admit when the callee is unresolved (the
            // typecheck pass already reported the lookup failure).
            if (calleeSym) {
                std::string name;
                if (auto* ms = dynamic_cast<MethodSymbol*>(calleeSym))
                    name = std::string(ms->name);
                else if (auto* cs2 =
                             dynamic_cast<ConstructorSymbol*>(calleeSym))
                    name = std::string(cs2->parent ? cs2->parent->name
                                                   : std::string_view());
                reportPureViolation(
                    ce->span.begin, "call",
                    std::string("the resolved target `") + name +
                    "` is not pure-declared");
            }
        }
        // Recurse into callee + args. The callee itself may be a member
        // access (which involves reads — fine).
        checkExpr(ce->callee.get());
        for (const auto& a : ce->args)
            if (a.value) checkExpr(a.value.get());
        // §22.15 contrapositive: an `owned` argument is owned-consuming.
        for (const auto& a : ce->args) {
            if (a.modifier == "owned" || a.modifier == "scoped" ||
                a.modifier == "ref" || a.modifier == "out" ||
                a.modifier == "out owned" || a.modifier == "out var" ||
                a.modifier == "out using var" ||
                a.modifier == "out owned var") {
                reportPureViolation(
                    ce->span.begin, "call argument",
                    std::string("a `") + std::string(a.modifier) +
                    "` argument is not admitted in a pure call");
            }
        }
        return;
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(e)) {
        // §22.15 contrapositive: class construction outside the throw
        // carve-out is rejected. The throw-carve-out arm in checkStmts
        // handles the `throw new E(...)` case directly; a NewExpr reached
        // here is outside that position.
        // Slice 2 admits value/record/enum construction and trivial-dispose
        // box construction; class construction with a non-pure ctor is the
        // load-bearing rejection. Slice 2's record path was a silent
        // admit; #2061 refines it to require a pure-classified record ctor
        // (synthesized positional ctor when admissible; `pure`-declared
        // user ctor otherwise).
        if (ne->type) {
            const TypeRef* rt = ctx_.resolvedType(*ne->type);
            if (rt && rt->kind == TypeRef::Kind::Named &&
                rt->declaration &&
                rt->declaration->kind == ast::TypeDeclKind::Class) {
                reportPureViolation(
                    ne->span.begin, "class construction",
                    "`new C(...)` runs the constructor and destructor of "
                    "user code (admitted only at the throw operand; the "
                    "throw carve-out requires a pure ctor + trivial-"
                    "dispose class)");
            } else if (rt && rt->kind == TypeRef::Kind::Named &&
                       rt->declaration &&
                       rt->declaration->kind == ast::TypeDeclKind::Record) {
                // §22.15 "Members classified by rule" (#2061, item 2):
                // record construction `new R(args)` is admitted iff the
                // invoked ctor is compiler-synthesized-pure (positional
                // record auto-hashable) or `pure`-declared. The
                // classification is a class-identity property of the
                // record, not an arity match — see
                // recordConstructionIsPure above.
                if (!recordConstructionIsPure(*ne)) {
                    reportPureViolation(
                        ne->span.begin, "record construction",
                        "the invoked constructor is not pure-declared and "
                        "the record is not of the synthesized-pure "
                        "positional form (§22.15 \"Members classified by "
                        "rule\" — declare the ctor `pure`, or drop the "
                        "disqualifying class-typed field so the record is "
                        "auto-hashable)");
                }
            }
        }
        for (const auto& a : ne->args)
            if (a.value) checkExpr(a.value.get());
        return;
    }
    if (auto* ce2 = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
        checkExpr(ce2->callee.get());
        for (const auto& a : ce2->args)
            if (a.value) checkExpr(a.value.get());
        return;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
        // §22.15 rule 6 accessor-read arm (fix #3630, v0.286): a property READ
        // spelled as `c.Value` (a MemberAccessExpr, NOT a CallExpr — property
        // getters are NOT spelled as `c.Value()` in Bezel) is a call to the
        // property's getter accessor. The CALL-shape case `c.Foo()` reaches
        // `callTargetIsPure` via the CallExpr arm above; this is the
        // MEMBER-ACCESS shape, which the previous implementation silently
        // admitted by descending only into the target. Resolve the
        // MemberAccessExpr's member via `ctx_.exprSymbol` — ExprTypecheck
        // populates it from `resolveMemberAccess` (ExprTypecheck_Exprs.cpp
        // ~3437, which mirrors the call-resolution path). For an INDEXER read
        // (`obj[i]`, the IndexExpr arm below), the route through here is NOT
        // taken — indexer reads are themselves IndexExpr nodes, and get
        // their OWN rule-6 accessor-read check in the IndexExpr arm below
        // (fix #3721), asking the SAME `propertyGetterIsPure` predicate. A
        // non-property member (field, method-of-target-as-value, etc.) is
        // NOT this rule's concern — field reads are the frame's
        // responsibility, and methods are reached via CallExpr, never
        // MemberAccessExpr.
        if (auto* ps = dynamic_cast<PropertySymbol*>(ctx_.exprSymbol(*e))) {
            if (!propertyGetterIsPure(ps)) {
                reportPureViolation(
                    e->span.begin, "accessor read",
                    std::string("the resolved target `") +
                        std::string(ps->name) +
                        "` is not pure-declared (§22.15 rule 6 / #3630)");
            }
        }
        checkExpr(ma->target.get());
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        // fix #3721 (§22.15 rule 6, v0.286): the INDEXER twin of the
        // MemberAccessExpr arm above. `obj[i]` on a class receiver with a
        // user-declared indexer is a call to that indexer's `get` accessor
        // — rule 6's "accessor read" form — exactly like `c.Value` is a
        // call to a named property's getter, just spelled with brackets
        // instead of a dot. The previous implementation here only
        // descended into the target and the index expressions, so a
        // non-`pure` indexer getter's side effects ran unobserved inside a
        // `pure` body (issue's measured repro: compile exit 0, run exit 4
        // — the getter's `Calls := Calls + 1` executed inside `pure P`).
        //
        // NO NEW RECORDING SITE, NO SIXTH PREDICATE. #3721's own
        // structural note claimed no writer ever attaches a symbol to an
        // `ast::IndexExpr` — REFUTED: `ExprTypecheck_Exprs.cpp`'s
        // `visitExpr` IndexExpr arm already calls
        // `ctx_.setExprSymbol(&e, ps)` (fix #2530, §22.3/§23) for the
        // user-declared-indexer branch — the grep the issue based that
        // claim on (`setExprSymbol(&ix\|setExprSymbol(ix`) missed it
        // because the call spells the node `&e` (the shared `ast::Expr&`
        // parameter), not `&ix`. That attachment already reaches BOTH a
        // local and an imported indexer identically (the same
        // `tt->declaration->members` walk resolves both — #2414's comment
        // there records the imported-candidate fix), so
        // `ctx_.exprSymbol(*e)` already returns the resolved
        // `PropertySymbol` here, same as the MemberAccessExpr arm. The
        // purity classification itself is also already both-sources:
        // `PropertySymbol::accessorIsPure` (Symbol.hpp) is populated
        // identically for a Property or an Indexer by
        // SymbolCollection.cpp's single `PropertyDecl` arm (`pd->isIndexer`
        // is just a bit on the same node) locally, and by
        // `ModuleLoader.cpp`'s Indexer case (`p->accessorIsPure =
        // mm.accessorIsPure`, mirroring the Property case) across the
        // module boundary — `ModuleMetadataBuilder.cpp`'s exporter walk for
        // `MetaMember::accessorIsPure` (#3618) already says explicitly
        // "Covers both Property AND Indexer". So `propertyGetterIsPure`,
        // the SAME authority the MemberAccessExpr arm asks, already
        // answers the indexer question correctly on both sides of the
        // module boundary — this arm only needed to ASK it. A builtin
        // collection index (Array<T>, Dictionary<K,V>, a span, an inline
        // array, a string) resolves no user `PropertySymbol` at this site
        // (the ExprTypecheck_Exprs.cpp branch that calls `setExprSymbol`
        // is gated on `tt->kind == TypeRef::Kind::Named && tt->declaration`
        // with a matching indexer candidate), so `ctx_.exprSymbol(*e)`
        // stays null there and this arm is a no-op for every builtin/array
        // shape — see the commit body's §6b enumeration.
        if (auto* ps = dynamic_cast<PropertySymbol*>(ctx_.exprSymbol(*e))) {
            if (!propertyGetterIsPure(ps)) {
                reportPureViolation(
                    e->span.begin, "indexer read",
                    "the resolved indexer's `get` accessor is not "
                    "pure-declared (§22.15 rule 6 / #3721)");
            }
        }
        checkExpr(ix->target.get());
        for (const auto& i : ix->indices)
            if (i.value) checkExpr(i.value.get());
        return;
    }
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(e)) {
        // fix #3279 (v0.263, stdlib §9.8 + spec §22.15): `x in r` is the
        // §17.5 membership operator, semantically equivalent to the call
        // `r.Contains(x)`. §22.15's calls-to-pure-members rule requires
        // the called member to be pure-classified, so the operator's
        // receiver's Contains member must be pure for the operator to be
        // admitted in a pure body or contract clause. Range<T>.Contains is
        // pure-classified via its `pure` modifier (stdlib §9.8,
        // intrinsic-purity contract); string/span Contains members are
        // pure-declared in stdlib; user types' Contains members are
        // admitted iff `pure`-declared on the receiving type. Any other
        // receiver (Array<T>, OwningArray<T>, user types without a pure
        // Contains, etc.) is rejected — the operator is rejected at this
        // site, named "in" over the receiver, citing §22.15 and the issue.
        //
        // Pre-fix state: the walker only descended into left and right
        // operands, never inspecting the operator itself, so an `in` over
        // Array<T> was admitted silently in pure bodies even though
        // Array<T>.Contains is not pure-declared. The fix closes the gap.
        if (be->op == "in") {
            if (!inOperatorCalleeIsPure(*be)) {
                const TypeRef* recvTy = be->right ? ctx_.exprType(*be->right)
                                                  : nullptr;
                std::string recvName = recvTy ? std::string(recvTy->headName())
                                              : std::string("<expr>");
                if (recvName.empty()) recvName = "<expr>";
                reportPureViolation(
                    be->span.begin,
                    std::string("`in` over `") + recvName + "`",
                    "the `in` operator is semantically equivalent to the "
                    "callee `Contains(value)` (\xC2\xA7""17.5); "
                    "\xC2\xA7""22.15's calls-to-pure-members rule requires "
                    "the receiver's Contains member to be "
                    "pure-classified \xE2\x80\x94 `" + recvName +
                    ".Contains` is not declared `pure` "
                    "(\xC2\xA7""22.15, #3279, stdlib \xC2\xA7""9.8)");
            }
        }
        // narrow #3610 (§22.15 rule 6, v0.286): every OTHER binary operator
        // token may be a §25 user-declared overload — including the §25.2
        // equality forms `=` / `<>` — and an overload is a call. `in` is
        // excluded because it is not an overload (§17.5 `Contains`, handled
        // above); `and` / `or` never resolve one.
        checkOperatorOverloadPurity(ctx_.exprSymbol(*be), be->op,
                                    be->span.begin);
        checkExpr(be->left.get());
        checkExpr(be->right.get());
        return;
    }
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(e)) {
        // §22.15 contrapositive: `await` is rejected (it interleaves with
        // the domain's continuation queue). `await` is admitted only in
        // async contexts, and `pure` excludes async — but defend here too.
        if (ue->op == "await") {
            reportPureViolation(
                ue->span.begin, "`await`",
                "interleaves with the domain's continuation queue");
        }
        // narrow #3610 (§22.15 rule 6, v0.286): the unary half. `typeUnary`
        // records a resolved §25 unary overload on the node exactly as the
        // binary path does; `await` is rejected above and resolves none.
        checkOperatorOverloadPurity(ctx_.exprSymbol(*ue), ue->op,
                                    ue->span.begin);
        checkExpr(ue->operand.get());
        return;
    }
    if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(e)) {
        checkExpr(tt->operand.get());
        return;
    }
    if (auto* cond = dynamic_cast<const ast::ConditionalExpr*>(e)) {
        checkExpr(cond->thenValue.get());
        checkExpr(cond->condition.get());
        checkExpr(cond->elseValue.get());
        return;
    }
    if (auto* te = dynamic_cast<const ast::ThrowExpr*>(e)) {
        // §22.16 (v0.203, #2053): `throw` is REJECTED in a contract clause
        // (clauseMode_), mirroring the throw-statement rule above.
        if (clauseMode_) {
            reportPureViolation(
                te->span.begin, "`throw`",
                "a contract clause states a proposition — the mechanism, "
                "message, and blame of a violation belong to §22.16's "
                "machinery, not to the clause's own throw (§22.16)");
            if (te->operand) checkExpr(te->operand.get());
            return;
        }
        // §22.15 throw carve-out: a throw expression follows the same rule
        // as the throw statement.
        if (te->operand) checkExpr(te->operand.get());
        return;
    }
    if (auto* le = dynamic_cast<const ast::LambdaExpr*>(e)) {
        // §22.15 rule 4: a lambda body defined inside a pure body is NOT
        // checked under these rules — it is a separate function whose
        // later invocation happens at the caller's effect discipline.
        // Intentionally do not recurse into the lambda body.
        return;
    }
    // §17.15 / §22.16 (v0.216, #2098): the binding atom is ADMITTED in
    // contract clauses — it is the form's motivating position (§22.16: "its
    // bindings are clause-internal names, letting a clause name a subresult
    // instead of recomputing it"). Clause initializers and the body are
    // checked under this walker's rules expression-wise. One clause-specific
    // rejection: an atom clause spelled as a MARKED TRANSFER (`var y :=
    // owned x;` — a K3.4 consuming position elsewhere) would break §22.16's
    // borrow-only guarantee and is rejected in a contract clause (§17.15;
    // owned productions cannot arise in the pure fragment, so clause atoms
    // never carry disposals).
    if (auto* bx = dynamic_cast<const ast::BindExpr*>(e)) {
        for (const auto& cl : bx->clauses) {
            if (!cl) continue;
            if (clauseMode_ && cl->initializerHasOwnedMarker) {
                reportPureViolation(
                    cl->initializer ? cl->initializer->span.begin
                                    : bx->span.begin,
                    "marked binding transfer",
                    "a binding-atom clause spelled `var y := owned x;` is a "
                    "K3.4 consuming position and would break the contract "
                    "clause's borrow-only guarantee (§22.16 / §17.15, "
                    "v0.216)");
            }
            if (cl->initializer) checkExpr(cl->initializer.get());
        }
        if (bx->body) checkExpr(bx->body.get());
        return;
    }
    if (auto* we = dynamic_cast<const ast::WithExpr*>(e)) {
        checkExpr(we->source.get());
        for (const auto& a : we->assigns)
            checkExpr(a.value.get());
        return;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
        for (const auto& el : cl->elements) checkExpr(el.get());
        for (const auto& k : cl->keys) checkExpr(k.get());
        return;
    }
    if (auto* ie = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
        for (const auto& part : ie->parts)
            if (part.expr) checkExpr(part.expr.get());
        return;
    }
    // Literals, IdentifierExpr, etc. — admitted as reads (§22.15 rule 1).
}

void PureBodyWalker::checkContractClauses(
    const std::vector<ast::ContractClause>& clauses) {
    // §22.16 (v0.203, #2053) — the clause-region equivalent of checkStmts.
    // Each clause expression is walked under §22.15's pure-body rules with
    // the clause-specific throw rejection (clauseMode_); the bool typing and
    // the per-amendment `result`/`old` placement checks happen elsewhere
    // (the bool typing lands in ExprTypecheck; result/old are addressed in
    // a later contract slice alongside lowering).
    if (clauses.empty()) return;
    const bool saveClauseMode = clauseMode_;
    clauseMode_ = true;
    for (const auto& c : clauses) {
        if (c.expr) checkExpr(c.expr.get());
    }
    clauseMode_ = saveClauseMode;
}

void PureBodyWalker::checkInvariantExpr(const ast::Expr& e) {
    // §22.16 (v0.203, #2053): an `invariant E;` member declares a predicate
    // checked under this section's clause rules in the context of the
    // declaring class. Throw/await are rejected (clauseMode_); the pure-body
    // rules apply expression-wise.
    const bool saveClauseMode = clauseMode_;
    clauseMode_ = true;
    checkExpr(&e);
    clauseMode_ = saveClauseMode;
}

// Walk the SourceFile: for every pure member, walk its body.
void checkOneMember(SemaContext& ctx, MemberSymbol& member) {
    // Determine purity of this member.
    bool isPure = false;
    bool isCtor = false;
    std::string label;
    if (auto* ms = dynamic_cast<MethodSymbol*>(&member)) {
        // fix #3609 (§22.15, v0.286): through the one predicate. This arm read
        // the `ast::OperatorDecl` bool and the modifiers but NOT the symbol
        // field; `checkPureBodies` walks only this module's own types and
        // extension blocks, so the field was structurally always false here and
        // adding it changes nothing.
        isPure = memberIsPure(ms);
        if (ms->isOperator)
            label = std::string("operator `") + std::string(ms->operatorToken) +
                    "`";
        else
            label = std::string("method `") + std::string(ms->name) + "`";
        // §22.16 (v0.203, #2053): contract clauses are type/purity-checked
        // on EVERY method regardless of `pure` — a non-pure method may
        // carry clauses, and each clause expression must satisfy the pure-
        // body rules expression-wise (clauses never have effects). Run the
        // clause walker before the body walker so a clause diagnostic is
        // reported before any body diagnostic.
        const auto* md = dynamic_cast<const ast::MethodDecl*>(member.decl);
        const auto* od = dynamic_cast<const ast::OperatorDecl*>(member.decl);
        PureBodyWalker w(ctx, member, label, /*isConstructor=*/false);
        if (md) w.checkContractClauses(md->contractClauses);
        // §22.15 body walk runs only for pure methods/operators.
        if (!isPure) return;
        if (md) {
            if (md->bodyKind == ast::MethodBodyKind::Expression &&
                md->exprBody) {
                w.checkExpr(md->exprBody.get());
            } else if (md->bodyKind == ast::MethodBodyKind::Block) {
                w.checkStmts(md->stmts);
            }
        } else if (od) {
            if (od->bodyKind == ast::MethodBodyKind::Expression &&
                od->exprBody) {
                w.checkExpr(od->exprBody.get());
            } else if (od->bodyKind == ast::MethodBodyKind::Block) {
                w.checkStmts(od->stmts);
            }
        }
        return;
    }
    if (auto* cs = dynamic_cast<ConstructorSymbol*>(&member)) {
        const auto* cd = dynamic_cast<const ast::ConstructorDecl*>(member.decl);
        // §22.16 (v0.203, #2053): clauses are checked on every constructor;
        // the §22.15 body walk runs only for pure ctors.
        if (cd) {
            label = std::string("constructor of `") +
                    std::string(cs->parent ? cs->parent->name
                                           : std::string_view()) +
                    "`";
            PureBodyWalker w(ctx, member, label, /*isConstructor=*/true);
            w.checkContractClauses(cd->contractClauses);
            if (!cd->isPure) return;
            isCtor = true;
            isPure = true;
            // §22.15 "Pure constructors" (#2061, item 1): a pure
            // constructor's explicit `base(...)`/`this(...)` chain target
            // must itself be pure. Run before the body walk so the chain
            // diagnostic is reported first (the chain's purity shapes the
            // whole ctors contract).
            w.checkPureCtorChainPurity(*cd, *cs);
            w.checkStmts(cd->body);
        }
        return;
    }
    if (auto* ps = dynamic_cast<PropertySymbol*>(&member)) {
        // Property-level pure applies to the getter. For Slice 2, walk
        // the getter body (expression-bodied / block-getter) — auto-
        // properties have no authored body.
        // fix #3609 (§22.15, v0.286): through the one predicate — the
        // GETTER-level one, which keeps this arm's accessor-level `pure get`
        // source. Routing a property through the member-level `memberIsPure`
        // alone would stop walking a `pure get` body, turning a §22.15
        // diagnostic into silence.
        const auto* pd = dynamic_cast<const ast::PropertyDecl*>(member.decl);
        if (!propertyGetterIsPure(ps)) return;
        label = std::string("property `") + std::string(ps->name) + "`";
        PureBodyWalker w(ctx, member, label, /*isConstructor=*/false);
        if (pd) {
            if (pd->bodyKind == ast::PropertyBodyKind::Expression &&
                pd->exprBody) {
                w.checkExpr(pd->exprBody.get());
            } else if (pd->bodyKind == ast::PropertyBodyKind::Block) {
                for (const auto& a : pd->accessors) {
                    if (a.isSet) continue;  // getter only
                    if (a.bodyKind == ast::PropertyAccessorBodyKind::Expression
                        && a.exprBody) {
                        w.checkExpr(a.exprBody.get());
                    } else if (a.bodyKind ==
                               ast::PropertyAccessorBodyKind::Block) {
                        w.checkStmts(a.stmts);
                    }
                }
            }
        }
        return;
    }
}

// §22.16 (v0.240, #2941): walk every source-defined invariant member's
// predicate under the same clause rules as `requires`/`ensures` (clauseMode:
// throw/await rejected, pure-body rules expression-wise, `old`/`result`
// rejected). Invariants are emitted at codegen time and evaluated at the
// construction-exit / public-entry / public-exit boundaries §22.16 fixes
// (see LlvmBackend_Setup.cpp `emitInvariantCheck`); the sema walk here
// validates the predicate's purity, name-resolves it, and type-checks it.
// The walker takes a `MemberSymbol&` only for diagnostic context; it does
// not dereference it. We pass the FIRST member symbol of the class (any
// real member works; ctor would also work) so the reference is valid.
// Imported classes are skipped (td == nullptr): their invariant records
// are declaration facts in the artifact interface, not something this
// compilation re-evaluates, and re-purity-checking them would break every
// importer of a pre-built library.
static void checkClassInvariants(SemaContext& ctx, const TypeSymbol& t) {
    const auto* td = dynamic_cast<const ast::TypeDecl*>(t.decl);
    if (!td) return;  // imported type: no AST in this compilation
    // Find any real member symbol to satisfy the walker's constructor; the
    // walker doesn't dereference the reference, only stores it.
    MemberSymbol* anchor = nullptr;
    for (const auto& m : t.members) {
        if (auto* mem = dynamic_cast<MemberSymbol*>(m)) {
            anchor = mem;
            break;
        }
    }
    if (!anchor) {
        // Empty class with only invariants and no other members — skip
        // (the class couldn't be instantiated anyway).
        return;
    }
    for (const auto& member : td->members) {
        const auto* inv =
            dynamic_cast<const ast::InvariantDecl*>(member.get());
        if (!inv || !inv->expr) continue;
        PureBodyWalker w(ctx, *anchor,
                         std::string("invariant of `") +
                             std::string(t.name) + "`",
                         /*isConstructor=*/false);
        w.checkInvariantExpr(*inv->expr);
    }
}

// fix #3597 (§22.15 "Pure-body rules" / §22.16 "Contract clauses" / §11.6
// "Nested types", v0.286): ALL of the per-type work `checkPureBodies` used
// to run inline for a module-level type, now ONE function so the shared
// recursive nested-type walker (fix #3574's `forEachTypeAndNested`,
// promoted to sema/NestedTypeWalk.hpp for exactly this caller) can drive it
// at every nesting depth instead of the one-level hand-copy this replaces.
// That one-level copy escaped along TWO independent axes measured on
// d8e07d7: §22.15's pure-body rule (the member loop below) never reached a
// type nested two levels deep (1/1/0 over a depth-0/1/2 ladder), and the
// pure-constructor field-initializer rule (the walk at the end of this
// function) was never applied to a nested type's OWN field initializers AT
// ALL -- it ran only for the outer `t` the old loop iterated, so a nested
// class with a `pure constructor` escaped it already at depth 1 (1/0 over
// depth 0/1).
//
// Diagnostic-order note: the old code interleaved a nested type's
// diagnostics with its outer type's remaining sibling members (nested type N
// declared between members A and B emitted A's diagnostic, then N's, then
// B's). This walker is PRE-ORDER over TYPES, not members: it runs this
// entire function for `t` (invariants, every member, then the field-init
// walk) before descending into any of `t`'s nested types, so a nested type's
// diagnostics now follow ALL of its outer type's own-member diagnostics
// rather than appearing at the nested type's declaration position among
// them. Measured over the in-tree lit corpus: no existing CHECK/CHECK-NEXT
// sequence assumes the old interleaving (the one lit pin that exercises
// invariant-plus-member-body diagnostics together,
// invariant_member_not_evaluated_2941.bzl, already uses CHECK-DAG "so the
// order of diagnostic emission doesn't matter"), and no gtest indexes
// `diagnostics()` by position.
static void checkOneType(SemaContext& ctx, TypeSymbol& t) {
    // §22.16 (v0.240, #2941): purity-check every source-defined invariant
    // member BEFORE the member-purity walk, so an invariant diagnostic is
    // reported ahead of any body diagnostic (matching the clause-before-body
    // ordering at the checkOneMember call site).
    checkClassInvariants(ctx, t);
    for (Symbol* m : t.members) {
        auto* mem = dynamic_cast<MemberSymbol*>(m);
        if (!mem) continue;
        checkOneMember(ctx, *mem);
    }
    // §22.15 "Pure constructors" (v0.202, #2052): the birth-store rule
    // extends to the class's field initializers — they execute as part
    // of construction, so a class with a pure constructor must have
    // field initializers satisfying the pure-body rules. Walk every
    // field initializer under a synthesized pure-ctor context.
    bool hasPureCtor = false;
    MemberSymbol* pureCtorMember = nullptr;
    for (Symbol* m : t.members) {
        auto* cs = dynamic_cast<ConstructorSymbol*>(m);
        if (!cs) continue;
        // fix #3609 (§22.15, v0.286): through the one predicate. This gate
        // read `ast::ConstructorDecl::isPure` alone — the NARROWEST of the
        // five spellings — so a ctor whose purity arrived on any other
        // channel left the class's field initializers unwalked. `t` is a
        // local type here, so the widening is inert today; it is the
        // divergence that mattered.
        if (constructorIsPure(cs)) {
            hasPureCtor = true;
            pureCtorMember = cs;
            break;
        }
    }
    if (hasPureCtor && pureCtorMember) {
        const std::string label =
            std::string("field initializer of class `") +
            std::string(t.name) +
            "` (pure constructor birth-store rule)";
        for (Symbol* m : t.members) {
            auto* fs = dynamic_cast<FieldSymbol*>(m);
            if (!fs) continue;
            const auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->initializer) continue;
            PureBodyWalker w(ctx, *pureCtorMember, label,
                             /*isConstructor=*/true);
            w.checkExpr(fd->initializer.get());
        }
    }
}

}  // namespace

bool checkPureBodies(const ast::SourceFile& src, SemaContext& ctx) {
    (void)src;
    if (!ctx.module()) return !ctx.diagnostics().hasErrors();
    // fix #3597 (§11.6, v0.286): every type this module declares, at every
    // nesting depth -- the shared recursive walker (fix #3574,
    // sema/NestedTypeWalk.hpp) replaces the old one-level hand-copy.
    forEachTypeAndNested(ctx.module(),
                         [&](TypeSymbol* t) { checkOneType(ctx, *t); });
    // §22.15 (fix #3175, v0.254): extension blocks (`extension T`) live at
    // module scope as `ExtensionSymbol`s, NOT inside a `TypeSymbol`'s members
    // (§A.3, SymbolCollection.cpp:439; Symbol.hpp:793). A `pure` extension
    // method or expression-bodied extension property carries the same
    // purity obligation as a class member — the body MUST satisfy the §22.15
    // closed-list rules, and the import-side artifact carries the `isPure`
    // bit (`ModuleMetadataBuilder.cpp:870` writes it from the member's
    // modifiers with no body check behind it). The earlier
    // `checkPureBodies` did not iterate extension blocks; the comment that
    // asserted it did (former line 1224) was the cause. Run the same
    // `checkOneMember` path used for class members. `ExtensionSymbol` is
    // only ever created by `collectMember` from a local `ast::ExtensionDecl`
    // (SymbolCollection.cpp:440) — there is no import path that synthesizes
    // one, so imported-extension handling is a non-issue. Per the issue's
    // "fix shape" note: the receiver-form distinction (by-value for value/
    // primitive extended types, by-pointer for class extended types) is
    // `MethodSymbol::extensionDecl`'s concern at codegen — `checkOneMember`
    // works on the body's AST and does not depend on `parent`.
    std::vector<ExtensionSymbol*> extensions;
    for (Symbol* child : ctx.module()->children) {
        if (auto* e = dynamic_cast<ExtensionSymbol*>(child))
            extensions.push_back(e);
    }
    for (ExtensionSymbol* ext : extensions) {
        for (Symbol* m : ext->members) {
            auto* mem = dynamic_cast<MemberSymbol*>(m);
            if (!mem) continue;
            checkOneMember(ctx, *mem);
        }
    }
    return !ctx.diagnostics().hasErrors();
}

}  // namespace bezel::sema
