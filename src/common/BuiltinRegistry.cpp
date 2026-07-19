#include "common/BuiltinRegistry.hpp"

#include <algorithm>
#include <array>

// #1414 Stage 3 — the data-only builtin registry (see BuiltinRegistry.hpp).
//
// Each table below is a literal snapshot of the name set that was previously
// spelled out inline at the delegating site; the corresponding row in
// tests/support/builtin_registry_snapshot_test.cpp asserts it has neither
// grown nor shrunk.  Adding or renaming a builtin now means editing exactly
// one array here (plus the site-local lowering, when a new representation is
// involved) rather than hunting every hand-list.

namespace bezel::common {

namespace {

template <std::size_t N>
bool contains(const std::array<std::string_view, N>& tbl, std::string_view n) {
    return std::find(tbl.begin(), tbl.end(), n) != tbl.end();
}

// §28.6 / §5.11 (#1008 Part C): the element-owning collections.
constexpr std::array<std::string_view, 6> kOwningCollectionHeads = {
    "OwningArray", "OwningDictionary", "OwningSet",
    "OwningStack", "OwningQueue",      "OwningLinkedList",
};

// §28.3 / §5.11: the owning collections plus the owning wrappers.
constexpr std::array<std::string_view, 12> kOwningTypeHeads = {
    "OwningArray", "OwningDictionary", "OwningSet",
    "OwningStack", "OwningQueue",      "OwningLinkedList",
    "Shared",      "Weak",             "ChannelSender",
    "OwningTask",  "IOwningEnumerable", "CancellationTokenSource",
};

// §28.3 "Task-family calls produce owned frames" (v0.190, SB-01): the
// Task-family type heads. Distinct from kOwningTypeHeads, which carries
// `OwningTask` but NOT the plain `Task` head — a `Task`/`Task<T>` return is
// container-ownership-transferring only in a NON-`async` body (an `async`
// state machine is its own fresh frame), so plain `Task` cannot join the
// unconditional owning-type set without regressing async return paths; the
// `Task` head's ownership-transferring role is gated on `!isAsync_` at the
// one ownership-pass site that consults this predicate. A new name-set fact
// (not a #1414 refactor of a pre-existing hand-list), so it has no
// pre-refactor snapshot row to pin.
constexpr std::array<std::string_view, 2> kTaskFamilyHeads = {
    "Task", "OwningTask",
};

// §4.4–§4.7 / §4.14: the bz_array_t*-backed collections (isCollectionTypeName).
// LinkedList/OwningLinkedList are bz_llist_t-backed and excluded here.
constexpr std::array<std::string_view, 10> kArrayBackedCollections = {
    "Array",  "OwningArray", "OwningBuffer",
    "Stack",  "OwningStack", "Queue",
    "OwningQueue", "Set",    "OwningSet",
    // stdlib §4.18 (#1426): SortedSet<T> shares the generic-key hash table
    // (bz_dict_alloc_generic) but routes through the collection-dispatch gate
    // (isArr → isSetName), like Set/OwningSet.
    "SortedSet",
};

// §4.8 / §4.17: the bz_dict_t*-backed collections (isDictTypeName).
constexpr std::array<std::string_view, 3> kDictionaryNames = {
    "Dictionary", "OwningDictionary", "SortedDictionary",
};

// §4.6 / §4.7: the bz_llist_t*-backed collections (isLinkedListTypeName).
constexpr std::array<std::string_view, 2> kLinkedListNames = {
    "LinkedList", "OwningLinkedList",
};

// The heap-pointer reference builtins shared by BOTH codegen rep paths in
// LlvmBackend_Layout.cpp (see header).  This is the intersection of the two
// pre-refactor hand-lists: OwningBuffer (astNode-only) and the FFI class names
// (imported-only) are NOT here — they stay as explicit deltas at the sites.
constexpr std::array<std::string_view, 64> kHeapPointerReferenceBuiltins = {
    // §4 collections (SortedSet: stdlib §4.18/#1426 — bz_dict_t* rep, both paths)
    "Array", "OwningArray", "Dictionary", "OwningDictionary",
    "SortedDictionary", "SortedSet", "Stack", "OwningStack", "Queue", "OwningQueue",
    "Set", "OwningSet", "LinkedList", "OwningLinkedList", "LinkedListNode",
    // §3 nullary heap classes with pointer rep
    "StringBuilder", "Random",
    // stdlib §8.1 (#2025): Stopwatch is a heap-allocated bz_stopwatch_t* — the
    // generic `new T()` path (where T: new()) reaches lowerNew's bz_stopwatch_alloc
    // dispatch and returns ptr, so the type must lower to ptr here too, or the
    // function return type (i32 fallback) mismatches the ptr operand.
    "Stopwatch",
    // §29 iteration / collection interfaces
    "IEnumerable", "IEnumerator", "IOwningEnumerable", "IOwningEnumerator",
    "IAsyncEnumerable", "IAsyncEnumerator", "IOwningAsyncEnumerable",
    "IOwningAsyncEnumerator", "IReadOnlyCollection", "IReadOnlyList",
    "ICollection", "IList", "IOwningCollection", "IOwningList",
    // §30 tasks
    "Task", "OwningTask",
    // §2.11 / §24.4 subscription; §6.1/§30.8 cancellation; §6.10 nursery
    "Subscription", "CancellationToken", "CancellationTokenSource", "TaskScope",
    // §28.13 / §2.16 Shared/Weak; §30.14/§30.15 channels; §5 groupings
    "Shared", "Weak", "Channel", "ChannelSender", "IGrouping",
    // §24.3 event sources; §6.5 / fix #1536: EventSource<T>/AsyncEventSource<T>
    // (the producer-side face, §24.3) are heap-allocated like the IEvent<T>/
    // IAsyncEvent<T> interfaces above — same ptr rep, no user TypeDecl.
    "IEvent", "IAsyncEvent", "EventSource", "AsyncEventSource",
    // §6.2/§6.3 sync primitives
    "Mutex", "Lock", "Semaphore", "SemaphoreToken",
    // §6.6 task-completion sources; §6.4 atomics; #545 Buffer
    "TaskCompletionSource", "OwningTaskCompletionSource",
    "Atomic", "AtomicInteger", "Buffer",
    // §2.14–2.15 serialization vocabulary + concrete handles (#1390)
    "ISerializer", "IDeserializer", "ISerializable", "IDeserializable",
    "BinarySerializer", "BinaryDeserializer", "JsonSerializer",
    "JsonDeserializer",
};

// §31.4 / #1621: the built-in exception hierarchy (child name -> parent
// name); the root `Exception` maps to "" (no parent). MIRRORS
// `Emitter::builtinExceptionHierarchy()` in src/codegen/LlvmBackend_Setup.cpp
// — see the cross-reference comment on `builtinExceptionParentOf` in the
// header. Any edit here (a new built-in exception type, or a re-parented
// one) must be mirrored there, and vice versa.
struct BuiltinExceptionEdge {
    std::string_view name;
    std::string_view parent;
};
constexpr std::array<BuiltinExceptionEdge, 28> kBuiltinExceptionHierarchy = {{
    {"Exception", ""},
    {"ArgumentException", "Exception"},
    {"ArgumentNullException", "ArgumentException"},
    {"ArgumentOutOfRangeException", "ArgumentException"},
    {"ArithmeticException", "Exception"},
    {"OverflowException", "ArithmeticException"},
    {"DivideByZeroException", "ArithmeticException"},
    {"InvalidOperationException", "Exception"},
    {"DisposedAccessException", "InvalidOperationException"},
    {"NotImplementedException", "Exception"},
    {"NullReferenceException", "Exception"},
    {"IndexOutOfRangeException", "Exception"},
    {"KeyNotFoundException", "Exception"},
    {"MatchFailureException", "Exception"},
    {"OperationCanceledException", "Exception"},
    {"TypeInitializationException", "Exception"},
    {"FormatException", "Exception"},
    {"StackOverflowException", "Exception"},
    {"ChannelClosedException", "Exception"},
    {"ChannelFullException", "Exception"},
    {"DomainFaultedException", "Exception"},
    {"SerializationException", "Exception"},
    {"TimeoutException", "Exception"},
    {"IOException", "Exception"},
    {"FileNotFoundException", "IOException"},
    {"DirectoryNotFoundException", "IOException"},
    {"EndOfStreamException", "IOException"},
    {"PathTooLongException", "IOException"},
}};

}  // namespace

bool isOwningPrefixedName(std::string_view head) {
    return head.rfind("Owning", 0) == 0;
}

bool isOwningCollectionHead(std::string_view head) {
    return contains(kOwningCollectionHeads, head);
}

bool isOwningTypeHead(std::string_view head) {
    return contains(kOwningTypeHeads, head);
}

bool isTaskFamilyHead(std::string_view head) {
    return contains(kTaskFamilyHeads, head);
}

bool isArrayBackedCollectionName(std::string_view n) {
    return contains(kArrayBackedCollections, n);
}

bool isDictionaryName(std::string_view n) {
    return contains(kDictionaryNames, n);
}

bool isLinkedListName(std::string_view n) {
    return contains(kLinkedListNames, n);
}

bool isHeapPointerReferenceBuiltin(std::string_view n) {
    return contains(kHeapPointerReferenceBuiltins, n);
}

std::string_view builtinExceptionParentOf(std::string_view name) {
    for (const auto& edge : kBuiltinExceptionHierarchy)
        if (edge.name == name) return edge.parent;
    return {};
}

// #1936 / §19.5 / §31: membership test over the same hierarchy table (the root
// `Exception` is present as an edge with an empty parent, so it is recognized).
bool isBuiltinExceptionName(std::string_view name) {
    for (const auto& edge : kBuiltinExceptionHierarchy)
        if (edge.name == name) return true;
    return false;
}

}  // namespace bezel::common
