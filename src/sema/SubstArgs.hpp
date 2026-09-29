#pragma once

// Shared authority for eager generic substitution over a `TypeRef`.
//
// Three eager recursive walkers over the same TypeRef kind set lived on
// three different classes:
//
//   - `Resolver::applyGenericSubst`              (src/sema/TypeResolution_Checks.cpp, pre-fix #4188)
//   - `Resolver::substituteAliasGenericArgs`     (src/sema/TypeResolution_Names.cpp)
//   - `ExprTC::substituteGenericFieldType`       (src/sema/ExprTypecheck.cpp)
//
// They agree today; nothing kept them agreeing. fix #4212 hoists them into
// this single `sema::substituteGenericArgs` (per §14.1, §27.8, v0.298).
// The out-of-scope cousins `Emitter::applyMethodGenericSubst` and
// `unifyMethodReturnTypeArg` are deliberately NOT folded in (unification is
// a different operation at a different layer; `common::RecordSubstEnv` is a
// lazy environment-chain resolver, not an eager rewriter).
//
// The walker substitutes every bare-name TypeRef whose declaration is null
// (i.e. a generic parameter reference, §14.1) and whose single-part syntactic
// head matches a key in `subst.subst` with the corresponding concrete type.
// Recurses into Named-with-genericArgs, InlineArray element, Function
// parameter/return. Preserves the `?` modifier across substitution
// (`T? → int?` when `T→int`). Returns `tr` unchanged (no allocation) when no
// substitution applies — callers can rely on pointer equality for the
// "nothing changed" signal.

#include "support/SourceLocation.hpp"
#include "sema/SemaContext.hpp"
#include "sema/TypeRef.hpp"

#include <string_view>
#include <unordered_map>

namespace bezel::sema {

// Eager-substitution walker parameters.
//
// `ctx`         — SemaContext that owns `tr`; used for fresh TypeRef
//                 allocations (never mutates the input).
// `subst`       — `name -> TypeRef*` map. Names are the single-part syntactic
//                 head of the generic parameter reference being substituted.
//                 An empty `subst` is allowed and turns the walker into an
//                 identity function (no allocation).
// `useLoc`      — used only when `validateNestedInlineArrays` is true, to
//                 report the §15.1 "an inline array may not be the element
//                 of another inline array" diagnostic at the reference site
//                 that actually built the nested shape (fix #4280).
// `validateNestedInlineArrays` — alias-substitution re-validation (the §15.1
//                 shape only forms at substitution time when the alias body
//                 is itself `[N]T` and the supplied `T` is an inline array).
//                 `false` for Resolver::applyGenericSubst and
//                 ExprTC::substituteGenericFieldType; `true` for
//                 Resolver::substituteAliasGenericArgs.
struct SubstArgs {
    SemaContext& ctx;
    const std::unordered_map<std::string_view, TypeRef*>& subst;
    SourceLocation useLoc{};
    bool validateNestedInlineArrays = false;
};

// The single authority. See SubstArgs above for parameter semantics.
// Per §14.1 (instantiation), §27.8 (each instantiation of the alias is
// interchangeable with the corresponding instantiation of the aliased type),
// v0.298.
TypeRef* substituteGenericArgs(TypeRef* tr, const SubstArgs& args);

}  // namespace bezel::sema