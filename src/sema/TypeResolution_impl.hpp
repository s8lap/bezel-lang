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

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/Node.hpp"
#include "sema/ConstFold.hpp"   // §6.5/§17.14 (#3045): ConstShiftDomainFault
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

    // §19.3 (#1847): is `n` the simple name of a BUILT-IN (stdlib) type — any
    // built-in class/interface/record/enum/value type the compiler resolves
    // without a user `TypeDecl` (Exception and its hierarchy, DateTime, the
    // collections, StringBuilder, …)? A thin predicate over the authoritative
    // `builtinTypeArity` table, exposed (via the free-function wrapper
    // `isBuiltinTypeName` in TypeResolution.hpp) so ExprTC's top-level
    // case-pattern validator can recognize a built-in type-test pattern
    // (`case e of IOException => …`) without re-listing the built-in name set.
    static bool isBuiltinTypeName(std::string_view n) {
        return builtinTypeArity(n) != kNotBuiltin;
    }

    // §9.8 / #2325: the CLASS half of checkSerializationEligibility's
    // component-eligibility and constructor-selection checks. Split into its
    // own LATE pass — called from INSIDE typecheckExpressions() itself
    // (ExprTypecheck.cpp, via the free-function wrapper below), not as part
    // of resolveTypes() (this class's run()) — because §9.8 counts a
    // primary-constructor parameter CAPTURED into a private field (§11.1:
    // referenced in a body position — a method/property/constructor/
    // destructor body, e.g. `property Id: int => id;`) as a genuine
    // serialized component, and that classification
    // (FieldSymbol::posParamBodyRef/posParamInitRef) is only computed by
    // ExprTypecheck's OWN expression walk (ExprTypecheck_Exprs.cpp) — so this
    // check cannot run any earlier than the end of that same function.
    // Originally this was wired as a separate call the DRIVER made after
    // typecheckExpressions(); that broke the moment a test-harness pipeline
    // (which builds its own fresh SemaContext + resolveTypes +
    // typecheckExpressions independent of Driver::compileFile — there are
    // dozens of such call sites, not just the driver's) called
    // typecheckExpressions() without ALSO separately calling this — silently
    // readmitting classes that should have been rejected (verified: this
    // regressed 4 tests in serialization_tier2_classes_test.cpp before the
    // fix). Living inside typecheckExpressions() itself means every existing
    // and future caller gets it automatically, with no separate wiring to
    // remember or to miss. Public so the free-function wrapper
    // `bezel::sema::checkClassSerializationEligibilityLate` (TypeResolution.hpp)
    // — which ExprTypecheck.cpp calls — can reach it from a fresh Resolver,
    // exactly like run() itself.
    void checkClassSerializationEligibilityLate();

    // fix #3235 (§30.14 / §14.2, v0.254): the §14.2 `where T: sendable`
    // satisfaction check runs INSIDE `resolveTypes()` (from
    // `checkOneConstraint`'s `item.kind == "sendable"` arm), but
    // `FieldSymbol::posParamBodyRef` — the §22.10 captured-vs-uncaptured
    // bit that decides whether a primary-ctor parameter counts as a
    // §30.14 rule-2 storage member — is computed by ExprTypecheck's
    // OWN expression walk (`ExprTypecheck_Exprs.cpp`), which runs AFTER
    // `resolveTypes()` finishes. Reading `posParamBodyRef` at the
    // constraint site therefore answers "uncaptured" for every class,
    // and a captured primary-ctor parameter of a non-sendable type
    // passes `where T: sendable` — the accept-hole #3165 noted as
    // unresolved on the §14.2 route.
    //
    // The fix mirrors #2325's `checkClassSerializationEligibilityLate`
    // pattern: record each `where T: sendable` site in
    // `pendingSendableConstraints_` (a span, the constraint param name,
    // and the resolved argument TypeRef — the same fields the in-place
    // `report` lambda reads), DO NOT call `isSendableType` here, and
    // replay the recorded list at the tail of `typecheckExpressions()`
    // via `checkSendableConstraintsLate` (which the free function
    // `bezel::sema::checkSendableConstraintsLate` invokes, parallel to
    // `checkClassSerializationEligibilityLate`). At that point the
    // expression walk has finished, so `posParamBodyRef` is set on
    // every captured parameter, and the §30.14 storage walk in
    // `isSendableType` reads the bit correctly.
    //
    // This is also the deferred-nested (`checkDeferredNestedConstraints`)
    // path: nested sendable obligations recorded during resolve are
    // replayed through `checkOneConstraint`'s sendable arm, which
    // appends to `pendingSendableConstraints_` — same late-pass
    // discharge. No special wiring for that case.
    void checkSendableConstraintsLate();

    // §14.2 / fix #3160: §14.2 constraint check at an instantiation site
    // that does NOT have an `ast::Type` node — the receiver of a
    // `Type<...>.staticMember` access (`Ferry<Bad>.Count()`). Public so the
    // free-function wrapper `bezel::sema::checkGenericConstraintsAtExpr`
    // (TypeResolution.hpp) can call it from a fresh Resolver; same pattern
    // as `checkClassSerializationEligibilityLate` and
    // `checkSendableConstraintsLate`. See the function's own comment for
    // why the deferred-nested / deferred-type-bound paths are intentionally
    // skipped here.
    void checkGenericConstraintsAtUseSite(
        SourceLocation useSite,
        const std::vector<const ast::Type*>& argAsts,
        const std::vector<TypeRef*>& argRefs, TypeSymbol* tsym);

    // §22.12 rule 4 residual + §9.7 static-field bullet (v0.265, #3273 /
    // #3290): value-site pass for `static readonly` fields/auto-properties
    // whose declared type is a borrow-bearing record, a non-owning choice
    // that holds borrows, or a plain inline array carrying borrows. The
    // §9.7 null-source form requires every reference-typed field (and
    // every reference-typed slot of every inline-array field) of the
    // stored value to be `null`; this walk examines the initializer and
    // emits a §9.7 diagnostic when a non-null reference-typed component
    // is statically observable. CancellationToken's value-builtin flag
    // makes the predicate false there, so its `None` sentinel and the
    // every-default `token := CancellationToken.None` pass untouched.
    // Lives on Resolver for the same reason as
    // checkStaticReadonlyBareTInstantiations: it needs the diagnostic
    // stream. Called from typecheckExpressions() AFTER initializers have
    // been typechecked, so positional-argument resolution has named every
    // argument.
    void checkStaticReadonlyNullSource();

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

    // fix #3574 (§11.6 "Nested types", v0.286): the ONE recursive walk over
    // every type this module declares — each top-level type and, transitively,
    // every type nested inside one, pre-order in declaration order. Thirteen
    // check* passes below hand-copied a one-level walk and so stopped enforcing
    // their rule at nesting depth 2; they all route through this instead.
    void forEachTypeAndNested(const std::function<void(TypeSymbol*)>& fn);

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
    // When non-null, `*isThis` receives true for `this(...)` and false for
    // `base(...)` (written only when the function returns true) — the
    // §22.10 convenience-constructor rule below needs the two apart.
    static bool isChainCall(const ast::Stmt* s, bool* isThis = nullptr);

    void checkTypeConstructorChains(TypeSymbol* t);

    // Slice S35 (§12.6 / Appendix C.6): `async` does not combine with
    // `abstract` on a method. (An abstract async method declares a
    // Task / Task<T> return type without `async` instead.)
    void checkAsyncModifiers();

    void checkTypeAsyncModifiers(TypeSymbol* t);

    // §22.15 (v0.202, #2052): placement + signature restrictions on `pure`
    // members. A pure member is rejected on member kinds that do not admit
    // it (events, fields, destructors, static constructors), in combination
    // with `async`, on iterator methods (those whose body contains `yield`),
    // when its signature carries a `ref`/`out`/`out owned`/`owned`/`scoped`
    // parameter or an `owned` class-typed return, on the auto-property
    // declaration form (redundant — the auto-getter is implicitly pure), and
    // on the `set` accessor. The pure-body rule set (closed-body, throw
    // carve-out, pure constructors) is enforced in a later slice.
    void checkPureModifiers();
    void checkTypePureModifiers(TypeSymbol* t);

    // §22.15 (v0.202, #2052) "Checking and dispatch" rules 1+2+3:
    //   1. Purity is never dropped — an `override` of a pure member, or a
    //      class member implementing a pure interface member, must be
    //      declared `pure`.
    //   2. Purity may be strengthened — an override MAY declare `pure`
    //      where the base does not (permissive; no diagnostic).
    //   3. Monotone down the chain — an override matches against its
    //      nearest overridden declaration.
    // Rule 1 is the load-bearing diagnostic; rules 2 and 3 fall out of
    // checking each override against its nearest base.
    void checkPureDispatch();
    void checkTypePureDispatch(TypeSymbol* t);

    // §23.2 (v0.214, issue #2088, proposal 0058): `readonly` on a property /
    // indexer / expression-bodied member is REDUNDANT on body-bearing forms
    // (expression-bodied properties, expression-bodied indexers, get-only block
    // forms, block forms declaring a setter, and the expression-bodied
    // properties of choices/enums/extensions — all parse to PropertyDecl with
    // bodyKind ∈ {Expression, Block, isIndexer}). It is admitted only where
    // the SHAPE cannot express read-onlyness: auto-properties (§23.1) and
    // interface properties (§11.1 — whose abstract accessor set is declared,
    // not implemented), where the bodyKind is Auto. The grammar's `readonly?`
    // slot on the body-bearing productions is an over-admission (shared-
    // production policy); this pass performs the semantic rejection spelled
    // out in §23.2.
    void checkReadonlyPropertyForms();
    void checkTypeReadonlyPropertyForms(TypeSymbol* t);
    void checkTypeReadonlyPropertyFormsMember(PropertySymbol* ps);

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

    // #2208 (§35.1, v0.226): a choice may not transitively contain itself
    // through value-typed components (case payloads, including payload records
    // and nested/nullable/inline forms). The walker is recordValueContainsSelf;
    // this function fires the diagnostic on the choice when the walker reports
    // back to the choice itself (the record-side diagnostic fires separately
    // from checkRecordRestrictions when the cycle is reachable from a record).
    void checkChoiceSelfContainment();

    // §9.8 (issue #550): records and enums that declare `: ISerializable` or
    // `: IDeserializable` must satisfy the synthesis-eligibility rule.  Types
    // that declare the conformance but are ineligible (ineligible component
    // type, no hand-written Serialize/Deserialize) are a compile error at the
    // conformance declaration.  (Classes are NOT checked here — see the
    // PUBLIC checkClassSerializationEligibilityLate() above — but the
    // PRE-PASS at the top of this function's definition still synthesizes
    // each class's `serializedClass` tree/graph shape from field types,
    // because ExprTypecheck's `.Serialize()`/`.Deserialize()` call
    // resolution depends on that shape already being set.)
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

    // Slice S17 (§11.7): `abstract` and `static` are mutually exclusive;
    // `sealed` and `static` are mutually exclusive. v0.242 (#3052): the
    // `sealed abstract` PAIR is rejected too — the closed abstract class is
    // spelled `closed`, which combines with none of `abstract`/`sealed`/
    // `static` and appears only on a class.
    void checkClassModifierCombinations();
    void checkTypeModifierCombinations(TypeSymbol* t);
    // fix #3100 (§19.6 / §11.7, v0.254): a `closed` class's cross-module
    // exhaustiveness guarantee (§19.6 last sentence) depends on the COMPLETE
    // leaf set being visible at the importer, but `internal` classes are
    // NOT exported (ModuleMetadataBuilder.cpp:2112, "§13.2 internal types
    // not exported"). A `closed` root with a non-public concrete subclass
    // therefore has an importer-invisible leaf, and the importer's
    // `caseArmsExhaustive` walk proves exhaustiveness against an
    // incomplete leaf set — silent wrong behavior at runtime (a `case` the
    // static proof accepts but whose runtime can dispatch to a leaf no arm
    // names, raising MatchFailureException). Reject at the declaration
    // site so the dependent module does not produce an artifact whose
    // cross-module guarantee is unsound, and so the importer sees a
    // closed root only over a leaf set it can read.
    void checkClosedHierarchyLeafVisibility();

    // Slice S16 (§11.7): a class declared `static` may contain only
    // static members — no instance fields, methods, properties,
    // indexers, instance constructors, destructors, or owned fields.
    // A static constructor, static members, class-level consts (which
    // are implicitly static), and nested types are permitted.
    void checkStaticClassMembers();
    void checkStaticIndexers();  // §23.5: static indexers are not supported
    // fix #2758 (§23.5, v0.241): "Indexer parameters may not declare default
    // values" — the declaration-side premise of the positional-only index-
    // argument rule (A.4 IndexerDecl records the ParamList over-admission).
    void checkIndexerParamDefaults();

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
    // must be hashable (§3.6 rule 2, at the instantiation site).
    void checkHashContainerKeys();
    // stdlib §4.2 / spec §14.4 (#1327/#1463/#2713): the single authoritative
    // predicate for "may this type be an owning collection's element?".  Returns
    // an empty string when `el` is admissible (a class or interface type, or an
    // unresolved generic parameter — see below), otherwise a noun phrase naming
    // the structural reason, for the §3.6-rule-2 diagnostic.
    //
    // An unresolved generic parameter (Kind::Named, declaration == nullptr, a
    // name that is not a value builtin) returns "" — admissible — because no
    // concrete type is known at the declaration site.  That is exactly why the
    // per-instantiation sweep below exists: it re-runs this same predicate on
    // the concrete binding.  Both callers share this one function so the
    // declaration-site and instantiation-site answers cannot diverge (the #1463
    // name-list-skew lesson).
    static std::string owningElementBadDesc(const TypeRef* el);
    // True iff `bound` is a BARE generic-parameter binding — a single-part
    // Named ref with no declaration that names neither a primitive nor a
    // builtin. Hoisted from checkDeferredNestedConstraints' local lambda
    // (#2111) so that pass and the #2713 owning-element closure share ONE
    // definition of "this binding is itself a type parameter"; two copies of
    // this predicate is precisely the skew #1463 was about.
    static bool isBareGenericParamBinding(const TypeRef* bound,
                                          const ast::Type* boundAst);
    // stdlib §4.2 / spec §14.4 / §3.6 rule 2 (#2713): Pass 2 for the owning-
    // collection element check.  The declaration-site check in resolveType()
    // admits a bare generic parameter; this walks the recorded instantiations of
    // the enclosing generic type and re-checks the concrete binding, reporting
    // at the instantiation site per §3.6 rule 2.
    void checkOwningElementInstantiations();

    // fix #2911 + fix #2962 (§17.4 / §3.6 rule 2, v0.241): Pass 2 for the
    // degenerate-`as`-target rule at a TYPE-level generic parameter. The
    // declaration-site check in ExprTypecheck admits `o as T` inside the
    // generic body (T is a placeholder there); this walks the recorded
    // instantiations of the enclosing generic and re-checks the concrete
    // binding, reporting at the instantiation site. Modelled on
    // checkOwningElementInstantiations above and asking the SAME shared
    // predicate the direct check asks (degenerateAsTargetReason).
    void checkAsTargetInstantiations();

    // §22.12 rule 3/4 (v0.262, #3272): classify a concrete substitution at
    // the instantiation site for a `static readonly` bare-parameter
    // declaration, emitting the diagnostic per §3.6 rule 2's pattern. The
    // dispatch is rule 3 (immutable content → silent), rule 4 (class-typed
    // warning, suppressed for the warning-suppression set), and the
    // residual paragraph's per-instantiation cases (interface / function →
    // warning explicitly; owning-choice → rejection; owning-inline-array
    // → warning). Method rather than free function because it needs
    // Resolver's diagnostic stream (the ctx_ member is private).
    void classifyStaticReadonlySubstitution(
        const TypeRef* rt, std::string_view paramName,
        std::string_view memberName, bool isField,
        SourceLocation declLoc, std::string_view enclosingTypeName);

    // §22.12 rule 3/4 (v0.262, #3272): pass 2 for the `static readonly`
    // bare-parameter classification. The declaration-site check in
    // checkTypeStaticFieldDomains records each static readonly field/
    // auto-property whose declared type is a bare parameter of the
    // enclosing TYPE; this walks the recorded instantiations and applies
    // rule 3 (immutable content → clean), rule 4 (class type → warning,
    // suppressed for the suppression set), or the residual paragraph's
    // per-instantiation cases (interface, function, borrow-bearing record,
    // holds-borrows choice, owning choice → rejection, owning-inline-array
    // → default warning). The diagnostic lands at the instantiation site
    // per §3.6 rule 2.
    void checkStaticReadonlyBareTInstantiations();

    // §14.6 (v0.262, #3271): expanding in-SCC instantiation edges are a
    // compile error at the declaration holding the edge. Walks the
    // instantiations_ record (one edge per generic-type instantiation
    // resolved in the program), builds the instantiation-request graph over
    // generic-type nodes, computes strongly-connected components, and
    // rejects every in-SCC edge whose substitution is neither a bare
    // in-scope parameter of the source declaration nor a closed type (the
    // §14.6 per-edge condition). The diagnostic names the cycle and the
    // growing argument and lands at the declaration holding the edge.
    void checkExpandingInstantiationEdges();

    // §14.6 (v0.262, #3271), generic-MEMBER half: the same per-edge condition
    // applied to a generic member's SELF-edge — a call inside `M<T>`'s body
    // spelling `M<Pair<T, T>>(…)`. `instantiations_` models generic TYPE nodes
    // only, so this edge class is invisible to
    // checkExpandingInstantiationEdges above; it is recorded separately in
    // genericMemberEdges_ and verdicted here against the SAME
    // substitutionIsAdmitted predicate, so the two halves cannot drift apart.
    void checkExpandingGenericMemberEdges();

    // §14.6 (v0.262, #3271): record `call` into genericMemberEdges_ when its
    // callee spells the enclosing generic method's own name with explicit type
    // arguments (`M<…>(…)`, `this.M<…>(…)`, `C.M<…>(…)`). No-op outside a
    // generic method body and for every other callee shape.
    void recordGenericMemberSelfEdge(const ast::CallExpr& call);

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

    // fix #3032 (§15.4.1, v0.262): a type declaring a static
    // `FromCollectionLiteral` whose carrier is a recognized form
    // (`ReadOnlySpan<T>` or `owned OwningArray<T>`) must be `public` and must
    // return `owned C<T>` at the enclosing type's own arity; declaring both
    // carriers on the same type is a compile error. These are declaration-
    // site diagnostics — they fire on the method's own declaration so the
    // user is told the right thing (their factory's modifier or return type
    // is wrong, not their target type). Out of scope: the "declared directly
    // on the collection type" conjunct already holds structurally
    // (`TypeSymbol::members` carries only directly-declared members), and
    // the `static` conjunct is already enforced by the recognition predicate
    // `isCollectionLiteralAdmissibleTarget` (ExprTypecheck_Calls.cpp).
    void checkCollectionLiteralFactoryConjuncts();

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

    // #1272 / #2208 (§9 + §35.1, v0.226): true when the value-type `start`
    // transitively contains itself BY VALUE (a self-referential value type has
    // infinite inline size and cannot be laid out). Walks the value-containment
    // graph from `cur` — record instance fields, choice case payloads, and
    // inline-array elements of either — unwrapping nullable (`T?` for a value
    // type is still inline, §9) and inline-array wrappers. A class/interface
    // component is a heap reference and breaks the cycle; a bare generic type
    // parameter is opaque and conservatively ignored. `seen` guards against
    // unbounded recursion through unrelated cycles and terminates the walk on
    // the cycle itself on malformed recursive graphs (record↔choice, mutual
    // records, mutual choices, etc.). When `recordsAllowed` is false, records
    // are treated as cycle-breakers — the choice-side check uses this to fire
    // only on purely choice↔choice cycles, leaving record↔choice cycles to the
    // record-side diagnostic (no duplicate firing on a single malformed graph).
    // A recursive structure must use a class (heap reference) at the cycle's
    // break point.
    bool recordValueContainsSelf(TypeSymbol* start, TypeSymbol* cur,
                                 std::unordered_set<TypeSymbol*>& seen,
                                 bool recordsAllowed = true);

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

    // fix #2391 (§14.2/§2.8.2, v0.236): the IMPORTED-base analog of
    // `constraintSetsEquivalent`. Compares an override's locally-declared
    // `where T: ...` clause (the `ast::GenericConstraint* d` form) against an
    // IMPORTED base method's reconstructed clause set (the
    // `ImportedGenericConstraint* b` side-table form, populated by
    // ModuleLoader from MetaMember::genericParams). `b == nullptr` denotes the
    // imported base had no `where` clause for this parameter at all (the empty
    // set). Used by checkOverrideSignatureMatch when the base method is an
    // IMPORTED generic declaration (no `ast::MethodDecl`).
    bool derivedConstraintMatchesImported(
        const ast::GenericConstraint* d,
        const ImportedGenericConstraint* b);

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

    // §28.1 "Type requirement" (v0.193; bare-type-parameter arm v0.196): the
    // `owned` modifier, in an owned-admitting DECLARATION position — a method
    // parameter, a return type, an `out` parameter, a lambda parameter, or a
    // function-type parameter/return slot — requires the declared type
    // (nullability stripped) to be a class type, an interface type, or a bare
    // type parameter. A move-only value type (an owning inline array, §15.3)
    // admits the marker but redundantly (its own move semantics already
    // transfer the obligation), so it is accepted with NO diagnostic. Any
    // other type — a primitive, `string`, a function type, an ordinary record,
    // an enum, a non-owning inline array, or the unit type — carries no
    // disposal obligation to transfer (§28.9, the same reasoning as §28.5's
    // field restriction) and is a compile error at `loc`. `positionDesc` names
    // the position for the diagnostic ("parameter", "return type", …). A bare
    // type parameter is ADMITTED here UNLESS it is constrained `where T: value`
    // (§28.1 v0.196, #1817): such a parameter can never be a class or interface,
    // so the marker could never be more than redundant and is rejected at the
    // declaration — the one decidable point (the constraint set is read from
    // currentMethodConstraints_ and currentOuterType_). The per-instantiation
    // erasure of the marker at value/runtime-managed substitutions of an
    // OTHERWISE-unconstrained bare parameter (v0.196) is the rest of #1817 and
    // is NOT enforced by this declaration-site check.
    // A no-op for admissible / move-only / unresolved (Error) types.
    void checkOwnedDeclType(const TypeRef* t, SourceLocation loc,
                            std::string_view positionDesc);

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

    // §6.5 / §17.14 (v0.242, #3045): the fault probe paired with
    // `foldIntConstExpr` — same identifier resolution, so both walks see the
    // same const values. Called when the fold returns nullopt, to tell "a
    // constant shift count outside the §6.5 domain" (an evaluation fault,
    // a compile error at the constant expression) apart from "not a foldable
    // constant expression".
    ConstShiftDomainFault findConstShiftDomainFaultInScope(const ast::Expr& e,
                                                           Scope* scope);

    void resolveNamed(const ast::Type& t, Scope* scope, TypeRef& ref);

    // §14.2 / Appendix C.4: at a generic instantiation site
    // (`Named<...>` type, including `new C<...>()`), verify every supplied
    // type argument satisfies the constraints its corresponding type
    // parameter declares in the type's `where` clause. `t` carries the
    // instantiation AST node (its span is the diagnostic location; its
    // `genericArgs` are the argument AST nodes — used to skip arguments
    // that are themselves generic params, those can't be classified
    // statically); `argRefs` are the resolved argument TypeRefs
    // (positionally aligned with `tsym->genericParams`); `scope` is used to
    // detect whether an argument name is an in-scope generic parameter.
    void checkGenericConstraints(const ast::Type& t, TypeSymbol* tsym,
                                 const std::vector<TypeRef*>& argRefs,
                                 Scope* scope);

    // §14.2 / fix #3160: the same check, but for an instantiation site
    // that does NOT have an `ast::Type` node — the receiver of a
    // `Type<...>.staticMember` access, where the IdentifierExpr carries
    // its `typeArgs` as `ast::Type*` children but no wrapping `ast::Type`
    // is constructed. Pre-#3160 this path built the instantiated TypeRef
    // inline in ExprTC and never called checkGenericConstraints, so a
    // `where T: sendable` (or any §14.2 constraint) was silently skipped.
    // The full rationale and the deferred-nested edge case the use-site
    // path cannot reach (the Resolver's `deferredNested_` /
    // `deferredTypeBoundFailures_` lists are local to the resolveTypes-
    // time instance) live on the public declaration above.

    // §14.2: check a single constraint `item` (one of class / value /
    // notnull / new() / a type-bound) against type argument `arg` (resolved)
    // for the type parameter named `paramName`. `useSite` supplies the
    // diagnostic span (the wrapping-form caller passes the instantiation
    // `ast::Type`'s `span.begin`; the use-site overload from fix #3160
    // passes the IdentifierExpr's span). `argAst` is the argument's AST
    // node (for its spelling); `boundScope` resolves a LOCAL type-bound
    // constraint's bound on demand (an imported bound arrives already
    // resolved — see `sema::GenericParamConstraint`, fix #3146).
    void checkOneConstraint(SourceLocation useSite, std::string_view paramName,
                            const GenericParamConstraint& item,
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

    // §28.1 (v0.196, #1817): the generic constraints of the method/operator
    // whose signature is currently being resolved (set at the top of
    // resolveMember's MethodDecl/OperatorDecl branches, save/restored like
    // currentOuterType_). Consulted — in UNION with the enclosing type's
    // constraints (currentOuterType_->decl->genericConstraints) — by
    // checkOwnedDeclType so that an `owned` marker on a bare type parameter
    // constrained `where T: value` is rejected at the declaration (that
    // parameter can never be a class or interface, so the marker could never
    // be more than redundant). An operator carries no own constraints, so the
    // OperatorDecl branch sets this to null and the enclosing type supplies
    // the `where T: value` bound. Null outside a method/operator signature.
    const std::vector<ast::GenericConstraint>* currentMethodConstraints_ =
        nullptr;
    // #2713: the generic parameters the CURRENT method signature/body binds,
    // saved/restored exactly like currentMethodConstraints_ beside it.
    //
    // A method's type parameters and the enclosing type's are ONE name scope,
    // and the compiler already relies on that: the `where`-clause checker in
    // TypeResolution_Checks.cpp concatenates the two lists ("both are in
    // scope"). Nothing forbids a method parameter SHADOWING an enclosing one —
    // Appendix A.4's `GenericParam ::= Variance? Identifier` carries only the
    // variance restriction, no §13/§14/§28.1 rule imposes name uniqueness, and
    // sema/parser emit no shadowing diagnostic (not even for `M<T, T>`). So a
    // bare parameter NAME plus currentOuterType_ does not identify a binding.
    // The owning-element deferred obligation consults this to record only names
    // the enclosing TYPE binds; a name the method binds is a different
    // obligation whose bindings are not in instantiations_ at all (#2736(a)).
    // Null outside a method/operator signature; an operator declares no generic
    // parameters, so the OperatorDecl branch nulls it as well.
    const std::vector<ast::GenericParam>* currentMethodGenericParams_ = nullptr;

    // §14.6 (v0.262, #3271): the METHOD declaration whose signature and body
    // are being resolved, so a self-naming generic call inside that body can be
    // attributed to the generic MEMBER's own node (§14.6: "each generic type is
    // a node, and each generic member is its own node"). Saved/restored on
    // exactly the lifetime of currentMethodGenericParams_ beside it, and nulled
    // by the operator and constructor branches for the same reason those null
    // the parameter list — a leaked pointer would attribute a later member's
    // call to the wrong node.
    const ast::MethodDecl* currentMethodDecl_ = nullptr;

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
        // #2713: the generic parameters the METHOD enclosing this site binds
        // (null outside a method signature/body). A chained obligation that
        // re-keys through this site must not attribute an argument name to
        // `enclosingType` when the METHOD binds that name — `class Outer<U>`
        // with `method M<U>()` containing `Inner<U>` would otherwise validate
        // the method's `U` against Outer's, the same misattribution the
        // recorder guards against at TypeResolution_Names.cpp.
        const std::vector<ast::GenericParam>* methodGenericParams = nullptr;
    };
    std::vector<GenericInstantiation> instantiations_;

    // §14.6 (v0.262, #3271): one SELF-NAMING generic-member instantiation
    // request — a call inside generic member `M`'s own body that spells `M`
    // again with explicit type arguments (`M<...>(…)`, `this.M<...>(…)`,
    // `C.M<...>(…)`). This is the generic-MEMBER half of the §14.6
    // instantiation-request graph, which `instantiations_` cannot carry:
    // that record's node is always a generic TYPE, so a member's self-edge
    // has no representation there at all.
    //
    // Only the DIRECT self-loop is recorded, and only when the enclosing type
    // declares exactly one member of that name (checked at verdict time). Both
    // restrictions exist to make target attribution exact without a resolved
    // call graph: a self-loop is unconditionally inside its own SCC, so no SCC
    // walk is needed, and a unique name means the spelled callee can only be
    // the enclosing member. Mutual generic-member cycles and inference-supplied
    // type arguments are the recorded residual (see checkExpandingGenericMemberEdges).
    struct GenericMemberEdge {
        TypeSymbol* enclosingType = nullptr;         // declaring type of source
        const ast::MethodDecl* source = nullptr;     // the generic member
        std::vector<TypeRef*> argRefs;               // spelled type arguments
        std::vector<const ast::Type*> argAsts;       // their AST nodes
        SourceLocation loc;                          // the call site
    };
    std::vector<GenericMemberEdge> genericMemberEdges_;

    // §3.6 / §8.3 (v0.158, #1338): one hash-container instantiation site whose
    // key/element must be hashable. Recorded while resolving a
    // `Dictionary`/`OwningDictionary`/`Set`/`OwningSet` `Named<...>` annotation;
    // validated by checkHashContainerKeys() after every member type is resolved
    // (a record key's auto-hashable property depends on the record's own
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

    // stdlib §4.2 / spec §14.4 / §3.6 rule 2 (#2713): one owning-collection
    // instantiation whose owned element slot is a bare generic parameter of the
    // enclosing generic type, e.g. `OwningLinkedList<T>` inside `class H<T>`.
    // The declaration-site check cannot decide such a site (no concrete type is
    // known), so it is recorded here and re-checked per instantiation of the
    // enclosing type by checkOwningElementInstantiations().  Modelled on
    // hashContainerSites_ above — the existing precedent for this exact shape.
    struct OwningElementSite {
        std::string_view container;       // "OwningLinkedList" / "OwningArray" / …
        std::string_view paramName;       // the bare type parameter, e.g. "T"
        const ast::Type* site = nullptr;  // the `OwningLinkedList<T>` node
        TypeSymbol* enclosingType = nullptr;  // the generic body it sits in
    };
    std::vector<OwningElementSite> owningElementSites_;

    // fix #2911 + fix #2962 (§17.4 / §3.6 rule 2, v0.241): one `x as T` in a
    // generic body whose target is a bare type parameter OF THE ENCLOSING TYPE.
    // §17.4: "When the target is a generic type parameter, the check applies
    // per instantiation (§3.6 rule 2)". A METHOD's own parameter is excluded
    // here (its bindings are calls, which ExprTypecheck sees and this pass does
    // not) and is handled by ExprTC::checkDeferredAsTargetInstantiations —
    // the same split, and the same shadowing guard, as the #2713 owning-element
    // obligation beside it.
    struct AsTargetSite {
        std::string_view paramName;           // the bare parameter, e.g. "T"
        TypeSymbol* enclosingType = nullptr;  // the generic body it sits in
        SourceLocation loc;                   // the `x as T` expression
    };
    std::vector<AsTargetSite> asTargetSites_;

    // Record an `x as T` site when `t` is a bare parameter of the enclosing
    // TYPE; a no-op otherwise. Called from resolveTypesInExpr's TypeTestExpr
    // arm, where `t` has just been resolved.
    void recordAsTargetSite(const ast::Type& t, SourceLocation loc);

    // §22.12 rule 3/4 (v0.262, #3272): a `static readonly` field or
    // auto-property declared at a bare generic parameter of the enclosing
    // type (`T`, `T?`) is admitted at the declaration (no classification is
    // decidable there) and classified per instantiation, after substitution.
    // Recorded during checkTypeStaticFieldDomains(), validated by
    // checkStaticReadonlyBareTInstantiations() at the end of `run()`, when
    // every instantiation has been recorded.
    //
    // Modelled on AsTargetSite (#2911) and OwningElementSite (#2713) — the
    // existing per-instantiation precedents beside it. `paramName` keys the
    // substitution the per-instantiation pass walks; `enclosingType` is the
    // body whose instantiations_ it walks; `memberKind` discriminates field
    // from auto-property only for the diagnostic message — the rule is the
    // same.
    struct StaticReadonlyBareTSite {
        std::string_view paramName;           // the bare parameter, e.g. "T"
        std::string_view memberName;          // the field/property name
        bool isField;                         // field vs auto-property
        SourceLocation declLoc;               // the field/property declaration
        TypeSymbol* enclosingType = nullptr;  // the generic body it sits in
    };
    std::vector<StaticReadonlyBareTSite> staticReadonlyBareTSites_;

    // Record a `static readonly` field/auto-property whose declared type is
    // a bare parameter of the enclosing TYPE. The per-instantiation pass
    // consumed this list at the end of `run()`.
    void recordStaticReadonlyBareTSite(std::string_view paramName,
                                       std::string_view memberName,
                                       bool isField,
                                       SourceLocation declLoc,
                                       TypeSymbol* enclosingType);

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
