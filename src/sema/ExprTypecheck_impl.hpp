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
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <vector>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// per §15.4 (#1174 / #1866): a collection literal's admissible TARGET types are
// a CLOSED list of CONCRETE collections (inline array, `Array`/`OwningArray`,
// `Set`, `Stack`, `Queue`, `LinkedList`, `Dictionary`, and their owning forms,
// or a `FromCollectionLiteral` user type). A collection INTERFACE
// (`IEnumerable<T>`/`IList<T>`/`ICollection<T>`/`IReadOnly*`/…) is NOT on that
// list — it has no concrete backing the literal could be materialized into.
// `isCollectionLiteralInterfaceTarget` recognises a non-admissible interface
// target; `reportCollectionLiteralInterfaceTarget` emits the §15.4 rejection.
// Shared across the call-argument (#1174), string-method (#1174), and
// local-declaration (#1866) positions.
bool isCollectionLiteralInterfaceTarget(const TypeRef* t);
void reportCollectionLiteralInterfaceTarget(DiagnosticEngine& diags,
                                            SourceLocation loc,
                                            std::string_view ifaceName);

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

    // §2.2 v0.164 / #1365: import aliases for the current source file.
    // Mirrors TypeResolution's importAliases_; used in resolveMemberAccess to
    // support `Alias.TypeName.Member` in expression position (§2.7).
    std::unordered_map<std::string, std::string> importAliases_;
    // §2.2 v0.164 / #1365: explicitly-imported module names (dotted) for the
    // current source file. Used for module-prefix resolution (§2.7 rule 2).
    std::unordered_set<std::string> sourceImports_;
    // §2.2 v0.164 / #1365: bare names ambiguous due to import collision.
    std::unordered_set<std::string> ambiguousImportNames_;
    // §2.7 v0.164 / #1365: maps a MemberAccessExpr* → dotted module name when
    // that expression resolves to a partial module-name prefix in expression
    // position (e.g. `BankingApp.Transactions` in `BankingApp.Transactions.Calc`).
    // Lets the next MemberAccess level detect the complete module name.
    std::unordered_map<const ast::MemberAccessExpr*, std::string> partialModulePaths_;

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

    // §7.4 (v0.182, #1566): the set of local/parameter NAMES that are "mutably
    // captured" — assigned, or passed `ref`/`out`, anywhere inside ANY lambda of
    // the enclosing method body. Computed once per body (computeMutablyCaptured)
    // at the start of `runMethodBody`; the spec's rule is deliberately syntactic
    // and conservative (any lambda in the body counts, whether or not it escapes,
    // so no escape analysis is needed). While such a local is narrowed non-null,
    // any operation inside the controlled block that can transfer control to user
    // code — a call, a function-value invocation, a constructor, or an `await` —
    // may run the capturing closure and null the local, so it drops the narrowing
    // (invalidateCapturedNarrowings). Keyed by name (string_view into the stable
    // source text); matched against a narrowed symbol's `name`. Empty in the
    // common case, which makes the per-op invalidation a cheap no-op.
    std::unordered_set<std::string_view> mutablyCapturedNames_;

    // §22.5 (issue #1160): while type-checking parameter i's default-value
    // expression, that parameter and every parameter declared AFTER it are out
    // of scope — a default may read only EARLIER parameters. This set holds the
    // forbidden (self-and-later) parameter symbols for the default currently
    // being checked; it is empty outside that window. `lookupName` consults it
    // and records the offending name in `forwardParamRef_` so `runMethodBody`
    // can report the §22.5 error at the default expression.
    std::unordered_set<Symbol*> forbiddenForwardParams_;
    std::string forwardParamRef_;

    // issue #484 / §14.2: the MethodDecl currently being type-checked, or null
    // when outside a method body. Used by resolveMemberAccess to walk the
    // method's generic constraints when the receiver is a type parameter —
    // `x: T` where T has `where T: ISome` requires dispatching through the
    // constraint's member set, not the (null) declaration of T's TypeRef.
    const ast::MethodDecl* currentMethodDecl_ = nullptr;

    // §11.8 (v0.176, #807): while visiting a MemberAccessExpr's TARGET child,
    // this points at that target node. It lets the IdentifierExpr handler
    // recognize `T` in `T.Member(...)` — a type-parameter receiver — and mark
    // it with a GenericParamSymbol instead of reporting "unresolved
    // identifier". A bare `T` in any other expression position keeps today's
    // error. Saved/restored around each target visit.
    const ast::Expr* maTargetHint_ = nullptr;
    // §27.6 (v0.184): while visiting a CallExpr's *callee* child, this points at
    // that callee node. resolveMemberAccess consults it to tell a primitive-
    // intrinsic static member that is the DIRECT callee of a call (`int.Parse(s)`
    // — typed as its scalar result, dispatched by member/receiver in typeCall)
    // from one used as a BARE method reference (`strings.Select(int.Parse)` —
    // typed as the function value `(string) -> int` per §27.6). Identity-based
    // (the specific callee node), so a stale value never yields a false positive.
    // Saved/restored around each callee visit.
    const ast::Expr* calleeOfCallCtx_ = nullptr;
    // §11.8: one synthesized GenericParamSymbol marker per type-parameter
    // name, attached as the exprSymbol of a `T` member-access receiver so
    // resolveMemberAccess (and codegen) can identify the receiver kind.
    std::unordered_map<std::string_view, GenericParamSymbol*> typeParamMarkers_;
    // §11.8: is `name` a declared generic type parameter of the enclosing
    // method or type?
    bool isInScopeTypeParam(std::string_view name) const;
    // §11.8: the cached marker symbol for an in-scope type-parameter name.
    GenericParamSymbol* typeParamMarker(std::string_view name);
    // §11.8: search the enclosing method's constraints (then the enclosing
    // type's) for a `where <paramName>: I<...>` bound whose interface declares
    // a static OPERATOR member matching `op` with two parameters of the
    // interface's own recurrence parameter. Returns the interface's operator
    // MethodSymbol and stores the bound's instantiation TypeRef in *outBound.
    MethodSymbol* findConstraintStaticOperator(std::string_view paramName,
                                               std::string_view op,
                                               TypeRef** outBound);
    // §11.8: enumerate the `type`-kind constraint bounds declared for
    // `paramName` (method constraints first, then the enclosing type's),
    // invoking `fn` with each resolved bound TypeRef until it returns true.
    bool forEachConstraintBound(std::string_view paramName,
                                const std::function<bool(TypeRef*)>& fn);

    // Slice S6: in-progress method static-ness, so visitExpr's
    // IdentifierExpr handling can diagnose `this`/`base` in static
    // member bodies.
    bool inStaticMember_ = false;
    // §5 / issue #184: when a LocalDecl has an explicit wider integer type
    // (long, ulong, uint) and the initializer is an unsuffixed int literal
    // that overflows int, widen it to the declared type instead of erroring.
    PrimitiveType currentLiteralWideningHint_ = PrimitiveType::None;
    // §4.2 / issue #1155: true while typing an integer literal that is the
    // direct operand of a unary minus. A signed type's negative boundary
    // magnitude (|long.MinValue| = 2^63) is one greater than its positive
    // maximum, so `-9223372036854775808L` is long.MinValue and must be
    // admitted even though the bare literal `9223372036854775808L` overflows
    // `long`. Set by visitExpr around the operand of a unary `-`.
    bool literalUnderUnaryMinus_ = false;
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
    // per §21.7 (#1589): also reset (to 0) across a lambda boundary — a
    // lambda is its own callable, so a `return` inside it exits the lambda,
    // not the enclosing method, and is unaffected by that method's `finally`
    // (mirrors the `loopDepth_`/`catchDepth_`/`finallyLoopBoundary_` reset).
    int finallyDepth_ = 0;
    // per §21.5/§31.2 (v0.182, #1578): `loopDepth_` measured at the point
    // the nearest enclosing `finally` body was entered. A `break`/`continue`
    // whose target loop (the loop active at `loopDepth_` when the jump is
    // visited) was already open at that boundary was opened *outside* the
    // `finally` and therefore encloses it — such a jump would silently
    // discard an in-flight exception when the `finally` runs during
    // exception propagation. A target loop opened strictly after the
    // boundary is wholly inside the `finally` and is unaffected. -1 means
    // "not currently inside a finally"; also reset across a lambda boundary
    // (a lambda is its own callable, unaffected by an enclosing method's
    // `finally` — mirrors the `loopDepth_`/`catchDepth_` reset there).
    int finallyLoopBoundary_ = -1;
    // Slice S29: true while walking a field/auto-property initializer,
    // for the §22.8 rule that initializers may not observe instance
    // state (`this`, `base`, instance members).
    bool inFieldInitializer_ = false;
    // §10.6 (#772): true while typechecking the arm guards/bodies of a `case`
    // whose scrutinee is an enum (the scope into which that enum's constants are
    // injected). Gates the bare-enum-constant exprType attachment so it applies
    // only there — never in an enum-constant initializer's sibling arithmetic.
    bool inEnumScrutineeCaseArms_ = false;
    // async-readiness 3b (§30.2 / Appendix C.6): true while walking the body
    // of an `async` method or `async` lambda — the contexts where `await` is
    // valid. Reset to false across every lambda boundary (a non-async lambda
    // nested in an async method is itself a synchronous context) and set from
    // the lambda's `isAsync` flag. Static field initializers and static
    // constructors are never async, so an `await` there is rejected by this
    // gate without special-casing (Appendix C.6).
    bool inAsyncContext_ = false;
    // #721/§29.5: true while visiting an iterator method body (sync or async,
    // i.e. it contains `yield`). A plain `return;` in an iterator is forbidden
    // (use `yield break`); for an ASYNC iterator §21.7 does not catch it (it is
    // a VoidMethod ctx), so checkReturnForm consults this flag.
    bool inIteratorMethod_ = false;
    // §29.9: true while visiting a lambda body. `yield` is a method-level
    // construct only, so a `yield`/`yield break` inside a lambda is rejected
    // (otherwise codegen builds a malformed iterator state machine for the
    // lambda and the program SIGSEGVs at runtime — #747 R42). Saved/restored
    // (not just set) so nested lambdas behave correctly.
    bool inLambda_ = false;

    // per §24.3: when about to visit an event-handler lambda argument
    // (`<event>.Subscribe(h)` / `.Add` / `.Remove`, or `event += h`), this
    // holds the event's payload type so the lambda's untyped parameter is
    // given the handler's parameter type `T` — letting the body resolve
    // `e.Field` etc. Null when not in an event-handler context. Consumed (and
    // cleared) by visitExpr(LambdaExpr).
    TypeRef* pendingLambdaParamType_ = nullptr;

    // §23 (#1192): the member-access node that is the DIRECT target of a plain
    // `:=` assignment (e.g. `c.X` in `c.X := 7`). Set around the assignment
    // target's visitExpr in the AssignmentStmt handler; consumed by
    // resolveMemberAccess to EXEMPT exactly that node from the write-only
    // (set-only) property read diagnostic — a plain `:=` to a set-only property
    // is a write (handled by the setter), but any other access position (a
    // read, a compound-assignment target, an intermediate `.x` in a chain, an
    // RHS) is a read and must be rejected.
    const ast::Expr* plainSetAssignTargetNode_ = nullptr;

    // fix #829 / §A.7: when a lambda's expected type is a known function type
    // `(T1, …, Tn) -> R` (a typed-local initializer, an assignment RHS, or a
    // call argument bound to a function-typed parameter), this holds those
    // positional parameter types so an UNTYPED lambda parameter (`(a, b) => …`)
    // is given its inferred type. Without it the params stay untyped and the
    // body's operator/member resolution falls back to primitives — e.g. a
    // user-defined `operator +` on a record is missed and codegen emits a raw
    // `add` on the record struct (malformed IR). Consumed (and cleared) by
    // visitExpr(LambdaExpr); positionally matched to the lambda's params.
    std::vector<TypeRef*> pendingLambdaParamTypes_;

    // issue #777 / §30.6: when about to visit an `async` lambda whose expected
    // function type is `(…) -> Task<T>` / `(…) -> OwningTask<T>`, this holds the
    // peeled element type T so the lambda visitor can reject a forgotten `await`
    // (an expression body that produces a `Task` value where T is expected).
    // Null otherwise. Consumed (and cleared) by visitExpr(LambdaExpr).
    TypeRef* pendingLambdaReturnType_ = nullptr;

    // issue #777 / §30.6: true when `t` is a Named `Task<…>` / `OwningTask<…>`
    // (the async task wrapper). Used to detect a forgotten `await`.
    bool isTaskShapedRef(const TypeRef* t) const;

    // per §24.3: the payload type of an event-handler call's callee, or null
    // when `callee` is not `<event>.Subscribe/.Add/.Remove`. Used to set
    // pendingLambdaParamType_ before visiting the handler argument.
    TypeRef* eventHandlerPayloadType(const ast::Expr* callee);

    // §21.9 / §17.13 / §2.11 (#949): true when `e` is an `<event>.Subscribe(...)`
    // call. Subscribe returns an `owned Subscription` (a non-trivial-dispose
    // class, stdlib §2.11) but sema does not (yet) type the call result, so the
    // type-based drop/discard checks cannot see it. Recognized structurally so a
    // dropped or discarded Subscription is the hard error §21.9 / §17.13 require.
    bool isEventSubscribeDrop(const ast::Expr* e);

    // §28.9 / §22.5 (issue #1684): does `decl`'s class chain make it
    // non-trivial-dispose — a destructor, or a non-static `owned` field
    // (declared, or a captured `owned` primary-constructor parameter,
    // §22.10), anywhere from `decl` up through its base classes? Mirrors the
    // walk visitStmt's ExpressionStmt arm performs inline for the §21.9
    // bare-discard check (this file); duplicated rather than shared because
    // that walk is not exposed as a callable predicate, but both apply the
    // same §28.9 test.
    bool isNonTrivialDisposeClass(TypeSymbol* decl) const;

    // §22.5 v0.184 (issue #1684): is `e` a fresh owned-producing expression —
    // `new T()` or a call to an `owned`-returning method — whose produced
    // class type is non-trivial-dispose (isNonTrivialDisposeClass above)?
    // Scoped to the shapes §22.5's "Permitted default-value expressions"
    // actually admits in a default position (no `this`, no locals reach
    // here, so only `new` and a static-factory call are possible producers).
    // Used to reject such a default on a plain (non-`owned`) parameter — the
    // produced instance would have no owner to discharge it.
    bool exprIsOwnedProducerOfNonTrivialDisposeClass(const ast::Expr& e) const;

    // stdlib §5 (D7): the source element type T of a LINQ operator/aggregate
    // call whose lambda argument is `(T) -> …` (Where/Select/Count/Any/All/
    // First/OrderBy/…), or null otherwise. Used to set pendingLambdaParamType_
    // before visiting the lambda argument so an untyped LINQ lambda parameter
    // is typed as the element type (its body can then resolve `p.x`).
    TypeRef* linqLambdaElementType(const ast::Expr* callee);

    // stdlib §5 / §9.6: the element type T of a LINQ *source* TypeRef (a
    // collection, lazy IEnumerable sentinel, Range<T>, or string). Factored out
    // of linqLambdaElementType so both a LINQ operator's receiver element and a
    // Zip combiner's `other` element (U) can be derived uniformly. Null when the
    // TypeRef is not a LINQ source.
    TypeRef* linqSourceElementType(TypeRef* recvTy);

    // stdlib §5 Combination (#1845): for a `Zip(other, (p, q) => …)` call, return
    // the combine lambda's two parameter types {T, U} — T = the receiver's source
    // element, U = the `other` argument's source element — so an untyped `(p, q)`
    // combiner gets its parameters typed (its body then resolves and its return
    // type R is known). Empty when not such a call.
    std::vector<TypeRef*> zipCombineLambdaParamTypes(const ast::CallExpr& ce);

    // stdlib §5 (#1124): for an `Aggregate(seed, (acc, x) => …)` call, return the
    // fold lambda's two parameter types {TAccum, T} — TAccum = the seed argument's
    // type, T = the source element type — so an untyped `(acc, x)` fold lambda gets
    // its parameters typed (its body then resolves; otherwise codegen emits a
    // `ret void` against the i32 accumulator return type). Empty when not such a call.
    std::vector<TypeRef*> aggregateFoldLambdaParamTypes(const ast::CallExpr& ce);

    // §2.3: cache of synthetic TypeSymbol objects for always-available
    // built-in classes (Console, Math, Convert, Environment).
    std::unordered_map<std::string, TypeSymbol*> builtinClassCache_;

    // Return (or lazily create) a synthetic TypeSymbol for a built-in class.
    TypeSymbol* builtinClassSym(std::string_view name);

    // §32.7 / stdlib §10: build a Named TypeRef for a value-builtin resolved
    // by NAME (declaration=null, astNode carries the single-part name). This is
    // the shape codegen's layout keys off to lower IntPtr/UIntPtr to a
    // pointer-sized integer (i64 on host) — a builtinClassSym (Class-kind)
    // declaration would instead lower as a reference pointer. Mirrors the
    // `rb.Pointer` → IntPtr synthesis (fix #985).
    TypeRef* namedValueBuiltinRef(std::string_view name);

    // Slice S29: report when a bare identifier inside a field
    // initializer resolves to a non-static member of the current type
    // (an implicit-`this` reference, forbidden by §22.8). Static
    // members, class constants, enum constants, locals, parameters,
    // types, and free names are all permitted.
    void checkNotInstanceMember(const ast::IdentifierExpr& id, Symbol* sym);

    // §27.4 (v0.183, #1408) / A.7 LambdaDestructure: resolve a destructuring
    // lambda parameter's head to its exact positional-record TypeSymbol
    // (checked against `targetType` when the lambda's expected function type
    // supplies one — §27.4 "no subtype test"; when `targetType` is null the
    // head itself supplies the parameter's type), then bind every (possibly
    // nested) slot as a LocalSymbol into `scope`, typed as the corresponding
    // positional field, mirroring the §19.3 case-pattern destructuring desugar
    // (ExprTypecheck.cpp's CaseStmt/CaseExpr DestructuringPattern handling).
    // Returns a Named TypeRef for the resolved record type, or null when a
    // diagnostic was already reported (unresolved head / class head / target
    // mismatch / arity mismatch).
    TypeRef* bindLambdaDestructure(const ast::DestructuringPattern& dp,
                                   TypeRef* targetType, Scope& scope);

    // §17.11 (#951): does `default(T)` have NO well-defined zero value? True for
    // a non-nullable reference type (string/class/interface/function) and,
    // recursively, a record any of whose positional fields — or an inline array
    // whose element — is such a type. Nullable types, enums, value primitives,
    // value-builtins, and generic-parameter/unresolved refs return false.
    bool defaultIllFormed(const TypeRef* t);

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

    // §19.6 (v0.196, #1816): compose a leaf class's ROOT INSTANTIATION — the
    // instantiation of the root declaration `root` that `leaf` derives from,
    // obtained by composing the base-clause substitutions along `leaf`'s
    // single-inheritance derivation chain (unique per §19.6). The result is a
    // Named TypeRef of `root` whose generic args are expressed in LEAF's own
    // type parameters: for `IntBox: Box<int>` it is `Box<int>`; for
    // `L<Y>: M<Y>` with `M<Z>: D<Array<Z>>` it is `D<Array<Y>>`. Returns null
    // when the chain cannot be composed (a missing/unresolved base ref, a
    // generic base link with mismatched arity, a cycle, or `root` not on the
    // class chain) — the caller then falls back to today's name-match. Reads
    // baseTypeRefs (parallel to baseTypes, carrying generic args) and reuses
    // substituteReturnType as the per-level substitution engine.
    TypeRef* leafRootInstantiation(TypeSymbol* leaf, TypeSymbol* root);

    // §19.3/§5.1/§12.9 (v0.196, #1913): project the Named type `sub` — whose
    // declaration is `ancDecl` or a CLASS-chain subtype of it — up to its
    // instantiation of `ancDecl`, expressed in `sub`'s own concrete generic
    // arguments (`Ok<string>` up to `Result` is `Result<string>`). Composes
    // leafRootInstantiation (which walks the class chain) and substitutes
    // `sub`'s actual args. Returns null when the chain cannot be composed or
    // `ancDecl` is not reached as a class ancestor (an interface ancestor, an
    // arity mismatch) — the caller then conservatively admits the pattern.
    TypeRef* projectTypeToAncestorDecl(const TypeRef* sub, TypeSymbol* ancDecl);

    // §19.6 v0.196 (#1874/#1912): does an unguarded typed binding `var x: bt`
    // cover EVERY non-null value of a scrutinee whose instantiation, projected
    // to `bt`'s declaration, is `proj`? (In the same-declaration case `proj` is
    // the scrutinee itself; in the cross-declaration/ancestor case it is the
    // scrutinee projected up the class chain, cf. projectTypeToAncestorDecl.)
    // Arg-SENSITIVE per §12.9: variance-compatible AND — because the "covers all
    // values" proof must hold at EVERY instantiation of the enclosing body
    // (§31.2.1) — a RIGID scrutinee argument is covered only by that SAME rigid
    // parameter, never a ground or other arg. Shared by the same-declaration and
    // cross-declaration disjuncts of checkCaseExhaustiveness's #784 block.
    bool typedBindingCoversAllValues(const TypeRef* proj, const TypeRef* bt);

    // §19.3/§19.5/§5.1/§12.9 (v0.196, #1913): is the typed binding pattern
    // `var x: patTy` a type-test that NO runtime value of scrutinee type
    // `scrutTy` could ever satisfy — because its generic INSTANTIATION is
    // incompatible with the scrutinee? A typed binding binds the matched value
    // at the STATIC type `patTy`, but the runtime type-test erases generic
    // arguments (checks only the class tag), so a wrong instantiation
    // (`var x: Ok<string>` at scrutinee `Result<int>`) would match at runtime and
    // bind `Ok<int>` as `Ok<string>` — a type-safety hole (§5.1: `Result<int>`
    // is not assignable to `Result<string>`; class hierarchies are invariant,
    // §12.9). This is the ADMISSION counterpart of the arg-SENSITIVE §19.6
    // coverage the #1874 exhaustiveness fix installed. Returns true (=> the
    // caller REJECTS) only when incompatibility is PROVABLE on ground structure:
    // `scrutTy` and `patTy` project (through the class chain) to a common CLASS
    // declaration whose invariant arguments are disjoint and neither
    // reference-converts to the other under §12.9. A rigid/free type parameter,
    // an uncomposable chain, an interface common declaration, an unrelated
    // declaration, or any non-class/interface shape yields a CONSERVATIVE accept
    // (false) — never an over-rejection.
    bool typeTestGenericInstantiationImpossible(const TypeRef* scrutTy,
                                                const TypeRef* patTy);

    // §19.3/§5.1/§12.9 (v0.197, #1920): collect every instantiation of the
    // interface declaration `ifaceDecl` that the Named type `classTy` (a class
    // or interface carrying concrete generic args) implements or extends, each
    // re-expressed in `classTy`'s OWN concrete arguments (`Box<string>` over
    // `Box<T>: IBox<T>` yields `IBox<string>`). BFS over the ancestor graph
    // (single class base + interface bases), substituting the carried concrete
    // args at each level; a declaration reached at TWO distinct instantiations
    // (`C<T>: I<T>, I<int>` — the implemented-twice shape) contributes BOTH
    // entries, deduplicated only by structural identity so diamonds are bounded
    // without merging distinct instantiations. Appends to `out`.
    void collectAncestorInstantiationsOf(const TypeRef* classTy,
                                         TypeSymbol* ifaceDecl,
                                         std::vector<TypeRef*>& out);

    // §19.3/§19.5/§5.1/§12.9 (v0.197, #1920): the INTERFACE-scrutinee
    // counterpart of typeTestGenericInstantiationImpossible. Is the typed
    // binding `var x: patTy` (a class pattern) a type-test that NO runtime value
    // of the generic-interface scrutinee `scrutTy` (`IBox<int>`) could satisfy —
    // because none of the pattern class's implemented instantiations of that
    // interface is compatible with the scrutinee (`Box<string>` implements only
    // `IBox<string>`, disjoint from `IBox<int>` under §12.9)? The erased runtime
    // type-test checks only the class tag, so such a binding would match and
    // bind at a wrong static type. Returns true (=> the caller REJECTS) only for
    // a FINAL pattern class (plain `sealed`, non-abstract — no subclass can add
    // a compatible implementation, §12.4) whose FULL implemented-interface set
    // is disjoint from the scrutinee. A non-final class, an interface pattern, a
    // non-generic or non-interface scrutinee, a class that does not implement
    // the interface at all, or any compatible (incl. rigid/free-parameter)
    // instantiation yields a CONSERVATIVE accept (false) — never an
    // over-rejection.
    bool typeTestInterfaceInstantiationImpossible(const TypeRef* scrutTy,
                                                  const TypeRef* patTy);

    // §19.3/§19.5/§5.1/§12.9 (v0.197, #1927): the erased-type-test UNIQUENESS
    // counterpart of the interface-scrutinee admission check. A typed binding
    // `var x: C<A>` against a generic-interface scrutinee `I<B>` binds the value
    // at the STATIC type `C<A>`, but the runtime type-test erases generic args
    // (it checks only the class tag). So the bind is sound only when `C<A>` is
    // the UNIQUE instantiation of `C` whose values can have static type `I<B>` —
    // otherwise a `C<Q≠A>` value (same erased class tag, also an `I<B>`) matches
    // and binds at the wrong static type. #1920 rejects only when NO
    // instantiation of `C` conforms (the arm is unreachable); this catches the
    // dual hole where `C<A>` DOES conform but is NOT the unique conformer: a
    // GROUND implemented instantiation of `I` (`C<T>: I<T>, I<int>` at scrutinee
    // `I<int>` — every `C<Q>` conforms, #1927(b)), a class parameter left FREE by
    // the scrutinee, or an implemented instantiation whose invariant match pins a
    // parameter to a value OTHER than `A` (`Box<T>: IBox<T>` at `IBox<int>` with
    // pattern `Box<string>` — `Box<int>` conforms, #1927(a)). Returns true (=>
    // the caller REJECTS) only on a PROVABLE second conformer. An INVARIANT
    // scrutinee interface only (a variant one admits a subtype range — a distinct
    // erasure slice, a residual); a rigid/free scrutinee or pattern argument, an
    // imported interface whose variance is unknown, a non-generic pattern class,
    // or any undecidable match yields a CONSERVATIVE accept (false) — never an
    // over-rejection.
    bool typeTestInterfaceBindAmbiguous(const TypeRef* scrutTy,
                                        const TypeRef* patTy);

    // §19.6 (v0.196, #1874): does an ANCESTOR arm of type `armTy` (an
    // instantiation of `matchTy`, a strict ancestor of the DETERMINED generic
    // leaf `leaf`) cover `leaf`'s sole inhabiting instantiation `leaf<σ>`? The
    // subtype walk (§19.3) lets a pattern naming an ancestor cover a leaf
    // descendant, but for a determined generic leaf that cover is arg-SENSITIVE:
    // the leaf is covered only "by an unguarded type pattern it is assignable to"
    // (§19.6), so the leaf's determined instantiation projected up to `matchTy`
    // (leafRootInstantiation(leaf, matchTy) substituted by σ = `solution`) must
    // be IDENTICAL to `armTy` component-wise. A wrong-instantiation ancestor arm
    // (`Result<string>` at scrutinee `Result<int>`) does NOT cover the determined
    // `Ok<int>`/`Err<int>`, closing the residual the direct-path #1862 fix left
    // in the ancestor branch; a bare NamedPattern ancestor (armTy == nullptr)
    // carries no instantiation and never discharges a determined generic leaf.
    bool ancestorArmCoversDeterminedLeaf(
        const TypeRef* armTy, TypeSymbol* leaf, TypeSymbol* matchTy,
        const std::unordered_map<std::string_view, TypeRef*>& solution);

    // §19.6 (v0.196, #1816 s2): the determination outcome of solving a closed-
    // hierarchy leaf's type parameters against a GROUND scrutinee `S` — the
    // multiplicity of the leaf's inhabiting instantiations at `S`. `NoSolution`
    // (empty set) ⇒ the leaf is uninhabited at `S` and EXEMPT from coverage;
    // `Determined` (a singleton) ⇒ the leaf's obligation is the single admitted
    // instantiation carried in `solution`; `Undetermined` (>1, or undecidable at
    // this decidability level) ⇒ leaf coverage is not pinned here (later lanes).
    struct LeafInstSolutions {
        enum class Outcome { NoSolution, Determined, Undetermined };
        Outcome outcome = Outcome::Undetermined;
        // The sole inhabiting substitution (leaf-param name -> ground TypeRef),
        // populated only when `outcome == Determined`.
        std::unordered_map<std::string_view, TypeRef*> solution;
        // §19.6 (v0.196, #1816 s3): the leaf parameters left FREE after a clean
        // solve — those with NO occurrence in the root instantiation. Populated
        // (in declaration order) only in the free-parameter `Undetermined` sub-
        // case; empty for the undecidable (InvMatch::Unknown) `Undetermined`
        // sub-case. A non-empty list marks the leaf's coverage statically
        // UNATTAINABLE (every substitution inhabits — the opposite of exempt):
        // no type-test arm covers it, only an irrefutable arm does.
        std::vector<std::string_view> freeParams;
        // §19.6 (v0.196, #1816 s4): set by the RIGID-scrutinee solver when the
        // leaf's coverage is statically UNATTAINABLE — either an undecidable
        // rigid-identity component (a ground/composite/different-rigid component
        // of the root instantiation equated against a rigid enclosing parameter,
        // e.g. `IntError: Result<int>` at `Result<T>`) or a free leaf parameter.
        // The leaf is NEVER exempt under a rigid `S`; when this is set the caller
        // records it in the unattainable set (naming `freeParams`, possibly
        // empty). Defaults false — the ground solver never sets it, so s1/s2/s3
        // are undisturbed.
        bool unattainable = false;
    };

    // §19.6 (v0.196, #1816 s2): solve `leaf`'s type parameters against a GROUND
    // scrutinee `scrutTy` (the caller passes the NON-NULLABLE form of `S`).
    // Composes `leaf`'s root instantiation (leafRootInstantiation) and unifies it
    // against `scrutTy` COMPONENT-WISE, INVARIANT, EXACT — no §5.1 conversion and
    // no §12.9 variance step, the syntactic/decidable image of the runtime test.
    // A structural mismatch (distinct ground constructors or ground arguments) or
    // a repeated-parameter inconsistency (`L<Y>: D<Y,Y>` against `D<int,string>`)
    // yields NoSolution ⇒ exempt; a clean solve binding every leaf parameter
    // yields Determined; a leaf parameter left unbound (no occurrence in the root
    // instantiation) or any undecidable component yields Undetermined. The caller
    // MUST gate this on `isFullyGroundTypeRef(scrutTy)` — a rigid `S` is a later
    // lane's domain and this solver does not engage on it.
    LeafInstSolutions solveLeafInstantiations(TypeSymbol* leaf, TypeSymbol* root,
                                              const TypeRef* scrutTy);

    // §19.6 (v0.196, #1816 s4): the NON-GROUND (rigid) counterpart of
    // solveLeafInstantiations — engaged when the scrutinee `S` contains a rigid
    // enclosing-body type parameter (`isFullyGroundTypeRef(scrutTy)` is false),
    // the case the ground solver defers. Matches the leaf's composed root
    // instantiation against `S` under a RIGID-AWARE three-valued identity that
    // keys rigidity SYNTACTICALLY (isInScopeTypeParam — never null-decl
    // typeRefEqual, whose blindness would wrongly solve a rigid equation and
    // reopen the false-exemption hole). A leaf is NEVER exempt under a rigid `S`:
    // an undecidable rigid-identity component (`IntError: Result<int>` at
    // `Result<T>`) or a free leaf parameter sets `unattainable` (coverage
    // statically unattainable — requires a `_` arm, §19.6); a clean determined
    // solve (`Ok<T>: Result<T>` at `Result<T>` ⇒ `T`) is left for the name-match
    // to discharge, so the determined rigid shape stays exhaustive.
    LeafInstSolutions solveLeafInstantiationsRigid(TypeSymbol* leaf,
                                                   TypeSymbol* root,
                                                   const TypeRef* scrutTy);

    // §19.6 (v0.196, #1816 s5): CLAUSE (b) of the determination rule — FILTER
    // the ground solver's sole (Determined) solution `solution` by the leaf's
    // own + every chain-base's SCALAR `where`-constraints (`class` / `value` /
    // `notnull` / `new`; a type-bound `where Y: I` is s6). Returns true iff
    // `solution` PROVABLY violates such a constraint on some decl along the
    // `leaf` → `root` derivation chain — then σ is not a well-formed
    // instantiation (§14.2), the singleton solution set empties, and the caller
    // marks the leaf uninhabited/EXEMPT. Only a PROVABLE violation drops the
    // solution; a satisfied or undecidable constraint keeps it (never a false
    // exemption of an inhabited leaf).
    bool leafSolutionViolatesScalarConstraint(
        TypeSymbol* leaf, TypeSymbol* root,
        const std::unordered_map<std::string_view, TypeRef*>& solution);

    // §19.6 (v0.196, #1816 s5): does a FULLY-GROUND solved type PROVABLY satisfy
    // a `new` constraint (an accessible parameterless ctor / a well-defined
    // default)? Mirrors Resolver::argSatisfiesNew (§14.2 / §17.11). `decided` is
    // set false when unverifiable here — the `new` filter then keeps the
    // solution (never a false exemption).
    bool groundSatisfiesNew(const TypeRef* arg, bool& decided);

    // §19.6 (v0.196, #1816 s6): does a FULLY-GROUND solved type PROVABLY satisfy
    // a type-bound `where Y: I` (§14.2)? Discharged by declaration-based
    // subtyping (isSubtype) on an already-GROUND σ(Y) — a free parameter is
    // NEVER rescued by a census of the bound's conformers (the §19.6 census
    // prohibition). `decided` is set false when unverifiable here — the filter
    // then keeps the solution (never a false exemption; declines D8).
    bool groundSatisfiesTypeBound(const TypeRef* arg, const TypeRef* bound,
                                  bool& decided);

    // §5.1 / §19 (#1153): range-check each integer `case` pattern constant /
    // range-bound literal against an integer scrutinee's type. A literal that
    // does not fit was silently truncated to the scrutinee width at codegen
    // (litAtWidth) → wrong matches; reject it with the §5.1 narrowing
    // diagnostic instead. Engaged from both the CaseExpr and CaseStmt handlers.
    void checkCasePatternConstantRanges(const std::vector<ast::CaseArm>& arms,
                                        TypeRef* scrutTy);

    // §19.3 (#1847): validate a TOP-LEVEL bare-identifier `case`/`when` pattern.
    // The grammar leaves a single-/qualified-identifier pattern ambiguous (§A.7):
    // it is a TYPE TEST when the name resolves to a type (or a type parameter,
    // v0.182), a CONSTANT pattern when it resolves to a named `const`, and an
    // ENUM-CONSTANT pattern when it names a constant of the scrutinee enum. A
    // bare identifier that resolves to none of these — a runtime local/parameter/
    // field VALUE, or an entirely unresolved name — is neither a type nor a
    // constant and must be a compile error. Sema previously let it through and
    // codegen's NamedPattern handler returned nullptr ("deferred"): in statement
    // position the arm silently vanished (the mandatory `_` catch-all skipped,
    // §19.6); in expression position `case.end` was left unterminated (malformed
    // IR / ICE). Engaged for a top-level NamedPattern from both the CaseExpr and
    // CaseStmt handlers.
    void checkTopLevelNamedPattern(const ast::NamedPattern* np, TypeRef* scrutTy);

    // §19.3 (#1847): shared diagnostic for a top-level bare-identifier pattern
    // that resolves to a runtime VALUE (a local, parameter, or field) rather
    // than a type or constant.
    void reportNamedPatternIsValue(const ast::NamedPattern* np,
                                   std::string_view name);

    // Slice S21: does a callable with this return-type ascription
    // produce a value? No ascription → no (unit). An ascription that
    // resolves to the unit type → no. Anything else → yes.
    bool returnsValue(const ast::Type* rt);

    // async-readiness 3b (§30.1): compute the return-FORM context and the
    // result-assignability type for an `async` method's body by peeling the
    // task wrapper. `Task` → VoidMethod (no result); `Task<T>` / `OwningTask<T>`
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

    // §8.6 / §5.1 (#1311): does a primitive value type box into a builtin
    // generic interface it implements — IComparable<T>/IEquatable<T> where T
    // is the primitive's own keyword alias (stdlib §9.1–§9.5: every numeric
    // and char primitive implements both; Boolean implements IEquatable<bool>
    // only, NOT IComparable — §9.4)? Recognised by: `to` is a single-arg
    // IComparable/IEquatable whose argument is the SAME primitive as `fromPrim`
    // (no widening — Byte implements IComparable<byte>, not IComparable<int>).
    // Used by checkAssignable (storage boxing) and implicitlyConvertible
    // (argument-to-parameter boxing) so the two paths agree.
    static bool primitiveBoxesIntoBuiltinIface(PrimitiveType fromPrim,
                                               const TypeRef* to);

    // Slices T1/T4: assignability check (§5.1, §7, §12 subtyping).
    // Handles two cases and conservatively skips everything else
    // (mixed primitive/named, generics, null, Object target) so it
    // never false-positives while the type lattice is incomplete:
    //   - primitive → primitive: identity or §5.1 widening.
    //   - named → named (non-generic): identity or subclass/interface.
    // A non-nullable target accepting a nullable source is rejected.
    void checkAssignable(TypeRef* from, TypeRef* to,
                         SourceLocation loc, const std::string& what,
                         const ast::Expr* fromExpr = nullptr);

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

    // §31.2.1: nearest common base of the types listed in a multi-type catch
    // arm binding (`var e: T1, T2, ...`). Returns the TypeRef to use as the
    // binding's static type: the most specific Named type that is a base of
    // every listed type. When no user-declared common base exists (e.g. all
    // types directly inherit a built-in like `Exception`), the built-in base
    // TypeRef is returned. Returns null if the types are empty or unresolved.
    TypeRef* catchArmNcb(
        const std::vector<std::unique_ptr<ast::Type>>& types);

    // #1621 / §31.2.1 / §20: the simple name of a Named TypeRef — its
    // TypeSymbol's name when declared, else the AST use-site's last
    // qualified-name part (for a built-in with no TypeSymbol, e.g.
    // `IOException`), else `importedName` (a synthetic built-in-ancestor
    // TypeRef minted by `builtinAncestorTypeRef`, which has neither). Empty
    // for a non-Named ref. Shared by `commonTypeOf`'s `convertsInto` and
    // `catchArmNcb`'s ancestor walk so both name a type identically.
    static std::string_view namedRefSimpleName(const TypeRef* ref);

    // #1621 / §31.2.1: mint a headless Named TypeRef standing for a built-in
    // exception ancestor name reached only through
    // `common::builtinExceptionParentOf` (no AST node names it directly —
    // e.g. `Exception` reached while walking up from `OverflowException`).
    // `declaration` stays null (it IS a built-in) and `importedName` carries
    // the name so `namedRefSimpleName` still reports it correctly.
    TypeRef* builtinAncestorTypeRef(std::string_view name);

    // #1621 / §31.2.1: the ancestor-name chain of `ty` (most-specific first,
    // `ty` itself included), walking the TypeSymbol base-type chain, the AST
    // TypeDecl's declared bases (built-in bases have no TypeSymbol and are
    // only visible here), AND — once a walked node is itself a built-in
    // Named type with no further AST bases — the built-in exception
    // hierarchy table (`common::builtinExceptionParentOf`), so a purely
    // built-in ancestor chain (e.g. `OverflowException` ->
    // `ArithmeticException` -> `Exception`) is still discoverable. Bounded:
    // a node is "visited" the first time its name (or, for an unnamed node,
    // its pointer) is seen, and the guard gates the WALK itself, so a §12.10
    // base-type cycle that reaches this walk during error recovery cannot
    // loop forever (#1624 precedent); a hard iteration cap is defense in
    // depth.
    std::vector<std::pair<std::string_view, TypeRef*>> namedAncestorChain(
        TypeRef* ty);

    // #1621 / §31.2.1: does `ty` (or one of its ancestors, per
    // `namedAncestorChain`) carry the simple name `ancestorName`? Used by
    // both `convertsInto` (does `from` subtype/widen into `to`, by name, when
    // one side is a built-in with no TypeSymbol to run `isSubtype` on) and
    // `catchArmNcb`'s Step 2 (does every OTHER listed type also have this
    // candidate ancestor?).
    bool hasNamedAncestor(TypeRef* ty, std::string_view ancestorName);

    // §11 / §12.9 / §5.1 (#1935): does the USER-declared type `from` actually
    // implement (directly or transitively) a BUILTIN interface whose simple name
    // is `builtinBaseName`? Interface conformance in Bezel is nominal — a type
    // conforms only by listing the interface among its bases — so a builtin base
    // absent from `from`'s transitive declared-base closure is PROOF `from` does
    // not conform. A builtin interface base carries a null declaration and is
    // therefore dropped from `baseTypeRefs` (TypeResolution records only bases
    // with a resolved TypeSymbol); it survives only on the SYNTACTIC AST base
    // list, and its own builtin supers (IList<T> → … → IEnumerable<T>) live only
    // in the hardcoded builtin interface hierarchy. This walk consults all three
    // — `baseTypeRefs`, the AST base list, and `builtinInterfaceAncestors` on any
    // builtin node — so a class implementing a more-derived builtin interface
    // still counts as implementing its builtin supers. Used by `checkAssignable`
    // to REJECT a user-declared source assigned to a builtin generic interface it
    // does not implement, instead of `variantGenericAssignable`'s conservative
    // user→builtin residual accept.
    bool userTypeImplementsBuiltinIface(TypeRef* from,
                                        std::string_view builtinBaseName);

    // Slice S7: diagnose assignment to a readonly field outside the
    // declaring class's constructor. Tracks only the `this.<field>`
    // target shape; `other.f := X` would need cross-object readonly
    // tracking that we don't have yet (and rarely matters in practice).
    void checkReadonlyAssignment(const ast::AssignmentStmt& as);

    // §6.6 / A.6 (v0.184, #1690): reject an assignment/increment `target`
    // whose lvalue chain bases on a method-call result — grammar A.6
    // restricts an LValue's base to `Identifier | "this"`, and §6.6 states
    // "A.6's lvalue shape applies unchanged: a method-call result cannot
    // base the chain — extract it to a local first." Shared by
    // AssignmentStmt and IncrementStmt, which both consume grammar A.6's
    // `LValue` production.
    void checkLValueChainNotCallBased(const ast::Expr& target);

    // §22.3 "ref and out argument locations" (v0.184, #1682): validates a
    // bare `ref`/`out` call argument's operand against the section's
    // assignable-storage-location categories — a mutable local variable, a
    // writable field, an indexer slot, or a property with an accessible
    // setter — and its declarative exclusions: no `using var` binding, no
    // readonly field/property outside its assignable context (§22.8/§23.1),
    // and no mutable static admitted only in the two forms of §22.12 rule 2.
    // The section's remaining exclusion — no location of OWNED state (an
    // owned local/parameter, an owned field, an owning-collection/owning-
    // inline-array slot) — is dataflow-dependent (whether a plain local
    // currently holds an owned value) and is checked separately by the
    // ownership pass (MethodOwnership::checkRefOutOwnedOperand), which
    // tracks live ownership state; this function only covers the
    // declaration-based categories. A no-op for every call-argument
    // modifier except the two BARE pre-declared forms "ref" and "out" —
    // "out var"/"out using var"/"out owned var" always declare a FRESH
    // local (no pre-existing-location question), and "owned"/"scoped" are
    // unrelated call-site markers (§22.3 / §28.14).
    void checkRefOutOperandCategory(const ast::CallArgument& a);
    // Shared by the Identifier and MemberAccess cases of
    // checkRefOutOperandCategory: readonly-outside-context (§22.8) and
    // mutable-static-rule-2 (§22.12) for a field operand.
    void checkRefOutFieldOperand(FieldSymbol& fs, const ast::Expr& target,
                                 const char* kind);
    // Shared by the MemberAccess and Index cases of
    // checkRefOutOperandCategory: no-setter / readonly-auto-outside-context /
    // setter-visibility for a property (incl. indexer) operand — mirrors the
    // §23.1/§23.3/§13.6 assignment-target checks inline in visitStmt.
    void checkRefOutPropertyOperand(PropertySymbol& ps, const ast::Expr& target,
                                    const char* kind);

    // Slice S5: member lookup that walks the base-type chain.
    // BFS over the inheritance graph (no cycle detection — the spec
    // forbids cycles; sema's class-graph validation slice will catch
    // them). First match wins; ambiguity resolution between sibling
    // interfaces is a separate slice.
    Symbol* lookupMember(TypeSymbol* type, std::string_view name);

    // §26.5: extension-member lookup. Consulted ONLY when no instance member
    // matches (instance members win — an extension never shadows a real
    // member). Searches the module's ExtensionSymbols for one whose target
    // type equals `receiver` (or one of its ancestor types, per §26.5 step 2)
    // and that declares a member named `name`; returns that MemberSymbol, or
    // null. `receiver` is the receiver's resolved TypeRef (its nullability is
    // ignored — §26.2 forbids `extension int?`).
    // §26.5 (#714): when a generic-extension method is found (extension has
    // free type variables), the inferred binding (e.g. T → int) is written into
    // `outBinding` so the caller can record it for typeCall and codegen.
    Symbol* lookupExtensionMember(
        const TypeRef* receiver,
        std::string_view name,
        std::vector<std::pair<std::string, TypeRef*>>* outBinding = nullptr);

    // §26.5 step 1 (v0.183): when an instance member of `name` name-blocks
    // extension lookup for `receiver` and the committed instance call then
    // fails (no applicable overload, wrong arity, or inaccessible), the
    // spec requires the diagnostic to "note a same-named extension where
    // one exists" — a name-blocked extension was never even considered, so
    // without this note a caller cannot tell their extension was shadowed
    // rather than simply not found. Returns an empty string when no
    // same-named extension matches `receiver` (append nothing), else a
    // clause of the form "; note: ..." ready to append to a diagnostic
    // message.
    std::string extensionNameBlockedNote(const TypeRef* receiver,
                                         std::string_view name);

    // §26.1 / §26.5: bind a resolved extension member to the member-access
    // expression — set its exprSymbol and the access's exprType. For a method
    // this is the §27.6 method-reference function type (the CallExpr handler
    // reads the MethodSymbol's decl for a call's return type); for a computed
    // property it is the property's declared type.
    void resolveExtensionMember(const ast::MemberAccessExpr& ma,
                                Symbol* member);

    Symbol* lookupName(std::string_view name);

    // §16.4 / §19.6 (issue #738): per-`case` pattern legality checks shared by
    // the expression and statement case forms — reject `step` in a range
    // pattern (stride has no membership meaning, §16.4) and an unguarded `_`
    // catch-all in a non-last arm (§19.6). Both are clean diagnostics that
    // replace a downstream codegen IR failure / silently-unreachable arms.
    void validateCaseArms(const std::vector<ast::CaseArm>& arms);

    // §19.3 (v0.167, #1371): every member of a `|` pattern-alternative list must
    // resolve to a CONSTANT — a type member is a compile error "in any
    // position", at arm level or inside a positional destructuring slot.
    // `checkAltMembersConstant` walks a pattern (recursing through
    // DestructuringPattern slots and, per §19.3 v0.183 / #1584, AltPattern
    // members) and reports a member that names a type; `altMemberNamesType`
    // resolves whether one member expression denotes a type, `altPartsNameType`
    // the shared dotted-name resolution over an already-split QualifiedName.
    void checkAltMembersConstant(const ast::Pattern* p);
    bool altMemberNamesType(const ast::Expr* c);
    bool altPartsNameType(const std::vector<std::string_view>& parts);

    // §31.1 (issue #739): the operand of `throw` must be an exception — a CLASS
    // instance. A primitive/string/record/enum/function thrown value miscompiles
    // (codegen treats it as an exception-object pointer → SIGSEGV); reject it
    // with a clean diagnostic. No-op when the value's type is not yet resolved.
    void checkThrowableType(const ast::Expr& value);

    // §5.1 (issue #738): fold `e` to a compile-time integer constant, resolving
    // `const` identifiers through the typecheck scope. Used to decide whether an
    // int initializer/value may implicitly narrow to a narrower integer target
    // (a literal/arithmetic/const may; a variable or call result may not).
    std::optional<long long> foldIntConstExprTC(const ast::Expr& e);

    // §17.14 (v0.184, #1688): does `e` contain a `/` or `mod` node whose RHS
    // folds to a compile-time zero, using the SAME identifier resolution as
    // foldIntConstExprTC (lexical-scope `lookupName`, not a type's member-
    // const list)? Called when foldIntConstExprTC(e) returns nullopt for a
    // local `const` initializer, to give the specific div/mod-by-zero
    // diagnostic instead of silently accepting the expression (which then
    // compiled and deferred the fault to a runtime DivideByZeroException).
    bool localConstDivByZeroTC(const ast::Expr& e);

    // §15.1 (issue #734): fold a LOCAL-`const` inline-array size onto its
    // resolved TypeRef. Member/module consts are folded earlier (type
    // resolution); a local const is only in scope during this pass, so when a
    // local declaration's type is `[N]T` with N still unfolded (arraySizeValue
    // < 0) and N resolves to in-scope local consts, set arraySizeValue so
    // codegen builds `[N x T]` rather than a flat scalar. No-op otherwise.
    void foldLocalConstArraySize(TypeRef* declType);

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

    // §22.4 (v0.183, #1585) caller-side narrowing: if `cond` is a call to a
    // method that declares a `when true`/`when false` out-nullability
    // postcondition, and the requested branch is the one where the result
    // equals the stated constant, record the out recipient in `nullNarrowed_`
    // (known non-null). Returns true iff any narrowing was applied. The call
    // was already visited (so `cond`'s callee resolved and out-recipient
    // symbols are bound), and the #1188 out-argument invalidation already
    // cleared the recipient — this re-establishes it on the matching branch.
    bool narrowFromOutPostcond(const ast::Expr* cond, bool forTrueBranch);

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

    // §22.4 (v0.183, #1585) callee-side check: at a point where the body
    // returns the value `returnValue` (a `return E;` or the expression body of
    // an expression-bodied method), verify that every out-nullability
    // postcondition param whose stated constant this path may produce is
    // provably non-null. Uses §7 narrowing (`collectNarrowings` of the returned
    // expression) so `return conn <> null;` discharges a `when true` clause and
    // a path returning the other constant carries no obligation. A no-op unless
    // the enclosing bool method declares postcondition params.
    void checkOutPostcondAtReturn(const ast::Expr* returnValue,
                                  SourceLocation loc);
    // §22.4 (v0.183, #1585): the out-nullability postcondition params of the
    // method body currently being walked — (param symbol, whenTrue) pairs.
    // Populated in `runMethodBody` only for a bool-returning method; cleared
    // across a nested lambda body so a lambda `return` never discharges the
    // enclosing method's clause. Saved/restored by `runMethodBody`.
    std::vector<std::pair<ParameterSymbol*, bool>> currentOutPostconds_;

    // §7.4 (v0.182, #1566): recompute `mutablyCapturedNames_` from a method body
    // (its statements and/or expression body). Syntactic full-body scan: records
    // the name of every local/parameter assigned or passed `ref`/`out` inside any
    // lambda, excluding a lambda's own parameters (which shadow an outer name).
    void computeMutablyCaptured(
        const std::vector<std::unique_ptr<ast::Stmt>>* stmts,
        const ast::Expr* exprBody);

    // §7.4 (v0.182, #1566): drop the null-narrowing (and any §12.7 type-
    // narrowing) of every currently-narrowed local whose name is in
    // `mutablyCapturedNames_`. Called at each control-transfer operation inside a
    // controlled block; a no-op when nothing in the body is mutably captured.
    void invalidateCapturedNarrowings();

    void visitMember(const ast::Member& m);

    // §22.12 (#747 R42): true iff `e` is a compile-time-constant expression
    // admissible as a `const` member initializer — a literal (not an
    // interpolated string), a unary/binary/conditional expression over
    // constants, or a reference to another const / enum constant. Must be
    // called AFTER the initializer is type-checked (it reads exprSymbol).
    bool isConstInitExpr(const ast::Expr& e);

    // §22.13 (#747 R42): reject a `static readonly` field whose initializer
    // reads a `static readonly` field of the same type declared LATER. Called
    // with the FieldDecl after its initializer is type-checked.
    void checkStaticReadonlyForwardRef(const ast::FieldDecl* fd);

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
    // argument static types (null when unresolved). `fillsDefault[i]` is
    // true when candidate i's §22.5 binding left one or more of its
    // parameters to be filled by their declared defaults — used by §22.6's
    // defaulted-arity tie-break (v0.181): a candidate applicable without
    // filling any default is preferred over one that fills one or more,
    // provided it is better-or-equal at every supplied-argument position.
    // Emits the ambiguous / unreachable diagnostic itself when applicable.
    // `candNames` supplies a display string per candidate for the
    // ambiguous-case citation. `extraNote` (default empty), when non-empty,
    // is appended verbatim to an emitted diagnostic's message — used by
    // member-access call sites to carry the §26.5 step 1 name-blocked-
    // extension note (see extensionNameBlockedNote); callers outside the
    // member-access path (e.g. `new T(...)` constructor overloads) pass the
    // default and get no such suffix.
    OverloadResult resolveOverload(
        const std::vector<std::vector<const TypeRef*>>& paramTypes,
        const std::vector<const TypeRef*>& argTypes,
        const std::vector<bool>& fillsDefault,
        const std::vector<std::string>& candNames, SourceLocation loc,
        const std::string& calleeDesc,
        const std::string& extraNote = std::string());

    // §22.6 method-overload resolution. Given the initially-resolved
    // MethodSymbol for a call and the call site, returns the MethodSymbol
    // the call resolves to: applies type-based §22.6 selection when the
    // name has multiple same-arity, all-positional candidates and arg/param
    // types are decidable; otherwise falls back to arity-based first-wins
    // (preserving the prior behavior on named-arg / modifier / undecidable
    // calls). Emits the ambiguous / unreachable diagnostics itself.
    MethodSymbol* resolveMethodOverload(MethodSymbol* initial,
                                        const ast::CallExpr& ce);

    // §22.6/§12.1 (#1090): collect every same-name method overload visible on
    // `start` and its base classes, most-derived first, with a more-derived
    // overload shadowing a less-derived one of the same signature (so an
    // override does not duplicate the method it overrides). Lets overload
    // resolution and the arity check see an overload declared in a base class
    // that `lookupMember`'s first-match BFS stopped short of.
    void collectHierarchyOverloads(TypeSymbol* start, std::string_view name,
                                   std::vector<MethodSymbol*>& out);

    // §14.5 (v0.184, #1686): a `ref`/`out` argument bound to a generic
    // method's type parameter must unify EXACTLY with the parameter's
    // substituted type — no conversion, and no join across conflicting
    // occurrences, at either the binding or the later applicability check.
    // Runs UNCONDITIONALLY (independent of whether the call's return type
    // needs a substitution) for a call to a generic method that has at
    // least one ref/out parameter whose declared type mentions a method
    // type parameter. Returns true when a §14.5 diagnostic was emitted, in
    // which case the caller must stop typing the call (no codegen for a
    // rejected call).
    bool checkGenericRefOutExactUnification(const ast::CallExpr& ce,
                                            const ast::MethodDecl* md);

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
    // `receiver` (optional, default null): the dot-call's receiver TypeRef,
    // when known. §26.5 step 1 (v0.183): when supplied and access is
    // rejected, the diagnostic notes a same-named extension that matches
    // `receiver` where one exists (name-blocked by the instance member,
    // never consulted) — see extensionNameBlockedNote.
    bool checkMemberVisibility(const MemberSymbol* msym,
                               std::string_view memberName,
                               SourceLocation loc,
                               const TypeRef* receiver = nullptr);

    // §7.5 null-conditional member access `x?.M`. Resolves the member on
    // the target's (non-nullable) type exactly like `.`, but the result
    // type is ALWAYS nullable (the access yields null when the target is
    // null). The member's own type is looked up; its nullable form is the
    // expression's type, so member access chains (`a?.b?.c`) resolve.
    void resolveNullCondMember(const ast::NullCondMemberExpr& nm);

    // §7.5 / #751: result type of a builtin (string/primitive) RECEIVER's
    // property-like value member (e.g. `string.Length`, `string.IsAscii`) for
    // the null-conditional `x?.M` path. resolveMemberAccess types these via a
    // large inlined intrinsic block keyed on the MemberAccessExpr node, which
    // resolveNullCondMember cannot run — so `x?.Length` was left untyped and a
    // following `??` (lowerCoalesce) silently read 0. Returns the member's
    // NON-nullable result type, or null when unrecognized; the caller lifts it
    // to the nullable form. Only the property-like value members reachable as a
    // bare `?.` access are covered — the numeric-primitive intrinsics
    // (CompareTo, Abs, …) are all METHODS, reached through `?(`
    // (NullCondCallExpr), not this path.
    TypeRef* builtinValueMemberType(const TypeRef* recvTy,
                                    std::string_view member);

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

    // §25.2 / §12.2 (v0.181, #1562): collect every `operator <token>` overload
    // of the given arity declared on `ts` OR inherited from any of its base
    // classes. A derived-typed operand reaches its base's operators (there is
    // no user-facing upcast that could otherwise reach them); §12.2's per-token
    // no-hiding rule keeps a single inheritance chain duplicate-free. Walks the
    // `baseTypes` chain transitively with a seen-set (cycle-safe).
    std::vector<MethodSymbol*> collectInheritedOperators(TypeSymbol* ts,
                                                         std::string_view token,
                                                         std::size_t arity);

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

    // §28.11 / §4.15 (#871): if `operand` is `coll[i]` whose receiver `coll` has
    // a BORROWING-span static type (ArraySpan<T>/ReadOnlyArraySpan<T> — views into
    // a borrowing Array<T> — or any read-only span), return that span type name;
    // otherwise the empty string. Such spans' slots hold borrows with no
    // ownership to transfer, so `move span[i]` is rejected at compile time. The
    // OWNING `Span<T>` (over an `OwningBuffer`) is NOT matched here.
    std::string moveThroughBorrowingSpanSlotName(const ast::Expr& operand) const;

    // §19.3 / §15.3 (v0.184, #1665): if `operand` is `s[i]` where `s` is a
    // `case`/`when` pattern binding (a LocalSymbol whose `decl` is a pattern
    // `BindingPattern`, not a plain `var`-declared LocalDecl) over a MOVE-ONLY
    // owning-inline-array scrutinee, return the bound name; otherwise the
    // empty string. The binding is a borrow of the scrutinee's value — it
    // does not own the array — so `move s[i]` is rejected at compile time by
    // `typeUnary`'s `move` branch.
    std::string moveThroughBorrowedMoveOnlyArrayBindingName(
        const ast::Expr& operand) const;

    // §19.3 / §15.3 (v0.196, #1745): the whole-binding sibling of the above.
    // If `operand` is a bare identifier `s` naming a `case`/`when` pattern
    // binding (a LocalSymbol whose `decl` is a pattern `BindingPattern`) over a
    // MOVE-ONLY owning-inline-array scrutinee, return the bound name; otherwise
    // the empty string. The binding is a borrow of the scrutinee's value — it
    // does not own the array — so `move s` (moving the WHOLE binding) is
    // rejected by `typeUnary`'s `move` branch, just as `move s[i]` (a single
    // slot) is: laundering the whole borrowed value would duplicate ownership
    // the original scrutinee still holds.
    std::string moveThroughBorrowedMoveOnlyArrayWholeBindingName(
        const ast::Expr& operand) const;

    // §30.14 / §3.6 (#600): sendability derived-type property.
    // Returns true iff `rt` satisfies the §30.14 sendable table for a
    // Channel<T> element or Domain.Spawn argument/result.
    //
    // The `inProgress` set implements the coinductive recursion rule:
    // a class or named type being evaluated is assumed sendable until
    // proven otherwise.  Callers should pass an empty set on initial
    // call; recursive sub-calls share the same set.
    bool isSendable(const TypeRef* rt,
                    std::unordered_set<const TypeSymbol*>& inProgress) const;

    // Convenience overload with a private inProgress set (top-level call).
    bool isSendable(const TypeRef* rt) const {
        std::unordered_set<const TypeSymbol*> ip;
        return isSendable(rt, ip);
    }

    // §30.14 (#600): true iff the entry argument to Domain.Spawn is
    // capture-free: a static-method-reference (MemberAccessExpr whose
    // target is a TypeSymbol) or a non-capturing lambda (a LambdaExpr
    // whose body references no locals from the enclosing scope).
    bool isCaptureFreeEntry(const ast::Expr* entryExpr) const;

    // Helper: does the lambda body (recursively) reference any identifier
    // whose resolved symbol is NOT in `ownParamSyms` and is a local or
    // parameter symbol (i.e. captured from the outer scope)?
    bool lambdaBodyCaptures(
        const ast::LambdaExpr* lam,
        const std::unordered_set<const Symbol*>& ownParamSyms) const;

    // Walk the given expression tree and check if any IdentifierExpr
    // resolves to a symbol that is a local/parameter NOT in ownParamSyms.
    bool exprCaptures(const ast::Expr& e,
                      const std::unordered_set<const Symbol*>& ownParamSyms) const;
    bool stmtCaptures(const ast::Stmt& s,
                      const std::unordered_set<const Symbol*>& ownParamSyms) const;

    void visitChildren(const ast::Expr& e);
};

}  // namespace bezel::sema
