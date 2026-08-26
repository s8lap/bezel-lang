#include "codegen/LlvmBackend_impl.hpp"

// #602 / §28.11 v0.134 — element-policy unification, batch 1.
// fix #619 / §28.11 v0.135 — widen class/interface row to include Interface.
// #602 batch 4 — uniform runtime finalization from the policy (the #545 hook).
//
// This translation unit owns the single source of truth for collection-element
// slot behavior: the `ElementPolicy` struct and its factory
// `elementPolicyFor(elemTy, owningFamily)`.  Every field in `ElementPolicy`
// corresponds directly to a column in the §28.11 v0.135 "element slot policy"
// table, and the factory's switch mirrors the table row-for-row with a
// per-row `// per §28.11 v0.135` citation.
//
// Batch 1: factory wired into the EXISTING helpers in LlvmBackend_Setup.cpp as
// thin delegates; no call sites change.
// Batches 2/3: all 82 call sites routed through the policy directly; old helpers
// (owningCollectionElemDisposer, owningDictValueDisposer, isStringElementSet,
// isStringValueDict, stringElementReleaseDisposer) deleted.
// fix #619: the batch-1 delegates had Class-only pre-checks that blocked
// interface element types from reaching the factory.  Those pre-checks are
// removed; collectionElemIsClass widened to Class|Interface.
// Batch 4 (#545 hook): elementDisposerArg() added (LlvmBackend_impl.hpp) —
// the single site that maps (elemTy, owningFamily) → llvm::Value* for every
// runtime dispose_elem parameter. bz_channel_new migrated onto it; Assign.cpp
// OwningDictionary move-out gate migrated from collectionElemIsClass to
// elementPolicyFor; isStringElementArray deleted (inlined into
// emitArrayRetainStringsIfStringArray which now calls elementPolicyFor directly).

namespace bezel::codegen {

// -------------------------------------------------------------------------
// elementPolicyFor — the §28.11 v0.135 table in code
// -------------------------------------------------------------------------
//
// Parameters
//   elemTy      — the resolved sema type of ONE element (genericArgs[0] for
//                 most collections; genericArgs[1] for an OwningDictionary
//                 value slot).  Null or Error → Value category (no policy).
//   owningFamily — true iff the containing collection is in the OWNING family
//                 (name prefixed "Owning", per the stdlib §4 naming convention).
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
    // per §28.11 v0.135: class/interface × owning  → owned (disposer thunk)
    //                    class/interface × borrowing → borrow (no disposer)
    //
    // §28.13 / stdlib §2.16 (#603): Shared<T> and Weak<T> have null declaration
    // (they are compiler-known builtins, not user TypeDecls). They are ref-counted
    // heap pointers that behave like class instances in collection slots: an
    // OwningArray<Shared<T>> OWNS its handles (releases on remove/clear/free) and
    // a borrowing Array<Shared<T>> just borrows them. Detect by astNode name before
    // the declaration check so they fall into the ClassOwned/ClassBorrow arm, not
    // the Value arm (the fall-through after this block).
    auto isSharedOrWeak = [](const sema::TypeRef* t) {
        if (!t || t->kind != sema::TypeRef::Kind::Named) return false;
        if (t->declaration) return false;
        if (!t->astNode || !t->astNode->name || t->astNode->name->parts.empty())
            return false;
        auto n = t->headName();
        return n == "Shared" || n == "Weak";
    };
    // stdlib §5 (#651): IGrouping<TKey, T> elements are bz_grouping_t* pointers
    // owned by the Array<IGrouping> produced by GroupBy(...).ToArray(). They need
    // a disposer thunk that calls bz_grouping_free(ptr) to release the element
    // array and key data. Always treated as owned (GroupBy produces fresh groups).
    auto isIGroupingElem = [](const sema::TypeRef* t) {
        if (!t || t->kind != sema::TypeRef::Kind::Named) return false;
        if (t->declaration) return false;
        if (!t->astNode || !t->astNode->name || t->astNode->name->parts.empty())
            return false;
        return t->headName() == "IGrouping";
    };
    if (isIGroupingElem(elemTy)) {
        // Use the cached groupingFreeThunk() (built once per module,
        // null-checked, same pattern as sharedReleaseThunk).
        return ElementPolicy{
            ElementPolicy::Category::ClassOwned,
            ptrTy(),
            groupingFreeThunk(),
            /*retainRelease=*/false
        };
    }
    // §2.11 (#838): a Subscription is a runtime-managed handle (bz_subscription_t*,
    // null declaration — a compiler-known builtin like Shared/Weak/IGrouping) that
    // is disposed by bz_subscription_dispose (unsubscribing). An owning collection
    // OWNS its Subscriptions and must dispose each at scope-end / clear / remove;
    // before this it fell through to the Value policy (null disposer) and the
    // subscriptions leaked (the handler kept firing after the collection died). A
    // borrowing collection of Subscriptions just borrows them (no disposal).
    if (isSubscriptionType(elemTy)) {
        if (owningFamily)
            return ElementPolicy{
                ElementPolicy::Category::ClassOwned,
                ptrTy(),
                subscriptionDisposeThunk(),
                /*retainRelease=*/false
            };
        return ElementPolicy{
            ElementPolicy::Category::ClassBorrow,
            ptrTy(),
            /*disposerThunk=*/nullptr,
            /*retainRelease=*/false
        };
    }
    // §28.11 (#868): builtin OWNING-collection element types (OwningArray<R> etc.
    // as elements of an outer collection). These have declaration==nullptr
    // (compiler-known builtins, same as Shared/Weak/IGrouping/Subscription) so
    // they fall past the declaration-based class check below → were silently
    // treated as plain Value → inner collection never freed → leak.
    // Detect by name prefix and route to nestedCollectionDisposerThunk for the
    // owning-family (outer collection owns the inner), or ClassBorrow for the
    // borrowing-family (outer borrows inner — must NOT dispose it, or double-free).
    auto isOwningBuiltinCollection = [](const sema::TypeRef* t) {
        if (!t || t->kind != sema::TypeRef::Kind::Named) return false;
        if (t->declaration) return false;
        if (!t->astNode || !t->astNode->name || t->astNode->name->parts.empty())
            return false;
        std::string_view n = t->headName();
        return n == "OwningArray" || n == "OwningStack" || n == "OwningQueue" ||
               n == "OwningLinkedList" || n == "OwningDictionary" || n == "OwningSet";
    };
    if (isOwningBuiltinCollection(elemTy)) {
        if (owningFamily) {
            // per §28.11, #868: owned slot — the inner collection must be freed
            // via its own runtime free fn + inner element disposer (recursive).
            return ElementPolicy{
                ElementPolicy::Category::ClassOwned,
                ptrTy(),
                nestedCollectionDisposerThunk(elemTy),
                /*retainRelease=*/false
            };
        } else {
            // per §28.11, #868: borrowing outer collection — must NOT dispose the
            // inner collections (they are owned elsewhere); borrow-only slot.
            return ElementPolicy{
                ElementPolicy::Category::ClassBorrow,
                ptrTy(),
                /*disposerThunk=*/nullptr,
                /*retainRelease=*/false
            };
        }
    }

    // §30.5 / §28.11 (#1889): an owning collection of Task-family values
    // (Task / Task<T> / OwningTask<T>) owns each task frame, and disposing the
    // collection must also dispose the unconsumed class result and the fault
    // object (§30.9) of every element. A task is a builtin class with no user
    // TypeDecl (declaration == nullptr OR declaration->decl == nullptr), so it
    // does not match the declaration-based class arm below, nor any of the
    // other builtin-handle arms above — before this fix it fell through to the
    // Value row (null disposer), and bz_array_free / bz_llist_free /
    // bz_dict_free was called with a NULL disposer; every element's task
    // frame, async frame, and result slot leaked (208 B per element under
    // `bz$App$Make`, the #1889 element-half / #2224 deferred gap).
    //
    // The element thunk (taskElementDisposerThunk) runs the same complete
    // non-blocking task disposal protocol the `using var` task scope-end path
    // and the owned Task-family field path share via emitTaskDisposal —
    // fault-object prelude, per-T result_disposer attach (for OwningTask<T:
    // class>), and bz_task_fire_and_forget[_checked]. The borrowing family
    // retains the existing borrow-only policy: a borrowed task frame is
    // disposed elsewhere (the mint CALL's scope), so the outer collection must
    // NOT fire-and-forget it again — a double fire-and-forget on one
    // bz_task_t* corrupts async continuation state (#3096's symptom). The
    // Task branch is deliberately BEFORE the Shared/Weak / user-class arm so
    // a builtin task TypeRef never falls into the declaration-based test.
    if (elemTy && isTaskTypeName(elemTy)) {
        if (owningFamily) {
            return ElementPolicy{
                ElementPolicy::Category::ClassOwned,
                ptrTy(),
                taskElementDisposerThunk(elemTy),
                /*retainRelease=*/false
            };
        }
        return ElementPolicy{
            ElementPolicy::Category::ClassBorrow,
            ptrTy(),
            /*disposerThunk=*/nullptr,
            /*retainRelease=*/false
        };
    }

    bool isSharedWeakElem = isSharedOrWeak(elemTy);
    // Distinguish Shared vs Weak for per-element release thunk selection.
    auto isWeakElem = [](const sema::TypeRef* t) {
        if (!t || t->kind != sema::TypeRef::Kind::Named) return false;
        if (t->declaration) return false;
        if (!t->astNode || !t->astNode->name || t->astNode->name->parts.empty())
            return false;
        return t->headName() == "Weak";
    };
    if (isSharedWeakElem ||
        (elemTy && elemTy->kind == sema::TypeRef::Kind::Named &&
         elemTy->declaration &&
         (elemTy->declaration->kind == ast::TypeDeclKind::Class ||
          elemTy->declaration->kind == ast::TypeDeclKind::Interface))) {
        if (owningFamily) {
            // per §28.11 v0.135: owned slot — disposal cascades via vtable
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
            // per §28.11 v0.135: borrow slot — no element disposal obligation.
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
            // crashing; this preserves prior behavior (a null disposer for
            // non-class elements, as in all prior helper implementations) and
            // keeps the suite green.
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
