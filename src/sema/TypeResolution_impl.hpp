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

    // §12.1 (v0.181, #1560): does a derived (override) parameter type MATCH
    // a base parameter type — exactly, or via the one admitted widening
    // (base `T`, derived `T?`)? Mirrors §11.2's interface-implementation
    // nullability allowance; the reverse (base `T?`, derived `T`) is NOT a
    // match. Public so other passes (§22.6 overload-set dedup —
    // ExprTC::collectHierarchyOverloads must recognize a widened override as
    // shadowing its base declaration, not as a second, separately-callable
    // overload) can reuse it via the free-function wrapper
    // `bezel::sema::overrideParamTypeMatches` in TypeResolution.hpp.
    // `isByRefParam` (#1592): true when the parameter's modifier is `ref`/
    // `out` (any out-variant) — a VALUE-typed widening under such a modifier
    // is still declined (see overrideWideningIsAbiSafe in
    // TypeResolution_Checks.cpp): the caller's storage is sized for the
    // narrow, un-boxed form, and a by-reference parameter aliases that
    // storage directly rather than passing a copy a vtable thunk could
    // marshal.
    static bool overrideParamTypeMatches(const TypeRef* derivedTy,
                                         const TypeRef* baseTy,
                                         bool isByRefParam = false);

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

    // §2.2 v0.164 / #1365: import aliases declared in this source file.
    // Maps alias identifier → dotted module name.
    // E.g. `import BankingApp.Transactions as Tx;` → {"Tx" → "BankingApp.Transactions"}
    std::unordered_map<std::string, std::string> importAliases_;

    // §2.2 v0.164 / #1365: bare names ambiguous because ≥2 imported modules
    // export a public type with the same name (§2.2 collision rule). Using a
    // bare ambiguous name is a compile error; qualification resolves it.
    std::unordered_set<std::string> ambiguousImportNames_;

    // Slice S38 (§13.7 / Appendix C.2): a class's base class and
    // implemented interfaces must be at least as visible as the class.
    // Effective top-level visibility defaults to `internal`. Restricted
    // to top-level types (nested-type effective visibility per §11.6 is
    // deferred). Bases resolving to a built-in (null declaration) are
    // effectively public and skipped.
    static int effVisRank(ast::Visibility v);

    void checkBaseVisibility();
    // §13.3 (#956): a member's signature types must be at least as visible as
    // the member (targets the clear private-type-in-non-private-member case).
    void checkMemberSignatureVisibility();
    void checkTypeMemberSignatureVisibility(TypeSymbol* t);

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

    // §22.12 rules 2 and 4 (v0.163, #1363): statics-and-domains enforcement.
    //   Rule 2: a mutable (non-readonly, non-const) static field or static
    //     auto-property whose resolved type is a user-declared class or
    //     interface is a compile error unless the type is in the compiler-known
    //     internally-synchronized set (Mutex, Semaphore, Atomic<T>,
    //     AtomicInteger<T>, Channel<T>, ChannelSender<T>).
    //   Rule 4: a static readonly field/auto-property of user-declared class or
    //     interface type emits a declaration-site warning unless the type is in
    //     the synchronized set.
    //   Primitive value types, string, enums, and records are not checked here
    //   (Rule 3 makes them unrestricted; the Rule 2 extension to primitive
    //   value types is tracked as #1363-followup).
    void checkStaticFieldDomains();
    void checkTypeStaticFieldDomains(TypeSymbol* t);
    void checkOperatorGroups();  // §25.2: comparison/equality operator groups
    // §8.3 (#1193): a type overriding `Equals` must also override `GetHashCode`.
    void checkEqualsHashCodePairing();
    // §25.2 (v0.158, #1338): a record/enum that overloads `=`/`<>` with BOTH
    // operands of the declaring type must also declare `Equals(T)` and
    // `override GetHashCode`.  Heterogeneous-operand overloads are exempt.
    void checkEqualityCoDeclaration();
    // §3.6 / §8.3 (v0.158, #1338): every recorded `Dictionary<K,V>` /
    // `OwningDictionary<K,V>` key (K) and `Set<T>` / `OwningSet<T>` element (T)
    // must be equality-bearing (§3.6 rule 2, at the instantiation site).
    void checkHashContainerKeys();

    void checkStaticClassMember(TypeSymbol* t, Symbol* m);

    // Slice S14: within one type, two members that aren't both
    // overloadable cannot share a name. Methods and constructors are
    // overloadable (same name, different signature is fine); fields,
    // properties, events, constants, and nested types are not. A
    // collision between a non-overloadable member and ANY other member
    // of the same name is an error.
    void checkDuplicateMembers();

    // §11.3 / §13.2 (#747 R42): two top-level types in one module may not share
    // a name (the member/nested clash rule extended to the module namespace).
    void checkDuplicateTopLevelTypes();

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

    // #1272 (§9): true when the value-type record `start` transitively contains
    // itself BY VALUE (a self-referential record has infinite inline size and
    // cannot be laid out). Walks the value-containment graph from `cur` — record
    // fields, nullable-record fields (`T?` for a value type is still inline),
    // and inline-array-of-record fields — following only value-typed records; a
    // class/interface field (a heap reference) breaks the cycle. `seen` guards
    // against unbounded recursion through other records. A recursive record must
    // use a class (heap reference) instead.
    bool recordValueContainsSelf(TypeSymbol* start, TypeSymbol* cur,
                                 std::unordered_set<TypeSymbol*>& seen);

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

    // #1614 (§12.1): a property/indexer `override` must match the base
    // member's signature — the property (return) type exactly, the accessor
    // set/get presence, and (for indexers) the index-parameter signature
    // (count, modifiers, types with the §12.1 T→T? widening). Mirrors the
    // method-override check (checkOverrideSignatureMatch) for the
    // property/indexer member kinds, which previously had NO check at all.
    void checkPropertyOverrideSignatureMatch(PropertySymbol* derived,
                                             PropertySymbol* base);

    // §14.2 (v0.182, #1579): are two `where <T>: ...` clauses on a type
    // parameter the SAME constraint set — compared order-insensitively and
    // after type-alias resolution? A null pointer denotes the type parameter
    // carrying no `where` clause at all (the empty set). Shared by
    // checkOverrideSignatureMatch's override-constraint-inheritance check.
    bool constraintSetsEquivalent(const ast::GenericConstraint* a,
                                  const ast::GenericConstraint* b);

    // Build the base-class generic substitution map (base class T → the
    // concrete arg the derived class binds it to) for an override pair, e.g.
    // Container<T> base, IntBox : Container<int> → {T → int}. Shared by
    // checkOverrideSignatureMatch and overrideParamsMatch (#1090).
    void buildBaseGenericSubst(
        MethodSymbol* derived, MethodSymbol* base,
        std::unordered_map<std::string_view, TypeRef*>& out);

    // §12.1/§22.5 (#1090): does the derived method's parameter list match the
    // base method's (same arity, same parameter types after generic subst)?
    // Used to pick the correct overload among several same-name base members
    // before validating an override. Non-reporting.
    bool overrideParamsMatch(MethodSymbol* derived, MethodSymbol* base);

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

    // §26.4/§26.5 (#1599): recursively walks an extension target type's
    // generic-argument tree, binding each bare single-segment non-nullable
    // identifier as a block-level type parameter — at ANY nesting depth
    // (v0.183: `extension IEnumerable<Array<T>>` binds `T`). A generic arg
    // that is itself a generic instantiation (carries its own non-empty
    // genericArgs, e.g. `Array<byte>` or `Array<T>`) is a CONCRETE
    // instantiation head per §26.4 — that head name (`Array`) must NOT be
    // bound as a free type parameter; instead we recurse into its own
    // generic args to find any nested bare identifiers.
    void bindExtensionTargetTypeParams(const ast::Type& target,
                                        Scope& extScope, Scope* parent);

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

    // §28.14 / §22.3 (v0.175): `scoped` is valid only on a FUNCTION-typed
    // parameter / field. Checks the RESOLVED type (so a type alias to a
    // function type is admitted transparently) and reports otherwise.
    // `what` selects the diagnostic wording ("parameter" / "field").
    void checkScopedIsFunctionTyped(const ast::Type* declaredType,
                                    Scope* scope, std::string_view name,
                                    std::string_view what,
                                    SourceLocation loc);

    // §28.14 (v0.180): a `scoped` parameter on a STATIC method is admitted
    // only when the method's return is `owned` — otherwise no call site can
    // ever resolve a bounding object (form 2 "using var" or form 3 "owned
    // field of this") for the stored callback/borrow: there is no receiver,
    // no constructed object, and no instance to hold the stored value.
    // Applies uniformly to function-typed and class/interface-typed `scoped`
    // parameters — both share the same bounding-object mechanism.
    void checkScopedStaticNeedsOwnedReturn(const std::vector<ast::Param>& params,
                                           bool isStatic, bool returnOwned,
                                           std::string_view methodName,
                                           SourceLocation loc);

    // §22.4 (v0.183, #1585): a `when true`/`when false` out-nullability
    // postcondition is admitted ONLY on an `out`/`out owned` parameter of
    // nullable type in a bool-returning method. `returnType` is the enclosing
    // method's declared return type (null for a void method or a non-method
    // context such as a constructor/operator/indexer, in which the clause is
    // never admitted). Reports a compile error for every ill-placed clause.
    void checkOutNullPostcond(const std::vector<ast::Param>& params,
                              const ast::Type* returnType, Scope* scope,
                              SourceLocation loc);

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

    // §11.8 / §14.2 (v0.176, #807): a type-bound constraint failure recorded
    // DURING base-list resolution, re-checked after every type's baseTypes are
    // populated. The recurrence form `class Meters: INumber<Meters>` checks the
    // `where T: INumber<T>` bound on the INumber<Meters> instantiation while
    // Meters' own base list is still being resolved — Meters.baseTypes does not
    // yet contain INumber, so an eager check false-rejects the self-referential
    // conformance. Deferring the *report* (not the check) closes the ordering
    // hole for every mutually/self-referential conformance without weakening
    // the constraint: the re-check runs with complete base sets and reports
    // any persistent violation.
    struct DeferredTypeBoundFailure {
        SourceLocation loc;
        std::string param;      // constraint's type-parameter name
        std::string boundName;  // bound's declaration name (diagnostic)
        std::string argName;    // argument spelling (diagnostic)
        TypeRef* arg = nullptr;
        TypeRef* bound = nullptr;
    };
    std::vector<DeferredTypeBoundFailure> deferredTypeBoundFailures_;
    // Flush deferredTypeBoundFailures_: re-run argSatisfiesTypeBound with all
    // base sets populated; report the entries that still fail. per §14.2.
    void recheckDeferredTypeBounds();

    // §11.8 (v0.176, #807): a class/record declaring conformance to an
    // interface with STATIC members (static methods / static operators) must
    // supply matching statics with exact signatures (the §11.2 rule applied
    // to statics: same name/arity/param types/param modifiers/return type
    // including `owned`; operators match by token + signature). Runs after
    // base types resolve. Covers both user static interfaces and the
    // stdlib §2.19 builtins (INumber/ISteppable/IParsable).
    void checkStaticInterfaceConformance();

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

    // §12.9 / §11.8 (v0.181, issue #1559): variance markers (`in`/`out`) are
    // interface-only syntax — the shared `GenericParam` grammar (Appendix
    // A.4) admits the marker on every generic-parameter list (classes,
    // records, enums, methods, type aliases, extension methods), but only an
    // interface's OWN type parameters may carry one; everywhere else it is a
    // compile error. A method's type parameters are never variant, even
    // inside an interface. Additionally, an interface that declares a static
    // member (a `static` method, or an operator — implicitly static, §25)
    // may not mark its own parameters variant either (§11.8). Driving loop
    // over top-level declarations; recurses into nested types.
    void checkVarianceMarkerScope();
    void checkVarianceMarkerScopeForType(TypeSymbol& t);
    void checkMethodGenericParamsNoVariance(Symbol* member);
    static bool declaresStaticMember(TypeSymbol& t);

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

    // §3.6 / §8.3 (v0.158, #1338): one hash-container instantiation site whose
    // key/element must be equality-bearing. Recorded while resolving a
    // `Dictionary`/`OwningDictionary`/`Set`/`OwningSet` `Named<...>` annotation;
    // validated by checkHashContainerKeys() after every member type is resolved
    // (a record key's auto-equality-bearing property depends on the record's own
    // members being resolved, which may happen after this site is visited).
    struct HashContainerSite {
        std::string_view container;   // "Dictionary" / "Set" / …
        std::string_view property;    // "key" / "element" (for the diagnostic)
        TypeRef* keyRef = nullptr;    // the K / T type argument
        const ast::Type* site = nullptr;  // the annotation node (span)
        // §1384: when keyRef is a generic parameter (declaration == null), the
        // enclosing type is stored so checkHashContainerKeys can walk
        // instantiations of that type to find the concrete bound for the key.
        TypeSymbol* enclosingType = nullptr;
    };
    std::vector<HashContainerSite> hashContainerSites_;

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
