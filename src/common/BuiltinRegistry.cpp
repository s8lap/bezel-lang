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
struct BuiltinParamNameRow {
    std::string_view head;
    std::string_view member;   // empty for a CONSTRUCTOR row
    std::uint8_t total;
    std::uint8_t required;
    std::array<std::string_view, 8> names;
};

// stdlib §9.6 `String` (v0.283). The STATIC members are keyed under the same
// `"string"` head as the instance ones — no §9.6 member name is both, so one
// table answers both spellings and the lookup normalizes `"String"` to
// `"string"`.
//
// The four overload pairs #3398 had to OMIT — `Replace/2`, `Repeat/2`,
// `Split/1`, `AsSpan/1` — are present since #3424 as MULTIPLE candidate rows
// at the same argument count. `Trim/0`, `ToUpper/0` and the other nullary
// members carry no parameters and so cannot take a named argument at all.
constexpr std::array<BuiltinParamNameRow, 37> kStringParamNames = {{
    // Construction (static)
    {"string", "From", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"string", "From", 3, 3, {"buffer", "start", "length", "", "", "", "", ""}},  // renamed v0.282 (§1)
    {"string", "FromCStringPointer", 1, 1, {"p", "", "", "", "", "", "", ""}},
    {"string", "FromWStringPointer", 1, 1, {"p", "", "", "", "", "", "", ""}},
    {"string", "Join", 2, 2, {"separator", "items", "", "", "", "", "", ""}},
    // §9.6 declares two arity-2 `Repeat` overloads that disagree (#3424).
    {"string", "Repeat", 2, 2, {"c", "count", "", "", "", "", "", ""}},
    {"string", "Repeat", 2, 2, {"unit", "count", "", "", "", "", "", ""}},
    // Inspection (static). fix #3854 (stdlib §9.6, v0.290): `IsNullOrEmpty`
    // was impl-ahead-of-spec surface (retired at v0.290, proposal 0123 R6) —
    // deleted, not just left unregistered here, so its row does not silently
    // resurrect a named-argument binding for a call the sema layer now rejects.
    {"string", "IsNullOrWhiteSpace", 1, 1, {"value", "", "", "", "", "", "", ""}},
    // Inspection (instance)
    {"string", "Contains", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"string", "StartsWith", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"string", "EndsWith", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"string", "IndexOf", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"string", "IndexOf", 2, 2, {"value", "startIndex", "", "", "", "", "", ""}},
    {"string", "LastIndexOf", 1, 1, {"value", "", "", "", "", "", "", ""}},
    // Substring / span views
    {"string", "Substring", 1, 1, {"startIndex", "", "", "", "", "", "", ""}},
    {"string", "Substring", 2, 2, {"startIndex", "length", "", "", "", "", "", ""}},
    // §9.6 declares `AsSpan(start)` and `AsSpan(r: Range<int>)` (#3424).
    {"string", "AsSpan", 1, 1, {"start", "", "", "", "", "", "", ""}},
    {"string", "AsSpan", 1, 1, {"r", "", "", "", "", "", "", ""}},
    {"string", "AsSpan", 2, 2, {"start", "length", "", "", "", "", "", ""}},
    // Modification
    {"string", "Trim", 1, 1, {"chars", "", "", "", "", "", "", ""}},
    {"string", "PadLeft", 1, 1, {"totalWidth", "", "", "", "", "", "", ""}},
    {"string", "PadLeft", 2, 2, {"totalWidth", "paddingChar", "", "", "", "", "", ""}},
    {"string", "PadRight", 1, 1, {"totalWidth", "", "", "", "", "", "", ""}},
    {"string", "PadRight", 2, 2, {"totalWidth", "paddingChar", "", "", "", "", "", ""}},
    {"string", "PadCenter", 1, 1, {"totalWidth", "", "", "", "", "", "", ""}},
    {"string", "PadCenter", 2, 2, {"totalWidth", "paddingChar", "", "", "", "", "", ""}},
    {"string", "Insert", 2, 2, {"startIndex", "value", "", "", "", "", "", ""}},
    {"string", "Remove", 1, 1, {"startIndex", "", "", "", "", "", "", ""}},
    {"string", "Remove", 2, 2, {"startIndex", "length", "", "", "", "", "", ""}},  // renamed v0.282 (§1)
    // §9.6 declares two arity-2 `Replace` overloads that disagree (#3424).
    {"string", "Replace", 2, 2, {"oldChar", "newChar", "", "", "", "", "", ""}},
    {"string", "Replace", 2, 2, {"oldValue", "newValue", "", "", "", "", "", ""}},
    // §9.6 declares `Split(separator: char)`, `Split(separator: string)` and
    // `Split(separators: Array<char>)` — two candidate names at arity 1.
    {"string", "Split", 1, 1, {"separator", "", "", "", "", "", "", ""}},
    {"string", "Split", 1, 1, {"separators", "", "", "", "", "", "", ""}},
    // Comparison and equality. fix #3853 (stdlib §9.6, v0.290, proposal 0123
    // R6): the static `Compare(a, b)` / `Compare(a, b, ignoreCase)` rows this
    // table used to carry were RETIRED — deleted, not just left unregistered,
    // so they do not silently resurrect a named-argument binding for a call
    // the sema layer now rejects outright (the #3854 `IsNullOrEmpty` pattern
    // above). The instance replacement is the 2-arg `CompareTo` row below,
    // parallel to `Equals`'s.
    {"string", "CompareTo", 1, 1, {"other", "", "", "", "", "", "", ""}},
    {"string", "CompareTo", 2, 2, {"other", "ignoreCase", "", "", "", "", "", ""}},
    // §9.6 declares BOTH `Equals(other: string?)` and the `Object?` override
    // at arity 1, and they AGREE on the name, so the single row answers both.
    {"string", "Equals", 1, 1, {"other", "", "", "", "", "", "", ""}},
    {"string", "Equals", 2, 2, {"other", "ignoreCase", "", "", "", "", "", ""}},
}};

// stdlib §3.2 `Encoding` (v0.290). No overload at a shared arity disagrees on
// names here, so the whole declared surface is covered.
//
// fix #3851 (stdlib §3.2 / §1, v0.290, proposal 0123 R5 Option B): dropped
// the three-argument `GetString(bytes, start, length)` row (was renamed to
// the (start, length) window convention at v0.282; retired outright at
// v0.290 — a window over an Array<byte> is a slice, not a parameter pair).
// The call form is rejected in sema (ExprTypecheck_Calls.cpp) ahead of the
// named-argument lookup this table serves, so removing the row costs no
// coverage: a 3-argument `GetString` call never reaches
// `checkBuiltinNamedArgs` un-rejected.
constexpr std::array<BuiltinParamNameRow, 4> kEncodingParamNames = {{
    {"Encoding", "GetBytes", 1, 1, {"s", "", "", "", "", "", "", ""}},
    {"Encoding", "GetBytes", 2, 2, {"s", "destination", "", "", "", "", "", ""}},
    {"Encoding", "GetString", 1, 1, {"bytes", "", "", "", "", "", "", ""}},
    {"Encoding", "GetByteCount", 1, 1, {"s", "", "", "", "", "", "", ""}},
}};

// fix #3424 — stdlib §3.1 `StringBuilder` (v0.283). Every `Append` /
// `AppendLine` overload declares its single parameter `value`, so one row per
// argument count answers all of them.
constexpr std::array<BuiltinParamNameRow, 6> kStringBuilderParamNames = {{
    {"StringBuilder", "Append", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"StringBuilder", "AppendLine", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"StringBuilder", "AppendFormat", 2, 2, {"format", "args", "", "", "", "", "", ""}},
    {"StringBuilder", "Insert", 2, 2, {"index", "value", "", "", "", "", "", ""}},
    {"StringBuilder", "Remove", 2, 2, {"startIndex", "length", "", "", "", "", "", ""}},
    {"StringBuilder", "Replace", 2, 2, {"oldValue", "newValue", "", "", "", "", "", ""}},
}};
// fix #3424 (stdlib §2.2 / spec §22.5, v0.315): `Math` static methods that
// declare named parameters.  DivRem has an `out` parameter; §22.5 makes the
// `out` slot positional-only but the by-value `a` and `b` slots may be named.
// Uses the partial-naming shape: the `out` slot is an empty string.
constexpr std::array<BuiltinParamNameRow, 15> kMathParamNames = {{
    {"Math", "Abs", 1, 1, {"x", "", "", "", "", "", "", ""}},
    {"Math", "Min", 2, 2, {"a", "b", "", "", "", "", "", ""}},
    {"Math", "Max", 2, 2, {"a", "b", "", "", "", "", "", ""}},
    {"Math", "Clamp", 3, 3, {"value", "low", "high", "", "", "", "", ""}},
    {"Math", "Sqrt", 1, 1, {"x", "", "", "", "", "", "", ""}},
    {"Math", "Pow", 2, 2, {"x", "y", "", "", "", "", "", ""}},
    {"Math", "Floor", 1, 1, {"x", "", "", "", "", "", "", ""}},
    {"Math", "Ceiling", 1, 1, {"x", "", "", "", "", "", "", ""}},
    {"Math", "Round", 2, 1, {"x", "digits", "", "", "", "", "", ""}},
    {"Math", "Truncate", 1, 1, {"x", "", "", "", "", "", "", ""}},
    {"Math", "Sign", 1, 1, {"x", "", "", "", "", "", "", ""}},
    {"Math", "Fmod", 2, 2, {"x", "y", "", "", "", "", "", ""}},
    {"Math", "IEEERemainder", 2, 2, {"x", "y", "", "", "", "", "", ""}},
    // DivRem overloads (stdlib §2.2): 3 params, 2 named (a, b), 1 positional-only.
    {"Math", "DivRem", 3, 2, {"a", "b", "", "", "", "", "", ""}},
    {"Math", "DivRem", 3, 2, {"a", "b", "", "", "", "", "", ""}},  // long overload
}};

// fix #3424 (stdlib §2.3 / spec §22.5, v0.315): `Convert` static methods that
// declare named parameters.  Each TryTo* method has an `out` parameter; §22.5 makes
// the `out` slot positional-only but the by-value `value` slot may be named.
// Uses the partial-naming shape: the `out` slot is an empty string.
constexpr std::array<BuiltinParamNameRow, 12> kConvertParamNames = {{
    {"Convert", "ToInt32", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"Convert", "ToInt64", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"Convert", "ToDouble", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"Convert", "ToDecimal", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"Convert", "ToBoolean", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"Convert", "ToString", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"Convert", "ToString", 2, 2, {"value", "toBase", "", "", "", "", "", ""}},
    // TryTo* overloads (stdlib §2.3): 2 params, 1 named (value), 1 positional-only.
    {"Convert", "TryToInt32", 2, 1, {"value", "", "", "", "", "", "", ""}},
    {"Convert", "TryToInt64", 2, 1, {"value", "", "", "", "", "", "", ""}},
    {"Convert", "TryToDouble", 2, 1, {"value", "", "", "", "", "", "", ""}},
    {"Convert", "TryToDecimal", 2, 1, {"value", "", "", "", "", "", "", ""}},
    {"Convert", "TryToBoolean", 2, 1, {"value", "", "", "", "", "", "", ""}},
}};

// fix #3425 — the built-in CONSTRUCTOR rows) (§17.7's closed constructor
// surface; the argument-taking constructors the standard library declares).
// A head absent here rejects a named `new` argument loudly rather than
// binding it positionally, so the coverage below is a floor, not a contract
// (the remaining argument-taking heads are issue 3534).
//
// The stdlib heads whose only declared constructor is parameterless
// (`SortedDictionary`, `SortedSet`, `Stack`, `Queue`, `Mutex`, `Stopwatch`,
// `CancellationTokenSource`, `TaskScope`, the event sources, the
// task-completion sources, …) carry no row for the same reason `Trim/0`
// carries none: a nullary constructor cannot take a named argument.
//
// fix #3533 (stdlib §2.5 / spec §22.5, v0.315): DateTime's eight-parameter
// constructor (year, month, day, hour, minute, second, millisecond, kind) — all
// eight parameters are named in the table. The row uses the 8-element names array
// (wider than the pre-#3533 four-element limit).
constexpr std::array<BuiltinParamNameRow, 35> kBuiltinCtorParamNames = {{
    // stdlib §2.5 — the temporal value records.
    {"DateTime", "", 3, 3, {"year", "month", "day", "", "", "", "", ""}},
    {"DateTime", "", 8, 6, {"year", "month", "day", "hour", "minute", "second", "millisecond", "kind"}},
    {"DateOnly", "", 3, 3, {"year", "month", "day", "", "", "", "", ""}},
    {"TimeOnly", "", 4, 2, {"hour", "minute", "second", "millisecond", "", "", "", ""}},
    // stdlib §3.1 — `StringBuilder(capacity)` and `StringBuilder(initial)`
    // share an argument count and disagree on names: two candidates (#3424's
    // overload-aware lookup is what admits them).
    {"StringBuilder", "", 1, 1, {"capacity", "", "", "", "", "", "", ""}},
    {"StringBuilder", "", 1, 1, {"initial", "", "", "", "", "", "", ""}},
    // stdlib §3.3 / §31.4 — the two record-shaped reserved builtins whose
    // `lowerNew` arm honours the binding BY NAME (#3098); see
    // `isBuiltinCtorLoweredByName`.
    {"ReadOnlyStringSpan", "", 3, 3, {"Source", "Start", "Length", "", "", "", "", ""}},
    {"ExceptionInfo", "", 3, 3, {"TypeName", "Message", "StackTrace", "", "", "", "", ""}},
    // stdlib §4.14 / §4.15 — the span records' direct constructors. Lowered
    // by POSITION (LlvmBackend_Exprs2.cpp), so a reorder stays rejected.
    {"Span", "", 3, 3, {"Buffer", "Start", "Length", "", "", "", "", ""}},
    {"ReadOnlySpan", "", 3, 3, {"Buffer", "Start", "Length", "", "", "", "", ""}},
    {"ArraySpan", "", 3, 3, {"Source", "Start", "Length", "", "", "", "", ""}},
    {"ReadOnlyArraySpan", "", 3, 3, {"Source", "Start", "Length", "", "", "", "", ""}},
    // stdlib §4.2 / §4.5 / §4.6 / §4.8 / §4.14 — the collections.
    {"Array", "", 1, 1, {"capacity", "", "", "", "", "", "", ""}},
    {"Array", "", 2, 2, {"size", "fill", "", "", "", "", "", ""}},
    {"OwningArray", "", 1, 1, {"capacity", "", "", "", "", "", "", ""}},
    {"OwningBuffer", "", 1, 1, {"length", "", "", "", "", "", "", ""}},
    {"OwningBuffer", "", 2, 2, {"length", "init", "", "", "", "", "", ""}},
    {"LinkedList", "", 1, 1, {"source", "", "", "", "", "", "", ""}},
    {"Dictionary", "", 1, 1, {"capacity", "", "", "", "", "", "", ""}},
    {"Dictionary", "", 1, 1, {"comparer", "", "", "", "", "", "", ""}},
    {"Dictionary", "", 2, 2, {"capacity", "comparer", "", "", "", "", "", ""}},
    {"OwningDictionary", "", 1, 1, {"capacity", "", "", "", "", "", "", ""}},
    {"OwningDictionary", "", 1, 1, {"comparer", "", "", "", "", "", "", ""}},
    {"OwningDictionary", "", 2, 2, {"capacity", "comparer", "", "", "", "", "", ""}},
    {"Set", "", 1, 1, {"capacity", "", "", "", "", "", "", ""}},
    {"Set", "", 1, 1, {"comparer", "", "", "", "", "", "", ""}},
    {"Set", "", 2, 2, {"capacity", "comparer", "", "", "", "", "", ""}},
    {"OwningSet", "", 1, 1, {"capacity", "", "", "", "", "", "", ""}},
    {"OwningSet", "", 1, 1, {"comparer", "", "", "", "", "", "", ""}},
    {"OwningSet", "", 2, 2, {"capacity", "comparer", "", "", "", "", "", ""}},
    // stdlib §2.16 / §6.3 / §6.4 / §6.7 / §7.2 / §8.2 — the remaining
    // argument-taking reserved heads.
    {"Shared", "", 1, 1, {"value", "", "", "", "", "", "", ""}},
    {"Random", "", 1, 1, {"seed", "", "", "", "", "", "", ""}},
    {"Semaphore", "", 1, 1, {"initialCount", "", "", "", "", "", "", ""}},
    {"Semaphore", "", 2, 2, {"initialCount", "maxCount", "", "", "", "", "", ""}},
    {"Channel", "", 1, 1, {"capacity", "", "", "", "", "", "", ""}},
}};

// fix #3534 (stdlib §2.14 / §2.15 / §7.2 / spec §22.5, v0.315): add
// FileStream, JsonSerializer, JsonDeserializer, and BinaryDeserializer
// constructor rows.  FileStream's 2-param form defaults `access` to
// FileAccess.ReadWrite, so the row says total=2, required=2 (all positional
// args must be supplied by the caller; the default is applied post-binding).
// JsonSerializer has two arity-1 candidates (no-arg and options); overload
// resolution picks the no-arg form when no argument is given, the options form
// when JsonOptions is passed.  Each JsonDeserializer input form and each
// BinaryDeserializer form has its own row; the 2-param forms use the partial
// naming shape (one param named, one positional).
constexpr std::array<BuiltinParamNameRow, 6> kBuiltinSerializationCtorParamNames = {{
    // stdlib §7.2 — FileStream.
    {"FileStream", "", 2, 2, {"path", "mode", "", "", "", "", "", ""}},
    {"FileStream", "", 3, 3, {"path", "mode", "access", "", "", "", "", ""}},
    // stdlib §2.15 — JsonSerializer.
    {"JsonSerializer", "", 1, 1, {"options", "", "", "", "", "", "", ""}},
    // stdlib §2.15 — JsonDeserializer.
    {"JsonDeserializer", "", 1, 1, {"input", "", "", "", "", "", "", ""}},
    {"JsonDeserializer", "", 2, 2, {"input", "options", "", "", "", "", "", ""}},
    // stdlib §2.14 — BinaryDeserializer.
    // BinaryDeserializer(input, ignoreUnknownFields) — both named.
    {"BinaryDeserializer", "", 2, 2, {"input", "ignoreUnknownFields", "", "", "", "", "", ""}},
}};

// fix #3425 — the built-in EXCEPTION constructors (spec §31.4). `Exception`
// declares `(message)` and `(message, cause)`; `ArgumentException` and
// `ArgumentOutOfRangeException` declare `(message, paramName)` instead
// (stdlib §2.5); `TimeoutException` declares `(message, inner)` (stdlib §6.1).
//
// The one-argument `(message)` form is shared by the WHOLE family and is
// answered generically in the lookup below from `isBuiltinExceptionName`, so
// only the arity-2 forms — where the family DISAGREES — need rows here.
constexpr std::array<BuiltinParamNameRow, 4> kBuiltinExceptionCtorParamNames = {{
    {"Exception", "", 2, 2, {"message", "cause", "", "", "", "", "", ""}},
    {"ArgumentException", "", 2, 2, {"message", "paramName", "", "", "", "", "", ""}},
    {"ArgumentOutOfRangeException", "", 2, 2, {"message", "paramName", "", "", "", "", "", ""}},
    {"TimeoutException", "", 2, 2, {"message", "inner", "", "", "", "", "", ""}},
}};

// fix #3425: the two record-shaped builtins whose `lowerNew` arm resolves each
// argument to its FIELD SLOT BY NAME before lowering (#3098,
// LlvmBackend_Exprs2.cpp). See the header.
constexpr std::array<std::string_view, 2> kBuiltinCtorLoweredByNameHeads = {
    "ExceptionInfo", "ReadOnlyStringSpan",
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

// Collect every row of `tbl` whose declared shape admits `argCount`
// arguments, in the table's declaration order. Shared by the member and
// constructor lookups so the "required <= argCount <= total" admission rule
// has one spelling. // per §22.5, fix #3424 / #3425
template <typename Tbl>
void collectRows(const Tbl& tbl, std::string_view head, std::string_view member,
                 std::size_t argCount, std::vector<BuiltinFormals>& out) {
    for (const auto& r : tbl) {
        if (r.head != head || r.member != member) continue;
        if (argCount < r.required || argCount > r.total) continue;
        BuiltinFormals f;
        f.names.assign(r.names.begin(),
                       r.names.begin() + static_cast<std::ptrdiff_t>(r.total));
        f.required = r.required;
        out.push_back(std::move(f));
    }
}

}  // namespace

// fix #3398 (§22.5, v0.283; stdlib §1 / §9.6 / §3.2), widened and made
// overload-aware by fix #3424: see the header.
std::vector<BuiltinFormals> builtinParamNameCandidates(std::string_view typeHead,
                                                       std::string_view member,
                                                       std::size_t argCount) {
    // The static call spelling is `String.From(...)`; the instance receiver's
    // head is the `string` keyword. One table answers both.
    if (typeHead == "String") typeHead = "string";
    std::vector<BuiltinFormals> out;
    if (typeHead == "string")
        collectRows(kStringParamNames, typeHead, member, argCount, out);
    else if (typeHead == "Encoding")
        collectRows(kEncodingParamNames, typeHead, member, argCount, out);
    else if (typeHead == "StringBuilder")
        collectRows(kStringBuilderParamNames, typeHead, member, argCount, out);
    else if (typeHead == "Math")
        collectRows(kMathParamNames, typeHead, member, argCount, out);
    else if (typeHead == "Convert")
        collectRows(kConvertParamNames, typeHead, member, argCount, out);
    return out;
}  // per §22.5 / stdlib §1 / §9.6 / §3.1 / §3.2

// fix #3425 (§22.5 / §17.7, v0.283): see the header.
std::vector<BuiltinFormals> builtinCtorParamNameCandidates(
    std::string_view typeHead, std::size_t argCount) {
    std::vector<BuiltinFormals> out;
    collectRows(kBuiltinCtorParamNames, typeHead, /*member=*/"", argCount, out);
    collectRows(kBuiltinExceptionCtorParamNames, typeHead, /*member=*/"",
                argCount, out);
    collectRows(kBuiltinSerializationCtorParamNames, typeHead, /*member=*/"",
                argCount, out);
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

bool isBuiltinCtorLoweredByName(std::string_view head) {
    return contains(kBuiltinCtorLoweredByNameHeads, head);
}  // per §22.5 / §17.7, fix #3425 (the #3098 `lowerNew` arm)

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
