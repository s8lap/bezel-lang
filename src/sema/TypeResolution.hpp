#pragma once

// Sema slice 2 — Type Resolution.
//
// Walks every `ast::Type` node in the SourceFile (base-type lists, member
// signatures, generic constraint args, local-decl type ascriptions, etc.)
// and resolves it to a `TypeRef`. The resolved TypeRefs are stored in
// `SemaContext::resolvedType(...)` keyed by the AST node pointer.
//
// Lookup discipline (single-compile-unit; cross-module resolution lands
// when the driver supports multi-file builds):
//
//   1. Primitive-type keywords (int, bool, …) resolve to TypeRef::Primitive.
//   2. The unit shape `()` resolves to TypeRef::Unit.
//   3. Inline arrays `[N]T` and `[N owned]T` resolve to TypeRef::InlineArray.
//   4. Function types `(...) -> T` resolve to TypeRef::Function.
//   5. NamedTypes: their first identifier is looked up in (in order)
//      enclosing-method generic params, enclosing-type generic params,
//      enclosing module's top-level types + aliases, the special
//      auto-imported names (Object, etc.). A nested dotted name selects
//      a child of the resolved root.
//
// Diagnostics:
//   - Unresolved type name.
//   - Generic-arity mismatch (Foo<T, U> referenced with one arg).
//
// What's still deferred:
//   - Multi-file / multi-module name lookup. Single compile unit only.
//   - Constraint-clause resolution (sema slice with constraint handling).
//   - Variance compatibility checks (typecheck-time).

#include <array>
#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/TypeRef.hpp"

// Forward-declared rather than `#include "common/OwnershipFacts.hpp"`: used
// only by-pointer below (`typeHasNoDefault`'s `incoming` parameter).
namespace bezel::common {
struct RecordSubstEnv;
}  // namespace bezel::common

namespace bezel::sema {

bool resolveTypes(const ast::SourceFile& src, SemaContext& ctx);

// §9.8 / #2325: the CLASS half of serialization synthesis-eligibility
// checking (component eligibility + constructor selection). Split out from
// resolveTypes()'s checkSerializationEligibility because it needs
// FieldSymbol::posParamBodyRef/posParamInitRef (§11.1 primary-constructor
// parameter capture classification), which typecheckExpressions computes —
// so it cannot run as part of resolveTypes(). typecheckExpressions() itself
// calls this at the end of its own body (ExprTypecheck.cpp) so every one of
// its many callers (the driver, module loading, the test harness) gets it
// for free with no separate wiring; it is exposed here as a free function
// only so ExprTypecheck.cpp can call it without depending on the Resolver
// class directly. See TypeResolution_impl.hpp's declaration comment on
// Resolver::checkClassSerializationEligibilityLate for the full rationale.
bool checkClassSerializationEligibilityLate(const ast::SourceFile& src,
                                            SemaContext& ctx);

// §30.14 / §14.2 / #3235: replay the §14.2 `where T: sendable` sites
// `Resolver::checkOneConstraint` deferred during `resolveTypes()` —
// those whose verdict depends on `FieldSymbol::posParamBodyRef`, the
// §22.10 captured-vs-uncaptured bit computed by ExprTypecheck's
// expression walk. The bit is set by the time `typecheckExpressions()`
// returns, so the §30.14 rule-2 storage walk in `isSendableType` sees a
// class's captured primary-ctor parameters of non-sendable type and
// reports them. Same rationale as `checkClassSerializationEligibilityLate`
// (#2325): living inside `typecheckExpressions()` itself means every
// existing and future caller gets it for free with no separate wiring —
// a separately-invoked pass would need every test-harness pipeline
// (dozens build their own SemaContext + resolveTypes +
// typecheckExpressions independent of Driver::compileFile) to also
// remember to call it. Exposed as a free function so ExprTypecheck.cpp
// can invoke it without depending on Resolver directly. See
// Resolver::checkSendableConstraintsLate's own comment for the
// per-entry replay detail.
bool checkSendableConstraintsLate(const ast::SourceFile& src,
                                  SemaContext& ctx);

// §14.2 / fix #3160: §14.2 constraint check at an instantiation site that
// does NOT have an `ast::Type` node — the receiver of a
// `Type<...>.staticMember` access (`Ferry<Bad>.Count()`). The IdentifierExpr
// in that position is the one syntactic path that mints an instantiated
// TypeRef without going through `Resolver::resolveType` (which is where the
// wrapping-form `Resolver::checkGenericConstraints` runs). Pre-#3160 the
// `where T: sendable` (and every §14.2 constraint form) was silently skipped
// at this site. Exposed as a free function so ExprTypecheck.cpp's
// IdentifierExpr handler can invoke it without depending on Resolver
// directly. `argAsts` and `argRefs` are positionally aligned with
// `tsym->genericParams`. Same pattern as `checkClassSerializationEligibilityLate`
// and `checkSendableConstraintsLate`: a fresh Resolver wrapping `ctx` runs
// the check; the deferred-nested / deferred-type-bound paths are local to
// each Resolver instance and already flushed by the time this is called,
// so the common case (concrete typeArgs) is handled and the deferred-edge
// case stays no-worse than pre-#3160.
bool checkGenericConstraintsAtExpr(SemaContext& ctx,
                                   SourceLocation useSite,
                                   const std::vector<const ast::Type*>& argAsts,
                                   const std::vector<TypeRef*>& argRefs,
                                   TypeSymbol* tsym);

// §22.12 rule 4 residual + §9.7 static-field bullet (v0.265, #3273 /
// #3290): the §9.7 null-source value-site pass for `static readonly`
// fields and auto-properties whose declared type is one of §9.7's three
// deferred kinds (borrow-bearing record, non-owning choice that holds
// borrows, plain inline array carrying borrows). Walks every such
// initializer and emits a §9.7 diagnostic when a non-null reference-typed
// component is statically observable. Called from `typecheckExpressions()`
// AFTER the expression walk so positional-argument resolution has named
// every argument; same pattern as `checkSendableConstraintsLate`.
bool checkStaticReadonlyNullSource(SemaContext& ctx);

// Structural equality of two resolved TypeRefs (§19/§22.6 "exact match").
// Returns false (conservatively) when either is null or an Error kind.
// Exposes the resolver's existing structural-equality utility for reuse by
// other passes (e.g. §22.6 overload resolution).
bool typeRefEqual(const TypeRef* a, const TypeRef* b);

// §5.1 "Implicit conversions" numeric/char widening table (fix #4853, v0.307):
// true iff `from` implicitly widens to `to` (distinct primitives; identity is
// the caller's). Exposes sema's one copy of the table (`ExprTC::
// primitiveWidens`) so codegen's `ctorForArgs` can prefer a §5.1-admitted
// conversion over a narrowing without a second, drifting copy. Covers the
// TYPE-based rows only — constant narrowing is value-based and not modelled.
bool primitiveImplicitlyWidens(PrimitiveType from, PrimitiveType to);

// §14.2 (fix #4863, v0.307): does the candidate `arg` satisfy the type-bound
// `bound` as an INTERFACE INSTANTIATION — not merely as a declaration match.
// `arg` and `bound` are full TypeRefs with genericArgs preserved; the
// function walks `arg`'s transitive baseTypeRefs (`baseTypeRefs` is the
// parallel-instantiation vector to `baseTypes`, populated by
// SymbolCollection/ModuleLoader and used by override-signature checking) and
// accepts only when a base's TypeRef equals the bound under `typeRefEqual`.
// Non-generic bounds (e.g. `where T: IDisposable`) admit any instantiation
// of the bound's declaration — the no-generic-args specialization of the
// §14.2 constraint table. Bare-parameter markers in the bound's args
// (Named with declaration=nullptr, the §14.2 type-parameter marker) name
// the GENERIC OWNER's type parameter; the single-entry substitution
// {constraint's param → candidate} applies (see the body comment for the
// multi-param gap). Exposed as a free function so ExprTypecheck_Calls.cpp's
// call-site constraint check (the OTHER half of #4863 — the wrapping-form
// `Resolver::checkGenericConstraints` only fires at instantiation sites,
// not at method-call sites like `H.F<C>(c, …)`) can call it without
// depending on Resolver directly.
bool argSatisfiesTypeBound(const TypeRef* arg, const TypeRef* bound);

// #2787 / spec §14.2 rule 3 + §12.9 / stdlib §2.2 §4.4 §5.1: does the INTERFACE
// type `t` satisfy the self-recurrent bound `where T: IComparable<T>` — i.e.
// does it name `IComparable<t>` (ITSELF) in its extension clause?
//
// This is THE ONE definition of that predicate. It previously existed in three
// independent copies — codegen's selfComparableIfaceCompareTo (#2768, reused by
// #2770 and #2782), an inline scan in sema's Sort/Sorted gate (#2768), and
// sema's selfComparableIfaceArg (#2782) — which is the shape that produced the
// `outerDepth` sentinel defect an earlier run had to fix twice and then sweep.
// A pending divergence made the risk concrete: #2776 records that this test does
// not reach a CROSS-MODULE interface (an imported interface's `baseTypeRefs` does
// not carry its `IComparable` base), and that fix must apply to every caller.
//
// It is deliberately the EXACT constraint, not a weaker proxy:
//   * `IComparable` is matched by IDENTITY against the builtin symbol, never by
//     name — #1867's recorded convention ("Blanket-accepting by decl/name was
//     too lenient — it admitted a WRONG generic argument"), which also stops a
//     user-declared interface that happens to be called `IComparable` from
//     masquerading as the stdlib §2.8 one.
//   * the argument is compared with `typeRefEqual`, §14.2's exact,
//     component-wise, INVARIANT identity — so `interface IG<T>:
//     IComparable<IG<int>>` does NOT satisfy it for `IG<string>` (§12.9,
//     IComparable is invariant), where a bare `arg->declaration == d` would
//     have admitted it.
//   * nullable is excluded: §14.2 states "A type-bound constraint — naming a
//     class or interface — implies non-null T".
// `icomparable` is the caller's `ctx_.builtinInterfaceSym("IComparable")`;
// passing null yields false.
bool selfComparableIface(const TypeRef* t, const TypeSymbol* icomparable);

// Does a derived (override) parameter type MATCH a base parameter type —
// exactly, or via the one nullability widening §12.1 admits (base `T`,
// derived `T?`, v0.181, #1560)? The reverse (base `T?`, derived `T`) is NOT
// a match. Exposes the resolver's override-match predicate for reuse by
// other passes (e.g. §22.6's collectHierarchyOverloads, which must
// recognize a widened override as shadowing — not overloading — its base).
// `isByRefParam` (#1592): pass true for a `ref`/`out` parameter — a
// VALUE-typed widening is declined in that position even though it is now
// admitted for a plain by-value parameter (see TypeResolution_Checks.cpp).
bool overrideParamTypeMatches(const TypeRef* derivedTy, const TypeRef* baseTy,
                              bool isByRefParam = false);

// §19.3 (#1847): is `n` the simple name of a built-in (stdlib) type? Exposes
// the resolver's authoritative built-in-type-name recognizer (a thin predicate
// over `builtinTypeArity`) so the top-level case-pattern validator in ExprTC
// can accept a built-in type-test pattern (`case e of IOException => …`)
// without duplicating the built-in name list. Single source of truth:
// delegates to Resolver::isBuiltinTypeName.
bool isBuiltinTypeName(std::string_view n);

// fix #4309 (§14.2/§17.11, v0.298; send-back on #4294): the closed set of
// builtin, null-declaration INTERFACE names that `Resolver::argSatisfiesNew`
// and `typeHasNoDefaultWalk` (TypeResolution_Checks.cpp) both reject
// unconditionally as non-constructible (§14.2) / non-defaultable (§17.11) —
// every `I*` name in `Resolver::builtinTypeArity`'s table EXCEPT the seven
// `SemaContext::isSynthesizedBuiltinInterfaceName` names (which already get
// a real, TypeSymbol-backed Interface arm) and `IGrouping` (fix #4294's own
// deliberately separate two-line branch, kept apart — see that fix's commit
// and TypeResolution_Checks.cpp's own comment beside this array's
// definition). Exposed as a `const&` — not merely a membership predicate —
// so `tests/sema/*4309*` can assert the SAME array both call sites consult
// against the two authoritative tables it was derived from, rather than
// re-typing a third copy of the name list in test code.
const std::array<std::string_view, 25>& nonConstructibleBuiltinInterfaceNames();

// §2.8.3 rule 4/§12.7/§19.3 (#1946, #2069): the STRICT runtime-reification oracle. A
// type argument is runtime-reified — the per-monomorphization descriptor (#1843)
// distinguishes it from a sibling — iff it is a concrete Named type (resolved
// declaration, or a null-decl builtin generic with reified args), a primitive,
// or a FOLDED-size inline array whose element is itself reified, RECURSIVELY
// through generic/element arguments. A function type, an unfolded inline array,
// Unit/Error, or a bare parameter placeholder collapses in the descriptor key
// (appendDescKey: `Kind::Function` → "F" at any depth) → NOT distinguishable.
// Hoisted from ExprTC (which keeps a forwarding alias) so codegen's #2069
// per-monomorphization collapse guard (rejectCollapsedTypeTestTarget) shares the
// SAME predicate — critically the RECURSION into declared-Named arguments that
// codegen's descTypeArgConcrete lacks (the nested `is Wrap<Inner<T>>` collapse,
// #2184). Single source of truth: ExprTC::typeArgRuntimeReified delegates here.
bool typeArgRuntimeReified(const TypeRef* t);

// §17.4 "Degenerate value-type targets" (v0.241; #2911 + #2962) — THE predicate
// for the two arms of ONE spec paragraph, which opens with the shared principle
// they both instantiate: "`as` requires its result type `T?` to exist and to be
// informative."
//
//   * an INLINE-ARRAY target (any form — `[N]T`, `[N]T?`, and the owning forms)
//     has no `T?` to be: §15.1 "admits no nullable inline-array form", so the
//     operator's contract is unsatisfiable;
//   * the UNIT target has nothing to say: `()` has exactly one value, so the
//     result could never carry more information than `x is ()`.
//
// Returns the reason-and-remedy body of the diagnostic (each arm names its OWN
// remedy — `is` + copy-out vs. `x is ()`), or an empty string when the target is
// an admissible `as` target. One definition, because the spec is one paragraph:
// the DIRECT check at the `as` expression (ExprTypecheck_Exprs.cpp) and the two
// §3.6-rule-2 per-instantiation joins (Resolver::checkAsTargetInstantiations for
// a TYPE parameter, ExprTC's deferred join for a METHOD parameter) all call it,
// so the answer and the wording cannot drift between them — the #1463
// name-list-skew lesson, generalized exactly as `owningElementBadDesc` applies
// it for §14.4.
std::string degenerateAsTargetReason(const TypeRef* target);

// §27.8 (v0.294; fix #4038/#4048/#4050/#4051/review #4062): resolve a
// `TypeAliasSymbol` through to the TypeSymbol its `aliasedType` names (a
// class/record/interface/enum). THE shared resolution this whole family of
// "does this WRITTEN NAME denote a type" classification predicates needs —
// and, per the review chain across #4038 (sema `resolveMemberAccess`), #4048
// (sema `altPartsNameType`), and #4050 (BOTH sema's destructuring-pattern
// head resolution AND codegen's independent, parallel `patternMatches`
// DestructuringPattern resolution — the two layers do NOT share sema's
// result, so codegen needs the same fix separately), a free function here —
// reachable from both `sema::ExprTC` (via a thin forwarding member,
// `ExprTC::resolveAliasToTypeSymbol`) and `codegen::Emitter` (directly; both
// classes hold a `SemaContext&` named `ctx_`) — is the ONE seam that closes
// the whole family rather than each site (sema AND codegen alike)
// hand-rolling its own copy. Mirrors the established pattern in this header:
// `typeArgRuntimeReified`'s own comment ("Hoisted from ExprTC … so codegen's
// … shares the SAME predicate").
//
// `aliasSym->decl`'s `aliasedType` was already eagerly resolved by
// `Resolver::resolveTopDecl` when the `type` declaration itself was
// processed (TypeResolution.cpp) — the same fact fix #578's type-position
// alias transparency (TypeResolution_Names.cpp) also reuses.
//
// Returns null when: the alias is declaration-less (should not happen for a
// module-internal alias, §27.8 — defensive only); the aliased type is not
// `TypeRef::Kind::Named` (a primitive, function type, or bare generic-
// parameter target — none of those are the TypeSymbol-based "type" this
// family recognizes; review #4062 tracks the SEPARATE codegen hardening a
// non-Named alias target still needs, a missing never-match guard rather
// than a missing resolution); OR — #4047 — the alias's `aliasedType` has not
// yet been resolved because it is declared TEXTUALLY BEFORE the (possibly
// further-aliased) type it names, and `Resolver::resolveTopDecl`'s single
// forward pass over `src.topDecls` has not reached it yet (in that case
// `underlying` is non-null with `Kind::Named` but `underlying->declaration`
// is itself null, so this function still returns null — never a dangling or
// garbage TypeSymbol*). Callers inherit that same known-open ordering gap;
// #4047 tracks it, not this function.
TypeSymbol* resolveAliasToTypeSymbol(SemaContext& ctx, TypeAliasSymbol* aliasSym);

// fix #4554 (§2.7, v0.306): the chain-head warning row — a chain head
// resolves to a type while a same-named accessible member exists on the
// enclosing type (§2.7 rule 1's "type outranks member" precedence for chain
// heads), AND/OR a local type shadows an imported module-name prefix. The
// declaration-site warning fires only when the member's declared type is
// NOT the resolved type (the `property Color: Color` carve-out); the
// module-shadow warning fires when the head name is in `sourceImports_`.
// Called from ExprTypecheck_Exprs.cpp's IdentifierExpr handler right after
// the chain-head lookup resolves to a type.
void warnChainHeadTypeShadows(
    SemaContext& ctx,
    std::string_view headName,
    const TypeSymbol* resolvedType,
    const std::unordered_set<std::string>& sourceImports,
    const Symbol* shadowedEnclosingMember,
    SourceLocation loc);

// fix #4166 review follow-up (§14.2/§17.11/§35.10, v0.298): THE shared
// authority for "does `default(T)` (equivalently, at a value-type
// substitution, `new T()`) have NO well-defined value?" — the §17.11
// "defaultable" glossary entry, applied recursively. Two independent
// copies of this question existed on two different classes:
// `ExprTC::defaultIllFormed` (ExprTypecheck_Exprs.cpp, the `default`
// operator's own well-formedness check, fix #3982/#3986/#3988/#3995) and
// `Resolver::argSatisfiesNew`'s Record arm (TypeResolution_Checks.cpp, the
// `where T: new()` constraint check, fix #3141/#3988/#4002/#4166) — the
// SAME mirroring-not-sharing constraint `resolveAliasToTypeSymbol` above
// already documents applies here too (`Resolver::run` executes before
// `ExprTC` exists, Driver.cpp), except this time the two copies had
// drifted: `argSatisfiesNew`'s port (#4166) covered only the Record-kind
// recursion, leaving Enum/Choice/InlineArray/`isValueBuiltin`-aware-Class
// divergences from `defaultIllFormed`'s full switch (see #4219, #4223).
// `ExprTC::defaultIllFormed` now forwards here; `argSatisfiesNew` calls
// this directly. Both share ONE cycle guard (PATH semantics, keyed by
// `const TypeRef*` instantiation identity — mirrors
// `common::isBorrowBearingRecordCommon`, OwnershipFacts.cpp, and that
// file's own fix #3933 for why the key is the TypeRef instantiation and
// not the bare TypeSymbol* declaration) — `defaultIllFormed` had none
// before this, and a self-referential record reached through an explicit
// `default(...)` SIGSEGV'd identically to the `where T: new()` shape
// #4166's own review caught.
//
// One divergence resolved in `argSatisfiesNew`'s favor rather than
// `defaultIllFormed`'s: a non-nullable `Object`-typed component IS
// non-defaultable here (§17.11's own definition: "Not defaultable:
// non-nullable reference types"), even though `defaultIllFormed`'s
// pre-unification text deliberately excluded `Object` as a separate,
// out-of-scope #951-era decision — `argSatisfiesNew`'s existing, tested
// behavior already rejected it, and porting the narrower exclusion instead
// would have silently reintroduced a false accept there (measured against
// `xmod_imported_positional_new_args_3141_test.cpp`).
//
// A second such divergence, caught by review after the first landed: a
// null-declaration `Named` type is not only an unresolved generic
// parameter — it is also how every always-available builtin (`Array`,
// `Dictionary`, `Set`, `OwningTask`, …) is represented, and
// `defaultIllFormed`'s prior text unconditionally treated ALL of them as
// defaultable, `argSatisfiesNew`'s regression relative to its own
// pre-hoist `categorize()`-based behavior. Fixed NARROWLY, by builtin
// NAME, for the stdlib collection family
// (`Array`/`Dictionary`/`Set`/`Stack`/`Queue`/`LinkedList`/
// `SortedDictionary`/`SortedSet`, their `Owning*` counterparts, and
// `Task`/`OwningTask`) rather than by mirroring `Resolver::categorize`'s
// `astNode`/`builtinTypeArity` fallback wholesale — that fallback does
// not distinguish these REFERENCE builtins from VALUE ones (`Guid`,
// `IntPtr`, `DateTime`, …), and a first attempt at the wholesale mirror
// broke `default(Guid)` / `default(IntPtr)`
// (tests/lit/builtin_closed_ctor_surface_accept_2994.bzl), which
// `defaultIllFormed`'s ORIGINAL text had always accepted. See
// `typeHasNoDefaultWalk`'s Named-arm comment (TypeResolution_Checks.cpp)
// for the full account. Third occurrence in this run of the same defect
// class — a predicate over a null-declaration `Named` type that silently
// drops the ~50-builtin population (issue #4229 tracks two more;
// `fix #4199` was a fourth) — and a caution for the next one: verify
// against BOTH the `where T: new()` and `default(...)` call paths before
// picking a fallback, not just the one the regression report named.
//
// `incoming` is the enclosing frame's substitution environment (null/empty
// at a top-level, non-nested call — every existing call site passes what
// it already did).
bool typeHasNoDefault(const TypeRef* t, SemaContext& ctx,
                      const common::RecordSubstEnv* incoming = nullptr);

// fix #4472 (§14.2, v0.302; follow-up to fix #4446): does ground CLASS
// declaration `d` fail `where T: new()` because it is `abstract` or
// `closed`? §14.2 v0.302: "Nor does an `abstract` or `closed` class
// [satisfy `new()`], whether it declares a `public` parameterless
// constructor or takes §22.7's implicit one: §12.5 makes `new` on such a
// class a compile error, so admitting the substitution would satisfy a
// constraint whose sole purpose — `new T()` in the generic body — the
// language forbids on the substituted type." (`d` is assumed to already be
// `ast::TypeDeclKind::Class`; the caller's own kind switch establishes
// that — this function only reads `d->modifiers`.)
//
// THREE call sites ask this exact question as part of their own "does this
// class satisfy new()" arm: `ExprTC::groundSatisfiesNew` (ExprTypecheck.cpp,
// fixed by #4446), `Resolver::argSatisfiesNew` (TypeResolution_Checks.cpp),
// and the inline Class arm of `checkMethodGenericConstraints`'s "new" item
// (ExprTypecheck_Calls.cpp) — #4446's own SCOPE NOTE named the latter two
// as separate, un-fixed duplicates and filed the gap as #4472. Rather than
// growing a third independently-maintained copy of the modifier test, all
// three now call this ONE function; the surrounding ctor-enumeration logic
// (primary-constructor parameter check, accessible-parameterless-ctor scan)
// stays separate per call site — each has a differently-shaped result
// protocol (bool return + decided out-param vs. satisfied/decided pair vs.
// early continue), so unifying that too is a larger, separately-scoped
// refactor outside what #4472 asks for.
bool isAbstractOrClosedClass(const TypeSymbol* d);

} // namespace bezel::sema
