#pragma once

// fix #9150 / fix #9151 (§28.14, §28.9, §12.1, §12.5, §11.2, §11.8, v0.359):
// the §28.14 CALL-BOUNDED DEFAULT, proved on a DECLARATION whose BODY the
// caller cannot read.
//
// WHY A SECOND PROOF, AND WHY IT IS DECLARATION-ONLY. §28.14 (v0.359), *The
// call-bounded default*: "A function-typed parameter declared *without*
// `scoped` is **call-bounded**: the callee receives the callback for the
// duration of the call only — or, when the call mints a deferred-activation
// frame, for that frame's life. ... These obligations are compiler-verified for
// user-authored callees."
//
// The default is a property of the PARAMETER, not of the dispatch. So for a
// callee whose body the caller CANNOT read —
//   * an ABSTRACT member (§12.5: an abstract member "declares it without a
//     body; every concrete subclass must override it"), whose runtime callee is
//     an OVERRIDE;
//   * an INTERFACE member (§11.2: interface members "have no body"; §11.8
//     declines default interface methods), whose runtime callee is an
//     IMPLEMENTATION;
//   * an IMPORTED member (the callee lives in another module), whose runtime
//     callee is that module's member or an override of it;
// the proof is the DECLARATION: §12.1 and §11.2 require an override / an
// implementation to match the base member's parameter modifiers and return type
// EXACTLY, and every one of those bodies is user-authored, so §28.14's
// obligations were verified for it in its own module.
//
// THE ONE THING A DECLARATION MUST ALSO RULE OUT. §28.14's own exception: a
// callee that "mints a deferred-activation frame" keeps the callback "for that
// frame's life" — an `async` callee's task, an iterator's frame (§28.14 *Call-
// bounded × deferred activation*). The frame is ANNOUNCED BY THE RETURN SLOT
// (§28.14: "the deferred-activation return type is the signature-level
// announcement of the retention"), so a declaration-only proof declines:
//   * a Task-family or iterable-interface return (the frame is announced);
//   * a FUNCTION-typed return (it could carry the callback OUT of the call);
//   * a return whose origin is unknown — a CLASS type parameter a subclass may
//     instantiate at such a head (§12.1: return types must match exactly, so
//     the base's declaration decides; a method's OWN type parameter stays
//     abstract in every override and cannot become a frame).
//
// Every clause fails closed → ESCAPE → a leak, never an early release and never
// a double free.
//
// ONE copy, shared by the two arms that need it — the LOCAL no-body callee
// (src/codegen/LlvmBackend_Calls2.cpp) and the IMPORTED callee
// (src/codegen/LlvmBackend_Setup.cpp) — so the two cannot drift, the same
// discipline LlvmBackend_IndexerParams.hpp follows for the indexer twin.

#include <string_view>

#include "common/BuiltinRegistry.hpp"
#include "sema/SemaContext.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::codegen {

// The declaration facts the proof reads.
struct CallBoundedDeclaration {
    // The FORMAL's modifier: "" is the plain (call-bounded) spelling; `scoped`
    // is storage-bounded (§28.14: the callee MAY store the callback) and
    // `owned` / `ref` / `out` transfer or alias the value.
    std::string_view formalModifier;
    // The FORMAL's RESOLVED type: §28.14's default is stated for a FUNCTION-
    // typed parameter.
    const sema::TypeRef* formalType = nullptr;
    // The CALLEE's declared return type.
    const sema::TypeRef* returnType = nullptr;

    // The return type's ORIGIN, as the caller resolved it.
    enum class ReturnOrigin {
        // A CONCRETE type: no frame can be minted at it.
        Concrete,
        // A type parameter the METHOD declares itself: it stays abstract in
        // every override (§12.1), so it cannot become an async or iterator
        // return — no frame.
        MethodTypeParam,
        // Anything else (a CLASS type parameter a subclass may instantiate at a
        // Task-family or iterable head, or an origin the caller cannot read):
        // UNKNOWN → decline.
        Unknown,
    };
    ReturnOrigin returnOrigin = ReturnOrigin::Unknown;
};

// Is the callee's declared return slot FRAME-FREE — i.e. does it neither
// announce a deferred-activation frame nor leave the callback's fate
// unproven? (§28.14 *Call-bounded × deferred activation*, v0.359.)
inline bool declaredReturnIsFrameFree(const CallBoundedDeclaration& d) {
    const sema::TypeRef* rt = d.returnType;
    // No return type, or an unresolved one: the frame's announcement is
    // unknown → decline.
    if (!rt || rt->kind == sema::TypeRef::Kind::Error) return false;
    // A FUNCTION-typed return could carry the callback OUT of the call.
    if (rt->kind == sema::TypeRef::Kind::Function) return false;
    if (rt->kind == sema::TypeRef::Kind::Named) {
        const std::string_view head = rt->headName();
        // The announced deferred-activation heads: a Task-family or iterable-
        // interface return mints a frame that may hold the callback for the
        // frame's life (§28.14 *Call-bounded × deferred activation*).
        if (common::isTaskFamilyHead(head) ||
            common::isIterableInterfaceHead(head))
            return false;
        return d.returnOrigin == CallBoundedDeclaration::ReturnOrigin::Concrete ||
               d.returnOrigin ==
                   CallBoundedDeclaration::ReturnOrigin::MethodTypeParam;
    }
    // A primitive, a record, a choice: no frame, no callback carried out.
    return true;
}

// The §28.14 call-bounded default, proved on a declaration.
inline bool declaredFormalIsCallBounded(const CallBoundedDeclaration& d) {
    // The formal must be PLAIN: `scoped` is storage-bounded (§28.14).
    if (!d.formalModifier.empty()) return false;
    // The formal must be FUNCTION-TYPED: §28.14's default is stated for a
    // function-typed parameter. A bare type parameter is `Kind::Named` at
    // every instantiation (§28.3's set-erasing positions) and never arms
    // this.
    if (!d.formalType ||
        d.formalType->kind != sema::TypeRef::Kind::Function)
        return false;
    if (d.formalType->isResolvedTypeParam()) return false;
    return declaredReturnIsFrameFree(d);
}

}  // namespace bezel::codegen
