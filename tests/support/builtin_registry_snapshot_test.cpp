// #1414 Stage 3 — behaviour-preservation snapshot for the data-only builtin
// registry (src/common/BuiltinRegistry.{hpp,cpp}).
//
// The registry is a refactor: each of its tables replaces a hand-written
// string-comparison chain that previously lived inline in sema / ownership /
// codegen.  The refactor's whole value depends on it being behaviour-
// preserving — NO name may enter or leave any set.  These tests pin every
// table against the literal name set spelled out at the pre-refactor site, so
// an accidental add/drop/rename is caught mechanically rather than surfacing as
// a codegen truncation or a missed disposal in the field (the #1390 / #1406 /
// v0.168-rename bug family this stage exists to kill).
//
// Each `kSnapshot*` array below pins a registry table's CURRENT membership;
// the negatives are near-neighbours from adjacent sets that must stay out, plus
// — since the R27 gate cycle — every name a later pick REMOVED, each annotated
// with the pick that removed it.  If a real stdlib addition changes a set,
// update BOTH the registry table and the matching snapshot here in the same
// commit — that is the point.

#include "test_harness.hpp"

#include "common/BuiltinRegistry.hpp"

#include <array>
#include <string_view>

using namespace bezel::common;

// A snapshot row is a bare builtin name, but `BEZEL_EXPECT_TRUE` reports only
// the source LINE — so a name that had left its registry set surfaced as
// "assertion failed: isHeapPointerReferenceBuiltin(n) (...:517)", with nothing
// naming WHICH builtin had drifted and no way to tell a 73-row set's one
// changed row from a wholesale table corruption. This wrapper evaluates the
// membership claim ONCE, names the offending row, and fails the test exactly
// as the bare macro does. It adds no tolerance: the claim itself is still
// spelled out by hand, both directions, on every row.
//
// A REGISTRY CHANGE IS A TEST CHANGE. Every pick that adds, drops or renames a
// name in src/common/BuiltinRegistry.cpp must edit the matching `kSnapshot*`
// in the SAME commit — and a name that is REMOVED is pinned as an explicit
// negative, naming the pick that removed it, rather than deleted. A deletion
// makes the test pass and leaves nothing recording the decision: the next
// pick to re-add the name is caught by the negative, not by a green test that
// says the name was never there. That asymmetry is the whole reason the four
// sets below drifted (three picks, one gate cycle) while every assertion in
// this file was still "correct" against its own stale array.
#define SNAPSHOT_MEMBERSHIP(pred, n)                                            \
    do {                                                                       \
        if (!(pred(n))) {                                                      \
            ::bezel::test::currentTestFailed() = true;                         \
            std::cout << "    snapshot row drifted: " #pred "(\"" << (n)      \
                      << "\") is false\n";                                     \
        }                                                                      \
    } while (false)

// The negative direction, for the same reason: a name that has wrongly JOINED a
// set is as much a drift as one that has wrongly left it, and the two must be
// reported the same legibly.
#define SNAPSHOT_NON_MEMBERSHIP(pred, n)                                        \
    do {                                                                       \
        if ((pred(n))) {                                                       \
            ::bezel::test::currentTestFailed() = true;                         \
            std::cout << "    snapshot row drifted: " #pred "(\"" << (n)      \
                      << "\") is true but must stay out\n";                    \
        }                                                                      \
    } while (false)

// --- isOwningCollectionHead (§28.6/§5.11) : the element-owning collections ---
BEZEL_TEST(builtin_registry_snapshot, owning_collection_head_set) {
    static constexpr std::array<std::string_view, 6> kSnapshot = {
        "OwningArray", "OwningDictionary", "OwningSet",
        "OwningStack", "OwningQueue",      "OwningLinkedList",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isOwningCollectionHead(n));
    // Owning WRAPPERS are NOT collections; borrowing collections are NOT owning.
    for (std::string_view n : {"Shared", "Weak", "ChannelSender", "OwningTask",
                               "IOwningEnumerable", "CancellationTokenSource",
                               "Array", "Dictionary", "OwningBuffer", ""})
        BEZEL_EXPECT_FALSE(isOwningCollectionHead(n));
}

// --- isOwningTypeHead (§28.3/§5.11) : owning collections + owning wrappers ---
BEZEL_TEST(builtin_registry_snapshot, owning_type_head_set) {
    static constexpr std::array<std::string_view, 15> kSnapshot = {
        "OwningArray", "OwningDictionary", "OwningSet",
        "OwningStack", "OwningQueue",      "OwningLinkedList",
        "Shared",      "Weak",             "ChannelSender",
        "OwningTask",  "IOwningEnumerable", "CancellationTokenSource",
        // INTENTIONAL addition (#2369, §28.6/§28.3 v0.221 #1787 ruling): the
        // three remaining owning iterable-interface heads — `checkBorrowReturned
        // ThroughOwningType` never inspected them, so a borrow returned through
        // `IOwningEnumerator<T>`/`IOwningAsyncEnumerable<T>`/
        // `IOwningAsyncEnumerator<T>` sailed past the §28.6 return-shape check.
        "IOwningEnumerator", "IOwningAsyncEnumerable", "IOwningAsyncEnumerator",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isOwningTypeHead(n));
    for (std::string_view n : {"Array", "Dictionary", "OwningBuffer",
                               "OwningTaskCompletionSource", "Task",
                               "IEnumerable", "IEnumerator",
                               "IAsyncEnumerable", "IAsyncEnumerator", ""})
        BEZEL_EXPECT_FALSE(isOwningTypeHead(n));
}

// --- isOwningIterableInterfaceHead (§28.6/§28.3 v0.221, #1787 ruling; #2369):
// the four consumer-disposed owning iterable-interface heads whose receiver-
// owned-field bare-read carve-out §28.6 bullet 1 explicitly excludes. A new
// name-set fact introduced with #2369 (no pre-refactor hand-list), pinned here
// so an accidental add/drop is caught. ---
BEZEL_TEST(builtin_registry_snapshot, owning_iterable_interface_head_set) {
    static constexpr std::array<std::string_view, 4> kSnapshot = {
        "IOwningEnumerable", "IOwningEnumerator",
        "IOwningAsyncEnumerable", "IOwningAsyncEnumerator",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isOwningIterableInterfaceHead(n));
    // The non-owning iterable heads, the task family, and the owning
    // collections/wrappers must stay OUT (different carve-out surfaces).
    for (std::string_view n : {"IEnumerable", "IEnumerator",
                               "IAsyncEnumerable", "IAsyncEnumerator",
                               "Task", "OwningTask", "OwningArray",
                               "Shared", "ChannelSender", ""})
        BEZEL_EXPECT_FALSE(isOwningIterableInterfaceHead(n));
}

// --- isOwningPrefixedName : the 16 rfind("Owning",0) sites, as a prefix test ---
BEZEL_TEST(builtin_registry_snapshot, owning_prefix_predicate) {
    // Behaviour is EXACTLY head.rfind("Owning",0)==0 — a prefix, not a set.
    for (std::string_view n : {"Owning", "OwningArray", "OwningBuffer",
                               "OwningTask", "OwningTaskCompletionSource",
                               "OwningWhatever"})
        BEZEL_EXPECT_TRUE(isOwningPrefixedName(n));
    for (std::string_view n : {"Array", "IOwningList", "Own", "owning",
                               "MyOwning", ""})
        BEZEL_EXPECT_FALSE(isOwningPrefixedName(n));
}

// --- isIterFormatEnumerableHead / isSyncIterFormatEnumerableHead (#3419,
//     §29.2, stdlib §4.12–§4.13): the enumerable-wrap grid's DECLARED-HEAD
//     axis. NOT a migration snapshot — these tables are new with #3419, which
//     consolidated 24 hand-written spellings of them. The pins are therefore
//     about MEMBERSHIP, and the negatives carry the load: the four ENUMERATOR
//     heads must stay OUT (an enumerator is what a wrap produces, not what one
//     is created for), and the richer list/collection interfaces must stay out
//     (they keep the array-backed protocol and take no wrap at all — wrapping
//     one is #2815's SIGSEGV in reverse).
BEZEL_TEST(builtin_registry_snapshot, iter_format_enumerable_head_set) {
    static constexpr std::array<std::string_view, 4> kSnapshot = {
        "IEnumerable", "IOwningEnumerable",
        "IAsyncEnumerable", "IOwningAsyncEnumerable",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isIterFormatEnumerableHead(n));
    for (std::string_view n : {"IEnumerator", "IOwningEnumerator",
                               "IAsyncEnumerator", "IOwningAsyncEnumerator",
                               "IReadOnlyCollection", "IReadOnlyList",
                               "ICollection", "IList", "IOwningCollection",
                               "IOwningList", "IGrouping", "Array", ""})
        BEZEL_EXPECT_FALSE(isIterFormatEnumerableHead(n));
    // It is exactly the ENUMERABLE half of the eight-name §28.3 announced set.
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isIterableInterfaceHead(n));
}

BEZEL_TEST(builtin_registry_snapshot, sync_iter_format_enumerable_head_set) {
    static constexpr std::array<std::string_view, 2> kSnapshot = {
        "IEnumerable", "IOwningEnumerable",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isSyncIterFormatEnumerableHead(n));
    // The ASYNC twins must stay out: they take the `await for` desugaring and
    // its own enumerator acquisition (§30.7), so a site that admitted all four
    // would hand an async source a synchronous enumerator.
    for (std::string_view n : {"IAsyncEnumerable", "IOwningAsyncEnumerable",
                               "IEnumerator", "IOwningEnumerator",
                               "IReadOnlyList", "IList", "Array", ""})
        BEZEL_EXPECT_FALSE(isSyncIterFormatEnumerableHead(n));
    // The sync pair is a subset of the four-name set.
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isIterFormatEnumerableHead(n));
}

// --- isIterFormatEnumeratorHead (#3419, §29.2): the ENUMERATOR-format axis,
//     the complement of isIterFormatEnumerableHead within the eight iterator/
//     enumerator heads. NOT a migration snapshot — new with this lane's
//     #3419 residue pass, which retired eight independent two-name copies in
//     LlvmBackend_Calls2.cpp (MoveNext/TakeCurrent/MoveNextAsync/explicit-
//     Dispose dispatch and the declaration-less §8.3 identity-Equals/
//     GetHashCode fallback).
BEZEL_TEST(builtin_registry_snapshot, iter_format_enumerator_head_set) {
    static constexpr std::array<std::string_view, 4> kSnapshot = {
        "IEnumerator", "IOwningEnumerator",
        "IAsyncEnumerator", "IOwningAsyncEnumerator",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isIterFormatEnumeratorHead(n));
    // The four ENUMERABLE heads (the sibling axis) and the surrounding
    // non-iterator heads must stay out.
    for (std::string_view n : {"IEnumerable", "IOwningEnumerable",
                               "IAsyncEnumerable", "IOwningAsyncEnumerable",
                               "IReadOnlyCollection", "IReadOnlyList",
                               "ICollection", "IList", "IOwningCollection",
                               "IOwningList", "IGrouping", "Array", ""})
        BEZEL_EXPECT_FALSE(isIterFormatEnumeratorHead(n));
    // It is exactly the ENUMERATOR half of the eight-name §28.3 announced set,
    // and disjoint from isIterFormatEnumerableHead (the two axes partition
    // the eight-name set).
    for (auto n : kSnapshot) {
        BEZEL_EXPECT_TRUE(isIterableInterfaceHead(n));
        BEZEL_EXPECT_FALSE(isIterFormatEnumerableHead(n));
    }
}

BEZEL_TEST(builtin_registry_snapshot, sync_iter_format_enumerator_head_set) {
    static constexpr std::array<std::string_view, 2> kSnapshot = {
        "IEnumerator", "IOwningEnumerator",
    };
    for (auto n : kSnapshot)
        BEZEL_EXPECT_TRUE(isSyncIterFormatEnumeratorHead(n));
    // The ASYNC twins must stay out: they take the MoveNextAsync vtable
    // shape and its own dispatch (LlvmBackend_Async.cpp), a disjoint
    // lowering path.
    for (std::string_view n : {"IAsyncEnumerator", "IOwningAsyncEnumerator",
                               "IEnumerable", "IOwningEnumerable",
                               "IReadOnlyList", "IList", "Array", ""})
        BEZEL_EXPECT_FALSE(isSyncIterFormatEnumeratorHead(n));
    // The sync pair is a subset of the four-name enumerator-format set.
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isIterFormatEnumeratorHead(n));
}

// --- isSyncOwningIterableInterfaceHead (#3606, §29.1/§29.3): the SYNCHRONOUS
//     half of isOwningIterableInterfaceHead — the two owning iterator-method
//     RETURN heads (producer side), a different §29 family from the
//     isSyncIterFormatEnumerableHead pair above (the §29.2 for-in SOURCE
//     protocol, consumer side; see #3606's issue body on why these are not
//     unified together). NOT a migration snapshot — new with #3606, which
//     retired the two hand-written `retName == "IOwningEnumerable" ||
//     retName == "IOwningEnumerator"` copies in LlvmBackend_Iterators.cpp.
//     The negatives carry the load: the two ASYNC owning heads must stay OUT
//     (they take `await for` / the async iterator emitter in
//     LlvmBackend_Async.cpp, a disjoint lowering path) and the two non-owning
//     iterable heads must stay OUT (they keep the borrowing/non-destructive
//     Current contract).
BEZEL_TEST(builtin_registry_snapshot, sync_owning_iterable_interface_head_set) {
    static constexpr std::array<std::string_view, 2> kSnapshot = {
        "IOwningEnumerable", "IOwningEnumerator",
    };
    for (auto n : kSnapshot)
        BEZEL_EXPECT_TRUE(isSyncOwningIterableInterfaceHead(n));
    // The async owning twins and the two non-owning iterable heads stay out.
    for (std::string_view n : {"IOwningAsyncEnumerable", "IOwningAsyncEnumerator",
                               "IEnumerable", "IEnumerator",
                               "IAsyncEnumerable", "IAsyncEnumerator",
                               "Task", "OwningTask", "OwningArray",
                               "Shared", "ChannelSender", ""})
        BEZEL_EXPECT_FALSE(isSyncOwningIterableInterfaceHead(n));
    // The sync-owning pair is a subset of the four-name owning-iterable set.
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isOwningIterableInterfaceHead(n));
}

// --- isIteratorFormatDispatchHead (#3419, §29.2 / §29.11 / §28.6): the SIX
//     iterator-format heads the §29.2 SYNC `using var` admission list
//     recognises — all four iterator-format ENUMERABLE heads (sync + async,
//     owning + non-owning) plus the two SYNC ENUMERATOR heads. The two ASYNC
//     enumerator heads are deliberately excluded (they take the `await using`
//     path).
//
//     This predicate is the SIXTH spelling of the enumerable-wrap grid (#3419),
//     but unlike the five prior point repairs it is the SHARED authority:
//     `LlvmBackend_Setup.cpp`'s `using var` admission reads it instead of
//     carrying its own six-name copy. The snapshot below pins the membership
//     so a future head addition or removal is caught here, not by an
//     off-by-one leak in the field (#3392, #3418).
//
//     NOT a migration snapshot — new with #3419. The pin that carries the
//     load is the asymmetry around the two async enumerator heads (the
//     `await using` route) and the surrounding non-iterator heads (the
//     richer read/collection interfaces take the BUILTIN for-in desugaring,
//     not the `using var` admission).
BEZEL_TEST(builtin_registry_snapshot, iterator_format_dispatch_head_set) {
    static constexpr std::array<std::string_view, 6> kSnapshot = {
        "IEnumerable", "IOwningEnumerable",
        "IEnumerator", "IOwningEnumerator",
        "IAsyncEnumerable", "IOwningAsyncEnumerable",
    };
    for (auto n : kSnapshot)
        BEZEL_EXPECT_TRUE(isIteratorFormatDispatchHead(n));
    // The two ASYNC enumerator heads stay out - they take `await using`.
    for (std::string_view n : {"IAsyncEnumerator", "IOwningAsyncEnumerator"})
        BEZEL_EXPECT_FALSE(isIteratorFormatDispatchHead(n));
    // Non-iterator heads (the richer read/collection interfaces, the task
    // family, the collection family) stay out - they reach a different
    // admission gate (`isBuiltinForInEnumerableHead` for the for-in
    // desugaring, `isArrayEnumeratorWrapSourceName` for the source family,
    // etc.).
    for (std::string_view n : {"IReadOnlyCollection", "IReadOnlyList",
                               "ICollection", "IList",
                               "IOwningCollection", "IOwningList",
                               "IGrouping",
                               "Array", "OwningArray", "OwningBuffer",
                               "Stack", "OwningStack", "Queue", "OwningQueue",
                               "LinkedList", "OwningLinkedList",
                               "Set", "OwningSet", "SortedSet",
                               "Dictionary", "OwningDictionary", "SortedDictionary",
                               "Task", "OwningTask", ""})
        BEZEL_EXPECT_FALSE(isIteratorFormatDispatchHead(n));
    // The four iterator-format enumerable heads are the wrap-creator axes;
    // the sync iter-format enumerable pair is the §29.2 for-in source.
    for (auto n : kSnapshot) {
        if (std::string_view(n) != "IEnumerator" &&
            std::string_view(n) != "IOwningEnumerator") {
            BEZEL_EXPECT_TRUE(isIterFormatEnumerableHead(n));
        }
    }
    for (std::string_view n : {"IEnumerable", "IOwningEnumerable"})
        BEZEL_EXPECT_TRUE(isSyncIterFormatEnumerableHead(n));
}

// --- isBuiltinForInEnumerableHead (#3468, §29.2, stdlib §4.12–§4.13): the
//     family gate of the builtin `for-in` desugaring. NOT a migration snapshot
//     — the table is new with #3468, which retired the two hand-written copies
//     of it (the lowering and its self-declared "mirror", which had drifted).
//     The pins that carry the load are the ASYMMETRIES: it is a strict SUPERSET
//     of the sync iterator-format pair (the four richer read/collection
//     interfaces reach the same desugaring but keep the ARRAY protocol), and it
//     excludes the ASYNC heads (they take `await for`) and every ENUMERATOR
//     head (an enumerator is not an enumerable).
BEZEL_TEST(builtin_registry_snapshot, builtin_for_in_enumerable_head_set) {
    static constexpr std::array<std::string_view, 6> kSnapshot = {
        "IEnumerable", "IOwningEnumerable",
        "IReadOnlyCollection", "IReadOnlyList", "ICollection", "IList",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isBuiltinForInEnumerableHead(n));
    for (std::string_view n : {"IAsyncEnumerable", "IOwningAsyncEnumerable",
                               "IEnumerator", "IOwningEnumerator",
                               "IAsyncEnumerator", "IOwningAsyncEnumerator",
                               "IOwningCollection", "IOwningList",
                               "IGrouping", "Array", "OwningArray",
                               "LinkedList", "Set", ""})
        BEZEL_EXPECT_FALSE(isBuiltinForInEnumerableHead(n));
    // Strict superset of the sync iterator-format pair: those two are the ones
    // whose runtime value is a vtable-backed enumerator; the other four in this
    // set reach the same handler and stay on the array protocol.
    for (std::string_view n : {"IEnumerable", "IOwningEnumerable"}) {
        BEZEL_EXPECT_TRUE(isSyncIterFormatEnumerableHead(n));
        BEZEL_EXPECT_TRUE(isBuiltinForInEnumerableHead(n));
    }
    for (std::string_view n : {"IReadOnlyCollection", "IReadOnlyList",
                               "ICollection", "IList"}) {
        BEZEL_EXPECT_TRUE(isBuiltinForInEnumerableHead(n));
        BEZEL_EXPECT_FALSE(isSyncIterFormatEnumerableHead(n));
    }
}

// --- isArrayEnumeratorWrapSourceName (#3419, §29.2, stdlib §4.4–§4.5/§4.11/
//     §4.14): the grid's SOURCE-FAMILY axis. The negatives are the whole point
//     of the predicate existing separately from isArrayBackedCollectionName:
//     `Set`/`OwningSet` over a value element are a generic-key HASH table that
//     is SNAPSHOTTED rather than wrapped, `SortedSet` is excluded from the wrap
//     outright (the snapshot is in hash order and would break stdlib §4.18's
//     ordered-iteration contract), and `FrozenArray` has no `new` form to
//     reach the coercion with. Both predicates answer truthfully about
//     overlapping name sets; the DIFFERENCE is asserted here so a later lane
//     cannot "simplify" one into the other.
BEZEL_TEST(builtin_registry_snapshot, array_enumerator_wrap_source_set) {
    static constexpr std::array<std::string_view, 7> kSnapshot = {
        "Array", "OwningArray", "OwningBuffer",
        "Stack", "OwningStack", "Queue", "OwningQueue",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isArrayEnumeratorWrapSourceName(n));
    for (std::string_view n : {"Set", "OwningSet", "SortedSet", "FrozenArray",
                               "LinkedList", "OwningLinkedList", "Dictionary",
                               "OwningDictionary", "SortedDictionary",
                               "IEnumerable", ""})
        BEZEL_EXPECT_FALSE(isArrayEnumeratorWrapSourceName(n));
    // Every wrap source IS array-backed, but not every array-backed collection
    // is a wrap source — the containment is one-way, and that asymmetry is the
    // reason both predicates exist.
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isArrayBackedCollectionName(n));
    for (std::string_view n : {"Set", "OwningSet", "SortedSet", "FrozenArray"}) {
        BEZEL_EXPECT_TRUE(isArrayBackedCollectionName(n));
        BEZEL_EXPECT_FALSE(isArrayEnumeratorWrapSourceName(n));
    }
}

// --- isArrayBackedCollectionName (§4.4–§4.7/§4.14) : isCollectionTypeName ---
BEZEL_TEST(builtin_registry_snapshot, array_backed_collection_set) {
    static constexpr std::array<std::string_view, 11> kSnapshot = {
        "Array",  "OwningArray", "OwningBuffer",
        "Stack",  "OwningStack", "Queue",
        "OwningQueue", "Set",    "OwningSet",
        // INTENTIONAL addition (stdlib §4.18, #1426): SortedSet joined the
        // collection-dispatch gate — it was MISSING pre-#1426 (i32-typed,
        // lowerNew no-op), the 4th missed-name-list instance.
        "SortedSet",
        // INTENTIONAL addition (fix #3249, stdlib §4.19, v0.186): FrozenArray<T>
        // — the deeply-immutable shared-read primitive shares the bz_array_t*
        // rep with Array<T> (the same `Length`/indexer/`ToArray`/destructor
        // runtime functions; the only differences are the absence of mutating
        // members and the §22.12-suppression-set membership).
        "FrozenArray",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isArrayBackedCollectionName(n));
    // LinkedList (bz_llist_t) and Dictionary (bz_dict_t) are NOT array-backed.
    for (std::string_view n : {"LinkedList", "OwningLinkedList", "Dictionary",
                               "OwningDictionary", "SortedDictionary",
                               "Buffer", ""})
        BEZEL_EXPECT_FALSE(isArrayBackedCollectionName(n));
}

// --- isDictionaryName (§4.8/§4.17) : isDictTypeName ---
BEZEL_TEST(builtin_registry_snapshot, dictionary_set) {
    static constexpr std::array<std::string_view, 3> kSnapshot = {
        "Dictionary", "OwningDictionary", "SortedDictionary",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isDictionaryName(n));
    for (std::string_view n : {"Array", "Set", "OwningSet", "SortedSet",
                               "Map", ""})
        BEZEL_EXPECT_FALSE(isDictionaryName(n));
}

// --- isLinkedListName (§4.6/§4.7) : isLinkedListTypeName ---
BEZEL_TEST(builtin_registry_snapshot, linked_list_set) {
    static constexpr std::array<std::string_view, 2> kSnapshot = {
        "LinkedList", "OwningLinkedList",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isLinkedListName(n));
    for (std::string_view n : {"LinkedListNode", "Array", "OwningArray", ""})
        BEZEL_EXPECT_FALSE(isLinkedListName(n));
}

// --- isFfiBufferBuiltinName (#2809 / §32.5 / stdlib §10.1–§10.3): the three
// FFI-boundary BUFFER classes, the set every §10 member handler must recognise
// BEFORE the declaration-less `.Length` STRING fallback claims the receiver.
// A name MISSING here is a `%bz.string` field-1 read off a raw malloc'd buffer
// (#2809's ASan heap-buffer-overflow), so the set's completeness is
// load-bearing in the same way isReservedBuiltinTypeName's is. The FFI VALUE
// types IntPtr/UIntPtr are §32.7 pointer-sized integers, not buffers, and are
// negatives here on purpose. ---
BEZEL_TEST(builtin_registry_snapshot, ffi_buffer_builtin_set) {
    static constexpr std::array<std::string_view, 3> kSnapshot = {
        "CString", "WString", "RawBytes",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isFfiBufferBuiltinName(n));
    for (std::string_view n : {"IntPtr", "UIntPtr", "Ptr", "string", "String",
                               "StringBuilder", "Buffer", "OwningBuffer",
                               "Array", ""})
        BEZEL_EXPECT_FALSE(isFfiBufferBuiltinName(n));
}

// --- isNoOverrideIdentityBuiltinName (#4126, stdlib §6.1/§8.1): the two
// builtin heap-pointer CLASSES (Stopwatch, CancellationToken) sharing the
// FFI-buffer trio's "no vtable, no dedicated Object-contract interceptor"
// shape but kept as their own registry row (see BuiltinRegistry.cpp for
// why a shared boolean with kFfiBufferBuiltins would misstate the two
// families as interchangeable). ---
BEZEL_TEST(builtin_registry_snapshot, no_override_identity_builtin_set) {
    static constexpr std::array<std::string_view, 2> kSnapshot = {
        "Stopwatch", "CancellationToken",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isNoOverrideIdentityBuiltinName(n));
    for (std::string_view n : {"CString", "WString", "RawBytes", "Guid",
                               "Range", "DateTime", "TimeSpan",
                               "CancellationTokenSource", "Random", ""})
        BEZEL_EXPECT_FALSE(isNoOverrideIdentityBuiltinName(n));
}

// --- isIterableInterfaceHead (§28.3/§29.3/§30.7 v0.201, #2048 / proposal 0048):
// the iterable/enumerator interface heads whose unmarked return type is the
// iterator convention's announced caller-owned frame. A static signature
// returning one of these is in the announced set of the §28.3 declaration-site
// static-return error. ---
BEZEL_TEST(builtin_registry_snapshot, iterable_interface_head_set) {
    static constexpr std::array<std::string_view, 8> kSnapshot = {
        "IEnumerable", "IEnumerator",
        "IAsyncEnumerable", "IAsyncEnumerator",
        "IOwningEnumerable", "IOwningEnumerator",
        "IOwningAsyncEnumerable", "IOwningAsyncEnumerator",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isIterableInterfaceHead(n));
    // The near-neighbours that must stay OUT: the task family (a different
    // carve), the non-deferred collection interfaces, the collection concrete
    // types, and the lazy-query sentinel is structural (not name-keyed).
    for (std::string_view n : {"Task", "OwningTask", "ICollection", "IList",
                               "IOwningCollection", "IOwningList",
                               "IReadOnlyCollection", "IReadOnlyList",
                               "Array", "OwningArray", "IEnumerable<int>", ""})
        BEZEL_EXPECT_FALSE(isIterableInterfaceHead(n));
}

// --- isHeapPointerReferenceBuiltin : the shared core of BOTH Layout rep lists.
// This is the intersection of the two pre-refactor hand-lists in
// LlvmBackend_Layout.cpp; OwningBuffer (astNode-only) and the FFI class names
// (imported-only) are deliberately NOT in the shared set — they are the small
// site-local deltas the two call sites still test explicitly.
BEZEL_TEST(builtin_registry_snapshot, heap_pointer_reference_builtin_set) {
    static constexpr std::array<std::string_view, 73> kSnapshot = {
        "Array", "OwningArray", "Dictionary", "OwningDictionary",
        // INTENTIONAL addition (stdlib §4.18, #1426): SortedSet — bz_dict_t*
        // rep on both the astNode and imported paths.
        "SortedDictionary", "SortedSet", "Stack", "OwningStack", "Queue", "OwningQueue",
        "Set", "OwningSet", "LinkedList", "OwningLinkedList", "LinkedListNode",
        // INTENTIONAL addition (fix #3249, stdlib §4.19, v0.186): FrozenArray<T>
        // — heap-allocated via the §4.19 `From` overloads; same bz_array_t*
        // rep on both rep paths.
        "FrozenArray",
        // INTENTIONAL addition (fix #1740, v0.254): OwningBuffer<T> is a
        // heap-owning collection (ptr rep on the astNode path; identical role
        // to OwningArray<T>). Historically the BuiltinRegistry separated it
        // into kNonHeapReservedBuiltins with a comment claiming it was still a
        // heap ptr — the EFFECT was that producerState(new OwningBuffer)
        // classified the producer as Borrowed (a value type), so `var buf :=
        // new OwningBuffer<double>(4);` initialized `buf` in Borrowed state
        // and the §28.3 consume-site freeze misclassifed the subsequent
        // `owned buf` as a borrow and silently dropped the move. OwningArray
        // is in the heap table, so OwningBuffer belongs alongside it.
        "OwningBuffer",
        "StringBuilder", "Random",
        // INTENTIONAL addition (stdlib §8.1, #2025): Stopwatch — bz_stopwatch_t*
        // heap rep; without it, the generic `new T()` path returns ptr while the
        // type lowered to i32, IR-verifying as a return-type mismatch.
        "Stopwatch",
        // INTENTIONAL addition (stdlib §7.2, #645): FileStream — bz_filestream_t*
        // heap rep, same reason as Stopwatch directly above.
        "FileStream",
        "IEnumerable", "IEnumerator", "IOwningEnumerable", "IOwningEnumerator",
        "IAsyncEnumerable", "IAsyncEnumerator", "IOwningAsyncEnumerable",
        "IOwningAsyncEnumerator", "IReadOnlyCollection", "IReadOnlyList",
        "ICollection", "IList", "IOwningCollection", "IOwningList",
        "Task", "OwningTask",
        "Subscription", "CancellationToken", "CancellationTokenSource",
        "TaskScope",
        "Shared", "Weak", "Channel", "ChannelSender", "IGrouping",
        // INTENTIONAL addition (issue #4178): IOrderedEnumerable<T> —
        // same bz_lazy_t* ptr rep as IEnumerable/IGrouping above; without
        // it, a function whose declared return type is an explicit
        // `IOrderedEnumerable<T>` annotation IR-verify-fails ("Function
        // return type does not match operand type of return inst!").
        "IOrderedEnumerable",
        // INTENTIONAL addition (stdlib §6.5, fix #1536): EventSource<T> /
        // AsyncEventSource<T> — the producer-side event face (§24.3), same
        // ptr rep as IEvent<T>/IAsyncEvent<T>.
        "IEvent", "IAsyncEvent", "EventSource", "AsyncEventSource",
        // INTENTIONAL addition (stdlib §6.5, fix #1832, v0.187):
        // ISharedEvent<T> / SharedEventSource<T> — the cross-domain
        // publication-point pair, same ptr rep as the instance pair. The
        // cross-domain synchronization discipline lives at the runtime
        // entry point (bz_shared_event_source_*); type-name registration
        // here just admits the names.
        "ISharedEvent", "SharedEventSource",
        "Mutex", "Lock", "Semaphore", "SemaphoreToken",
        "TaskCompletionSource", "OwningTaskCompletionSource",
        // fix #5390 (§2.3, v0.314) renamed the builtin `Buffer<T>` to the
        // internal-only `__Buffer<T>`: §2.3's always-available table does not
        // list Buffer, so no stdlib module declares it and `builtinTypeArity`
        // answered 1 for an impl-ahead-of-spec intrinsic. The heap row is the
        // same bz_array_t* rep either way, so the row is RENAMED, not dropped.
        "Atomic", "AtomicInteger", "__Buffer",
        "ISerializer", "IDeserializer", "ISerializable", "IDeserializable",
        "BinarySerializer", "BinaryDeserializer", "JsonSerializer",
        "JsonDeserializer",
        // INTENTIONAL addition (fix #4326, stdlib §2.10/§2.18, §28.2/
        // §28.12/§18.5): IDisposable, IAsyncDisposable, IFormattable — three
        // more null-declaration builtin interfaces sharing IOrderedEnumerable's
        // exact shape (directly above): naming one as a parameter or return
        // type IR-verifier-crashed because lowerTypeRef's Named case fell
        // through to the i32 default instead of ptr.
        "IDisposable", "IAsyncDisposable", "IFormattable",
    };
    for (auto n : kSnapshot) SNAPSHOT_MEMBERSHIP(isHeapPointerReferenceBuiltin, n);
    // The site-local deltas must NOT be in the shared set. (OwningBuffer was
    // historically here; fix #1740 promoted it to the heap table since it IS a
    // heap-owning collection — see the OwningArray twin directly above.)
    for (std::string_view n : {"IntPtr", "UIntPtr", "CString",
                               "WString", "RawBytes"})
        BEZEL_EXPECT_FALSE(isHeapPointerReferenceBuiltin(n));
    // Value-type / span / temporal builtins are NOT heap-pointer references.
    for (std::string_view n : {"DateTime", "TimeSpan", "Guid", "Range", "Span",
                               "ReadOnlySpan", "ArraySpan", "ReadOnlyStringSpan",
                               "JsonOptions", "Boolean", ""})
        BEZEL_EXPECT_FALSE(isHeapPointerReferenceBuiltin(n));
    // REMOVED by fix #5390 (§2.3, v0.314) — pinned rather than deleted. The
    // user-spellable `Buffer` is no longer a builtin at all: `builtinTypeArity`
    // answers kNotBuiltin for it and `Buffer<int>` in user source is an
    // unresolved type, so it cannot lower to the shared ptr rep either. Only
    // the internal-only `__Buffer` spelling (pinned above) is a member. Without
    // this row a later pick could re-add `Buffer` and the positive-only loop
    // would stay green, which is precisely how the three R27 picks that broke
    // this file were able to land unnoticed.
    for (std::string_view n : {"Buffer"})
        BEZEL_EXPECT_FALSE(isHeapPointerReferenceBuiltin(n));
    // Every heap row is a RESERVED name (the heap half IS the first clause of
    // `isReservedBuiltinTypeName`) — the reverse half of the derivation the
    // non-heap snapshot checks below, so the two halves cannot drift apart.
    for (auto n : kSnapshot) SNAPSHOT_MEMBERSHIP(isReservedBuiltinTypeName, n);
}

// --- isReservedBuiltinTypeName (#2757 / §7.1 / §14.1): the COMPLETE reserved
// builtin type-name set — the heap table above plus the non-heap Named-case
// names. `Emitter::isBuiltinTypeRefName` used to keep a hand-maintained
// duplicate of this set and it DRIFTED (59 names against the registry's 65), so
// a plain `Stopwatch?` was read as a nullable generic type PARAMETER and took
// the {i8, i32} placeholder — a `bz_stopwatch_t*` passed as `i32`. That is the
// third instance of the miss-a-list class (#1390, #1406) the registry exists to
// end, so the duplicate is gone and the predicate derives from here.
//
// The test is necessarily NEGATIVE at its call sites ("not a builtin name,
// therefore a type parameter" — a parameter is defined by the absence of a
// declaration, so there is no positive parameter registry to test against),
// which makes COMPLETENESS load-bearing rather than merely tidy: a name missing
// here is silently misread as a type parameter at all three consumers. Pinned
// in both directions — every name in, every near-neighbour out.
BEZEL_TEST(builtin_registry_snapshot, reserved_builtin_type_name_set) {
    // (a) the non-heap half: the names lowerTypeRef's Named case compares that
    // are NOT heap-pointer references. Exactly `kNonHeapReservedBuiltins`.
    // (OwningBuffer was here before fix #1740; it IS a heap-owning collection
    // and is now in the heap table, so it is reserved via the heap half, not
    // this half.)
    static constexpr std::array<std::string_view, 18> kNonHeapSnapshot = {
        // §32.5 / §32.7 — FFI boundary
        "IntPtr", "UIntPtr", "CString", "WString", "RawBytes",
        // §2.5 — temporal / identity value builtins
        "DateTime", "TimeSpan", "DateOnly", "TimeOnly", "Guid",
        // §9.8 (#499)
        "Range",
        // §4.14 / §4.15 (#549/#779) / §3.3 (#883) — the {ptr,i32,i32} spans
        "ArraySpan", "ReadOnlyArraySpan", "Span", "ReadOnlySpan",
        "ReadOnlyStringSpan",
        // §2.15 (#687)
        "JsonOptions",
        // §31.4 — the Exception.CauseChain element record
        "ExceptionInfo",
    };
    for (auto n : kNonHeapSnapshot) {
        SNAPSHOT_MEMBERSHIP(isReservedBuiltinTypeName, n);
        // ...and each is genuinely OUTSIDE the heap table (no redundancy: if one
        // of these were also in the heap set, the split would be meaningless).
        BEZEL_EXPECT_FALSE(isHeapPointerReferenceBuiltin(n));
    }
    // (b) the heap half is reserved too — spot-check the names whose absence
    // from the deleted duplicate WAS the defect, plus the long-standing ones.
    for (std::string_view n : {"Stopwatch", "FileStream", "TaskScope",
                               "SortedDictionary", "SortedSet",
                               "IEvent", "IAsyncEvent", "ISharedEvent",
                               "EventSource", "AsyncEventSource",
                               "SharedEventSource",
                               "ISerializer",
                               "IDeserializer", "ISerializable",
                               "IDeserializable", "BinarySerializer",
                               "BinaryDeserializer", "JsonSerializer",
                               "JsonDeserializer",
                               "Array", "Dictionary", "Task", "Shared",
                               "Mutex", "__Buffer", "IGrouping",
                               // #4178: IOrderedEnumerable<T> joined the heap
                               // table alongside its sibling IGrouping above.
                               "IOrderedEnumerable"})
        BEZEL_EXPECT_TRUE(isReservedBuiltinTypeName(n));
    // (c) OUT: ordinary generic type-parameter spellings must stay classifiable
    // as parameters — this is the whole point of the negative test.
    for (std::string_view n : {"T", "U", "K", "V", "TKey", "TResult",
                               "Self", "MyClass", "Stopwatch2", "stopwatch",
                               ""})
        BEZEL_EXPECT_FALSE(isReservedBuiltinTypeName(n));
    // (d) OUT by deliberate exclusion: `DictionaryEntry` / `DrainedEntry` are
    // compared in the Named case, but only on a SYNTHETIC TypeRef (no astNode,
    // importedName-keyed, genericArgs.size() >= 2), and neither resolves as a
    // written type annotation at all ("unresolved type name" — measured). No
    // declaration-less single-part Named ref can carry them, so listing them
    // would change only the generic-argument inference binding, to no end.
    for (std::string_view n : {"DictionaryEntry", "DrainedEntry"})
        BEZEL_EXPECT_FALSE(isReservedBuiltinTypeName(n));
    // (e) OUT: built-in EXCEPTION names are not in the registry's reserved set —
    // codegen tests those separately against its live `builtinVtable_` map,
    // which is not a duplicated name list and so cannot drift from a table.
    for (std::string_view n : {"Exception", "ArgumentException", "IOException"})
        BEZEL_EXPECT_FALSE(isReservedBuiltinTypeName(n));
    // (f) OUT since fix #5390 (§2.3, v0.314): the user-spellable `Buffer` is
    // no longer a builtin type name — §2.3's always-available table does not
    // list it, so it is reserved only under the internal-only `__Buffer`
    // spelling (pinned in (b) above). Pinned as a negative so the rename is a
    // recorded fact rather than a silent disappearance from (b).
    for (std::string_view n : {"Buffer"})
        BEZEL_EXPECT_FALSE(isReservedBuiltinTypeName(n));
    // (g) The reserved set is exactly the DISJOINT union of the two halves, so
    // the reserved set is DERIVED from them rather than hand-kept a third time.
    // Both directions are load-bearing and both are pinned:
    //   - (a) already asserts every non-heap row is reserved and NOT a heap
    //     pointer — i.e. nonHeap ∩ heap = ∅, which is the whole disjointness
    //     claim (it is symmetric, so no second direction is needed here);
    //   - the heap snapshot's own test asserts every one of its 73 rows is
    //     reserved, i.e. heap ⊆ reserved.
    // What neither can give is the reverse — a name reserved through NEITHER
    // half is invisible to a positive-only snapshot. See the note on
    // `SNAPSHOT_MEMBERSHIP` at the top of this file: closing that gap needs an
    // enumeration API on BuiltinRegistry.hpp, which is a compiler change and
    // not a test change.
}

// --- isVtableLessBuiltinClassName (#2921 / #2923; spec §8.1 / §8.5 / §28.2 /
// §28.9, v0.240): the reserved built-in CLASSES — every reserved name that is
// NOT an interface head.
//
// WHY IT IS A DERIVATION, NOT A LIST. Two lowerings assume the receiver carries
// a Bezel vtable at word 0 and are memory-unsafe without one: the slot-2 Dispose
// dispatch (`emitDynamicDispose`) jumped to address 0 on a `StringBuilder`
// (#2921, MEASURED `mov (%rax),%rcx ; call *0x10(%rcx)`), and
// `lowerObjectToString`'s `bz_obj_is_string` — a NEGATIVE test, "word 0 is not
// one of the emitted vtables ⇒ it is a string" — mis-answered "string" for every
// built-in and got each one retained + released as a `%bz.string` (#2923, a
// double free). Both now gate on this ONE predicate.
//
// The set is `isReservedBuiltinTypeName` MINUS the interface heads, and the
// direction matters: a built-in INTERFACE head is the one reserved name whose
// runtime value may be a USER class (`var e: IEnumerable<int> := new MyList();`
// — measured to compile and run), so those must keep the dynamic dispatch. A
// newly-added built-in CLASS therefore joins this set automatically and cannot
// land back in the wild jump, while a forgotten new built-in INTERFACE costs a
// leak or a type-name rendering. Pinned in both directions below.
BEZEL_TEST(builtin_registry_snapshot, vtable_less_builtin_class_name_set) {
    // (a) IN: concrete built-in CLASSES across every family — collections,
    // §3 nullary heap classes, sync primitives, tasks/TCS, event sources,
    // Shared/Weak, serializer handles, and the non-heap reserved names.
    for (std::string_view n : {"Array", "OwningArray", "Dictionary",
                               "OwningDictionary", "SortedDictionary",
                               "SortedSet", "Stack", "Queue", "Set",
                               "LinkedList", "LinkedListNode",
                               // fix #3249 (§4.19, v0.186): FrozenArray<T> —
                               // heap-allocated, no vtable; the same vtable-less
                               // treatment as Array<T> on the bz_array_t* rep.
                               "FrozenArray",
                               "StringBuilder", "Random", "Stopwatch",
                               "FileStream", "Task", "OwningTask",
                               "Subscription", "CancellationToken",
                               "CancellationTokenSource", "TaskScope",
                               "Shared", "Weak", "Channel", "ChannelSender",
                               "EventSource", "AsyncEventSource",
                               // fix #1832 (stdlib §6.5, v0.187):
                               // SharedEventSource<T> — concrete class head,
                               // not an interface; same vtable-less treatment
                               // as the instance pair (heap-allocated, no
                               // vtable, freed via bz_shared_event_source_free).
                               "SharedEventSource",
                               "Mutex", "Lock", "Semaphore", "SemaphoreToken",
                               "TaskCompletionSource",
                               "OwningTaskCompletionSource",
                               // fix #5390 (§2.3, v0.314): `Buffer` renamed to
                               // the internal-only `__Buffer`; same bz_array_t*
                               // rep, so the same vtable-less treatment. The
                               // user-spellable `Buffer` is pinned OUT in (e).
                               "Atomic", "AtomicInteger", "__Buffer",
                               "OwningBuffer", "BinarySerializer",
                               "BinaryDeserializer", "JsonSerializer",
                               "JsonDeserializer", "CString", "WString",
                               "RawBytes"})
        SNAPSHOT_MEMBERSHIP(isVtableLessBuiltinClassName, n);
    // (b) OUT: every reserved INTERFACE head — the receivers that may hold a
    // user class and must keep dispatching through its vtable. This is the
    // whole content of the subtraction, so it is pinned name-for-name.
    static constexpr std::array<std::string_view, 25> kInterfaceHeads = {
        "IEnumerable", "IEnumerator", "IOwningEnumerable", "IOwningEnumerator",
        "IAsyncEnumerable", "IAsyncEnumerator", "IOwningAsyncEnumerable",
        "IOwningAsyncEnumerator", "IReadOnlyCollection", "IReadOnlyList",
        "ICollection", "IList", "IOwningCollection", "IOwningList",
        "IEvent", "IAsyncEvent", "ISharedEvent", "IGrouping",
        "ISerializer", "IDeserializer", "ISerializable", "IDeserializable",
        // INTENTIONAL addition (fix #4326): IDisposable, IAsyncDisposable,
        // IFormattable — the necessary companion of that fix's
        // kHeapPointerReferenceBuiltins addition; see BuiltinRegistry.cpp's
        // comment on kBuiltinInterfaceHeads for why leaving these out would
        // have made isVtableLessBuiltinClassName wrongly true for them.
        "IDisposable", "IAsyncDisposable", "IFormattable",
    };
    for (auto n : kInterfaceHeads) {
        BEZEL_EXPECT_FALSE(isVtableLessBuiltinClassName(n));
        // ...and each really is a reserved name (so the exclusion is a genuine
        // subtraction, not a name that was never in the source set).
        BEZEL_EXPECT_TRUE(isReservedBuiltinTypeName(n));
    }
    // (c) OUT: a generic type-PARAMETER spelling and a user type name — the
    // other inhabitants of the declaration-less Named shape, which must keep
    // dispatching dynamically.
    for (std::string_view n : {"T", "U", "TKey", "MyClass", "Stopwatch2",
                               "stringbuilder", ""})
        BEZEL_EXPECT_FALSE(isVtableLessBuiltinClassName(n));
    // (d) OUT: built-in EXCEPTIONS. They DO carry a vtable (codegen emits one
    // per built-in exception type), so they must not be routed away from the
    // dispatch — and they are outside `isReservedBuiltinTypeName` for exactly
    // that reason, which this pins.
    for (std::string_view n : {"Exception", "ArgumentException", "IOException",
                               "DisposedAccessException"})
        BEZEL_EXPECT_FALSE(isVtableLessBuiltinClassName(n));
    // (e) OUT since fix #5390 (§2.3, v0.314): the user-spellable `Buffer` is
    // no longer a builtin type name, so it is not a reserved name and cannot
    // be a reserved built-in class. Pinned as a negative so the rename is
    // recorded rather than being a silent drop from (a) above.
    for (std::string_view n : {"Buffer"})
        BEZEL_EXPECT_FALSE(isVtableLessBuiltinClassName(n));
}

// --- isBuiltinInterfaceHead (#2969; spec §8.1 / §8.5 / §18.5 / §28.9, v0.240):
// the COMPLEMENT of `isVtableLessBuiltinClassName` within the reserved-name set
// — the reserved built-in INTERFACE heads, exposed as a predicate of their own
// for the §2969 codegen routing (`tryBuiltinIfaceHeadRender`,
// `LlvmBackend_String.cpp`). Pinned name-for-name so an accidental add/drop
// is caught mechanically — the predicate is the one table the §2969 routing
// reads, so any drift between this snapshot and the `kBuiltinInterfaceHeads`
// array it mirrors is the regression shape.
BEZEL_TEST(builtin_registry_snapshot, builtin_interface_head_set_8_1) {
    static constexpr std::array<std::string_view, 25> kInterfaceHeads = {
        "IEnumerable", "IEnumerator", "IOwningEnumerable", "IOwningEnumerator",
        "IAsyncEnumerable", "IAsyncEnumerator", "IOwningAsyncEnumerable",
        "IOwningAsyncEnumerator", "IReadOnlyCollection", "IReadOnlyList",
        "ICollection", "IList", "IOwningCollection", "IOwningList",
        "IEvent", "IAsyncEvent", "ISharedEvent", "IGrouping",
        "ISerializer", "IDeserializer", "ISerializable", "IDeserializable",
        // INTENTIONAL addition (fix #4326) — see the companion snapshot
        // above (vtable_less_builtin_class_name_set) for the full rationale.
        "IDisposable", "IAsyncDisposable", "IFormattable",
    };
    for (auto n : kInterfaceHeads) {
        BEZEL_EXPECT_TRUE(isBuiltinInterfaceHead(n));
        // Every name is also reserved — the §2969 routing only fires on the
        // declaration-less-Named-shape subset, and that subset IS the reserved
        // name set. This guards against a future "interface head that's not
        // reserved" silently bypassing the discriminator.
        BEZEL_EXPECT_TRUE(isReservedBuiltinTypeName(n));
        // Every name is EXCLUDED from `isVtableLessBuiltinClassName` — the
        // whole content of the §2923/§2975 split. The complement predicate
        // here exists precisely because the vtable-less set excludes these;
        // pinning it makes the symmetry mechanical.
        BEZEL_EXPECT_FALSE(isVtableLessBuiltinClassName(n));
    }
    // (c) OUT: a generic type-PARAMETER spelling and a user type name — the
    // other inhabitants of the declaration-less Named shape.
    for (std::string_view n : {"T", "U", "TKey", "MyClass", "Stopwatch2",
                               "stringbuilder", ""})
        BEZEL_EXPECT_FALSE(isBuiltinInterfaceHead(n));
    // (d) OUT: every reserved built-in CLASS — they belong to the vtable-less
    // half of the split, NOT the interface-head half. Listing them here is
    // the more thorough end of the symmetry pin (above we checked every
    // interface head against this predicate; here we walk the other way).
    for (std::string_view n : {"Array", "OwningArray", "Dictionary",
                               "OwningDictionary", "SortedDictionary",
                               "SortedSet", "Stack", "Queue", "Set",
                               "LinkedList", "LinkedListNode",
                               // fix #3249 (§4.19, v0.186): FrozenArray<T> —
                               // reserved built-in CLASS (not interface head);
                               // it sits in the vtable-less half of the split.
                               "FrozenArray",
                               "StringBuilder", "Random", "Stopwatch",
                               "FileStream", "Task", "OwningTask",
                               "Subscription", "CancellationToken",
                               "CancellationTokenSource", "TaskScope",
                               "Shared", "Weak", "Channel", "ChannelSender",
                               "EventSource", "AsyncEventSource",
                               // fix #1832 (stdlib §6.5, v0.187):
                               // SharedEventSource<T> — class head, NOT an
                               // interface head (the interface in this pair
                               // is ISharedEvent<T>).
                               "SharedEventSource",
                               "Mutex", "Lock", "Semaphore", "SemaphoreToken",
                               "TaskCompletionSource",
                               "OwningTaskCompletionSource",
                               // fix #5390 (§2.3, v0.314): `Buffer` -> `__Buffer`.
                               "Atomic", "AtomicInteger", "__Buffer",
                               "OwningBuffer", "BinarySerializer",
                               "BinaryDeserializer", "JsonSerializer",
                               "JsonDeserializer", "CString", "WString",
                               "RawBytes"})
        BEZEL_EXPECT_FALSE(isBuiltinInterfaceHead(n));
}

// --- §22.16 (v0.203, #2053): ContractViolationException registration ---
// Per spec §22.16 "Failure semantics and the enforcement modes",
// ContractViolationException joins the built-in hierarchy DERIVING DIRECTLY
// FROM Exception (clause evaluation is a language operation, like the
// indexer's IndexOutOfRangeException). Verified here at the data-only
// registry tier — the runtime lowering that actually raises it lands in
// a later slice; this snapshot pins the registration so an accidental
// drop/reparent is caught mechanically.
BEZEL_TEST(builtin_registry_snapshot, contract_violation_exception_registered_spec_22_16) {
    BEZEL_EXPECT_TRUE(isBuiltinExceptionName("ContractViolationException"));
    BEZEL_EXPECT_EQ(builtinExceptionParentOf("ContractViolationException"),
                    std::string_view("Exception"));
    // Sanity: the well-known hierarchy is unchanged around the new entry.
    BEZEL_EXPECT_TRUE(isBuiltinExceptionName("Exception"));
    BEZEL_EXPECT_EQ(builtinExceptionParentOf("Exception"), std::string_view(""));
    BEZEL_EXPECT_EQ(builtinExceptionParentOf("ArgumentException"),
                    std::string_view("Exception"));
    BEZEL_EXPECT_EQ(builtinExceptionParentOf("IndexOutOfRangeException"),
                    std::string_view("Exception"));
}

// fix #2992 (§28.3 / §28.9 / §28.10, v0.240): the disposable-builtin-head set
// — the table the §28.10 vacuity lint (rhsIsDisposableClassType Path 2) and the
// codegen auto-drop chain's end-of-chain guard (registerUsingDisposal) now
// SHARE. Pinning the exact membership here means a change to either site's
// behaviour is caught mechanically: adding a name to the table without a
// chain arm (or vice versa) is the defect class #2992 makes loud.
BEZEL_TEST(builtin_registry_snapshot, disposable_builtin_head_name_set_spec_28_3_28_9) {
    // IN: every name in kDisposableBuiltinHeadNames — the lint's pre-#2992 Path 2
    // population, lifted verbatim. A `using var` of any of these is the canonical
    // §28.10 pattern (not vacuous), and the auto-drop chain MUST claim each one.
    // #2269 (cycle-3) removed StringBuilder from kDisposableBuiltinHeadNames
    // because §28.9 L7363 classifies it as TRIVIAL-DISPOSE — no chain claim
    // needed. The string is documented in the OUT-list below.
    for (std::string_view n : {
        "BinarySerializer", "BinaryDeserializer",
        "JsonSerializer", "JsonDeserializer",
        "CString", "WString", "RawBytes",
        "FileStream",
        "IEnumerable", "IEnumerator",
        "IOwningEnumerable", "IOwningEnumerator",
        "IAsyncEnumerable", "IAsyncEnumerator",
        "IOwningAsyncEnumerable", "IOwningAsyncEnumerator",
        "Channel", "ChannelSender",
        "EventSource", "AsyncEventSource",
        // fix #1832 (stdlib §6.5, v0.187): SharedEventSource<T> — same
        // §28.9 vtable-less reclaim discipline as the instance sources.
        "SharedEventSource",
        "Subscription",
        "TaskScope", "CancellationTokenSource",
        "Mutex", "Lock", "Semaphore", "SemaphoreToken",
        "OwningTask", "Task", "Shared", "Weak",
        "OwningArray", "OwningDictionary", "OwningSet", "OwningStack",
        "OwningQueue", "OwningLinkedList", "OwningBuffer",
        "TaskCompletionSource", "OwningTaskCompletionSource",
    })
        BEZEL_EXPECT_TRUE(isDisposableBuiltinHeadName(n));

    // OUT: value builtins with no disposal obligation — a `using var` of these
    // IS vacuous (§28.9), so the lint fires (correctly) and the chain registers
    // nothing (there is nothing to release).
    for (std::string_view n : {
        "Guid", "DateTime", "TimeSpan", "DateOnly", "TimeOnly",
        "Range", "CancellationToken", "JsonOptions",
        "IntPtr", "UIntPtr",
    })
        BEZEL_EXPECT_FALSE(isDisposableBuiltinHeadName(n));

    // OUT (measured disagreement, filed separately): the borrowing collections
    // and four vtable-less builtins ARE chain-claimed (they own heap) but are
    // intentionally NOT in this table — #2695 excluded the borrowing collections
    // so the lint fires on a bound-then-pinned alias whose `const` remedy
    // compiles. The four vtable-less builtins (Random/Atomic/AtomicInteger/
    // Stopwatch/Buffer) share the same gap on factory-call forms. Pinning them
    // OUT here documents the disagreement; reconciling it is a separate decision.
    for (std::string_view n : {
        "Array", "Stack", "Queue", "Set", "SortedSet",
        "Dictionary", "SortedDictionary", "LinkedList",
        "Random", "Atomic", "AtomicInteger", "Stopwatch", "Buffer",
    })
        BEZEL_EXPECT_FALSE(isDisposableBuiltinHeadName(n));
}

// fix #3119 (spec §8.5, v0.252; stdlib §2.13 rule 1): the qualified module
// path every reserved built-in class renders under. Pinned here so an
// accidental add/drop/rename is caught mechanically. The 14 keyword-aliased
// primitives (§3.4) and the built-in INTERFACE heads (§2.13 rule 1
// deliberately does not name them) stay OUT — they render lowercase or
// not at all, respectively.
BEZEL_TEST(builtin_registry_snapshot, builtin_class_module_path_set) {
    static constexpr std::array<std::string_view, 85> kSnapshot = {
        // fix #5186 (spec §8.1, v0.311; stdlib §2.13): the §3.4 reserved
        // keyword primitives whose module is `System`. These two rows
        // mirror the chain-head resolution the table enables
        // (`System.Object`, `System.Type`); without them `var o:
        // System.Object := …` (a §2.13 rule 1 / §8.1 spelling) would
        // reach `resolveMemberAccess`'s chain-head stamp and the
        // primitive-typed read would find no `builtinClassModulePathOf`
        // entry to render the qualified `System.Object` argument name.
        "Object", "Type",
        // §2 System — §2.5/§2.6 value-builtins whose box thunk renders the
        // type-name literal (fix #3119 (a)).
        "DateTime", "TimeSpan", "DateOnly", "TimeOnly", "Guid",
        // fix #4120 (stdlib §10.1/§10.2/§10.3, v0.295): the FFI-boundary
        // buffer classes — §10's preamble states they live in `System`.
        "CString", "WString", "RawBytes",
        // §2.17 — built-in EXCEPTIONS, all in `System`.
        "Exception", "ArgumentException", "ArgumentNullException",
        "ArgumentOutOfRangeException", "ArithmeticException", "OverflowException",
        "DivideByZeroException", "InvalidOperationException",
        "DisposedAccessException", "NotImplementedException",
        "NullReferenceException", "IndexOutOfRangeException",
        "KeyNotFoundException", "MatchFailureException",
        "OperationCanceledException", "TypeInitializationException",
        "FormatException", "StackOverflowException", "ChannelClosedException",
        "ChannelFullException", "DomainFaultedException",
        "SerializationException", "TimeoutException", "IOException",
        "FileNotFoundException", "DirectoryNotFoundException",
        "EndOfStreamException", "PathTooLongException",
        "ContractViolationException",
        // §2.16 — Shared/Weak.
        "Shared", "Weak",
        // §2.7 — Random.
        "Random",
        // §3 System.Text — StringBuilder.
        "StringBuilder",
        // §4 System.Collections.Generic — collections.
        "Array", "OwningArray", "Stack", "OwningStack", "Queue", "OwningQueue",
        "Set", "OwningSet", "SortedSet", "Dictionary", "OwningDictionary",
        "SortedDictionary", "LinkedList", "OwningLinkedList",
        "LinkedListNode", "OwningBuffer",
        // fix #3249 (§4.19, v0.186): FrozenArray<T> — System.Collections.Generic.
        "FrozenArray",
        // §6 System.Threading — sync, atomics, channels, nursery, etc.
        "Mutex", "Lock", "Semaphore", "SemaphoreToken", "Atomic",
        "AtomicInteger", "TaskCompletionSource", "OwningTaskCompletionSource",
        // fix #6079 (§2.3 L161, v0.338): Task — the Task family's home module.
        "Task",
        "TaskScope", "CancellationToken", "CancellationTokenSource",
        "Channel", "ChannelSender", "Domain",
        // §7 System.IO — FileStream.
        "FileStream",
        // §8 System.Diagnostics — Stopwatch.
        "Stopwatch",
        // §2.15 — concrete serializers.
        "BinarySerializer", "BinaryDeserializer", "JsonSerializer",
        "JsonDeserializer",
        // §24.3 — event sources.
        "EventSource", "AsyncEventSource",
        // fix #1832 (stdlib §6.5, v0.187): SharedEventSource<T> —
        // cross-domain publication point. Same module as the instance pair.
        "SharedEventSource",
        // §2.11 — Subscription.
        "Subscription",
    };
    for (auto n : kSnapshot)
        BEZEL_EXPECT_TRUE(builtinClassModulePathOf(n).size() > 0);
    // Keyword aliases and interface heads deliberately OUT — see comment
    // above and the stdlib §2.13 rule 1 + rule 2 contract.
    for (std::string_view n : {
        "bool", "byte", "sbyte", "short", "ushort", "int", "uint", "long",
        "ulong", "float", "double", "decimal", "char", "string",
        "IEnumerable", "IEnumerator", "ICollection", "IList",
        "ISerializer", "IDeserializer", "IEvent", "IAsyncEvent",
        // fix #1832: ISharedEvent<T> is an interface head (§2.13 rule 1).
        "ISharedEvent",
    })
        BEZEL_EXPECT_TRUE(builtinClassModulePathOf(n).empty());
}

// fix #5177 (stdlib §2.13/§2.17, §4.8, §6, §7.5, v0.310): ten built-in
// exceptions are declared OUTSIDE `System` and must render their actual
// declaring module, not the uniform "System.<Name>" the table wrongly gave
// every exception before this fix. `builtin_class_module_path_set` above
// only asserts non-emptiness, so it could not have caught this — pin the
// exact values here.
BEZEL_TEST(builtin_registry_snapshot, builtin_exception_module_path_non_system_5177) {
    // stdlib §4.8: declared in System.Collections.Generic.
    BEZEL_EXPECT_EQ(builtinClassModulePathOf("KeyNotFoundException"),
                     std::string_view("System.Collections.Generic"));
    // stdlib §6: declared in System.Threading.
    for (std::string_view n : {"ChannelClosedException", "ChannelFullException",
                               "DomainFaultedException", "TimeoutException"})
        BEZEL_EXPECT_EQ(builtinClassModulePathOf(n),
                         std::string_view("System.Threading"));
    // stdlib §7.5: declared in System.IO.
    for (std::string_view n : {"IOException", "FileNotFoundException",
                               "DirectoryNotFoundException",
                               "EndOfStreamException", "PathTooLongException"})
        BEZEL_EXPECT_EQ(builtinClassModulePathOf(n), std::string_view("System.IO"));
    // Sanity: a genuinely System-declared exception is unaffected.
    BEZEL_EXPECT_EQ(builtinClassModulePathOf("ArgumentException"),
                     std::string_view("System"));
}

// fix #3119 (spec §3.4; stdlib §2.13 rule 2, v0.252): the 14 keyword-aliased
// built-in types (spec §3.4). Per stdlib §2.13 rule 2 they render lowercase
// in BOTH `Name` and `FullName`. Pinned here so a missed entry surfaces as
// a test failure rather than a silently-qualified rendering.
BEZEL_TEST(builtin_registry_snapshot, keyword_alias_builtin_set) {
    static constexpr std::array<std::string_view, 14> kSnapshot = {
        "bool", "byte", "sbyte", "short", "ushort", "int", "uint",
        "long", "ulong", "float", "double", "decimal", "char", "string",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isKeywordAliasBuiltinName(n));
    // Near-neighbours that must stay OUT: the reserved PascalCase aliases
    // (`Boolean`, `Int32`, …), the non-keyword classes (`int` ≠ `Integer`),
    // and any non-keyword name.
    for (std::string_view n : {"Boolean", "Int32", "String", "Integer",
                               "int32", "object", "Int", "Char", ""})
        BEZEL_EXPECT_FALSE(isKeywordAliasBuiltinName(n));
}

// --- isStringLinqOperatorMemberName (#3589, §5/§9.6): the string-receiver
//     LINQ exemption gate. NOT a migration snapshot in the usual sense — the
//     table collapses two hand-spellings a #3589 measurement found were
//     byte-identical (the `.` member-access gate and its `?.` mirror), so
//     this pins the union those two already agreed on. fix #3636 RETIRED
//     `ForEach` from this gate: instance `.ForEach(...)` is NOT a stdlib §5
//     member (the only documented `ForEach` is the unrelated static
//     `Parallel.ForEach`), the codegen arm emitted malformed IR (LLVM
//     verifier: "Instruction has a name, but provides a void value!"), and
//     the builtin-collection twin table (`kBuiltinCollectionLinqOperatorNames`,
//     see below) already excluded `ForEach` — so the two tables now agree.
//     fix #3786 ADDED `MinOrNull`/`MaxOrNull` (stdlib §5.1): issue #3444's
//     widening question for these two names is resolved — the
//     string-receiver materialize path (`isStringRecvTerm`, part of the same
//     `lazyTerminal` disjunction #3764 widened) was proven end-to-end to
//     compose with the #2783 array arm, and a genuine codegen bug in the
//     `elemTypeRef`/`isValTy` derivation for a string receiver (found while
//     proving it) was fixed alongside. `ElementAtOrNull`/`OfType` remain
//     OUT — `ElementAtOrNull` has no lowering on ANY receiver (a separate,
//     larger gap: #3444/#3764), and `OfType` was removed from the stdlib
//     entirely at v0.284 (issue #3559) — #3786 does not touch either.
BEZEL_TEST(builtin_registry_snapshot, string_linq_operator_member_name_set) {
    static constexpr std::array<std::string_view, 39> kSnapshot = {
        "Where", "Select", "SelectMany", "Skip", "Take", "TakeWhile",
        "SkipWhile", "Distinct", "OrderBy", "OrderByDescending", "ThenBy",
        "ThenByDescending", "Concat", "Zip", "Union", "Intersect", "Except",
        "Reverse", "Any", "All", "Count", "Sum", "Min", "Max", "Average",
        "First", "Last", "Single", "ElementAt", "FirstOrNull", "LastOrNull",
        "SingleOrNull", "ToArray", "ToSet", "ToDictionary", "GroupBy",
        "Aggregate", "MinOrNull", "MaxOrNull",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isStringLinqOperatorMemberName(n));
    // Real stdlib §5/§5.1 names deliberately absent from THIS table (see
    // above: `ElementAtOrNull`/`OfType` stay gated on purpose), plus
    // `ForEach` retired by #3636 (not a stdlib §5 instance member — only the
    // static `Parallel.ForEach` exists), plus a retired/never-real name and a
    // near-neighbour typo.
    for (std::string_view n : {"ElementAtOrNull",
                               "Contains", "OfType", "ToList", "Sorted",
                               "Reversed", "ForEach", ""})
        BEZEL_EXPECT_FALSE(isStringLinqOperatorMemberName(n));
}

// --- isBuiltinCollectionLinqOperatorName (#3589, §5): the builtin non-owning-
//     collection-family bare-reference / member-access LINQ exemption gate.
//     Collapses four hand-spellings this fix found were meant to be one
//     question (three byte-identical, the fourth missing only `ToArray`,
//     masked by that family's own separate `isDeclaredMethod` admitting it —
//     see the header comment for the full #3589 reasoning). `Reverse`/
//     `Count`/`Contains` stay OUT on purpose: each builtin family pre-empts
//     them through its own dedicated handler before this gate runs.
BEZEL_TEST(builtin_registry_snapshot,
           builtin_collection_linq_operator_name_set) {
    static constexpr std::array<std::string_view, 38> kSnapshot = {
        "Where", "OfType", "Select", "SelectMany", "Any", "All", "First",
        "FirstOrNull", "Last", "LastOrNull", "Single", "SingleOrNull",
        "ElementAt", "Sum", "Min", "Max", "MinOrNull", "MaxOrNull",
        "Average", "Aggregate", "OrderBy", "OrderByDescending", "Distinct",
        "Union", "Intersect", "Except", "Take", "TakeWhile", "Skip",
        "SkipWhile", "Concat", "Zip", "GroupBy", "ToArray", "ToDictionary",
        "ToSet", "ThenBy", "ThenByDescending",
    };
    for (auto n : kSnapshot)
        BEZEL_EXPECT_TRUE(isBuiltinCollectionLinqOperatorName(n));
    // Pre-empted-elsewhere exclusions (see header) plus fictitious names.
    for (std::string_view n : {"Reverse", "Count", "Contains", "ElementAtOrNull",
                               "ToList", "Append", "SequenceEqual", "Join",
                               "AsReadOnly", "ForEach", ""})
        BEZEL_EXPECT_FALSE(isBuiltinCollectionLinqOperatorName(n));
}

// --- isBuiltinIntLengthPropertyHead (fix #4766 / #4833, §5.1, v0.307): the
//     builtin heap-type heads that declare `readonly property Length : int`
//     and that the narrow #4766 typing arm in `ExprTypecheck_Calls.cpp`
//     `resolveMemberAccess` answers for. Transcribed verbatim from the inline
//     name list the #4833 consolidation removed; the negatives are heads that
//     the UPPER arm at ~20867 handles (they declare `Count`, not `Length`),
//     plus the builtins the property-type axis intentionally excludes (Channel,
//     the §2.x value records, the task family, the FFI/value types).
//
//     SHRUNK from 9 rows to 3 by two R27 picks, both spec rulings that stand:
//       - fix #4811 (ba980cfa, stdlib §4.4, v0.315) removed Array/OwningArray.
//         The Count-vs-Length convention (stdlib §4.4 p.74) gives resizable
//         Array<T> `Count` ONLY — "which is why the resizable Array<T> says
//         Count where .NET's fixed array says Length" — so `Array<T>.Length` is
//         an error, not a typed read.
//       - fix #5416 (393cd85f, stdlib §4.11, v0.315) removed Stack/OwningStack
//         and Queue/OwningQueue for the same reason: those four declare `Count`
//         (via IReadOnlyCollection<T>), not `Length`.
//     Both groups MOVED rather than vanished: they are still builtin collection
//     heads, and `.Count` on them is typed by the UPPER arm — which is why they
//     are pinned as negatives below instead of being dropped, and why they also
//     appear as negatives in `isBuiltinIntCountPropertyHead` below. What is left
//     is the three heads that genuinely declare `Length`. ---
BEZEL_TEST(builtin_registry_snapshot, builtin_int_length_property_head_set) {
    static constexpr std::array<std::string_view, 3> kSnapshot = {
        // §3.1 StringBuilder
        "StringBuilder",
        // §4.14 OwningBuffer<T>
        "OwningBuffer",
        // §4.19 FrozenArray<T> (annotation shape only)
        "FrozenArray",
    };
    for (auto n : kSnapshot)
        SNAPSHOT_MEMBERSHIP(isBuiltinIntLengthPropertyHead, n);
    // Heads that declare `Count` (not `Length`) — handled by the upper arm
    // and/or by `isBuiltinIntCountPropertyHead` below — must stay OUT.
    for (std::string_view n : {"Set", "OwningSet", "LinkedList",
                               "OwningLinkedList", "Dictionary",
                               "OwningDictionary", "SortedDictionary",
                               "SortedSet", "Channel", "CString", "WString",
                               "RawBytes", "string", "int", "Task", ""})
        BEZEL_EXPECT_FALSE(isBuiltinIntLengthPropertyHead(n));
    // MOVED OUT, pinned as negatives (see the header):
    //   - fix #4811 (ba980cfa, stdlib §4.4, v0.315) — Array<T> is resizable
    //     and says `Count`, never `Length`.
    //   - fix #5416 (393cd85f, stdlib §4.11, v0.315) — Stack<T>/Queue<T> and
    //     their Owning* forms declare `Count` via IReadOnlyCollection<T>.
    // These are the two picks that made `Array<T>.Length` / `Stack<T>.Length` /
    // `Queue<T>.Length` compile errors, so the registry must NOT grow a `Length`
    // arm for them again without a spec change that reverses the ruling.
    for (std::string_view n : {"Array", "OwningArray", "Stack", "OwningStack",
                               "Queue", "OwningQueue"})
        BEZEL_EXPECT_FALSE(isBuiltinIntLengthPropertyHead(n));
    // DERIVED, not hand-listed: `Length` and `Count` are the two answers to one
    // question, and a builtin head declares exactly one of them. Asserting the
    // two snapshots are disjoint turns that into a mechanical fact — a later
    // pick that adds a head to one table while editing only that table's
    // snapshot is caught here, at the pair, rather than by whichever test
    // happens to mention the name.
    static constexpr std::array<std::string_view, 3> kCountHeads = {
        "SortedDictionary", "SortedSet", "Channel",
    };
    for (auto n : kSnapshot) SNAPSHOT_NON_MEMBERSHIP(isBuiltinIntCountPropertyHead, n);
    for (auto n : kCountHeads) SNAPSHOT_NON_MEMBERSHIP(isBuiltinIntLengthPropertyHead, n);
    // ...and each `Count` row is a real member, so the exclusion above is a
    // genuine two-way split and not a pair of empty lists.
    for (auto n : kCountHeads) SNAPSHOT_MEMBERSHIP(isBuiltinIntCountPropertyHead, n);
}

// --- isBuiltinIntCountPropertyHead (fix #4766 / #4833, §5.1, v0.307): the
//     builtin heap-type heads that declare `readonly property Count : int`
//     but that the upper ExprTypecheck_Calls.cpp `Count` arm (~20867) does
//     NOT cover (which handles Array/OwningArray/Stack/OwningStack/Queue/
//     OwningQueue/Set/OwningSet/LinkedList/OwningLinkedList/Dictionary/
//     OwningDictionary/OwningBuffer). Transcribed verbatim from the inline
//     name list #4833 removed; the negatives are heads that declare `Length`
//     (not `Count`), plus the heads the upper arm handles. ---
BEZEL_TEST(builtin_registry_snapshot, builtin_int_count_property_head_set) {
    static constexpr std::array<std::string_view, 3> kSnapshot = {
        "SortedDictionary", "SortedSet", "Channel",
    };
    for (auto n : kSnapshot)
        BEZEL_EXPECT_TRUE(isBuiltinIntCountPropertyHead(n));
    // Heads handled by the upper arm (declare `Count` and are typed there),
    // heads that declare `Length` instead of `Count`, and assorted non-
    // collections.
    for (std::string_view n : {"Array", "OwningArray", "Stack", "OwningStack",
                               "Queue", "OwningQueue", "Set", "OwningSet",
                               "LinkedList", "OwningLinkedList", "Dictionary",
                               "OwningDictionary", "OwningBuffer",
                               "StringBuilder", "FrozenArray",
                               "CString", "WString", "RawBytes",
                               "string", "int", "Task", ""})
        BEZEL_EXPECT_FALSE(isBuiltinIntCountPropertyHead(n));
}

// --- The two COMPOSITE head sets #6312's `checkAssignable` arm derives, pinned
// as unions. fix-forward #6312 (§5.1, §4.6, §12.9, v0.338): that arm spelled
// its sixteen built-in collection heads and fourteen iterator / collection-
// interface heads as local `toH == "..."` chains, which is what
// scripts/check_enumerable_grid_spellings.py (#3419) exists to catch. It now
// reads them off the shared classifier as a UNION of the tables pinned above.
//
// The five component snapshots pin the PARTS. They do NOT pin the union: each
// one's negatives are a handful of near-neighbours, so adding a fresh head to
// `kArrayBackedCollections` / `kDictionaryNames` / `kLinkedListNames` /
// `kIterableInterfaceHeads` / `kBuiltinForInEnumerableHeads` would leave all
// five green while SILENTLY WIDENING the #6312 arm — which rejects the
// conversion, and so would start rejecting legal programs. The mirror failure
// (a DROP in any of the five) narrows the arm and re-opens the IR-verifier
// hole `Take(3)` against `Take(x: Array<int>)` came through. Both directions
// are pinned here.
//
// A name may be an INTERFACE head, a collection head, or neither; no name is
// both, so the two unions are disjoint and together name every built-in
// container head #6312 rejects a primitive at. The SPAN heads (`Span`,
// `ReadOnlySpan`, `ArraySpan`, `ReadOnlyArraySpan`, stdlib §4.14–§4.15) are
// deliberately absent from BOTH: a span is no wrap source and no
// iterator-format head, so no predicate owns them, and #6312's arm spells
// them locally.
BEZEL_TEST(builtin_registry_snapshot,
           composite_container_head_unions_cover_6312_builtin_targets) {
    // The sixteen built-in collection CLASS heads (stdlib §4.4–§4.8, §4.14,
    // §4.17–§4.19) — the three codegen dispatch partitions, which between them
    // name every collection.
    static constexpr std::array<std::string_view, 16> kCollections = {
        "Array",        "Dictionary",   "Set",           "Stack",
        "Queue",        "LinkedList",   "SortedDictionary", "SortedSet",
        "OwningArray",  "OwningDictionary", "OwningSet",  "OwningStack",
        "OwningQueue",  "OwningLinkedList", "OwningBuffer", "FrozenArray",
    };
    for (auto n : kCollections)
        BEZEL_EXPECT_TRUE(isArrayBackedCollectionName(n) ||
                          isDictionaryName(n) || isLinkedListName(n));
    // Not a collection: the interface and span heads, the task family, the
    // FFI buffers, the node/spin-off types, and the non-collections.
    for (std::string_view n : {"IEnumerable", "IEnumerator", "IList",
                               "ICollection", "IReadOnlyList",
                               "IReadOnlyCollection", "IOwningList",
                               "IOwningCollection", "Span", "ReadOnlySpan",
                               "ArraySpan", "ReadOnlyArraySpan",
                               "ReadOnlyStringSpan", "LinkedListNode",
                               "Task", "OwningTask", "CString", "WString",
                               "RawBytes", "IReadOnlySpan", "IGrouping",
                               // The reserved builtins that are neither a
                               // collection nor an interface head: the slot map
                               // (§4.16), the bit reader/writer family (§4.15),
                               // the string/sync/channel/atomic classes and the
                               // exception record.
                               "Counter", "OwningSlotMap", "SlotHandle",
                               "BitReader", "BitWriter", "ArrayBitReader",
                               "ArrayBitWriter", "StringBuilder", "Channel",
                               "ChannelSender", "Stopwatch", "Random",
                               "Atomic", "AtomicInteger", "Shared", "Weak",
                               "CancellationToken", "ExceptionInfo", "Buffer",
                               "string", "int", ""})
        BEZEL_EXPECT_FALSE(isArrayBackedCollectionName(n) ||
                           isDictionaryName(n) || isLinkedListName(n));

    // The fourteen iterator and collection-interface heads (stdlib §4.4–§4.5,
    // §4.12–§4.13) — `isIterableInterfaceHead | isBuiltinForInEnumerableHead`
    // plus the two owning collection interfaces, which no predicate owns as a
    // group.
    static constexpr std::array<std::string_view, 14> kIterables = {
        "IEnumerable", "IEnumerator", "IAsyncEnumerable", "IAsyncEnumerator",
        "IOwningEnumerable", "IOwningEnumerator", "IOwningAsyncEnumerable",
        "IOwningAsyncEnumerator", "IReadOnlyList", "IReadOnlyCollection",
        "IList", "ICollection", "IOwningList", "IOwningCollection",
    };
    for (auto n : kIterables)
        BEZEL_EXPECT_TRUE(isIterableInterfaceHead(n) ||
                          isBuiltinForInEnumerableHead(n) ||
                          n == "IOwningList" || n == "IOwningCollection");
    // The out-of-vocabulary neighbours: the collection CLASS heads (a disjoint
    // union — no name is both), the event / serialization / disposable faces,
    // and the spans.
    for (std::string_view n : {"Array", "OwningArray", "Dictionary",
                               "OwningDictionary", "Set", "OwningSet",
                               "SortedSet", "Stack", "OwningStack", "Queue",
                               "OwningQueue", "LinkedList", "OwningLinkedList",
                               "OwningBuffer", "FrozenArray", "Span",
                               "ReadOnlySpan", "ArraySpan",
                               "ReadOnlyArraySpan", "IEvent", "IAsyncEvent",
                               "ISharedEvent", "IGrouping", "ISerializer",
                               "IDeserializer", "ISerializable",
                               "IDeserializable", "IDisposable",
                               "IAsyncDisposable", "IFormattable",
                               "IEqualityComparer", "IReadOnlySpan",
                               // The reserved builtins that are neither a
                               // collection nor an iterator head, and the
                               // near-miss interface names whose only relation
                               // to the two unions is a shared prefix.
                               "Buffer", "StringBuilder", "Channel",
                               "ChannelSender", "Stopwatch", "Random", "Shared",
                               "Weak", "CancellationToken",
                               "CancellationTokenSource", "OwningTask",
                               "OwningSlotMap", "SlotHandle", "BitReader",
                               "BitWriter", "ArrayBitReader", "ArrayBitWriter",
                               "ExceptionInfo", "ICounter", "ITimeSpan",
                               "IReadOnlySet", "Task", ""})
        BEZEL_EXPECT_FALSE(isIterableInterfaceHead(n) ||
                           isBuiltinForInEnumerableHead(n) ||
                           n == "IOwningList" || n == "IOwningCollection");
}
