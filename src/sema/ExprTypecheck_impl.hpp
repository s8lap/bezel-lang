#pragma once

// Private implementation header for the expression-typecheck worker.
//
// Declares the `ExprTC` worker class (sema slice 3 — see ExprTypecheck.hpp for
// the milestone narrative) whose methods are defined across several translation
// units (ExprTypecheck.cpp and ExprTypecheck_*.cpp). This mirrors the codegen
// refactor (cf. LlvmBackend_impl.hpp): the public surface stays in
// ExprTypecheck.hpp (the `typecheckExpressions` free function), while the worker
// class — previously placed entirely inline in an anonymous namespace — is split
// so its definitions can live in separately-compiled units.
//
// `Scope` is declared as a PRIVATE nested type of `ExprTC`. A namespace-scope
// `bezel::sema::Scope` also exists in TypeResolution.cpp; nesting it here avoids
// a program-wide ODR violation. Member bodies refer to it unqualified, which
// resolves to the nested type, so the bodies need no change.

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

class ExprTC {
public:
    ExprTC(SemaContext& ctx) : ctx_(ctx) {}

    void run(const ast::SourceFile& src);

private:
    // Per-walker scope for identifier lookups inside method bodies (slice 3b).
    struct Scope {
        std::unordered_map<std::string_view, Symbol*> names;
        Scope* parent = nullptr;
    };

    SemaContext& ctx_;
    std::unique_ptr<Scope> moduleScope_;
    Scope* currentScope_ = nullptr;
    TypeSymbol* currentType_ = nullptr;
    // §26.1: the ExtensionSymbol whose members are being type-checked, or null
    // outside an extension block. An extension method's params live on the
    // ExtensionSymbol (not on currentType_, which points at the extended type),
    // so paramSymbolsOf consults this when set.
    ExtensionSymbol* currentExtension_ = nullptr;

    // §12.7 flow narrowing. When `if x is T` (or the else branch of
    // `if x is not T`) narrows a simple variable `x` to `T`, an entry maps the
    // variable's resolved symbol (a LocalSymbol or ParameterSymbol) to the
    // narrowed TypeRef for the duration of the controlled block. An
    // IdentifierExpr that resolves to a narrowed symbol takes the narrowed type
    // in place of its declared one, so member access on it resolves against `T`
    // (`resolveMemberAccess` reads the target's exprType first). The map is
    // saved/restored around each controlled block so narrowing never leaks past
    // it; a reassignment of `x` inside the block drops its entry from that point
    // (re-widening to the declared type).
    std::unordered_map<Symbol*, TypeRef*> narrowings_;

    // §7.4 / §8.4 nullability flow-narrowing (issue #418). The set of simple
    // variables (LocalSymbol / ParameterSymbol) statically known to be non-null
    // at the current program point. A symbol is added when a null test proves
    // it non-null (`if x <> null` then-block, `if x = null` else-block, the
    // matching `is not null` / `is null` forms, an early-return `= null` guard),
    // when a non-null value is assigned to it, and on the all-paths join of an
    // if-statement; it is removed when a (possibly-)null value is assigned. The
    // §7.4 member-access gate consults this set: a bare member access on a
    // nullable simple variable NOT in this set is rejected. Unlike `narrowings_`
    // this does NOT rewrite the variable's exprType — the declared (nullable)
    // type is preserved so codegen's nullable-value layout logic (the tagged
    // {hasValue; payload} representation of `record?`/`int?`) stays correct.
    // Block-scoped the same way `narrowings_` is (saved/restored per block).
    std::unordered_set<Symbol*> nullNarrowed_;

    // Slice S6: in-progress method static-ness, so visitExpr's
    // IdentifierExpr handling can diagnose `this`/`base` in static
    // member bodies.
    bool inStaticMember_ = false;
    // §5 / issue #184: when a LocalDecl has an explicit wider integer type
    // (long, ulong, uint) and the initializer is an unsuffixed int literal
    // that overflows int, widen it to the declared type instead of erroring.
    PrimitiveType currentLiteralWideningHint_ = PrimitiveType::None;
    // Slice S7: tracks whether we're inside a constructor (or static
    // constructor) body — the only places where `this.<readonly>` may
    // be assigned. Field initializers are handled separately (slice
    // S7 doesn't flag those — they're part of the initialization flow).
    bool inConstructor_ = false;
    // Slice S21: the return-form context of the callable currently
    // being walked, for the §21.7 form rules.
    enum class ReturnCtx { None, ValueMethod, VoidMethod, CtorDtor };
    ReturnCtx returnCtx_ = ReturnCtx::None;
    // Slice T3: the resolved return type of the value-returning
    // callable currently being walked (null otherwise), for primitive
    // return-value assignability (§5.1).
    TypeRef* currentReturnType_ = nullptr;
    // Slice S26: enclosing-loop nesting depth, for the §21.5 rule that
    // `break`/`continue` are legal only inside a loop.
    int loopDepth_ = 0;
    // Slice S27: enclosing catch-arm-body depth, for the §31.5 rule
    // that a bare `throw;` / `throw` re-raise is valid only in a catch
    // arm body.
    int catchDepth_ = 0;
    // per §31.2: `return` inside a `finally` block is a compile error.
    // Appendix A grammar: "`return` does not appear in `finally` blocks."
    // Tracks finally-block nesting so visitStmt can reject ReturnStmt there.
    int finallyDepth_ = 0;
    // Slice S29: true while walking a field/auto-property initializer,
    // for the §22.8 rule that initializers may not observe instance
    // state (`this`, `base`, instance members).
    bool inFieldInitializer_ = false;
    // async-readiness 3b (§30.2 / Appendix C.6): true while walking the body
    // of an `async` method or `async` lambda — the contexts where `await` is
    // valid. Reset to false across every lambda boundary (a non-async lambda
    // nested in an async method is itself a synchronous context) and set from
    // the lambda's `isAsync` flag. Static field initializers and static
    // constructors are never async, so an `await` there is rejected by this
    // gate without special-casing (Appendix C.6).
    bool inAsyncContext_ = false;

    // per §24.3: when about to visit an event-handler lambda argument
    // (`<event>.Subscribe(h)` / `.Add` / `.Remove`, or `event += h`), this
    // holds the event's payload type so the lambda's untyped parameter is
    // given the handler's parameter type `T` — letting the body resolve
    // `e.Field` etc. Null when not in an event-handler context. Consumed (and
    // cleared) by visitExpr(LambdaExpr).
    TypeRef* pendingLambdaParamType_ = nullptr;

    // per §24.3: the payload type of an event-handler call's callee, or null
    // when `callee` is not `<event>.Subscribe/.Add/.Remove`. Used to set
    // pendingLambdaParamType_ before visiting the handler argument.
    TypeRef* eventHandlerPayloadType(const ast::Expr* callee);

    // stdlib §5 (D7): the source element type T of a LINQ operator/aggregate
    // call whose lambda argument is `(T) -> …` (Where/Select/Count/Any/All/
    // First/OrderBy/…), or null otherwise. Used to set pendingLambdaParamType_
    // before visiting the lambda argument so an untyped LINQ lambda parameter
    // is typed as the element type (its body can then resolve `p.x`).
    TypeRef* linqLambdaElementType(const ast::Expr* callee);

    // §2.3: cache of synthetic TypeSymbol objects for always-available
    // built-in classes (Console, Math, Convert, Environment).
    std::unordered_map<std::string, TypeSymbol*> builtinClassCache_;

    // Return (or lazily create) a synthetic TypeSymbol for a built-in class.
    TypeSymbol* builtinClassSym(std::string_view name);

    // Slice S29: report when a bare identifier inside a field
    // initializer resolves to a non-static member of the current type
    // (an implicit-`this` reference, forbidden by §22.8). Static
    // members, class constants, enum constants, locals, parameters,
    // types, and free names are all permitted.
    void checkNotInstanceMember(const ast::IdentifierExpr& id, Symbol* sym);

    // Slice DR1 (§21.7): does control definitely leave this statement
    // list abruptly (return / throw / non-terminating loop) before
    // reaching its end? Conservatively returns true only when certain,
    // so a value-method check that flags `!bodyAlwaysExits` never
    // false-positives on a body that does always exit (modulo the
    // noreturn-call case, which mainstream compilers also flag).
    bool bodyAlwaysExits(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    // §29.3: check if a statement list contains any yield return/break
    // (making it an iterator method body that is exempt from DR1).
    static bool stmtsContainYield(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    bool stmtAlwaysExits(const ast::Stmt& s);

    bool armBodyAlwaysExits(const ast::CaseArm& arm);

    // §19.6: exhaustiveness analysis for an EXPRESSION-position `case`.
    // In expression position an unmatched value raises MatchFailureException
    // at runtime; the spec says the compiler WARNS (not errors) when it
    // cannot prove exhaustiveness statically. Statement-position `case`
    // falls through with no action and is exempt (handled in CaseStmt). The
    // analysis proves exhaustiveness for the four spec-listed scrutinee
    // shapes (enum / bool / sealed-abstract closed hierarchy / nullable),
    // and treats an unguarded `_` or unconstrained `var x` arm as a
    // catch-all that is always exhaustive.
    void checkCaseExhaustiveness(const ast::CaseExpr& ce, TypeRef* scrutTy);

    // Slice S21: does a callable with this return-type ascription
    // produce a value? No ascription → no (unit). An ascription that
    // resolves to the unit type → no. Anything else → yes.
    bool returnsValue(const ast::Type* rt);

    // async-readiness 3b (§30.1): compute the return-FORM context and the
    // result-assignability type for an `async` method's body by peeling the
    // task wrapper. `Task` → VoidMethod (no result); `Task<T>` / `OwnedTask<T>`
    // → ValueMethod with result type T; everything else → VoidMethod.
    void asyncReturnCtxAndType(const ast::Type* rt, ReturnCtx& outCtx,
                               TypeRef*& outType);

    // Slice S21 (§21.7) return-statement form rules:
    //  - constructor / destructor: `return expression;` is an error.
    //  - value-returning method: bare `return;` is an error.
    //  - unit method: `return expression;` is an error.
    // The "every exit path returns" coverage rule needs flow analysis
    // and is left to a definite-return slice.
    void checkReturnForm(const ast::ReturnStmt& rs);

    // Slice T1: §5.1 numeric/char implicit-widening table. Returns
    // true iff `from` implicitly widens to `to` (both primitives,
    // distinct). Identity is handled by the caller.
    static bool primitiveWidens(PrimitiveType from, PrimitiveType to);

    // Slice T4: is `from` a subtype of `to` — i.e. does `to` appear in
    // `from`'s transitive base-type chain (base class or implemented
    // interface)? BFS, cycle-guarded.
    static bool isSubtype(TypeSymbol* from, TypeSymbol* to);

    // Slices T1/T4: assignability check (§5.1, §7, §12 subtyping).
    // Handles two cases and conservatively skips everything else
    // (mixed primitive/named, generics, null, Object target) so it
    // never false-positives while the type lattice is incomplete:
    //   - primitive → primitive: identity or §5.1 widening.
    //   - named → named (non-generic): identity or subclass/interface.
    // A non-nullable target accepting a nullable source is rejected.
    void checkAssignable(TypeRef* from, TypeRef* to,
                         SourceLocation loc, const std::string& what);

    // Slice S18: a condition must be `bool` (§21.2 if, §21.3 while,
    // §20 if-expression). Only flags when the condition has a resolved
    // primitive type that is not bool — conditions whose type isn't yet
    // inferred (unresolved identifiers, un-typed calls) are left alone
    // so this pass never produces false positives on incomplete typing.
    void checkConditionIsBool(const ast::Expr& cond, std::string_view ctx);

    // §20: type the `if` expression `x if c else y` with the COMMON type
    // of its two branches and record it on `e`. A branch whose type is
    // null (a bottom-typed `throw`/diverging expression) imposes no
    // constraint — the result type is the other branch's. When both
    // branches have resolved types, the common type is the one the other
    // widens/subtypes into; if neither converts to the other (e.g. a
    // primitive vs a class, as in the `throw E if c else x` shape), no
    // common type exists and a clean §20 type error is reported instead
    // of letting codegen build an ill-typed PHI and crash. Returns the
    // common TypeRef (already set on `e`), or null when none exists.
    void typeConditional(const ast::ConditionalExpr& cnd, const ast::Expr& e);

    // §19 + §20: type a `case` EXPRESSION with the COMMON type of its
    // (non-diverging) arm bodies and record it on `e`. Mirrors
    // typeConditional but folds over N arms: a bottom-typed (`throw` /
    // diverging) arm leaves its body's type null and imposes no constraint;
    // the result is the common type of the value-producing arms. When two
    // value arms have no common type (neither widens/subtypes into the
    // other), a clean §20 error is reported instead of letting codegen build
    // an ill-typed result PHI and crash. When EVERY arm diverges, the case
    // produces no value and no type is recorded.
    void typeCase(const ast::CaseExpr& cse, const ast::Expr& e);

    // §20: the pairwise common type of two branch/arm types — the one the
    // other widens (§5.1) or subtypes (§12) into. Returns null when neither
    // converts to the other (no common type). Shared by typeConditional and
    // typeCase so the `if`-expression and `case`-expression common-type
    // rules stay identical. Conservative (mirrors checkAssignable) so it
    // never false-positives while the type lattice is incomplete; an
    // Error-kind input yields the other operand (don't cascade), and an
    // `Object` operand is the universal common supertype.
    TypeRef* commonTypeOf(TypeRef* a, TypeRef* b);

    // Slice S7: diagnose assignment to a readonly field outside the
    // declaring class's constructor. Tracks only the `this.<field>`
    // target shape; `other.f := X` would need cross-object readonly
    // tracking that we don't have yet (and rarely matters in practice).
    void checkReadonlyAssignment(const ast::AssignmentStmt& as);

    // Slice S5: member lookup that walks the base-type chain.
    // BFS over the inheritance graph (no cycle detection — the spec
    // forbids cycles; sema's class-graph validation slice will catch
    // them). First match wins; ambiguity resolution between sibling
    // interfaces is a separate slice.
    Symbol* lookupMember(TypeSymbol* type, std::string_view name);

    // §26.5: extension-member lookup. Consulted ONLY when no instance member
    // matches (instance members win — an extension never shadows a real
    // member). Searches the module's ExtensionSymbols for one whose target
    // type equals `receiver` and that declares a member named `name`; returns
    // that MemberSymbol, or null. `receiver` is the receiver's resolved
    // TypeRef (its nullability is ignored — §26.2 forbids `extension int?`).
    // This slice matches the receiver's type EXACTLY (primitive by kind, user
    // type by declaration + generic args); base/interface/generic-extension
    // resolution (§26.5 step 2/3) is deferred (#433).
    Symbol* lookupExtensionMember(const TypeRef* receiver,
                                  std::string_view name);

    // §26.1 / §26.5: bind a resolved extension member to the member-access
    // expression — set its exprSymbol and the access's exprType. For a method
    // this is the §27.6 method-reference function type (the CallExpr handler
    // reads the MethodSymbol's decl for a call's return type); for a computed
    // property it is the property's declared type.
    void resolveExtensionMember(const ast::MemberAccessExpr& ma,
                                Symbol* member);

    Symbol* lookupName(std::string_view name);

    // §12.7: collect the flow-narrowings that an `if`/`elif` condition
    // establishes for one branch. `forTrueBranch` selects the controlled
    // (then) block (`x is T` narrows there; `x is not T` does not) versus the
    // `else` branch (`x is not T` narrows there; `x is T` does not). Each
    // narrowing maps the tested variable's resolved symbol to the test type's
    // resolved TypeRef. `and`-joined tests contribute all their narrowings to
    // the true branch (§12.7). The condition must already have been visited so
    // operand symbols and the test type are resolved. Entries are written into
    // `narrowings_`; the caller saves/restores the map for block scoping.
    void collectNarrowings(const ast::Expr* cond, bool forTrueBranch);

    // §12.7 helper: if `cond` is a `<simple var> is T` / `is not T` test that
    // narrows in the requested branch, record the narrowing for the variable's
    // symbol. Returns true iff a narrowing was applied.
    bool narrowFromTypeTest(const ast::Expr* cond, bool forTrueBranch);

    // §7.4 / §8.4 helper: if `cond` is a `<simple var> <> null` / `= null`
    // test that narrows in the requested branch (`<> null` narrows the true
    // branch, `= null` narrows the else branch — and `is not null` / `is null`
    // desugar to these by the parser), record the variable's symbol in
    // `nullNarrowed_` (known non-null). Returns true iff a narrowing was
    // applied. (issue #418)
    bool narrowFromNullTest(const ast::Expr* cond, bool forTrueBranch);

    // The resolved declared TypeRef of a local or parameter symbol, or null for
    // any other symbol kind (or when the type is unresolved). Used by
    // null-narrowing to compute the non-nullable form.
    TypeRef* declaredTypeOf(Symbol* sym);

    // §7.4 (issue #418): is `sym` a simple variable currently known non-null?
    // True when it is in `nullNarrowed_` OR has a §12.7 type-narrowing (which is
    // always to a non-nullable form) OR its declared type is non-nullable.
    bool isKnownNonNull(Symbol* sym);

    // §7.4 (issue #418): update `nullNarrowed_` for an assignment of a value of
    // `valueType` to the simple variable `sym` — a non-null value makes it
    // known-non-null; a (possibly-)null value clears that knowledge.
    void narrowOnAssignment(Symbol* sym, const TypeRef* valueType);

    void visitTopDecl(const ast::TopDecl& d);

    // Param-symbol lookup for the in-progress method/ctor on currentType_.
    const std::vector<Symbol*>* paramSymbolsOf(const ast::Member& m) const;

    // Build a method scope (params on top of module scope), then visit
    // body. Locals are registered into this scope as visitStmt walks
    // LocalDecls.
    void runMethodBody(
        const std::vector<ast::Param>& params,
        const std::vector<std::unique_ptr<ast::Stmt>>* stmts,
        const ast::Expr* exprBody,
        const std::vector<Symbol*>* paramSyms);

    void visitMember(const ast::Member& m);

    void visitStmt(const ast::Stmt& s);

    void visitExpr(const ast::Expr& e);

    // Slice 3h: resolve a CallExpr's return type from the callee's
    // resolved MethodSymbol.
    void typeCall(const ast::CallExpr& ce);

    void checkCallArity(const ast::CallExpr& ce, MethodSymbol* ms);

    // §2.2 / §2.5 / §2.6 step 3 (issue #328 inc 3): type-check a call to an
    // IMPORTED method (no ast::MethodDecl) against its reconstructed
    // ParameterSymbol signature — argument count + per-argument assignability.
    void checkImportedCallArity(const ast::CallExpr& ce, MethodSymbol* ms);

    // §22.5 argument-to-parameter binding. Maps a call's argument list
    // (positional, then named) onto a parameter list:
    //   - positional arguments fill parameters in order;
    //   - a named argument `name := expr` fills the parameter named `name`;
    //   - "named arguments may appear in any order after positional
    //     arguments" — a positional argument that follows a named one is an
    //     error;
    //   - an unknown parameter name, a parameter bound twice (positionally
    //     then by name, or by two named args), and a required (non-default)
    //     parameter left unfilled are each errors.
    // When `emit` is true the diagnostics above are reported (with `loc` /
    // `calleeDesc`); when false the routine is silent (used to test a
    // candidate's applicability during §22.6 overload resolution).
    struct BindResult {
        bool ok = false;            // binding succeeded with no violations
        // paramToArg[p] = index into `args` filling parameter p, or -1 when
        // the parameter takes its default (valid only when `ok`).
        std::vector<int> paramToArg;
    };
    BindResult bindArgsToParams(const std::vector<ast::Param>& params,
                               const std::vector<ast::CallArgument>& args,
                               SourceLocation loc,
                               const std::string& calleeDesc, bool emit);

    // §5.1: is a value of static type `from` implicitly convertible to
    // `to`? Identity is NOT counted here (that is the "exact" relation,
    // tested with typeRefEqual); this returns true only for a non-trivial
    // §5.1 conversion: numeric/char widening, non-nullable reference →
    // nullable, or subtype → supertype. Conservative: returns false for
    // type pairs the lattice can't yet judge, so a candidate is treated as
    // inapplicable rather than spuriously applicable.
    bool implicitlyConvertible(const TypeRef* from, const TypeRef* to);

    // §22.6 per-argument match classification against a parameter type.
    enum class ArgMatch { NotApplicable, Convert, Exact };
    ArgMatch classifyArg(const TypeRef* argTy, const TypeRef* paramTy);

    // §22.6 type-based overload resolution over a set of candidate
    // callables sharing the call's arity and binding all-positional.
    enum class OverloadOutcome {
        Resolved,     // a unique winner: `winnerIndex` is set.
        Ambiguous,    // diagnostic emitted (candidates cited); no winner.
        Unreachable,  // diagnostic emitted (none cited); no winner.
        Undecidable   // could not judge (unresolved arg/param types): the
                      // caller should fall back to arity-based selection.
    };
    struct OverloadResult {
        OverloadOutcome outcome = OverloadOutcome::Undecidable;
        std::size_t winnerIndex = 0;  // valid only when Resolved
    };

    // `paramTypes[i]` holds candidate i's ordered parameter TypeRefs (each
    // may be null when unresolved). `argTypes` holds the call's positional
    // argument static types (null when unresolved). Emits the ambiguous /
    // unreachable diagnostic itself when applicable. `candNames` supplies a
    // display string per candidate for the ambiguous-case citation.
    OverloadResult resolveOverload(
        const std::vector<std::vector<const TypeRef*>>& paramTypes,
        const std::vector<const TypeRef*>& argTypes,
        const std::vector<std::string>& candNames, SourceLocation loc,
        const std::string& calleeDesc);

    // §22.6 method-overload resolution. Given the initially-resolved
    // MethodSymbol for a call and the call site, returns the MethodSymbol
    // the call resolves to: applies type-based §22.6 selection when the
    // name has multiple same-arity, all-positional candidates and arg/param
    // types are decidable; otherwise falls back to arity-based first-wins
    // (preserving the prior behavior on named-arg / modifier / undecidable
    // calls). Emits the ambiguous / unreachable diagnostics itself.
    MethodSymbol* resolveMethodOverload(MethodSymbol* initial,
                                        const ast::CallExpr& ce);

    // Slices T5/T6/T7: shared check of an all-positional argument list
    // against a parameter list — argument count within [required,
    // total] (optional params lower the minimum), then per-argument
    // assignability. Callers must have already ruled out named args
    // and overload ambiguity.
    void checkArgsAgainstParams(const std::vector<ast::Param>& params,
                                const std::vector<ast::CallArgument>& args,
                                SourceLocation loc,
                                const std::string& calleeDesc);

    // Slice T7: check `new T(args)` against T's sole constructor (if
    // any). Zero declared constructors → the implicit default ctor
    // takes no arguments. Multiple constructors → overload resolution
    // is deferred, so skip.
    void checkNewExprArgs(const ast::NewExpr& ne, TypeSymbol* t);

    // §2.5 / §22.7 (issue #328 FINDING [7] B): check `new ImportedT(args)`
    // against an imported type's reconstructed constructor signatures (no
    // ast::ConstructorDecl; ParameterSymbol resolved types). Single-ctor,
    // all-positional case; overloads/named args deferred.
    void checkImportedNewExprArgs(const ast::NewExpr& ne, TypeSymbol* t,
                                  const std::vector<ConstructorSymbol*>& ctors);

    // §17.1: postfix `.` member access. The target's type determines
    // the lookup scope. For now we handle `this.member` (lookup in the
    // current type) and `<expr>.member` where the expression has a
    // resolved Named or Primitive-Object type. Generics and inherited
    // members from base types are deferred to a later slice.
    // The TypeSymbol a member-access target denotes (for `.` and `?.`):
    // `this`/`base` keywords, then the target's exprType, then a fallback
    // through its exprSymbol (local/parameter declared type, or a type name
    // for static access). Returns null when it can't be determined.
    TypeSymbol* memberTargetType(const ast::Expr* target);

    void resolveMemberAccess(const ast::MemberAccessExpr& ma);

    // per §14.1/§14.2: substitute a member's declared type against the
    // receiver's concrete generic arguments. When a field/property declared
    // with a generic-parameter type (`v: T`) is accessed off a receiver of a
    // concrete instantiation (`b: Holder<Inner>`), the access `b.v` has type
    // `Inner`, not the bare `T`. Returns `declared` unchanged when no
    // substitution applies (non-generic field, missing receiver args, etc.).
    // See issue #361 (D13): without this, `b.v.x` cannot resolve `.x` because
    // `b.v` was typed as the unresolved type parameter.
    TypeRef* substituteMemberType(TypeRef* declared,
                                  const ast::Expr* target,
                                  TypeSymbol* parent);

    // per §14.1/§14.2 + §A.7 (issue #391): return-target generic
    // type-argument inference. A generic method whose type parameter appears
    // ONLY in the return type (`Create<T>(): T`) cannot have T inferred from
    // arguments, and Bezel has no call-site type-argument syntax (§A.7
    // PrimarySuffix has no `<...>` form), so T must be solved from the call
    // site's *target* type — the same "target type" the spec already uses for
    // collection literals (§15.4) and the `new()` shorthand (§17.7).
    //
    // If `ce` resolves to a generic method with one or more type parameters
    // left unbound by its arguments, this unifies the method's declared return
    // type against `target` to solve them. On success it re-types the CallExpr
    // to the substituted return type and returns true; on failure (no
    // applicable generic method, the params are already bound by arguments, or
    // the return type does not unify with `target`) it returns false and
    // leaves the call's type untouched. `target` may be null, in which case
    // the routine only reports whether the call *needs* inference (via
    // `outNeedsInfer`) without solving anything — the caller uses that to emit
    // the "cannot infer" diagnostic in a no-target context (§15.4 / §17.7:
    // inference is not available for `var` without annotation).
    bool inferReturnTargetTypeArgs(const ast::CallExpr& ce, TypeRef* target,
                                   bool* outNeedsInfer);

    // Recursive unification of a declared return TypeRef against a concrete
    // target TypeRef, binding any type-parameter names in `paramNames` into
    // `solved`. Returns false on a structural mismatch that prevents solving.
    bool unifyReturnAgainstTarget(
        const TypeRef* declared, const TypeRef* target,
        const std::unordered_set<std::string_view>& paramNames,
        std::unordered_map<std::string_view, TypeRef*>& solved);

    // Build the substituted form of a declared return TypeRef, replacing each
    // type-parameter name in `solved` with its concrete TypeRef. Returns
    // `declared` unchanged when nothing applies.
    TypeRef* substituteReturnType(
        TypeRef* declared,
        const std::unordered_map<std::string_view, TypeRef*>& solved);

    // per §14.1 + §A.7 (issue #391): emit the "cannot infer the type argument"
    // diagnostic when `ce` calls a generic method whose return-only type
    // parameter cannot be solved because the call sits in a no-target context
    // (a bare expression/discard statement, or a `var` without annotation).
    void reportNoTargetInferenceIfNeeded(const ast::CallExpr& ce);

    // §24.3: the event visibility fence. Outside the declaring class, an
    // event member's static type is `IEvent<T>` / `IAsyncEvent<T>`, which
    // exposes only `Subscribe`. The producer-side operations `Raise` /
    // `RaiseAsync` / `Add` / `Remove` (and the `+=` / `-=` desugaring) live
    // on `EventSource<T>` / `AsyncEventSource<T>`, the in-class face. Given
    // an event member, the accessed operation name (one of the producer-side
    // names, or `+=` / `-=`), and the access location, this emits a
    // diagnostic when the access is from outside the declaring class. A
    // derived class may use the producer-side face only for a `protected`
    // event declared on a base class (§24.5). `Subscribe` is never flagged.
    void checkEventVisibilityFence(EventSymbol* es, std::string_view op,
                                   SourceLocation loc);

    // §13.1 member-visibility enforcement (issue #432). Checks that the
    // member `msym` accessed at `loc` is visible from the current type
    // context `currentType_`. Reports a diagnostic and returns true when
    // access is rejected; returns false when access is allowed (caller
    // continues normally). Handles all four levels:
    //   private   — declaring type only (§13.1).
    //   protected — declaring type + derived types (§13.1).
    //   internal  — same module only; cross-module import → rejected (§13.4).
    //   public    — always allowed.
    // The overload-suppression heuristic (a non-private sibling with the same
    // name → suppress the error) is applied for private and protected, matching
    // the original private-only check.
    bool checkMemberVisibility(const MemberSymbol* msym,
                               std::string_view memberName,
                               SourceLocation loc);

    // §7.5 null-conditional member access `x?.M`. Resolves the member on
    // the target's (non-nullable) type exactly like `.`, but the result
    // type is ALWAYS nullable (the access yields null when the target is
    // null). The member's own type is looked up; its nullable form is the
    // expression's type, so member access chains (`a?.b?.c`) resolve.
    void resolveNullCondMember(const ast::NullCondMemberExpr& nm);

    // §17.1 result-type table for binary operators. Slice 3c covers the
    // common cases with no operator-overload resolution: equality and
    // relational ops produce bool; arithmetic / shift / bitwise produce
    // the left operand's type (when the operands type-match); logical
    // ops `and` / `or` produce bool; `??` produces the non-nullable
    // result type of the LHS; range ops produce a Range value (slice 3c
    // leaves Range as Error).
    // §25: the user-defined `static operator <op>` method that applies to a
    // binary expression, if either operand's type (class or record)
    // declares one. `=`/`<>` on classes are excluded (they invoke Equals,
    // §25.2).
    MethodSymbol* findBinaryOperator(const ast::BinaryExpr& be);

    void typeBinary(const ast::BinaryExpr& be);

    // §17.1 result-type table for unary operators (slice 3c).
    // §25: a user-defined unary `operator <op>` (one parameter) declared on
    // the operand's type. Overloadable prefix unary operators are `-`/`~`.
    MethodSymbol* findUnaryOperator(const ast::UnaryExpr& ue);

    void typeUnary(const ast::UnaryExpr& ue);

    // §28.8.1 / §28.11 (issue #423): if `operand` is `coll[i]` whose receiver
    // `coll` has an INTERFACE static type, return the interface type name;
    // otherwise the empty string. `move coll[i]` is admitted only on the
    // enumerated concrete owning receivers (§28.11) — an interface is not one of
    // them — so a non-empty result is rejected by `typeUnary`'s `move` branch.
    std::string moveThroughInterfaceSlotName(const ast::Expr& operand) const;

    void visitChildren(const ast::Expr& e);
};

}  // namespace bezel::sema
