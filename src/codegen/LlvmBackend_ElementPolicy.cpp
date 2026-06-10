#include "codegen/LlvmBackend_impl.hpp"

// #602 / §28.11 v0.134 — element-policy unification, batch 1.
//
// This translation unit owns the single source of truth for collection-element
// slot behavior: the `ElementPolicy` struct and its factory
// `elementPolicyFor(elemTy, owningFamily)`.  Every field in `ElementPolicy`
// corresponds directly to a column in the §28.11 v0.134 "element slot policy"
// table, and the factory's switch mirrors the table row-for-row with a
// per-row `// per §28.11 v0.134` citation.
//
// In batch 1 the factory is wired into the EXISTING helpers in
// LlvmBackend_Setup.cpp as thin delegates; no call sites change yet.
// Batch 2/3 will route all 82 call sites through the policy directly.

namespace bezel::codegen {

// -------------------------------------------------------------------------
// elementPolicyFor — the §28.11 v0.134 table in code
// -------------------------------------------------------------------------
//
// Parameters
//   elemTy      — the resolved sema type of ONE element (genericArgs[0] for
//                 most collections; genericArgs[1] for an OwnedDictionary
//                 value slot).  Null or Error → Value category (no policy).
//   owningFamily — true iff the containing collection is in the OWNING family
//                 (name prefixed "Owned", per the stdlib §4 naming convention).
//
// The table from §28.11 v0.134:
//
//  Element category                  | Borrowing family | Owning family
//  ──────────────────────────────────┼──────────────────┼──────────────
//  value type (non-move-only)        | copies           | rejected (*)
//  runtime-managed ref (string, fn…) | shared refs      | rejected (*)
//  class / interface type            | borrows          | owned
//  move-only value type              | rejected (*)     | rejected (*)
//
//  (*) sema rejects these combinations at instantiation; the policy for a
//  rejected combination is undefined (callers should not reach it, but we
//  return a safe Value/null policy rather than crashing).
//
// NOTE: "function types" (closures / first-class functions, §27.5 / §28.9)
// are categorised as RuntimeManaged — they are reference types managed by
// the runtime ARC (bz_closure_retain / bz_closure_release), exactly like
// string.  A future batch may split them further if needed; for now they
// share the RuntimeManaged row and the retainRelease=true flag tells callers
// to invoke bz_closure_retain/release rather than bz_string_retain/release
// (the concrete thunk selection remains in the helpers; the category is what
// matters here).
//
// "Boxed values" (§28.9 / §8.6 — a value type stored as an Object/interface
// at runtime via the boxing bridge) are also RuntimeManaged at the slot level:
// they are managed through the bz_box refcount.  In practice, an element type
// that arrived here as a Named TypeRef wrapping a record is handled by the
// VALUE arm (records are value types); a boxed record arriving as an
// Object-typed slot would be Named/Class, handled by the CLASS arm.  The
// boxing-for-dicts path is separate (valueBoxFreeThunk) and not touched here.

Emitter::ElementPolicy Emitter::elementPolicyFor(const sema::TypeRef* elemTy,
                                                  bool owningFamily) {
    // ── Row 3: class / interface type ──────────────────────────────────────
    // per §28.11 v0.134: class/interface × owning  → owned (disposer thunk)
    //                    class/interface × borrowing → borrow (no disposer)
    //
    // §28.13 / stdlib §2.16 (#603): Shared<T> and Weak<T> have null declaration
    // (they are compiler-known builtins, not user TypeDecls). They are ref-counted
    // heap pointers that behave like class instances in collection slots: an
    // OwnedArray<Shared<T>> OWNS its handles (releases on remove/clear/free) and
    // a borrowing Array<Shared<T>> just borrows them. Detect by astNode name before
    // the declaration check so they fall into the ClassOwned/ClassBorrow arm, not
    // the Value arm (the fall-through after this block).
    auto isSharedOrWeak = [](const sema::TypeRef* t) {
        if (!t || t->kind != sema::TypeRef::Kind::Named) return false;
        if (t->declaration) return false;
        if (!t->astNode || !t->astNode->name || t->astNode->name->parts.empty())
            return false;
        auto n = t->astNode->name->parts[0];
        return n == "Shared" || n == "Weak";
    };
    bool isSharedWeakElem = isSharedOrWeak(elemTy);
    // Distinguish Shared vs Weak for per-element release thunk selection.
    auto isWeakElem = [](const sema::TypeRef* t) {
        if (!t || t->kind != sema::TypeRef::Kind::Named) return false;
        if (t->declaration) return false;
        if (!t->astNode || !t->astNode->name || t->astNode->name->parts.empty())
            return false;
        return t->astNode->name->parts[0] == "Weak";
    };
    if (isSharedWeakElem ||
        (elemTy && elemTy->kind == sema::TypeRef::Kind::Named &&
         elemTy->declaration &&
         (elemTy->declaration->kind == ast::TypeDeclKind::Class ||
          elemTy->declaration->kind == ast::TypeDeclKind::Interface))) {
        if (owningFamily) {
            // per §28.11 v0.134: owned slot — disposal cascades via vtable
            // for class/interface; via bz_shared_release / bz_weak_release for
            // Shared<T>/Weak<T> handles (which have no vtable).
            llvm::Function* thunk = isSharedWeakElem
                ? (isWeakElem(elemTy) ? weakReleaseThunk() : sharedReleaseThunk())
                : elementDisposerThunk();
            return ElementPolicy{
                ElementPolicy::Category::ClassOwned,
                ptrTy(),
                thunk,
                /*retainRelease=*/false
            };
        } else {
            // per §28.11 v0.134: borrow slot — no element disposal obligation.
            return ElementPolicy{
                ElementPolicy::Category::ClassBorrow,
                ptrTy(),
                /*disposerThunk=*/nullptr,
                /*retainRelease=*/false
            };
        }
    }

    // ── Row 2: runtime-managed reference (string, function types) ──────────
    // per §28.11 v0.134: runtime-managed × borrowing → shared references
    //                    runtime-managed × owning    → rejected at instantiation
    //                    (sema enforces the restriction; we return RuntimeManaged
    //                     for the borrowing case and a safe Value policy for the
    //                     owning case, which should never be reached).
    if (elemTy) {
        bool isString =
            elemTy->kind == sema::TypeRef::Kind::Primitive &&
            elemTy->primitive == sema::PrimitiveType::String;
        bool isFunctionType = elemTy->kind == sema::TypeRef::Kind::Function;
        if (isString || isFunctionType) {
            // per §28.11 v0.134: shared-reference slot — retain/release on
            // insert/remove/free; no disposal (runtime manages lifetime).
            // The owning family cannot have string/function-typed elements
            // (sema rejects at instantiation — §14.4 / §15.2); if somehow
            // reached, fall through to Value (safe no-op policy).
            if (!owningFamily) {
                return ElementPolicy{
                    ElementPolicy::Category::RuntimeManaged,
                    ptrTy(),
                    /*disposerThunk=*/nullptr,
                    /*retainRelease=*/true
                };
            }
            // DIVERGENCE vs §28.11 v0.134: owning family with runtime-managed
            // element type — spec says "rejected at instantiation"; if we reach
            // here the sema gate did not fire (possible in imported/generic
            // contexts not yet fully checked).  Return Value (no-op) rather than
            // crashing; this preserves prior behavior (owningCollectionElemDisposer
            // returns null for non-class elements) and keeps batch 1 green.
            return ElementPolicy{
                ElementPolicy::Category::Value,
                ptrTy(),
                nullptr,
                false
            };
        }
    }

    // ── Row 1: value type (non-move-only) ──────────────────────────────────
    // per §28.11 v0.134: value × borrowing → copies (no slot-level bookkeeping
    //                    value × owning   → rejected at instantiation
    //                    (same safe-fallback as above for the owning case)
    //
    // Everything that reaches here — primitives other than string, enums,
    // records, unit, inline arrays, unresolved/Error types — is treated as a
    // plain value.  Records are value types (§9); enums are integer-backed
    // (§10); inline arrays carry their elements by value (§15.1).
    //
    // Row 4 (move-only value type, §15.3): also lands here — rejected at
    // instantiation for BOTH families (§28.11 v0.134); the safe Value policy
    // means callers produce no per-slot disposal, which is the correct no-op.
    //
    // slotTy is nullptr here — value types are embedded/copied inline and
    // their concrete LLVM type varies; callers that need the LLVM type must
    // use lowerTypeRef directly (the helpers this batch delegates through all
    // do so already — they only call elementPolicyFor for the category).
    return ElementPolicy{
        ElementPolicy::Category::Value,
        /*slotTy=*/nullptr,
        /*disposerThunk=*/nullptr,
        /*retainRelease=*/false
    };
}

} // namespace bezel::codegen
