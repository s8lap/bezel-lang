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

// --- isArrayBackedCollectionName (§4.4–§4.7/§4.14) : isCollectionTypeName ---
BEZEL_TEST(builtin_registry_snapshot, array_backed_collection_set) {
    static constexpr std::array<std::string_view, 10> kSnapshot = {
        "Array",  "OwningArray", "OwningBuffer",
        "Stack",  "OwningStack", "Queue",
        "OwningQueue", "Set",    "OwningSet",
        // INTENTIONAL addition (stdlib §4.18, #1426): SortedSet joined the
        // collection-dispatch gate — it was MISSING pre-#1426 (i32-typed,
        // lowerNew no-op), the 4th missed-name-list instance.
        "SortedSet",
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
    static constexpr std::array<std::string_view, 65> kSnapshot = {
        "Array", "OwningArray", "Dictionary", "OwningDictionary",
        // INTENTIONAL addition (stdlib §4.18, #1426): SortedSet — bz_dict_t*
        // rep on both the astNode and imported paths.
        "SortedDictionary", "SortedSet", "Stack", "OwningStack", "Queue", "OwningQueue",
        "Set", "OwningSet", "LinkedList", "OwningLinkedList", "LinkedListNode",
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
        // INTENTIONAL addition (stdlib §6.5, fix #1536): EventSource<T> /
        // AsyncEventSource<T> — the producer-side event face (§24.3), same
        // ptr rep as IEvent<T>/IAsyncEvent<T>.
        "IEvent", "IAsyncEvent", "EventSource", "AsyncEventSource",
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
                               "JsonOptions", "Boolean", ""})
        BEZEL_EXPECT_FALSE(isHeapPointerReferenceBuiltin(n));
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
    static constexpr std::array<std::string_view, 19> kNonHeapSnapshot = {
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
        // §4.14 — heap ptr, but only on the astNode rep path
        "OwningBuffer",
        // §31.4 — the Exception.CauseChain element record
        "ExceptionInfo",
    };
    for (auto n : kNonHeapSnapshot) {
        BEZEL_EXPECT_TRUE(isReservedBuiltinTypeName(n));
        // ...and each is genuinely OUTSIDE the heap table (no redundancy: if one
        // of these were also in the heap set, the split would be meaningless).
        BEZEL_EXPECT_FALSE(isHeapPointerReferenceBuiltin(n));
    }
    // (b) the heap half is reserved too — spot-check the names whose absence
    // from the deleted duplicate WAS the defect, plus the long-standing ones.
    for (std::string_view n : {"Stopwatch", "FileStream", "TaskScope",
                               "SortedDictionary", "SortedSet",
                               "IEvent", "IAsyncEvent", "EventSource",
                               "AsyncEventSource", "ISerializer",
                               "IDeserializer", "ISerializable",
                               "IDeserializable", "BinarySerializer",
                               "BinaryDeserializer", "JsonSerializer",
                               "JsonDeserializer",
                               "Array", "Dictionary", "Task", "Shared",
                               "Mutex", "Buffer", "IGrouping"})
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
