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
// announcement of the retention"), so a declaration-only proof reads the
// RETURN SLOT for the frame announcement:
//   * a Task-family or iterable-interface return (the frame is announced) —
//     fix #9309: this is the FRAME-RESIDENT arm, and it is ADMITTED: the
//     frame holds a reference of its OWN on the closure cell and drops it at
//     the frame's end, so the caller's own reference is the caller's to drop
//     (§28.9) — see `declaredFormalIsCallBounded`'s own note;
//   * a FUNCTION-typed return (it could carry the callback OUT of the call) —
//     DECLINED;
//   * a return whose origin is unknown — a CLASS type parameter a subclass may
//     instantiate at such a head (§12.1: return types must match exactly, so
//     the base's declaration decides; a method's OWN type parameter stays
//     abstract in every override and cannot become a frame) — DECLINED.
//
// Every remaining clause fails closed → ESCAPE → a leak, never an early
// release and never a double free.
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

// fix #9309 (§28.14 *Call-bounded × deferred activation* / §28.9, v0.359):
// does this RESOLVED type ANNOUNCE a deferred-activation FRAME — a Task-
// family or iterable-interface head? §28.14: "the deferred-activation return
// type is the signature-level announcement of the retention"; the heads are
// the ones `common::isTaskFamilyHead` / `common::isIterableInterfaceHead`
// name (the registry the whole compiler reads — §28.3 *Deferred-activation
// callees*).
inline bool typeAnnouncesDeferredFrame(const sema::TypeRef* t) {
    if (!t || t->kind != sema::TypeRef::Kind::Named) return false;
    const std::string_view head = t->headName();
    return common::isTaskFamilyHead(head) ||
           common::isIterableInterfaceHead(head);
}

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

// fix #9309 (§28.14 *Call-bounded × deferred activation* / §28.9, v0.359):
// does the callee's DECLARED return slot announce a deferred-activation
// FRAME? The frame-residence arm of `declaredFormalIsCallBounded` below.
inline bool declaredReturnAnnouncesDeferredFrame(
        const CallBoundedDeclaration& d) {
    return typeAnnouncesDeferredFrame(d.returnType);
}

// The §28.14 call-bounded default, proved on a declaration.
//
// fix #9309 (§28.14 *Call-bounded × deferred activation* / §28.9, v0.359):
// the FRAME-RESIDENT case is admitted, and it is the one case `declared-
// ReturnIsFrameFree` declines. §28.14: "the callee receives the callback for
// the duration of the call only — or, when the call mints a deferred-
// activation frame, for that frame's life". So a callee whose declared return
// slot ANNOUNCES a frame (a Task-family or iterable-interface head) HOLDS the
// callback in that frame — and the frame takes its OWN +1 on the closure cell
// (the sync iterator's construction retain, `LlvmBackend_Iterators.cpp`'s
// `emitIteratorMethod` capture-store retain; the async task's, `LlvmBackend_
// Async.cpp`'s `emitAsyncMethod`; both dropped at the frame's terminal edge)
// and drops it there. The callback therefore SURVIVES the call, but the
// caller's reference is the CALLER's to drop: §28.9 — "every function-value
// holder releases exactly one reference" — the local is a holder and the frame
// is a holder, and each drops its own. That is the arrangement #1864 / #2609
// (the sync / async ITERATOR producer release) and #8867 (the plain ASYNC
// producer release) already rely on for a +1 PRODUCER argument; this is the
// same arrangement for the closure LOCAL.
//
// WHY THIS IS NOT THE EARLY RELEASE THE #9150 DECLINE GUARDED AGAINST. The
// #9150 proof declined the frame-announcing return because "releasing at
// scope end would be a use-after-free while the frame is alive" — the frame
// would read a closure the local's scope end had freed. MEASURED on the
// issue's own repro (`BEZEL_DUMP_IR=1`), the frame does NOT borrow the
// caller's reference: `bz$M.Sub$M` (the iterator method) emits
// `call void @bz_closure_retain(ptr %iter.clcap)` at construction and the
// frame's `Dispose` / `CloneDispose` emit the matching
// `bz_closure_release`. The frame holds a reference of its OWN, so the
// caller's scope-end release cannot free the closure the frame still holds.
//
// The `viaDeferredFrame` out-param reports WHICH arm admitted the formal, so
// the caller can tell "the frame holds a reference of its own" (the closure
// may OUTLIVE the caller's scope — its capture cells must not be freed with
// that scope) from the frame-free arm (the closure dies with the scope).
inline bool declaredFormalIsCallBounded(const CallBoundedDeclaration& d,
                                        bool* viaDeferredFrame = nullptr) {
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
    if (declaredReturnIsFrameFree(d)) return true;
    // fix #9309 (§28.14 *Call-bounded × deferred activation* / §28.9, v0.359):
    // the FRAME-RESIDENT arm — the declared return slot announces a deferred-
    // activation frame, so the callee holds the callback for that frame's
    // life and the frame takes a reference of its own (see the note above).
    if (declaredReturnAnnouncesDeferredFrame(d)) {
        if (viaDeferredFrame) *viaDeferredFrame = true;
        return true;
    }
    return false;
}

}  // namespace bezel::codegen
