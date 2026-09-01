#pragma once

// fix #3638 — the ONE both-sides answer to "does a §30.5 Pattern A task's
// RESULT type carry a disposal obligation because it is an INTRINSIC
// COLLECTION?"
//
// Why this header exists. Two passes ask that question and, before #3638, only
// one of them could answer it:
//
//   * codegen — `Emitter::patternAResultCollectionDisposer`
//     (`src/codegen/LlvmBackend_Disposal.cpp`, added by fix #2279, v0.236).
//     It builds a per-collection-kind free thunk and so treats
//     `Task<Array<T>>` exactly like `Task<SomeClass>`: the task owns the
//     collection's backing storage and frees it at the K8 teardown boundary.
//
//   * ownership/sema — `taskResultArgIsClass`
//     (`src/ownership/Ownership_Checks.cpp`), the gate that makes
//     `awaitTaskKind` report `PatternA` and thereby makes
//     `checkAwaitSitesExpr` enforce §30.5's owning-location confinement.
//     It recognised a user `class` declaration plus a hand-written allowlist
//     of the six `Owning*` collection heads and the sync-primitive result
//     heads — and NOTHING for the plain BORROWING intrinsic collections.
//
// The consequence was a silent miscompile, not a diagnostic difference: sema
// accepted an UNCONFINED `var arr := await GetArr();` (it classified the await
// `ValueResult`, which carries no site obligation), while codegen freed
// `arr`'s backing at the task's teardown — a use-after-free on every
// subsequent read. #3638 measured 7x wrong value / 2x hang / 1x SIGSEGV and
// zero correct runs over ten executions of one binary.
//
// This is LANE_RULES §6's dominant defect generator — a second, drifting
// spelling of a classification an existing authority already makes — so the
// fix routes sema through a shared predicate rather than copying codegen's
// name list into a third place. Codegen still keys its own thunk selection off
// the collection KIND (it must: it picks `bz_array_free` vs `bz_dict_free` vs
// `bz_llist_free` vs `bz_buffer_free`); what is shared is only the
// membership question, which is the part that drifted.
//
// per §30.5 (producer-side return-type rules: "`Task<T>` for class T ...
// disposing the task disposes the result"), §28.3, v0.286

#include <algorithm>
#include <array>
#include <string_view>

namespace bezel::common {

// The intrinsic-collection type heads whose appearance as `T` in a Pattern A
// `Task<T>` means the task owns heap backing storage that its teardown frees.
//
// §6b exclusion diff — this set against codegen's `patternAResultCollection-
// Disposer` computation, in BOTH directions:
//
//   * ADMITTED HERE, admitted there: all fifteen heads below. Every one takes
//     a real free thunk in `patternAResultCollectionDisposer` (array-backed →
//     `bz_array_free`, dictionary-backed → `bz_dict_free`, linked-list →
//     `bz_llist_free`, `OwningBuffer` → `bz_buffer_free`).
//
//   * EXCLUDED HERE, admitted there — `OwningTask`. Codegen's membership test
//     ends in an open-ended `common::isOwningPrefixedName(collName)` fallback,
//     which matches `OwningTask` and routes a `Task<OwningTask<U>>` result into
//     the array-backed free path. That contradicts codegen's OWN comment
//     twelve lines further down (`isTaskTypeName(tElem)` → "a nested Task-
//     family result is BORROWED, not OWNED ... disposing here is a double-free
//     / UAF"), which is unreachable precisely because the collection branch
//     runs first. This header encodes the documented INTENT, so sema does not
//     start rejecting nested-task shapes on the strength of a codegen defect.
//     Filed separately rather than fixed here — #3638's lane does not own
//     codegen.
//
//   * EXCLUDED HERE, excluded there — `FrozenArray` (§4.19). It is in
//     `BuiltinRegistry`'s `kArrayBackedCollections`, so reusing
//     `isArrayBackedCollectionName` would have admitted it, but codegen's list
//     does NOT (it is neither `Owning*`-prefixed nor one of
//     Array/Stack/Queue/Set/SortedSet), so codegen frees nothing for a
//     `Task<FrozenArray<T>>`. Admitting it in sema alone would be a pure
//     false-reject with no disposal to guard. That asymmetry is exactly why
//     this is a closed table rather than a composition of the three existing
//     registry predicates.
//
//   * Nothing previously admitted by `taskResultArgIsClass` is removed: the
//     new test is OR-ed in beside the class-declaration check and the existing
//     `Owning*`-collection / sync-primitive allowlists.
//
// per §30.5 / §28.3 / stdlib §4.4–§4.9, §4.14, §4.17, §4.18, v0.286
constexpr std::array<std::string_view, 15> kPatternAResultCollectionHeads = {
    // bz_array_t*-backed (§4.4, §4.5, §4.10, §4.11, §4.18)
    "Array", "OwningArray", "Stack", "OwningStack", "Queue", "OwningQueue",
    "Set", "OwningSet", "SortedSet",
    // bz_dict_t*-backed (§4.8, §4.17)
    "Dictionary", "OwningDictionary", "SortedDictionary",
    // bz_llist_t*-backed (§4.6, §4.7)
    "LinkedList", "OwningLinkedList",
    // manual-storage primitive (§4.14) — bz_buffer_free, no element disposer
    "OwningBuffer",
};

// True when `head` is one of the intrinsic-collection heads above. Pure
// membership over a literal table, like every other `BuiltinRegistry`
// predicate. // per §30.5 / §28.3, #3638
inline bool isPatternAResultCollectionHead(std::string_view head) {
    return std::find(kPatternAResultCollectionHeads.begin(),
                     kPatternAResultCollectionHeads.end(),
                     head) != kPatternAResultCollectionHeads.end();
}

}  // namespace bezel::common
