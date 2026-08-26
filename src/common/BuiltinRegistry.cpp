#include "common/BuiltinRegistry.hpp"

#include "sema/TypeRef.hpp"   // PrimitiveType (for primitiveBuiltinSpelling, fix #3119)

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

// §4.4–§4.7 / §4.14 / §4.19: the bz_array_t*-backed collections
// (isCollectionTypeName). LinkedList/OwningLinkedList are bz_llist_t-backed and
// excluded here. FrozenArray<T> shares the bz_array_t* representation with
// Array<T> (§4.19, fix #3249): the only construction forms are the two
// §4.19 `From` overloads, so the storage is owned and shared only via the
// §30.14 string-precedent tolerance — no mutating member ever sees it.
constexpr std::array<std::string_view, 11> kArrayBackedCollections = {
    "Array",  "OwningArray", "OwningBuffer",
    "Stack",  "OwningStack", "Queue",
    "OwningQueue", "Set",    "OwningSet",
    // stdlib §4.18 (#1426): SortedSet<T> shares the generic-key hash table
    // (bz_dict_alloc_generic) but routes through the collection-dispatch gate
    // (isArr → isSetName), like Set/OwningSet.
    "SortedSet",
    // fix #3249: §4.19 FrozenArray<T> — same bz_array_t* rep as Array<T>;
    // its `Length` / indexer-get / `ToArray` / destructor all lower through
    // bz_array_* the same way, so it joins the array-backed collection gate.
    "FrozenArray",
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
constexpr std::array<std::string_view, 69> kHeapPointerReferenceBuiltins = {
    // §4 collections (SortedSet: stdlib §4.18/#1426 — bz_dict_t* rep, both paths)
    "Array", "OwningArray", "Dictionary", "OwningDictionary",
    "SortedDictionary", "SortedSet", "Stack", "OwningStack", "Queue", "OwningQueue",
    "Set", "OwningSet", "LinkedList", "OwningLinkedList", "LinkedListNode",
    // fix #3249 (§4.19, v0.186): FrozenArray<T> — same bz_array_t* rep on
    // both rep paths; the type is heap-allocated (constructed via the §4.19
    // `From` overloads), so its type-annotation lower lowers to ptr on both
    // the astNode and the imported-name paths.
    "FrozenArray",
    // §4.14 (fix #1740, v0.254): OwningBuffer<T> is a heap-owning collection
    // (ptr rep on the astNode path; identical role to OwningArray<T>). It was
    // historically in kNonHeapReservedBuiltins with a comment claiming it was
    // still a heap ptr — but the EFFECT was that producerState(new OwningBuffer)
    // classified the producer as Borrowed (a value type), so `var buf := new
    // OwningBuffer<double>(4);` initialized `buf` in Borrowed state and the
    // §28.3 consume-site freeze misclassifed the subsequent `owned buf` as a
    // borrow and silently dropped the move. OwningArray is in the heap table,
    // so OwningBuffer belongs alongside it.
    "OwningBuffer",
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
    //
    // fix #1832 / §6.5 (v0.187): the cross-domain publication-point pair
    // ISharedEvent<T> / SharedEventSource<T> share the §24.3 same ptr rep (a
    // bz_event_source_t*; a real synchronization discipline around the
    // subscriber list is recorded for a later slice and is the runtime-side
    // contract — the type-name registration here just admits them). Both names
    // are heap-pointer references with no user TypeDecl, exactly like the
    // instance pair above.
    "IEvent", "IAsyncEvent", "ISharedEvent",
    "EventSource", "AsyncEventSource", "SharedEventSource",
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
constexpr std::array<std::string_view, 18> kNonHeapReservedBuiltins = {
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
    // §31.4: ExceptionInfo — the {ptr,ptr,ptr} Exception.CauseChain element.
    "ExceptionInfo",
};

// fix #2994 (§17.7 "Standard-library builtins have a closed constructor
// surface", v0.241): the two REJECTING arms of that rule. See the header for
// the contract and for why the rest of `kNonHeapReservedBuiltins` stays out.
//
// New name-set FACTS, not a #1414 refactor of a pre-existing hand-list (the
// `kTaskFamilyHeads` precedent above), so neither has a pre-refactor snapshot
// row in tests/support/builtin_registry_snapshot_test.cpp to pin; they are
// pinned by the behavioral grid in
// tests/codegen/builtin_closed_ctor_surface_2994_test.cpp instead.
constexpr std::array<std::string_view, 3> kNoConstructorBuiltins = {
    // stdlib §2.6, verbatim: "`Guid` declares no constructor, so
    // `new Guid(...)` is a compile error at any argument list (language spec
    // §17.7's closed-constructor-surface rule); construct via `Empty`,
    // `NewGuid()`, `FromByteArray`, `Parse`/`TryParse`/`ParseOrNull`, or
    // `default(Guid)`."
    "Guid",
    // stdlib §10.0 declares `Zero`, `operator =` and `operator <>` on each —
    // and no constructor. The §5.2 explicit conversions `IntPtr(longValue)` /
    // `long(ptr)` are CallExprs, not `new`, and are unaffected.
    "IntPtr", "UIntPtr",
};

constexpr std::array<std::string_view, 2> kPositionalCtorOnlyBuiltinRecords = {
    // §31.4: `public record ExceptionInfo(TypeName: string, Message: string,
    // StackTrace: string);` — the Exception.CauseChain element type.
    "ExceptionInfo",
    // stdlib §3.3: `public record ReadOnlyStringSpan(Source: string,
    // Start: int, Length: int): IReadOnlySpan<char>`.
    "ReadOnlyStringSpan",
};

// fix #3136 (§17.7 "Standard-library builtins have a closed constructor
// surface", v0.241; §31.4; stdlib §3.3): the DECLARED POSITIONAL ARITY of each
// arm-2 record. The original `kPositionalCtorOnlyBuiltinRecords` above carries
// the membership set #2994 grew; this table carries the matching arity so the
// sema arm can reject "2-arg" and "4-arg" lists as well as the parameterless
// one #2994 already rejected. Both current entries declare THREE fields, but
// the table is per-record rather than a single constant so a future arm-2
// record with a different declared arity does not force a literal update at
// the call site.
//
// The arity is the count of DECLARED FIELDS, which is the count of POSITIONAL
// PARAMETERS the doc declares for the record's positional primary constructor
// (§9.1): "A record's positional constructor takes its field names as
// parameter names." A named-argument list is still arity-equal (the names
// resolve against the same field set, #3098), so this arity is the right
// number to compare `ne.args.size()` against for the §17.7 rejection.
// per §17.7 / §31.4 / stdlib §3.3
struct PositionalCtorOnlyBuiltinRecord {
    std::string_view name;
    std::size_t arity;
};
constexpr std::array<PositionalCtorOnlyBuiltinRecord, 2>
kPositionalCtorOnlyBuiltinRecordArities = {{
    // §31.4: `public record ExceptionInfo(TypeName: string, Message: string,
    // StackTrace: string);` — three declared positional fields.
    {"ExceptionInfo", 3},
    // stdlib §3.3: `public record ReadOnlyStringSpan(Source: string,
    // Start: int, Length: int): IReadOnlySpan<char>` — three declared
    // positional fields.
    {"ReadOnlyStringSpan", 3},
}};

// #2921 / #2923: the reserved built-in INTERFACE heads — the subset of
// `isReservedBuiltinTypeName` whose runtime value may be a USER class (a user
// type may declare `class MyList: IEnumerable<int>`, and an
// `IEnumerable<int>`-typed slot then holds a real vtable-bearing object).
// They are therefore the exact complement of `isVtableLessBuiltinClassName`:
// every OTHER reserved name denotes a concrete built-in whose runtime value is
// a header-less C struct. See the header for why the derivation runs in this
// direction (a forgotten entry here costs a leak, never a wild jump).
constexpr std::array<std::string_view, 22> kBuiltinInterfaceHeads = {
    // §29 iteration / collection interfaces
    "IEnumerable", "IEnumerator", "IOwningEnumerable", "IOwningEnumerator",
    "IAsyncEnumerable", "IAsyncEnumerator", "IOwningAsyncEnumerable",
    "IOwningAsyncEnumerator", "IReadOnlyCollection", "IReadOnlyList",
    "ICollection", "IList", "IOwningCollection", "IOwningList",
    // §24.3 / §6.5 event faces; §5 groupings
    "IEvent", "IAsyncEvent", "ISharedEvent", "IGrouping",
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

// fix #2994 / §17.7 (v0.241): the closed-constructor-surface arms. Both are
// subsets of kNonHeapReservedBuiltins — the value-shaped half codegen's #2974
// gate exempts — so neither can collide with the pointer-shaped population
// that gate still rejects.
bool isNoConstructorBuiltin(std::string_view n) {
    return contains(kNoConstructorBuiltins, n);
}  // per §17.7 / stdlib §2.6 / stdlib §10.0 — `new T(...)` at NO argument list

bool isPositionalCtorOnlyBuiltinRecord(std::string_view n) {
    return contains(kPositionalCtorOnlyBuiltinRecords, n);
}  // per §17.7 / §31.4 / stdlib §3.3 — only the undeclared `new T()` is rejected

// fix #3136 (§17.7, v0.241): the DECLARED arity of each arm-2 record, or 0
// when `n` is not in the set. The 0 sentinel lets a single call site express
// both the membership test and the arity check (`if (auto a = ...; a &&
// ne.args.size() != a)`), avoiding a hand-rolled "check `is...Only` and then
// look up arity" two-step. The header carries the full rationale; the comment
// here names the contract only.
std::size_t positionalCtorArityOfBuiltinRecord(std::string_view n) {
    for (const auto& r : kPositionalCtorOnlyBuiltinRecordArities)
        if (r.name == n) return r.arity;
    return 0;
}  // per §17.7 / §31.4 / stdlib §3.3

// #2921 / #2923 / §8.1 / §8.5 / §28.2 / §28.9: the reserved built-in CLASSES —
// every reserved name that is not an interface head. Their runtime values are
// plain C structs with no Bezel vtable at word 0, so neither the slot-2 Dispose
// dispatch nor `bz_obj_is_string`'s negative "not-a-vtable ⇒ a string" test may
// be applied to them. See the header for the full contract.
bool isVtableLessBuiltinClassName(std::string_view n) {
    return isReservedBuiltinTypeName(n) &&
           !contains(kBuiltinInterfaceHeads, n);
}

// fix #2969 (§8.5 / §8.1 / §18.5 / §28.9, v0.240): the reserved built-in
// INTERFACE heads — exposed as its own predicate so the §2969 codegen routing
// does not have to re-list the `kBuiltinInterfaceHeads` names. See the header
// for the contract — split off from `isVtableLessBuiltinClassName` because a
// user class may legitimately declare `class MyE: IEnumerable<int>`, so the
// dynamic dispatch on these names must remain reachable even though they are
// the one reserved-name population that §2923's fix declined to route away
// from it.
bool isBuiltinInterfaceHead(std::string_view n) {
    return contains(kBuiltinInterfaceHeads, n);
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
    // fix #2269 (§28.9, v0.240): StringBuilder is REMOVED from the disposable
    // set. The spec (§28.9 L7363) classifies StringBuilder as TRIVIAL-DISPOSE
    // ("declares no destructor and no `owned` fields, and its intrinsic
    // backing storage is released by the instance's own free, exactly as a
    // borrowing collection's is") and §28.9 L7357 forbids the ownership-
    // discharge warning on a `new` expression of a trivial-dispose type that
    // isn't stored. #2190 added it to this table on a stale v0.226 reading;
    // the §28.3 ceremony now over-fires on `var sb := new StringBuilder();`
    // and `using var sb := new StringBuilder();` draws the vacuous-pin lint
    // the spec says it must not. Removing it restores the spec-trivial-dispose
    // classification: StringBuilder carries the intrinsic backing-storage free
    // (the only disposal work) and nothing else, so the lint and the §28.3
    // ceremony both correctly classify it as ordinary reclaim (no user code).
    // // per §28.9 (v0.240), fix #2269
    //
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
    // fix #1832 / §6.5 (v0.187): SharedEventSource<T> — same §28.9
    // vtable-less reclaim discipline as the instance sources (the
    // synchronized quiescence / lock discipline is a runtime concern; the
    // shape that matters here is that an owned field of this head is
    // bz_event_source_t*-shaped and freed through the chain).
    "SharedEventSource",
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

// fix #3017 (§28.3 / §28.9 / §28.10, v0.240): the 13 built-in class heads
// the codegen auto-drop chain (registerUsingDisposal) claims disposal for but
// that are DELIBERATELY ABSENT from `kDisposableBuiltinHeadNames` above —
// the §28.10 vacuity-lint's table. Each name carries a real disposal
// obligation (§28.9 trivial-dispose reclaim for the borrowing collections;
// specific runtime frees for the vtable-less builtins), so a factory-call
// `using var x := C.Make()` over one is the CORRECT canonical pattern, not
// vacuous. The alias form (`var src := new T(); using var p := src;`) is NOT
// affected by this table: the lint's `rhsIsDisposableClassType` Path 2 still
// returns false for these heads, and the IdentifierExpr RHS bypasses every
// CallExpr disjunct in `constInitializerRejected`.
//
// Population measured 2026-08-09 on d2a3d3e (#2992's authority-differential
// analysis): every name here has a dedicated disposal arm in
// `registerUsingDisposal` and is absent from `kDisposableBuiltinHeadNames`.
//
// **Borrowing collections** (§4, §28.11 trivial-dispose):
//   - Array, Stack, Queue, Set, SortedSet, Dictionary, SortedDictionary,
//     LinkedList — each reclaimed by the chain's collection-family arms
//     (array-backed, dictionary-backed, linked-list). #2695 deliberately
//     excluded the borrowing collections from `kDisposableBuiltinHeadNames`
//     so the lint fires on a bound-then-pinned alias — that decision is
//     preserved (see `kDisposableBuiltinHeadNames`'s own header comment).
//
// **Vtable-less builtins** (§6.4 / §3 / §8.1 / §10.1):
//   - Random — fix #2921; `bz_random_free` (stdlib §3 trivial-dispose).
//   - Atomic, AtomicInteger — fix #2973; `bz_atomic_dispose`.
//   - Stopwatch — §8.1 (#2025); `bz_stopwatch_free`.
//   - Buffer — #545; element-buffer free.
//
// Adding a new name here is a soundness-adjacent change: the chain must
// have an arm that reclaims it, or it stays a leak (the end-of-chain guard
// in `registerUsingDisposal` catches future omissions of any vtable-less
// builtin, #2992). The lint's two predicates (`isDisposableBuiltinHeadName`
// and `isChainClaimedBuiltinHeadName`) are EXPLICITLY disjoint — the former
// is the lint's narrow table, the latter is the factory-call-form closure
// for the gap #2695 created.  // per §28.3 / §28.9 / §28.10, fix #3017
//
// fix #2269 (§28.9, v0.240): `StringBuilder` joins the chain-claimed set —
// it is chain-claimed by the codegen auto-drop chain (registerUsingDisposal
// routes it through the borrowing-`Array<string>` free with the string-
// element release thunk, per #1185 / LlvmBackend_Setup.cpp:15725). The spec
// classifies StringBuilder as trivial-dispose (§28.9 L7363 — "declares no
// destructor and no `owned` fields, and its intrinsic backing storage is
// released by the instance's own free, exactly as a borrowing collection's
// is"), so `localIsTrivialDisposeNewBinding` (Ownership.cpp, the §28.3
// auto-drop warning suppressor) must admit it — which this set is the
// authority for. Removing it from `kDisposableBuiltinHeadNames` (the lint
// table) closed the vacuous-pin over-fire; this addition closes the
// §28.3 auto-drop over-fire. The two predicates remain disjoint: the
// lint's table is the disposable-but-non-trivial-dispose surface, the
// chain's table is the trivial-dispose surface the chain still reclaims.
// // per §28.9 (v0.240), fix #2269
constexpr std::array<std::string_view, 14> kChainClaimedBuiltinHeadNames = {
    "Array", "Stack", "Queue", "Set", "SortedSet",
    "Dictionary", "SortedDictionary", "LinkedList",
    "Random", "Atomic", "AtomicInteger", "Stopwatch", "Buffer",
    "StringBuilder",
};

bool isChainClaimedBuiltinHeadName(std::string_view head) {
    return contains(kChainClaimedBuiltinHeadNames, head);
}  // per §28.3 / §28.9 / §28.10, fix #3017

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

// fix #3119 (spec §8.5, v0.252; stdlib §2.13 rule 1): the QUALIFIED module
// path every reserved BUILT-IN TYPE renders under in §8.5's name-only default
// and stdlib §2.13's `Type.FullName` rule 1. Each entry pairs the simple
// builtin name with the stdlib module path it belongs to per the stdlib's
// section structure (System, System.Text, System.Collections.Generic,
// System.Collections.Linq, System.Threading, System.IO, System.Diagnostics).
//
// The table covers BOTH reserved built-in CLASSES (the §8.5 default's named
// population: `Mutex`, `Array`, `Exception`, …) AND reserved built-in VALUE
// TYPES whose box-thunk name-only default is the §8.5 class case (the
// DateTime/Guid/… family in `System` — their box vtable's slot-3 default is
// the type-name literal). Stdlib §2.13 rule 1's "renders every named
// component qualified by its declaring module path" applies to both.
//
// **Deliberately ABSENT from this table**:
//   - The 14 KEYWORD-ALIASED primitive value types and `string` (spec §3.4:
//     `bool`, `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`,
//     `ulong`, `float`, `double`, `decimal`, `char`, `string`). Per stdlib
//     §2.13 rule 2 ("the keyword-aliased types … render as their lowercase
//     keyword in BOTH properties"), these are NOT qualified — they render
//     lowercase (`int`, `string`, …). The keyword is the canonical name, a
//     recorded exception to qualification.
//   - The reserved built-in INTERFACE heads (`IEnumerable`, `IList`, …,
//     `IReadOnlyList`, `IEvent`, `ISerializer`, etc.). §8.5's name-only
//     default never names an interface — a class-typed receiver's default is
//     the runtime CLASS's name, and a generic-instantiation head renders as
//     the head itself (rule 3's "<args>" suffix is the only addition).
//
// The lookup is the single authority for the module path of a builtin name;
// the eight §8.5 emission sites (`emitVtablelessBuiltinToString`, the
// `bz$extostr$<name>` exception thunks, `valueBuiltinBoxToStringThunk`, the
// box thunk slot-3 default, the class-slot-3 thunk default, the enum default,
// the `classToStringThunk` shared default, and the #2457/#2458/#2459 call-
// site literals) read through it so the spelling stays one literal.
//
// Adding a builtin means editing this table INSTEAD of the eight sites, plus
// a snapshot row in tests/support/builtin_registry_snapshot_test.cpp — the
// same #1414 pattern.
struct BuiltinClassModulePath {
    std::string_view name;
    std::string_view modulePath;
};
constexpr std::array<BuiltinClassModulePath, 79> kBuiltinClassModulePaths = {{
    // stdlib §2 (System) — the §2.5/§2.6 temporal/identity value types are
    // value-shaped (§3.1) but their BOX thunk (`valueBuiltinBoxToStringThunk`)
    // is in the §8.5 default path, so they belong here too.
    {"DateTime", "System"},
    {"TimeSpan", "System"},
    {"DateOnly", "System"},
    {"TimeOnly", "System"},
    {"Guid", "System"},
    // stdlib §2.17 (#1621): built-in EXCEPTIONS — all live in `System` per
    // the stdlib's section structure. Each renders as "System.<Name>".
    {"Exception", "System"},
    {"ArgumentException", "System"},
    {"ArgumentNullException", "System"},
    {"ArgumentOutOfRangeException", "System"},
    {"ArithmeticException", "System"},
    {"OverflowException", "System"},
    {"DivideByZeroException", "System"},
    {"InvalidOperationException", "System"},
    {"DisposedAccessException", "System"},
    {"NotImplementedException", "System"},
    {"NullReferenceException", "System"},
    {"IndexOutOfRangeException", "System"},
    {"KeyNotFoundException", "System"},
    {"MatchFailureException", "System"},
    {"OperationCanceledException", "System"},
    {"TypeInitializationException", "System"},
    {"FormatException", "System"},
    {"StackOverflowException", "System"},
    {"ChannelClosedException", "System"},
    {"ChannelFullException", "System"},
    {"DomainFaultedException", "System"},
    {"SerializationException", "System"},
    {"TimeoutException", "System"},
    {"IOException", "System"},
    {"FileNotFoundException", "System"},
    {"DirectoryNotFoundException", "System"},
    {"EndOfStreamException", "System"},
    {"PathTooLongException", "System"},
    {"ContractViolationException", "System"},
    // stdlib §2.16 (§28.13): Shared<T>, Weak<T> — class heads in `System`.
    {"Shared", "System"},
    {"Weak", "System"},
    // stdlib §2.4: Environment — a static class (no name-only default; §8.5
    // is silent on static classes — listed here only for completeness, not
    // consulted).
    // stdlib §2.20: Result<T, E> — a class-shaped generic in `System`.
    // Not in the §8.5 default path (its declaration carries a ToString
    // override per the stdlib doc).
    // stdlib §2.7: Random — vtable-less built-in class.
    {"Random", "System"},
    // stdlib §3.1 / §3.2 (System.Text): StringBuilder, Encoding concrete
    // subclasses. Encoding concrete subclasses (ASCII/UTF8/…) are
    // implementation-internal and do not surface as user-named types; they
    // are not in this table.
    {"StringBuilder", "System.Text"},
    // stdlib §4 (System.Collections.Generic) — collections and dicts.
    {"Array", "System.Collections.Generic"},
    {"OwningArray", "System.Collections.Generic"},
    {"Stack", "System.Collections.Generic"},
    {"OwningStack", "System.Collections.Generic"},
    {"Queue", "System.Collections.Generic"},
    {"OwningQueue", "System.Collections.Generic"},
    {"Set", "System.Collections.Generic"},
    {"OwningSet", "System.Collections.Generic"},
    {"SortedSet", "System.Collections.Generic"},
    {"Dictionary", "System.Collections.Generic"},
    {"OwningDictionary", "System.Collections.Generic"},
    {"SortedDictionary", "System.Collections.Generic"},
    {"LinkedList", "System.Collections.Generic"},
    {"OwningLinkedList", "System.Collections.Generic"},
    {"LinkedListNode", "System.Collections.Generic"},
    {"OwningBuffer", "System.Collections.Generic"},
    // fix #3249 (§4.19, v0.186): FrozenArray<T> — heap-backed bz_array_t*,
    // System.Collections.Generic per the stdlib section structure; the
    // §8.5 name-only default and stdlib §2.13 rule 1 both consult this table.
    {"FrozenArray", "System.Collections.Generic"},
    // stdlib §4.18 (#1414): DictionaryEntry, DrainedEntry — synthetic
    // TypeRefs (no astNode, importedName-keyed); not reachable from a written
    // type annotation and therefore not in §8.5's default path.
    // stdlib §6 (System.Threading): the §6.2/§6.3 sync primitives, §6.4
    // atomics, §6.6 task-completion sources, §6.10 nursery, §30.15 channels.
    {"Mutex", "System.Threading"},
    {"Lock", "System.Threading"},
    {"Semaphore", "System.Threading"},
    {"SemaphoreToken", "System.Threading"},
    {"Atomic", "System.Threading"},
    {"AtomicInteger", "System.Threading"},
    {"TaskCompletionSource", "System.Threading"},
    {"OwningTaskCompletionSource", "System.Threading"},
    {"TaskScope", "System.Threading"},
    {"CancellationToken", "System.Threading"},
    {"CancellationTokenSource", "System.Threading"},
    {"Channel", "System.Threading"},
    {"ChannelSender", "System.Threading"},
    {"Domain", "System.Threading"},
    // stdlib §7 (System.IO): FileStream.
    {"FileStream", "System.IO"},
    // stdlib §8 (System.Diagnostics): Stopwatch.
    {"Stopwatch", "System.Diagnostics"},
    // stdlib §2.15 (#687): concrete serializers/deserializers; class heads
    // under `System` (the serialization vocab in §2.14 is interfaces).
    {"BinarySerializer", "System"},
    {"BinaryDeserializer", "System"},
    {"JsonSerializer", "System"},
    {"JsonDeserializer", "System"},
    // §24.3 (#1536): EventSource / AsyncEventSource / IEvent / IAsyncEvent —
    // the class heads go under `System`, the interface heads are out.
    {"EventSource", "System"},
    {"AsyncEventSource", "System"},
    // fix #1832 / §6.5 (v0.187): the shared cross-domain publication
    // point. Same module as the instance pair (System.Threading, exposed as
    // `System` per §2.3's auto-import rule); the interface head ISharedEvent
    // is intentionally absent per the §2.13 rule 1 contract.
    {"SharedEventSource", "System"},
    // §2.11 (#2190): Subscription — a class head under `System`.
    {"Subscription", "System"},
}};

// fix #3119: lookup the qualified module path for a reserved built-in name
// (class or value-type whose box-thunk default is the §8.5 case). Returns
// an empty string_view when `name` is NOT in the table — callers test
// `!result.empty()` rather than `find != end` to keep one chokepoint and one
// spelling.
//
// Keyword-aliased primitives (`int`, `string`, etc.) and built-in INTERFACE
// heads deliberately return empty here; those renderings are governed by
// stdlib §2.13 rule 2 (keyword lowercase) and rule 1 (interfaces do not
// render their own name — a class-typed receiver's default is the runtime
// class's name) respectively, not by this module-path lookup.
std::string_view builtinClassModulePathOf(std::string_view name) {
    for (const auto& e : kBuiltinClassModulePaths)
        if (e.name == name) return e.modulePath;
    return {};
}  // per §8.5 / stdlib §2.13 rule 1, fix #3119 — reserved built-in type → module path

// fix #3119: the 14 keyword-aliased built-in types (spec §3.4) — `string`
// plus the 13 primitive value-type keywords. Per stdlib §2.13 rule 2, they
// render as their lowercase keyword in BOTH `Name` and `FullName`; the
// keyword IS the canonical name, a recorded exception to qualification.
// Membership is a negative test (a name NOT here is not a keyword alias)
// so a missed entry silently renders qualified, which a regression test
// catches before any user-visible log line.
constexpr std::array<std::string_view, 14> kKeywordAliasBuiltinNames = {
    "bool", "byte", "sbyte", "short", "ushort", "int", "uint",
    "long", "ulong", "float", "double", "decimal", "char", "string",
};

bool isKeywordAliasBuiltinName(std::string_view n) {
    return contains(kKeywordAliasBuiltinNames, n);
}  // per stdlib §2.13 rule 2 / spec §3.4, fix #3119

// fix #3119: the lowercase source spelling of a `PrimitiveType`. Mirrors
// `primitiveSpelling` in `sema/ExprTypecheck_Exprs.cpp` (kept private to
// sema; this is the codegen-side read for stdlib §2.13 rule 3's generic-
// arg rendering, which needs to spell a `Primitive` type argument as its
// keyword). `Object` is the only non-lowercase spelling — §3.4 reserves
// no keyword for `Object`, and §2.13 rule 2's "lowercase keyword" wording
// applies only to the 14 keyword-aliased entries.
std::string_view primitiveBuiltinSpelling(int primitive) {
    using P = sema::PrimitiveType;
    switch (static_cast<P>(primitive)) {
        case P::Bool:    return "bool";
        case P::Char:    return "char";
        case P::String:  return "string";
        case P::Byte:    return "byte";
        case P::SByte:   return "sbyte";
        case P::Short:   return "short";
        case P::UShort:  return "ushort";
        case P::Int:     return "int";
        case P::UInt:    return "uint";
        case P::Long:    return "long";
        case P::ULong:   return "ulong";
        case P::Float:   return "float";
        case P::Double:  return "double";
        case P::Decimal: return "decimal";
        case P::Object:  return "Object";
        case P::None:    return "<none>";
    }
    return "<unknown>";
}  // per stdlib §2.13 rule 2 / §3.4, fix #3119

}  // namespace bezel::common
