#pragma once

// #1414 Stage 4 Phase A — the shared ownership-FACTS core (behaviour-preserving).
//
// The compiler decides "does this expression PRODUCE a fresh owned value?" and
// "does this call TRANSFER an owned element to the caller?" in several places.
// Historically each place carried its OWN copy of that expression-shape
// classification, and the copies drifted silently past a green suite — the
// #1388 drift class (and #1415, found during the review of this plan).  This
// module is the ONE home for the expression-SHAPE half of that decision, so a
// single fact lives in a single place.
//
// Two consumers query this core (see the delegating wrappers, each commented as
// "the {sema,codegen}-side consumer of the shared ownership-facts core"):
//   * SEMA — the ownership pass (src/ownership/Ownership.cpp `producerState`),
//     which runs BEFORE monomorphization and supplies an identity resolver.
//   * CODEGEN — `Emitter::isFreshOwnedClassProducer` /
//     `Emitter::isBuiltinOwnedTransferCall` (src/codegen/LlvmBackend_Setup.cpp),
//     which run inside monomorphized bodies and inject `resolveTypeParamRef`
//     (backed by `genericSubstSemaTy_`) as the resolver hook.
//
// Scope boundaries (per the plan, §"Stage 4", amendments 1 & 3):
//   * This core owns EXPRESSION SHAPE only.  MECHANISM selection — disposerFns_,
//     runtime-vs-vtable routing, unwind-ineligibility, the §28 discharge
//     machinery — stays in codegen and is NOT moved here.
//   * All NAME-KEYED tables live in the Stage-3 BuiltinRegistry; this core
//     CONSULTS the registry rather than re-listing builtin names.  (The
//     Pop/Dequeue-on-`OwningStack`/`OwningQueue` and TakeCurrent-on-owning-
//     enumerator guards below are per-method operand pairings, NOT a reusable
//     name table, so they stay inline — see OwnershipFacts.cpp.)
//
// Phase A is a BEHAVIOUR-PRESERVING extraction: every classification result is
// byte-identical to the pre-refactor wrappers.  The drift fixes (#1388, #1415)
// are Phase B, deliberately NOT done here.

#include <functional>

namespace bezel::ast {
struct Expr;
struct CallExpr;
struct MethodDecl;
}  // namespace bezel::ast

namespace bezel::sema {
class SemaContext;
struct TypeRef;
}  // namespace bezel::sema

namespace bezel::common {

// A hook that resolves a bare type-parameter TypeRef (`T` / `K`, a single-part
// Named ref with no `declaration`) to its CONCRETE substituted sema type in the
// caller's current context.  Codegen supplies `resolveTypeParamRef` (backed by
// the active `genericSubstSemaTy_` monomorphization binding); sema, which runs
// before monomorphization, supplies the identity resolver.  Any non-parameter
// ref must be returned unchanged. // per §14.1 / fix #567
using TypeParamResolver =
    std::function<const sema::TypeRef*(const sema::TypeRef*)>;

// Shared shape-recognition: if the callee of `call` resolves to a user
// `ast::MethodDecl` (a MethodSymbol whose `decl` is a MethodDecl), return it;
// null otherwise (unresolved overload, a builtin with no MethodSymbol, or a
// call through a value/function).  Both consumers reach the `returnOwned`
// authority through this one navigation.
const ast::MethodDecl* calleeMethodDecl(const ast::CallExpr& call,
                                        const sema::SemaContext& ctx);

// EXPRESSION-SHAPE producer classification (#1039 / #1053 — the weakest of the
// four historic copies): a literal `new T(...)` or a call to a method declared
// to return `owned T`, whose STATIC result type is a heap CLASS.  A variable /
// field / `this` / property receiver is neither, so it is never a fresh
// producer; a null / value-type / value-builtin result is excluded.
//
// `resolveParam` is the documented shared resolver hook; the weakest-copy shape
// intentionally does NOT resolve the receiver type-parameter (a `new T()` in a
// monomorphized body is conservatively NOT a fresh producer here — leak-but-
// sound), so behaviour matches the pre-refactor codegen predicate exactly.
bool isFreshOwnedClassProducer(const ast::Expr* e,
                               const sema::SemaContext& ctx,
                               const TypeParamResolver& resolveParam);

// EXPRESSION-SHAPE transfer classification (#1349 / #642 / §4.5–§4.11 / §2.14 /
// §29.1): builtin methods that TRANSFER an owned class element to the caller.
// These have no MethodSymbol, so recognition keys on the callee name plus — for
// the names with a borrowing counterpart — an OWNING receiver type.
bool isBuiltinOwnedTransferCall(const ast::CallExpr& call,
                                const sema::SemaContext& ctx);

// §28.3 "Factory methods and sourceless returns" / Appendix C.3 (fix #1499): a
// STATIC method returning a plain class/interface type with NO `owned`
// annotation has no STRUCTURAL borrow source — a static call has no receiver —
// so the CALLER must discharge its result (bind with `using var`, store in an
// `owned` field, pass to an `owned` parameter, or dispose) exactly as for
// `new`. This predicate is true ONLY when the callee genuinely PRODUCES a fresh
// owned value: it fires when `md` is `static`, its resolved result `resultTy`
// is a plain class/interface (NOT a value-builtin, NOT an owning-collection/
// -wrapper head — those carry the more specific §28.6 documented-source rule),
// AND EVERY return path of `md`'s body yields a fresh owned producer (`new
// T(...)` or an `owned`-returning call — via isFreshOwnedClassProducer).
//
// A static method that returns a BORROW on ANY path is DELIBERATELY excluded:
// a static/instance FIELD read (`_shared` — process-lived source, §28.3
// producers "Static property access ... Borrowed"), a parameter, `this`, or a
// plain local alias. Classifying such a borrow-return as owned would make
// `using var r := Get()` DISPOSE a value the callee does not own — a
// double-free of a shared/borrowed object (§28.10) — the exact soundness
// regression this predicate must avoid (the caller-side owned-assumption is
// sound only opposite the callee-side borrow-return prohibition, §28.3
// "Factory methods" non-normative note / borrow-lifetime rules; #1499).
//
// Callers must have already established `md.returnOwned == false` (an explicit
// `owned` return is owned directly, without this rule). `resolveParam` is the
// shared type-parameter resolver hook, threaded into isFreshOwnedClassProducer.
bool staticFactoryReturnsFreshOwned(const ast::MethodDecl& md,
                                    const sema::TypeRef* resultTy,
                                    const sema::SemaContext& ctx,
                                    const TypeParamResolver& resolveParam);

}  // namespace bezel::common
