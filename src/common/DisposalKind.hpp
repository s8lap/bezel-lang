#pragma once

// The `using` / `await using` DISPOSAL ANCHOR (campaign-1 `using-heads`).
//
// §28.12 (v0.345) is explicit about the key: "`await using` selects the
// logical-disposal path by the class of the object the binding HOLDS ... never
// by the binding's static type." The compiler had THREE near-answering
// authorities for that question, each answering a DIFFERENT question, each
// with a different population — the #3419 "duplicated classification
// predicates" shape:
//
//   1. `Emitter::registerUsingDisposal`'s local lambdas
//      `typeMayHoldClass` / `awaitUsingSlotMayHoldClass`
//      (src/codegen/LlvmBackend_Setup.cpp) — the CODEGEN side. One bit ("may
//      this slot hold a class?"), a local lambda inside one function, keyed on
//      the DECLARED type first.
//
//   2. `MethodOwnership::rhsIsDisposableClassType` (src/ownership/Ownership.cpp)
//      — the SEMA/OWNERSHIP side. A BOOLEAN ("does this type carry a
//      disposal obligation?"), keyed on the declared type only, and with NO
//      runtime-managed arm at all (which is why the §28.9 vacuity lint and
//      the codegen predicate disagreed — #7338).
//
//   3. `Emitter::usingVarBuiltinHeadName` (src/codegen/LlvmBackend_Setup.cpp)
//      — the head-NAME resolver. A NAME, by name only, and it cannot name a
//      MONOMORPHIZED generic head (which is why #3482's head reached the
//      word-0 read).
//
// THIS pair of files is the ONE authority those sites now read. It answers
// the anchor's question — *what disposal obligation does this right-hand side
// carry, and of which kind?* — as a KIND, not a boolean, because a boolean
// opt-in can only admit or refuse: it cannot say WHICH obligation the
// right-hand side carries, and the `await using var` spelling is exactly the
// spelling a boolean misses.
//
// The five kinds (per the campaign record, "The anchor — one page"):
//
//   NONE              §28.9: the RHS is a runtime-managed value (`string`,
//                     `Type`, boxed value types, function values) or a value
//                     type with no owned payload. Scope exit does nothing;
//                     §28.10: "the disposal at scope end is a no-op ... and
//                     the only effect is the reassignability restriction".
//   CLASS-SYNC        §28.4: the RHS's runtime class carries a destructor
//                     chain. Scope exit runs the chain, then reclaims.
//   CLASS-ASYNC       §28.12: the RHS's runtime class implements
//                     `IAsyncDisposable`. `await using` awaits its
//                     `DisposeAsync`; step 2 then runs the cascade and the
//                     chain, and the destructor body at a level that declares
//                     or overrides a non-`abstract` `DisposeAsync` is
//                     SKIPPED.
//   VALUE-LIKE PAYLOAD §35.3 / §35.4: a value type whose active case holds a
//                     runtime-managed payload with a +1 reference. Scope exit
//                     RELEASES the reference — there is no `Dispose()` to
//                     call, but the runtime lifetime must be the plain-`var`
//                     twin's (§28.10).
//   MOVE-ONLY         §15.3 / §35.4: an owning inline array or an owning
//                     choice. Scope exit disposes the owned slots in place.
//
// THE KEY IS THE SLOT'S REPRESENTATION, NOT ITS DECLARED TYPE. The
// representation is the right-hand side's OWN type; `representationOf` below
// resolves it, MONOMORPHIZATION-AWARE (a `new T()` at `T = Stopwatch` names
// no builtin head until the live substitution is consulted — #3482).

#include <string_view>
#include <unordered_map>

namespace bezel::sema {
struct TypeRef;
struct TypeSymbol;
class SemaContext;
}  // namespace bezel::sema

namespace bezel::common {

// The five disposal kinds. See the file header for the spec citations.
enum class DisposalKind {
  None,              // §28.9
  ClassSync,         // §28.4
  ClassAsync,        // §28.12
  ValueLikePayload,  // §35.3 / §35.4
  MoveOnly,          // §15.3 / §35.4
};

// A live monomorphization substitution: type-parameter name -> concrete type.
// `Emitter::genericSubstSemaTy_` has exactly this shape, so the codegen site
// passes it by pointer without a conversion.
using GenericTypeSubst =
    std::unordered_map<std::string_view, const sema::TypeRef*>;

// The anchor's answer for ONE slot.
struct DisposalVerdict {
  // The KIND of the obligation the right-hand side carries.
  DisposalKind kind = DisposalKind::None;

  // The representation's class, when the representation IS a class. For a
  // `new C()` right-hand side this is the RUNTIME class (§28.12's "the class
  // of the object the binding holds"), so the async path can resolve both the
  // awaited `DisposeAsync` and §28.12 step 2's reclamation from it.
  const sema::TypeSymbol* cls = nullptr;

  // The slot MAY hold a class instance at run time — the conservative bit the
  // codegen registration needs (an `Object`- or interface-typed slot whose
  // right-hand side does not settle the question statically).
  bool mayHoldClass = false;

  // Scope exit has REAL work to do — the §28.10 vacuity lint's question
  // ("does this type carry a disposal obligation?"). This is deliberately
  // narrower than `kind != None`: a class with neither a destructor nor an
  // `owned` field carries no obligation of its own, and §28.10's lint has
  // always answered that way (fix #3588 kept it separate from
  // `isNonTrivialDisposeClass` on purpose — see that predicate's comment for
  // the five axes on which the two differ).
  bool carriesObligation = false;
};

// §28.9's runtime-managed set, as the DISPOSAL question reads it:
//   - a `string` value,
//   - the `Type` token (an interned runtime value — §28.9: "the instance is
//     not compiler-freed at any holder's release point ... the trivial-dispose
//     free below would set the disposed flag on a token every other holder
//     still reads"),
//   - a function value.
// A BOXED VALUE TYPE is deliberately NOT in this set: §28.12 permits the
// synchronous disposal for it, because a box's word 0 IS a vtable (#7338).
bool isRuntimeManagedValue(const sema::TypeRef* t);

// The slot's REPRESENTATION: the right-hand side's OWN type, resolved
// MONOMORPHIZATION-AWARE. `rhsTy` is the right-hand side's own type (for a
// `new C()` right-hand side, the CONSTRUCTED type); `subst` is the live
// instantiation's substitution (null when there is none). A type parameter
// resolves through the substitution to the concrete type the instantiation
// bound it to, so a monomorphized head can finally be named (#3482).
const sema::TypeRef* representationOf(const sema::TypeRef* rhsTy,
                                       const GenericTypeSubst* subst);

// THE ANCHOR. Classify `t` — the slot's representation — into a kind.
DisposalVerdict classifyDisposal(const sema::TypeRef* t, sema::SemaContext& ctx);

// Does this class (or one of its bases) declare a `DisposeAsync` method?
// §28.12's "the RHS's runtime class implements `IAsyncDisposable`".
bool classDeclaresDisposeAsync(const sema::TypeSymbol* cls);

}  // namespace bezel::common
