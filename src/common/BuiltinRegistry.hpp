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

#include <string_view>

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

// Array-backed collections (bz_array_t*): the receiver names whose indexer /
// .Count lower through the bz_array_* runtime.  Backs isCollectionTypeName.
// per §4.4–§4.7 / §4.14
bool isArrayBackedCollectionName(std::string_view n);

// bz_dict_t*-backed collections (K-indexed get/set).  Backs isDictTypeName.
// per §4.8 / §4.17
bool isDictionaryName(std::string_view n);

// bz_llist_t*-backed collections.  Backs isLinkedListTypeName. // per §4.6/§4.7
bool isLinkedListName(std::string_view n);

// #2809 / spec §32.5 / stdlib §10.1–§10.3: the three FFI-boundary BUFFER
// classes — `CString`, `WString`, `RawBytes`. Each is a raw malloc'd buffer
// (or, for RawBytes, a { ptr, len } struct over one), NOT a `%bz.string` and
// NOT an object with a header, so any handler that reaches into one at a
// `%bz.string` field offset reads out of bounds.
//
// This is the set the member-access handlers in `LlvmBackend_Exprs.cpp` must
// recognise BEFORE the declaration-less `.Length` string fallback claims the
// receiver: `isStringFallbackTarget` is a negative list ("not StringBuilder,
// not Buffer, not FileStream, not a collection → assume `%bz.string`"), and
// these three were its fifth missing category (#2809: a `c: CString` parameter
// read `c.Length` as a `%bz.string` length field → heap-buffer-overflow).
// The FFI value TYPES `IntPtr`/`UIntPtr` are deliberately NOT here: they are
// pointer-sized integers, not buffers, and no member handler dereferences one.
// per §32.5 / stdlib §10.1–§10.3
bool isFfiBufferBuiltinName(std::string_view n);

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

}  // namespace bezel::common
