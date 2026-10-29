#include "common/BuiltinRegistry.hpp"

#include "sema/TypeRef.hpp"   // PrimitiveType (for primitiveBuiltinSpelling, fix #3119)

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

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

// §28.3 row 4 (v0.313, #5123): the plain builtin collection classes.
constexpr std::array<std::string_view, 8> kPlainBuiltinCollectionHeads = {
    "Array", "Dictionary", "Set", "Stack", "Queue", "LinkedList",
    "SortedDictionary", "SortedSet",
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

// #3419 / §29.2 / stdlib §4.12–§4.13: the four ITERATOR-FORMAT ENUMERABLE heads
// — the DECLARED-HEAD axis of the enumerable-wrap grid. A subset of
// `kIterableInterfaceHeads` (the four ENUMERATOR heads are excluded: an
// enumerator is what a wrap produces, not what one is created for). See the
// header for why a hand-spelled copy of this list is a defect generator.
constexpr std::array<std::string_view, 4> kIterFormatEnumerableHeads = {
    "IEnumerable", "IOwningEnumerable",
    "IAsyncEnumerable", "IOwningAsyncEnumerable",
};

// The SYNCHRONOUS half — the two heads the §29.2 `for` desugaring and the
// synchronous wrap creators admit.
constexpr std::array<std::string_view, 3> kSyncIterFormatEnumerableHeads = {
    "IEnumerable", "IOwningEnumerable", "IOrderedEnumerable",
};

// #3419 / §29.2: the four ITERATOR-FORMAT ENUMERATOR heads — the complement
// of `kIterFormatEnumerableHeads` within the eight iterator/enumerator heads.
// An enumerator is what a §29.2 wrap PRODUCES (the MoveNext/Current/
// TakeCurrent vtable contract), so a receiver already typed at one of these
// four is the vtable-backed object the wrap machinery hands out, not
// something a wrap decision is made about. See the header for the routing/
// dispatch sites (MoveNext, TakeCurrent, MoveNextAsync, explicit Dispose,
// the declaration-less §8.3 identity-Equals/GetHashCode fallback) that ask
// this question rather than the wrap-eligibility one above.
constexpr std::array<std::string_view, 4> kIterFormatEnumeratorHeads = {
    "IEnumerator", "IOwningEnumerator",
    "IAsyncEnumerator", "IOwningAsyncEnumerator",
};

// The SYNCHRONOUS half of the above — the two heads the vtable-slot MoveNext
// / TakeCurrent / explicit-Dispose dispatch sites admit. The async twins take
// the MoveNextAsync vtable shape and its own dispatch (`LlvmBackend_Async.cpp`),
// a disjoint lowering path. Exactly analogous to `kSyncIterFormatEnumerableHeads`
// above, the sync half of the sibling (Enumerable-format) question.
constexpr std::array<std::string_view, 2> kSyncIterFormatEnumeratorHeads = {
    "IEnumerator", "IOwningEnumerator",
};

// fix #3606 / §29.1 / §29.3: the SYNCHRONOUS half of
// `kOwningIterableInterfaceHeads` above — the two owning iterator-method
// return heads whose declared shape carries the §29.1 owning contract. The
// two async owning heads are excluded (they take the async iterator emitter,
// a disjoint lowering path in `LlvmBackend_Async.cpp`).
constexpr std::array<std::string_view, 2> kSyncOwningIterableInterfaceHeads = {
    "IOwningEnumerable", "IOwningEnumerator",
};

// fix #3468 / §29.2 / stdlib §4.12–§4.13 / §4.5 / §4.14: the SIX iteration-
// interface heads the §29.2 builtin `for-in` desugaring
// (`Emitter::lowerIEnumerableBuiltinFor`) handles — the two iterator-format
// enumerable heads plus the four richer read/collection interfaces whose
// runtime value is still one of the iteration representations. A source typed
// at any OTHER head is not this desugaring's business (a user class routes to
// `lowerIEnumerableFor`, a concrete collection to `lowerArrayFor` and friends).
//
// This list was spelled by hand in exactly two places — the lowering and the
// frame-slot predicate that DECLARES ITSELF ITS MIRROR — and the mirror had
// drifted in four independent ways (issue 3468). Both now read
// `classifyBuiltinForInSource`, which reads this table.
constexpr std::array<std::string_view, 6> kBuiltinForInEnumerableHeads = {
    "IEnumerable", "IOwningEnumerable",
    "IReadOnlyCollection", "IReadOnlyList", "ICollection", "IList",
};

// #3419 / §29.2 / stdlib §4.4–§4.5 / §4.11 / §4.14: the SOURCE-FAMILY axis —
// the collection heads that take the ARRAY-ENUMERATOR wrap. Deliberately NOT
// `kArrayBackedCollections`: see the header for why `Set`/`OwningSet` (hash
// backing → snapshot) and `SortedSet` (§4.18 ordered iteration) must stay out.
constexpr std::array<std::string_view, 7> kArrayEnumeratorWrapSources = {
    "Array", "OwningArray", "OwningBuffer",
    "Stack", "OwningStack", "Queue", "OwningQueue",
};

// fix #3419 / §29.2 / §29.11 / §28.6: the SIX iterator-format HEADS that the
// §29.2 SYNC `using var` admission list recognises — all four iterator-format
// ENUMERABLE heads (sync + async, owning + non-owning) plus the two sync
// ENUMERATOR heads (`IEnumerator<T>`, `IOwningEnumerator<T>`). The two ASYNC
// enumerator heads are deliberately excluded: they take the `await using`
// path (`LlvmBackend_Async.cpp`), a disjoint lowering.
//
// `LlvmBackend_Setup.cpp`'s `using var` handling (the #3418 site) used to spell
// this six-name list by hand at line 15655; that was the fifth independent
// spelling of the enumerable-wrap grid in this run (the fifth point repair
// before the structural fix that #3419 asks for). Every spelling has been the
// SAME question — "does this `using var` local's declared head admit the sync
// iterator-format handling?" — so a single predicate is what every consumer
// should read. Sites that ask a DIFFERENT question (routing through the
// MoveNext / TakeCurrent / GetAsyncEnumerator vtable slots, etc.) consult
// narrower head subsets and do NOT use this predicate; the
// `check_enumerable_grid_spellings` ratchet still flags their chains as
// classH drift-risk so a future head addition cannot silently miss them.
// per §29.2 / §29.11 / §28.6, #3419
constexpr std::array<std::string_view, 6> kIteratorFormatDispatchHeads = {
    "IEnumerable", "IOwningEnumerable",
    "IEnumerator", "IOwningEnumerator",
    "IAsyncEnumerable", "IOwningAsyncEnumerable",
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

// fix #6265 / #6267 (§3.4, stdlib §4.4–§4.11, §4.18): the isArrLike family of
// collection TYPE NAMES that a bare-name static access (`Array.Foo(x)`,
// `Array.Foo;`) is checked against. Consumed via isArrLikeCollectionName.
constexpr std::array<std::string_view, 11> kArrLikeCollectionNames = {
    "Array", "OwningArray", "Stack", "OwningStack", "Queue", "OwningQueue",
    "Set", "OwningSet", "LinkedList", "OwningLinkedList", "SortedSet",
};

// #2809 / §32.5 / stdlib §10.1–§10.3: the FFI-boundary BUFFER classes. See the
// header for why the set exists and who consults it. `IntPtr`/`UIntPtr` are
// §32.7 pointer-sized integer VALUE types, not buffers, and stay out.
constexpr std::array<std::string_view, 3> kFfiBufferBuiltins = {
    "CString", "WString", "RawBytes",
};

// fix #4126 (stdlib §6.1/§8.1, v0.295): see the header for the full
// rationale. Two builtin heap-pointer classes sharing the FFI-buffer trio's
// exact defect shape (no vtable, no dedicated Object-contract interceptor),
// kept as their own row rather than folded into `kFfiBufferBuiltins` (which
// also drives FFI-specific malloc/free disposal and member dispatch that
// does not apply to either of these).
constexpr std::array<std::string_view, 2> kNoOverrideIdentityBuiltins = {
    "Stopwatch", "CancellationToken",
};

// The heap-pointer reference builtins shared by BOTH codegen rep paths in
// LlvmBackend_Layout.cpp (see header).  This is the intersection of the two
// pre-refactor hand-lists: OwningBuffer (astNode-only) and the FFI class names
// (imported-only) are NOT here — they stay as explicit deltas at the sites.
constexpr std::array<std::string_view, 73> kHeapPointerReferenceBuiltins = {
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
    // §2.11 / §24 subscription; §6.1/§30.8 cancellation; §6.10 nursery
    "Subscription", "CancellationToken", "CancellationTokenSource", "TaskScope",
    // §28.13 / §2.16 Shared/Weak; §30.14/§30.15 channels; §5 groupings
    "Shared", "Weak", "Channel", "ChannelSender", "IGrouping",
    // issue #4178: IOrderedEnumerable<T> — the OrderBy/OrderByDescending
    // result interface (stdlib §5). Same bz_lazy_t* ptr rep as IEnumerable
    // above and its sibling IGrouping (both already in this table); this
    // was the actual root cause of "Function return type does not match
    // operand type of return inst! ret ptr ... / i32" — an IR-verifier
    // failure, not a clean diagnostic — on any function whose declared
    // return type is an EXPLICIT `IOrderedEnumerable<T>` annotation:
    // lowerTypeRef's Named case fell through both this table's astNode-path
    // and imported-name-path checks to the i32 default, mismatching the
    // ptr the LINQ runtime actually produces.
    "IOrderedEnumerable",
    // §24 event sources; §6.5 / fix #1536: EventSource<T>/AsyncEventSource<T>
    // (the producer-side face, §24) are heap-allocated like the IEvent<T>/
    // IAsyncEvent<T> interfaces above — same ptr rep, no user TypeDecl.
    //
    // fix #1832 / §6.5 (v0.187): the cross-domain publication-point pair
    // ISharedEvent<T> / SharedEventSource<T> share the §24 same ptr rep (a
    // bz_event_source_t*; a real synchronization discipline around the
    // subscriber list is recorded for a later slice and is the runtime-side
    // contract — the type-name registration here just admits them). Both names
    // are heap-pointer references with no user TypeDecl, exactly like the
    // instance pair above.
    "IEvent", "IAsyncEvent", "ISharedEvent",
    "EventSource", "AsyncEventSource", "SharedEventSource",
    // §6.2/§6.3 sync primitives
    "Mutex", "Lock", "Semaphore", "SemaphoreToken",
    // §6.6 task-completion sources; §6.4 atomics; #545 __Buffer
    "TaskCompletionSource", "OwningTaskCompletionSource",
    "Atomic", "AtomicInteger", "__Buffer",
    // §2.14–2.15 serialization vocabulary + concrete handles (#1390)
    "ISerializer", "IDeserializer", "ISerializable", "IDeserializable",
    "BinarySerializer", "BinaryDeserializer", "JsonSerializer",
    "JsonDeserializer",
    // fix #4326 (stdlib §2.10/§2.18, §28.2/§28.12/§18.5): IDisposable,
    // IAsyncDisposable and IFormattable are null-declaration builtin
    // interfaces (`Resolver::builtinTypeArity`, TypeResolution_Names.cpp,
    // all three arity-0) with NO user TypeDecl — the exact shape
    // IOrderedEnumerable was missing this table for (issue #4178, directly
    // above). Naming any of the three as a parameter, return, field, or
    // local-annotation type IR-verifier-crashed ("Call parameter type does
    // not match function signature!" / the return/field/local twins of
    // that message) because lowerTypeRef's Named case fell through both
    // this table's astNode-path and imported-name-path checks
    // (LlvmBackend_Layout.cpp) to the i32 default, mismatching the `ptr`
    // every conforming value (any class instance, for IDisposable/
    // IAsyncDisposable; any IFormattable-implementing class) actually is.
    // MEASURED (issue #4326, this file's own parent `f99d7c4`, clean tree,
    // `bezelc` rebuilt directly): a parameter typed `IDisposable`, a
    // parameter typed `IFormattable`, and — found by this fix's own
    // mechanical derivation over the full null-declaration-interface
    // population, NOT named in the filed issue — a parameter typed
    // `IAsyncDisposable` all reproduced the identical crash.
    "IDisposable", "IAsyncDisposable", "IFormattable",
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
constexpr std::array<std::string_view, 25> kBuiltinInterfaceHeads = {
    // §29 iteration / collection interfaces
    "IEnumerable", "IEnumerator", "IOwningEnumerable", "IOwningEnumerator",
    "IAsyncEnumerable", "IAsyncEnumerator", "IOwningAsyncEnumerable",
    "IOwningAsyncEnumerator", "IReadOnlyCollection", "IReadOnlyList",
    "ICollection", "IList", "IOwningCollection", "IOwningList",
    // §24 / §6.5 event faces; §5 groupings
    "IEvent", "IAsyncEvent", "ISharedEvent", "IGrouping",
    // §2.14–§2.15 serialization vocabulary
    "ISerializer", "IDeserializer", "ISerializable", "IDeserializable",
    // fix #4326 (§28.2/§28.12/§18.5): IDisposable, IAsyncDisposable,
    // IFormattable — a NECESSARY companion of that fix's
    // `kHeapPointerReferenceBuiltins` addition, not a second, independent
    // change. Adding these three to the heap-pointer table made them newly
    // `isReservedBuiltinTypeName`-true; `isVtableLessBuiltinClassName` is
    // DERIVED as "reserved MINUS this table" (see the header), so leaving
    // them out of THIS table would have made `isVtableLessBuiltinClassName`
    // silently start answering `true` for all three — exactly the #2921/
    // #2923 wild-jump/double-free shape this table exists to prevent,
    // applied to receivers whose runtime value may be ANY user class (every
    // class implicitly implements `IDisposable`, stdlib §2.10; a class may
    // explicitly implement `IAsyncDisposable`/`IFormattable`, stdlib §2.10/
    // §2.18 — measured: `class Money: IFormattable ... end Money;` compiles
    // and a `var f: IFormattable := new Money(...);` dispatches through it).
    // MEASURED without this addition (fix #4326's own investigation, same
    // rebuilt `bezelc`): `d.Dispose()` through an `owned d: IDisposable`
    // parameter/local silently did NOT run the real class's destructor (no
    // crash, no diagnostic — `isVtableLessBuiltinClassName("IDisposable")`
    // was wrongly `true`, routing the call through the vtable-less path
    // instead of the real dynamic-dispatch slot), and `$"{f:X}"` on an
    // `IFormattable`-typed receiver printed the literal text "IFormattable"
    // instead of dispatching to the real `ToString(format)` override
    // (`LlvmBackend_String.cpp`'s #2969 routing falls back to the vtable-
    // less-builtin renderer). Both are FIXED by this addition (see the
    // regression tests in
    // idisposable_iasyncdisposable_iformattable_heap_ptr_4326_test.cpp).
    "IDisposable", "IAsyncDisposable", "IFormattable",
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

// fix #3398 (spec §22.5, v0.283; stdlib §1 Conventions — *Window
// parameters*), widened and made overload-aware by fix #3424, and joined by
// the constructor twin fix #3425 (§17.7): the built-in parameter-name tables.
// See the header for the contract, and for why a MISS is a rejection rather
// than permission.
//
// Every row is transcribed from the standard-library document's own
// declaration; nothing here is inferred. `names` holds `total` entries in
// declaration order (the trailing slots of a shorter row are unused);
// `required` counts the LEADING parameters that declare no default, so one
// row answers for every admissible argument count of an overload with §22.5
// optional parameters.
// ONE row of the built-in parameter table: a DECLARED FORM of a built-in
// member (or of a built-in constructor, whose `member` is empty), carrying
// the parameter NAME (\u00a722.5), the parameter TYPE (\u00a75.1) and the
// parameter NULLABILITY (\u00a77.3) of that form.
//
// This is the ONE table the built-in parameter facts live in. It replaced
// three independent ones \u2014 the parameter-NAME rows (74 rows across
// string / Encoding / StringBuilder / Math / Convert and the 45 constructor
// rows), the 93-row NULLABILITY table, and (for the surface the admission
// gate owns) the parameter-TYPE rows that no table carried at all. A form's
// name, type and nullability are ONE row, so the consumers cannot read two
// tables that disagree.
//
// `kinds[p]` is the declared parameter TYPE at position `p` as a
// `BuiltinParamType` bit set; `kBPTAny` (the zero value) means "no type
// opinion at this position", so a surface that has not been transcribed yet
// costs a rejection that does not fire, never a false one.
//
// `nonNullable` is a bit mask: bit `p` set means the parameter at position `p`
// is declared non-nullable in EVERY overload of this argument count.
//
// Every row is transcribed from the standard-library document's own
// declaration; nothing here is inferred. The standard-library section cited
// per group.
struct BuiltinParamRow {
    std::string_view head;
    std::string_view member;   // empty for a CONSTRUCTOR row
    std::uint8_t total;        // the form's parameter count
    std::uint8_t required;     // the LEADING parameters that declare no default
    std::uint8_t nonNullable;  // bit `p` set: position `p` is non-nullable
    std::array<std::string_view, 8> names;   // the \u00a722.5 declared names
    std::array<std::uint32_t, 8> kinds;      // the declared parameter types
    // The DECLARED SIGNATURE, exactly as the standard library writes it \u2014
    // "`Parse(s: string, radix: int): int`" \u2014 with the RETURN type the
    // declaration carries. Empty for the rows the admission gate does not
    // quote. A diagnostic that names a built-in's declared form quotes the
    // standard library's own words, not a re-spelling of them.
    std::string_view sig;
};

constexpr std::array<BuiltinParamRow, 345> kBuiltinParams = {{
    // fix #7421 (stdlib §9.6, §5.1, §7.3, v0.345): the §9.6 `String` head's
    // DECLARED PARAMETER TYPES, transcribed from the standard library's own
    // listing.
    //
    // The `kinds` column is the §5.1 admission question's own vocabulary —
    // one bit per DECLARED type, so a position whose declared overload set is
    // `char` OR `string` carries both — and the `nonNullable` column is the
    // §7.3 nullability of the position, bit `p` set meaning "declared
    // non-nullable in every overload of this argument count". The `sig` column
    // is the DECLARED FORM the diagnostic quotes, in the standard library's own
    // words (" / " separates two overloads of the same argument count).
    //
    // WHY THIS TABLE AND NOT ANOTHER ONE. This is the ONE built-in parameter
    // table (`kBuiltinParams`): the §9.6 gate, the §7.3 nullability gate and
    // the §5.1 admission gate all read THIS row, so the parameter NAME
    // (§22.5), the parameter TYPE (§5.1) and the parameter NULLABILITY (§7.3)
    // of a form cannot disagree. It is not a sixth hand-maintained mirror: the
    // §3.1 `StringBuilder` surface, the §2.2 / §2.3 `Math` / `Convert` surface
    // and the §2.5 temporal surface were folded into it before this change.
    //
    // THE DERIVATION IS MECHANICAL AND IS CHECKED IN:
    // `scripts/check_builtin_declared_params.py` parses stdlib §9.6 out of
    // `docs/Bezel_Standard_Library.md` and asserts that every declared form's
    // parameter TYPES and NULLABILITY are the ones transcribed here — a
    // BUILD-FREE SOURCE ratchet, wired into `scripts/gate_full_cycle.sh`
    // beside `check_string_arity_table.py` and `check_string_mint_methods.py`.
    // A change to §9.6's listing therefore fails the gate until this table is
    // brought back into step with the document.
    //
    // A position with NO type opinion (`kBPTAny`, the zero value) is the
    // fail-safe direction: a miss costs a rejection that does not fire, never
    // a false one. `From`'s one-argument form is such a position — stdlib
    // §9.6 declares `From(value: char)`, `From(value: CString)` and
    // `From(value: WString)` at that argument count, and the FFI types have no
    // bit in the vocabulary (the compiler's own `String.From` gate owns that
    // form's argument type).
    {"string", "From", 1, 1, 0x00, {"value", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"string", "From", 3, 3, 0x07, {"buffer", "start", "length", "", "", "", "", ""}, {kBPTArrayChar, kBPTInt, kBPTInt, 0, 0, 0, 0, 0}, "From(buffer: Array<char>, start: int, length: int): string"},
    {"string", "FromCStringPointer", 1, 1, 0x00, {"p", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"string", "FromWStringPointer", 1, 1, 0x00, {"p", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    // The `Join` positions carry NO type opinion: the dedicated `string.Join`
    // gate (fix #7150 / #7167 / #7191) already asks the §5.1 question against
    // the DECLARED `separator: string` and `items: IEnumerable<string>`, with
    // the full §5.1 / §12.9 machinery (an `IEnumerable<string>`-typed
    // argument is admitted there, and the covariance rules decide it). A
    // second, weaker test here would REFUSE a legal argument the dedicated
    // gate admits, so the position is left to its owner.
    {"string", "Join", 2, 2, 0x03, {"separator", "items", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "Join(separator: string, items: IEnumerable<string>): string"},
    {"string", "Repeat", 2, 2, 0x03, {"c", "count", "", "", "", "", "", ""}, {kBPTChar, kBPTInt, 0, 0, 0, 0, 0, 0}, "Repeat(c: char, count: int): string"},
    {"string", "Repeat", 2, 2, 0x03, {"unit", "count", "", "", "", "", "", ""}, {kBPTString, kBPTInt, 0, 0, 0, 0, 0, 0}, "Repeat(unit: string, count: int): string"},
    {"string", "IsNullOrWhiteSpace", 1, 1, 0x00, {"value", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "IsNullOrWhiteSpace(value: string?): bool"},
    {"string", "Contains", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTChar | kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Contains(value: char): bool / Contains(value: string): bool"},
    {"string", "StartsWith", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "StartsWith(value: string): bool"},
    {"string", "EndsWith", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "EndsWith(value: string): bool"},
    {"string", "IndexOf", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTChar | kBPTString, 0, 0, 0, 0, 0, 0, 0}, "IndexOf(value: char, startIndex: int := 0): int / IndexOf(value: string, startIndex: int := 0): int"},
    {"string", "IndexOf", 2, 2, 0x03, {"value", "startIndex", "", "", "", "", "", ""}, {kBPTChar | kBPTString, kBPTInt, 0, 0, 0, 0, 0, 0}, "IndexOf(value: char, startIndex: int := 0): int / IndexOf(value: string, startIndex: int := 0): int"},
    {"string", "LastIndexOf", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTChar | kBPTString, 0, 0, 0, 0, 0, 0, 0}, "LastIndexOf(value: char): int / LastIndexOf(value: string): int"},
    {"string", "Substring", 1, 1, 0x01, {"startIndex", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "Substring(startIndex: int): string"},
    {"string", "Substring", 2, 2, 0x03, {"startIndex", "length", "", "", "", "", "", ""}, {kBPTInt, kBPTInt, 0, 0, 0, 0, 0, 0}, "Substring(startIndex: int, length: int): string"},
    {"string", "AsSpan", 1, 1, 0x01, {"start", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "AsSpan(start: int): ReadOnlyStringSpan"},
    {"string", "AsSpan", 1, 1, 0x01, {"r", "", "", "", "", "", "", ""}, {kBPTRangeInt, 0, 0, 0, 0, 0, 0, 0}, "AsSpan(r: Range<int>): ReadOnlyStringSpan"},
    {"string", "AsSpan", 2, 2, 0x03, {"start", "length", "", "", "", "", "", ""}, {kBPTInt, kBPTInt, 0, 0, 0, 0, 0, 0}, "AsSpan(start: int, length: int): ReadOnlyStringSpan"},
    {"string", "Trim", 1, 1, 0x01, {"chars", "", "", "", "", "", "", ""}, {kBPTEnumerableChar, 0, 0, 0, 0, 0, 0, 0}, "Trim(chars: IEnumerable<char>): string"},
    {"string", "PadLeft", 1, 1, 0x01, {"totalWidth", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "PadLeft(totalWidth: int, paddingChar: char := ' '): string"},
    {"string", "PadLeft", 2, 2, 0x03, {"totalWidth", "paddingChar", "", "", "", "", "", ""}, {kBPTInt, kBPTChar, 0, 0, 0, 0, 0, 0}, "PadLeft(totalWidth: int, paddingChar: char := ' '): string"},
    {"string", "PadRight", 1, 1, 0x01, {"totalWidth", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "PadRight(totalWidth: int, paddingChar: char := ' '): string"},
    {"string", "PadRight", 2, 2, 0x03, {"totalWidth", "paddingChar", "", "", "", "", "", ""}, {kBPTInt, kBPTChar, 0, 0, 0, 0, 0, 0}, "PadRight(totalWidth: int, paddingChar: char := ' '): string"},
    {"string", "PadCenter", 1, 1, 0x01, {"totalWidth", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "PadCenter(totalWidth: int, paddingChar: char := ' '): string"},
    {"string", "PadCenter", 2, 2, 0x03, {"totalWidth", "paddingChar", "", "", "", "", "", ""}, {kBPTInt, kBPTChar, 0, 0, 0, 0, 0, 0}, "PadCenter(totalWidth: int, paddingChar: char := ' '): string"},
    {"string", "Insert", 2, 2, 0x03, {"startIndex", "value", "", "", "", "", "", ""}, {kBPTInt, kBPTString, 0, 0, 0, 0, 0, 0}, "Insert(startIndex: int, value: string): string"},
    {"string", "Remove", 1, 1, 0x01, {"startIndex", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "Remove(startIndex: int): string"},
    {"string", "Remove", 2, 2, 0x03, {"startIndex", "length", "", "", "", "", "", ""}, {kBPTInt, kBPTInt, 0, 0, 0, 0, 0, 0}, "Remove(startIndex: int, length: int): string"},
    {"string", "Replace", 2, 2, 0x03, {"oldChar", "newChar", "", "", "", "", "", ""}, {kBPTChar, kBPTChar, 0, 0, 0, 0, 0, 0}, "Replace(oldChar: char, newChar: char): string"},
    {"string", "Replace", 2, 2, 0x03, {"oldValue", "newValue", "", "", "", "", "", ""}, {kBPTString, kBPTString, 0, 0, 0, 0, 0, 0}, "Replace(oldValue: string, newValue: string): string"},
    {"string", "Split", 1, 1, 0x01, {"separator", "", "", "", "", "", "", ""}, {kBPTChar | kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Split(separator: char): owned Array<string> / Split(separator: string): owned Array<string>"},
    {"string", "Split", 1, 1, 0x01, {"separators", "", "", "", "", "", "", ""}, {kBPTArrayChar, 0, 0, 0, 0, 0, 0, 0}, "Split(separators: Array<char>): owned Array<string>"},
    {"string", "CompareTo", 1, 1, 0x01, {"other", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "CompareTo(other: string): int"},
    {"string", "CompareTo", 2, 2, 0x03, {"other", "ignoreCase", "", "", "", "", "", ""}, {kBPTString, kBPTBool, 0, 0, 0, 0, 0, 0}, "CompareTo(other: string, ignoreCase: bool): int"},
    {"string", "Equals", 1, 1, 0x00, {"other", "", "", "", "", "", "", ""}, {kBPTString | kBPTObject, 0, 0, 0, 0, 0, 0, 0}, "Equals(other: string): bool"},
    {"string", "Equals", 2, 2, 0x02, {"other", "ignoreCase", "", "", "", "", "", ""}, {kBPTString | kBPTObject, kBPTBool, 0, 0, 0, 0, 0, 0}, "Equals(other: string?, ignoreCase: bool): bool"},
    {"Encoding", "GetBytes", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Encoding", "GetBytes", 2, 2, 0x00, {"s", "destination", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Encoding", "GetString", 1, 1, 0x00, {"bytes", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Encoding", "GetByteCount", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    // fix #7207-fold (stdlib §3.1, v0.345): the §3.1 `StringBuilder`
    // surface lives HERE — in the ONE declared-signature table — and not in
    // a second parallel table. Before the fold these six rows carried only
    // the §22.5 NAMES, and the declared KINDS and SIGNATURES the diagnostic
    // quotes lived in a separate eight-row table inside the checker; the
    // two could drift (the `AppendLine()` and `Clear()` forms §3.1 declares
    // were in the checker only, and absent here).
    //
    // `kinds[p]` is the declared parameter TYPE (§5.1). §3.1 declares
    // `Append(value: string)` among SEVEN overloads (`string` / `char` /
    // `Object` / `bool` / the numeric primitives / `ReadOnlyStringSpan`), so
    // that position carries NO type opinion (`0`) — a table that has not
    // transcribed a position costs a rejection that does not fire, never a
    // false one. `AppendFormat`'s SECOND position declares
    // `args: Array<Object>`, and the kind vocabulary has no ARRAY bit, so
    // that one position is checked by the checker itself, keyed by the
    // member.
    //
    // fix #7420 (stdlib §3.1, v0.345): `AppendLine`'s VALUE position
    // carries the declared KIND (`kBPTString`). §3.1 declares EXACTLY TWO
    // `AppendLine` forms — `AppendLine(): StringBuilder` and
    // `AppendLine(value: string): StringBuilder` — so there is ONE declared
    // value type, and NO `AppendLine(value: Object)` and NO per-type
    // overloads (unlike `Append`, which has seven). The position was
    // kindless, so `sb.AppendLine(5)`, `sb.AppendLine('c')` and
    // `sb.AppendLine(true)` reached no declared form and were ADMITTED
    // (§22.6: a call with no applicable candidate is a compile error; §5.1:
    // an `int` reaches a `string` parameter only by an explicit
    // conversion). `Insert`'s VALUE position stays kindless for a different
    // reason — §3.1 declares exactly one `Insert` form too, but #6807's own
    // dedicated check already rejects a non-`string` value there, and two
    // diagnostics for one argument would be noise.
    {"StringBuilder", "Append", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "Append(value: string): StringBuilder"},
    {"StringBuilder", "AppendLine", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "AppendLine(): StringBuilder"},
    {"StringBuilder", "AppendLine", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "AppendLine(value: string): StringBuilder"},
    {"StringBuilder", "AppendFormat", 2, 2, 0x01, {"format", "args", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "AppendFormat(format: string, args: Array<Object>): StringBuilder"},
    {"StringBuilder", "Insert", 2, 2, 0x02, {"index", "value", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "Insert(index: int, value: string): StringBuilder"},
    {"StringBuilder", "Remove", 2, 2, 0x00, {"startIndex", "length", "", "", "", "", "", ""}, {kBPTInt, kBPTInt, 0, 0, 0, 0, 0, 0}, "Remove(startIndex: int, length: int): StringBuilder"},
    {"StringBuilder", "Replace", 2, 2, 0x03, {"oldValue", "newValue", "", "", "", "", "", ""}, {kBPTString, kBPTString, 0, 0, 0, 0, 0, 0}, "Replace(oldValue: string, newValue: string): StringBuilder"},
    {"StringBuilder", "Clear", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "Clear(): StringBuilder"},
    {"Math", "Abs", 1, 1, 0x01, {"x", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "Abs(x: int): int"},
    {"Math", "Abs", 1, 1, 0x01, {"", "", "", "", "", "", "", ""}, {kBPTLong, 0, 0, 0, 0, 0, 0, 0}, "Abs(x: long): long"},
    {"Math", "Abs", 1, 1, 0x01, {"", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "Abs(x: double): double"},
    {"Math", "Abs", 1, 1, 0x01, {"", "", "", "", "", "", "", ""}, {kBPTDecimal, 0, 0, 0, 0, 0, 0, 0}, "Abs(x: decimal): decimal"},
    {"Math", "Min", 2, 2, 0x00, {"a", "b", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Math", "Max", 2, 2, 0x00, {"a", "b", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Math", "Clamp", 3, 3, 0x00, {"value", "low", "high", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Math", "Sqrt", 1, 1, 0x01, {"x", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "Sqrt(x: double): double"},
    {"Math", "Pow", 2, 2, 0x03, {"x", "y", "", "", "", "", "", ""}, {kBPTDouble, kBPTDouble, 0, 0, 0, 0, 0, 0}, "Pow(x: double, y: double): double"},
    {"Math", "Floor", 1, 1, 0x01, {"x", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "Floor(x: double): double"},
    {"Math", "Floor", 1, 1, 0x01, {"", "", "", "", "", "", "", ""}, {kBPTDecimal, 0, 0, 0, 0, 0, 0, 0}, "Floor(x: decimal): decimal"},
    {"Math", "Ceiling", 1, 1, 0x01, {"x", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "Ceiling(x: double): double"},
    {"Math", "Ceiling", 1, 1, 0x01, {"", "", "", "", "", "", "", ""}, {kBPTDecimal, 0, 0, 0, 0, 0, 0, 0}, "Ceiling(x: decimal): decimal"},
    {"Math", "Round", 1, 1, 0x01, {"", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "Round(x: double, digits: int := 0): double"},
    {"Math", "Round", 2, 1, 0x03, {"x", "digits", "", "", "", "", "", ""}, {kBPTDouble, kBPTInt, 0, 0, 0, 0, 0, 0}, "Round(x: double, digits: int := 0): double"},
    {"Math", "Truncate", 1, 1, 0x01, {"x", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "Truncate(x: double): double"},
    {"Math", "Truncate", 1, 1, 0x01, {"", "", "", "", "", "", "", ""}, {kBPTDecimal, 0, 0, 0, 0, 0, 0, 0}, "Truncate(x: decimal): decimal"},
    {"Math", "Sign", 1, 1, 0x01, {"x", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Math", "Fmod", 2, 2, 0x03, {"x", "y", "", "", "", "", "", ""}, {kBPTDouble, kBPTDouble, 0, 0, 0, 0, 0, 0}, "Fmod(x: double, y: double): double"},
    {"Math", "IEEERemainder", 2, 2, 0x03, {"x", "y", "", "", "", "", "", ""}, {kBPTDouble, kBPTDouble, 0, 0, 0, 0, 0, 0}, "IEEERemainder(x: double, y: double): double"},
    {"Math", "DivRem", 3, 2, 0x00, {"a", "b", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Math", "DivRem", 3, 2, 0x00, {"a", "b", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Convert", "ToInt32", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToInt32(value: string): int"},
    {"Convert", "ToInt64", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToInt64(value: string): long"},
    {"Convert", "ToDouble", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToDouble(value: string): double"},
    {"Convert", "ToDecimal", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToDecimal(value: string): decimal"},
    {"Convert", "ToBoolean", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToBoolean(value: string): bool"},
    {"Convert", "ToString", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "ToString(value: int): string"},
    {"Convert", "ToString", 1, 1, 0x01, {"", "", "", "", "", "", "", ""}, {kBPTLong, 0, 0, 0, 0, 0, 0, 0}, "ToString(value: long): string"},
    {"Convert", "ToString", 1, 1, 0x01, {"", "", "", "", "", "", "", ""}, {kBPTFloat, 0, 0, 0, 0, 0, 0, 0}, "ToString(value: float): string"},
    {"Convert", "ToString", 1, 1, 0x01, {"", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "ToString(value: double): string"},
    {"Convert", "ToString", 1, 1, 0x01, {"", "", "", "", "", "", "", ""}, {kBPTDecimal, 0, 0, 0, 0, 0, 0, 0}, "ToString(value: decimal): string"},
    {"Convert", "ToString", 1, 1, 0x01, {"", "", "", "", "", "", "", ""}, {kBPTBool, 0, 0, 0, 0, 0, 0, 0}, "ToString(value: bool): string"},
    {"Convert", "ToString", 2, 2, 0x01, {"value", "toBase", "", "", "", "", "", ""}, {kBPTInt, kBPTInt, 0, 0, 0, 0, 0, 0}, "ToString(value: int, toBase: int): string"},
    {"Convert", "ToString", 2, 2, 0x01, {"", "", "", "", "", "", "", ""}, {kBPTLong, kBPTInt, 0, 0, 0, 0, 0, 0}, "ToString(value: long, toBase: int): string"},
    {"Convert", "TryToInt32", 2, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryToInt32(value: string, out result: int): bool"},
    {"Convert", "TryToInt64", 2, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryToInt64(value: string, out result: long): bool"},
    {"Convert", "TryToDouble", 2, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryToDouble(value: string, out result: double): bool"},
    {"Convert", "TryToDecimal", 2, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryToDecimal(value: string, out result: decimal): bool"},
    {"Convert", "TryToBoolean", 2, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryToBoolean(value: string, out result: bool): bool"},
    // stdlib §8.2 — the `Debug` class. It is a PURE-STATIC utility class with
    // exactly two members, and its whole surface is here: `WriteLine(message:
    // string)` and `Assert(condition: bool, message: string := "")`. The
    // `Assert` forms are two rows because the table keys a row on the
    // ARGUMENT COUNT (§22.5): the one-argument form and the two-argument
    // form. Before these rows the `Debug` head was in NO table at all, so
    // nothing checked a `Debug` call against §8.2: `Debug.WriteLine(<a
    // StringBuilder>)` compiled and wrote an EMPTY LINE, `Debug.WriteLine(5)`
    // aborted the compiler in the LLVM verifier ("Call parameter type does
    // not match function signature!"), `Debug.WriteLine("a", "b")` DROPPED the
    // surplus argument, and `Debug.Assert(<a string>)` aborted it too.
    {"Debug", "WriteLine", 1, 1, 0x01, {"message", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "WriteLine(message: string)"},
    // fix #7419 (stdlib §8.2, v0.345): the ONE-argument `Assert` row quoted
    // `Assert(condition: bool)` — a TRUNCATED declared signature. stdlib §8.2
    // declares ONE signature, `Assert(condition: bool, message: string := "")`
    // (docs/Bezel_Standard_Library.md:5123, and stdlib §1's "Optional trailing
    // argument" rule: "an optional trailing argument with a nameable default
    // ... is one signature carrying a `:=` default, not an overload pair
    // (... `Debug.Assert(condition, message := "")`)"), so the diagnostic told
    // the reader that §8.2 declares a ONE-parameter `Assert`. The row is
    // keyed on the ARGUMENT COUNT (the table's shape), so the two rows are
    // the two CALL FORMS of that one signature; the one-argument form now
    // quotes the declared signature it is a call form of.
    {"Debug", "Assert", 1, 1, 0x01, {"condition", "", "", "", "", "", "", ""}, {kBPTBool, 0, 0, 0, 0, 0, 0, 0}, "Assert(condition: bool, message: string := \"\")"},
    {"Debug", "Assert", 2, 2, 0x03, {"condition", "message", "", "", "", "", "", ""}, {kBPTBool, kBPTString, 0, 0, 0, 0, 0, 0}, "Assert(condition: bool, message: string)"},
    {"DateTime", "", 3, 3, 0x00, {"year", "month", "day", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"DateTime", "", 8, 6, 0x00, {"year", "month", "day", "hour", "minute", "second", "millisecond", "kind"}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"DateOnly", "", 3, 3, 0x00, {"year", "month", "day", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"TimeOnly", "", 4, 2, 0x00, {"hour", "minute", "second", "millisecond", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"StringBuilder", "", 1, 1, 0x01, {"capacity", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"StringBuilder", "", 1, 1, 0x01, {"initial", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"ReadOnlyStringSpan", "", 3, 3, 0x00, {"Source", "Start", "Length", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"ExceptionInfo", "", 3, 3, 0x00, {"TypeName", "Message", "StackTrace", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Span", "", 3, 3, 0x00, {"Buffer", "Start", "Length", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"ReadOnlySpan", "", 3, 3, 0x00, {"Buffer", "Start", "Length", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"ArraySpan", "", 3, 3, 0x00, {"Source", "Start", "Length", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"ReadOnlyArraySpan", "", 3, 3, 0x00, {"Source", "Start", "Length", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Array", "", 1, 1, 0x00, {"capacity", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Array", "", 2, 2, 0x00, {"size", "fill", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"OwningArray", "", 1, 1, 0x00, {"capacity", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"OwningBuffer", "", 1, 1, 0x00, {"length", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"OwningBuffer", "", 2, 2, 0x00, {"length", "init", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"LinkedList", "", 1, 1, 0x00, {"source", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Dictionary", "", 1, 1, 0x00, {"capacity", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Dictionary", "", 1, 1, 0x00, {"comparer", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Dictionary", "", 2, 2, 0x00, {"capacity", "comparer", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"OwningDictionary", "", 1, 1, 0x00, {"capacity", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"OwningDictionary", "", 1, 1, 0x00, {"comparer", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"OwningDictionary", "", 2, 2, 0x00, {"capacity", "comparer", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Set", "", 1, 1, 0x00, {"capacity", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Set", "", 1, 1, 0x00, {"comparer", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Set", "", 2, 2, 0x00, {"capacity", "comparer", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"OwningSet", "", 1, 1, 0x00, {"capacity", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"OwningSet", "", 1, 1, 0x00, {"comparer", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"OwningSet", "", 2, 2, 0x00, {"capacity", "comparer", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Shared", "", 1, 1, 0x00, {"value", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Random", "", 1, 1, 0x00, {"seed", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Semaphore", "", 1, 1, 0x00, {"initialCount", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Semaphore", "", 2, 2, 0x00, {"initialCount", "maxCount", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Channel", "", 1, 1, 0x00, {"capacity", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Exception", "", 2, 2, 0x00, {"message", "cause", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"ArgumentException", "", 2, 2, 0x00, {"message", "paramName", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"ArgumentOutOfRangeException", "", 2, 2, 0x00, {"message", "paramName", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"TimeoutException", "", 2, 2, 0x00, {"message", "inner", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"FileStream", "", 2, 2, 0x00, {"path", "mode", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"FileStream", "", 3, 3, 0x00, {"path", "mode", "access", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"JsonSerializer", "", 1, 1, 0x00, {"options", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"JsonDeserializer", "", 1, 1, 0x00, {"input", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"JsonDeserializer", "", 2, 2, 0x00, {"input", "options", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"BinaryDeserializer", "", 2, 1, 0x00, {"input", "ignoreUnknownFields", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"SByte", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): sbyte"},
    {"SByte", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: ReadOnlyStringSpan): sbyte"},
    {"SByte", "Parse", 2, 2, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, 0, 0, 0, 0, 0, 0}, "Parse(s: string, radix: int): sbyte"},
    {"SByte", "ParseOrNull", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): sbyte?"},
    {"SByte", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: sbyte): bool"},
    {"SByte", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: ReadOnlyStringSpan, out result: sbyte): bool"},
    {"SByte", "TryParse", 3, 3, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, kBPTOut, 0, 0, 0, 0, 0}, "TryParse(s: string, radix: int, out result: sbyte): bool"},
    {"SByte", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"SByte", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"SByte", "ToString", 1, 1, 0x01, {"radix", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "ToString(radix: int): string"},
    {"Byte", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): byte"},
    {"Byte", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: ReadOnlyStringSpan): byte"},
    {"Byte", "Parse", 2, 2, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, 0, 0, 0, 0, 0, 0}, "Parse(s: string, radix: int): byte"},
    {"Byte", "ParseOrNull", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): byte?"},
    {"Byte", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: byte): bool"},
    {"Byte", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: ReadOnlyStringSpan, out result: byte): bool"},
    {"Byte", "TryParse", 3, 3, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, kBPTOut, 0, 0, 0, 0, 0}, "TryParse(s: string, radix: int, out result: byte): bool"},
    {"Byte", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"Byte", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"Byte", "ToString", 1, 1, 0x01, {"radix", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "ToString(radix: int): string"},
    {"Int16", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): short"},
    {"Int16", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: ReadOnlyStringSpan): short"},
    {"Int16", "Parse", 2, 2, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, 0, 0, 0, 0, 0, 0}, "Parse(s: string, radix: int): short"},
    {"Int16", "ParseOrNull", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): short?"},
    {"Int16", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: short): bool"},
    {"Int16", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: ReadOnlyStringSpan, out result: short): bool"},
    {"Int16", "TryParse", 3, 3, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, kBPTOut, 0, 0, 0, 0, 0}, "TryParse(s: string, radix: int, out result: short): bool"},
    {"Int16", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"Int16", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"Int16", "ToString", 1, 1, 0x01, {"radix", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "ToString(radix: int): string"},
    {"UInt16", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): ushort"},
    {"UInt16", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: ReadOnlyStringSpan): ushort"},
    {"UInt16", "Parse", 2, 2, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, 0, 0, 0, 0, 0, 0}, "Parse(s: string, radix: int): ushort"},
    {"UInt16", "ParseOrNull", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): ushort?"},
    {"UInt16", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: ushort): bool"},
    {"UInt16", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: ReadOnlyStringSpan, out result: ushort): bool"},
    {"UInt16", "TryParse", 3, 3, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, kBPTOut, 0, 0, 0, 0, 0}, "TryParse(s: string, radix: int, out result: ushort): bool"},
    {"UInt16", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"UInt16", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"UInt16", "ToString", 1, 1, 0x01, {"radix", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "ToString(radix: int): string"},
    {"Int32", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): int"},
    {"Int32", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: ReadOnlyStringSpan): int"},
    {"Int32", "Parse", 2, 2, 0x01, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, 0, 0, 0, 0, 0, 0}, "Parse(s: string, radix: int): int"},
    {"Int32", "ParseOrNull", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): int?"},
    {"Int32", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: int): bool"},
    {"Int32", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: ReadOnlyStringSpan, out result: int): bool"},
    {"Int32", "TryParse", 3, 3, 0x01, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, kBPTOut, 0, 0, 0, 0, 0}, "TryParse(s: string, radix: int, out result: int): bool"},
    {"Int32", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"Int32", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"Int32", "ToString", 1, 1, 0x01, {"radix", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "ToString(radix: int): string"},
    {"UInt32", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): uint"},
    {"UInt32", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: ReadOnlyStringSpan): uint"},
    {"UInt32", "Parse", 2, 2, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, 0, 0, 0, 0, 0, 0}, "Parse(s: string, radix: int): uint"},
    {"UInt32", "ParseOrNull", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): uint?"},
    {"UInt32", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: uint): bool"},
    {"UInt32", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: ReadOnlyStringSpan, out result: uint): bool"},
    {"UInt32", "TryParse", 3, 3, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, kBPTOut, 0, 0, 0, 0, 0}, "TryParse(s: string, radix: int, out result: uint): bool"},
    {"UInt32", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"UInt32", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"UInt32", "ToString", 1, 1, 0x01, {"radix", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "ToString(radix: int): string"},
    {"Int64", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): long"},
    {"Int64", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: ReadOnlyStringSpan): long"},
    {"Int64", "Parse", 2, 2, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, 0, 0, 0, 0, 0, 0}, "Parse(s: string, radix: int): long"},
    {"Int64", "ParseOrNull", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): long?"},
    {"Int64", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: long): bool"},
    {"Int64", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: ReadOnlyStringSpan, out result: long): bool"},
    {"Int64", "TryParse", 3, 3, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, kBPTOut, 0, 0, 0, 0, 0}, "TryParse(s: string, radix: int, out result: long): bool"},
    {"Int64", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"Int64", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"Int64", "ToString", 1, 1, 0x01, {"radix", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "ToString(radix: int): string"},
    {"UInt64", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): ulong"},
    {"UInt64", "Parse", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: ReadOnlyStringSpan): ulong"},
    {"UInt64", "Parse", 2, 2, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, 0, 0, 0, 0, 0, 0}, "Parse(s: string, radix: int): ulong"},
    {"UInt64", "ParseOrNull", 1, 1, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): ulong?"},
    {"UInt64", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: ulong): bool"},
    {"UInt64", "TryParse", 2, 2, 0x00, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: ReadOnlyStringSpan, out result: ulong): bool"},
    {"UInt64", "TryParse", 3, 3, 0x00, {"s", "radix", "", "", "", "", "", ""}, {kBPTString, kBPTInt, kBPTOut, 0, 0, 0, 0, 0}, "TryParse(s: string, radix: int, out result: ulong): bool"},
    {"UInt64", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"UInt64", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"UInt64", "ToString", 1, 1, 0x01, {"radix", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "ToString(radix: int): string"},
    {"Double", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): double"},
    {"Double", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: ReadOnlyStringSpan): double"},
    {"Double", "ParseOrNull", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): double?"},
    {"Double", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: double): bool"},
    {"Double", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: ReadOnlyStringSpan, out result: double): bool"},
    {"Double", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"Double", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"Float", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): float"},
    {"Float", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: ReadOnlyStringSpan): float"},
    {"Float", "ParseOrNull", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): float?"},
    {"Float", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: float): bool"},
    {"Float", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: ReadOnlyStringSpan, out result: float): bool"},
    {"Float", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"Float", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    // fix #7546 (stdlib §9.2, spec §5.1, v0.350): the static FP predicates and
    // equality variants were declared by no row, so no §5.1 argument check ran
    // (`Float.IeeeEquals(d, d)` over a `double` was admitted).
    {"Double", "IsNaN", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "IsNaN(value: double): bool"},
    {"Double", "IsInfinity", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "IsInfinity(value: double): bool"},
    {"Double", "IsPositiveInfinity", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "IsPositiveInfinity(value: double): bool"},
    {"Double", "IsNegativeInfinity", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "IsNegativeInfinity(value: double): bool"},
    {"Double", "IsFinite", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "IsFinite(value: double): bool"},
    {"Double", "IeeeEquals", 2, 2, 0x03, {"a", "b", "", "", "", "", "", ""}, {kBPTDouble, kBPTDouble, 0, 0, 0, 0, 0, 0}, "IeeeEquals(a: double, b: double): bool"},
    {"Double", "BitwiseEquals", 2, 2, 0x03, {"a", "b", "", "", "", "", "", ""}, {kBPTDouble, kBPTDouble, 0, 0, 0, 0, 0, 0}, "BitwiseEquals(a: double, b: double): bool"},
    {"Float", "IsNaN", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTFloat, 0, 0, 0, 0, 0, 0, 0}, "IsNaN(value: float): bool"},
    {"Float", "IsInfinity", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTFloat, 0, 0, 0, 0, 0, 0, 0}, "IsInfinity(value: float): bool"},
    {"Float", "IsPositiveInfinity", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTFloat, 0, 0, 0, 0, 0, 0, 0}, "IsPositiveInfinity(value: float): bool"},
    {"Float", "IsNegativeInfinity", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTFloat, 0, 0, 0, 0, 0, 0, 0}, "IsNegativeInfinity(value: float): bool"},
    {"Float", "IsFinite", 1, 1, 0x01, {"value", "", "", "", "", "", "", ""}, {kBPTFloat, 0, 0, 0, 0, 0, 0, 0}, "IsFinite(value: float): bool"},
    {"Float", "IeeeEquals", 2, 2, 0x03, {"a", "b", "", "", "", "", "", ""}, {kBPTFloat, kBPTFloat, 0, 0, 0, 0, 0, 0}, "IeeeEquals(a: float, b: float): bool"},
    {"Float", "BitwiseEquals", 2, 2, 0x03, {"a", "b", "", "", "", "", "", ""}, {kBPTFloat, kBPTFloat, 0, 0, 0, 0, 0, 0}, "BitwiseEquals(a: float, b: float): bool"},
    {"Decimal", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): decimal"},
    {"Decimal", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: ReadOnlyStringSpan): decimal"},
    {"Decimal", "ParseOrNull", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): decimal?"},
    {"Decimal", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: decimal): bool"},
    {"Decimal", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTSpan, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: ReadOnlyStringSpan, out result: decimal): bool"},
    {"Decimal", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"Decimal", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"Decimal", "Round", 1, 1, 0x00, {"digits", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "Round(digits: int): decimal"},
    {"Boolean", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): bool"},
    {"Boolean", "ParseOrNull", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): bool?"},
    {"Boolean", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: bool): bool"},
    {"Boolean", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    // fix #7333 / #7336 (stdlib §2.18, v0.345): the FORM is declared for
    // `bool` and `char`; only the VOCABULARY is empty. §2.18's table lists
    // "`bool`, `char`, `string` | *(none)* | a format spec on these is a
    // compile error" — the format form exists for them (they are
    // "intrinsically formattable"), and what a format spec means there is
    // nothing. So the `ToString(format: string)` ROW belongs here, and the
    // SPEC is what the format-spec gate refuses (a LITERAL spec on a
    // `bool`/`char` receiver), never the form's absence. Without the row
    // the admission gate refused `b.ToString(fmt)` — a legal call that the
    // landed control `a_bool_receiver_with_a_runtime_format_still_compiles_7336`
    // pins — and split the two spellings of one overload.
    {"Boolean", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"Char", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): char"},
    {"Char", "ParseOrNull", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): char?"},
    {"Char", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: char): bool"},
    {"Char", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    // fix #7333 / #7336 (stdlib §2.18, v0.345): the `char` twin of the
    // `Boolean` row above — the form is declared, the vocabulary is empty.
    {"Char", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    // fix #7546 (stdlib §9.5, spec §5.1, v0.350): the `char` static classification
    // and case-mapping members, which no row declared.
    {"Char", "IsDigit", 1, 1, 0x01, {"c", "", "", "", "", "", "", ""}, {kBPTChar, 0, 0, 0, 0, 0, 0, 0}, "IsDigit(c: char): bool"},
    {"Char", "IsLetter", 1, 1, 0x01, {"c", "", "", "", "", "", "", ""}, {kBPTChar, 0, 0, 0, 0, 0, 0, 0}, "IsLetter(c: char): bool"},
    {"Char", "IsLetterOrDigit", 1, 1, 0x01, {"c", "", "", "", "", "", "", ""}, {kBPTChar, 0, 0, 0, 0, 0, 0, 0}, "IsLetterOrDigit(c: char): bool"},
    {"Char", "IsWhiteSpace", 1, 1, 0x01, {"c", "", "", "", "", "", "", ""}, {kBPTChar, 0, 0, 0, 0, 0, 0, 0}, "IsWhiteSpace(c: char): bool"},
    {"Char", "IsUpper", 1, 1, 0x01, {"c", "", "", "", "", "", "", ""}, {kBPTChar, 0, 0, 0, 0, 0, 0, 0}, "IsUpper(c: char): bool"},
    {"Char", "IsLower", 1, 1, 0x01, {"c", "", "", "", "", "", "", ""}, {kBPTChar, 0, 0, 0, 0, 0, 0, 0}, "IsLower(c: char): bool"},
    {"Char", "IsControl", 1, 1, 0x01, {"c", "", "", "", "", "", "", ""}, {kBPTChar, 0, 0, 0, 0, 0, 0, 0}, "IsControl(c: char): bool"},
    {"Char", "IsPunctuation", 1, 1, 0x01, {"c", "", "", "", "", "", "", ""}, {kBPTChar, 0, 0, 0, 0, 0, 0, 0}, "IsPunctuation(c: char): bool"},
    {"Char", "ToUpper", 1, 1, 0x01, {"c", "", "", "", "", "", "", ""}, {kBPTChar, 0, 0, 0, 0, 0, 0, 0}, "ToUpper(c: char): char"},
    {"Char", "ToLower", 1, 1, 0x01, {"c", "", "", "", "", "", "", ""}, {kBPTChar, 0, 0, 0, 0, 0, 0, 0}, "ToLower(c: char): char"},
    {"Guid", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): Guid"},
    {"Guid", "ParseOrNull", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): Guid?"},
    {"Guid", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: Guid): bool"},
    {"Guid", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"Guid", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"DateTime", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): DateTime"},
    {"DateTime", "ParseOrNull", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): DateTime?"},
    {"DateTime", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: DateTime): bool"},
    {"DateTime", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"DateTime", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"DateOnly", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): DateOnly"},
    {"DateOnly", "ParseOrNull", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): DateOnly?"},
    {"DateOnly", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: DateOnly): bool"},
    {"DateOnly", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"DateOnly", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"TimeOnly", "Parse", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "Parse(s: string): TimeOnly"},
    {"TimeOnly", "ParseOrNull", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ParseOrNull(s: string): TimeOnly?"},
    {"TimeOnly", "TryParse", 2, 2, 0x01, {"s", "", "", "", "", "", "", ""}, {kBPTString, kBPTOut, 0, 0, 0, 0, 0, 0}, "TryParse(s: string, out result: TimeOnly): bool"},
    {"TimeOnly", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"TimeOnly", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    {"TimeSpan", "ToString", 0, 0, 0x00, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, "ToString(): string"},
    {"TimeSpan", "ToString", 1, 1, 0x01, {"format", "", "", "", "", "", "", ""}, {kBPTString, 0, 0, 0, 0, 0, 0, 0}, "ToString(format: string): string"},
    // fix #7443 (stdlib §2.5, §5.1, v0.345): the §2.5 CLASS-SPELLING static
    // members' DECLARED PARAMETER TYPES.
    //
    // THE DEFECT. `TimeSpan.FromTicks`, `DateTime.FromTicks`,
    // `DateTime.FromUnixTimeSeconds`, the `TimeSpan.From*` family and the
    // `DateTime.Add*` family are BUILT-IN CLASS calls: they have no
    // MethodSymbol, so §22.5's per-argument check never bound their
    // arguments, and the ONE parameter table carried NO row for them — so the
    // §5.1 admission gate (`ExprTC::checkBuiltinCallAdmission`) had no
    // declared form to admit against and stood down. An argument the
    // standard library does not declare therefore reached the lowering:
    //
    //   TimeSpan.FromTicks(vul)          -> ADMITTED and SILENTLY WRAPPED
    //                                        (-446744073709551616) — a
    //                                        `ulong` at a `long` parameter is
    //                                        never an implicit conversion
    //                                        (§5.1's "between signed and
    //                                        unsigned types of the same or
    //                                        narrower width");
    //   TimeSpan.FromTicks(1.5)          -> the compiler ABORTED
    //                                        ("SExt only operates on
    //                                        integer" — MALFORMED IR);
    //   DateTime.FromTicks(1.5)          -> the same abort;
    //   DateTime.FromUnixTimeSeconds(vul) -> ADMITTED and silently wrapped.
    //
    // With the rows below the SAME admission gate the §9.1-§9.5 primitive
    // heads are admitted by answers the §5.1 question for these members too.
    {"DateTime", "FromTicks", 1, 1, 0x01, {"ticks", "", "", "", "", "", "", ""}, {kBPTLong, 0, 0, 0, 0, 0, 0, 0}, "FromTicks(ticks: long, kind: DateTimeKind := DateTimeKind.Unspecified): DateTime"},
    {"DateTime", "FromTicks", 2, 1, 0x01, {"ticks", "kind", "", "", "", "", "", ""}, {kBPTLong, 0, 0, 0, 0, 0, 0, 0}, "FromTicks(ticks: long, kind: DateTimeKind := DateTimeKind.Unspecified): DateTime"},
    {"DateTime", "FromUnixTimeSeconds", 1, 1, 0x01, {"seconds", "", "", "", "", "", "", ""}, {kBPTLong, 0, 0, 0, 0, 0, 0, 0}, "FromUnixTimeSeconds(seconds: long): DateTime"},
    {"DateTime", "AddYears", 1, 1, 0x01, {"years", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "AddYears(years: int): DateTime"},
    {"DateTime", "AddMonths", 1, 1, 0x01, {"months", "", "", "", "", "", "", ""}, {kBPTInt, 0, 0, 0, 0, 0, 0, 0}, "AddMonths(months: int): DateTime"},
    {"DateTime", "AddDays", 1, 1, 0x01, {"days", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "AddDays(days: double): DateTime"},
    {"DateTime", "AddHours", 1, 1, 0x01, {"hours", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "AddHours(hours: double): DateTime"},
    {"DateTime", "AddMinutes", 1, 1, 0x01, {"minutes", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "AddMinutes(minutes: double): DateTime"},
    {"DateTime", "AddSeconds", 1, 1, 0x01, {"seconds", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "AddSeconds(seconds: double): DateTime"},
    {"DateTime", "AddMilliseconds", 1, 1, 0x01, {"ms", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "AddMilliseconds(ms: double): DateTime"},
    {"TimeSpan", "FromTicks", 1, 1, 0x01, {"ticks", "", "", "", "", "", "", ""}, {kBPTLong, 0, 0, 0, 0, 0, 0, 0}, "FromTicks(ticks: long): TimeSpan"},
    {"TimeSpan", "FromDays", 1, 1, 0x01, {"days", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "FromDays(days: double): TimeSpan"},
    {"TimeSpan", "FromHours", 1, 1, 0x01, {"hours", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "FromHours(hours: double): TimeSpan"},
    {"TimeSpan", "FromMinutes", 1, 1, 0x01, {"minutes", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "FromMinutes(minutes: double): TimeSpan"},
    {"TimeSpan", "FromSeconds", 1, 1, 0x01, {"seconds", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "FromSeconds(seconds: double): TimeSpan"},
    {"TimeSpan", "FromMilliseconds", 1, 1, 0x01, {"ms", "", "", "", "", "", "", ""}, {kBPTDouble, 0, 0, 0, 0, 0, 0, 0}, "FromMilliseconds(ms: double): TimeSpan"},
    // fix #6812 (stdlib §7.3, v0.344): the ASYNC text members. Their
    // `token` parameter is declared `token: CancellationToken :=
    // CancellationToken.None` — OPTIONAL — so the two-argument form requires
    // ONE argument and the three-argument form requires TWO. The `required`
    // column is the declared MINIMUM, not the arity.
    {"File", "ReadAllTextAsync", 1, 1, 0x01, {"path", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"File", "ReadAllTextAsync", 2, 1, 0x01, {"path", "token", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"File", "WriteAllTextAsync", 2, 2, 0x03, {"path", "contents", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"File", "WriteAllTextAsync", 3, 2, 0x03, {"path", "contents", "token", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Environment", "GetInt", 1, 1, 0x01, {"name", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Environment", "GetBool", 1, 1, 0x01, {"name", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    // fix #6812 (§7.3, v0.340; stdlib §2.4 v0.344): the REST of stdlib §2.4's
    // `Environment` surface. `Get(name: string): string?` and
    // `Set(name: string, value: string?)` declare their FIRST parameter
    // non-nullable, exactly as `GetInt` / `GetBool` do, so the `null` literal
    // reaches no candidate there either (§7.3). `Set`'s SECOND parameter is
    // declared `string?` — nullable — so only position 0 is non-nullable
    // (`Environment.Set("A", null)` stays legal).
    {"Environment", "Get", 1, 1, 0x01, {"name", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Environment", "Set", 2, 2, 0x01, {"name", "value", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    // fix #6812 (§7.3, v0.340; stdlib §10.1 / §10.2 v0.344): the FFI-boundary
    // string factories. `CString.From(s: string): owned CString` and
    // `WString.From(s: string): owned WString` declare their parameter
    // non-nullable, so `CString.From(null)` reaches no candidate (§7.3).
    {"CString", "From", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"WString", "From", 1, 1, 0x01, {"s", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"File", "ReadAllText", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"File", "WriteAllText", 2, 0, 0x03, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"File", "ReadAllBytes", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"File", "WriteAllBytes", 2, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"File", "ReadAllLines", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"File", "WriteAllLines", 2, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"File", "Exists", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"File", "Delete", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"File", "Copy", 3, 0, 0x03, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"File", "Move", 2, 0, 0x03, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Directory", "Exists", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Directory", "CreateDirectory", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Directory", "Delete", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Directory", "Delete", 2, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Directory", "GetFiles", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Directory", "GetDirectories", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Path", "Combine", 2, 0, 0x03, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Path", "GetFileName", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Path", "GetExtension", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Path", "GetFileNameWithoutExtension", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Path", "GetDirectoryName", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"Path", "GetFullPath", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"TextWriter", "Write", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
    {"TextWriter", "WriteLine", 1, 0, 0x01, {"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}, ""},
}};



// fix #3425: the two record-shaped builtins whose `lowerNew` arm resolves each
// argument to its FIELD SLOT BY NAME before lowering (#3098,
// LlvmBackend_Exprs2.cpp). See the header.
constexpr std::array<std::string_view, 2> kBuiltinCtorLoweredByNameHeads = {
    "ExceptionInfo", "ReadOnlyStringSpan",
};

// fix #7502: the heads whose constructor rows are the complete §2.5
// constructor surface (DateTime, DateOnly, TimeOnly). See the header.
constexpr std::array<std::string_view, 3> kBuiltinCtorSurfaceClosedByRowsHeads = {
    "DateTime", "DateOnly", "TimeOnly",
};

// The heads `kStringParamNames` / `kEncodingParamNames` /
// `kStringBuilderParamNames` answer for. See the header for the two roles this
// predicate plays at the call site.
constexpr std::array<std::string_view, 6> kBuiltinNamedArgTableHeads = {
    "string", "String", "Encoding", "StringBuilder", "Math", "Convert",
};

// fix #3589 (stdlib §5 / §9.6): see the header — the string-receiver LINQ
// exemption gate. 39 names; two independent hand-spellings (the `.` and `?.`
// member-access gates) were found byte-identical, confirming they answer the
// one question. fix #3636 removed `ForEach`: instance `.ForEach(...)` is NOT
// in stdlib §5 (the only documented `ForEach` is the unrelated static
// `Parallel.ForEach`), the broken-by-IR-verifier codegen arm at
// `LlvmBackend_Calls.cpp` was retired in the same fix, and the builtin-
// collection twin (`kBuiltinCollectionLinqOperatorNames` below) already
// excluded `ForEach` — so the two tables are now consistent on this name.
// fix #3786 (stdlib §5.1, v0.286) added `MinOrNull`/`MaxOrNull`: the
// collection twin already admitted them (fix #2783's array arm), and this
// table was the one remaining gap #3444 flagged and explicitly declined to
// close without first proving the string-receiver materialize path lowers
// them correctly (issue 3444's warning) — fix #3764 completed that proof by
// adding both names to `LlvmBackend_Calls.cpp`'s `isArrTerminal`, the SAME
// disjunction `isStringRecvTerm` already sits in, so a string receiver now
// materializes via `bz_lazy_to_array(lazy, esz=4)` and runs the existing
// #2783 array arm exactly as First/Sum/Min/Max/... already do (verified
// end-to-end below before this table changed, not assumed).
constexpr std::array<std::string_view, 39> kStringLinqOperatorMemberNames = {
    "Where",     "Select",     "SelectMany", "Skip",
    "Take",      "TakeWhile",  "SkipWhile",  "Distinct",
    "OrderBy",   "OrderByDescending",        "ThenBy",
    "ThenByDescending",         "Concat",     "Zip",
    "Union",     "Intersect",  "Except",     "Reverse",
    "Any",       "All",        "Count",      "Sum",
    "Min",       "Max",        "Average",    "First",
    "Last",      "Single",     "ElementAt",  "FirstOrNull",
    "LastOrNull", "SingleOrNull",            "ToArray",
    "ToSet",     "ToDictionary",             "GroupBy",
    "Aggregate", "MinOrNull",  "MaxOrNull",
};

// fix #3589 (stdlib §5): see the header — the builtin-collection-family
// bare-reference / member-access LINQ exemption gate. 38 names — the union of
// four hand-spellings that a #3589 measurement found were meant to be
// identical (three were already byte-identical; the fourth, the
// `Array`/`OwningArray` copy, was missing `ToArray` only because that
// family's `isDeclaredMethod` set admits it separately — see the header).
constexpr std::array<std::string_view, 38> kBuiltinCollectionLinqOperatorNames = {
    "Where",     "OfType",     "Select",     "SelectMany",
    "Any",       "All",        "First",      "FirstOrNull",
    "Last",      "LastOrNull", "Single",     "SingleOrNull",
    "ElementAt", "Sum",        "Min",        "Max",
    "MinOrNull", "MaxOrNull",  "Average",    "Aggregate",
    "OrderBy",   "OrderByDescending",        "Distinct",
    "Union",     "Intersect",  "Except",     "Take",
    "TakeWhile", "Skip",       "SkipWhile",  "Concat",
    "Zip",       "GroupBy",    "ToArray",    "ToDictionary",
    "ToSet",     "ThenBy",     "ThenByDescending",
};

// fix #3670 (stdlib §5): see the header — the lambda-parameter-seeding gate.
// 20 names; a DIFFERENT question from the two tables above (shape, not name
// existence) — deliberately not merged with either. Extracted unchanged from
// `linqLambdaElementType`'s `elemLambda` (no name added or removed).
constexpr std::array<std::string_view, 20> kLinqElementLambdaOperatorNames = {
    "Where",     "Select",     "Count",      "Any",
    "All",       "First",      "Last",       "Single",
    "OrderBy",   "OrderByDescending",        "Min",
    "Max",       "Sum",        "TakeWhile",  "SkipWhile",
    "SelectMany", "Average",   "ToDictionary",
    "ThenBy",    "ThenByDescending",
};

// fix #3670 (stdlib §5): see the header — the lazy-passthrough-typing gate.
// 11 names (`Take` excluded on purpose — see the header). Collapses four
// hand-spelled, byte-identical-bodied `if` arms in `typeLinqOperator` that
// answered this one question in four pieces.
constexpr std::array<std::string_view, 11> kLinqLazyPassthroughOperatorNames = {
    "Where",     "Skip",       "Distinct",   "OrderBy",
    "OrderByDescending",       "Concat",     "TakeWhile",
    "SkipWhile", "Union",      "Intersect",  "Except",
};

// fix #3763 (stdlib §5 / §4.6 / §4.7 / §4.10 / §4.18, v0.287): see the header
// for the membership rule (stdlib §5's `Reverse` naming note read with spec
// §26.5 rule 1) and for why the `OwningBuffer`, `FrozenArray` and dictionary
// families are deliberately NOT here (see the header for the full note).
//
// fix #3825 (stdlib §5 / §4.11, v0.307): Stack/OwningStack/Queue/OwningQueue
// declare no instance `Reverse()` per stdlib §4.11, so the §5 naming note says
// the lazy operator applies — and does. Previously excluded here and dispatched
// to the in-place `bz_array_*` mutator via the `isCollectionTypeName` structural
// path; #3825 admitted them and updated the two pins that accepted the in-place
// behaviour (`stack_reverse_is_lazy_not_in_place`, `queue_reverse_is_lazy_not_in_place`).
constexpr std::array<std::string_view, 9> kLazyReverseDirectReceiverHeads = {
    "Set", "OwningSet", "SortedSet", "LinkedList", "OwningLinkedList",
    "Stack", "OwningStack", "Queue", "OwningQueue",
};

// fix #4766 / #4833 (§5.1, v0.307): the builtin heap-type heads that declare
// a `readonly property Length : int` per the stdlib. Transcribed verbatim from
// the narrow #4766 typing arm that lived inline at
// ExprTypecheck_Calls.cpp's `resolveMemberAccess`; the consolidation
// (BuiltinRegistry hand-list removal) is #4833.
//
// Heads:
//   - §4.4 Array, §4.5 OwningArray, §4.11 Stack/OwningStack/Queue/OwningQueue
//     (the structural Count alias; #4811 tracks whether it should exist);
//   - §3.1 StringBuilder;
//   - §4.14 OwningBuffer<T>;
//   - §4.19 FrozenArray<T> (annotation shape only).
// ⚠ `Set`/`OwningSet`/`LinkedList`/`OwningLinkedList` declare no `Length`
// property — they declare `Count`, handled by the upper arm at
// ExprTypecheck_Calls.cpp:~20867. See `isBuiltinIntCountPropertyHead` for the
// `.Count` analog. Two different questions over overlapping name sets is
// exactly why both predicates exist and are each named for their own question.
// fix #4811 (§4.4, v0.315): remove Array/OwningArray from this set. The
// Count-vs-Length convention (stdlib §4.4 p.74) gives resizable Array<T>
// `Count` ONLY — "which is why the resizable Array<T> says Count where .NET's
// fixed array says Length". fix #5416 (§4.11, v0.315): also removes
// Stack/Queue/OwningStack/OwningQueue — these four declare Count (via
// IReadOnlyCollection<T>), not Length, per stdlib §4.11.
constexpr std::array<std::string_view, 3> kBuiltinIntLengthPropertyHeads = {
    "StringBuilder",
    "OwningBuffer",
    "FrozenArray",
};

// fix #4766 / #4833 (§5.1, v0.307): the builtin heap-type heads that declare
// a `readonly property Count : int` per the stdlib but that the upper
// ExprTypecheck_Calls.cpp `Count` arm (~line 20867) does NOT type. Transcribed
// verbatim from the narrow #4766 arm; the consolidation is #4833.
//
// Heads:
//   - §4.17 SortedDictionary<K, V>;
//   - §4.18 SortedSet<T>;
//   - §6.9 Channel<T>.
// ⚠ `Dictionary`/`OwningDictionary`/`Set`/`OwningSet`/`LinkedList`/
// `OwningLinkedList` are typed by the upper arm (Array/Dictionary set
// expansion). `Array`/`OwningArray`/`Stack`/`OwningStack`/`Queue`/
// `OwningQueue` are also typed by the upper arm (those names happen to be in
// BOTH the upper arm's collection set AND this narrow Count set is empty for
// them — they declare `Count` and the upper arm handles it). See header for
// the full split.
constexpr std::array<std::string_view, 3> kBuiltinIntCountPropertyHeads = {
    "SortedDictionary", "SortedSet", "Channel",
};

}  // namespace

bool isOwningPrefixedName(std::string_view head) {
    return head.rfind("Owning", 0) == 0;
}  // per §5.11 / #1372 — Owning* prefix test (consolidates the 16 codegen rfind sites)

bool isOwningCollectionHead(std::string_view head) {
    return contains(kOwningCollectionHeads, head);
}  // per §28.6 / §5.11 (#1008 Part C) — element-owning collection heads

bool isPlainBuiltinCollectionHead(std::string_view head) {
    return contains(kPlainBuiltinCollectionHeads, head);
}  // per §28.3 row 4 (v0.313) — plain builtin collection class heads

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

// #3419 / §29.2 / stdlib §4.12–§4.13: the four iterator-format ENUMERABLE heads
// — the enumerable-wrap grid's declared-head axis. // per §29.2, #3419
bool isIterFormatEnumerableHead(std::string_view head) {
    return contains(kIterFormatEnumerableHeads, head);
}

// #3993 / #4004 / #4010: the NON-OWNING half of the above — the subtraction
// against `isOwningIterableInterfaceHead`. See the header for why this
// specific two-name set (not the broader four-name one above) is the sound
// exemption for a borrow-through-owned-return double-dispose check.
// // per §28.3, §28.6, #3419, #3993, #4004, #4005, #4007, #4010
bool isNonOwningIterFormatEnumerableHead(std::string_view head) {
    return isIterFormatEnumerableHead(head) &&
           !isOwningIterableInterfaceHead(head);
}

// The synchronous half of the above. // per §29.2, #3419
bool isSyncIterFormatEnumerableHead(std::string_view head) {
    return contains(kSyncIterFormatEnumerableHeads, head);
}

// #3419 / §29.2: the four iterator-format ENUMERATOR heads — the complement
// of isIterFormatEnumerableHead. // per §29.2, #3419
bool isIterFormatEnumeratorHead(std::string_view head) {
    return contains(kIterFormatEnumeratorHeads, head);
}

// The synchronous half of the above. // per §29.2, #3419
bool isSyncIterFormatEnumeratorHead(std::string_view head) {
    return contains(kSyncIterFormatEnumeratorHeads, head);
}

// fix #3606 / §29.1 / §29.3: the synchronous half of
// `isOwningIterableInterfaceHead` above.
bool isSyncOwningIterableInterfaceHead(std::string_view head) {
    return contains(kSyncOwningIterableInterfaceHeads, head);
}

// fix #3468 / §29.2: the iteration-interface heads the builtin for-in
// desugaring handles. // per §29.2, #3468
bool isBuiltinForInEnumerableHead(std::string_view head) {
    return contains(kBuiltinForInEnumerableHeads, head);
}

// #3419 / §29.2 / stdlib §4.4–§4.5 / §4.11 / §4.14: the array-enumerator wrap
// SOURCE families. Not isArrayBackedCollectionName — see the header.
// per §29.2, #3419
bool isArrayEnumeratorWrapSourceName(std::string_view n) {
    return contains(kArrayEnumeratorWrapSources, n);
}

// fix #3419 / §29.2 / §29.11 / §28.6: the six iterator-format HEADS the
// SYNC `using var` admission recognises. See kIteratorFormatDispatchHeads
// for the set membership; this is the predicate form (every consumer of the
// set should call this rather than spell the names). // per §29.2, #3419
bool isIteratorFormatDispatchHead(std::string_view head) {
    return contains(kIteratorFormatDispatchHeads, head);
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

bool isArrLikeCollectionName(std::string_view n) {
    return contains(kArrLikeCollectionNames, n);
}  // per §3.4 / §4.4–§4.11, §4.18 — #6265 bare-name static check family

bool isBareCollectionTypeName(std::string_view n) {
    return isArrLikeCollectionName(n) || isDictionaryName(n) || n == "FrozenArray";
}  // per §3.4 / §4.5–§4.8, §4.19 — #6267 member-access check family (Dictionary + FrozenArray added)

bool isFfiBufferBuiltinName(std::string_view n) {
    return contains(kFfiBufferBuiltins, n);
}  // per §32.5 / stdlib §10.1–§10.3 (#2809) — the raw-buffer FFI classes

bool isNoOverrideIdentityBuiltinName(std::string_view n) {
    return contains(kNoOverrideIdentityBuiltins, n);
}  // per §6.1 / §8.1 (#4126) — Stopwatch / CancellationToken, no-vtable heap ptrs

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
// INTERFACE heads — exposed as its own predicate so the #2969 codegen routing
// does not have to re-list the `kBuiltinInterfaceHeads` names. See the header
// for the contract — split off from `isVtableLessBuiltinClassName` because a
// user class may legitimately declare `class MyE: IEnumerable<int>`, so the
// dynamic dispatch on these names must remain reachable even though they are
// the one reserved-name population that #2923's fix declined to route away
// from it.
bool isBuiltinInterfaceHead(std::string_view n) {
    return contains(kBuiltinInterfaceHeads, n);
}

// fix #7456 / #7457 (spec §9.7 / §14.2 / §28.12, stdlib §2.10 / §4.2 /
// §4.12 / §4.13, v0.345): THE BUILT-IN DISPOSAL FAMILY — the interfaces that
// extend `IDisposable` directly or transitively.
//
// This is the ONE table the compiler now derives every "is this head a
// disposal interface?" answer from. THREE questions were answered by THREE
// DISAGREEING hand-lists, and the disagreement produced two shipped defects:
//
//   * the §28.10 vacuity lint's table (`kDisposableBuiltinHeadNames`, below)
//     knew SIX of the family's ten heads, so a binding declared at
//     `IDisposable` / `IAsyncDisposable` / `IOwningCollection<T>` /
//     `IOwningList<T>` was told "the RHS is a value-typed/runtime-managed
//     value with no disposal obligation" while the obligation was real and
//     discharged (#7456);
//   * the `where`-bound check (`primitiveNeverConformsToBuiltinIface`,
//     src/sema/TypeRef.hpp) knew TWO, so a RECORD type argument against
//     `where T: IEnumerator<R>` was ADMITTED although §9.7 forbids a record
//     from implementing `IDisposable` "directly or through an interface
//     extending it" (#7457);
//   * the declaration-site chain table (`kDisposalHeadParents`,
//     src/sema/TypeResolution_Checks.cpp) knew all TEN — and its own header
//     recorded the RESIDUAL: "THE TABLE MUST EVENTUALLY BE DERIVED
//     MECHANICALLY … so that a FUTURE standard-library interface extending
//     `IDisposable` cannot miss again" (#7424).
//
// The table below IS that derivation: one hand-written table, in the
// data-only registry (beside `kBuiltinExceptionHierarchy`, the exactly-
// analogous built-in hierarchy table), read by all three sites.
//
// THE CORRECTNESS OF THIS TABLE IS MECHANICALLY GUARANTEED (#7424's
// RESIDUAL). It is still hand-written, but a ratchet derives the family from
// `docs/Bezel_Standard_Library.md` — every `public interface` that extends
// `IDisposable` directly or transitively — and compares the derivation
// against THIS table, against the `std::array<..., N>` size below, and
// against the comment block below. It fails CLOSED: a stdlib interface
// extending `IDisposable` that is missing here, a chain parent that
// disagrees, or a wrong stdlib section attribution all turn the gate RED
// before a program can reach the compiler. See
// `scripts/check_disposal_family_chain.py` (wired into
// `scripts/gate_full_cycle.sh`).
//
// THE MEMBERS, AND WHERE EACH IS DECLARED (stdlib §2.10 / §4.2 / §4.12 /
// §4.13, v0.345):
//
//   interface              extends              stdlib
//   --------------------- -------------------- --------
//   IDisposable           — (root)             §2.10
//   IAsyncDisposable       IDisposable          §2.10
//   IOwningCollection<T>   IDisposable          §4.2
//   IOwningList<T>         IOwningCollection<T> §4.2
//   IEnumerator<out T>     IDisposable          §4.12
//   IAsyncEnumerator<out T> IAsyncDisposable    §4.12
//   IOwningEnumerable<T>   IDisposable          §4.13
//   IOwningEnumerator<T>   IDisposable          §4.13
//   IOwningAsyncEnumerable<T> IAsyncDisposable  §4.13
//   IOwningAsyncEnumerator<T> IAsyncDisposable  §4.13
//
// Spec §14.2's worked example (`where T: IDisposable`) names the same
// closure: "an interface conforms exactly when it extends `IDisposable`
// (rule 3): `IAsyncDisposable`, `IEnumerator<T>`, `IAsyncEnumerator<T>`, and
// any user-defined interface that lists `IDisposable` in its extension
// clause"; stdlib §2.10 adds "the interfaces that extend `IDisposable`
// directly or transitively".
//
// The USER side of the family is not a table: a user interface's own
// extension clause is carried on its symbol (`TypeSymbol::baseTypes` for a
// user base, `TypeSymbol::nonSymbolBaseRefs` for a built-in base), and
// `declaresBuiltinInterface` is the ONE place the question "does this type
// declare built-in interface `name`?" is answered (Symbol.hpp). Only the
// BUILT-IN heads are tabulated here.
// per §9.7 / §14.2 / §28.12, stdlib §2.10 / §4.2 / §4.12 / §4.13, fix #7456,
// fix #7457
constexpr std::array<BuiltinDisposalFamilyHead, 10> kBuiltinDisposalFamilyHeads = {{
    {"IDisposable", ""},
    {"IAsyncDisposable", "IDisposable"},
    // §4.12 — the two borrowing enumeration interfaces.
    {"IEnumerator", "IDisposable"},
    {"IAsyncEnumerator", "IAsyncDisposable"},
    // §4.13 — the four owning iteration interfaces.
    {"IOwningEnumerable", "IDisposable"},
    {"IOwningEnumerator", "IDisposable"},
    {"IOwningAsyncEnumerable", "IAsyncDisposable"},
    {"IOwningAsyncEnumerator", "IAsyncDisposable"},
    // §4.2 — the two owning-collection interfaces. `IOwningList` reaches
    // `IDisposable` THROUGH `IOwningCollection`.
    {"IOwningCollection", "IDisposable"},
    {"IOwningList", "IOwningCollection"},
}};

// Is `head` one of the built-in disposal interfaces? (the DIRECT case — the
// transitive closure is walked by the callers that have a chain to walk)
// per §9.7 / §14.2 / §28.12, fix #7456, fix #7457
bool isBuiltinDisposalFamilyHead(std::string_view head) {
    for (const auto& d : kBuiltinDisposalFamilyHeads)
        if (d.head == head) return true;
    return false;
}

// The interface the built-in disposal head `head` extends (empty for
// `IDisposable` itself).
// per §9.7 / §14.2 / §28.12, fix #7456, fix #7457
std::string_view builtinDisposalFamilyParent(std::string_view head) {
    for (const auto& d : kBuiltinDisposalFamilyHeads)
        if (d.head == head) return d.parent;
    return {};
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
    // §30.15 channels; §24 events; §2.11 subscription.
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

// fix #7456 (§9.7 / §14.2 / §28.12, stdlib §2.10 / §4.2 / §4.12 / §4.13,
// v0.345): the table now carries the WHOLE built-in DISPOSAL FAMILY, derived
// from `kBuiltinDisposalFamilyHeads` (the one hand-written chain table, above)
// rather than spelled out a second time. The lint's table knew only SIX of the
// family's TEN heads, so a binding declared at `IDisposable` /
// `IAsyncDisposable` / `IOwningCollection<T>` / `IOwningList<T>` was told
// "the RHS is a value-typed/runtime-managed value with no disposal obligation"
// while the obligation was REAL and discharged (§8.7: "The compiler generates
// `IDisposable` on every non-static class"; §28.12: the disposal dispatches
// through any interface to the runtime class).
//
// The four heads are NOT added as literal rows above: they are DERIVED, so a
// future standard-library interface extending `IDisposable` cannot miss again
// — the residual #7424's own header recorded ("THE TABLE MUST EVENTUALLY BE
// DERIVED MECHANICALLY … so that a FUTURE standard-library interface
// extending `IDisposable` cannot miss again"). The literal rows the six
// already carried are KEPT (they are the six the table's history names), and
// the derived term is a strict superset of them.
// per §28.3 / §28.9 / §28.10, fix #2992, fix #7456
bool isDisposableBuiltinHeadName(std::string_view head) {
    return contains(kDisposableBuiltinHeadNames, head) ||
           isBuiltinDisposalFamilyHead(head);
}

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
    "Random", "Atomic", "AtomicInteger", "Stopwatch", "__Buffer",
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
constexpr std::array<BuiltinClassModulePath, 88> kBuiltinClassModulePaths = {{
    // fix #5186 (spec §8.1, v0.311; stdlib §2.13): `Object` and `Type` are the
    // §2.3 Row 1 core types whose module is `System`. The `Object` row is the
    // type-argument rendering (§2.13 rule 3 / `spec85TypeRefQualifiedName`'s
    // `K::Primitive` arm) — the bare `Object` head would otherwise render as
    // just `Object`, not `System.Object`; the `Type` row is the
    // `typeof(...).FullName` rendering under stdlib §2.13 rule 1.
    {"Object", "System"},
    {"Type", "System"},
    // stdlib §2 (System) — the §2.5/§2.6 temporal/identity value types are
    // value-shaped (§3.1) but their BOX thunk (`valueBuiltinBoxToStringThunk`)
    // is in the §8.5 default path, so they belong here too.
    {"DateTime", "System"},
    {"TimeSpan", "System"},
    {"DateOnly", "System"},
    {"TimeOnly", "System"},
    {"Guid", "System"},
    // fix #4120 (stdlib §10.1/§10.2/§10.3, v0.295): the FFI-boundary BUFFER
    // classes (`kFfiBufferBuiltins` above) — "types of §10.0-§10.4 live in
    // the System module and are auto-imported" (stdlib §10 preamble). Class-
    // shaped (heap-allocated, `decl == nullptr`), no declared ToString
    // override, so the §8.5 no-override default (qualified type name) is
    // the correct rendering, read through this table by the same
    // `LlvmBackend_Calls2.cpp` literal chain FrozenArray's row above serves.
    {"CString", "System"},
    {"WString", "System"},
    {"RawBytes", "System"},
    // fix #5177 (stdlib §2.13/§2.17, v0.310): built-in EXCEPTIONS' module
    // path is the declaring module, NOT uniformly `System` — stdlib §2.17
    // covers only the §31.4 built-in hierarchy actually declared in
    // `System`. Ten rows are declared elsewhere and were wrongly rendering
    // "System.<Name>": `KeyNotFoundException` (stdlib §4.8,
    // System.Collections.Generic), the five §7.5 IO exceptions
    // (System.IO), and `ChannelClosedException`/`ChannelFullException`/
    // `DomainFaultedException`/`TimeoutException` (stdlib §6,
    // System.Threading).
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
    // fix #5177 (stdlib §4.8, v0.310): declared in System.Collections.Generic.
    {"KeyNotFoundException", "System.Collections.Generic"},
    {"MatchFailureException", "System"},
    {"OperationCanceledException", "System"},
    {"TypeInitializationException", "System"},
    {"FormatException", "System"},
    {"StackOverflowException", "System"},
    // fix #5177 (stdlib §6, v0.310): declared in System.Threading.
    {"ChannelClosedException", "System.Threading"},
    {"ChannelFullException", "System.Threading"},
    {"DomainFaultedException", "System.Threading"},
    {"SerializationException", "System"},
    // fix #5177 (stdlib §6, v0.310): declared in System.Threading.
    {"TimeoutException", "System.Threading"},
    // fix #5177 (stdlib §7.5, v0.310): declared in System.IO.
    {"IOException", "System.IO"},
    {"FileNotFoundException", "System.IO"},
    {"DirectoryNotFoundException", "System.IO"},
    {"EndOfStreamException", "System.IO"},
    {"PathTooLongException", "System.IO"},
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
    // fix #6079 (spec §2.3 L161, v0.338; §2.7 rule 2): the Task family's
    // qualified spelling `System.Threading.Task` under `import System.Threading;`.
    // `Task` sits in sema's kClassesOutsideTable (closed-class set), not here,
    // so without this row `builtinTypeHomeModuleOf("Task")` is empty and the
    // #6040 qualified-builtin arm in TypeResolution_Names.cpp never matches.
    {"Task", "System.Threading"},
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
    // stdlib §7.1 (System.IO): Stream — abstract base class for byte I/O.
    // fix #4867.
    {"Stream", "System.IO"},
    // stdlib §7.4 (System.IO): TextReader / TextWriter — abstract base classes
    // for character input / output. fix #4867.
    {"TextReader", "System.IO"},
    {"TextWriter", "System.IO"},
    // stdlib §8 (System.Diagnostics): Stopwatch.
    {"Stopwatch", "System.Diagnostics"},
    // stdlib §2.15 (#687): concrete serializers/deserializers; class heads
    // under `System` (the serialization vocab in §2.14 is interfaces).
    {"BinarySerializer", "System"},
    {"BinaryDeserializer", "System"},
    {"JsonSerializer", "System"},
    {"JsonDeserializer", "System"},
    // §24 (#1536): EventSource / AsyncEventSource / IEvent / IAsyncEvent —
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

// fix #4698 (§2.2, v0.313): see the header. `Debug` (stdlib §8.2) is a static
// class with no §8.5 row, so it is added here rather than to the table.
//
// fix #6080 (spec §2.3, v0.338; §2.7 rule 2): `IGrouping<TKey, T>` is the one
// named declaration of the auto-imported `System.Collections.Linq` module
// (§2.3's "Auto-imported module" row). It is an INTERFACE head, so it is
// deliberately absent from `kBuiltinClassModulePaths` (the §8.5 rendering
// table: an interface never names itself there). Its home is read only by
// the alias/qualifier resolution, so the row lives in this home-only table
// and cannot change any §8.5 rendering. Without it, `System.Collections.Linq.
// IGrouping<K, V>` fell through to "unresolved type name `System`".
//
// fix #6154 (spec §2.3, v0.338; §2.7 rule 2): the same gap for every OTHER
// interface head and the few non-interface heads the §2.3 always-available
// table names whose home has no §8.5 row (`IEnumerable<T>` under
// `System.Collections.Generic`, `IOrderedEnumerable<T>` under
// `System.Collections.Linq`, `IComparable<T>` under `System`, ...). Each row
// is the module the §2.3 table lists the name under; a qualifier naming any
// other module still falls through (`System.Text.IEnumerable<int>` stays
// rejected). The §8.5 table is untouched: these names have no
// class-rendering row to change.
struct BuiltinHomeOnlyModulePath {
    std::string_view name;
    std::string_view modulePath;
};
constexpr std::array<BuiltinHomeOnlyModulePath, 36> kBuiltinHomeOnlyModulePaths = {{
    {"IGrouping", "System.Collections.Linq"},
    {"IOrderedEnumerable", "System.Collections.Linq"},
    // §2.3 `System.Collections.Generic` row: the iteration and collection
    // interfaces, then the owning-collection interfaces.
    {"IEnumerable", "System.Collections.Generic"},
    {"IEnumerator", "System.Collections.Generic"},
    {"IAsyncEnumerable", "System.Collections.Generic"},
    {"IAsyncEnumerator", "System.Collections.Generic"},
    {"IReadOnlyCollection", "System.Collections.Generic"},
    {"IReadOnlyList", "System.Collections.Generic"},
    {"ICollection", "System.Collections.Generic"},
    {"IList", "System.Collections.Generic"},
    {"IOwningEnumerable", "System.Collections.Generic"},
    {"IOwningEnumerator", "System.Collections.Generic"},
    {"IOwningAsyncEnumerable", "System.Collections.Generic"},
    {"IOwningAsyncEnumerator", "System.Collections.Generic"},
    {"IOwningCollection", "System.Collections.Generic"},
    {"IOwningList", "System.Collections.Generic"},
    {"IEqualityComparer", "System.Collections.Generic"},
    {"DictionaryEntry", "System.Collections.Generic"},
    {"OwningSlotMap", "System.Collections.Generic"},
    {"Span", "System.Collections.Generic"},
    {"ReadOnlySpan", "System.Collections.Generic"},
    {"ArraySpan", "System.Collections.Generic"},
    {"ReadOnlyArraySpan", "System.Collections.Generic"},
    // §2.3 `System.Text` row.
    {"Encoding", "System.Text"},
    {"ReadOnlyStringSpan", "System.Text"},
    // §2.3 `System` row: the interface heads, and the FFI-boundary types.
    {"IComparable", "System"},
    {"IEquatable", "System"},
    {"ISerializer", "System"},
    {"IDeserializer", "System"},
    {"ISerializable", "System"},
    {"IDeserializable", "System"},
    {"IntPtr", "System"},
    {"UIntPtr", "System"},
    {"IDisposable", "System"},
    {"IAsyncDisposable", "System"},
    // IEvent / IAsyncEvent / ISharedEvent are held out pending #6241: §6.5
    // places them under System.Threading, so a `System.` row would newly
    // accept a spelling the stdlib text does not give them.
    // §2.3 Task family: the owning task is declared with `Task` under
    // System.Threading (qualified spelling needs `import System.Threading;`).
    {"OwningTask", "System.Threading"},
}};

std::string_view builtinTypeHomeModuleOf(std::string_view name) {
    if (name == "Debug") return "System.Diagnostics";
    for (const auto& e : kBuiltinHomeOnlyModulePaths)
        if (e.name == name) return e.modulePath;
    return builtinClassModulePathOf(name);
}

// §2.3 (v0.338) auto-imported module rows; see the header for the list's role.
constexpr std::array<std::string_view, 4> kAutoImportedModulePaths = {
    "System", "System.Text", "System.Collections.Generic",
    "System.Collections.Linq",
};

bool isAutoImportedModulePath(std::string_view modulePath) {
    return contains(kAutoImportedModulePaths, modulePath);
}

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

namespace {

// The head the ONE parameter table is keyed by. The primitive keyword
// spellings name the same standard-library types §9.1-§9.5 declares ("SByte,
// Byte, Int16, UInt16, Int32, UInt32, Int64, UInt64"), and the static call
// spelling of the string class is `String` where the table says `string`.
// per §9.1 / stdlib §9.1-§9.6, fix #7296
std::string_view normaliseBuiltinParamHead(std::string_view head) {
    if (head == "String") return "string";
    if (head == "int") return "Int32";
    if (head == "bool") return "Boolean";
    if (head == "char") return "Char";
    if (head == "double") return "Double";
    if (head == "float") return "Float";
    if (head == "decimal") return "Decimal";
    if (head == "byte") return "Byte";
    if (head == "sbyte") return "SByte";
    if (head == "short") return "Int16";
    if (head == "ushort") return "UInt16";
    if (head == "uint") return "UInt32";
    if (head == "long") return "Int64";
    if (head == "ulong") return "UInt64";
    return head;
}

// Does this row carry a §22.5 NAME surface? A row that carries only the type
// and the nullability is not a named-argument candidate.
bool rowCarriesNames(const BuiltinParamRow& r) {
    for (std::size_t i = 0; i < r.total; ++i)
        if (!r.names[i].empty()) return true;
    return false;
}

// Collect every row of the ONE parameter table whose declared shape admits
// `argCount` arguments, in the table's declaration order. Shared by the
// member and constructor lookups so the "required <= argCount <= total"
// admission rule has one spelling. // per §22.5, fix #3424 / #3425
void collectRows(std::string_view head, std::string_view member,
                 std::size_t argCount, std::vector<BuiltinFormals>& out) {
    head = normaliseBuiltinParamHead(head);
    for (const auto& r : kBuiltinParams) {
        if (r.head != head || r.member != member) continue;
        if (argCount < r.required || argCount > r.total) continue;
        if (!rowCarriesNames(r)) continue;
        BuiltinFormals f;
        f.names.assign(r.names.begin(),
                       r.names.begin() + static_cast<std::ptrdiff_t>(r.total));
        f.required = r.required;
        out.push_back(std::move(f));
    }
}

}  // namespace

// The head the built-in parameter table is keyed by — the public spelling of
// `normaliseBuiltinParamHead`, so a caller that reads the table itself asks
// the table the question.
// per §9.1 / stdlib §9.1-§9.6, fix #7296
std::string_view builtinParamHeadSpelling(std::string_view typeHead) {
    return normaliseBuiltinParamHead(typeHead);
}

// fix #3398 (§22.5, v0.283; stdlib §1 / §9.6 / §3.2), widened and made
// overload-aware by fix #3424: see the header.
std::vector<BuiltinFormals> builtinParamNameCandidates(std::string_view typeHead,
                                                       std::string_view member,
                                                       std::size_t argCount) {
    // The static call spelling is `String.From(...)`; the instance receiver's
    // head is the `string` keyword. One table answers both.
    std::vector<BuiltinFormals> out;
    collectRows(typeHead, member, argCount, out);
    return out;
}  // per §22.5 / stdlib §1 / §9.6 / §3.1 / §3.2

// fix #3425 (§22.5 / §17.7, v0.283): see the header.
std::vector<BuiltinFormals> builtinCtorParamNameCandidates(
    std::string_view typeHead, std::size_t argCount) {
    std::vector<BuiltinFormals> out;
    collectRows(typeHead, /*member=*/"", argCount, out);
    // Spec §31.4: every standard exception type declares `constructor(message:
    // string)`; the arity-2 forms disagree across the family and are the rows
    // above. Answered from the hierarchy rather than one row per name so a new
    // built-in exception cannot silently lose its named-argument surface.
    if (argCount == 1 && isBuiltinExceptionName(typeHead)) {
        BuiltinFormals f;
        f.names.push_back("message");
        f.required = 1;
        out.push_back(std::move(f));
    }
    return out;
}  // per §22.5 / §17.7 / §31.4 / stdlib §2.5 / §3.1 / §4 / §6 / §7.2

// fix #6782 (spec §7 "parameter pass" and §7.3 "Assigning `null` to a
// non-nullable target", v0.340): the built-in members and constructors whose
// parameter at `position` is declared NON-nullable in EVERY overload of that
// argument count. Each row is (head, member or "" for a constructor, argument
// count, position). A row says nothing about the parameter's TYPE: several
// rows have overloads of different types at the same position
// (`StringBuilder(initial: string)` and `(capacity: int)`, `Append(value:
// string)` and `Append(value: Object)`, `Int32.ToString(format: string)` and
// its radix form, `Parse(s: string)` and `Parse(s: ReadOnlyStringSpan)`), and
// overload resolution, not this table, decides between them. What the rows
// share is that no overload at that position takes null. The declarations that
// do (`String.IsNullOrWhiteSpace(value: string?)`, the `Equals(other:
// string?, ignoreCase)` form) are deliberately absent. Standard-library
// section cited per group.
bool builtinParamIsNonNullable(std::string_view typeHead,
                               std::string_view member, std::size_t argCount,
                               std::size_t position) {
    // §31.4: every standard exception type's one- and two-argument
    // constructors are `(message: string)` and either `(message: string,
    // cause: Exception)` (the surface of a type that declares no constructor;
    // `TimeoutException` spells the second `inner: Exception`) or `(message:
    // string, paramName: string)` (`ArgumentException`,
    // `ArgumentOutOfRangeException`). No position of any of them is nullable.
    if (member.empty() && (argCount == 1 || argCount == 2) &&
        position < argCount && isBuiltinExceptionName(typeHead))
        return true;
    // A primitive receiver or a static call spelled with its keyword
    // (`int.Parse`, `bool.Parse`) names the same stdlib type as the declared
    // head; fix #7296 (§9.1, v0.344) extended that to the other integer
    // keywords — "SByte, Byte, Int16, UInt16, Int32, UInt32, Int64, UInt64".
    const std::string_view head = normaliseBuiltinParamHead(typeHead);
    // fix #7421 (stdlib §9.6, §7.3, v0.345): the §9.6 `String` head's
    // nullability is asked by the §9.6 DECLARED-PARAMETER pass
    // (`ExprTC::checkBuiltinStringDeclaredParams`), which reads the SAME
    // rows' `nonNullable` column — so this query stands down for that head.
    // It is the one-diagnostic-per-call discipline: the §7 nullability GATE
    // must not add a second diagnostic to a `string.Join` / `string.Repeat`
    // call whose dedicated gate has already refused the argument, and the
    // §9.6 pass reports the nullability with the §7.3 rule named.
    if (head == "string") return false;
    for (const auto& r : kBuiltinParams) {
        if (r.head != head || r.member != member) continue;
        if (r.total != argCount) continue;
        if (position < r.total && (r.nonNullable & (1u << position)))
            return true;
    }
    return false;
}  // per §7.3 (v0.340) / stdlib §2.3–§9.5, §31.4, fix #6782

// The ONE read of the built-in parameter table: the declared forms of
// `typeHead.member` (or of the constructor `typeHead`, when `member` is
// empty) at `argCount` arguments, each carrying its parameter NAMES (§22.5),
// its parameter TYPES (§5.1) and its parameter NULLABILITY (§7.3).
//
// `argCount` == kBuiltinAnyArity returns every declared form of the member,
// whatever its arity — the question "does the standard library declare this
// member at all?" is the §22.6 arity question's own.
std::vector<BuiltinParamForm> builtinParamForms(std::string_view typeHead,
                                                 std::string_view member,
                                                 std::size_t argCount) {
    const std::string_view head = normaliseBuiltinParamHead(typeHead);
    std::vector<BuiltinParamForm> out;
    for (const auto& r : kBuiltinParams) {
        if (r.head != head || r.member != member) continue;
        if (argCount != kBuiltinAnyArity && r.total != argCount) continue;
        BuiltinParamForm f;
        f.total = r.total;
        f.required = r.required;
        f.nonNullable = r.nonNullable;
        f.sig = r.sig;
        for (std::size_t i = 0; i < r.total; ++i) {
            f.names.push_back(r.names[i]);
            f.kinds.push_back(r.kinds[i]);
        }
        out.push_back(std::move(f));
    }
    return out;
}  // per §22.5 / §22.6 / §5.1 / §7.3, stdlib §2.3-§9.6

bool isBuiltinCtorLoweredByName(std::string_view head) {
    return contains(kBuiltinCtorLoweredByNameHeads, head);
}  // per §22.5 / §17.7, fix #3425 (the #3098 `lowerNew` arm)

bool isBuiltinCtorSurfaceClosedByRows(std::string_view head) {
    return contains(kBuiltinCtorSurfaceClosedByRowsHeads, head);
}  // per §22.5 / §17.7, stdlib §2.5, fix #7502

bool isBuiltinNamedArgTableHead(std::string_view head) {
    return contains(kBuiltinNamedArgTableHeads, head);
}  // per §22.5, fix #3398

bool isStringLinqOperatorMemberName(std::string_view n) {
    return contains(kStringLinqOperatorMemberNames, n);
}  // per §5 / §9.6, fix #3589

bool isBuiltinCollectionLinqOperatorName(std::string_view n) {
    return contains(kBuiltinCollectionLinqOperatorNames, n);
}  // per §5, fix #3589

bool isLinqElementLambdaOperatorName(std::string_view m) {
    return contains(kLinqElementLambdaOperatorNames, m);
}  // per §5, fix #3670

bool isLinqLazyPassthroughOperatorName(std::string_view m) {
    return contains(kLinqLazyPassthroughOperatorNames, m);
}  // per §5, fix #3670

bool isLazyReverseDirectReceiverHead(std::string_view head) {
    return contains(kLazyReverseDirectReceiverHeads, head);
}  // per §5 / §4.6 / §4.7 / §4.10 / §4.18, fix #3763

bool isBuiltinIntLengthPropertyHead(std::string_view head) {
    return contains(kBuiltinIntLengthPropertyHeads, head);
}  // per §5.1 / §3.1 / §4.4 / §4.5 / §4.11 / §4.14 / §4.19, fix #4766 + #4833

bool isBuiltinIntCountPropertyHead(std::string_view head) {
    return contains(kBuiltinIntCountPropertyHeads, head);
}  // per §5.1 / §4.17 / §4.18 / §6.9, fix #4766 + #4833

// fix #5044 (§2.3 Row 10, stdlib §4, v0.307): the §2.3 always-available
// bare-name Row 10 collection/iterator heads. The single authority for
// `isAlwaysAvailableBareName`'s Row 10 chain, replacing the hand-spelled
// comparison chain that previously duplicated this list independently.
// kBuiltinInterfaceHeads (which is in the declaration section below) holds
// the span-interface heads separately; those are included here too because
// §2.3 Row 10 covers stdlib §4 in its entirety.
constexpr std::array<std::string_view, 46> kAlwaysAvailableBareNameCollections = {
    // §4.4 / §4.5: array-backed collections
    "Array", "OwningArray",
    // §4.11: stack / queue
    "Stack", "OwningStack", "Queue", "OwningQueue",
    // §4.10 / §4.18: set types
    "Set", "OwningSet", "SortedSet",
    // §4.8 / §4.17: dictionary types
    "Dictionary", "OwningDictionary", "SortedDictionary",
    "DictionaryEntry", "DrainedEntry",
    // §4.6 / §4.7: linked list
    "LinkedList", "OwningLinkedList", "LinkedListNode",
    // §4.14 / §4.19: buffer / frozen array
    "OwningBuffer", "FrozenArray",
    // §4.15: span types
    "Span", "ReadOnlySpan",
    // §4.16: bit reader / writer
    "BitReader", "BitWriter",
    // §4.15: array span types
    "ArraySpan", "ReadOnlyArraySpan",
    "ArrayBitReader", "ArrayBitWriter",
    // §4.12 / §4.13: iterator interfaces
    "IEnumerable", "IEnumerator",
    "IAsyncEnumerable", "IAsyncEnumerator",
    "IOwningEnumerable", "IOwningEnumerator",
    "IOwningAsyncEnumerable", "IOwningAsyncEnumerator",
    // §4.4 / §4.5: collection interfaces
    "IReadOnlyCollection", "IReadOnlyList",
    "ICollection", "IList",
    "IOwningCollection", "IOwningList",
    // §4.15 / §4.4 / §4.5 / §4.8: supporting interfaces
    "IReadOnlySpan", "IEqualityComparer",
    // §4.14: slot map
    "Counter", "OwningSlotMap", "SlotHandle",
};

bool isAlwaysAvailableBareNameCollection(std::string_view n) {
    return contains(kAlwaysAvailableBareNameCollections, n);
}  // per §2.3 / stdlib §4, fix #5044

}  // namespace bezel::common
