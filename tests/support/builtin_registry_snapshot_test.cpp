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
// Each `kSnapshot*` array below is the EXACT set as it stood before the
// migration; the negatives are near-neighbours from adjacent sets that must
// stay out.  If a real stdlib addition changes a set, update BOTH the registry
// table and the matching snapshot here in the same commit — that is the point.

#include "test_harness.hpp"

#include "common/BuiltinRegistry.hpp"

#include <array>
#include <string_view>

using namespace bezel::common;

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
    static constexpr std::array<std::string_view, 12> kSnapshot = {
        "OwningArray", "OwningDictionary", "OwningSet",
        "OwningStack", "OwningQueue",      "OwningLinkedList",
        "Shared",      "Weak",             "ChannelSender",
        "OwningTask",  "IOwningEnumerable", "CancellationTokenSource",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isOwningTypeHead(n));
    for (std::string_view n : {"Array", "Dictionary", "OwningBuffer",
                               "OwningTaskCompletionSource", "Task", ""})
        BEZEL_EXPECT_FALSE(isOwningTypeHead(n));
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

// --- isArrayBackedCollectionName (§4.4–§4.7/§4.14) : isCollectionTypeName ---
BEZEL_TEST(builtin_registry_snapshot, array_backed_collection_set) {
    static constexpr std::array<std::string_view, 9> kSnapshot = {
        "Array",  "OwningArray", "OwningBuffer",
        "Stack",  "OwningStack", "Queue",
        "OwningQueue", "Set",    "OwningSet",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isArrayBackedCollectionName(n));
    // LinkedList (bz_llist_t) and Dictionary (bz_dict_t) are NOT array-backed.
    for (std::string_view n : {"LinkedList", "OwningLinkedList", "Dictionary",
                               "OwningDictionary", "SortedDictionary",
                               "SortedSet", "Buffer", ""})
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

// --- isHeapPointerReferenceBuiltin : the shared core of BOTH Layout rep lists.
// This is the intersection of the two pre-refactor hand-lists in
// LlvmBackend_Layout.cpp; OwningBuffer (astNode-only) and the FFI class names
// (imported-only) are deliberately NOT in the shared set — they are the small
// site-local deltas the two call sites still test explicitly.
BEZEL_TEST(builtin_registry_snapshot, heap_pointer_reference_builtin_set) {
    static constexpr std::array<std::string_view, 60> kSnapshot = {
        "Array", "OwningArray", "Dictionary", "OwningDictionary",
        "SortedDictionary", "Stack", "OwningStack", "Queue", "OwningQueue",
        "Set", "OwningSet", "LinkedList", "OwningLinkedList", "LinkedListNode",
        "StringBuilder", "Random",
        "IEnumerable", "IEnumerator", "IOwningEnumerable", "IOwningEnumerator",
        "IAsyncEnumerable", "IAsyncEnumerator", "IOwningAsyncEnumerable",
        "IOwningAsyncEnumerator", "IReadOnlyCollection", "IReadOnlyList",
        "ICollection", "IList", "IOwningCollection", "IOwningList",
        "Task", "OwningTask",
        "Subscription", "CancellationToken", "CancellationTokenSource",
        "TaskScope",
        "Shared", "Weak", "Channel", "ChannelSender", "IGrouping",
        "IEvent", "IAsyncEvent",
        "Mutex", "Lock", "Semaphore", "SemaphoreToken",
        "TaskCompletionSource", "OwningTaskCompletionSource",
        "Atomic", "AtomicInteger", "Buffer",
        "ISerializer", "IDeserializer", "ISerializable", "IDeserializable",
        "BinarySerializer", "BinaryDeserializer", "JsonSerializer",
        "JsonDeserializer",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isHeapPointerReferenceBuiltin(n));
    // The two site-local deltas must NOT be in the shared set.
    for (std::string_view n : {"OwningBuffer", "IntPtr", "UIntPtr", "CString",
                               "WString", "RawBytes"})
        BEZEL_EXPECT_FALSE(isHeapPointerReferenceBuiltin(n));
    // Value-type / span / temporal builtins are NOT heap-pointer references.
    for (std::string_view n : {"DateTime", "TimeSpan", "Guid", "Range", "Span",
                               "ReadOnlySpan", "ArraySpan", "ReadOnlyStringSpan",
                               "JsonOptions", "SortedSet", "Boolean", ""})
        BEZEL_EXPECT_FALSE(isHeapPointerReferenceBuiltin(n));
}
