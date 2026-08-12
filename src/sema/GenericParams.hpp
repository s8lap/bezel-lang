#pragma once

// §14.1 "Generic declarations" / §12.9 "Variance" / §2.5 "Dependency
// resolution" (v0.254): a type's declared generic parameters — their NAMES, in
// declaration order, and their VARIANCE markers — read from whichever source
// carries them.
//
// WHY THIS IS NOT JUST `ts->decl->genericParams` (umbrella #3152, family #3143).
// `ModuleLoader` never assigns `sema::Symbol::decl` for an imported type: there
// is no `ast::TypeDecl` to assign, because the importing compilation has no AST
// for a dependency's types at all (`reconstructTypeSymbol`, ModuleLoader.cpp).
// So down-casting a TypeSymbol's `decl` to an `ast::TypeDecl` yields null for
// EVERY import. That is not a null-dereference risk — every site guards — but
// the guard's `false` branch was uniformly written as the substantive claim
// "this type has no generic parameters", which is wrong across a module
// boundary and silent.
// Measured on `main` @ a0b655e, on real two-module builds
// (`bezelc -c lib.bzl -o lib.o`, then `bezelc --import lib.o app.bzl`), against
// byte-identical single-module controls that all compile and run correctly:
//
//   * an imported `interface I<out T>`: `var x: I<Animal> := a;` where
//     `a: I<Dog>` — a §12.9 covariant assignment — is REJECTED with "the
//     initializer for `x` has a type that is not implicitly convertible to the
//     target type (§5.1)". Variance read off the absent AST is `""`, i.e.
//     invariant.
//   * an imported `record R<T>(rv: T, rtag: int)` named as a method PARAMETER
//     or RETURN type in the consumer: `Function return type does not match
//     operand type of return inst!` / `Call parameter type does not match
//     function signature!` — `%record.R = type { i32, i32 }`, the erased
//     placeholder layout, against the specialized `%record.R$spec0` value.
//   * an imported generic BASE with a `T`-typed constructor parameter
//     (`class Child : GBase<Cell>`): `Call parameter type does not match
//     function signature!` at `bz$GBase$ctor(ptr, i32)`.
//
// `TypeSymbol::genericParams` is populated on BOTH sides — `SymbolCollection`
// for a local declaration, `ModuleLoader::convertGenericParams` for an imported
// one, which sets `GenericParamSymbol::name` and `::variance` from the
// artifact's `MetaGenericParam` — so this family needs no new field and no
// metadata change. It needs one accessor that consults both sources.
//
// PRECEDENCE. The AST is read first when it exists, so a locally-declared type
// answers exactly as it did before this accessor existed (a strictly additive
// change: only imported symbols see new behaviour). The symbol side is the
// fallback, which is the same shape as the other model accessors in this tree —
// `sema::enumDeclaredValues` (EnumValues.hpp), `sema::builtinExceptionBaseOf`
// (SymbolCollection.cpp, #2950), `Emitter::recordIsPositional`
// (LlvmBackend_impl.hpp).
//
// ⚠ ONE accessor, not N agreeing copies. Six independent spellings of this walk
// existed before #3143 (`genericParamNamesOf`, `RecordSubstGuard`'s two
// branches, and one hand-rolled `td ? … : ts->genericParams` at each of
// LlvmBackend_Calls.cpp, LlvmBackend_Calls2.cpp ×2, LlvmBackend_Layout.cpp);
// the other sixteen sites read the AST only. Two copies of one predicate
// reading different sources is the shape that produced the `recordTypeOfRef`
// and §8.5-rendering divergences — add call sites here, not new walks.

#include <cstddef>
#include <string_view>
#include <vector>

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"

namespace bezel::sema {

// -- THE ONE SOURCE DECISION -----------------------------------------------
//
// Every accessor below routes through this and nothing else, so there is
// exactly one `dynamic_cast` in the family and the "which source" question is
// answered in one place. It returns the AST parameter list when the type has an
// `ast::TypeDecl` (a locally-declared type -- unchanged behaviour), and null
// otherwise, in which case each accessor reads `TypeSymbol::genericParams`.
namespace detail {
inline const std::vector<ast::GenericParam>* astGenericParams(
        const TypeSymbol* ts) {
    if (!ts) return nullptr;
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl))
        return &td->genericParams;
    return nullptr;
}
}  // namespace detail

// §14.1: how many generic parameters `ts` DECLARES -- 0 for a null or
// non-generic type. This is also the declared ARITY every caller compares
// against a `TypeRef::genericArgs` size, and it is the whole question at the
// predicate sites ("is the parent generic?"), so it allocates nothing.
inline std::size_t genericParamCount(const TypeSymbol* ts) {
    if (const auto* ast = detail::astGenericParams(ts)) return ast->size();
    return ts ? ts->genericParams.size() : 0;
}

// §14.1: the NAME of the i-th declared parameter -- the key codegen's
// `genericSubst_` / `genericSubstSemaTy_` maps are built on. Empty for an
// out-of-range index, so a caller never reads past the declared list.
//
// Indexing is POSITIONAL against the declared list on both sides, never
// compacted: the maps bind name[i] to `genericArgs[i]`, so a hypothetical null
// slot must leave the following parameters at their own positions rather than
// shifting them onto their neighbours' arguments. (Neither
// `SymbolCollection::collectGenericParams` nor
// `ModuleLoader::convertGenericParams` can produce a null, so this states which
// invariant is load-bearing, not a live case.)
inline std::string_view genericParamNameAt(const TypeSymbol* ts,
                                           std::size_t i) {
    if (const auto* ast = detail::astGenericParams(ts))
        return i < ast->size() ? (*ast)[i].name : std::string_view{};
    if (!ts || i >= ts->genericParams.size()) return {};
    const Symbol* gp = ts->genericParams[i];
    return gp ? gp->name : std::string_view{};
}

// §12.9: the variance marker of `ts`'s i-th declared parameter -- "in", "out",
// or "" for invariant, which is also the answer for an out-of-range index.
// Allocation-free: `variantGenericAssignable` calls this once per type
// ARGUMENT, so building a vector here would make an N-parameter type cost N
// walks and 2N allocations where the AST-only code it replaced did one index.
inline std::string_view genericParamVariance(const TypeSymbol* ts,
                                             std::size_t i) {
    if (const auto* ast = detail::astGenericParams(ts))
        return i < ast->size() ? (*ast)[i].variance : std::string_view{};
    if (!ts || i >= ts->genericParams.size()) return {};
    if (const auto* gps =
            dynamic_cast<const GenericParamSymbol*>(ts->genericParams[i]))
        return gps->variance;
    return {};
}

// §12.9: does `ts` declare ANY variant (`in`/`out`) parameter? Short-circuits
// on the first one and allocates nothing.
inline bool hasVariantGenericParam(const TypeSymbol* ts) {
    const std::size_t n = genericParamCount(ts);
    for (std::size_t i = 0; i < n; ++i) {
        const std::string_view v = genericParamVariance(ts, i);
        if (v == "in" || v == "out") return true;
    }
    return false;
}

// §14.1: just the NAMES, in declaration order -- for the substitution-building
// sites, which need the whole list anyway because they walk it to fill a map.
// Call it only AFTER the cheap guards (`genericArgs.empty()`, the receiver-type
// checks) have decided a substitution is actually wanted: it is the one
// allocating accessor here.
inline std::vector<std::string_view> genericParamNames(const TypeSymbol* ts) {
    const std::size_t n = genericParamCount(ts);
    std::vector<std::string_view> names;
    names.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        names.push_back(genericParamNameAt(ts, i));
    return names;
}

}  // namespace bezel::sema
