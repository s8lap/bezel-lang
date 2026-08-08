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
// §28.6 / §28.3 (v0.221, #1787 ruling; #2369): all FOUR owning iterable-
// interface heads (`IOwningEnumerable`, `IOwningEnumerator`,
// `IOwningAsyncEnumerable`, `IOwningAsyncEnumerator`) are members — "the
// owning heads take no unmarked instance/extension spelling at all", so
// `checkBorrowReturnedThroughOwningType` must inspect a borrow returned
// through any of the four (previously only `IOwningEnumerable` was listed,
// letting the three sibling heads sail past the §28.6 return-shape check).
constexpr std::array<std::string_view, 15> kOwningTypeHeads = {
    "OwningArray", "OwningDictionary", "OwningSet",
    "OwningStack", "OwningQueue",      "OwningLinkedList",
    "Shared",      "Weak",             "ChannelSender",
    "OwningTask",  "IOwningEnumerable", "CancellationTokenSource",
    // #2369: the three remaining owning iterable-interface heads.
    "IOwningEnumerator", "IOwningAsyncEnumerable", "IOwningAsyncEnumerator",
};

// §28.6 / §28.3 (v0.221, #1787 ruling; #2369): the OWNING iterable-interface
// heads — the four consumer-disposed, non-idempotent deferred-activation
// interfaces whose receiver-owned-field bare-read carve-out §28.6 bullet 1
// EXPLICITLY excludes ("The same method-return exclusion applies to a field
// typed at an owning iterable-interface head"). `checkBorrowReturnedThrough
// OwningType`'s `deferredActivationOwningHead` gate (the #1660 carve-out
// removal) consults this set; it overlaps `kIterableInterfaceHeads` (the
// full 8-member iterator-convention family) and `kOwningTypeHeads` (these
// four are the owning iterable subset of that owning-head set).
constexpr std::array<std::string_view, 4> kOwningIterableInterfaceHeads = {
    "IOwningEnumerable", "IOwningEnumerator",
    "IOwningAsyncEnumerable", "IOwningAsyncEnumerator",
};

// §28.3 / §29.3 / §30.7 (v0.201, #2048 / proposal 0048): the iterable and
// enumerator interfaces whose unmarked return type is the announcement of a
// caller-owned deferred-activation frame (the iterator convention). A static
// signature declaring one of these heads is in the announced set of the
// §28.3 *Factory methods and sourceless returns* declaration-site error —
// the unmarked type itself announces the classification, so no `owned`
// marker is required. (Each is also a member of kHeapPointerReferenceBuiltins
// below for lowering; this set is the ownership-pass membership test.)
constexpr std::array<std::string_view, 8> kIterableInterfaceHeads = {
    "IEnumerable", "IEnumerator",
    "IAsyncEnumerable", "IAsyncEnumerator",
    "IOwningEnumerable", "IOwningEnumerator",
    "IOwningAsyncEnumerable", "IOwningAsyncEnumerator",
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

// #2809 / §32.5 / stdlib §10.1–§10.3: the FFI-boundary BUFFER classes. See the
// header for why the set exists and who consults it. `IntPtr`/`UIntPtr` are
// §32.7 pointer-sized integer VALUE types, not buffers, and stay out.
constexpr std::array<std::string_view, 3> kFfiBufferBuiltins = {
    "CString", "WString", "RawBytes",
};

// The heap-pointer reference builtins shared by BOTH codegen rep paths in
// LlvmBackend_Layout.cpp (see header).  This is the intersection of the two
// pre-refactor hand-lists: OwningBuffer (astNode-only) and the FFI class names
// (imported-only) are NOT here — they stay as explicit deltas at the sites.
constexpr std::array<std::string_view, 65> kHeapPointerReferenceBuiltins = {
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
    // stdlib §7.2 (#645): FileStream is a heap-allocated bz_filestream_t* —
    // same reason as Stopwatch above: the type must lower to ptr on both rep
    // paths or the `new FileStream(...)` result (a ptr) mismatches the
    // fallback i32 lowering of the declared type.
    "FileStream",
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

// #2757: the RESERVED builtin type names that lowerTypeRef's Named case
// recognizes but that are NOT heap-pointer references — the complement that,
// together with kHeapPointerReferenceBuiltins above, makes up the complete
// reserved-name set (`isReservedBuiltinTypeName`, see the header for why the
// set exists and who consults it). Derived by grepping every name compared in
// that Named case (src/codegen/LlvmBackend_Layout.cpp) and subtracting the
// heap table; each entry names the branch it comes from.
//
// `DictionaryEntry` and `DrainedEntry` are compared there too and are
// deliberately ABSENT: their arm is reached only by a synthetic TypeRef
// (no astNode, importedName-keyed, genericArgs.size() >= 2), and neither name
// resolves as a written type annotation at all (`bezelc` reports "unresolved
// type name" — measured, not inferred), so no declaration-less single-part
// Named ref can ever carry them. Adding them would only change the
// generic-argument inference binding, with nothing to gain.
constexpr std::array<std::string_view, 19> kNonHeapReservedBuiltins = {
    // §32.5 / §32.7 / stdlib §10: FFI-boundary types (i64 / opaque ptr).
    "IntPtr", "UIntPtr", "CString", "WString", "RawBytes",
    // §2.5: the temporal/identity value builtins (i64 / i32 / {i64,i64}).
    "DateTime", "TimeSpan", "DateOnly", "TimeOnly", "Guid",
    // §9.8 (#499): Range<T> — the intrinsic value-type record.
    "Range",
    // §4.14 / §4.15 (#549/#779) / §3.3 (#883): the {ptr,i32,i32} span shapes.
    "ArraySpan", "ReadOnlyArraySpan", "Span", "ReadOnlySpan",
    "ReadOnlyStringSpan",
    // §2.15 (#687): JsonOptions — a compiler-known value record.
    "JsonOptions",
    // §4.14: OwningBuffer<T> — a heap ptr, but only on the astNode rep path,
    // so it is not a member of the shared heap table above.
    "OwningBuffer",
    // §31.4: ExceptionInfo — the {ptr,ptr,ptr} Exception.CauseChain element.
    "ExceptionInfo",
};

// #2921 / #2923: the reserved built-in INTERFACE heads — the subset of
// `isReservedBuiltinTypeName` whose runtime value may be a USER class (a user
// type may declare `class MyList: IEnumerable<int>`, and an
// `IEnumerable<int>`-typed slot then holds a real vtable-bearing object).
// They are therefore the exact complement of `isVtableLessBuiltinClassName`:
// every OTHER reserved name denotes a concrete built-in whose runtime value is
// a header-less C struct. See the header for why the derivation runs in this
// direction (a forgotten entry here costs a leak, never a wild jump).
constexpr std::array<std::string_view, 21> kBuiltinInterfaceHeads = {
    // §29 iteration / collection interfaces
    "IEnumerable", "IEnumerator", "IOwningEnumerable", "IOwningEnumerator",
    "IAsyncEnumerable", "IAsyncEnumerator", "IOwningAsyncEnumerable",
    "IOwningAsyncEnumerator", "IReadOnlyCollection", "IReadOnlyList",
    "ICollection", "IList", "IOwningCollection", "IOwningList",
    // §24.3 / §6.5 event faces; §5 groupings
    "IEvent", "IAsyncEvent", "IGrouping",
    // §2.14–§2.15 serialization vocabulary
    "ISerializer", "IDeserializer", "ISerializable", "IDeserializable",
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
constexpr std::array<BuiltinExceptionEdge, 29> kBuiltinExceptionHierarchy = {{
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
    // §22.16 (v0.203, #2053): clause evaluation is a language operation,
    // like the indexer's IndexOutOfRangeException; derives directly from
    // Exception per §22.16 "Failure semantics and the enforcement modes".
    {"ContractViolationException", "Exception"},
}};

}  // namespace

bool isOwningPrefixedName(std::string_view head) {
    return head.rfind("Owning", 0) == 0;
}  // per §5.11 / #1372 — Owning* prefix test (consolidates the 16 codegen rfind sites)

bool isOwningCollectionHead(std::string_view head) {
    return contains(kOwningCollectionHeads, head);
}  // per §28.6 / §5.11 (#1008 Part C) — element-owning collection heads

bool isOwningTypeHead(std::string_view head) {
    return contains(kOwningTypeHeads, head);
}  // per §28.3 / §5.11 — owning-type heads (collections + wrappers)

bool isTaskFamilyHead(std::string_view head) {
    return contains(kTaskFamilyHeads, head);
}  // per §28.3 v0.190 (SB-01) — Task-family heads; `Task` here only transfers ownership under !isAsync_

// §28.3 / §29.3 / §30.7 (v0.201, #2048 / proposal 0048): the iterable or
// enumerator interface heads — the iterator convention's announced caller-
// owned frame. A static signature with one of these return types is in the
// announced set of the §28.3 *Factory methods and sourceless returns*
// declaration-site error. // per §28.3 v0.201
bool isIterableInterfaceHead(std::string_view head) {
    return contains(kIterableInterfaceHeads, head);
}

// §28.6 / §28.3 (v0.221, #1787 ruling; #2369): the OWNING iterable-interface
// heads — the four consumer-disposed, non-idempotent deferred-activation
// interfaces whose receiver-owned-field bare-read carve-out §28.6 bullet 1
// explicitly excludes. Consulted by `checkBorrowReturnedThroughOwningType`'s
// `deferredActivationOwningHead` gate (the #1660 carve-out removal, extended
// to all four owning iterable heads). // per §28.6, §28.3
bool isOwningIterableInterfaceHead(std::string_view head) {
    return contains(kOwningIterableInterfaceHeads, head);
}

bool isArrayBackedCollectionName(std::string_view n) {
    return contains(kArrayBackedCollections, n);
}  // per §4.4–§4.7 / §4.14 — bz_array_t*-backed collection names

bool isDictionaryName(std::string_view n) {
    return contains(kDictionaryNames, n);
}  // per §4.8 / §4.17 — bz_dict_t*-backed K-indexed collection names

bool isLinkedListName(std::string_view n) {
    return contains(kLinkedListNames, n);
}  // per §4.6 / §4.7 — bz_llist_t*-backed linked-list names

bool isFfiBufferBuiltinName(std::string_view n) {
    return contains(kFfiBufferBuiltins, n);
}  // per §32.5 / stdlib §10.1–§10.3 (#2809) — the raw-buffer FFI classes

bool isHeapPointerReferenceBuiltin(std::string_view n) {
    return contains(kHeapPointerReferenceBuiltins, n);
}  // per §4 / §6 / §29 / §30 / §2.14–§2.15 — heap-pointer reference builtins shared by the two codegen rep paths

// #2757 / §7.1 / §14.1: the COMPLETE reserved builtin type-name set — the heap
// table plus the non-heap Named-case names. See the header for the contract.
bool isReservedBuiltinTypeName(std::string_view n) {
    return contains(kHeapPointerReferenceBuiltins, n) ||
           contains(kNonHeapReservedBuiltins, n);
}

// #2921 / #2923 / §8.1 / §8.5 / §28.2 / §28.9: the reserved built-in CLASSES —
// every reserved name that is not an interface head. Their runtime values are
// plain C structs with no Bezel vtable at word 0, so neither the slot-2 Dispose
// dispatch nor `bz_obj_is_string`'s negative "not-a-vtable ⇒ a string" test may
// be applied to them. See the header for the full contract.
bool isVtableLessBuiltinClassName(std::string_view n) {
    return isReservedBuiltinTypeName(n) &&
           !contains(kBuiltinInterfaceHeads, n);
}

// fix #2992 (§28.3 / §28.9 / §28.10, v0.240): the built-in type heads whose
// `using var` carries a REAL §28.10 disposal obligation — the set the §28.10
// vacuity lint (rhsIsDisposableClassType Path 2) uses to decide a `using var`
// is NOT vacuous, and that the codegen auto-drop chain
// (registerUsingDisposal) consults at its end-of-chain guard to make a silent
// fallthrough loud. See the header for why this is the shared authority.
//
// This is the EXACT population that lived inline in rhsIsDisposableClassType's
// Path 2 (Ownership.cpp ~5046) pre-#2992 — lifted verbatim, not reconciled
// with codegen's broader chain-claimed set. The two DISAGREE on 13 names (the
// borrowing collections Array/Stack/Queue/Set/SortedSet/Dictionary/
// SortedDictionary/LinkedList, plus Stopwatch, Random, Atomic, AtomicInteger,
// Buffer): every one is claimed by the chain but absent here, so a factory-
// call `using var x := MakeArr()` over `Array<T>` fires a false-positive
// vacuity warning (measured 2026-08-09 on d2a3d3e). Reconciling them requires
// a design decision about whether borrowing collections belong in the LINT's
// disposable set — #2695 deliberately excluded them so the lint fires on a
// bound-then-pinned alias whose `const` remedy compiles (the lint's coherence
// invariant). That decision is filed separately; the table here matches the
// lint's current table so the lift is behaviour-preserving.
// per §28.3 / §28.9 / §28.10, fix #2992
constexpr std::array<std::string_view, 41> kDisposableBuiltinHeadNames = {
    // stdlib §3.1 / spec §28.9 (#2190): StringBuilder owns its heap parts array.
    "StringBuilder",
    // §2.15 concrete serializers/deserializers (#2073 rework 3b).
    "BinarySerializer", "BinaryDeserializer",
    "JsonSerializer", "JsonDeserializer",
    // §32.6 / stdlib §10: CString/WString/RawBytes carry a free() obligation.
    "CString", "WString", "RawBytes",
    // stdlib §7.2 (#645): FileStream — flush + close on dispose.
    "FileStream",
    // §29.1/§29.2: the iteration-protocol sentinel family (#2385/#2451).
    "IEnumerable", "IEnumerator",
    "IOwningEnumerable", "IOwningEnumerator",
    "IAsyncEnumerable", "IAsyncEnumerator",
    "IOwningAsyncEnumerable", "IOwningAsyncEnumerator",
    // §30.15 channels; §24.3 events; §2.11 subscription.
    "Channel", "ChannelSender",
    "EventSource", "AsyncEventSource",
    "Subscription",
    // §6.10 nursery; §6.1 cancellation.
    "TaskScope", "CancellationTokenSource",
    // §6.2/§6.3 sync primitives.
    "Mutex", "Lock", "Semaphore", "SemaphoreToken",
    // §30 tasks; §28.13 Shared/Weak.
    "OwningTask", "Task", "Shared", "Weak",
    // §28.11 owning collections (the borrowing variants are intentionally
    // absent — see the header comment and #2695).
    "OwningArray", "OwningDictionary", "OwningSet", "OwningStack",
    "OwningQueue", "OwningLinkedList", "OwningBuffer",
    // §6.6 task-completion sources.
    "TaskCompletionSource", "OwningTaskCompletionSource",
};

bool isDisposableBuiltinHeadName(std::string_view head) {
    return contains(kDisposableBuiltinHeadNames, head);
}  // per §28.3 / §28.9 / §28.10, fix #2992

std::string_view builtinExceptionParentOf(std::string_view name) {
    for (const auto& edge : kBuiltinExceptionHierarchy)
        if (edge.name == name) return edge.parent;
    return {};
}  // per §31.4 / #1621 — built-in exception parent lookup (the only access from sema)

// #1936 / §19.5 / §31: membership test over the same hierarchy table (the root
// `Exception` is present as an edge with an empty parent, so it is recognized).
// per §19.5 / §31 / #1936.
bool isBuiltinExceptionName(std::string_view name) {
    for (const auto& edge : kBuiltinExceptionHierarchy)
        if (edge.name == name) return true;
    return false;
}

}  // namespace bezel::common
