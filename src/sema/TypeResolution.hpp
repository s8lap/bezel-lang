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

#include <string_view>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

bool resolveTypes(const ast::SourceFile& src, SemaContext& ctx);

// §9.8 / #2325: the CLASS half of serialization synthesis-eligibility
// checking (component eligibility + constructor selection). Split out from
// resolveTypes()'s checkSerializationEligibility because it needs
// FieldSymbol::posParamBodyRef/posParamInitRef (§11.1 primary-constructor
// parameter capture classification), which typecheckExpressions computes —
// so the driver must call this AFTER typecheckExpressions(), not as part of
// resolveTypes(). See TypeResolution_impl.hpp's declaration comment on
// Resolver::checkClassSerializationEligibilityLate for the full rationale.
bool checkClassSerializationEligibilityLate(const ast::SourceFile& src,
                                            SemaContext& ctx);

// Structural equality of two resolved TypeRefs (§19/§22.6 "exact match").
// Returns false (conservatively) when either is null or an Error kind.
// Exposes the resolver's existing structural-equality utility for reuse by
// other passes (e.g. §22.6 overload resolution).
bool typeRefEqual(const TypeRef* a, const TypeRef* b);

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

// §14.2/§12.9/§19.3 (#1946, #2069): the STRICT runtime-reification oracle. A
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

} // namespace bezel::sema
