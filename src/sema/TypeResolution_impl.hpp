#pragma once

// Private implementation header for the type-resolution worker.
//
// Declares the `Resolver` class whose methods are defined across several
// translation units (TypeResolution.cpp and TypeResolution_*.cpp), mirroring
// the codegen split (cf. LlvmBackend_impl.hpp). This header is included ONLY
// by those implementation files; the public surface stays in
// TypeResolution.hpp (resolveTypes / primitiveFromName).
//
// `Scope` is nested as a private type of `Resolver`: an identically-named
// `bezel::sema::Scope` also exists (in ExprTypecheck.cpp), so keeping it at
// namespace scope here would be a program-wide ODR clash. As a nested type it
// cannot collide.

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// Walker for type-resolution. Threads a Scope* through the AST visit.
class Resolver {
public:
    Resolver(SemaContext& ctx) : ctx_(ctx) {}

    void run(const ast::SourceFile& src);

    // Slice S19: structural equality of two resolved TypeRefs. Returns
    // false (conservatively) when either is null or an Error kind — the
    // caller skips the overload-equality check rather than risk a false
    // collision on a not-yet-resolvable type. Public so other passes
    // (§22.6 overload resolution) can reuse it via the free-function
    // wrapper `bezel::sema::typeRefEqual` in TypeResolution.hpp.
    static bool typeRefEqual(const TypeRef* a, const TypeRef* b);

private:
    // Lookup environment in scope. A stack of scopes (module-level + per-type
    // + per-method). Each scope contributes a small name → Symbol table.
    struct Scope {
        std::unordered_map<std::string_view, Symbol*> names;
        Scope* parent = nullptr;
    };

    SemaContext& ctx_;

    // §2.15 / issue #631: the set of dotted namespace names declared with
    // `import` in the current source file. Populated in run() from
    // src.imports and consulted in resolveNamed to gate builtin types that
    // require an explicit import (BinarySerializer / BinaryDeserializer).
    std::unordered_set<std::string> sourceImports_;

    // Slice S38 (§13.7 / Appendix C.2): a class's base class and
    // implemented interfaces must be at least as visible as the class.
    // Effective top-level visibility defaults to `internal`. Restricted
    // to top-level types (nested-type effective visibility per §11.6 is
    // deferred). Bases resolving to a built-in (null declaration) are
    // effectively public and skipped.
    static int effVisRank(ast::Visibility v);

    void checkBaseVisibility();

    // Slice S37 (§22.7 / Appendix C.5): a static constructor takes no
    // parameters. (The no-access-modifier rule is a parser concern.)
    void checkStaticConstructorParams();

    void checkTypeStaticCtorParams(TypeSymbol* t);

    // Slice S36 (§12.8 / Appendix C.5): a constructor may contain at
    // most one `base(...)` / `this(...)` chain call, and it must be the
    // first statement (the chaining position).
    void checkConstructorChains();

    // True if `s` is a constructor chain call: an expression statement
    // whose expression is a call on the bare identifier `this`/`base`.
    static bool isChainCall(const ast::Stmt* s);

    void checkTypeConstructorChains(TypeSymbol* t);

    // Slice S35 (§12.6 / Appendix C.6): `async` does not combine with
    // `abstract` on a method. (An abstract async method declares a
    // Task / Task<T> return type without `async` instead.)
    void checkAsyncModifiers();

    void checkTypeAsyncModifiers(TypeSymbol* t);

    // Slice S34 (§12.1): "Fields, constructors, destructors, and static
    // members may not be virtual." A static method or property carrying
    // `virtual` / `abstract` / `override` is rejected.
    void checkStaticNotVirtual();

    void checkTypeStaticNotVirtual(TypeSymbol* t);

    // Slice S33: a method's body presence must match its kind.
    //  - In a class: a method marked `abstract` must have no body, and
    //    a non-abstract method must have one (§12.5).
    //  - In an interface: methods have no body (§11.2); a body is an
    //    error.
    // Records / enums: treated like classes (their methods are
    // concrete). Operators are always concrete and handled by the
    // parser's body requirement.
    void checkMethodBodies();

    void checkTypeMethodBodies(TypeSymbol* t);

    // Slice S32 (§10.5): an enum may not declare instance fields,
    // constructors, nested types, or indexers, and its methods may not
    // be virtual / abstract / override / sealed override. Static
    // fields and static methods are permitted (§10.4).
    void checkEnumRestrictions();

    // §14.2 (#746): a type parameter may not carry contradictory constraints
    // (e.g. both `class` and `value`). Checked once per generic declaration
    // (type + method `where` clauses), independent of instantiation.
    void checkGenericConstraintConsistency();

    // Slice S30 (§22.12): a `const` declaration requires a primitive
    // value type or `string`. Object, named types, arrays, functions,
    // and unit are rejected. Only the explicit-ascription case is
    // checked here; type-inferred consts (and the compile-time-constant
    // requirement on the value) are deferred.
    void checkConstTypes();

    void checkTypeConsts(TypeSymbol* t);

    // Slice S28 (§13.6): a property/indexer accessor's explicit
    // visibility must be more restrictive than the property's overall
    // visibility; an accessor that is *less* restrictive is a compile
    // error. Restrictiveness rank: public(3) > internal/protected(2) >
    // private(1). `internal` and `protected` are incomparable (same
    // rank), so we only flag a strictly-less-restrictive accessor.
    static int visRank(ast::Visibility v);

    void checkAccessorVisibility();

    void checkTypeAccessorVisibility(TypeSymbol* t);

    // Slice S24 (§2.3): generic arity of the always-available built-in
    // type names. kNotBuiltin means "not a built-in"; kBuiltinTaskLike
    // marks `Task`, which is valid both bare (`Task`) and unary
    // (`Task<T>`). `Object` is handled earlier as a primitive.
    static constexpr int kNotBuiltin = -1;
    static constexpr int kBuiltinTaskLike = -2;
    static int builtinTypeArity(std::string_view n);

    // Slice S23 (§9.7): a record may not have a destructor, an `owned`
    // field, an inline-array field whose slots carry the `owned`
    // marker, or implement `IDisposable`. Records have copy semantics
    // only and never own a resource.
    void checkRecordRestrictions();

    // §9.8 (issue #550): records and enums that declare `: ISerializable` or
    // `: IDeserializable` must satisfy the synthesis-eligibility rule.  Types
    // that declare the conformance but are ineligible (ineligible component
    // type, no hand-written Serialize/Deserialize) are a compile error at the
    // conformance declaration.
    void checkSerializationEligibility();

    // Slice S22 (§13.2): a top-level type may be `public` or `internal`
    // (the latter is also the default when no visibility keyword is
    // written). `protected` and `private` are rejected — there is no
    // enclosing class for `protected` and no file-scope concept for
    // `private`.
    void checkTopLevelVisibility();

    // Slice S19: the overload-distinguishing form of a parameter
    // modifier (§22.6). `owned` on an ordinary parameter does NOT
    // distinguish overloads — it normalizes to the plain value form —
    // so two overloads differing only by ordinary `owned` collide.
    // `out owned` IS distinct from `out`.
    static std::string_view overloadModifier(std::string_view mod);

    // Slice S19 (§22.6): within a type, two same-named methods (or two
    // constructors) may not have signatures that are indistinguishable
    // under overload resolution — same parameter count, the same
    // overload-modifier on each parameter, and structurally equal
    // parameter types. Conservatively skips a pair when any parameter
    // type fails to resolve.
    void checkOverloadSignatures();

    void checkTypeOverloads(TypeSymbol* t);

    bool signaturesIndistinguishable(const std::vector<Symbol*>& a,
                                     const std::vector<Symbol*>& b);

    // Slice S17 (§11.7): `abstract` and `static` are mutually
    // exclusive; `sealed` and `static` are mutually exclusive.
    // `sealed abstract` is permitted (a closed abstract class).
    void checkClassModifierCombinations();

    // Slice S16 (§11.7): a class declared `static` may contain only
    // static members — no instance fields, methods, properties,
    // indexers, instance constructors, destructors, or owned fields.
    // A static constructor, static members, class-level consts (which
    // are implicitly static), and nested types are permitted.
    void checkStaticClassMembers();
    void checkStaticIndexers();  // §23.5: static indexers are not supported

    void checkStaticClassMember(TypeSymbol* t, Symbol* m);

    // Slice S14: within one type, two members that aren't both
    // overloadable cannot share a name. Methods and constructors are
    // overloadable (same name, different signature is fine); fields,
    // properties, events, constants, and nested types are not. A
    // collision between a non-overloadable member and ANY other member
    // of the same name is an error.
    void checkDuplicateMembers();

    // #594 (§28.2): user code may not declare a method named `Dispose()` on
    // a class or record. The compiler synthesises `Dispose()` from the
    // `destructor` (§28.4); a user-declared `Dispose()` is never called by
    // the ownership machinery and silently traps.
    void checkUserDeclaredDispose();

    static bool isOverloadable(Symbol* m);

    void checkTypeDuplicates(TypeSymbol* t);

    // Slice S12: DFS over baseTypes; if we revisit a node already in
    // the current path, report a cycle anchored at the offending
    // TypeSymbol. Each cycle is reported once (at its smallest entry).
    // Returns true if any cycle was found.
    bool checkInheritanceCycles();

    // Slice S13: is this method member abstract? A method is abstract
    // when its parent is an interface (interface methods are implicitly
    // abstract), when it carries the `abstract` modifier, or when its
    // MethodDecl has an Abstract body.
    static bool hasMod(MemberSymbol* m, std::string_view want);

    // Slice S15: a base member is overridable when it is `virtual`,
    // `abstract`, or `override` — but not `sealed override` (which
    // seals it against further overriding, §12.4).
    static bool baseMemberOverridable(MemberSymbol* m);

    // Slice S20 (§12.1): an override must match the base member's
    // visibility, return type, parameter types, and parameter modifiers
    // exactly. Parameter modifiers that must match: ref, out, owned.
    // Return modifier that must match: owned. Only methods are handled
    // here (property/indexer override-match is a later slice). When the
    // base member of the same name is itself an overload set, this
    // matches against the single representative S15 recorded — refining
    // to overload-aware matching awaits the overload-resolution slice.
    // §12.1 / §14: when a derived class overrides a method from a generic
    // base class (e.g. `class Box : Container<int>`), the base method's
    // type references use the generic param (T). Apply the substitution
    // T→int before comparing against the override's concrete types.
    // Returns the substituted TypeRef if `tr` is a generic param that
    // appears in `genericSubst`, otherwise returns `tr` unchanged.
    TypeRef* applyGenericSubst(
        TypeRef* tr,
        const std::unordered_map<std::string_view, TypeRef*>& genericSubst);

    void checkOverrideSignatureMatch(MemberSymbol* derivedSym,
                                     MemberSymbol* baseSym);

    // Slice S15: §12.2 forbids a derived class from reusing a base
    // CLASS member's name except to override a virtual/abstract base
    // member (§12.1). Interface members are excluded — re-declaring an
    // interface member is implementation (matching), not hiding.
    void checkHidingAndOverride();

    bool methodIsAbstract(MethodSymbol* ms);

    // Slice S13: walk a concrete class's inheritance chain; every
    // abstract method name must be matched by a concrete method
    // somewhere in the chain. Name-based (overload-aware matching is a
    // later slice).
    void checkAbstractImplementations();

    // Per §12: inheritance must be well-founded (no cycles).  Diamond
    // inheritance (shared base through multiple paths) is legal for interfaces
    // — each base is visited once per DFS path, not globally, so a shared
    // ancestor is not a cycle.  `onStack` = nodes currently being explored
    // (detecting true back-edges); `done` = fully-explored nodes that need
    // not be re-entered.
    bool checkCycleFrom(
        TypeSymbol* t,
        std::unordered_set<TypeSymbol*>& onStack,
        std::unordered_set<TypeSymbol*>& done,
        std::vector<TypeSymbol*>& path,
        std::unordered_set<TypeSymbol*>& reported);

    void resolveTopDecl(const ast::TopDecl& decl, Scope* parent);

    void resolveTypeDecl(const ast::TypeDecl& td, Scope* parent);

    void resolveMember(const ast::Member& m, Scope* parent);

    Symbol* lookupName(std::string_view name, Scope* scope);

    // Slice 3d: walk a method/ctor body and resolve every type
    // ascription on LocalDecl / catch arm. Recurses through control-
    // flow constructs. Locals themselves are not registered here (that
    // happens later in ExprTypecheck); we only need their `: Type`
    // ascriptions resolved so identifier-on-local typing works.
    //
    // Slice 3f: also resolves types embedded in expressions —
    // NewExpr::type, TypeTestExpr::testType, TypeOfExpr::queriedType,
    // DefaultExpr::type, lambda return/param types — so ExprTypecheck
    // can type those expressions.
    void resolveTypesInBody(
        const std::vector<std::unique_ptr<ast::Stmt>>& body, Scope* scope);
    void resolveTypesInStmt(const ast::Stmt& s, Scope* scope);
    void resolveTypesInExpr(const ast::Expr& e, Scope* scope);

    // §19: resolve the Type nodes embedded in a case-arm pattern (a typed
    // `var x: T` binding, a primitive `T` type pattern, or any nested
    // sub-pattern of a destructuring). Without this, `bp->type` carries no
    // resolved TypeRef and ExprTypecheck cannot type the bound name (so a
    // member access on it in the guard/body fails to resolve — issue: a
    // member-access guard `var c: Circle when c.r > 5` crashed codegen).
    void resolveTypesInPattern(const ast::Pattern& p, Scope* scope);

    TypeRef* resolveType(const ast::Type& t, Scope* scope);

    // §15.1 (issue #734): fold an inline-array size `N` to a compile-time
    // integer. N must be a compile-time integer expression: integer literals,
    // `const` declarations whose value is itself an int-const expression, and
    // arithmetic on these are all admitted. Returns nullopt when `e` is not a
    // foldable int-const (e.g. references a not-yet-bound local const, an enum
    // constant, or a non-integer expression). `depth` guards against cyclic
    // const references.
    std::optional<long long> foldIntConstExpr(const ast::Expr& e, Scope* scope,
                                              int depth);

    void resolveNamed(const ast::Type& t, Scope* scope, TypeRef& ref);

    // §14.2 / Appendix C.4: at a generic instantiation site
    // (`Named<...>` type, including `new C<...>()`), verify every supplied
    // type argument satisfies the constraints its corresponding type
    // parameter declares in the type's `where` clause. `tsym` is the
    // resolved generic type; `t` carries the AST argument nodes (used to
    // skip arguments that are themselves generic params — those can't be
    // classified statically); `argRefs` are the resolved argument TypeRefs
    // (positionally aligned with `tsym->genericParams`); `scope` is used to
    // detect whether an argument name is an in-scope generic parameter.
    void checkGenericConstraints(const ast::Type& t, TypeSymbol* tsym,
                                 const std::vector<TypeRef*>& argRefs,
                                 Scope* scope);

    // §14.2: check a single constraint `item` (one of class / value /
    // notnull / new() / a type-bound) against type argument `arg` (resolved)
    // for the type parameter named `paramName`. `t` supplies the diagnostic
    // span; `argAst` is the argument's AST node (for its spelling);
    // `boundScope` resolves a type-bound constraint's bound on demand.
    void checkOneConstraint(const ast::Type& t, std::string_view paramName,
                            const ast::ConstraintItem& item,
                            const ast::Type* argAst, TypeRef* arg,
                            Scope* boundScope);

    // §14.2: is the AST type node `t` a reference to a generic parameter
    // currently in scope (rather than a concrete type)? Used to skip
    // constraint checks on arguments whose classification is only known at
    // the outer instantiation.
    bool isGenericParamRef(const ast::Type& t, Scope* scope);

    // §14.2 / Appendix C.4 reference-vs-value classification of a resolved
    // argument TypeRef. Unknown means "can't classify here" (built-in named
    // type with no user declaration, generic param, Error) — the caller
    // defers rather than risk a false positive.
    enum class TypeCategory { Reference, Value, Unknown };
    TypeCategory categorize(const TypeRef* a);

    // §14.2 interface/base conformance: does `arg` (a Named TypeRef with a
    // resolved declaration) have `bound`'s declaration in its transitive
    // base/interface set, or equal it? BFS over baseTypes, cycle-guarded.
    bool argSatisfiesTypeBound(const TypeRef* arg, const TypeRef* bound);

    // §14.2 `where T: new()`: does the resolved argument type admit a
    // parameterless construction? Sets `decided=false` when the argument
    // can't be classified (built-ins, unresolved, generic param) so the
    // caller defers; otherwise sets `decided=true` and returns the verdict.
    bool argSatisfiesNew(const TypeRef* arg, bool& decided);

    // §12.9 / issue #431: for each interface with variant generic parameters
    // (`out T` or `in T`), verify that every direct occurrence of those
    // parameters in the interface's member signatures (method return types,
    // method parameter types, property types) is in the correct position.
    // `out T` (covariant) must appear only in output positions; `in T`
    // (contravariant) must appear only in input positions. Nested positions
    // (T inside a function-type parameter or inside another generic's args)
    // are deferred — the composition rules are sound but handled separately.
    void checkVariancePositions();

    // §32.5: validate that the AST type `t`, appearing in an `extern` method
    // signature, is an FFI-compatible type. `returnPosition` permits the unit
    // type `()` (valid as a return type only). `declSpelling` names the
    // offending site for the diagnostic message ("parameter `x`" or "return
    // type"). Reports a §32.5 error when `t` is not FFI-compatible and returns
    // false; returns true when compatible. The check is shape-driven on the
    // AST so it recognizes the FFI-boundary named types
    // (CString/WString/RawBytes/IntPtr/UIntPtr), which have no user TypeSymbol.
    bool checkExternType(const ast::Type& t, bool returnPosition,
                         const std::string& declSpelling);

    // §32.5: the FFI-compatibility predicate without diagnostics. Used by
    // checkExternType and, recursively, for inline-array element types and
    // function-type parameter/return types. `returnPosition` permits unit.
    static bool isFfiCompatibleType(const ast::Type& t, bool returnPosition);

    // ---- §14.2: nested-generic constraint propagation -------------------
    //
    // When an outer generic's body instantiates an inner constrained generic
    // with one of the outer's own type parameters as the argument
    // (e.g. `class Box<W> ... C<W> ...` where `class C<T> where T: new()`),
    // the inner constraint cannot be checked while resolving the outer body —
    // W's classification is unknown there. The obligation is deferred to each
    // *concrete* instantiation of the outer generic (`Box<IFoo>` binds
    // W = IFoo, so `C<W=IFoo>` must satisfy `where T: new()`).
    //
    // Resolution is a single source-order pass, so the outer instantiation
    // site may be visited before *or* after the inner generic's body. To stay
    // order-independent we record both halves during the pass and join them in
    // a final step (checkDeferredNestedConstraints):
    //   - DeferredNestedObligation: one inner constraint that depends on an
    //     outer type parameter (recorded while resolving the outer body).
    //   - GenericInstantiation: one concrete instantiation of a user generic
    //     (recorded at every `Named<...>` site for a user TypeSymbol).

    // The TypeSymbol whose body is currently being resolved (set in
    // resolveTypeDecl). Null at module scope. Used to attribute a deferred
    // generic-param argument to the type parameter / owner it belongs to.
    TypeSymbol* currentOuterType_ = nullptr;

    struct DeferredNestedObligation {
        TypeSymbol* outerType = nullptr;        // e.g. Box (owns outerParam)
        std::string_view outerParamName;        // e.g. "W"
        std::string_view innerParamName;        // e.g. "T" (on innerType)
        const ast::ConstraintItem* item = nullptr;  // the inner constraint
        const ast::Type* innerSite = nullptr;   // the `C<W>` AST node (span)
    };
    std::vector<DeferredNestedObligation> deferredNested_;

    struct GenericInstantiation {
        TypeSymbol* type = nullptr;                  // e.g. Box
        TypeSymbol* enclosingType = nullptr;         // body this site sits in
        std::vector<TypeRef*> argRefs;               // concrete arg TypeRefs
        std::vector<const ast::Type*> argAsts;       // arg AST nodes (spelling)
        const ast::Type* site = nullptr;             // the `Box<IFoo>` node
    };
    std::vector<GenericInstantiation> instantiations_;

    // §14.2: record deferred obligations for an inner instantiation whose
    // argument `argAst` is a generic parameter of the type currently being
    // resolved. `innerParam` is the inner generic's parameter the argument
    // binds (its constraints, looked up in `innerDecl`'s `where` clauses, are
    // the obligations); `innerSite` is the inner `Named<...>` AST node.
    void recordDeferredNested(const ast::Type& argAst,
                              const GenericParamSymbol& innerParam,
                              const ast::TypeDecl& innerDecl,
                              const ast::Type& innerSite);

    // §14.2: final join — for every deferred inner obligation, re-check it
    // against each concrete binding the outer type's instantiation sites
    // supply for the obligation's outer parameter.
    void checkDeferredNestedConstraints();
};

}  // namespace bezel::sema
