#pragma once

// #1414 Stage 3 — the data-only builtin registry.
//
// A single home for the name-keyed facts about the compiler's built-in
// (stdlib) types that were previously scattered across sema, ownership, and
// codegen as hand-maintained string-comparison chains.  The motivating bug
// family — the v0.168 `Owned*`→`Owning*` rename that missed 16 `rfind`
// prefix checks, the SortedDictionary i32-truncation (#1406), and the
// serializer pointer truncation (#1390) — all trace to the same failure mode:
// the same name set spelled out by hand in several places, one of which is
// forgotten when a member is added or renamed.
//
// This module is DATA ONLY: each entry point is a pure `std::string_view`
// membership / prefix test backed by a literal table in BuiltinRegistry.cpp.
// The existing predicates (isCollectionTypeName, isDictTypeName,
// isOwningCollectionNameHead, isOwningTypeNameHead, the LlvmBackend_Layout
// rep hand-lists, the 16 `rfind("Owning",0)` sites) delegate here so a
// builtin's facts live in exactly one place.  Behaviour is preserved
// name-for-name; tests/support/builtin_registry_snapshot_test.cpp pins each
// table against the pre-refactor snapshot.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bezel::common {

// The `Owning*` family PREFIX test — exactly `head.rfind("Owning", 0) == 0`.
// Consolidates the 16 codegen sites that ask "does this receiver type name
// begin with `Owning`?" (element-owning collection / owning wrapper).  Kept as
// a prefix test (not set membership) so it is behaviour-identical to the sites
// it replaces regardless of which concrete names exist. // per §5.11 / #1372
bool isOwningPrefixedName(std::string_view head);

// The element-owning COLLECTION heads (§28.6 / §5.11, #1008 Part C): the
// borrow-return and read-only-capability rules apply to exactly these.
bool isOwningCollectionHead(std::string_view head);

// The owning TYPE heads (§28.3 / §5.11): the owning collections above plus the
// owning wrappers (Shared, Weak, ChannelSender, OwningTask, IOwningEnumerable,
// CancellationTokenSource) whose non-`owned` return is an owned producer.
bool isOwningTypeHead(std::string_view head);

// The plain (non-`owning`) builtin concrete collection class heads that carry
// no `ast::TypeDecl` (declaration-less `TypeRef`s) — `Array`, `Dictionary`,
// `Set`, `Stack`, `Queue`, `LinkedList`, `SortedDictionary`, `SortedSet`. Each
// is a class type (§3.1), so §28.3 row 4's static-return rejection applies to
// an unmarked return of one (#5123, v0.313).
bool isPlainBuiltinCollectionHead(std::string_view head);

// The Task-family type heads — `Task` and `OwningTask` (§28.3 "Task-family
// calls produce owned frames", v0.190, SB-01). A pre-substitution, name-keyed
// test used by the ownership pass to recognize a `Task`/`Task<T>` return whose
// container ownership transfers to the caller. Note this INCLUDES the plain
// `Task` head that `isOwningTypeHead` deliberately excludes: a `Task` return is
// ownership-transferring only in a NON-`async` body (the caller consults this
// under a `!isAsync_` guard), whereas the owning-type set applies to both sync
// and async bodies. `OwningTask` is in both sets.
bool isTaskFamilyHead(std::string_view head);

// §28.3 / §29.3 / §30.7 (v0.201, #2048 / proposal 0048): the iterable or
// enumerator interface heads (`IEnumerable<T>`, `IEnumerator<T>`,
// `IAsyncEnumerable<T>`, `IAsyncEnumerator<T>`, and the four `IOwning*`
// forms) — the iterator convention whose unmarked return type is itself the
// announcement of a caller-owned deferred-activation frame. A static
// signature with one of these return types is in the announced set of the
// §28.3 *Factory methods and sourceless returns* declaration-site error and
// needs no `owned` marker. // per §28.3 v0.201
bool isIterableInterfaceHead(std::string_view head);

// §28.6 / §28.3 (v0.221, #1787 ruling; #2369): the OWNING iterable-interface
// heads (`IOwningEnumerable<T>`, `IOwningEnumerator<T>`,
// `IOwningAsyncEnumerable<T>`, `IOwningAsyncEnumerator<T>`) — the four
// consumer-disposed, non-idempotent deferred-activation interfaces. §28.6
// bullet 1 excludes these from the receiver-owned-field bare-read borrow
// carve-out ("The same method-return exclusion applies to a field typed at an
// owning iterable-interface head"), so `checkBorrowReturnedThroughOwningType`'s
// `deferredActivationOwningHead` gate consults this set to reject `return
// this._field;` typed at any of the four (the #1660 carve-out removal,
// extended from `IOwningEnumerable` alone to all four owning iterable heads).
// per §28.6, §28.3
bool isOwningIterableInterfaceHead(std::string_view head);

// #3419 / §29.2 / stdlib §4.12–§4.13: the four ITERATOR-FORMAT ENUMERABLE
// heads — `IEnumerable<T>`, `IOwningEnumerable<T>`, `IAsyncEnumerable<T>`,
// `IOwningAsyncEnumerable<T>`. These are the DECLARED-HEAD axis of the §29.2
// enumerable-wrap grid: a value coerced to one of them must become a real
// vtable-backed enumerator, because the iteration and disposal paths read the
// value's first word as a vtable pointer (a raw `bz_array_t*` there is a
// SIGSEGV). The four ENUMERATOR heads are deliberately absent — an enumerator
// is what a wrap PRODUCES, not what it is created for; `isIterableInterfaceHead`
// is the eight-name set for the separate §28.3 announced-set question.
//
// The grid's two axes drift independently and silently: the site that MINTS a
// wrapper frame and the site that CLAIMS it each used to spell this head list
// by hand, so widening one (#3355) left the other behind (#3392 — a frame
// minted and never claimed, 6400 B / 200 allocs). A claim NARROWER than the
// mint leaks; a claim WIDER double-frees. `scripts/check_enumerable_grid_
// spellings.py` ratchets the remaining hand-spellings down.
// per §29.2 / stdlib §4.12–§4.13, #3419
bool isIterFormatEnumerableHead(std::string_view head);

// #3993 / #4004 / #4010 (§28.3, §28.6, stdlib §4.12–§4.13): the two
// NON-OWNING iterator-format enumerable heads — `IEnumerable<T>` and
// `IAsyncEnumerable<T>` — the subtraction `isIterFormatEnumerableHead(head)
// && !isOwningIterableInterfaceHead(head)`. Neither of these two extends
// `IDisposable`/`IAsyncDisposable` (stdlib §4.12), so a borrow-return check
// whose whole reasoning is "would this double-dispose through the declared
// static type" has nothing reachable to double-call for exactly these two —
// unlike every OTHER iterable-family head (the two ENUMERATOR heads and the
// four `IOwning*` heads), which DOES extend one of those interfaces and
// measurably double-frees through the identical shape. This is a NARROWER
// exemption than `isIterFormatEnumerableHead` alone (which also contains the
// two owning heads) — using that broader predicate here would be UNSOUND,
// not merely imprecise. One shared spelling for the two ownership-pass sites
// that need exactly this subtraction (a named method/operator's return,
// #3993/#4004, and a lambda's return against an `owned` function-type
// target, #4005) rather than two independent hand-derivations of the same
// set difference drifting apart. // per §28.3, §28.6, stdlib §4.12–§4.13,
// #3419, #3993, #4004, #4005, #4007, #4010
bool isNonOwningIterFormatEnumerableHead(std::string_view head);

// The SYNCHRONOUS half of `isIterFormatEnumerableHead` — `IEnumerable<T>` and
// `IOwningEnumerable<T>`. The §29.2 synchronous `for` desugaring and the
// synchronous wrap creators (`wrapAsIEnumerableForTarget` and the local-decl /
// property-read / indexer-read faces that route to it) admit exactly these two;
// the async twins take the `await for` desugaring and its own enumerator
// acquisition, so a site that admitted all four here would wrap an async source
// with a synchronous enumerator. // per §29.2 / stdlib §4.12–§4.13, #3419
bool isSyncIterFormatEnumerableHead(std::string_view head);

// #3419 / §29.2: the four ITERATOR-FORMAT ENUMERATOR heads — `IEnumerator<T>`,
// `IOwningEnumerator<T>`, `IAsyncEnumerator<T>`, `IOwningAsyncEnumerator<T>`.
// The complement of `isIterFormatEnumerableHead` within the eight iterator/
// enumerator heads: an enumerator is what a §29.2 wrap PRODUCES (the
// MoveNext/Current/TakeCurrent vtable contract), not a declared head a value
// is coerced TO, so a receiver typed at one of these four is already the
// vtable-backed object the wrap machinery hands out — routing/dispatch sites
// (MoveNext, TakeCurrent, MoveNextAsync, explicit Dispose, the declaration-
// less §8.3 identity-Equals/GetHashCode fallback) ask this question, never
// the wrap-eligibility one `isIterFormatEnumerableHead` answers.
// per §29.2, #3419
bool isIterFormatEnumeratorHead(std::string_view head);

// The SYNCHRONOUS half of `isIterFormatEnumeratorHead` — `IEnumerator<T>` and
// `IOwningEnumerator<T>`. The async twins (`IAsyncEnumerator<T>`,
// `IOwningAsyncEnumerator<T>`) take the MoveNextAsync vtable shape and its own
// dispatch (`LlvmBackend_Async.cpp`), a disjoint lowering path. Exactly
// analogous to `isSyncIterFormatEnumerableHead` above, which is the sync half
// of the sibling (Enumerable-format) question. // per §29.2 / §29.3, #3419
bool isSyncIterFormatEnumeratorHead(std::string_view head);

// fix #3606 / §29.1 / §29.3: the SYNCHRONOUS half of
// `isOwningIterableInterfaceHead` — `IOwningEnumerable<T>` and
// `IOwningEnumerator<T>`, the two owning iterator-method return heads whose
// declared shape puts the frame under the §29.1 owning contract (TakeCurrent,
// the disposeYieldedVal owning branch, and the owned-local yield-state scan
// all apply). The two ASYNC owning heads (`IOwningAsyncEnumerable<T>`,
// `IOwningAsyncEnumerator<T>`) are deliberately excluded — they take `await
// for` and the async iterator emitter (`LlvmBackend_Async.cpp`), a disjoint
// lowering path — as are the two non-owning heads (`IEnumerable<T>`,
// `IEnumerator<T>`), which keep the borrowing/non-destructive Current
// contract. Exactly analogous to `isSyncIterFormatEnumerableHead` above,
// which is the sync half of the sibling (non-owning-restricted) grid
// question. // per §29.1 / §29.3, #3606
bool isSyncOwningIterableInterfaceHead(std::string_view head);

// fix #3468 / §29.2 / stdlib §4.12–§4.13 / §4.5 / §4.14: the SIX iteration-
// interface heads the §29.2 builtin `for-in` desugaring handles —
// `IEnumerable<T>` and `IOwningEnumerable<T>` (the iterator-format enumerables)
// plus `IReadOnlyCollection<T>`, `IReadOnlyList<T>`, `ICollection<T>` and
// `IList<T>` (the richer read/collection interfaces, which keep the
// indexer-based array protocol). A source typed at any other head belongs to a
// different for-handler.
//
// This is the FAMILY GATE, not the protocol decision: whether such a source is
// array-backed or vtable-backed is `Emitter::classifyBuiltinForInSource`'s
// second half. Both questions used to be answered TWICE — once in
// `lowerIEnumerableBuiltinFor` and once in
// `forInBuiltinEnumeratorNeedsFrameSlot`, whose own header comment declares
// itself that lowering's mirror — and the mirror had drifted in four
// independent ways, leaking an abandoned iterator's enumerator and source
// frames at three measured source shapes (issue 3468). // per §29.2, #3468
bool isBuiltinForInEnumerableHead(std::string_view head);

// #3419 / §29.2 / stdlib §4.4–§4.5 / §4.11 / §4.14: the SOURCE-FAMILY axis of
// the enumerable-wrap grid — the collection heads whose backing is the dense
// `bz_array_t*` element array AND which therefore take the ARRAY-ENUMERATOR
// wrap when widened to an iterator-format enumerable head: `Array`,
// `OwningArray`, `OwningBuffer`, `Stack`, `OwningStack`, `Queue`, `OwningQueue`.
//
// ⚠ This is NOT `isArrayBackedCollectionName`, and the difference is load-
// bearing rather than an oversight. That predicate answers "is this
// `bz_array_t*`-backed" and so also contains `Set`, `OwningSet`, `SortedSet`
// and `FrozenArray`. For the wrap question those four are wrong:
//   * a `Set<T>`/`OwningSet<T>` over a VALUE/record/string element is a
//     generic-key HASH table whose slots are not a dense element array, so it
//     is SNAPSHOTTED (`bz_lazy_from_set` + drain) rather than wrapped — only
//     the class-element set, whose backing IS the dense array, takes the
//     array wrap, and that distinction is a `genericBackingType` query, not a
//     name;
//   * `SortedSet<T>` is deliberately excluded from the wrap entirely, because
//     the snapshot is in hash order and yielding a sorted set's elements
//     unordered would silently break stdlib §4.18's ordered-iteration contract.
// Two different questions over overlapping name sets is exactly why both
// predicates exist and are each named for their own question.
// per §29.2 / stdlib §4.4–§4.5 / §4.11 / §4.14, #3419
bool isArrayEnumeratorWrapSourceName(std::string_view n);

// fix #3419 / §29.2 / §29.11 / §28.6: the SIX iterator-format HEADS the SYNC
// `using var` admission list recognises — all four iterator-format ENUMERABLE
// heads (sync + async, owning + non-owning) plus the two sync ENUMERATOR heads
// (`IEnumerator<T>`, `IOwningEnumerator<T>`). The two ASYNC enumerator heads
// are excluded because they take the `await using` path (`LlvmBackend_Async.cpp`).
//
// `LlvmBackend_Setup.cpp`'s `using var` handling used to spell this six-name
// list by hand at one site; that was the fifth independent spelling of the
// enumerable-wrap grid in this run, before the structural fix that #3419 asks
// for. Every spelling has been the SAME question, so this single predicate is
// what every consumer should read — sites that ask a DIFFERENT question
// (routing through MoveNext / TakeCurrent / GetAsyncEnumerator vtable slots)
// consult narrower subsets and are correctly excluded.
//
// A site that genuinely needs a different member subset should NOT extend
// this predicate — it should add its own, narrower predicate and have the
// ratchet flag it as drift-risk. // per §29.2 / §29.11 / §28.6, #3419
bool isIteratorFormatDispatchHead(std::string_view head);

// Array-backed collections (bz_array_t*): the receiver names whose indexer /
// .Count lower through the bz_array_* runtime.  Backs isCollectionTypeName.
// per §4.4–§4.7 / §4.14
bool isArrayBackedCollectionName(std::string_view n);

// bz_dict_t*-backed collections (K-indexed get/set).  Backs isDictTypeName.
// per §4.8 / §4.17
bool isDictionaryName(std::string_view n);

// bz_llist_t*-backed collections.  Backs isLinkedListTypeName. // per §4.6/§4.7
bool isLinkedListName(std::string_view n);

// fix #6265 / #6267 (§3.4): the collection TYPE NAMES checked for a bare-name
// static access with no declared member. isArrLikeCollectionName is the
// isArrLike family (the call form, #6265); isBareCollectionTypeName is that
// family plus the dictionary family and FrozenArray (the member form, #6267).
bool isArrLikeCollectionName(std::string_view n);
bool isBareCollectionTypeName(std::string_view n);

// #2809 / spec §32.5 / stdlib §10.1–§10.3: the three FFI-boundary BUFFER
// classes — `CString`, `WString`, `RawBytes`. Each is a raw malloc'd buffer
// (or, for RawBytes, a { ptr, len } struct over one), NOT a `%bz.string` and
// NOT an object with a header, so any handler that reaches into one at a
// `%bz.string` field offset reads out of bounds.
//
// The member-access handlers in `LlvmBackend_Exprs.cpp` recognise these three
// via `isFfiBufferTypeRef` (which reads `builtinTypeName` for the
// declaration-less ANNOTATION shape AND the built-in factory shape) so a
// `c: CString` parameter's `.Length` is dispatched to `strlen`/`bz_wstrlen`/
// `bz_rawbytes_length` rather than `loadStringLen` (#2809: the parameter
// shape's `%bz.string` field-1 read was an ASan heap-buffer-overflow). After
// #2933 the `isStringFallbackTarget` predicate in `LlvmBackend_Exprs.cpp` is
// a POSITIVE list (return true ONLY when the receiver gives no information
// to rule a `%bz.string` out), so these three names are no longer excluded
// FROM the predicate — each dedicated `.Length` arm matches on its own.
// The FFI value TYPES `IntPtr`/`UIntPtr` are deliberately NOT here: they are
// pointer-sized integers, not buffers, and no member handler dereferences one.
// per §32.5 / stdlib §10.1–§10.3
bool isFfiBufferBuiltinName(std::string_view n);

// fix #4126 (stdlib §6.1/§8.1, v0.295): the two builtin HEAP-POINTER classes
// — `Stopwatch` (stdlib §8.1, a `bz_stopwatch_t*`) and `CancellationToken`
// (stdlib §6.1, a bare pointer to shared cancellation state, null for
// `.None`) — that share the exact shape #4120 fixed for the three FFI-buffer
// classes: `SemaContext::builtinClassSym` stamps `kind = Class`
// unconditionally, neither has a Bezel vtable at word 0, and neither has a
// dedicated `.ToString()`/`.Equals()`/`.GetHashCode()` interceptor in
// `tryLowerBuiltinClassCall`, so a call to any of the three universal-Object
// members misread the receiver's raw pointer as a class-vtable object
// (`cg_isClass`/`isNamedClassRecv`, LlvmBackend_Calls2.cpp) — confirmed as a
// live SIGSEGV in `bz_string_to_utf8z` for `Stopwatch.StartNew().ToString()`
// and `.GetHashCode()`, and as silently wrong values (`CancellationToken`'s
// `.ToString()` rendering the null-receiver sentinel "null", `.Equals()`
// comparing unrelated tokens equal, `.GetHashCode()` always 0) for a
// `CancellationToken`.
//
// Kept as its own registry row rather than folded into `kFfiBufferBuiltins`:
// that set also feeds FFI-buffer-SPECIFIC behavior (malloc/free disposal,
// `.Length`/`.Pointer`/`.ToBytes`/`.CopyTo` dispatch) that does not apply to
// either of these two — a shared boolean would give a reader the false
// impression the two families are interchangeable.
// per §6.1 / §8.1, stdlib preamble (both live in `System.*` per
// `kBuiltinClassModulePaths`, BuiltinRegistry.cpp)
bool isNoOverrideIdentityBuiltinName(std::string_view n);

// The heap-pointer reference builtins shared by BOTH codegen representation
// paths (the astNode-name path and the imported-name path in
// LlvmBackend_Layout.cpp): collections, iteration/collection interfaces, task
// types, sync primitives, serializer handles, etc. — each a pointer-sized heap
// object lowered to `ptr`.  The two call sites additionally test their own
// small deltas (OwningBuffer for the astNode path; the FFI class names for the
// imported path) that are NOT shared, so those stay explicit at the sites.
// per §4 / §6 / §29 / §30 / §2.14–2.15
bool isHeapPointerReferenceBuiltin(std::string_view n);

// #2757 / §7.1 / §14.1: is `n` a RESERVED built-in type name — i.e. one of the
// names `Emitter::lowerTypeRef`'s Named case lowers specially — as opposed to a
// user-chosen generic type PARAMETER name?
//
// A declaration-less, single-part Named TypeRef is one of exactly two things: a
// reserved builtin, or a type parameter. There is no positive registry of type-
// PARAMETER names to test against (a parameter is defined by the ABSENCE of a
// declaration), so every consumer necessarily asks the question negatively —
// "not a builtin name, therefore a type parameter". That makes this set's
// COMPLETENESS load-bearing: a missing name is silently read as a type
// parameter. Three codegen sites depend on it, all with that shape:
//   * lowerTypeRef's #568 nullable-type-parameter branch — a missed name takes
//     the {i8, i32} placeholder instead of its own lowering, truncating a heap
//     pointer into a 32-bit payload (#1390, #1406, #2757: `Stopwatch?`);
//   * `Emitter::isBareTypeParamRef` (#2743);
//   * the generic-argument INFERENCE binding in LlvmBackend_Calls2.cpp (#1162)
//     — a missed name is skipped rather than bound, so the call lowers the
//     parameter to the i32 fallback while the monomorphized body uses `ptr`.
//
// This is therefore the one home for the set, not a mirror of one: #2757 was
// the third instance (after #1390 and #1406) of a hand-maintained duplicate
// drifting from it, so `isBuiltinTypeRefName` now consults this and holds no
// name list of its own. Built-in EXCEPTION names are NOT here — codegen tests
// those against its live `builtinVtable_` map, which is not a duplicated list.
// Adding a `lowerTypeRef` Named-case branch means adding its name here, in the
// same commit, plus the snapshot row in
// tests/support/builtin_registry_snapshot_test.cpp.
bool isReservedBuiltinTypeName(std::string_view n);

// fix #2994 (§17.7 "Standard-library builtins have a closed constructor
// surface", v0.241): the two arms of that rule's REJECTING population, split
// out of `kNonHeapReservedBuiltins` — the value-shaped half that codegen's
// #2974 gate exempts, and therefore the half sema must now answer for.
//
// §17.7, verbatim: "For a standard-library builtin type — any type the
// standard-library document declares — the constructor surface is exactly what
// that document declares — `new T(...)` resolves only against a declared
// constructor, never against a compiler-supplied fallback."
//
// The compiler has no table of DECLARED builtin constructors to resolve
// against — a builtin's constructors live as arms of codegen's `lowerNew`, not
// as `ConstructorSymbol`s sema can see — so the rule is applied name-keyed, to
// exactly the two populations §17.7 enumerates. Every other member of
// `kNonHeapReservedBuiltins` (`DateTime` and the temporal family,
// `CString`/`WString`/`RawBytes`, `Range`, the span shapes, `JsonOptions`,
// `OwningBuffer`) is deliberately OUT: some declare constructors that work
// today, and the rest are their own measured gaps, tracked separately rather
// than folded in here on a guess.

// Arm 1 — the builtins that declare NO constructor at all: `Guid` (stdlib
// §2.6, whose prose carries the matching insurance note), `IntPtr`/`UIntPtr`
// (stdlib §10.0). §17.7: they admit "no `new T(...)` at any argument list", and
// the diagnostic names the declared producers. `IntPtr(longValue)` is the §5.2
// explicit CONVERSION, a CallExpr — not a `new` — and is untouched.
// per §17.7 / stdlib §2.6 / stdlib §10.0
bool isNoConstructorBuiltin(std::string_view n);

// Arm 2 — the builtin RECORDS whose only declared constructor is the positional
// primary one: `ExceptionInfo` (spec §31.4, `record ExceptionInfo(TypeName:
// string, Message: string, StackTrace: string)`) and `ReadOnlyStringSpan`
// (stdlib §3.3, `record ReadOnlyStringSpan(Source: string, Start: int, Length:
// int)`). §17.7 rejects "only the undeclared parameterless form `new T()` …
// exactly as for an identically-shaped user record" — and an identically-shaped
// user record does reject it ("required parameter `A` is not supplied", §22.5).
// per §17.7 / §31.4 / stdlib §3.3
bool isPositionalCtorOnlyBuiltinRecord(std::string_view n);

// fix #3136 (§17.7 "Standard-library builtins have a closed constructor
// surface", v0.241; §31.4; stdlib §3.3): the DECLARED POSITIONAL ARITY of each
// arm-2 record — `ExceptionInfo`'s `(TypeName, Message, StackTrace)` and
// `ReadOnlyStringSpan`'s `(Source, Start, Length)`. Both are 3, but the table
// is per-record (rather than a constant) so a future arm-2 record whose
// declared arity differs (e.g. a two-field or four-field builtin positional
// record) carries its own arity without growing the call site.
//
// §17.7: "the constructor surface is exactly what that document declares —
// `new T(...)` resolves only against a declared constructor, never against a
// compiler-supplied fallback." A two-argument or four-argument list resolves
// against NEITHER (the doc declares exactly three), so sema must reject it the
// same way it rejects the parameterless form (#2994). #3098 grew codegen's
// `lowerNew` arm to construct the 3-arg form, but the arity check has never
// been in sema; the wrong-arity call used to fall through to
// `lowerLocalDecl`'s #2434 zero-init fallback — a record of NULL strings,
// the #2922 class.
//
// Returns the declared arity, or 0 when `n` is not in the arm-2 set (so a
// caller tests membership on `isPositionalCtorOnlyBuiltinRecord` first and
// uses the returned arity as a positive-list check on `ne.args.size()`; 0
// never matches a real argument list, which keeps the call site a single
// `if (auto a = positionalCtorArityOfBuiltinRecord(n); a && ne.args.size()
// != a)` shape).
// per §17.7 / §31.4 / stdlib §3.3
std::size_t positionalCtorArityOfBuiltinRecord(std::string_view n);

// #2921 / #2923 / §8.1 / §8.5 / §28.2 / §28.9: is `n` a reserved built-in
// **class** whose runtime value carries NO Bezel vtable at word 0?
//
// The compiler emits vtables (layout `[0]=td$, [1]=itmap$, [2]=Dispose,
// [3]=ToString, [4+]=virtuals`) for exactly three kinds of object: a user
// `TypeSymbol`'s instances, a box, and a built-in EXCEPTION. Every other
// built-in is a plain runtime C struct — `bz_array_t*`, `bz_mutex_t*`,
// `bz_random_t*`, `bz_event_source_t*`, … — whose word 0 is payload data. Two
// lowerings assume the vtable unconditionally, and both are memory-unsafe on
// such a receiver:
//   * `emitDynamicDispose` loads word 0 and calls slot 2 — the wild jump of
//     #2921 (measured `mov (%rax),%rcx ; call *0x10(%rcx)` → address 0);
//   * `lowerObjectToString` asks `bz_obj_is_string`, which is a NEGATIVE test
//     ("word 0 is not one of the emitted vtables ⇒ it is a string"), so it
//     answers "string" for every one of them; the receiver is then read as a
//     `%bz.string` and, since #2846, RETAINED and later RELEASED — the double
//     free / heap-buffer-overflow of #2923.
// This predicate is the single gate both consult, so the question "does this
// receiver carry a Bezel vtable?" is asked once, from one table, rather than
// re-enumerated per site.
//
// Derived, deliberately, rather than hand-listed: it is
// `isReservedBuiltinTypeName` MINUS `kBuiltinInterfaceHeads`. The subtraction
// is the whole content of the rule — a built-in INTERFACE head is the one
// reserved name whose runtime value may be a *user* class (measured: `var e:
// IEnumerable<int> := new MyList(); e.Dispose();` compiles and runs), so those
// must keep the dynamic dispatch. Deriving in this direction makes the
// omission fail SAFE: a newly-added built-in CLASS joins this set
// automatically and gets the vtable-free treatment, while a forgotten new
// built-in INTERFACE costs a leak or a type-name rendering — never a wild
// jump. // per §8.1 / §8.5 / §28.2 / §28.9
bool isVtableLessBuiltinClassName(std::string_view n);

// fix #2969 (§8.5 / §8.1 / §18.5 / §28.9, v0.240): is `n` one of the reserved
// built-in INTERFACE heads? Exposed as its own predicate so the #2969 fix
// (codegen-side routing of an interface-head-typed interpolation hole through
// the vtable-less built-in renderer when the AST source statically determines
// the value is a built-in collection) does not have to re-list the
// `kBuiltinInterfaceHeads` names. See the header note on
// `isVtableLessBuiltinClassName` for why these are deliberately split off from
// that set — the predicate covers the case where a user class may legitimately
// implement a built-in interface head, which the derived vtable-less
// predicate excludes by construction.
// fix #5044 (§2.3 Row 10, stdlib §4, v0.307): the §2.3 always-available
// bare-name Row 10 collection/iterator heads — `Array`, `Stack`, `Queue`,
// `Set`, `Dictionary`, `LinkedList`, `FrozenArray`, and the `Owning*` forms,
// plus every `I*` interface in stdlib §4 (enumerable, collection, span).
//
// This is a PURPOSE-BUILT subset for §2.3's "alias collision" gate: it
// answers whether `n` is one of the names that blocks `import Foo as Bar`
// where `Bar` collides with an always-available bare name. It is NOT a
// general-purpose collection-name predicate — callers needing broader sets
// use the existing `isArrayBackedCollectionName`, `isDictionaryName`,
// `isLinkedListName`, `isIterableInterfaceHead`, or `isBuiltinForInEnumerableHead`
// predicates, which each answer different questions with intentionally
// different membership.
// per §2.3 / stdlib §4, fix #5044
bool isAlwaysAvailableBareNameCollection(std::string_view n);

// per §8.1 / §8.5 / §18.5 / §28.9
bool isBuiltinInterfaceHead(std::string_view n);

// fix #2992 (§28.3 / §28.9, v0.240): is `head` a reserved built-in type whose
// `using var` carries a REAL §28.10 disposal obligation? This is the SHARED
// authority that previously lived as a private table inside
// `MethodOwnership::rhsIsDisposableClassType`'s Path 2 (the §28.10 vacuity-lint
// gate, Ownership.cpp ~5046). Both the lint and the codegen auto-drop chain's
// end-of-chain guard (registerUsingDisposal, LlvmBackend_Setup.cpp) now consult
// this table, so a built-in in this set that no disposal arm claims becomes a
// compiler-detected hole rather than a silent leak.
//
// The table is lifted VERBATIM from the lint's pre-#2992 Path 2 — it is NOT
// reconciled with codegen's broader chain-claimed set. The two DISAGREE on 13
// names (the borrowing collections, Stopwatch, Random, Atomic/AtomicInteger,
// Buffer): every one is chain-claimed but absent here. This produces a false-
// positive vacuity warning on factory-call forms of those types (measured
// 2026-08-09 on d2a3d3e). Reconciling requires a design decision — #2695
// deliberately excluded the borrowing collections from this set so the lint
// fires on a bound-then-pinned alias whose `const` remedy compiles — filed
// separately. // per §28.3 / §28.9 / §28.10, fix #2992
bool isDisposableBuiltinHeadName(std::string_view head);

// fix #7456 / #7457 (spec §9.7 / §14.2 / §28.12; stdlib §2.10 / §4.2 /
// §4.12 / §4.13, v0.345): THE BUILT-IN DISPOSAL FAMILY — the interfaces that
// extend `IDisposable` directly or transitively.
//
// This is the ONE table every "is this head a disposal interface?" answer is
// derived from. Before it existed, THREE hand-lists answered that question
// and DISAGREED: the §28.10 vacuity lint's table knew six of the family's
// ten heads (#7456), the `where`-bound check knew two (#7457), and the
// declaration-site chain table knew all ten (#7424, whose own header recorded
// the derivation as its residual).
//
// `isBuiltinDisposalFamilyHead` answers the DIRECT case — "is this head one
// of the family's members?". A caller that has a chain to walk (the
// declaration-site check) reads `builtinDisposalFamilyParent` to walk it.
//
// The family, and where each member is declared (stdlib §2.10 / §4.2 / §4.12
// / §4.13):
//
//   IDisposable              (root)                §2.10
//   IAsyncDisposable          extends IDisposable  §2.10
//   IOwningCollection<T>      extends IDisposable  §4.2
//   IOwningList<T>            extends IOwningCollection<T>  §4.2
//   IEnumerator<out T>        extends IDisposable  §4.12
//   IAsyncEnumerator<out T>   extends IAsyncDisposable  §4.12
//   IOwningEnumerable<T>      extends IDisposable  §4.13
//   IOwningEnumerator<T>      extends IDisposable  §4.13
//   IOwningAsyncEnumerable<T> extends IAsyncDisposable  §4.13
//   IOwningAsyncEnumerator<T> extends IAsyncDisposable  §4.13
//
// Spec §14.2's worked example names the same closure ("an interface conforms
// exactly when it extends `IDisposable` (rule 3): `IAsyncDisposable`,
// `IEnumerator<T>`, `IAsyncEnumerator<T> …`") and stdlib §2.10 states it for
// the bound ("the interfaces that extend `IDisposable` directly or
// transitively").
//
// The USER side of the family is not tabulated: a user interface's own
// extension clause is carried on its symbol (`TypeSymbol::baseTypes` for a
// user base, `TypeSymbol::nonSymbolBaseRefs` for a built-in base), and
// `declaresBuiltinInterface` is the ONE place the question "does this type
// declare built-in interface `name`?" is answered (Symbol.hpp).
// per §9.7 / §14.2 / §28.12, stdlib §2.10 / §4.2 / §4.12 / §4.13, fix #7456,
// fix #7457
struct BuiltinDisposalFamilyHead {
    std::string_view head;    // the interface's own (unqualified) name
    std::string_view parent;  // the interface it extends; empty for the root
};

bool isBuiltinDisposalFamilyHead(std::string_view head);

// The interface the built-in disposal head `head` extends — empty for
// `IDisposable` itself, and empty for a head outside the family.
// per §9.7 / §14.2 / §28.12, fix #7456, fix #7457
std::string_view builtinDisposalFamilyParent(std::string_view head);

// Does the built-in disposal-family head `head` reach `IAsyncDisposable` along
// its chain (`IAsyncDisposable` itself, `IAsyncEnumerator<T>`, the
// `IOwningAsync*` interfaces)? DERIVED from the chain table above, so a
// future stdlib descendant of `IAsyncDisposable` follows automatically once
// the table matches the stdlib (scripts/check_disposal_family_chain.py). A
// head outside the family is not async. per §28.12, stdlib §2.10 / §4.12 /
// §4.13, fix #7497
bool builtinDisposalFamilyIsAsync(std::string_view head);

// fix #3017 (§28.3 / §28.9 / §28.10, v0.240): the 13 built-in class heads
// the codegen auto-drop chain (registerUsingDisposal, LlvmBackend_Setup.cpp)
// claims disposal for but that are DELIBERATELY ABSENT from
// `isDisposableBuiltinHeadName` — the §28.10 vacuity-lint table — because
// #2695's coherence invariant (the lint must fire on a bound-then-pinned
// alias whose `const` remedy compiles) requires the borrowing collections to
// stay out. For factory-call forms (`using var x := C.Make()`) the result IS
// a fresh owned value, so the lint must NOT fire — the chain DOES reclaim it.
// The alias form (`using var p := src;` over a local) keeps the current
// behaviour because rhsIsDisposableClassType still returns false on these
// heads and the IdentifierExpr RHS bypasses every CallExpr disjunct.
//
// The 13 names: 8 borrowing collections (§4 — §28.11 trivial-dispose; the
// chain's collection-family arms reclaim the backing) and 5 vtable-less
// builtins (Random/Atomic/AtomicInteger from fix #2921/#2973, Stopwatch from
// §8.1, Buffer from #545). See Ownership.cpp's `constInitializerRejected`
// for the disjunct this predicate closes the gap on.
bool isChainClaimedBuiltinHeadName(std::string_view head);

// §31.4 / #1621: given a built-in exception type's simple name, return the
// name of its DIRECT parent in the built-in exception hierarchy (e.g.
// "FileNotFoundException" -> "IOException"), or an empty string_view if
// `name` is not a recognized built-in exception name or is the hierarchy's
// root (`Exception`, which has no parent).
//
// This is sema's ONLY access to the built-in exception hierarchy — used by
// `ExprTC`'s multi-type catch-arm nearest-common-base walk (§31.2.1,
// src/sema/ExprTypecheck.cpp) to continue an ancestor walk past a built-in
// Named TypeRef (which carries no TypeSymbol / AST base-type list of its
// own), so a multi-type catch-arm binding whose listed types are two SIBLING
// built-ins (e.g. `OverflowException | ArgumentException`) still finds their
// shared ancestor `Exception` instead of spuriously reporting "no common
// base" or silently mistyping the binding.
//
// Codegen keeps an INDEPENDENT copy of the same edges —
// `Emitter::builtinExceptionHierarchy()` (src/codegen/LlvmBackend_Setup.cpp)
// — used to build the built-in exception descriptor/vtable globals that back
// runtime `isA` dispatch. The two tables are not merged into one because
// codegen's is `const char*`-keyed and threaded through several
// std::string-keyed maps at its call sites; merging would need a wider,
// separately-verified refactor than this fix's scope. They are hand-kept in
// sync — a table edit at either site must mirror the other (each definition
// cross-references the other's file/function).
std::string_view builtinExceptionParentOf(std::string_view name);

// #1936 / §19.5 / §31: true when `name` is the simple name of a recognized
// built-in exception type (including the hierarchy root `Exception`). Backed by
// the same `kBuiltinExceptionHierarchy` table as `builtinExceptionParentOf`, so
// it is sema's single authoritative "is this a built-in exception?" predicate —
// used to reject a value-typed scrutinee against a reference-typed (built-in
// exception) `var x: T` binding, where the built-in carries no user TypeSymbol
// and so is not caught by a `declaration`-gated class/interface test (§19.5).
bool isBuiltinExceptionName(std::string_view name);

// fix #3119 (spec §8.5, v0.252; stdlib §2.13 rule 1): the QUALIFIED module
// path a reserved built-in CLASS renders under in §8.5's name-only default and
// stdlib §2.13's `Type.FullName` rule 1. Returns the stdlib module path
// (e.g. "System.Threading" for `Mutex`, "System.Collections.Generic" for
// `Array`), or an empty string_view when `name` is not a reserved built-in
// class. Keyword-aliased primitives and built-in interface heads return empty
// here; their renderings are governed by rule 2 (keyword lowercase) and
// rule 1 (interfaces do not render their own name) respectively.
//
// Read by the eight §8.5 emission sites through the `spec85QualifiedTypeName`
// / `spec85TypeRefQualifiedName` helpers — adding a reserved built-in class
// means editing the table in BuiltinRegistry.cpp, NOT every site. // per
// §8.5 / stdlib §2.13 rule 1, fix #3119
std::string_view builtinClassModulePathOf(std::string_view name);

// fix #4698 (spec §2.2, v0.313 "an alias is an additional qualifier"): the
// module a compiler-built-in type or static class is DECLARED in, for the
// alias-qualified spelling `Alias.Name` (type and expression position). This
// is `builtinClassModulePathOf` plus the built-in static classes that have no
// §8.5 row (`Debug`, stdlib §8.2). Empty when `name` is not a built-in whose
// home module is known. Built-ins have no exported TypeSymbol in their module
// (`importedTypesOf` never lists them), so sema consults this table to decide
// that `Alias.Name` names the same entity as the bare `Name`.
std::string_view builtinTypeHomeModuleOf(std::string_view name);

// §2.3 (v0.338) auto-imported module rows: `System`, `System.Text`,
// `System.Collections.Generic`, `System.Collections.Linq`. The single list of
// those four module paths; sema's qualified-name resolution and the module
// loader's implementation-provided set both read it, so the rows are spelled
// once. `System.Threading` and the other explicit-import modules are NOT here.
bool isAutoImportedModulePath(std::string_view modulePath);

// fix #3119 (spec §3.4; stdlib §2.13 rule 2, v0.252): true when `name` is one
// of the 14 keyword-aliased built-in types — `string` plus the 13 primitive
// value-type keywords (`bool`, `byte`, `sbyte`, `short`, `ushort`, `int`,
// `uint`, `long`, `ulong`, `float`, `double`, `decimal`, `char`). Per stdlib
// §2.13 rule 2, these render as their lowercase keyword in BOTH `Name` and
// `FullName`; the keyword is the canonical name, a recorded exception to
// qualification. Membership is a NEGATIVE test (a name not here is not a
// keyword alias), so a missed entry silently renders qualified — caught by a
// regression test rather than a runtime symptom. // per §3.4 / stdlib §2.13
// rule 2, fix #3119
bool isKeywordAliasBuiltinName(std::string_view n);

// fix #3119 (stdlib §2.13 rule 2 / rule 3, v0.252): the SOURCE SPELLING of a
// `PrimitiveType` — the lowercase keyword per §3.4 (`bool`, `int`, `string`,
// …), matching what `primitiveSpelling` in sema emits for diagnostics and
// matching stdlib §2.13 rule 2's `Name` rendering for primitives. Returns
// `<unknown>` for an unrecognised `PrimitiveType` so a regression surfaces
// as a visibly-bogus name rather than silent empty rendering. `Object` is the
// only non-lowercase spelling: it has no lowercase keyword (§3.4) and is the
// root class for builtin-class purposes; render it as `Object` for clarity
// when it appears inside a generic instantiation's args (a `Object`-typed
// argument never appears in a `Dictionary<…, Object>` per the spec, but the
// rendering is still well-defined).
std::string_view primitiveBuiltinSpelling(int primitive);


// fix #3398 (spec §22.5 "Optional parameters and named arguments", v0.283;
// stdlib §1 Conventions — *Window parameters*; §9.6 `String`; §3.2
// `Encoding`), widened by fix #3424 and joined by the constructor twin
// fix #3425 (§17.7 "The `new` operator"): the declared PARAMETER NAMES of a
// built-in member overload, and of a built-in constructor overload.
//
// Why these tables exist at all: the compiler carries no parameter names for
// any built-in (stdlib, non-user-declared) member or constructor, so §22.5's
// binder had nothing to match a named argument against at a built-in call
// site. Every name was therefore DISCARDED and every argument bound
// POSITIONALLY — `"hello world".Remove(length := 6, startIndex := 5)`
// compiled clean and evaluated `Remove(6, 5)` (#3398), and
// `new Array<int>(fill := 7, size := 3)` compiled clean and allocated SEVEN
// slots of `3` (#3425, measured on ef339fa: `Count`=7, `[0]`=3, where the
// positional `new Array<int>(3, 7)` gives 3 and 7). Both are silent wrong
// answers, not merely missing rejections. The names in these tables are the
// ones the standard-library document declares, and stdlib §1 makes them API
// surface ("parameter names are API surface (named arguments, spec §22.5)").
//
// KEYED BY ARGUMENT COUNT, and OVERLOAD-AWARE (fix #3424). A built-in has no
// `MethodSymbol` / `ConstructorSymbol` and therefore no overload set to
// resolve against, and the stdlib's built-in overloads are distinguished at a
// call site chiefly by argument count (`Remove(startIndex)` /
// `Remove(startIndex, length)`). Where two overloads DO share an argument
// count and disagree on names — `string.Replace/2` (`oldChar, newChar` vs
// `oldValue, newValue`), `StringBuilder/1` (`capacity` vs `initial`),
// `Dictionary/1` (`capacity` vs `comparer`) — the lookup returns BOTH
// candidate lists and the caller accepts a call that binds against ANY ONE of
// them. #3398 shipped a single-list lookup and had to OMIT those overloads
// entirely (a rejection where the names were perfectly well declared); this
// is the fix that #3424 asked for. Keying off the overload the built-in TYPE
// dispatch picks is not available: that dispatch lives in codegen's
// `lowerCallDispatch` / `lowerNew` arm chain, which sema cannot consult.
//
// A row carries `required` — the count of LEADING parameters that declare no
// default — so a §22.5 optional parameter (stdlib `TimeOnly(hour, minute,
// second := 0, millisecond := 0)`) needs ONE row rather than one per
// admissible arity, and `FormalsView::ofNames` reports `hasDefault` from it.
//
// DELIBERATELY PARTIAL, and the partiality is safe because the CALLER treats
// a miss as a REJECTION, never as permission: `ExprTC::checkBuiltinNamedArgs`
// and `ExprTC::checkBuiltinCtorNamedArgs` report a named argument on a
// built-in the table does not cover rather than binding it positionally. So a
// missing row costs a loud not-supported diagnostic, never a wrong value —
// which is also why a widening is purely additive and needs no call-site
// change (except adding a head to `isBuiltinNamedArgTableHead` when a MEMBER
// receiver arrives in the static or intrinsic-TypeSymbol shape).
struct BuiltinFormals {
    // The declared parameter names, in declaration order. `size()` is the
    // overload's full parameter count, not the call's argument count.
    std::vector<std::string_view> names;
    // The leading parameters that carry no default; `names.size()` when every
    // parameter is required.
    std::size_t required = 0;
};

// The DECLARED TYPE of a built-in parameter, as a bit set: ONE position can
// admit several declared types (§22.6's overload set — `int.Parse` declares
// both `Parse(s: string)` and `Parse(s: ReadOnlyStringSpan)`, and
// `int.ToString` declares both `ToString(format: string)` and
// `ToString(radix: int)`).
//
// `kBPTAny` is the ZERO value and means "this position has no type opinion":
// the admission gate must not refuse on it. A row that has not been
// transcribed yet carries it, and a MISS therefore costs a rejection that does
// not fire, never a false rejection — the fail-safe direction every table in
// this registry takes.
enum BuiltinParamType : std::uint32_t {
    kBPTAny    = 0u,
    kBPTString = 1u << 0,   // `string`
    kBPTSpan   = 1u << 1,   // `ReadOnlyStringSpan`
    kBPTInt    = 1u << 2,
    kBPTLong   = 1u << 3,
    kBPTUInt   = 1u << 4,
    kBPTULong  = 1u << 5,
    kBPTByte   = 1u << 6,
    kBPTSByte  = 1u << 7,
    kBPTShort  = 1u << 8,
    kBPTUShort = 1u << 9,
    kBPTFloat  = 1u << 10,
    kBPTDouble = 1u << 11,
    kBPTDecimal = 1u << 12,
    kBPTBool   = 1u << 13,
    kBPTChar   = 1u << 14,
    kBPTObject = 1u << 15,  // `Object` (every type reaches it by boxing)
    kBPTOut    = 1u << 16,  // an `out` slot: positional-only, no type opinion
    // The NAMED types stdlib §9.6 declares at a parameter position. The
    // vocabulary above names the SCALAR and reference kinds only; §9.6's
    // `Trim(chars: IEnumerable<char>)`, `Split(separators: Array<char>)`,
    // `Join(separator: string, items: IEnumerable<string>)` and
    // `AsSpan(r: Range<int>)` declare a NAMED type at those positions, and a
    // §5.1 admission test needs to know WHICH one.
    //
    // fix #7421 (stdlib §9.6, §5.1, v0.345): the §9.6 head's declared
    // parameter types are read out of the ONE table (the `kinds` column), so
    // a position whose declared type is a named type needs a bit for it.
    kBPTEnumerableChar   = 1u << 17,  // `IEnumerable<char>`
    kBPTArrayChar        = 1u << 18,  // `Array<char>`
    kBPTEnumerableString = 1u << 19,  // `IEnumerable<string>`
    kBPTRangeInt         = 1u << 20,  // `Range<int>`
};

// ONE declared form of a built-in member or constructor, read out of the ONE
// parameter table (see `builtinParamForms`).
struct BuiltinParamForm {
    // The form's parameter count.
    std::size_t total = 0;
    // The declared parameter names, in declaration order. An empty vector
    // means the table carries no §22.5 name surface for this form.
    std::vector<std::string_view> names;
    // The leading parameters that carry no default.
    std::size_t required = 0;
    // The declared parameter TYPE at each position: a `BuiltinParamType` bit
    // set, or `kBPTAny` where the table has no type opinion.
    std::vector<std::uint32_t> kinds;
    // The §7.3 NON-NULLABLE positions of this form: bit `p` set means the
    // parameter at position `p` is declared non-nullable in EVERY overload of
    // this argument count.
    std::uint8_t nonNullable = 0;
    // The DECLARED SIGNATURE of this form, exactly as the standard library
    // writes it ("`Parse(s: string, radix: int): int`"), with the return
    // type the declaration carries. Empty for a form the standard library
    // does not spell out.
    std::string_view sig;
};

// The declared forms of the built-in member `typeHead.member` (or of the
// built-in CONSTRUCTOR `typeHead`, when `member` is empty) that admit
// `argCount` arguments, in the table's declaration order.
//
// This is the ONE read of the built-in parameter table: the parameter NAME
// (§22.5), the parameter TYPE (§5.1) and the parameter NULLABILITY (§7.3) of
// a form come from ONE row, so a caller that needs two of them cannot read
// two tables that disagree. `builtinParamNameCandidates` and
// `builtinParamIsNonNullable` are the two older spellings of a query over
// the same table.
//
// The head `"String"` (the static call spelling) normalizes to `"string"`,
// and the primitive keyword spellings (`int`, `bool`, …) normalize to the
// standard-library head §9.1-§9.5 declares.
// per §22.5 / §22.6 / §5.1 / §7.3 / stdlib §2.3, §2.5, §2.6, §3.1, §3.2,
// §7.3, §7.4, §9.1-§9.6
std::vector<BuiltinParamForm> builtinParamForms(std::string_view typeHead,
                                                 std::string_view member,
                                                 std::size_t argCount);

// The `argCount` that asks `builtinParamForms` for EVERY declared form of a
// member, whatever its arity — the §22.6 arity question's own ("does the
// standard library declare this member at all?").
inline constexpr std::size_t kBuiltinAnyArity = static_cast<std::size_t>(-1);

// The head the built-in parameter table is keyed by: the primitive keyword
// spellings (`int`, `bool`, …) name the same standard-library types
// §9.1-§9.5 declares, and the static call spelling of the string class is
// `String` where the table says `string`.
// per §9.1 / stdlib §9.1-§9.6, fix #7296
std::string_view builtinParamHeadSpelling(std::string_view typeHead);

// The MEMBER table (#3398, widened by #3424). Returns every candidate whose
// declared shape admits `argCount` arguments (`required <= argCount <=
// names.size()`), in declaration order, or an EMPTY vector when the registry
// carries no names for that (head, member) at that argument count. The head
// `"String"` (the static call spelling) normalizes to `"string"`.
// per §22.5 / stdlib §1 / §9.6 / §3.1 / §3.2, fix #3398 + #3424
std::vector<BuiltinFormals> builtinParamNameCandidates(std::string_view typeHead,
                                                       std::string_view member,
                                                       std::size_t argCount);

// The CONSTRUCTOR table (fix #3425 — §22.5 / §17.7). Same contract as the
// member lookup, keyed by (type head, argument count); a built-in
// constructor's declared surface is §17.7's closed one, so the rows are
// transcribed from the same standard-library declarations
// `isPositionalCtorOnlyBuiltinRecord` and friends already answer for.
// per §22.5 / §17.7 / stdlib §2.5 / §3.1 / §4 / §6 / §7.2, fix #3425
std::vector<BuiltinFormals> builtinCtorParamNameCandidates(
    std::string_view typeHead, std::size_t argCount);

// fix #6782 (spec §7 "parameter pass" and §7.3 "Assigning `null` to a
// non-nullable target", v0.340): is parameter `position` of the built-in member
// `typeHead.member` (or of the built-in constructor `typeHead`, when `member` is
// empty), at `argCount` arguments, declared NON-nullable in every overload of
// that arity? The answer is about nullability only, never about the
// parameter's type: overloads of different types can share a position
// (`StringBuilder(initial: string)` / `(capacity: int)`), and sema reports
// only an argument that is still nullable after §7.4 narrowing there, the
// `null` literal included, leaving type mismatches to overload resolution. The
// rows are the standard-library declarations of §2.3, §2.5, §2.6, §3.1, §7.3,
// §7.4, §9.1–§9.5 and §31.4, transcribed in BuiltinRegistry.cpp. A missed row
// costs a silently accepted null. The head is the stdlib or keyword spelling
// (`int`, `bool`, `Int32`, `Boolean`); the lookup normalizes it.
bool builtinParamIsNonNullable(std::string_view typeHead,
                               std::string_view member, std::size_t argCount,
                               std::size_t position);

// fix #3425: does codegen's `lowerNew` arm for `head` lower its arguments
// through the §22.5 BINDING (by parameter name) rather than by written
// position?
//
// True for exactly the two record-shaped reserved builtins whose `lowerNew`
// arm #3098 gave a by-name pass (`ExceptionInfo`, §31.4;
// `ReadOnlyStringSpan`, stdlib §3.3 — LlvmBackend_Exprs2.cpp): for those a
// REORDERED named argument list is honoured and must stay admitted. Every
// other built-in constructor is lowered by `ne.args[i]` position, so a
// binding that is not the identity permutation would be honoured by sema and
// ignored by codegen — the wrong answer this fix exists to remove — and is
// REJECTED until codegen can consume the binding (#3423, the member twin's
// same limitation). // per §22.5 / §17.7, fix #3425
bool isBuiltinCtorLoweredByName(std::string_view head);

// fix #7502 (spec §22.5 "Every parameter that declares no default must be
// bound" and §17.7 "closed constructor surface", v0.350; stdlib §2.5): the
// built-in heads whose COMPLETE constructor surface the parameter table
// carries, so an argument list that no row admits is a construction the
// standard library does not declare (`new DateOnly(2024)`, `new DateTime(2024,
// 3, 4, 5)`). Partial on purpose: a head is listed only once its rows are the
// whole of what the stdlib declares, because a MISS here costs a missed
// rejection, never a false one.
bool isBuiltinCtorSurfaceClosedByRows(std::string_view head);

// fix #3398: is `head` one of the built-in type heads
// `builtinParamNameCandidates` carries MEMBER rows for?
//
// Two call-site roles, both in `ExprTC::checkBuiltinNamedArgs`:
//   * it admits the STATIC call spelling (`String.From(...)`), whose target is
//     a bare identifier with no value type — there is nothing else at that
//     shape to tell a built-in static class apart from a user one, and a user
//     static call is already bound by `checkCallArity`, so the check claims
//     only the heads named here;
//   * it admits an INSTANCE receiver whose head is not in
//     `isReservedBuiltinTypeName` (`Encoding` is not a `lowerTypeRef` Named
//     case and so is absent from that set).
// per §22.5, fix #3398
bool isBuiltinNamedArgTableHead(std::string_view head);

// fix #3589 (stdlib §5 / §9.6, v0.286): is `n` one of the member names the
// STRING-receiver gate exempts from its "not yet supported in codegen"
// catch-all, so `typeCall()` can resolve the call via the `IEnumerable<char>`
// LINQ extension path (fix #1220, §9.6 — string is IEnumerable<char>)?
//
// This answers a DIFFERENT question from `isBuiltinCollectionLinqOperatorName`
// below, not merely a copy of it: this table folds in `Reverse` (string
// declares no native in-place `Reverse`, unlike `Array<T>`/§4.4, so the lazy
// §5 operator is the only `Reverse` a string receiver can mean) and `Count`
// (string has no dedicated `.Count` property handler pre-empting this gate
// the way Array/Dictionary/… do, per ExprTypecheck_Calls.cpp's #3589 site
// comments) — both DELIBERATELY excluded from the collection-family table
// because a dedicated mechanism intercepts them there first. It also carries
// `ForEach`, which stdlib §5 does not declare (`docs/Bezel_Standard_Library.md`
// §5's `IEnumerable<T>` extension has no `ForEach` row) but which has a real,
// separately-gated LOWERING (`LlvmBackend_Calls.cpp`'s `isArrTerminal` /
// `LlvmBackend_Calls3.cpp`'s "per §5 LINQ: .ForEach" arm) that this fix does
// not touch — removing the name here would only change which diagnostic a
// currently-broken call gets, not fix it; the lowering defect (an LLVM IR
// verifier failure on the void-returning closure call, reproduced on both a
// `string` and an `Array<int>` receiver) is filed separately (issue 3636).
//
// Collapses two byte-identical 38-name hand-spellings (the `.` member-access
// gate and its `?.` mirror) that a #3589 measurement found duplicated the
// entire list verbatim. per §5 / §9.6, fix #3589
bool isStringLinqOperatorMemberName(std::string_view n);

// fix #3589 (stdlib §5, v0.286): is `n` one of stdlib §5's LINQ extension
// operator names, admitted so a BARE reference (`var f := a.Where;`, no call)
// or a member-access on a builtin non-owning-collection-family receiver
// (`Array`/`OwningArray`, `Stack`/`OwningStack`/`Queue`/`OwningQueue`,
// `Dictionary`/`OwningDictionary`/`SortedDictionary`,
// `Set`/`OwningSet`/`SortedSet`/`LinkedList`/`OwningLinkedList`) is recognised
// as a REAL member (issue #2470/#2492: "not yet supported" rather than a false
// "no member") rather than routed to the collection-specific "no member"
// reject. Each receiver family co-declares its OWN `isDeclaredMethod` set
// alongside this table for its native (§4.x) surface — deliberately NOT
// folded in here, because that surface differs per family while the §5
// question does not.
//
// Carries `OfType` even though stdlib §5 no longer declares it (removed
// v0.284, fix #3559): fix #3559's own commit records that the four call-site
// membership lists (this table's four pre-unification copies) are
// "deliberately LEFT" because they decide WHICH diagnostic a bare `OfType`
// reference gets — the specific v0.284-removal wording, not a generic
// "no member" message that would send a reader hunting for a typo that is
// not there. Excludes `Reverse`, `Count`, and `Contains` on purpose: each of
// these families pre-empts them through its own dedicated handler before this
// gate is consulted (a bare `a.Reverse` on `Array<T>` hits the native
// imperative `Reverse()`, `.Count` hits a dedicated property handler, and
// `Contains` is every family's own native §4.x method) — admitting them here
// too would be harmless-redundant at best and is left out to keep this table
// answering exactly one question.
//
// Collapses four hand-spellings this fix's #3589 measurement found were
// meant to be identical (three were already byte-identical 38-name copies;
// the fourth — the `Array`/`OwningArray` site — was missing `ToArray` only
// because that family's own `isDeclaredMethod` already admits it separately,
// masking the gap; unifying on the union changes no observable accept/reject
// decision anywhere, confirmed by `scripts/behaviour_differential.sh`).
// per §5, fix #3589
bool isBuiltinCollectionLinqOperatorName(std::string_view n);

// fix #3670 (stdlib §5, v0.286): does `m` take a single lambda argument
// `(T) -> …` seeded with the LINQ receiver's own source element type T, so an
// un-annotated lambda parameter (`(p) => p.x`) can be typed as T (§27.4's
// parameter-type-inference rule; see `ExprTC::linqLambdaElementType`,
// the sole caller)?
//
// This is a THIRD vocabulary, not a rename of `isBuiltinCollectionLinqOperatorName`
// or `isStringLinqOperatorMemberName` above: those answer "is `n` a real §5
// name at all", a name-EXISTENCE question; this answers a lambda-SHAPE
// question, and the two sets deliberately disagree in both directions.
// Concrete examples, not an exhaustive account (see stdlib §5's grammar for
// each operator's exact signature):
//   * `Distinct`/`Take`/`Skip`/`OfType`/`Reversed`/`Sorted`/`ToArray`/`ToSet`
//     are real names in the other two tables that take NO lambda at all, so
//     they are correctly absent here.
//   * `ToDictionary`'s key-selector overload DOES take a `(T) -> TKey`
//     lambda and so IS included here, unlike the no-lambda operators above.
//   * `Count` is included here (its predicate overload
//     `Count(predicate: (T) -> bool)`, stdlib §5) though it is absent from
//     `isBuiltinCollectionLinqOperatorName` above — a dedicated property
//     handler pre-empts the bare `.Count` form on a builtin-collection
//     receiver before that gate runs, see that table's header — while it IS
//     present in `isStringLinqOperatorMemberName` (no such property handler
//     exists for a `string` receiver, per that table's header).
//   * `GroupBy(key: (T) -> TKey)` also takes a source-element-typed lambda
//     but is NOT in this table — an asymmetry inherited unchanged from the
//     pre-#3670 `elemLambda` list this table replaces verbatim; whether that
//     is a genuine pre-existing gap is a separate question this pure-routing
//     extraction does not resolve (see fix #3670's commit body).
// Folding this table into either existing one would silently make every name
// in the union answer BOTH questions, which is false for most of them —
// declined per fix #3589's own precedent (its commit left this exact list,
// then still inline at `linqLambdaElementType`'s `elemLambda`, untouched for
// the identical reason).
//
// Was a fifth hand-spelled ~20-name chain surviving #3589 (issue #3670);
// moved here unchanged (no name added or removed — this is a pure
// extraction, not a widening). per §5, fix #3670
bool isLinqElementLambdaOperatorName(std::string_view m);

// fix #3670 (stdlib §5, v0.286): is `m` one of the LINQ operators that, given
// a call already past the `isLinqHashableOperator` gate, produce a lazy
// `IEnumerable<T>` over the UNCHANGED source element type T — i.e. every
// deferred operator whose typing arm is exactly
// `ctx_.setExprType(&ce, makeLazyEnumerable(srcElem)); return true;` with no
// further per-operator computation (see `ExprTC::typeCall`'s
// `typeLinqOperator`, the sole caller)?
//
// A fourth vocabulary: not name existence, not lambda-parameter shape, but
// "does typing this call require nothing beyond wrapping the source element
// type unchanged". `Select`/`SelectMany`/`Zip`/`ToDictionary` project to a
// DIFFERENT element type (the lambda's return type) and so are excluded;
// `ThenBy`/`ThenByDescending` share this table's exact body
// (`IOrderedEnumerable<T>` reuses the same lazy sentinel) but are answered by
// a separate, adjacent `if` in the caller rather than folded in here, since
// they operate on the `IOrderedEnumerable<T>` receiver rather than a plain
// LINQ source — kept as their own arm so a reader does not have to cross-
// reference this table's docstring to see the receiver differs.
// `*OrNull`/`OfType`/`GroupBy`/aggregate forms compute a genuinely different
// result type and are handled by their own arms, later in the same function.
//
// EXCLUDES `Take`: stdlib §5 declares `Take(count: int): IEnumerable<T>`
// unconditionally, but fix #1476 (§4.10) carved out `OwningSet<T>.Take(query:
// T)` — a DIFFERENT, non-LINQ overload name-colliding on the OwningSet
// receiver — so the caller keeps `Take`'s admission as an explicit
// `m == "Take" && colName != "OwningSet"` conjunct beside this table rather
// than baking a receiver-dependent exception into a plain name predicate.
//
// Collapses four hand-spelled, byte-identical-bodied `if` arms (11 names:
// `Where`/`Skip`/`Distinct`, `OrderBy`/`OrderByDescending`,
// `Concat`/`TakeWhile`/`SkipWhile`, `Union`/`Intersect`/`Except`) that #3670
// found surviving #3589 as a still-inlined, still-duplicated-in-SHAPE (not
// name) copy of the same routing decision. per §5, fix #3670
bool isLinqLazyPassthroughOperatorName(std::string_view m);

// fix #3763 (stdlib §5 / §5's `Reverse` naming note v0.195 / §4.10 / §4.18 /
// §4.6 / §4.7, v0.287): the builtin collection HEADS on which stdlib §5's LAZY
// `Reverse(): IEnumerable<T>` is the ONLY declared meaning of a direct
// `recv.Reverse()`, so both sema (the typing arm) and codegen (the lazy
// operator gate) must admit them.
//
// The membership question is the §5 naming note's own, verbatim: "the lazy
// operator applies only where the receiver's static type has no instance
// `Reverse`" (stdlib §5, v0.195), read together with spec §26.5 rule 1
// ("instance members win", BY NAME). Only four types in this document declare
// an instance `Reverse()`: `Array<T>` (§4.4), `OwningArray<T>` (§4.5),
// `Span<T>`/`ReadOnlySpan<T>` and `ArraySpan<T>` (§4.15) — on each of those
// §26.5 rule 1 makes the in-place member win and the §5 operator unreachable.
// The Set family (`Set`/`OwningSet` §4.10, `SortedSet` §4.18) and the linked-
// list family (`LinkedList`/`OwningLinkedList` §4.6/§4.7) declare none, so §5
// is all a call on them can mean.
//
// fix #3825 (stdlib §5 / §4.11, v0.307): Stack/OwningStack/Queue/OwningQueue
// also declare no instance `Reverse()` per stdlib §4.11, and the §5 naming
// note therefore says the lazy operator applies — admitted here in #3825,
// replacing the earlier exclusion.
//
// ⚠ EXCLUDES `OwningBuffer` (§4.14) and `FrozenArray` (§4.19): both are
// `bz_array_t*`-backed and reach the same in-place mutator
// (`OwningBuffer<int>.Reverse()` measured mutating in place), and their lazy
// Reverse codegen has a separate gap (tracked separately). Also excludes
// `Dictionary`/`SortedDictionary` (§4.8/§4.17), whose §5 element type is a
// `DictionaryEntry<K, V>` STRUCT rather than a plain element — a different
// lazy-element lowering shape with no measured coverage here.
// per §5 / §4.6 / §4.7 / §4.10 / §4.18, fix #3763; §4.11 admission fix #3825
bool isLazyReverseDirectReceiverHead(std::string_view head);

// fix #4766 / #4833 (§5.1, v0.307): the builtin heap-type HEADS that declare a
// `readonly property Length : int` per the stdlib, used by
// `ExprTC::resolveMemberAccess`'s narrow #4766 arm to type the otherwise-untyped
// `.Length` read on a declared-null builtin collection. The set covers:
//   - §4.4/§4.5/§4.11: Array/OwningArray/Stack/OwningStack/Queue/OwningQueue
//     — the structural `Count` alias that codegen lowers through the same
//     runtime length call (`isLengthAlias`, the #2459 exemption in
//     LlvmBackend_Layout). ⚠ stdlib §4.4 (and its Count-vs-Length note) gives
//     these types `Count` ONLY — whether the alias should exist at all is
//     tracked by #4811. Typed here only so §5.1 is not skipped while it does.
//   - §3.1 StringBuilder.
//   - §4.14 OwningBuffer<T>.
//   - §4.19 FrozenArray<T> (the annotation shape only; the `From(...)` result
//     carries a builtinClassSym declaration and takes the declared-type
//     lookup path instead, never reaching this predicate).
//
// ⚠ This is NOT `isArrayBackedCollectionName` / `isDictionaryName` — the
// PROPERTY-TYPE axis of the #4766 arm is fundamentally distinct from the
// ARRAY-BACKED-COLLECTION axis (`isArrayBackedCollectionName`) and the
// DICTIONARY axis (`isDictionaryName`). `Set`/`OwningSet`/`LinkedList`/
// `OwningLinkedList` are `isArrayBackedCollectionName`-true (bz_array_t* rep)
// but declare no `Length` property — they declare `Count`, handled by the
// `Count` arm at ExprTypecheck_Calls.cpp:~20867 — so they are deliberately
// OUT here. `StringBuilder`/`FrozenArray`/`SortedDictionary`/`SortedSet`/
// `Channel` are NOT array-backed and not dictionary-backed but DO declare an
// int `Length` or `Count` property per the stdlib — so they belong here, not
// there. Two different questions over overlapping name sets is exactly why
// both predicates exist and are each named for their own question.
// per §5.1 / §3.1 / §4.4 / §4.5 / §4.11 / §4.14 / §4.19, fix #4766 + #4833
bool isBuiltinIntLengthPropertyHead(std::string_view head);

// Same question for `.Count` reads that the upper arm at
// ExprTypecheck_Calls.cpp:~20867 does NOT cover. The upper arm answers
// `.Count` for Array/OwningArray/Stack/OwningStack/Queue/OwningQueue/Set/
// OwningSet/LinkedList/OwningLinkedList/Dictionary/OwningDictionary/
// OwningBuffer (the bz_array_t*-backed collection set plus the bz_dict_t*-
// backed collection set). This predicate covers the heads whose `Count`
// property the upper arm does not type:
//   - §4.17 SortedDictionary<K, V>.
//   - §4.18 SortedSet<T>.
//   - §6.9 Channel<T>.
// All three declare `readonly property Count : int`; the upper arm missed
// them because SortedDictionary alone (of the three) is
// `isDictionaryName`-true — and the upper arm treats `.Count` on
// `SortedDictionary` as the bz_array_t*-side path, not the stdlib §4.17
// property. The narrow #4766 arm is what gives the .Count read a typed
// result on these three heads.
// per §5.1 / §4.17 / §4.18 / §6.9, fix #4766 + #4833
bool isBuiltinIntCountPropertyHead(std::string_view head);

}  // namespace bezel::common
