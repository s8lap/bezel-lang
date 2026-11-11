#pragma once

// fix #3862 (§10.4 / stdlib §9.9, v0.357): the compiler-provided per-enum
// static members `E.FromNameOrNull(name: string): E?` and
// `E.NameOf(value: E): string`.
//
// These two members have NO MethodSymbol — §10.4 declares them, so no
// declaration is written and no user declaration may override or overload
// one — which is why the codegen dispatches them by their own lowering and
// not through the ordinary member-call path.
//
// TWO questions about them are asked from two translation units, and they
// must agree:
//
//   1. the LOWERING (LlvmBackend_Exprs2.cpp): "does this call have the
//      compiler-provided shape?" — the member name plus a receiver that
//      NAMES an enum type;
//   2. the OWNERSHIP WALK (LlvmBackend_Setup.cpp): "is this call's
//      parameter a BORROW?" — §10.4 declares `name: string` / `value: E`
//      with NO `owned` marker, so the parameter is a borrow and the
//      argument's reference stays the CALLER's to discharge (§28.3.4's
//      "Parameter `p` (no modifier)" row, §28.6).
//
// This header is the ONE place both read, so the two cannot drift.

#include <string>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"

namespace bezel {

// The ENUM TypeSymbol an expression NAMES as a type — the receiver gate
// every compiler-provided per-enum static member dispatches on.
//
// The receiver must NAME an enum type — these are STATIC members (§10.4
// declares them `public static method …`). Without this gate a user class
// declaring its own `static FromNameOrNull(x): SomeEnum?` would be
// hijacked: the member name and the `E?` result type alone do not identify
// the compiler-provided member.
//
// This is codegen's twin of sema's `ExprTC::enumTypeNamedBy`
// (ExprTypecheck_Calls.cpp) and applies exactly the same gate, so the two
// agree.
//
// fix #4072 (§10.4 / §27.8, v0.294; family of
// #4038/#4048/#4050/#4051/#4070): a `type` alias of the enum resolves
// `tsym` to a `TypeAliasSymbol`, not a TypeSymbol or NestedTypeSymbol —
// the same missing-arm shape #4050's DestructuringPattern arm discovered
// (sema fix alone is NOT enough; codegen independently re-resolves here,
// with its OWN matching gap). Without this arm `AliasColor.FromNameOrNull
// (name)` reached the gate with `named` still null, the recognition fell
// through, the call's result type was empty for codegen to dispatch on,
// and the slot silently degraded to zero (#2434 backstop) — a SILENT WRONG
// VALUE, not a crash and not a diagnosed rejection. Resolve through the
// SAME shared `sema::resolveAliasToTypeSymbol` (TypeResolution.hpp,
// #4050/#4051) sema's arms call, so sema and codegen agree on the
// alias-qualified spelling exactly like the direct-name spelling.
inline const sema::TypeSymbol* codegenEnumTypeNamedBy(
        sema::SemaContext& ctx, const ast::Expr& e) {
    const sema::TypeSymbol* named = nullptr;
    if (sema::Symbol* tsym = ctx.exprSymbol(e)) {
        if (auto* ts = dynamic_cast<sema::TypeSymbol*>(tsym)) {
            if (ts->kind == ast::TypeDeclKind::Enum) named = ts;
        } else if (auto* nts = dynamic_cast<sema::NestedTypeSymbol*>(tsym)) {
            if (nts->type && nts->type->kind == ast::TypeDeclKind::Enum)
                named = nts->type;
        } else if (auto* aliasSym =
                       dynamic_cast<sema::TypeAliasSymbol*>(tsym)) {
            if (auto* ts = sema::resolveAliasToTypeSymbol(ctx, aliasSym))
                if (ts->kind == ast::TypeDeclKind::Enum) named = ts;
        }
    }
    return named;
}

// Which compiler-provided per-enum static member a call dispatches to (or
// `None` when the call is an ordinary user call).
//
// fix-forward (review r42-rev-m10-3862, FINDING 1): the ownership walk
// needs this to answer the PARAMETER-OWNERSHIP question. §10.4 declares
// `name: string` and `value: E` with NO `owned` marker, so BOTH parameters
// are BORROWS: the callee reads the argument and the CALLER keeps the
// reference (§28.3.4's "Parameter `p` (no modifier)" row — "Borrowed
// (source: caller)"; §28.6 — "the callee owns NO reference and must not
// release it"). Without the answer, the walk's default for an unmappable
// callee (no MethodSymbol) is CONSUMING, which suppressed the owned local's
// scope-end release and leaked the string on every call (MEASURED: 300
// calls → "11400 byte(s) leaked in 600 allocation(s)").
//
// The KIND (rather than a bool) is what the escape walk needs: only
// `FromNameOrNull`'s parameter is a `string`, so only THAT member's
// argument can be the tracked string local this walk is about.
enum class CompilerProvidedEnumMember {
    None,
    FromNameOrNull,
    NameOf,
};

inline CompilerProvidedEnumMember compilerProvidedEnumMemberKind(
        sema::SemaContext& ctx, const ast::Expr* callee) {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee);
    if (!ma || !ma->target) return CompilerProvidedEnumMember::None;
    if (ma->member != "FromNameOrNull" && ma->member != "NameOf")
        return CompilerProvidedEnumMember::None;
    if (!codegenEnumTypeNamedBy(ctx, *ma->target))
        return CompilerProvidedEnumMember::None;
    return ma->member == "FromNameOrNull"
               ? CompilerProvidedEnumMember::FromNameOrNull
               : CompilerProvidedEnumMember::NameOf;
}

// True iff `callee` is a CALL to one of the two compiler-provided per-enum
// static members — the member name plus a receiver that NAMES an enum
// type (the same gate the lowering applies, so the two agree).
inline bool isCompilerProvidedEnumMemberCall(sema::SemaContext& ctx,
                                             const ast::Expr* callee) {
    return compilerProvidedEnumMemberKind(ctx, callee) !=
           CompilerProvidedEnumMember::None;
}

}  // namespace bezel
