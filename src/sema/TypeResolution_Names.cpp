#include "sema/TypeResolution_impl.hpp"

#include <cstdio>   // send-back #4305: unhandledNullDeclClassConstraintSlot
#include <cstdlib>  // send-back #4305: unhandledNullDeclClassConstraintSlot

#include "common/BuiltinRegistry.hpp"  // isBuiltinExceptionName — §31.4 (#2950)
#include "common/OwnershipFacts.hpp"   // isMoveOnlyValueType — §14.4 / §15.3 (#3794)
#include "sema/ConstFold.hpp"
#include "sema/ExprTypecheck_impl.hpp"  // isValidRangeElementType / rangeElementTypeName — fix #3247 / §16.3
#include "sema/FieldSymbolAccessors.hpp"  // fieldDeclTypeOf — fix #5082 (§14.6, v0.307)
#include "sema/SubstArgs.hpp"  // fix #4212: shared generic-substitution walker (§14.1 / §27.8, v0.298)

namespace bezel::sema {

// v0.168 (#1372): the owning `Owned*` family was renamed `Owning*` (and the
// owning interfaces gained the standard `I` prefix; `GetOwnedEnumerator`
// followed as `GetOwningEnumerator`). When an OLD family name fails to resolve,
// point the user at the new spelling rather than a bare "unresolved type name".
// See rationale §3.9 / the v0.168 changelog. The map is identical to the docs
// sweep / scripts/migrate_owning_rename.py.
static std::string owningRenameHint(std::string_view name) {
    struct Entry { std::string_view oldName; std::string_view newName; };
    static const Entry kMap[] = {
        {"GetOwnedAsyncEnumerator", "GetOwningAsyncEnumerator"},
        {"GetOwnedEnumerator", "GetOwningEnumerator"},
        {"IOwnedList", "IOwningList"},
        {"IOwnedCollection", "IOwningCollection"},
        {"OwnedAsyncEnumerable", "IOwningAsyncEnumerable"},
        {"OwnedAsyncEnumerator", "IOwningAsyncEnumerator"},
        {"OwnedEnumerable", "IOwningEnumerable"},
        {"OwnedEnumerator", "IOwningEnumerator"},
        {"OwnedTaskCompletionSource", "OwningTaskCompletionSource"},
        {"OwnedTask", "OwningTask"},
        {"OwnedArray", "OwningArray"},
        {"OwnedBuffer", "OwningBuffer"},
        {"OwnedDictionary", "OwningDictionary"},
        {"OwnedSet", "OwningSet"},
        {"OwnedStack", "OwningStack"},
        {"OwnedQueue", "OwningQueue"},
        {"OwnedLinkedList", "OwningLinkedList"},
    };
    for (const auto& e : kMap) {
        if (name == e.oldName) {
            return std::string("`") + std::string(e.oldName) +
                   "` was renamed `" + std::string(e.newName) +
                   "` in v0.168 (#1372)";
        }
    }
    return std::string();
}

// stdlib §4.2 / spec §14.4 (v0.306): the index of the OWNED element slot in
// one of the six owning collections' generic-argument lists, or npos when
// `name` is not one of them. Slot 0 is `T` for the list/set/stack/queue
// families; slot 1 is `V` for `OwningDictionary<K, V>` (the key K is
// borrowed and value-like by design, §28.11, and is checked by
// checkHashContainerKeys instead).
//
// fix #2735: hoisted out of the declaration-site `ownedSlot` block below so
// the cross-module instantiation-site walk asks the SAME table. Two hand-typed
// copies of a name list is precisely the skew #2742 was about (one recorder
// taught something four siblings were not) — one definition, two call sites.
//
// `OwningBuffer<T>` / `Span<T>` / `ReadOnlySpan<T>` are DELIBERATELY absent:
// §14.4 names them as explicit exceptions that admit value-typed T, and they
// carry their own §28.11 move-only obligation via
// owningBufferSpanMoveOnlySites_. `OwningTask<T>` and the `IOwning*`
// interfaces are absent too — they are the #4305 kNullDeclClassConstraints
// table's subject (and #2736(b)'s residual for the task form), a different
// recorder with a different diagnostic.
static std::size_t owningCollectionOwnedSlot(std::string_view name) {
    if (name == "OwningArray" || name == "OwningSet" ||
        name == "OwningStack" || name == "OwningQueue" ||
        name == "OwningLinkedList")
        return 0;
    if (name == "OwningDictionary") return 1;
    return static_cast<std::size_t>(-1);
}

// fix #3147 (§14.4 / §28.5 / §2.8.3 rule 4, v0.252; family F12 of the
// imported-symbol seam #3152): the name of the BARE type parameter that is the
// declared type of the `owned` field `fs` — `T` or `T?` — or "" when `fs` is
// not an `owned` field, or its type is not a single-part name.
//
// Both sources in ONE place, which is the whole point: a LOCAL body field's
// type is on its `ast::FieldDecl`; an IMPORTED body field has no AST node at
// all (`ModuleLoader` never assigns `Symbol::decl`) and its reconstructed
// TypeRef rides `SemaContext::importedMemberType`. A bare type parameter has no
// TypeSymbol on either side — `Loader::lookupNamed` cannot resolve `T` — so the
// imported spelling is a Named TypeRef with a null declaration whose
// `importedName` is the parameter name.
//
// A POSITIONAL / primary-constructor field is deliberately NOT this check's
// subject: `posParam` marks it on both sides (an imported one carries a
// synthetic `ast::Param`, #2516), and the AST walk this replaced iterated
// `ast::FieldDecl` members only, so skipping it keeps the local answer
// identical.
static std::string_view ownedBareTypeParamOfField(const FieldSymbol& fs,
                                                  const SemaContext& ctx) {
    bool isOwned = false;
    for (std::string_view md : fs.modifiers)
        if (md == "owned") { isOwned = true; break; }
    if (!isOwned) return {};
    if (fs.posParam) return {};  // §9.1 / §22.10 positional field — see above
    if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs.decl)) {
        if (!fd->type || !fd->type->name || fd->type->name->parts.size() != 1)
            return {};
        return fd->type->name->parts.back();
    }
    const TypeRef* ft = ctx.importedMemberType(&fs);
    if (!ft || ft->kind != TypeRef::Kind::Named || ft->declaration) return {};
    std::string_view n = ft->importedName;
    // A qualified head (`Dep.Thing`) is not a type-parameter spelling; the local
    // branch rejects it via `parts.size() != 1` and this is the same rejection.
    if (n.empty() || n.find('.') != std::string_view::npos) return {};
    return n;
}

// Loud, UNCONDITIONAL failure (never compiled out by NDEBUG — matching
// src/common/AwaitScan.cpp's convention rather than a plain `assert()`,
// which release builds strip) for a `kNullDeclClassConstraints` table entry
// (§14.2/§14.4, fix #4305, send-back review) whose recorded slot has fallen
// out of range for its own name's `builtinTypeArity`. The table is
// hand-typed and nothing else pins its shape against the arity table it
// depends on: if a listed name's arity ever changes, the bounds check at
// the call site (`c.slot < ref.genericArgs.size()`) would otherwise
// silently stop firing for it — no crash, no failing test, just a quietly
// disarmed §14.2/§14.4 check. This makes that failure mode loud instead.
// fix #4679 / §4.14 / §14.4 / §28.9 (v0.307, last §4.14/§14.4 reject case
// from the issue's enumeration — `l4_14_0013` "OwningBuffer<string> is
// admitted"): the OwningBuffer/Span/ArraySpan family REJECTS the runtime-
// managed reference types as T (string, function types). Helper for the
// inline annotation-site check below — encapsulates the two-shape
// detection (Kind::Primitive + PrimitiveType::String for the `string`
// keyword, Kind::Function for the function type) so the rejection arm
// has one predicate rather than two inline tests that would have to be
// kept coherent. Returns false on a null ref, an Error ref, or any kind
// that is neither — caller should treat a false answer as "not rejected
// at this site; defer to other checks".
static bool isRuntimeManagedReferenceType(const TypeRef* el) {
    if (!el) return false;
    if (el->kind == TypeRef::Kind::Error) return false;
    // `string` is the §4.6 keyword-spelled, §28.9-runtime-managed primitive.
    if (el->kind == TypeRef::Kind::Primitive &&
        el->primitive == PrimitiveType::String) {
        return true;
    }
    // Function types (§27.1) — every spelling produces a Kind::Function
    // TypeRef — are the §28.9 second runtime-managed family.
    if (el->kind == TypeRef::Kind::Function) return true;
    return false;
}

[[noreturn]] static void unhandledNullDeclClassConstraintSlot(
    std::string_view name, std::size_t slot, int arity) {
    std::fprintf(stderr,
        "internal compiler error: src/sema/TypeResolution_Names.cpp's "
        "kNullDeclClassConstraints table names `%.*s` slot %zu, but "
        "Resolver::builtinTypeArity(\"%.*s\") now reports arity %d -- the "
        "table entry is stale relative to the arity table it depends on "
        "(see fix #4305)\n",
        static_cast<int>(name.size()), name.data(),
        static_cast<std::size_t>(slot),
        static_cast<int>(name.size()), name.data(), arity);
    std::abort();
}

int Resolver::builtinTypeArity(std::string_view n) {
    if (n == "Type" || n == "IDisposable" || n == "IAsyncDisposable" ||
        n == "Subscription" || n == "CancellationToken" ||
        // §6.1 / §30.8: CancellationTokenSource requires `import System.Threading`
        // per the spec, but the compiler does not yet enforce import-gating for
        // System.Threading (same stance as KeyNotFoundException / System.Collections.
        // Generic — see the comment near the KeyNotFoundException entry below).
        n == "CancellationTokenSource" ||
        // stdlib §6.10 (v0.175, #808): TaskScope — the structured-concurrency
        // nursery. Requires `import System.Threading` per the spec; import-
        // gating not yet enforced (same stance as CancellationTokenSource).
        n == "TaskScope" ||
        // §9.8 / issue #550: ISerializable / IDeserializable (stdlib §2.14).
        // Zero-arity interfaces; auto-imported with System (no explicit import
        // required). Resolved here so that `: ISerializable` / `: IDeserializable`
        // in record/enum base-type lists compile without "unresolved type" errors.
        n == "ISerializable" || n == "IDeserializable" ||
        // §2.14 / issue #550: ISerializer / IDeserializer — parameter types in
        // the synthesised / hand-written Serialize(s: ISerializer) /
        // Deserialize(d: IDeserializer) signatures (stdlib §2.14).
        n == "ISerializer" || n == "IDeserializer" ||
        // §18.5 / stdlib §2.18 (v0.172, #812/#1339): IFormattable — the opt-in
        // interface a user type implements to accept a string-interpolation
        // format spec (`ToString(format: string): string`). Zero-arity,
        // auto-imported; resolved here so `: IFormattable` compiles.
        n == "IFormattable") {
        return 0;
    }
    // Slice S25 (§31.4): language-built-in exception types, all
    // nullary, available without import.
    if (n == "Exception" || n == "ArgumentException" ||
        n == "ArgumentNullException" ||
        // stdlib §2.5/§4.4 (v0.183, #1396).
        n == "ArgumentOutOfRangeException" ||
        n == "ArithmeticException" ||
        n == "OverflowException" || n == "DivideByZeroException" ||
        n == "InvalidOperationException" ||
        // §31.4 / issue #601: DisposedAccessException — raised by checked
        // disposal mode (§28.2) on member access of a disposed instance; it
        // is under InvalidOperationException in the hierarchy (§31.4).
        n == "DisposedAccessException" ||
        n == "NotImplementedException" ||
        n == "NullReferenceException" ||
        n == "IndexOutOfRangeException" ||
        // issue #388 / stdlib §4.8: KeyNotFoundException — raised by the
        // Dictionary<K,V> indexer getter for an absent key. It lives in
        // System.Collections.Generic, which §2.3 auto-imports (v0.289,
        // proposal 0122 L3, #3843) — so resolving it (and its sibling
        // collection types Array/Dictionary/…) without an explicit import is
        // correct by design, not an unenforced gate. It joins the resolvable
        // built-in exception set for consistency, enabling `catch
        // KeyNotFoundException`.
        n == "KeyNotFoundException" ||
        n == "MatchFailureException" ||
        n == "OperationCanceledException" ||
        n == "TypeInitializationException" ||
        n == "FormatException" || n == "StackOverflowException" ||
        // §9.8 / issue #550: thrown by synthesised Serialize/Deserialize on
        // malformed binary input.
        n == "SerializationException" ||
        // issue #976 / stdlib §7.4 / §7.5: System.Threading TimeoutException and
        // the System.IO exception family. Per the spec these live in
        // System.Threading / System.IO and require `import`, but the compiler
        // does not yet enforce import-gating for those namespaces (File/Directory/
        // Path resolve without import too) — the still-open spec-ahead-of-impl
        // gap tracked by #3807 (unlike KeyNotFoundException's
        // System.Collections.Generic above, which v0.289 ratified as
        // auto-imported per §2.3, #3843). So they join the resolvable built-in
        // exception set, enabling `catch var ex: IOException` and `throw new
        // IOException(...)`. Hierarchy is in builtinExceptionHierarchy() (codegen).
        n == "TimeoutException" || n == "IOException" ||
        n == "FileNotFoundException" || n == "DirectoryNotFoundException" ||
        n == "EndOfStreamException" || n == "PathTooLongException" ||
        // §22.16 (v0.203, #2053): ContractViolationException — raised by the
        // always-on requires prologue (and the checked-contracts ensures/
        // invariant evaluation) when a clause evaluates false. Registered in
        // the built-in exception hierarchy (kBuiltinExceptionHierarchy, slice
        // 3) deriving directly from Exception; resolvable here so a
        // `catch ContractViolationException` arm type-checks.
        n == "ContractViolationException" ||
        // §31.4 (v0.162): the ExceptionInfo record — the element type of
        // Exception.CauseChain. A zero-arity built-in value type with three
        // string fields (TypeName, Message, StackTrace); resolves without import
        // so `var info: ExceptionInfo := ex.CauseChain[0]` type-checks.
        n == "ExceptionInfo") {
        return 0;
    }
    if (n == "Task") return kBuiltinTaskLike;
    // §6.6 / issue #974: TaskCompletionSource (non-generic, arity 0) and
    // TaskCompletionSource<T> (arity 1) share the same base name. Use
    // kBuiltinTaskLike so both arities (0 and 1) are accepted. The
    // distinction between the two is resolved at method-call time via
    // the presence or absence of a generic argument.
    if (n == "TaskCompletionSource") return kBuiltinTaskLike;
    // §6.6 / issue #974: OwningTaskCompletionSource<T> — always arity 1.
    if (n == "OwningTaskCompletionSource") return 1;
    if (n == "Array" || n == "OwningArray" || n == "OwningTask" ||
        n == "IEnumerable" || n == "IEnumerator" ||
        // stdlib §5 (issue #4178): IOrderedEnumerable<T> — the OrderBy/
        // OrderByDescending result interface (`: IEnumerable<T>`, arity 1,
        // same shape as its sibling IGrouping<TKey,T> below). The type
        // already flows through sema/codegen as a real lazy-query result
        // (ExprTypecheck.cpp / LlvmBackend_Calls3.cpp's "lazy sentinel")
        // whenever it is INFERRED (`var sorted := xs.OrderBy(...)`), but was
        // missing from this table, so writing it explicitly — a return type,
        // parameter type, local annotation, or generic argument — failed
        // "unresolved type name `IOrderedEnumerable`" even though the type
        // is fully functional. No user TypeDecl; same declaration-less
        // Named-ref shape as IGrouping/DictionaryEntry/DrainedEntry.
        n == "IOrderedEnumerable" ||
        n == "IAsyncEnumerable" || n == "IAsyncEnumerator" ||
        n == "IOwningEnumerable" || n == "IOwningEnumerator" ||
        n == "IOwningAsyncEnumerable" || n == "IOwningAsyncEnumerator" ||
        n == "IReadOnlyCollection" || n == "IReadOnlyList" ||
        n == "ICollection" || n == "IList" ||
        n == "IOwningCollection" || n == "IOwningList" ||
        n == "IEvent" || n == "IAsyncEvent" ||
        // fix #1832 / stdlib §6.5 (v0.187): the cross-domain publication-
        // point pair (sibling of IEvent/IAsyncEvent + EventSource/AsyncEventSource
        // above). ISharedEvent<out T> is the public subscribe-only face,
        // SharedEventSource<T> is the producer-side sealed class. Both are
        // declaration-less Named refs at arity 1, same shape as the instance
        // pair — the runtime synchronization is a follow-up slice; this just
        // admits the names so sema no longer rejects `SharedEventSource<int>`
        // / `ISharedEvent<int>` as "unresolved type name".
        n == "ISharedEvent" || n == "SharedEventSource" ||
        // stdlib §6.5 / spec §2.3 (fix #1536): EventSource<T> / AsyncEventSource<T>
        // — the producer-side (in-declaring-class) face of an event member
        // (§24). Per §2.3, `IEvent<T>`/`IAsyncEvent<T>` are auto-imported (they
        // are the public type of every event member) but `EventSource<T>` /
        // `AsyncEventSource<T>` require `import System.Threading`, like
        // `CancellationTokenSource` / `Mutex` and the rest of that module.
        // Before this fix neither name was registered at all, so a helper
        // method whose parameter type names the in-class face (spec-legal per
        // §24) could not be written. Same import-gating stance as
        // CancellationTokenSource/TaskScope above: not yet enforced. Arity 1
        // (single payload-type parameter); declaration stays null (no user
        // TypeDecl) — same representation as Channel<T> below.
        n == "EventSource" || n == "AsyncEventSource" ||
        // §2.8 / §2.9: auto-imported generic interfaces (stdlib §2.8–§2.9).
        // Registered here so they resolve as base types and field/param types
        // (fix #576: "unresolved type name 'IComparable'").
        // §5 / #975: IEqualityComparer<T> — the user-implemented equality agent
        // (Equals(a,b):bool, GetHashCode(value):int); arity 1.
        n == "IComparable" || n == "IEquatable" || n == "IEqualityComparer" ||
        // stdlib §2.19 / §11.8 (v0.176, #807): the static-abstraction
        // interfaces (abstract-only static members; recurrence-constrained).
        // Auto-imported, arity 1; the synthesized declaration is attached by
        // ctx_.builtinInterfaceSym below.
        n == "INumber" || n == "ISteppable" || n == "IParsable" ||
        // §3 stdlib collection types (arity 1):
        n == "Stack" || n == "OwningStack" ||
        n == "Queue" || n == "OwningQueue" ||
        n == "Set" || n == "OwningSet" ||
        n == "LinkedList" || n == "OwningLinkedList" ||
        // stdlib §4.21 (arity 1): OwningSlotMap<T> — registered here so that
        // `new OwningSlotMap<T>()` and `OwningSlotMap<T>` as a type argument
        // resolve without an explicit import. spec #7878 states it declares no indexer.
        n == "OwningSlotMap" ||
        // §28.13 / stdlib §2.16: Shared<T> and Weak<T> (arity 1).
        n == "Shared" || n == "Weak" ||
        // §4.6: LinkedListNode<T> is the node handle returned by Add*/Find; it
        // carries a single generic element-type parameter and is resolved
        // without a user TypeDecl (same pattern as the collection types above).
        n == "LinkedListNode" ||
        // §4.14 / issue #550: OwningBuffer<T> (e.g. OwningBuffer<byte> returned
        // by BinarySerializer.ToBytes() per stdlib §2.15).
        n == "OwningBuffer" ||
        // fix #3249 (§4.19, v0.186): FrozenArray<T> — the deeply-immutable,
        // fixed-size, bz_array_t*-backed shared-read primitive. Arity 1; the
        // resolved TypeRef carries no user TypeDecl (the declaration-less Named
        // shape every other §4 collection type uses here). Element-type
        // restriction is the §3.6 structural check (T must be a sendable value
        // type or string; classes/records excluded — the FrozenArray.From
        // copying overload does not admit class trees, the recorded pressure
        // on the §4.19 prose's "frozen ownership trees are a designed future
        // extension").
        n == "FrozenArray" ||
        // §4.14-§4.15 / issue #959: the concrete span VALUE types — borrowing
        // spans over Array<T> (ArraySpan/ReadOnlyArraySpan, §4.15) and owning
        // spans over OwningBuffer<T> (Span/ReadOnlySpan, §4.14). Each carries a
        // single element-type parameter and resolves to a Named TypeRef with a
        // null declaration (no user TypeDecl); codegen lowers them to the shared
        // bz.arrayspan struct {ptr,i32,i32} (lowerTypeRef / isSpanLikeTypeName).
        // Registering them lets a span be used as a parameter type, local
        // annotation, or record field type — not just inferred from `.AsSpan()`.
        //
        // §4.16 / issue #970: IReadOnlySpan<T> is the read-only interface over
        // any span. Spans can be implicitly boxed into it for call-bounded
        // parameter use (§8.6 call-bounded boxing). The synthesized TypeSymbol is
        // produced by ctx_.builtinInterfaceSym("IReadOnlySpan") below.
        n == "ArraySpan" || n == "ReadOnlyArraySpan" ||
        n == "Span" || n == "ReadOnlySpan" ||
        n == "IReadOnlySpan") {
        return 1;
    }
    // §3 stdlib two-parameter collection types (arity 2):
    if (n == "Dictionary" || n == "OwningDictionary") return 2;
    // §4 stdlib sorted collection types (stdlib §4.12/§4.13, v0.165 / #1354):
    // SortedDictionary<K,V> (arity 2) and SortedSet<T> (arity 1).  Both conform
    // to IReadOnlyCollection per the §4 update; registering them lets
    // for-in / LINQ / member-call type-checking proceed without "unresolved type".
    if (n == "SortedDictionary") return 2;
    if (n == "SortedSet") return 1;
    // issue #2794 (stdlib §4.8 / §4.9 / §5): the three 2-parameter built-in
    // entry/grouping types the compiler knows but `builtinTypeArity`'s hand
    // list was missing. Each is in a documented public surface
    // (`DictionaryEntry<K,V>` record, §4.8; `DrainedEntry<K,V>` class, §4.9;
    // `IGrouping<TKey,T>` interface, §5) yet was unresolvable as a WRITTEN
    // type annotation — `unresolved type name`, no exception for arity or
    // member-call surface — which prevented any helper method whose
    // signature names one from compiling. The same shape as EventSource
    // (#1536) and the Channel<T> family: arity 2, no user TypeDecl,
    // declaration stays null, codegen lowers via the dedicated arm at
    // LlvmBackend_Layout.cpp:1353 (DictionaryEntry / DrainedEntry — {K,V}
    // struct) or the §4 heap-pointer builtin arm (IGrouping — ptrTy(),
    // already a member of `kHeapPointerReferenceBuiltins`).
    if (n == "DictionaryEntry" || n == "DrainedEntry" ||
        n == "IGrouping") return 2;
    // stdlib §2.19 / spec §15.4.1 (issue #4178 investigation; NOT registered
    // here — see issue #4202). `ICollectionLiteralTarget<TSelf, T>` /
    // `IOwningCollectionLiteralTarget<TSelf, T>` were measured as a sibling
    // "unresolved type name" gap while fixing #4178's IOrderedEnumerable, and
    // a `builtinTypeArity` entry identical in shape to the one above was
    // written and tested — but naming them exposed a REAL, reachable
    // IR-verifier crash ("Invalid indices for GEP pointer type!"), not just
    // the expected "structural, not nominal, dispatch" gap #4202 already
    // tracks: any class with FIELDS that declares
    // `: ICollectionLiteralTarget<Self, T>` in its base list (the spec's own
    // §15.4.1 `RingBuffer<T>` worked example, once it has real fields and a
    // real `Push`/indexer body — the spec's own elided-body example did not
    // stress this) corrupts class-layout GEP codegen. A minimal
    // no-fields repro did not reveal it, which is why it was not caught
    // before the first attempt at this line. Reverted rather than shipped;
    // see issue #4202 for the measured repro and the GEP crash detail. Do
    // NOT re-add this line without first fixing whatever in class-layout
    // codegen treats a builtin interface base as a real, offset-consuming
    // base — the earlier structural-vs-nominal dispatch gap is the SMALLER
    // of the two problems here.
    // §30.14 / §30.15 (#600): Channel<T> and ChannelSender<T> (arity 1).
    // Domain is a static class (arity 0); ChannelClosedException is a nullary
    // exception type (§30.15); DomainFaultedException is a nullary exception.
    // §6.9 / §30.15 v0.174 (#809): ChannelFullException — nullary exception
    // raised by the sync Send on a full bounded channel.
    if (n == "Channel" || n == "ChannelSender") return 1;
    if (n == "Domain" || n == "ChannelClosedException" ||
        n == "ChannelFullException" ||
        n == "DomainFaultedException") return 0;
    // §6.4 / issue #647: Atomic<T> (arity 1) and AtomicInteger<T> (arity 1).
    // Both are generic compiler-intrinsic classes; T is restricted by the compiler
    // (see ExprTypecheck_Exprs.cpp).
    if (n == "Atomic" || n == "AtomicInteger") return 1;
    // §16.5 / issue #922: Range<T> is a compiler-intrinsic record (§9.8).
    // `var r: Range<int> := 1..5` is normative syntax (§16.5); arity 1 so
    // the explicit annotation resolves.  The resolved TypeRef receives
    // declaration = ctx_.builtinClassSym("Range") below (same pointer as the
    // inferred form) so isRangeType / .Start/.End / for-in codegen recognise
    // it identically to the inferred `1..5` form.
    if (n == "Range") return 1;
    // §10.5 / stdlib: `Ptr<T>` — compiler-intrinsic FFI pointer record (the
    // generic sibling of IntPtr/UIntPtr in stdlib §10). Arity 1; resolves to
    // a Named TypeRef with declaration = ctx_.builtinClassSym("Ptr") below,
    // same shape every other compiler-intrinsic arity-1 type uses here.
    // Without this registration `record ForeignBuf(P: Ptr<byte>)` errors
    // "unresolved type name `Ptr`" before isAdmissibleComponentType (#3327)
    // ever sees it, so the §9.2/§25.2 reject of FFI-pointer-component records
    // is unreachable.
    if (n == "Ptr") return 1;
    // issue #545: __Buffer<T> — fixed-capacity compiler-intrinsic primitive (arity 1).
    // T restricted to value-type primitives (int/byte/short/long/double/decimal/
    // char/bool) in this revision; class-element __Buffer is the §5.20 design pass.
    // fix #5390 (§2.3, v0.314): the builtin was previously named "Buffer". The
    // rename to "__Buffer" makes it internal-only (not always-available). "Buffer"
    // without a declaration in scope is now an unresolved type.
    if (n == "__Buffer") return 1;
    // §6.2 / issue #647: Mutex and Lock — synchronization primitives.
    // Mutex has a public constructor(). Lock has NO public constructor (obtained
    // only from Mutex.Acquire / TryAcquire); both are nullary class types.
    if (n == "Mutex" || n == "Lock") return 0;
    // §6.3 / issue #647: Semaphore and SemaphoreToken — counting semaphore.
    // Semaphore has constructor(initialCount: int) and constructor(initialCount,
    // maxCount); SemaphoreToken has NO public constructor (obtained only from
    // Semaphore.Acquire / TryAcquire); both are nullary class types here.
    if (n == "Semaphore" || n == "SemaphoreToken") return 0;
    // §3.3 / issue #883: ReadOnlyStringSpan — a string-backed read-only span
    // (System.Text, auto-imported). Non-generic record viewing a contiguous
    // range of a `string` (Source: string, Start: int, Length: int). Distinct
    // from ReadOnlySpan<char> (which views an OwningBuffer<char>) because string
    // storage is adaptive (1/2/4 bytes per code point — §4.6). Zero-arity.
    if (n == "ReadOnlyStringSpan") return 0;
    // §9.4 / issue #654: Boolean — builtin record aliased by `bool`; zero-arity.
    // Registered so `Boolean.TryParse(...)` resolves without "unresolved type".
    if (n == "Boolean") return 0;
    // §2.12 / issue #654: BitConverter — static class for IEEE 754
    // bit-reinterpretation; zero-arity, auto-imported.
    if (n == "BitConverter") return 0;
    // §3 nullary stdlib classes:
    if (n == "StringBuilder" || n == "Random" ||
        n == "DateTime" || n == "TimeSpan" ||
        // per §2.5 / #1357: DateOnly and TimeOnly — value types (stored as i32),
        // auto-imported like DateTime/TimeSpan; no user TypeDecl.
        n == "DateOnly" || n == "TimeOnly" ||
        // stdlib §2.6 (#1464): Guid — a 128-bit value record ({i64,i64}),
        // auto-imported like the temporal value types; no user TypeDecl. Without
        // this, `g: Guid` / `: Guid` / field-of-Guid annotations errored
        // "unresolved type name `Guid`" even though `Guid.NewGuid()` etc. work.
        n == "Guid" ||
        n == "File" || n == "Directory" || n == "Path" ||
        // stdlib §7.2 / issue #645: FileStream — the concrete §7.1 Stream over
        // an OS file handle. A heap-allocated builtin class (bz_filestream_t*)
        // with no user TypeDecl, disposable per §7.2's flush-on-dispose
        // contract. Its FileMode/FileAccess/SeekOrigin enums are NOT here —
        // they are ordinary Bezel enums injected by the stdlib prelude
        // (Driver.cpp), so they carry real TypeDecls.
        n == "FileStream" ||
        // fix #4867 / stdlib §7.1: Stream — abstract base for byte I/O.
        n == "Stream" ||
        // fix #4867 / stdlib §7.4: TextReader / TextWriter — abstract bases for
        // character input / output.
        n == "TextReader" || n == "TextWriter" ||
        n == "Encoding" || n == "Linq" ||
        // §2.15 / issue #550: BinarySerializer / BinaryDeserializer (opaque
        // runtime handles; require `import System.Serialization` per spec, but
        // like CancellationTokenSource we do not yet enforce import-gating here).
        n == "BinarySerializer" || n == "BinaryDeserializer" ||
        // §2.15 / issue #550: JsonSerializer / JsonDeserializer (opaque
        // runtime handles; same import-gating behaviour as Binary variants).
        n == "JsonSerializer" || n == "JsonDeserializer" ||
        // §2.15 / issue #687: JsonOptions — a compiler-known value record
        // (Indented: bool, IndentWidth: int, AllowTrailingCommas: bool,
        // IgnoreUnknownFields: bool, MaxDepth: int). Requires
        // `import System.Serialization` like the serializer/deserializer types.
        n == "JsonOptions" ||
        // §8.1 / issue #646: Stopwatch — heap-allocated timing class.
        // §8.2 / issue #646: Debug — static class for assertions and logging.
        // Both require `import System.Diagnostics` per spec (stdlib §8); the
        // gate enforcing that import lives below at the §2.3 admission site,
        // mirroring #631's System.Serialization pattern. The admission here
        // makes the type name resolvable to a Named ref with a null
        // declaration; the import gate fires only when the required import
        // is absent (and emits a diagnostic so codegen does not silently run).
        n == "Stopwatch" || n == "Debug") return 0;
    // §32.5 / stdlib §10: FFI-boundary types. IntPtr/UIntPtr are
    // compiler-intrinsic value types (pointer-sized integers); CString,
    // WString, RawBytes are FFI-boundary classes. They have no user
    // TypeDecl, so — like the §3 built-ins above — they resolve to a
    // Named TypeRef with a null declaration. All are nullary. Making them
    // resolvable lets §32 extern signatures using them compile (§32.5);
    // before this they errored "unresolved type name".
    if (n == "IntPtr" || n == "UIntPtr" ||
        n == "CString" || n == "WString" || n == "RawBytes") return 0;
    return kNotBuiltin;
}

// §26.4/§26.5 (#1599): recursively walk `target`'s generic-argument list,
// binding each bare single-segment non-nullable identifier as a block-level
// type parameter of the enclosing extension — at any nesting depth (v0.183:
// "The binding rule applies at any nesting depth of the extended type's
// argument positions... `extension IEnumerable<Array<T>>` binds T").
//
// A generic arg that is a `Named` type carrying its own non-empty
// `genericArgs` (e.g. `Array<byte>` or `Array<T>`) is itself a generic
// instantiation — per §26.4 "a concrete instantiation is written with...
// generic instantiations (`extension IEnumerable<int>`, `extension
// IEnumerable<Array<byte>>`)". Its head name (`Array`) is therefore NEVER a
// free type parameter, even though the name is a bare single segment and
// non-nullable — only recurse into ITS generic args to find any nested bare
// identifiers to bind. This is the #1599 fix: previously the single,
// non-recursive top-level loop treated any bare single-segment name as a
// type-parameter head regardless of whether it carried its own generic args,
// so `Array<byte>`'s own head `Array` was wrongly bound as a fresh free type
// parameter (silently discarding the `<byte>` argument) and the extension
// target unified with any `IEnumerable<X>` receiver.
//
// `functionSlotDiagReported` is shared with the recursive calls so the §26.4
// diagnostic fires EXACTLY ONCE per extension head (per issue #4448,
// v0.303, the spec's "should report once per extension head, at the
// offending identifier" wording) — even when the function-type slot is
// nested under multiple generic-instantiation heads
// (`extension IEnumerable<Array<(T) -> T>>`).
void Resolver::bindExtensionTargetTypeParams(
        const ast::Type& target, Scope& extScope, Scope* parent,
        bool& functionSlotDiagReported) {
    for (const auto& ga : target.genericArgs) {
        if (!ga) continue;
        if (ga->kind == ast::TypeKind::Named && ga->name &&
            ga->name->parts.size() == 1 && !ga->genericArgs.empty()) {
            // Concrete generic-instantiation head (e.g. `Array` in
            // `Array<byte>` / `Array<T>`): not a free type parameter.
            // Recurse to bind any nested bare identifiers in its own args.
            bindExtensionTargetTypeParams(*ga, extScope, parent,
                                          functionSlotDiagReported);
            continue;
        }
        if (ga->kind == ast::TypeKind::Named &&
            ga->name && ga->name->parts.size() == 1 &&
            ga->nullable) {
            // §26.4 (v0.183): "A nullable-marked identifier in an
            // argument position (`extension Array<T?>`) binds
            // nothing and is a compile error naming this rule —
            // only the bare non-nullable identifier introduces a
            // parameter." Report THIS specific rule here, at the
            // declaration, rather than falling through to ordinary
            // name resolution on `T` below: (1) the generic
            // "unresolved type name" fallback is actively
            // misleading here — `T` is not a typo, it is written
            // in exactly the position a type-parameter head would
            // occupy, just illegally nullable-marked; and (2) if an
            // unrelated type happened to be named `T` in enclosing
            // scope, ordinary name resolution would silently bind
            // to it instead of erroring — precisely the
            // action-at-a-distance hazard this rule exists to rule
            // out (mirroring the non-nullable binding rule's
            // "always, shadowing any same-named type in scope").
            std::string_view paramName = ga->name->parts[0];
            ctx_.diagnostics().report(
                Severity::Error, ga->span.begin,
                "nullable-marked type-parameter head `" +
                std::string(paramName) +
                "?` is not permitted in an extension's target type "
                "(§26.4): only a bare, non-nullable identifier "
                "introduces a block-level type parameter — write `" +
                std::string(paramName) + "` (a bare `" +
                std::string(paramName) + "` already unifies with a "
                "nullable receiver argument, e.g. `Array<int?>`)");
            // Register the (unbound) name anyway so the recursive
            // resolution of this same argument just below does not
            // cascade a second, redundant "unresolved type name"
            // diagnostic over the identical span — the rule above
            // is the diagnostic of record for this declaration.
            extScope.names[paramName] = nullptr;
            continue;
        }
        if (ga->kind == ast::TypeKind::Named &&
            ga->name && ga->name->parts.size() == 1 &&
            !ga->nullable) {
            // Single-part bare name with no generic args of its own: treat
            // as a type parameter.
            std::string_view paramName = ga->name->parts[0];
            // §14.1 / §26.4 (fix #5393, v0.314): the names in one type-parameter
            // list are distinct.  Check for duplicate within this list first.
            if (extScope.names.count(paramName) > 0) {
                ctx_.diagnostics().report(
                    Severity::Error, ga->span.begin,
                    "duplicate type parameter name `" +
                        std::string(paramName) +
                        "` in this type-parameter list (§14.1, v0.314)");
            }
            // §26.4 (#1346, v0.160): warn when the bare identifier that
            // becomes a block-level type parameter shadows a same-named
            // type already in scope. The binding is always the extension's
            // type parameter (action-at-a-distance would silently re-mean
            // the block if we fell back to the scope-found type), but a
            // shadowing author may not have intended the override.
            for (Scope* s = parent; s; s = s->parent) {
                auto it = s->names.find(paramName);
                if (it != s->names.end()) {
                    if (it->second != nullptr) {
                        ctx_.diagnostics().report(
                            Severity::Warning, ga->span.begin,
                            "type parameter `" + std::string(paramName) +
                            "` in this extension block shadows the type `" +
                            std::string(it->second->name) +
                            "` that is in scope (§26.4); the extension "
                            "type parameter always takes precedence");
                    }
                    break;
                }
            }
            extScope.names[paramName] = nullptr;
            continue;
        }
        if (ga->kind == ast::TypeKind::Function) {
            // §26.4 (v0.303, issue #4448): "Argument positions" are a
            // constructed generic type's argument positions and nothing
            // else — a function type's parameter and return slots are
            // NOT among them, so `extension Array<(T) -> T>` binds no
            // `T`, and the bare identifier there is a compile error
            // naming this rule rather than an unresolved-name error.
            // The nesting clause composes only through generic
            // arguments — `extension IEnumerable<Array<(T) -> T>>`
            // binds nothing either. This is the same near-miss, same
            // treatment as the nullable-marked argument-position
            // diagnostic above (which is itself §26.4's other named
            // reject): do NOT fall through to the generic "unresolved
            // type name `T`" fallback, because that is actively
            // misleading here — `T` is not a typo, it is written in
            // exactly the position a type-parameter head would occupy
            // (had the surrounding slot been an argument position
            // rather than a function-type slot). Report THIS rule
            // here, at the first offending bare identifier inside the
            // function type, naming the section and the reason.
            auto tryBare = [&](const ast::Type* t) -> std::string_view {
                if (t && t->kind == ast::TypeKind::Named &&
                    t->name && t->name->parts.size() == 1 &&
                    !t->nullable) {
                    std::string_view n = t->name->parts[0];
                    // The §26.4 diagnostic REPLACES the generic "unresolved
                    // type name" fallback (per issue #4448: "a compile
                    // error naming this rule rather than an unresolved-
                    // name error"). Skip any bare identifier that would
                    // resolve normally — i.e. primitives, built-in types
                    // (Array / String / Exception / ...), and types
                    // already in scope via the enclosing module —
                    // because for those `resolveNamed` does NOT emit
                    // "unresolved type name" and there is nothing to
                    // replace. The bare form matters precisely because
                    // it would otherwise cascade two `unresolved type
                    // name `T`` errors for one mistake (#4192's repro);
                    // a resolvable identifier is not that mistake.
                    if (primitiveFromName(n) != PrimitiveType::None)
                        return {};
                    if (isBuiltinTypeName(n))
                        return {};
                    if (lookupName(n, parent) != nullptr)
                        return {};
                    return n;
                }
                return {};
            };
            std::string_view firstBare;
            SourceLocation firstBareLoc = ga->span.begin;
            for (const auto& p : ga->funcParams) {
                auto n = tryBare(p.type.get());
                if (!n.empty()) {
                    // Register the unbound name anyway so the recursive
                    // resolution of this same argument below does not
                    // cascade a second, redundant "unresolved type
                    // name" diagnostic over the identical span — this
                    // rule is the diagnostic of record for the
                    // declaration, exactly as the nullable-marked arm
                    // above does. The same suppression is required for
                    // any *other* bare identifiers that appear in the
                    // same function type: each would otherwise fall
                    // through to ordinary name resolution and trigger
                    // an "unresolved type name" error of its own.
                    extScope.names[n] = nullptr;
                    if (firstBare.empty()) {
                        firstBare = n;
                        firstBareLoc = p.type->span.begin;
                    }
                }
            }
            if (ga->funcReturn) {
                auto n = tryBare(ga->funcReturn.get());
                if (!n.empty()) {
                    extScope.names[n] = nullptr;
                    if (firstBare.empty()) {
                        firstBare = n;
                        firstBareLoc = ga->funcReturn->span.begin;
                    }
                }
            }
            if (!firstBare.empty() && !functionSlotDiagReported) {
                ctx_.diagnostics().report(
                    Severity::Error, firstBareLoc,
                    "bare identifier `" + std::string(firstBare) +
                    "` in an extension's target-type function slot binds "
                    "nothing (§26.4): a function type's parameter and "
                    "return slots are not argument positions \xE2\x80\x94 "
                    "binding reaches only a constructed generic type's "
                    "argument positions, at any nesting depth through "
                    "generic arguments");
                functionSlotDiagReported = true;
            }
        }
    }
}

// fix #4144 (§26.2, v0.295): the extendable-target list is CLOSED — the
// section states exactly which kinds may be extended (classes including
// `Object`, interfaces, records, enums, built-in primitives) and exactly
// which may not (type parameters, nullable types directly, function types,
// choices, inline-array types) — but nothing in `src/sema/` gated
// `ExtensionDecl`'s resolved target kind against that list. A `choice`
// target reached codegen and crashed with an unterminated basic block
// (`case this of` lowering has no extension-aware branch, #4144); a nullable
// target (`extension int?`) reaches a DIFFERENT codegen crash (a
// call-parameter-type IR-verifier failure), because
// `extensionTargetMatchesReceiver` (ExprTypecheck.cpp ~6150) explicitly
// documents "ignoring nullability... because §26.2 forbids `extension
// int?`" — an invariant nothing was enforcing; a function-type target
// silently compiles and runs with the extension member's body degraded to
// a no-op via the #3079 backstop. Inline-array targets are the one kind
// that does NOT reach a crash today — an unrelated, pre-existing blanket
// member-lookup gate (ExprTypecheck_Calls.cpp ~15859) already rejects any
// non-allow-listed member access on an inline-array RECEIVER, citing
// §26.2 — but the extension DECLARATION itself still compiles clean with
// dead, uncallable members, which is not what a closed list means. Reject
// all four live-crash/live-gap kinds here, at the declaration site, right
// after the target type resolves — before any member of the block is
// typechecked or reaches codegen.
//
// "Type parameters" (`extension T` is meaningless, §26.2) is the fifth
// listed kind and is deliberately NOT given a case here: an extension's
// own §26.4 type-parameter binding only auto-binds a bare identifier
// found in the target type's GENERIC ARGUMENT positions
// (`bindExtensionTargetTypeParams` above), never the target type's own
// head, so a bare `extension T` can only ever resolve `T` as an ordinary
// name lookup — succeeding only if `T` names a real declared type (in
// which case it extends that concrete type, which is legitimate and not
// an instance of this rule at all) or failing with the existing, general
// "unresolved type name" diagnostic. There is no TypeRef shape a live
// program can produce here that represents "the target IS a type
// parameter", so there is nothing this gate could observe and reject that
// current behavior does not already handle correctly.
void Resolver::rejectDisallowedExtensionTarget(const ast::ExtensionDecl& ext) {
    if (!ext.targetType) return;
    const TypeRef* ref = ctx_.resolvedType(*ext.targetType);
    if (!ref) return;
    const SourceLocation loc = ext.targetType->span.begin;
    // §26.2: "Nullable types directly (`extension int?`). Extend the
    // non-nullable form; callers handle nullability at the call site with
    // `?.` (§7.5)." — the TOP-LEVEL nullable marker on the target type
    // itself; a nullable type nested inside a generic argument position
    // (`extension Array<int?>`) is unaffected — §26.4 already covers that
    // a bare block-level parameter unifies with nullable arguments.
    if (ref->nullable) {
        ctx_.diagnostics().report(
            Severity::Error, loc,
            "cannot extend a nullable type directly (\xC2\xA7""26.2): "
            "extend the non-nullable form instead \xE2\x80\x94 callers "
            "handle nullability at the call site with `?.` "
            "(\xC2\xA7""7.5)");
        return;
    }
    switch (ref->kind) {
        case TypeRef::Kind::Named: {
            // §26.2: "Choices. The extendable list above is closed against
            // the choice kind..."
            if (ref->declaration &&
                ref->declaration->kind == ast::TypeDeclKind::Choice) {
                ctx_.diagnostics().report(
                    Severity::Error, loc,
                    "cannot extend a choice type `" +
                    std::string(ref->declaration->name) +
                    "` (\xC2\xA7""26.2): the extendable list is closed "
                    "against the choice kind \xE2\x80\x94 no \xC2\xA7""14.2 "
                    "constraint names the value-like classification bound a "
                    "payload-generic extension surface would need, and "
                    "stdlib `Result<T, E>`'s value-payload combinator "
                    "surface is compiler-provided (\xC2\xA7""35.2), not an "
                    "extension block");
            }
            return;
        }
        case TypeRef::Kind::Function:
            // §26.2: "Function types."
            ctx_.diagnostics().report(
                Severity::Error, loc,
                "cannot extend a function type (\xC2\xA7""26.2)");
            return;
        case TypeRef::Kind::InlineArray:
            // §26.2: "Inline-array types (`extension [N]T`). Inline arrays
            // are value-typed but their identity is per-size... so an
            // extension declaration would have to fix N, making the form
            // rarely useful."
            ctx_.diagnostics().report(
                Severity::Error, loc,
                "cannot extend an inline-array type (\xC2\xA7""26.2): inline "
                "arrays are value-typed but their identity is per-size "
                "(`[3]int` and `[4]int` are distinct types), so an "
                "extension declaration would have to fix N \xE2\x80\x94 "
                "write a static method that takes a specific inline-array "
                "type per overload, or a heap-array view (`Array<T>`) the "
                "caller constructs from the inline array's contents");
            return;
        default:
            return;
    }
}

void Resolver::resolveTopDecl(const ast::TopDecl& decl, Scope* parent) {
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(&decl)) {
        resolveTypeDecl(*td, parent);
    } else if (auto* alias = dynamic_cast<const ast::TypeAlias*>(&decl)) {
        Scope aliasScope;
        aliasScope.parent = parent;
        for (const auto& g : alias->genericParams) {
            // Register a temporary symbol for lookup; we don't need to
            // create a TypeSymbol stand-in because the alias body only
            // ever references the name.
            aliasScope.names[g.name] = nullptr; // marker for "is generic"
        }
        if (alias->aliasedType) {
            resolveType(*alias->aliasedType, &aliasScope);
        }
        for (const auto& gc : alias->genericConstraints) {
            for (const auto& item : gc.items) {
                if (item.typeArg) resolveType(*item.typeArg, &aliasScope);
            }
        }
    } else if (auto* ext =
                   dynamic_cast<const ast::ExtensionDecl*>(&decl)) {
        Scope extScope;
        extScope.parent = parent;
        // §26.4: a generic extension `extension Foo<T>` introduces T as a
        // type parameter scoped to the entire extension body (member
        // signatures, return types, param types, body locals, and
        // where-constraints). The grammar stores the generic args as part of
        // the target type (ast::Type::genericArgs), not in a separate
        // genericParams list. Bind each single-part generic arg (a bare name
        // like `T`) into extScope as a nullptr marker — the same sentinel
        // that resolveTypeDecl uses for TypeDecl::genericParams — so that
        // resolveNamed's "is this name a generic param?" walk finds it.
        // This must be done BEFORE resolving the target type so that T is
        // already in scope when the targetType itself is processed (e.g.
        // `IEnumerable<T>` must not error on T while resolving the `T` arg).
        //
        // §26.4/§26.5 (#1599): the walk must recurse into any generic arg
        // that is ITSELF a generic instantiation (e.g. `Array<byte>` or
        // `Array<T>` inside `IEnumerable<Array<byte>>` / `IEnumerable<Array<T>>`)
        // — such an arg's own head name (`Array`) is a CONCRETE instantiation
        // head, not a free type parameter, even though it is a bare
        // single-segment non-nullable name. See bindExtensionTargetTypeParams.
        if (ext->targetType) {
            // §26.4 (v0.303, #4448): track whether the function-slot
            // diagnostic has fired for THIS extension head; reset per
            // top-level `extension` so nested extensions each get their
            // own once-per-head budget.
            bool functionSlotDiagReported = false;
            bindExtensionTargetTypeParams(*ext->targetType, extScope, parent,
                                          functionSlotDiagReported);
        }
        if (ext->targetType) resolveType(*ext->targetType, &extScope);
        // fix #4144 (§26.2, v0.295): reject a target kind the closed
        // extendable list excludes, before any member of the block is
        // resolved further or reaches codegen.
        rejectDisallowedExtensionTarget(*ext);
        for (const auto& gc : ext->genericConstraints) {
            for (const auto& item : gc.items) {
                if (item.typeArg) resolveType(*item.typeArg, &extScope);
            }
        }
        for (const auto& m : ext->members) {
            if (m) resolveMember(*m, &extScope);
        }
    } else if (auto* xb =
                   dynamic_cast<const ast::ExternBlock*>(&decl)) {
        for (const auto& d : xb->methods) {
            for (const auto& p : d.params) {
                if (p.type) {
                    resolveType(*p.type, parent);
                    // per §32.5: only FFI-compatible types may appear in an
                    // extern method signature. A parameter is never a return
                    // position, so the unit type `()` is not admitted here.
                    checkExternType(*p.type, /*returnPosition=*/false,
                                    std::string("parameter `") +
                                        std::string(p.name) + "`",
                                    parent, p.modifier);
                }
            }
            if (d.returnType) {
                resolveType(*d.returnType, parent);
                // per §32.5: the return slot may additionally be the unit
                // type `()`. (An omitted `: T` clause leaves returnType null
                // and is already a unit-returning method — §32.5.)
                checkExternType(*d.returnType, /*returnPosition=*/true,
                                "return type", parent);
            }
        }
    }
}

// fix #4047 (§27.8, v0.294): see the declaration comment in
// TypeResolution_impl.hpp for the full rationale. After the single forward
// pass over `src.topDecls` has resolved every top-level alias's
// `aliasedType` once, an outer alias whose target was an inner alias
// declared textually LATER sits with a `TypeRef::Kind::Named`,
// `declaration == nullptr` silent-unresolved state. Re-resolve every
// alias that matches that shape until either progress stops or the
// iteration cap is hit. A user-written alias cycle (A := B; B := A) is
// not a bug this fix is chartered to handle; the iteration cap makes the
// pass deterministic rather than infinite-looping, and the resulting
// still-unresolved state will surface as the same downstream diagnostic
// it does today.
void Resolver::resolveAliasForwardReferences(const ast::SourceFile& src,
                                              Scope* parent) {
    for (int iter = 0; iter < 32; ++iter) {
        bool anyResolved = false;
        for (const auto& decl : src.topDecls) {
            if (!decl) continue;
            auto* alias = dynamic_cast<const ast::TypeAlias*>(decl.get());
            if (!alias || !alias->aliasedType) continue;
            TypeRef* current = ctx_.resolvedType(*alias->aliasedType);
            // Only the buggy shape: Named with no declaration. Any other
            // shape (Primitive / Function / InlineArray / a Named that did
            // resolve) is left alone — re-running resolveType on a
            // correctly-resolved alias would just allocate a fresh TypeRef
            // and discard the old, costing nothing semantically but
            // producing no progress.
            if (!current) continue;
            if (current->kind != TypeRef::Kind::Named) continue;
            if (current->declaration != nullptr) continue;
            // fix #4105 (§27.8, v0.300): invalidate stale cache entries on
            // the alias node and its aliasedType node before calling
            // resolveType. The first forward pass caches the alias's own
            // TypeRef on the alias node (Kind::Named, TypeAliasSymbol) and
            // also caches an intermediate Kind::Named result on the
            // aliasedType node (e.g. for A:=B where B:=int, B's cache is
            // Kind::Named, B's TypeAliasSymbol from the resolveTopDecl call
            // for "type B := int" — the alias-transparency arm wrote the
            // Kind::Named result, not the final resolved type). Without
            // invalidation, resolveType below hits the stale cache and
            // returns without re-invoking resolveNamed, so the alias chain
            // never gets re-resolved through to the underlying type.
            // Invalidate the stale cache entry for the aliasedType node that
            // the first forward pass resolved to a Kind::Named / TypeAliasSymbol
            // entry.  The TypeAlias node itself (alias) is NOT a cache key —
            // only its aliasedType is.
            ctx_.setResolvedType(
                static_cast<const ast::Type*>(alias->aliasedType.get()),
                nullptr);
            // Rebuild the same per-alias generic-parameter scope that
            // resolveTopDecl used on the first pass, so a generic alias
            // (`type A<T> := B<T>;`) sees its own type parameter `T`
            // bound when the alias-transparency arm walks the inner
            // alias's aliasedType through it.
            Scope aliasScope;
            aliasScope.parent = parent;
            for (const auto& g : alias->genericParams) {
                aliasScope.names[g.name] = nullptr;
            }
            TypeRef* retried =
                resolveType(*alias->aliasedType, &aliasScope);
            if (retried && (retried->declaration != nullptr ||
                            retried->kind != TypeRef::Kind::Named)) {
                anyResolved = true;
            }
        }
        if (!anyResolved) return;
    }
}

void Resolver::resolveTypeDecl(const ast::TypeDecl& td, Scope* parent) {
    Scope typeScope;
    typeScope.parent = parent;
    // Resolve the outer TypeSymbol FIRST so we can attach NestedTypeSymbol
    // pointers when registering nested-type names in typeScope (fix C / #482).
    // Base types. Slice S5: cache the resolved TypeSymbols on the
    // declaring TypeSymbol so member lookup can walk them.
    TypeSymbol* tsym = nullptr;
    if (ctx_.module()) {
        for (Symbol* child : ctx_.module()->children) {
            if (auto* ts = dynamic_cast<TypeSymbol*>(child)) {
                if (ts->decl == &td) { tsym = ts; break; }
            }
        }
    }
    // §11.6 / §5.1 fix: for a nested type, the TypeSymbol lives inside the
    // outer type's member list as a NestedTypeSymbol::type, not in the module
    // children.  When the module-children scan above yields null, recover the
    // TypeSymbol from the enclosing type that is currently being resolved.
    // Without this, tsym stays null for nested types and baseTypes is never
    // populated, causing the §5.1 assignability walk to fail to find any base
    // class or interface — producing a spurious "not implicitly convertible"
    // error for any upcast from a nested class to its base/interface.
    if (!tsym && currentOuterType_) {
        for (Symbol* ms : currentOuterType_->members) {
            if (auto* nts = dynamic_cast<NestedTypeSymbol*>(ms)) {
                if (nts->type && nts->type->decl == &td) {
                    tsym = nts->type;
                    break;
                }
            }
        }
    }
    // §14.1 (fix #5393, v0.314): the names in one type-parameter list are
    // distinct.  Check for duplicate within this list before registering.
    for (std::size_t i = 0; i < td.genericParams.size(); ++i) {
        const auto& g = td.genericParams[i];
        for (std::size_t j = 0; j < i; ++j) {
            if (td.genericParams[j].name == g.name) {
                ctx_.diagnostics().report(
                    Severity::Error, td.span.begin,
                    "duplicate type parameter name `" +
                        std::string(g.name) +
                        "` in this type-parameter list (§14.1, v0.314)");
                break;
            }
        }
    }
    // Register the type's generic params + any nested types so members
    // can reference them.
    for (const auto& g : td.genericParams) {
        typeScope.names[g.name] = nullptr;  // marker for generic-param
    }
    // §11.3 / #482 fix C: register nested-type names with their actual
    // NestedTypeSymbol* (not nullptr). Storing nullptr for nested types
    // collided with the nullptr sentinel for generic params, causing
    // resolveNamed to mislabel a single-part nested-type reference inside
    // the outer class body as "generic param" (declaration=null). By storing
    // the real NestedTypeSymbol*, lookupName returns non-null for nested
    // types and the resolveNamed caller can distinguish the two cases.
    for (const auto& m : td.members) {
        if (auto* nt =
                dynamic_cast<const ast::NestedTypeMember*>(m.get())) {
            if (nt->decl && tsym) {
                // Find the NestedTypeSymbol in the outer type's member list.
                NestedTypeSymbol* nts = nullptr;
                for (Symbol* ms : tsym->members) {
                    if (auto* candidate =
                            dynamic_cast<NestedTypeSymbol*>(ms)) {
                        if (candidate->name == nt->decl->name) {
                            nts = candidate;
                            break;
                        }
                    }
                }
                // Store the NestedTypeSymbol* so resolveNamed can extract
                // ->type. Fall back to nullptr only when the symbol wasn't
                // found (should not happen after slice 1 ran correctly).
                typeScope.names[nt->decl->name] = nts;
            } else if (nt->decl) {
                // tsym not found (should not happen) — fall back to the
                // old nullptr marker so at least the name is visible.
                typeScope.names[nt->decl->name] = nullptr;
            }
        }
    }
    // §14.2: track the type whose body we're resolving, so a deferred
    // generic-param argument inside an inner instantiation (`C<W>`) can be
    // attributed to its owning type (Box) and re-checked at Box's concrete
    // instantiation sites. Save/restore around the body so a nested type
    // restores its enclosing type's value on exit.
    TypeSymbol* savedOuter = currentOuterType_;
    if (tsym) currentOuterType_ = tsym;
    // Slice S10: a class has at most one class-kind base; the
    // rest must be interfaces. Records and enums don't take
    // base classes (we don't enforce here — that's a separate
    // syntactic/structural check).
    std::size_t classBaseCount = 0;
    // §10.3 (fix #4565): "The colon list is one list ... if its first item
    // resolves to one of [the eight underlying-type] types, that item is the
    // underlying type ...; otherwise the underlying type is `int` and every
    // item names an interface." Only the list's FIRST item (index 0) may
    // supply the underlying type, and only when it is one of the eight
    // INTEGER primitive kinds — not any of PrimitiveType's other values
    // (`double`, `float`, `decimal`, `bool`, `char`, `string`, `Object`).
    std::size_t colonIndex = 0;
    auto isEnumUnderlyingPrimitive = [](PrimitiveType p) {
        switch (p) {
            case PrimitiveType::Byte:  case PrimitiveType::SByte:
            case PrimitiveType::Short: case PrimitiveType::UShort:
            case PrimitiveType::Int:   case PrimitiveType::UInt:
            case PrimitiveType::Long:  case PrimitiveType::ULong:
                return true;
            default: return false;
        }
    };
    for (const auto& bt : td.baseTypes) {
        if (!bt) { ++colonIndex; continue; }
        TypeRef* ref = resolveType(*bt, &typeScope);
        // §10.3 (#740, fix #4565): an enum's base type is its underlying
        // integer type (a primitive, e.g. `: long`), which does NOT resolve
        // to a TypeSymbol declaration and so is not a `baseTypes` entry.
        // Record it on the enum's TypeSymbol so codegen lowers the enum to
        // the right integer width. Pre-fix, this fired for ANY colon-list
        // position and ANY Primitive kind — `enum Ratio: double` (`double`
        // is Kind::Primitive but not one of the eight) was silently carried
        // into codegen as the underlying type, and enum equality's
        // unconditional `CreateICmpEQ` then emitted `icmp eq double`, which
        // fails IR verification (#4565's measured repro).
        if (tsym && td.kind == ast::TypeDeclKind::Enum && colonIndex == 0 &&
            ref && ref->kind == TypeRef::Kind::Primitive &&
            isEnumUnderlyingPrimitive(ref->primitive)) {
            tsym->enumUnderlyingType = ref;
        } else if (tsym && td.kind == ast::TypeDeclKind::Enum &&
                   colonIndex == 0 && ref &&
                   ref->kind != TypeRef::Kind::Error &&
                   !(ref->kind == TypeRef::Kind::Named &&
                     (!ref->declaration ||
                      ref->declaration->kind ==
                          ast::TypeDeclKind::Interface))) {
            // The "otherwise" branch's first-item failure (§10.3, v0.306):
            // the head is neither an admissible underlying-type keyword nor
            // an interface (a class, a record, another enum, or a
            // non-integer primitive like `double`). Per the spec's own
            // ruling (changelog: "§10.3's colon-list fallback names the
            // underlying-type fact"), the diagnostic names the
            // underlying-type fact rather than the literally-failing "not
            // an interface" — a conformance case pins the rejection, not
            // the wording (Appendix D).
            std::string spelling = "?";
            if (bt->name && !bt->name->parts.empty())
                spelling = std::string(bt->name->parts.back());
            ctx_.diagnostics().report(
                Severity::Error, bt->span.begin,
                std::string("enum `") + std::string(td.name) +
                "`'s underlying type `" + spelling + "` is not one of the "
                "eight admissible underlying types — byte, sbyte, short, "
                "ushort, int, uint, long, ulong — or their reserved "
                "PascalCase aliases, and is not an interface (§10.3)");
        }
        // §12 / §31.4 (#2950): a BUILT-IN exception base resolves to a Named
        // TypeRef with `declaration == nullptr` — it has no user TypeSymbol —
        // so the `baseTypes`/`baseTypeRefs` push below can never record it and
        // the module metadata built from `baseTypeRefs` never carried it. That
        // is why the §31.4 header is part of the class's LAYOUT but was
        // invisible to every consumer without an AST in hand, i.e. to the whole
        // importing side. Record it here, on the symbol, where local and
        // imported types can be asked the same question. The `!ref->declaration`
        // gate is what keeps a USER class that happens to be named `Exception`
        // (which does resolve to a declaration) from being mistaken for the
        // built-in — the AST-name walks this replaces had no such guard.
        if (tsym && td.kind == ast::TypeDeclKind::Class &&
            tsym->builtinExceptionBase.empty() && bt->name &&
            !bt->name->parts.empty() &&
            !(ref && ref->kind == TypeRef::Kind::Named && ref->declaration)) {
            std::string_view bn = bt->name->parts.back();
            if (common::isBuiltinExceptionName(bn))
                tsym->builtinExceptionBase = std::string(bn);
        }
        // §9.8 / §18.5 / §21.4 / §14.2 (#3139, family F1 of #3152): a declared
        // base that resolves to NO user TypeSymbol cannot ride the parallel
        // `baseTypes`/`baseTypeRefs` pair just below — `baseTypes` has nothing
        // to hold — so the `if (ref->declaration)` guard drops it on the LOCAL
        // side too, and `MetaTypeDecl::baseTypes` (built from `baseTypeRefs`)
        // never carried it either. That is every compiler-provided interface:
        // ISerializable, IDeserializable, IFormattable, IEnumerable<T> and the
        // §4 collection family. Record it on the symbol, in its own list, where
        // a local and an imported type can be asked the same question —
        // exactly what #2950 did for the built-in exception base one block up.
        // See TypeSymbol::nonSymbolBaseRefs for what a wrong answer measured as.
        //
        // The rule is STRUCTURAL — "resolved to something with no user
        // declaration", i.e. precisely the complement of what the guard below
        // keeps — rather than a hand-listed set of built-in names, so the two
        // cannot drift apart. That also means an enum's §10.3 primitive
        // underlying base and (on an already-erroring program) an unresolved
        // base land here; both callers of `nonSymbolBaseRefsOf` treat a
        // non-Named entry as "closure incomplete", which is the behaviour the
        // AST base walks they replace already had.
        if (tsym && ref) {
            bool already = false;
            for (const TypeRef* seen : tsym->nonSymbolBaseRefs)
                if (seen == ref) { already = true; break; }
            if (!already &&
                !(ref->kind == TypeRef::Kind::Named && ref->declaration))
                tsym->nonSymbolBaseRefs.push_back(ref);
        }
        if (tsym && ref && ref->kind == TypeRef::Kind::Named &&
            ref->declaration) {
            tsym->baseTypes.push_back(ref->declaration);
            tsym->baseTypeRefs.push_back(ref);  // preserve generic args
            TypeSymbol* base = ref->declaration;
            // Slice S9: a sealed class cannot be inherited from.
            if (base->kind == ast::TypeDeclKind::Class) {
                ++classBaseCount;
                if (td.kind == ast::TypeDeclKind::Class &&
                    classBaseCount > 1) {
                    ctx_.diagnostics().report(
                        Severity::Error, bt->span.begin,
                        std::string("class `") + std::string(td.name) +
                        "` already has a base class; only the "
                        "first base may be a class (single-"
                        "inheritance — §11.1)");
                }
                bool sealed = false;
                bool baseAbstract = false;
                for (auto mod : base->modifiers) {
                    if (mod == "sealed") sealed = true;
                    else if (mod == "abstract") baseAbstract = true;
                }
                const bool baseClosed = typeIsClosed(base);
                // §11.7 (v0.242, #3052): a `closed` class is a *closed
                // abstract class* — it may be inherited from within its
                // declaring module, but not across module boundaries
                // ("Classes derived from a `closed` base must be declared in
                // the same module; the compiler rejects subclasses in other
                // modules"). This is the spec's recommended closed-hierarchy
                // idiom for exhaustive `case` matching (§19.6), so a
                // same-module subclass must be accepted; only a cross-module
                // subclass is rejected. The current module owns every
                // locally-declared type, so a base flagged `isImported`
                // (reconstructed from a dependency's metadata, §2.5) is from
                // another module; a non-imported base is same-module.
                //
                // Through v0.241 the same semantics were spelled as the
                // modifier PAIR `sealed abstract`; that spelling is now
                // rejected at the declaration (checkClassModifierCombinations),
                // and the pair arm here is kept only so a program that carries
                // both errors reports the inheritance one at the same place it
                // always did rather than silently changing shape.
                //
                // Plain `sealed` (non-abstract, non-`closed`) keeps the §12.4
                // rule: it can never be inherited from, same module or not.
                if (baseClosed) {
                    if (ctx_.isImported(base)) {
                        ctx_.diagnostics().report(
                            Severity::Error, bt->span.begin,
                            std::string("cannot inherit from `closed` class `") +
                            std::string(base->name) +
                            "` declared in another module; a `closed` class "
                            "may be subclassed only within its declaring "
                            "module (§11.7)");
                    }
                    // else: same-module subclass of a `closed` base —
                    // permitted (§11.7).
                } else if (sealed) {
                    if (baseAbstract) {
                        if (ctx_.isImported(base)) {
                            ctx_.diagnostics().report(
                                Severity::Error, bt->span.begin,
                                std::string("cannot inherit from `sealed "
                                            "abstract` class `") +
                                std::string(base->name) +
                                "` declared in another module; a closed "
                                "abstract class may be subclassed only "
                                "within its declaring module (§11.7)");
                        }
                        // else: same-module subclass of a closed abstract
                        // base — permitted (§11.7).
                    } else {
                        ctx_.diagnostics().report(
                            Severity::Error, bt->span.begin,
                            std::string("cannot inherit from sealed class `") +
                            std::string(base->name) + "` (§12.4)");
                    }
                }
            }
        }
        ++colonIndex;
    }
    // Generic constraints.
    for (const auto& gc : td.genericConstraints) {
        for (const auto& item : gc.items) {
            if (item.typeArg) resolveType(*item.typeArg, &typeScope);
        }
    }
    // Positional fields (records).
    for (const auto& pf : td.positionalFields) {
        if (pf.type) resolveType(*pf.type, &typeScope);
        // fix #3742 (§22.5 "Optional parameters and named arguments" /
        // §22.10 "primary constructors", v0.286): the parameter's `:= Expr`
        // DEFAULT is an expression position of the class HEADER, and nothing
        // walked it either — the same gap, the same wild pointer. Measured on
        // f517ddb: `class Holder(a: int, owned r: Res := new Res(77))` with `r`
        // read in a body position SEGV'd on `new Holder(5)`. See the
        // `primaryBaseCtorArgs` loop just below for the whole mechanism; these
        // two are the header's only expression positions and they land
        // together.
        if (pf.defaultValue) resolveTypesInExpr(*pf.defaultValue, &typeScope);
        // fix #4601 (§22.10, v0.315): a function-typed `scoped` primary-ctor
        // parameter is rejected (§28.14 — its capture field would be a `scoped`
        // field, whose obligations §28.14 states over a declared field); a class-
        // or interface-typed `scoped` parameter is admitted and captured as a
        // non-owned private field. The parser removed the v0.180 blanket
        // rejection so class/interface types pass through; this branch checks
        // whether the resolved type is function (reject), class/interface (admit),
        // or other (reject with the standard scoped diagnostic).
        // Records have no modifiers (parsePositionalFieldList) so this branch is
        // a no-op for them.
        // NOTE: checkScopedIsFunctionTyped does the OPPOSITE — it accepts
        // function types — so we inline the type check directly here rather
        // than reuse that function.
        if (td.kind == ast::TypeDeclKind::Class &&
            pf.modifier == "scoped" && pf.type) {
            TypeRef* rt = ctx_.resolvedType(*pf.type);
            if (!rt) rt = resolveType(*pf.type, &typeScope);
            if (rt && rt->kind != TypeRef::Kind::Error) {
                if (rt->kind == TypeRef::Kind::Function) {
                    // Function type: rejected — use a regular constructor.
                    ctx_.diagnostics().report(
                        Severity::Error, pf.loc,
                        std::string("`scoped` is not permitted on a function-typed "
                                    "primary-constructor parameter (§22.10) — the "
                                    "scoped-storage contract (§28.14) needs an "
                                    "explicitly declared field; use a regular "
                                    "constructor instead; parameter `") +
                        std::string(pf.name) + std::string("`"));
                } else if (rt->kind == TypeRef::Kind::Named && rt->declaration &&
                           !rt->declaration->isValueBuiltin &&
                           (rt->declaration->kind == ast::TypeDeclKind::Class ||
                            rt->declaration->kind == ast::TypeDeclKind::Interface)) {
                    // Class or interface type: admitted.
                } else {
                    // Any other type: rejected with the standard diagnostic.
                    ctx_.diagnostics().report(
                        Severity::Error, pf.loc,
                        std::string("`scoped` is valid only on a function-typed, "
                                    "class-typed, or interface-typed parameter "
                                    "(§22.3, §28.14); `") +
                        std::string(pf.name) + "` is none of those");
                }
            }
        }
    }
    // fix #3742 (§22.10 "When a class with a primary constructor has a base
    // class, the base call appears in the inheritance clause" / §22.8 step 1
    // "The base or chained constructor call", v0.286): the inheritance-clause
    // base call's ARGUMENT expressions.
    //
    // An `ast::Type` node NESTED INSIDE an expression is resolved only by
    // `resolveTypesInExpr` — which is exactly why issue #163 added that call
    // for a field initializer one function down (`resolveMember`, whose
    // comment names `new T()` as the reason). Nothing walked
    // `primaryBaseCtorArgs`, so in `class Derived(a: int): Base(new Res(a))`
    // the `Res` type node stayed unresolved; `ExprTC`'s NewExpr arm READS
    // `ctx_.resolvedType(*ne->type)` (it never writes it) and therefore
    // attached no exprType, and codegen's `lowerNew` returns nullptr the
    // moment that read is null. `prePopulateBasePositionalFields` then took
    // its `if (!v) { baseArgVals.clear(); break; }` arm, so the argument was
    // silently NEVER EVALUATED and the base's positional slot kept its calloc
    // zero. Measured on f517ddb: the §11.1 ResourceHolder shape printed
    // `made=0 live=0` (the `new` never ran at all), and the body-referenced
    // shape SEGV'd reading the zero slot as a pointer.
    //
    // Only an argument CARRYING a type node was affected — which is why an
    // arithmetic, literal, interpolated-string or method-call argument of the
    // very same class type always worked (measured: `Base(Fact.Make(a))`
    // returns the right object). The discriminator is the unresolved type
    // node, not the argument's type.
    //
    // `&typeScope` is the scope `ExprTC` already type-checks these same
    // arguments in (ExprTypecheck.cpp's `primaryBaseCtorArgs` visit, #873):
    // a primary-constructor parameter is reachable there as a positional
    // FieldSymbol of the enclosing type.
    for (const auto& arg : td.primaryBaseCtorArgs) {
        if (arg.value) resolveTypesInExpr(*arg.value, &typeScope);
    }
    // §35.1 (v0.200, #2045): choice cases' payload fields — same Param shape
    // as record positional fields. Resolved here so the §35.4 classifier
    // (common/OwnershipFacts.cpp) and downstream §35.5 matching / §35.3
    // construction can read each payload's resolved type through
    // ctx_.resolvedType(*pf.type).
    for (const auto& cc : td.choiceCases) {
        for (const auto& pf : cc.payload) {
            if (pf.type) resolveType(*pf.type, &typeScope);
        }
    }
    // SerializedClassClause polymorphic case types.
    if (td.serializedClass) {
        for (const auto& pc : td.serializedClass->polyCases) {
            if (pc.type) resolveType(*pc.type, &typeScope);
        }
    }
    // Members.
    for (const auto& m : td.members) {
        if (m) resolveMember(*m, &typeScope);
    }
    // Enum constants don't carry types syntactically (their `:= value`
    // is a Const expression; types come from the underlying-int).
    currentOuterType_ = savedOuter;  // §14.2: restore enclosing-type context
}

void Resolver::resolveMember(const ast::Member& m, Scope* parent) {
    // fix #5111 (§14.6, v0.307): each member's body is walked exactly once
    // here (resolveMember is not re-entrant for a single member), so
    // clearing the annotated-locals side table on entry scopes it to THIS
    // member and prevents a local name from one method/operator/constructor
    // leaking into the next one's self-edge lookup. See
    // currentAnnotatedLocals_'s own declaration comment for the full
    // rationale of the table itself.
    currentAnnotatedLocals_.clear();
    if (auto* fd = dynamic_cast<const ast::FieldDecl*>(&m)) {
        if (fd->type) resolveType(*fd->type, parent);
        // per §28.14 (v0.175): a `scoped` FIELD must be function-typed.
        for (std::string_view mod : fd->modifiers) {
            if (mod == "scoped") {
                checkScopedIsFunctionTyped(fd->type.get(), parent, fd->name,
                                           "field", fd->span.begin);
            }
        }
        // §28.5 / §28.1 (v0.300, #4685): validate the declared type of an
        // `owned` FIELD at the declaration site. §28.5 restricts the
        // declared type of an `owned` slot to class, interface, or bare type
        // parameter — runtime-managed reference types (`string`,
        // `Type`, function types) and value types carry no disposal
        // obligation to transfer (§28.9). Two inline-array shapes are
        // distinct cases:
        //   - non-owning `[N]T` / `[N]T?` (no slot marker): slots hold
        //     borrows; the field-level `owned` is meaningless — reject
        //     (the spec calls this "the array's slots hold borrows; there
        //     is no per-slot ownership to manage").
        //   - owning `[N owned]T` / `[N owned]T?`: the type-level `owned`
        //     slot marker already conveys the obligation (the class's
        //     generated Dispose disposes each slot, §28.4 step 2). A
        //     separate field-level `owned` is a double-marking error
        //     (§28.5 bullet 6: "Writing `private owned slots: [N owned]
        //     Connection?;` is rejected as a double-marking error").
        //
        // The equivalent check for parameters/returns lives in
        // checkOwnedDeclType (§28.1 "Type requirement", v0.193). Field
        // declarations were never wired through that check, so the
        // class-side admission silently accepted runtime-managed and
        // value-typed fields (s28_5_0002 string / s28_5_0004 function /
        // s28_5_0006 non-owning inline-array / s28_5_0009 owning inline-
        // array field-modifier cases).
        //
        // Records already have their own restriction at
        // TypeResolution_Checks.cpp's checkRecordRestrictions (~line
        // 2072), which rejects any `owned` field on a record outright
        // before this check would fire — so this branch is class-side.
        bool isOwnedField = false;
        for (std::string_view mod : fd->modifiers) {
            if (mod == "owned") { isOwnedField = true; break; }
        }
        if (isOwnedField && fd->type) {
            const TypeRef* rt = ctx_.resolvedType(*fd->type);
            if (rt) {
                if (rt->kind == TypeRef::Kind::InlineArray) {
                    if (rt->arrayOwned) {
                        // §28.5 bullet 6: owning-inline-array field with the
                        // `owned` field modifier is a double-marking error.
                        // The type-level marker already enrolls the field in
                        // §28.4 step 2's per-slot dispose pass.
                        ctx_.diagnostics().report(
                            Severity::Error, fd->span.begin,
                            std::string("field `") + std::string(fd->name) +
                            "` of class declares both the `owned` field "
                            "modifier and an owning-inline-array type "
                            "`[N owned]T?` — the type-level `owned` slot "
                            "marker already conveys the obligation "
                            "(§28.4 step 2 disposes each non-null slot), so "
                            "the field-level `owned` is a double-marking "
                            "error (§28.5). Drop the field modifier; the "
                            "inline-array's slot marker is the only "
                            "marker the field needs");
                    } else {
                        // §28.5 bullet 5: inline-array without the slot
                        // marker may not be `owned` — slots hold borrows
                        // (or value-typed copies), no per-slot obligation.
                        ctx_.diagnostics().report(
                            Severity::Error, fd->span.begin,
                            std::string("field `") + std::string(fd->name) +
                            "` of class is `owned` but its inline-array "
                            "type `[N]T` / `[N]T?` lacks the `owned` slot "
                            "marker — an inline-array field without the "
                            "slot marker holds borrows in its slots, so "
                            "the field-level `owned` has no slot "
                            "obligation to manage (§28.5). Either add the "
                            "`owned` slot marker to make the slots own "
                            "their contents (`[N owned]T?`), or drop the "
                            "field-level `owned` modifier");
                    }
                } else {
                    // Non-inline-array case: defer to checkOwnedDeclType's
                    // §28.1 type-requirement machinery for `string`,
                    // `Type`, function types, value-type builtins, enum,
                    // record, non-class-or-interface Named, and bare-type-
                    // parameter with a `where T: value` constraint. The
                    // existing check renders a "owned is not admissible on
                    // field `f` of type …" diagnostic citing §28.1.
                    checkOwnedDeclType(
                        rt, fd->span.begin,
                        std::string("field `") + std::string(fd->name) + "`");
                }
            }
        }
        // Issue #163: resolve types embedded in the initializer
        // expression (e.g. `new T()`), so a class-typed field
        // initializer is fully typed — mirrors what ctor/method
        // bodies get via resolveTypesInBody below.
        if (fd->initializer) resolveTypesInExpr(*fd->initializer, parent);
    } else if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
        // §28.1 (v0.196, #1817): expose this method's own `where T: …` clauses
        // to checkOwnedDeclType (below) so it can reject an `owned` marker on a
        // bare type parameter constrained `where T: value`. Save/restore around
        // the whole branch, mirroring currentOuterType_ — a leaked pointer would
        // misattribute these constraints to a later member.
        const std::vector<ast::GenericConstraint>* savedConstraints =
            currentMethodConstraints_;
        currentMethodConstraints_ = &md->genericConstraints;
        // #2713: expose this method's OWN generic parameters for the same
        // save/restore lifetime, so the owning-element obligation can tell a
        // name bound HERE from one bound by the enclosing type it shadows.
        const std::vector<ast::GenericParam>* savedMethodParams =
            currentMethodGenericParams_;
        currentMethodGenericParams_ = &md->genericParams;
        // §14.6 (v0.262, #3271): expose this method's DECLARATION on the same
        // lifetime, so a self-naming generic call in the body below can be
        // attributed to this generic member's own node.
        const ast::MethodDecl* savedMethodDecl = currentMethodDecl_;
        currentMethodDecl_ = md;
        Scope methodScope;
        methodScope.parent = parent;
        for (std::size_t i = 0; i < md->genericParams.size(); ++i) {
            const auto& g = md->genericParams[i];
            // §14.1 / §26.4 / App. C.1 (fix #5393, #5227, #5257, #5264;
            // v0.314): the names in one type-parameter list are distinct. A
            // generic method's type parameter may not reuse the name of a type
            // parameter in scope at its declaration — its enclosing type's or
            // its generic extension block's.  Check for duplicate within this
            // list first.
            for (std::size_t j = 0; j < i; ++j) {
                if (md->genericParams[j].name == g.name) {
                    ctx_.diagnostics().report(
                        Severity::Error, md->span.begin,
                        "duplicate type parameter name `" +
                            std::string(g.name) +
                            "` in this type-parameter list (§14.1, v0.314)");
                    break;
                }
            }
            // Check against enclosing type's generic params (currentOuterType_ is
            // set when inside a type; null for extensions at module scope).
            if (currentOuterType_) {
                for (const Symbol* gp : currentOuterType_->genericParams) {
                    if (gp && gp->name == g.name) {
                        ctx_.diagnostics().report(
                            Severity::Error, md->span.begin,
                            "type parameter `" + std::string(g.name) +
                                "` reuses the name of a type parameter of "
                                "enclosing type `" +
                                std::string(currentOuterType_->name) +
                                "`; rename one of them (§14.1, v0.314)");
                        break;
                    }
                }
            }
            // Also check parent scope's names: for extension methods the extension
            // head's params live there (parent == extScope).  Skip the immediate
            // type's typeScope when currentOuterType_ is set (already checked
            // via currentOuterType_->genericParams) to avoid double-reporting.
            for (Scope* s = parent; s; s = s->parent) {
                if (currentOuterType_ && s == parent) continue;
                auto it = s->names.find(g.name);
                if (it != s->names.end()) {
                    ctx_.diagnostics().report(
                        Severity::Error, md->span.begin,
                        "type parameter `" + std::string(g.name) +
                            "` reuses the name of a type parameter in "
                            "enclosing scope; rename one of them (§14.1, v0.314)");
                    break;
                }
            }
            // Register in methodScope only after all checks.
            methodScope.names[g.name] = nullptr;
        }
        for (const auto& p : md->params) {
            if (p.type) resolveType(*p.type, &methodScope);
            // per §28.14 / §22.3 (v0.175): `scoped` only on function-typed params.
            if (p.modifier == "scoped")
                checkScopedIsFunctionTyped(p.type.get(), &methodScope, p.name,
                                           "parameter", md->span.begin);
            // §28.1 "Type requirement" (v0.193): an `owned` / `out owned`
            // parameter's type must be a class, interface, or bare type
            // parameter (move-only value types admitted-redundant). Read the
            // already-cached resolved type — re-resolving would double-report
            // any function-type-slot error the Function case emits.
            if ((p.modifier == "owned" || p.modifier == "out owned") && p.type)
                checkOwnedDeclType(ctx_.resolvedType(*p.type), md->span.begin,
                                   p.modifier == "out owned"
                                       ? "an `out` parameter"
                                       : "a parameter");
            // per §17.11 (#1166): resolve the inner type nodes of a parameter's
            // default-value expression (e.g. the `T` in `:= default(T)`) so the
            // §17.11 defaultIllFormed well-formedness check in ExprTypecheck can
            // run on it — without resolution `resolvedType(de->type)` is null and
            // an ill-formed `default(R)` param default slips through silently.
            if (p.defaultValue) resolveTypesInExpr(*p.defaultValue, &methodScope);
        }
        // per §28.14 (v0.180, #1606): `scoped` on a static method's parameter
        // requires an `owned` return — otherwise no call site can ever resolve
        // a bounding object for the stored value. AST-only check (isStatic /
        // returnOwned are both parsed flags), so it runs before type
        // resolution needs anything further.
        {
            bool isStatic = false;
            for (std::string_view mod : md->modifiers) {
                if (mod == "static") { isStatic = true; break; }
            }
            checkScopedStaticNeedsOwnedReturn(md->params, isStatic,
                                              md->returnOwned, md->name,
                                              md->span.begin);
        }
        if (md->returnType) resolveType(*md->returnType, &methodScope);
        // §28.1 "Type requirement" (v0.193): an `owned` return type must be a
        // class, interface, or bare type parameter (move-only value types
        // admitted-redundant). Uses the cached resolved return type.
        if (md->returnOwned && md->returnType)
            checkOwnedDeclType(ctx_.resolvedType(*md->returnType),
                               md->span.begin, "a return type");
        // §22.4 (v0.183, #1585): validate any out-nullability postcondition
        // against the resolved return/parameter types.
        checkOutNullPostcond(md->params, md->returnType.get(), &methodScope,
                             md->span.begin);
        for (const auto& gc : md->genericConstraints) {
            for (const auto& item : gc.items) {
                if (item.typeArg) resolveType(*item.typeArg, &methodScope);
            }
        }
        // Slice 3d: also resolve type ascriptions inside the method
        // body (LocalDecl `: Type`, foreach iteration type, …) so
        // ExprTypecheck can pick them up to type identifier uses.
        if (md->bodyKind == ast::MethodBodyKind::Block) {
            resolveTypesInBody(md->stmts, &methodScope);
        } else if (md->bodyKind == ast::MethodBodyKind::Expression &&
                   md->exprBody) {
            // Resolve types embedded in an expression body too (e.g.
            // `=> new T()`), mirroring block bodies — otherwise a
            // factory method's `new T()` has no resolved type.
            resolveTypesInExpr(*md->exprBody, &methodScope);
        }
        // §22.16 (v0.203, #2062): resolve type nodes embedded in each contract
        // clause's expression in the method scope — exactly as the body's
        // types are resolved above. Without this, a clause's `new T(...)`'s
        // `ne->type` is never resolved: the PureChecker's NewExpr admission
        // gate (`rt->declaration->kind == Class`, §22.15/§22.16) reads
        // `ctx_.resolvedType(*ne->type)` and gets null in the clause context,
        // silently admitting class construction that the same expression's
        // body-context arm correctly rejects. The clause expr is visited by
        // ExprTypecheck's `runMethodBody` for bool-typing AFTER this pass; it
        // too relies on the embedded type having been pre-resolved here.
        for (const auto& c : md->contractClauses) {
            if (c.expr) resolveTypesInExpr(*c.expr, &methodScope);
        }
        currentMethodConstraints_ = savedConstraints;  // §28.1 (v0.196, #1817)
        currentMethodGenericParams_ = savedMethodParams;  // #2713
        currentMethodDecl_ = savedMethodDecl;  // §14.6 (v0.262, #3271)
    } else if (auto* od =
                   dynamic_cast<const ast::OperatorDecl*>(&m)) {
        // §28.1 (v0.196, #1817): an operator carries no own generic constraints
        // (OperatorDecl has none); its `where T: value` bound comes from the
        // enclosing type (currentOuterType_). Null this out so a prior method's
        // constraints cannot leak into checkOwnedDeclType for the operator
        // return; restore on exit.
        const std::vector<ast::GenericConstraint>* savedConstraints =
            currentMethodConstraints_;
        currentMethodConstraints_ = nullptr;
        // #2713: an operator declares no generic parameters (OperatorDecl has
        // no genericParams list), so null this for the same reason the
        // constraints are nulled — a prior method's parameters must not leak in
        // and mask an enclosing-type name here.
        const std::vector<ast::GenericParam>* savedMethodParams =
            currentMethodGenericParams_;
        currentMethodGenericParams_ = nullptr;
        // §14.6 (v0.262, #3271): an operator is not a generic member, so no
        // self-edge can be attributed here; null the method decl for exactly
        // the reason the parameter list above is nulled.
        const ast::MethodDecl* savedMethodDecl = currentMethodDecl_;
        currentMethodDecl_ = nullptr;
        Scope methodScope;
        methodScope.parent = parent;
        for (const auto& p : od->params) {
            if (p.type) resolveType(*p.type, &methodScope);
            // per §28.14 / §22.3 (v0.175): `scoped` only on function-typed params.
            if (p.modifier == "scoped")
                checkScopedIsFunctionTyped(p.type.get(), &methodScope, p.name,
                                           "parameter", od->span.begin);
            // per §17.11 (#1166): resolve a parameter default's inner types.
            if (p.defaultValue) resolveTypesInExpr(*p.defaultValue, &methodScope);
        }
        if (od->returnType) resolveType(*od->returnType, &methodScope);
        // §28.1 "Type requirement" (v0.193): an operator's `owned` return type
        // (the only owned-admitting position on an operator — §25.2 rejects
        // `owned` on operator parameters) must be a class, interface, or bare
        // type parameter. Uses the cached resolved return type.
        if (od->returnOwned && od->returnType)
            checkOwnedDeclType(ctx_.resolvedType(*od->returnType),
                               od->span.begin, "a return type");
        // §22.4 (v0.183, #1585): the postcondition is a method-only feature —
        // an operator declaration never admits it (returnType passed as null).
        checkOutNullPostcond(od->params, nullptr, &methodScope, od->span.begin);
        if (od->bodyKind == ast::MethodBodyKind::Block) {
            resolveTypesInBody(od->stmts, &methodScope);
        } else if (od->bodyKind == ast::MethodBodyKind::Expression &&
                   od->exprBody) {
            resolveTypesInExpr(*od->exprBody, &methodScope);
        }
        currentMethodConstraints_ = savedConstraints;  // §28.1 (v0.196, #1817)
        currentMethodGenericParams_ = savedMethodParams;  // #2713
        currentMethodDecl_ = savedMethodDecl;  // §14.6 (v0.262, #3271)
    } else if (auto* cd =
                   dynamic_cast<const ast::ConstructorDecl*>(&m)) {
        Scope ctorScope;
        ctorScope.parent = parent;
        for (const auto& p : cd->params) {
            if (p.type) resolveType(*p.type, &ctorScope);
            // per §28.14 / §22.3 (v0.175): `scoped` only on function-typed params.
            if (p.modifier == "scoped")
                checkScopedIsFunctionTyped(p.type.get(), &ctorScope, p.name,
                                           "parameter", cd->span.begin);
            // per §17.11 (#1166): resolve a parameter default's inner types.
            if (p.defaultValue) resolveTypesInExpr(*p.defaultValue, &ctorScope);
        }
        // §22.4 (v0.183, #1585): a constructor is not a bool-returning method,
        // so any postcondition on its parameters is rejected (null returnType).
        checkOutNullPostcond(cd->params, nullptr, &ctorScope, cd->span.begin);
        resolveTypesInBody(cd->body, &ctorScope);
        // §22.16 (v0.203, #2062): resolve type nodes embedded in each contract
        // clause's expression in the constructor scope — mirrors the method-
        // decl handling above. Without this a clause's `new T(...)` has no
        // resolved type and the PureChecker's NewExpr admission gate
        // (`rt->declaration->kind == Class`, §22.15/§22.16) silently admits
        // class construction that the body-context arm correctly rejects.
        for (const auto& c : cd->contractClauses) {
            if (c.expr) resolveTypesInExpr(*c.expr, &ctorScope);
        }
    } else if (auto* dd =
                   dynamic_cast<const ast::DestructorDecl*>(&m)) {
        resolveTypesInBody(dd->body, parent);
    } else if (auto* pd =
                   dynamic_cast<const ast::PropertyDecl*>(&m)) {
        if (pd->type) resolveType(*pd->type, parent);
        for (const auto& p : pd->indexerParams) {
            if (p.type) resolveType(*p.type, parent);
            // per §17.11 (#1166): resolve an indexer parameter default's types.
            if (p.defaultValue) resolveTypesInExpr(*p.defaultValue, parent);
        }
        // §22.4 (v0.183, #1585): an indexer is not a bool-returning method; a
        // postcondition on an indexer parameter is rejected (null returnType).
        checkOutNullPostcond(pd->indexerParams, nullptr, parent, pd->span.begin);
        // §23.2/§23.3 (FINDING [4]): resolve types embedded in accessor
        // bodies — `new T()`, LocalDecl `: Type`, etc. — exactly as method
        // bodies are resolved above (line 283). Without this a getter/setter
        // body's `new Exception(...)` (the §23.3 throwing-setter idiom) has no
        // resolved type, so it is typed null and codegen elides the
        // construction, leaving the throw with a null exception value.
        Scope propScope;
        propScope.parent = parent;
        // The expression-body property form (`property P: T => expr;`).
        if (pd->bodyKind == ast::PropertyBodyKind::Expression && pd->exprBody)
            resolveTypesInExpr(*pd->exprBody, &propScope);
        // The auto-property initializer (`property P: T := expr;`).
        if (pd->autoInitializer)
            resolveTypesInExpr(*pd->autoInitializer, &propScope);
        // The block-form property accessors (`get => expr;` / `get => do … end;`).
        for (const auto& acc : pd->accessors) {
            if (acc.bodyKind == ast::PropertyAccessorBodyKind::Expression &&
                acc.exprBody)
                resolveTypesInExpr(*acc.exprBody, &propScope);
            else if (acc.bodyKind == ast::PropertyAccessorBodyKind::Block)
                resolveTypesInBody(acc.stmts, &propScope);
        }
    } else if (auto* ed = dynamic_cast<const ast::EventDecl*>(&m)) {
        if (ed->type) resolveType(*ed->type, parent);
    } else if (auto* cd2 = dynamic_cast<const ast::ConstDecl*>(&m)) {
        if (cd2->type) resolveType(*cd2->type, parent);
    } else if (auto* nt =
                   dynamic_cast<const ast::NestedTypeMember*>(&m)) {
        if (nt->decl) resolveTypeDecl(*nt->decl, parent);
    }
}

Symbol* Resolver::lookupName(std::string_view name, Scope* scope) {
    for (Scope* s = scope; s; s = s->parent) {
        auto it = s->names.find(name);
        if (it != s->names.end()) return it->second;
    }
    return nullptr;
}

TypeRef* Resolver::resolveType(const ast::Type& t, Scope* scope) {
    auto* ref = ctx_.makeTypeRef();
    ref->astNode = &t;
    ref->nullable = t.nullable;
    switch (t.kind) {
        case ast::TypeKind::Unit:
            ref->kind = TypeRef::Kind::Unit;
            break;
        case ast::TypeKind::InlineArray:
            ref->kind = TypeRef::Kind::InlineArray;
            ref->arraySize = t.arraySize.get();
            ref->arrayOwned = t.arrayOwned;
            if (t.arrayElement) {
                ref->arrayElement = resolveType(*t.arrayElement, scope);
                // §15.1 (#953): an inline array may not be the element of
                // another inline array (`[N][M]T`). Multi-dimensional inline
                // arrays are deferred ("The current spec admits only
                // one-dimensional inline arrays"). It was silently accepted and
                // literal initialization miscompiled (garbage/OOB reads).
                if (ref->arrayElement &&
                    ref->arrayElement->kind == TypeRef::Kind::InlineArray) {
                    reportNestedInlineArrayError(t.span.begin);
                }
                // fix (§15.1.1 v0.307, rule-corpus case s15_1_1_0002): "The
                // form `[N owned]T` ... T must be a class or interface type;
                // value types and
                // runtime-managed reference types are not admitted with the
                // `owned` marker." No check enforced this at all for a
                // CONCRETE element type (`[3 owned]int`, `[2 owned]Connection`
                // where Connection is a record, …) — it compiled cleanly.
                // Reuse the shared owning-collection element predicate
                // (`owningElementBadDesc`, stdlib §4.2 / spec §14.4): it
                // already classifies Primitive/Function/Unit/InlineArray/
                // Record/Enum/Choice/value-builtin as bad and Class/
                // Interface as admitted, matching this rule's "class or
                // interface" restriction exactly (§14.4's own restricted-
                // list note cites this very rule as its parallel). A BARE
                // GENERIC TYPE PARAMETER also returns "" (admitted) from
                // that predicate — correct here too, but only provisionally:
                // §15.1.1 says the restriction is checked "at instantiation"
                // for that case (paralleling §28.5's generic-owned-field
                // check). fix #4678: record such a site here (mirrors the
                // owning-collection recorder below in resolveNamed, same
                // shape: `owningElementBadDesc` empty + a bare single-part
                // Named element with no declaration and no generic args, not
                // shadowed by the current generic METHOD) so
                // checkOwningInlineArrayElementInstantiations() (Pass 2) can
                // re-run the same predicate against each concrete
                // instantiation of the enclosing generic type, e.g.
                // `Pool<string>` at a field-type mention (#4367's ruling,
                // v0.300: a field-type mention is an instantiation site).
                if (t.arrayOwned) {
                    std::string badDesc = owningElementBadDesc(ref->arrayElement);
                    if (badDesc.empty() && ref->arrayElement &&
                        ref->arrayElement->kind == TypeRef::Kind::Named &&
                        !ref->arrayElement->declaration &&
                        ref->arrayElement->astNode &&
                        ref->arrayElement->astNode->name &&
                        ref->arrayElement->astNode->name->parts.size() == 1 &&
                        ref->arrayElement->genericArgs.empty() &&
                        currentOuterType_) {
                        // Same shadowing guard as the owning-collection
                        // recorder (fix #2742): a generic METHOD's own type
                        // parameter of the same name denotes the method's
                        // binding, not the enclosing type's, and the
                        // method's bindings are not tracked in
                        // instantiations_ at all.
                        std::string_view elName =
                            ref->arrayElement->headName();
                        if (!elName.empty() &&
                            !nameBoundByCurrentMethod(elName))
                            owningInlineArrayElementSites_.push_back(
                                {elName, &t, currentOuterType_});
                    }
                    if (!badDesc.empty()) {
                        ctx_.diagnostics().report(
                            Severity::Error, t.span.begin,
                            std::string("`[N owned]T` requires T to be a "
                                        "class or interface type (\xC2\xA7"
                                        "15.1.1) \xE2\x80\x94 here T is ") +
                            badDesc +
                            ", which has no disposal obligation and cannot "
                            "be owned by an inline array's slots");
                    }
                }
            }
            // §15.1 (issue #734): N must be a compile-time integer expression
            // (integer literals, `const` declarations, and arithmetic on these).
            // Fold N to TypeRef::arraySizeValue so codegen builds a proper
            // `[N x T]` array type rather than a flat scalar (which yields
            // malformed element GEPs). Unfoldable sizes leave the value at -1
            // (codegen falls back to the scalar default).
            if (const ast::Expr* sz = t.arraySize.get()) {
                if (auto v = foldIntConstExpr(*sz, scope, 0)) {
                    // §15.1: "N must be greater than zero; `[0]T` is a compile
                    // error." A zero- (or negative-) sized inline array has no
                    // valid storage; it was silently accepted (a degenerate
                    // `[0 x T]`). Report and leave arraySizeValue unfolded.
                    if (*v <= 0) {
                        ctx_.diagnostics().report(
                            Severity::Error, t.span.begin,
                            "an inline-array size must be greater than zero — "
                            "`[0]T` is not a valid type (§15.1)");
                    } else {
                        ref->arraySizeValue = *v;
                    }
                } else if (auto sf = findConstShiftDomainFaultInScope(*sz,
                                                                      scope);
                           sf.found) {
                    // §6.5 / §17.14 (v0.242, #3045): a size that did not fold
                    // because a constant shift count is outside the left
                    // operand's `0..width−1` domain is an evaluation FAULT —
                    // "a compile error at the constant expression" (§17.14) —
                    // not merely "unfoldable". `[1 << 40]int` used to fold to
                    // `[1 << 8]int` = `[256]int` under the retired masking
                    // rule; leaving it unfoldable would silently degrade the
                    // type to codegen's scalar fallback. Unlike a `const`
                    // initializer or an enum member value, an inline-array
                    // SIZE expression is not visited by ExprTC, so this is the
                    // only place the fault can be reported — no duplicate.
                    ctx_.diagnostics().report(
                        Severity::Error, sz->span.begin,
                        std::string("an inline-array size shifts by ") +
                        std::to_string(sf.count) +
                        ", outside the left operand's `0.." +
                        std::to_string(sf.width - 1) +
                        "` domain — a constant shift count outside the domain "
                        "is a compile error at the constant expression "
                        "(§6.5/§17.14); write `(k & " +
                        std::to_string(sf.width - 1) +
                        ")` if the modular count is intended");
                }
            }
            break;
        case ast::TypeKind::Function:
            ref->kind = TypeRef::Kind::Function;
            ref->funcReturnOwned = t.funcReturnOwned;
            for (const auto& p : t.funcParams) {
                TypeRef::FuncParam fp;
                fp.modifier = p.modifier;
                fp.name = p.name;
                if (p.type) fp.type = resolveType(*p.type, scope);
                // per §28.14 / A.6 FuncParamMods (v0.175): `scoped` is valid
                // only on a function-typed position inside a function type.
                if (fp.modifier == "scoped")
                    checkScopedIsFunctionTyped(p.type.get(), scope, p.name,
                                               "parameter", t.span.begin);
                // §28.1 "Type requirement" (v0.193): an `owned` (or `out
                // owned`) function-type parameter slot must carry a class,
                // interface, or bare-type-parameter type (move-only value
                // types admitted-redundant). `(owned int) -> ()` is rejected.
                if (fp.modifier == "owned" || fp.modifier == "out owned")
                    checkOwnedDeclType(fp.type, t.span.begin,
                                       "a function-type parameter slot");
                ref->funcParams.push_back(std::move(fp));
            }
            if (t.funcReturn) {
                ref->funcReturn = resolveType(*t.funcReturn, scope);
                // §28.1 "Type requirement" (v0.193): an `owned` function-type
                // return slot (`() -> owned T`) has the same type requirement.
                if (ref->funcReturnOwned)
                    checkOwnedDeclType(ref->funcReturn, t.span.begin,
                                       "a function-type return slot");
            }
            break;
        case ast::TypeKind::Named:
            resolveNamed(t, scope, *ref);
            break;
    }
    ctx_.setResolvedType(&t, ref);
    return ref;
}

// §28.14 / §22.3 (v0.175; extended v0.180 #1439/#1545): `scoped` is valid on
// a FUNCTION-typed parameter/field (the original storage-bounded-capture
// form) OR a CLASS- or INTERFACE-typed parameter (v0.180's "scoped
// construction": the callee stores the *borrow* in a direct non-owned field
// instead of storing a callback in a `scoped` field — §28.14 "Class- and
// interface-typed `scoped` parameters"). A value-typed builtin (DateTime,
// TimeSpan, Guid, CancellationToken — TypeDeclKind::Class with
// isValueBuiltin) is excluded: those are copied, not borrowed, so `scoped`
// on one is vacuous. The check runs on the RESOLVED type so a type alias is
// admitted transparently. A nullable function type (`(() -> ())?`) or
// nullable class/interface type (`Stream?`) is admitted — the natural shape
// of a clearable `scoped` field. An unresolved (Error) type stays silent
// (the resolution error was already reported; no cascade).
void Resolver::checkScopedIsFunctionTyped(const ast::Type* declaredType,
                                          Scope* scope, std::string_view name,
                                          std::string_view what,
                                          SourceLocation loc) {
    if (!declaredType) return;
    TypeRef* rt = ctx_.resolvedType(*declaredType);
    if (!rt) rt = resolveType(*declaredType, scope);
    if (!rt || rt->kind == TypeRef::Kind::Error) return;
    if (rt->kind == TypeRef::Kind::Function) return;
    if (rt->kind == TypeRef::Kind::Named && rt->declaration &&
        !rt->declaration->isValueBuiltin &&
        (rt->declaration->kind == ast::TypeDeclKind::Class ||
         rt->declaration->kind == ast::TypeDeclKind::Interface))
        return;
    ctx_.diagnostics().report(
        Severity::Error, loc,
        std::string("`scoped` is valid only on a function-typed, class-"
                     "typed, or interface-typed ") +
        std::string(what) + " (§22.3, §28.14 v0.175/v0.180); `" +
        std::string(name) + "` does not have any of those types");
}

// §28.1 "Type requirement" (v0.193; bare-type-parameter arm v0.196). See the
// declaration in TypeResolution_impl.hpp for the full contract. The
// classification mirrors the owning-collection element-type check in
// resolveNamed (which enforces the same "value types have no disposal
// obligation and cannot be owned" rule for stdlib §4.2 owning collections),
// with one difference: §28.1 ADMITS a move-only value type (an owning inline
// array, §15.3) — the modifier is redundant there but not an error — whereas
// an owning collection rejects an inline-array element outright.
void Resolver::checkOwnedDeclType(const TypeRef* t, SourceLocation loc,
                                  std::string_view positionDesc) {
    if (!t) return;
    // §28.1 (v0.196, #1817): is the bare type parameter `param` constrained
    // `where param: value` — the one bound that excludes class and interface
    // substitutions outright? Consult the current method/operator's own clauses
    // (currentMethodConstraints_) UNION the enclosing type's clauses
    // (currentOuterType_'s TypeDecl). NARROW by design: only the `value`
    // constraint-item kind triggers; other exclusion shapes (a sealed-value
    // bound, an enum bound) are deliberately NOT decided here — under-rejecting
    // is the safe direction (over-rejecting would close valid mixed-domain
    // generics whose class/interface substitutions the marker is meant for).
    auto hasValueConstraint = [&](std::string_view param) -> bool {
        auto scan = [&](const std::vector<ast::GenericConstraint>& gcs) -> bool {
            for (const auto& gc : gcs) {
                if (gc.typeParam != param) continue;
                for (const auto& item : gc.items)
                    if (item.kind == "value") return true;
            }
            return false;
        };
        if (currentMethodConstraints_ && scan(*currentMethodConstraints_))
            return true;
        if (currentOuterType_) {
            if (auto* td = dynamic_cast<const ast::TypeDecl*>(
                    currentOuterType_->decl))
                if (scan(td->genericConstraints)) return true;
        }
        return false;
    };
    std::string badDesc;
    switch (t->kind) {
    case TypeRef::Kind::Error:
        return;  // resolution already failed — suppress cascade diagnostics.
    case TypeRef::Kind::Unit:
        badDesc = "the unit type `()`";
        break;
    case TypeRef::Kind::Function:
        // §28.5 / §28.9: function values are runtime-managed, so a
        // function-typed field carries no disposal obligation to
        // transfer (§28.5 bullet 4). Spec text says "Function-typed
        // fields may not be `owned`" — the "function-typed" wording is
        // what corpus cases (`diagnostic:` substring) pin.
        badDesc = "a function-typed value (a runtime-managed reference type, §28.9)";
        break;
    case TypeRef::Kind::Primitive:
        // `string` is a runtime-managed reference type (§28.9) and every other
        // primitive — including `Object`, which §28.2 excludes from the
        // `IDisposable` contract ("`Dispose()` is not on `Object`") — carries
        // no disposal obligation to transfer, so all are rejected. This
        // matches the owning-collection element-type check, whose "value types
        // have no disposal obligation" reasoning §28.1 explicitly aligns with
        // ("the same reasoning as §28.5's field restriction").
        badDesc = t->primitive == PrimitiveType::String
            ? "`string` (a runtime-managed reference type)"
            : "a primitive value type";
        break;
    case TypeRef::Kind::InlineArray:
        // §15.3: an owning inline array (`[N owned]T` / `[N owned]T?`) is a
        // move-only value type — the modifier is admitted-but-redundant (its
        // own move semantics already transfer the obligation), so accept with
        // no diagnostic. A non-owning inline array is a plain value type and
        // rejects the marker.
        if (t->arrayOwned) return;
        badDesc = "a non-owning inline array (a value type)";
        break;
    case TypeRef::Kind::Named: {
        const TypeSymbol* ts = t->declaration;
        if (ts) {
            if (ts->isValueBuiltin)
                badDesc = "a value-type builtin";
            else if (ts->kind == ast::TypeDeclKind::Record)
                badDesc = "a record (value type)";
            else if (ts->kind == ast::TypeDeclKind::Enum)
                badDesc = "an enum (value type)";
            // Class or Interface → admitted; badDesc stays empty.
        } else {
            // No TypeSymbol: a bare type parameter OR a builtin whose Named
            // TypeRef intentionally carries declaration=null. The value-type
            // builtins (DateTime/Guid/… — isValueBuiltinByName, #1463) reject;
            // the class/interface builtins (Array<T>, Task, …) and bare type
            // parameters are admitted. A bare type parameter is admitted here
            // unless it is constrained `where T: value` (§28.1 v0.196, #1817):
            // that parameter can never be a class or interface, so the marker
            // could never be more than redundant — reject at the declaration,
            // the one decidable point. The per-instantiation erasure of the
            // marker at value/runtime-managed substitutions of an otherwise-
            // unconstrained bare parameter is the rest of #1817, not this check.
            std::string_view nm = t->headName();
            if (isValueBuiltinByName(nm)) {
                badDesc = "a value-type builtin";
            } else if (nm == "Type") {
                // §28.9 (v0.300, #4393): `Type` is runtime-managed — the runtime
                // holds exactly one instance per type for the life of the process,
                // and user code never allocates one, so no holder acquires an
                // obligation. `owned Type` is inadmissible at every position
                // §28.1 lists (and is the named-but-declaration=null member of
                // §28.9's runtime-managed family at this check site; `string` is
                // the other runtime-managed member and is handled in the
                // `Primitive` arm above by `PrimitiveType::String`). The same
                // message the `Primitive`-arm `string` reject emits, framed for
                // `Type` and citing §28.9 alongside §28.1.
                badDesc = "the runtime `Type` token (a runtime-managed "
                          "reference type, §28.9)";
            } else if (hasValueConstraint(nm)) {
                // §28.1 (v0.196): "A declaration whose constraints exclude class
                // and interface substitutions outright (`where T: value`) rejects
                // the marker at the declaration." Emit the bare-parameter-specific
                // diagnostic directly (the generic message below names a bare type
                // parameter as admissible, which would contradict this reject).
                SourceLocation dloc = loc;
                if (t->astNode && t->astNode->span.begin.line != 0)
                    dloc = t->astNode->span.begin;
                ctx_.diagnostics().report(
                    Severity::Error, dloc,
                    "`owned` is not admissible on " + std::string(positionDesc) +
                    " of type a type parameter that can never be a class or "
                    "interface (`where " + std::string(nm) + ": value`) (§28.1)");
                return;
            }
        }
        break;
    }
    }
    if (badDesc.empty()) return;
    // Prefer the offending type's own source span when the parser populated one
    // (a line of 0 means it did not — function-type nodes and some lambda-param
    // annotations carry no span); otherwise fall back to the caller-supplied
    // position (the enclosing method/operator decl).
    if (t->astNode && t->astNode->span.begin.line != 0)
        loc = t->astNode->span.begin;
    ctx_.diagnostics().report(
        Severity::Error, loc,
        "`owned` is not admissible on " + std::string(positionDesc) +
        " of type " + badDesc +
        " — such a value carries no disposal obligation to transfer; `owned` "
        "requires a class type, an interface type, or a bare type parameter "
        "(§28.1)");
}

// §28.14 (v0.180, #1606): a `scoped` parameter on a STATIC method is admitted
// only when the method's return is `owned`, so that a call site can resolve
// bounding form 2 (a `using var` initializer) or form 3 (an `owned` field of
// `this`) for the stored value. A static method with no `owned` return has no
// receiver, no constructed object, and no instance to hold the stored value —
// nothing could ever bound the storage — so declaring `scoped` there is a
// compile error. Non-static methods are unaffected (form 5 resolves the
// receiver as the bounding object). AST-level check: `isStatic` and
// `returnOwned` are both parsed flags, so this needs no type resolution.
void Resolver::checkScopedStaticNeedsOwnedReturn(
    const std::vector<ast::Param>& params, bool isStatic, bool returnOwned,
    std::string_view methodName, SourceLocation loc) {
    if (!isStatic || returnOwned) return;
    for (const auto& p : params) {
        if (p.modifier != "scoped") continue;
        ctx_.diagnostics().report(
            Severity::Error, loc,
            std::string("`scoped` on parameter `") + std::string(p.name) +
            "` of static method `" + std::string(methodName) +
            "` requires the method to have an `owned` return (§28.14 "
            "v0.180); a static method with no `owned` return has no "
            "receiver, constructed object, or instance to bound the stored "
            "value");
        return;  // one diagnostic per method is enough.
    }
}

// §22.4 (v0.183, #1585): validate the placement of a `when true`/`when false`
// out-nullability postcondition. It is admitted only on an `out`/`out owned`
// parameter of nullable type in a bool-returning method; anywhere else is a
// compile error (A.5 OutNullPostcond / §22.4).
void Resolver::checkOutNullPostcond(const std::vector<ast::Param>& params,
                                    const ast::Type* returnType, Scope* scope,
                                    SourceLocation loc) {
    // The enclosing method must return `bool`. `returnType` is null in a
    // non-method context or a void method — in either case the clause is
    // rejected. Resolve leniently: an already-Error return type stays silent.
    TypeRef* rt = nullptr;
    if (returnType) {
        rt = ctx_.resolvedType(*returnType);
        if (!rt) rt = resolveType(*returnType, scope);
    }
    const bool returnsBool = rt && rt->kind == TypeRef::Kind::Primitive &&
                             rt->primitive == PrimitiveType::Bool;
    for (const auto& p : params) {
        if (p.outNullPostcond == ast::OutNullWhen::None) continue;
        const std::string clause =
            p.outNullPostcond == ast::OutNullWhen::WhenTrue ? "when true"
                                                            : "when false";
        // Must be `out` or `out owned`.
        if (p.modifier != "out" && p.modifier != "out owned") {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                "a `" + clause +
                    "` nullability postcondition is admitted only on an `out` "
                    "or `out owned` parameter (§22.4); `" +
                    std::string(p.name) + "` is not an out parameter");
            continue;
        }
        // The enclosing method must return `bool`.
        if (!returnsBool) {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                "a `" + clause +
                    "` nullability postcondition is admitted only in a "
                    "`bool`-returning method (§22.4)");
        }
        // The parameter type must be nullable (the false-return path assigns
        // `null`; the clause narrows away that nullability on the stated
        // return). Skip an unresolved/Error type to avoid cascading.
        TypeRef* pt = p.type ? ctx_.resolvedType(*p.type) : nullptr;
        if (!pt && p.type) pt = resolveType(*p.type, scope);
        if (pt && pt->kind != TypeRef::Kind::Error && !pt->nullable) {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                "a `" + clause +
                    "` nullability postcondition requires a nullable parameter "
                    "type (§22.4); `" +
                    std::string(p.name) + "` is non-nullable");
        }
    }
}

// fix #3619 (§17.14 / #3619, v0.286): the ONE Resolver-side identifier
// resolver for a compile-time integer constant — see the declaration in
// TypeResolution_impl.hpp. A bare name is a `const` whose initializer is
// itself an int-const expression. Member consts live on the enclosing type
// (tracked by currentOuterType_) and its class base chain; module/enclosing-
// scope consts are reached through the lexical `scope`. (Local consts are
// not yet registered at type-resolution time — see resolveTypesInBody — so a
// local-const size is not foldable here; the ExprTypecheck pass folds those,
// where locals are in scope.) Before this fix, `foldIntConstExpr` and
// `findConstShiftDomainFaultInScope` each spelled this closure inline,
// verbatim identical to each other.
std::optional<long long> Resolver::resolveConstIdentInScope(
    std::string_view name, int d, Scope* scope) {
    auto resolve = [this, scope](std::string_view n, int dd) {
        return resolveConstIdentInScope(n, dd, scope);
    };
    for (TypeSymbol* ts = currentOuterType_; ts;) {
        for (Symbol* m : ts->members)
            if (auto* cms = dynamic_cast<ConstMemberSymbol*>(m))
                if (cms->name == name) {
                    // fix #3230 (§17.14 / §15.1 / §22.12; family M11 of
                    // umbrella #3152): an IMPORTED class-level const has
                    // no AST ConstDecl (the symbol is reconstructed from
                    // dependency metadata, fix #328's reconstruction), so
                    // the AST path's `cd->value` is unevaluable and the
                    // fold would silently return nullopt — leaving
                    // `arraySizeValue` at -1, the §5.1 narrowing
                    // unchecked, and the case-pattern range ungated. Fall
                    // back to the typed value the loader populated
                    // (parseImportedConstIntText in ModuleLoader.cpp).
                    // The `if (cd) /` `if (cd->value)` pair is wrapped
                    // here in braces (not as a chained `else if`) so the
                    // `else` binds to the OUTER `if (auto* cd = ...)`,
                    // not to `if (cd->value)` — otherwise the imported
                    // case (`cd == nullptr`) would fall off the chain
                    // with no diagnostic and no fold. // per §17.14
                    if (auto* cd = dynamic_cast<const ast::ConstDecl*>(
                            cms->decl)) {
                        if (cd->value)
                            return foldIntConst(*cd->value, resolve, d + 1);
                    } else if (cms->intValue) {
                        return *cms->intValue;
                    }
                }
        TypeSymbol* nb = nullptr;
        for (TypeSymbol* base : ts->baseTypes)
            if (base && base->kind == ast::TypeDeclKind::Class) {
                nb = base;
                break;
            }
        ts = nb;
    }
    // #4591: AST-ONLY on purpose — unlike the currentOuterType_ walk above,
    // this arm can never observe a `ConstMemberSymbol` at all (imported OR
    // local), so it needs no `cms->intValue` fallback of its own. Per §2.7
    // rule 1 (v0.306), a bare name reaches a `const` ONLY through "the
    // enclosing type's accessible members, inherited included" — exactly the
    // currentOuterType_ + class-base-chain walk above, which #3230 already
    // covers. "The current module's declarations" and "imported bare names"
    // that rule 1 reaches AFTER the enclosing type are never `const`:
    // `ast::ConstDecl` is an A.4 Member production, reachable only inside a
    // type body (A.2's `TopDecl` admits no bare `const`), so `lookupName`
    // walking `scope` here — built from `Scope::names` insertions across
    // TypeResolution_Names.cpp/Checks.cpp — only ever yields a `TypeSymbol` /
    // `TypeAliasSymbol` / `NestedTypeSymbol` / a generic-param `nullptr`
    // marker; grepped exhaustively, no insertion site ever stores a
    // `ConstMemberSymbol`. Verified with a live two-module build (issue
    // #4591): a bare reference to another class's const — same-module
    // unrelated class, nested-type-to-enclosing-type, and cross-module
    // unrelated importing class, local AND imported — is rejected at
    // general expression resolution with "unresolved identifier `K`"
    // *before* this arm would matter, for both a local and an imported `K`.
    // The one live path (inheritance) is arm one, already fixed by #3230.
    if (Symbol* s = lookupName(name, scope))
        if (auto* cd = dynamic_cast<const ast::ConstDecl*>(s->decl))
            if (cd->value)
                return foldIntConst(*cd->value, resolve, d + 1);
    return std::nullopt;
}

std::optional<long long> Resolver::foldIntConstExpr(const ast::Expr& e,
                                                    Scope* scope, int depth) {
    std::function<std::optional<long long>(std::string_view, int)> resolve =
        [this, scope](std::string_view name, int d) {
            return resolveConstIdentInScope(name, d, scope);
        };
    return foldIntConst(e, resolve, depth);
}

// §6.5 / §17.14 (v0.242, #3045) — see the declaration. Shares
// `resolveConstIdentInScope` with `foldIntConstExpr` (fix #3619) so the
// probe and the fold agree on every const value they can see.
ConstShiftDomainFault Resolver::findConstShiftDomainFaultInScope(
    const ast::Expr& e, Scope* scope) {
    std::function<std::optional<long long>(std::string_view, int)> resolve =
        [this, scope](std::string_view name, int d) {
            return resolveConstIdentInScope(name, d, scope);
        };
    return findConstShiftDomainFault(e, resolve);
}

// fix #4188 (§27.8, v0.298): see the declaration comment in
// TypeResolution_impl.hpp. `tr` is a node reachable from a generic alias's
// cached `aliasedType` TypeRef; `subst` maps the alias's OWN generic
// parameter names to the CONCRETE type arguments supplied at a particular
// reference site.
// §15.1 (#953): "an inline array may not be the element of another inline
// array (`[N][M]T`)". Shared text for both the direct-syntax check
// (resolveType's InlineArray case, above) and the alias-substitution
// re-validation (substituteAliasGenericArgs, below) — one spelling, not a
// drifting second copy (LANE_RULES §6).
void Resolver::reportNestedInlineArrayError(SourceLocation loc) {
    ctx_.diagnostics().report(
        Severity::Error, loc,
        "an inline array may not be the element of another "
        "inline array (`[N][M]T`) — multi-dimensional inline "
        "arrays are not admitted (§15.1)");
}

// fix #4212 (Family I1, §14.1 / §27.8, v0.298): thin wrapper over the
// shared `sema::substituteGenericArgs` authority. The unified walker covers
// the same recursive kind set (Named with genericArgs, InlineArray element,
// Function params/return, nullable preservation); the only alias-specific
// behaviour is the §15.1 nested-inline-array re-validation (fix #4280),
// keyed on `validateNestedInlineArrays = true` so the diagnostic still
// reports at the alias reference site, not the alias declaration site.
TypeRef* Resolver::substituteAliasGenericArgs(
        TypeRef* tr,
        const std::unordered_map<std::string_view, TypeRef*>& subst,
        SourceLocation useLoc) {
    return sema::substituteGenericArgs(
        tr,
        sema::SubstArgs{ctx_, subst, useLoc,
                        /*validateNestedInlineArrays=*/true});
}

// stdlib §4.2 / spec §14.4 / §3.6 rule 2 (fix #2735; family F12 of the
// imported-symbol seam #3152) — the CROSS-MODULE half of #2713's
// per-instantiation owning-element sweep.
//
// §14.4 "Class-only restriction on owning generics" (v0.306) restricts the
// owning collections' element type to class and interface types, and §3.6
// rule 2 requires the property to be evaluated "at each instantiation, after
// substituting the type arguments ... rejected *at the instantiation site*".
// #2713 satisfies that by recording an `OwningElementSite` while resolving
// THIS compilation's own type bodies. An IMPORTED generic has no AST here at
// all — `ModuleLoader` builds its TypeRefs directly and `Symbol::decl` stays
// null for every imported symbol — so no site was ever recorded for it and
// `checkOwningElementInstantiations()` had nothing to walk; the defining
// module never instantiates the type, so it could not catch it either.
//
// MEASURED on 6f252b1d: a dependency's `public class Holder<T> where T: class`
// carrying `private owned _l: OwningLinkedList<T>` admitted `Holder<string>`
// in the importer with NO diagnostic at any severity, and the program was a
// `heap-use-after-free` (ASan, READ of size 8 in the importing module's
// method) — NOT the merely-wrong-value the issue reported. The mechanism is
// #2713's: codegen's `elementPolicyFor()` falls back to the Value row for a
// runtime-managed element in the owning family, so the collection stores the
// pointer WITHOUT taking a reference while the `owned` argument transfer still
// releases the caller's, leaving a dangling slot. The `OwningDictionary<K, V>`
// value slot and a method RETURN type (`static Make(owned x: T): owned
// OwningLinkedList<T>`, no field at all) reproduce the identical ASan report.
//
// NO NEW MODULE METADATA IS REQUIRED, and this is the point the issue got
// wrong. #3147 closed the byte-identical cross-module gap in the §28.5
// owned-field check by walking `tsym->members` at the instantiation site
// instead of the declaring type's AST: FieldSymbols are populated on BOTH
// sides, and `sema::storageComponentType` reads the local `ast::FieldDecl` and
// the imported `SemaContext::importedMemberType` in ONE place. That same
// channel already carries everything this rule needs — verified by direct
// instrumentation on the repro: the imported `Holder`'s private `_l` is
// present in `tsym->members` with a Named TypeRef whose `headName()` is
// `OwningLinkedList` and whose slot-0 argument is a declaration-less Named
// TypeRef with `importedName == "T"`, exactly the deferred shape the
// declaration-site recorder records locally. Method return types ride the
// sibling `SemaContext::importedReturnType` table, and parameter types the
// named both-sources accessor `formalParamResolvedType` (#3189's family).
//
// Rather than re-decide the rule here, the walk RECORDS an `OwningElementSite`
// keyed to the imported type, so `checkOwningElementInstantiations()` answers
// it with the same `owningElementBadDesc` predicate, the same chained-binding
// worklist, and the same diagnostic text the local path already emits — the
// #2742 discipline (one predicate, many recorders) applied to a new recorder.
//
// RESIDUALS, deliberately left and recorded rather than guessed at:
//   - A local variable inside an IMPORTED generic method's body. Its type is
//     not part of the exported signature surface at all (the body rides
//     `methodBodyText` and is re-parsed in a FRESH SemaContext, whose
//     obligations never reach this Resolver). MEASURED: that shape compiles
//     and runs CORRECTLY — it is a missing rejection, not a memory-safety
//     hole — so closing it is the part that would genuinely need new
//     metadata, and it stays on #2735.
//   - An obligation reached through an imported INTERMEDIATE generic
//     (`class Outer<U>` in the dependency holding an `Inner<U>` that holds the
//     collection). Pass 2's chained-binding worklist re-keys through
//     `instantiations_`, and no instantiation node exists for a member type
//     the loader built rather than `resolveType`.
//   - A field inherited from an imported BASE class: `tsym->members` is this
//     type's own member list.
void Resolver::recordImportedOwningElementObligations(TypeSymbol* tsym,
                                                      const ast::Type* site) {
    if (!tsym || tsym->genericParams.empty()) return;
    // LOCAL types are already covered by the declaration-site recorder in
    // resolveNamed(), which sees their bodies; running this walk for them too
    // would be redundant (Pass 2's `reported` key would collapse the duplicate)
    // but would also widen a well-tested path for no gain. Imported only.
    if (!ctx_.isImported(tsym)) return;
    if (!importedOwningElementScanned_.insert(tsym).second) return;

    // Record `container<... paramName ...>` when `ft` names an owning
    // collection whose OWNED slot is a bare type parameter of `tsym`.
    auto recordIfOwningElementSlot = [&](const TypeRef* ft) {
        if (!ft || ft->kind != TypeRef::Kind::Named) return;
        std::string_view head = ft->headName();
        std::size_t slot = owningCollectionOwnedSlot(head);
        if (slot == static_cast<std::size_t>(-1)) return;
        if (slot >= ft->genericArgs.size()) return;
        const TypeRef* el = ft->genericArgs[slot];
        // Only a DEFERRED element is recordable. A concrete or composite one
        // was already decided at the exporter's own declaration site (and by
        // `owningElementBadDesc` there), and carries no per-instantiation
        // obligation here.
        if (!el || el->kind != TypeRef::Kind::Named || el->declaration) return;
        if (!el->genericArgs.empty()) return;
        std::string_view elName = el->headName();
        if (elName.empty()) return;
        // The name must be one `tsym` itself binds; match against the type's
        // own parameter list and record THAT view, so Pass 2's
        // `genericParams[i]->name == s.paramName` comparison is over storage
        // with the same (loader-owned) lifetime as the symbol it walks.
        for (const Symbol* gp : tsym->genericParams) {
            if (!gp || gp->name != elName) continue;
            owningElementSites_.push_back({head, gp->name, site, tsym});
            return;
        }
    };

    for (const Symbol* mem : tsym->members) {
        // Fields and properties (including auto-property storage): the
        // #3147 channel, via the ONE accessor that consults both sources.
        if (dynamic_cast<const FieldSymbol*>(mem) ||
            dynamic_cast<const PropertySymbol*>(mem)) {
            recordIfOwningElementSlot(storageComponentType(mem, ctx_));
            continue;
        }
        auto* ms = dynamic_cast<const MethodSymbol*>(mem);
        if (!ms) continue;
        // A generic METHOD's own type parameter is NOT this check's subject:
        // its bindings are calls, which `instantiations_` does not record, and
        // a method parameter may legally SHADOW the type's (fix #2742). The
        // per-parameter name match below is against `tsym->genericParams`
        // only, so a shadowing method parameter is rejected by that test for
        // the same reason `nameBoundByCurrentMethod` rejects it locally — but
        // a NON-shadowing method parameter of the same name would not be
        // distinguishable, so skip a generic method's signature outright.
        // That residual is #2736(a), unchanged by this fix.
        if (!ms->genericParams.empty()) continue;
        recordIfOwningElementSlot(ctx_.importedReturnType(ms));
        // §2.8.2 (#2373 / #3194): an IMPORTED member's parameter carries no
        // syntactic `declType` at all — there is no `ast::Type` to attach —
        // and `ModuleLoader::convertParams` stashes the already-resolved
        // TypeRef on `resolvedDeclType` instead. Reading `declType` alone was
        // MEASURED here to make this arm DEAD for every import: an imported
        // `method Take(l: OwningLinkedList<T>)` still admitted `Taker<string>`
        // with the arm present. That is the exact false ACCEPT #2373 and #3194
        // each had to fix at their own site, so ask the named both-sources
        // accessor (`formalParamResolvedType`, sema/Symbol.hpp) rather than
        // hand-inlining the fallback a fourth time.
        for (std::size_t pi = 0; pi < formalParamCount(*ms); ++pi)
            recordIfOwningElementSlot(formalParamResolvedType(*ms, pi));
    }
}

// fix #4582 (§3.6 rule 2, v0.306) — see the declaration comment in
// TypeResolution_impl.hpp. Extracted VERBATIM out of `resolveNamed`'s
// arity-matched tail so the two spellings that instantiate a generic type
// share ONE recorder: `resolveNamed` (every ast::Type position) and
// `recordStaticReceiverInstantiation` (`G<Arg>.M(…)`, which has no ast::Type
// node). `siteType` is that node when there is one and null otherwise;
// `siteKey`/`loc` carry the site identity and location the node used to
// supply. The §14.2 constraint check stays at the call sites: the type-node
// spelling runs `checkGenericConstraints` (which needs the node), the
// static-receiver spelling already runs `checkGenericConstraintsAtExpr`
// (#3160/#4465) from ExprTypecheck, and running both here would double-report.
void Resolver::recordGenericInstantiation(
    TypeSymbol* tsym, const std::vector<TypeRef*>& argRefs,
    const std::vector<const ast::Type*>& argAsts, const ast::Type* siteType,
    const void* siteKey, SourceLocation loc) {
    // §28.5 (#1107, sweep #9): a generic CLASS with an `owned T` field (T a
    // type parameter) admits T = class/interface only. A `where T: class`
    // constraint over-admits `string` and function types (§14.2), which are
    // runtime-managed (§28.9) and carry no disposal obligation — so binding
    // such a type to the owned field's parameter is a compile error at the
    // instantiation site (paralleling the §14.4 owning-collection element
    // check and the `[N owned]T` rule of spec L1946). Fires for every
    // spelling — typed local / field / parameter / `new` — like §14.4.
    //
    // fix #3147 (§2.8.3 rule 4, v0.252; family F12 of the imported-symbol
    // seam #3152): this walk used to run over the DECLARING type's
    // `ast::TypeDecl` member list. `tsym` here is the type at a USE site, so
    // it is routinely IMPORTED — and `ModuleLoader` never assigns
    // `Symbol::decl` for an imported type, so the whole check was skipped
    // across a module boundary. MEASURED on a0b655e with a real two-module
    // build: `public class HolderO<T> where T: class` carrying
    // `public owned f: T?` in the DEPENDENCY admitted `HolderO<string>` in
    // the importer with NO diagnostic at any severity, while the identical
    // declarations compiled as ONE module produced the §28.5 reject below.
    //
    // The walk is now over `tsym->members` — FieldSymbols, which BOTH sides
    // populate — and the field's declared type comes from
    // `ownedBareTypeParamOfField`, which reads the local `ast::FieldDecl`
    // and the imported `SemaContext::importedMemberType` in ONE place.
    if (tsym->kind == ast::TypeDeclKind::Class) {
        for (const Symbol* mem : tsym->members) {
            auto* fs = dynamic_cast<const FieldSymbol*>(mem);
            if (!fs) continue;
            // The field's type must be a bare type parameter of tsym
            // (`T` or `T?`); a concrete or composite type carries its own
            // §28.5 field check at the declaration.
            std::string_view pn = ownedBareTypeParamOfField(*fs, ctx_);
            if (pn.empty()) continue;
            std::size_t pIdx = tsym->genericParams.size();
            for (std::size_t i = 0; i < tsym->genericParams.size(); ++i)
                if (tsym->genericParams[i] &&
                    tsym->genericParams[i]->name == pn) {
                    pIdx = i;
                    break;
                }
            if (pIdx >= argRefs.size()) continue;
            const TypeRef* arg = argRefs[pIdx];
            const char* badKind = nullptr;
            if (arg && arg->kind == TypeRef::Kind::Primitive &&
                arg->primitive == PrimitiveType::String)
                badKind = "`string`";
            else if (arg && arg->kind == TypeRef::Kind::Function)
                badKind = "a function value";
            if (badKind) {
                ctx_.diagnostics().report(
                    Severity::Error, loc,
                    std::string("`") + std::string(tsym->name) +
                    "` may not be instantiated with " + badKind +
                    " for type parameter `" + std::string(pn) +
                    "`: it is the type of the `owned` field `" +
                    std::string(fs->name) +
                    "`, and only class or interface types may be "
                    "`owned` \xE2\x80\x94 `string` and function values "
                    "are runtime-managed and carry no disposal "
                    "obligation (\xC2\xA7""28.5 / \xC2\xA7""28.9); use a "
                    "class-typed argument.");
            }
        }
    }
    // stdlib §4.2 / spec §14.4 / §3.6 rule 2 (fix #2735; family F12 of the
    // imported-symbol seam #3152): the same cross-module gap #3147 closed
    // for the §28.5 owned-field check, at the §14.4 owning-collection
    // ELEMENT check. An imported generic's obligations are not in
    // `owningElementSites_` because nothing in this compilation ever
    // resolved its body; synthesize them from its reconstructed member
    // surface before the instantiation is recorded just below, so Pass 2
    // sees both halves.
    recordImportedOwningElementObligations(tsym, siteType);
    // §14.2: record this concrete instantiation so a deferred inner
    // obligation owned by `tsym` (recorded while resolving tsym's body)
    // can be re-checked against the binding it supplies here. Recording
    // every well-formed instantiation (not only constrained ones) keeps
    // the final join order-independent: the instantiation site may be
    // visited before tsym's body, so the body's obligations may not yet
    // exist when we get here.
    GenericInstantiation inst;
    inst.type = tsym;
    inst.enclosingType = currentOuterType_;  // body this site sits in
    inst.siteKey = siteKey;
    inst.loc = loc;
    // #2713: record which names the ENCLOSING METHOD binds here, so a
    // chained owning-element obligation re-keying through this site can
    // tell a name bound by the method from one bound by enclosingType.
    inst.methodGenericParams = currentMethodGenericParams_;
    inst.argRefs = argRefs;
    inst.argAsts = argAsts;
    instantiations_.push_back(std::move(inst));
}

// fix #4582 (§3.6 rule 2, v0.306) — the static-member-access receiver half of
// the recorder above. `Fac<string>.Make(…)` parses as a CallExpr whose callee
// is a MemberAccessExpr whose target is an `ast::IdentifierExpr` carrying
// `typeArgs` — there is no `ast::Type` node anywhere on that path, so
// `resolveNamed` is never reached and nothing recorded the instantiation.
// §3.6 rule 2 ("the property is evaluated at EACH instantiation ... rejected
// at the instantiation site") does not distinguish spellings, and §14.4's
// class-only restriction on owning generics is one of the two patterns that
// sentence names, so the record belongs here too.
//
// This is deliberately a RECORD rather than a per-check workaround. #3160 and
// #4465 both reached this same obstacle from the §14.2 side and answered it
// with a temporary `Resolver` (`checkGenericConstraintsAtExpr`), which by
// construction cannot write into the real Resolver's `instantiations_` — and
// `instantiations_` is read by EIGHT independent checks, so one workaround per
// check is how the family acquired its holes. Recording here needs no
// temporary Resolver at all: `resolveTypesInExpr` is already part of the
// `resolveTopDecl` walk, which `Resolver::run` completes before it runs any
// Pass-2 check, so the record is in place for every one of them.
void Resolver::recordStaticReceiverInstantiation(const ast::IdentifierExpr& id,
                                                 Scope* scope) {
    if (id.typeArgs.empty()) return;
    // The spelled name must denote a TYPE. An IdentifierExpr carrying explicit
    // type arguments is either a generic type used as a static-member-access
    // receiver (`Fac<string>.Make(…)`) or a generic METHOD call (`M<int>(…)`);
    // only the first is an instantiation `instantiations_` models at all — a
    // generic method's own bindings are the #2736(a) residual. ExprTypecheck's
    // §14.2 use-site check discriminates with the same `dynamic_cast` (fix
    // #3160), so the two paths agree on which spellings are type receivers.
    // A generic ALIAS receiver is NOT handled here, matching that path.
    auto* tsym = dynamic_cast<TypeSymbol*>(lookupName(id.name, scope));
    if (!tsym) return;
    // Arity mismatch: the same posture as resolveNamed's guard — the mismatch
    // is diagnosed elsewhere and re-checking a misaligned argument list would
    // only add noise.
    if (id.typeArgs.size() != tsym->genericParams.size()) return;
    std::vector<TypeRef*> argRefs;
    std::vector<const ast::Type*> argAsts;
    argRefs.reserve(id.typeArgs.size());
    argAsts.reserve(id.typeArgs.size());
    for (const auto& ta : id.typeArgs) {
        argRefs.push_back(ta ? ctx_.resolvedType(*ta) : nullptr);
        argAsts.push_back(ta.get());
    }
    // No ast::Type node exists for the receiver, so the IdentifierExpr is both
    // the per-site dedup key and the diagnostic anchor. Its span IS stamped
    // (it comes through parseExpression's stampSpan wrap), which is why #3160
    // chose it over the wrapping type path that prints 0:0.
    recordGenericInstantiation(tsym, argRefs, argAsts, /*siteType=*/nullptr,
                               /*siteKey=*/&id, id.span.begin);
}

void Resolver::resolveNamed(const ast::Type& t, Scope* scope, TypeRef& ref) {
    if (!t.name || t.name->parts.empty()) {
        ref.kind = TypeRef::Kind::Error;
        return;
    }
    std::string_view first = t.name->parts.front();
    // fix #4698 (§2.2, v0.313; §2.7 rule 1): `Alias.Builtin` where `Alias` is
    // an unshadowed import alias of the module that declares the compiler-
    // built-in `Builtin` (`Th.CancellationTokenSource` under `import
    // System.Threading as Th;`). Built-ins are not in their module's exported
    // TypeSymbol list, so the alias arm below would reject the name; the
    // qualifier adds nothing ("an additional qualifier, not a rename"), so
    // drop it and resolve the bare name. Idempotent across re-resolution.
    if (t.name->parts.size() >= 2 && !importAliases_.empty()) {
        auto aliasIt = importAliases_.find(std::string(first));
        // §2.7 rule 1 / §2.3: an always-available bare name outranks a
        // same-named alias (the alias is unreachable).
        if (aliasIt != importAliases_.end() && !lookupName(first, scope) &&
            !isAlwaysAvailableBareName(first)) {
            bool isGenericMarker = false;
            for (Scope* s = scope; s; s = s->parent)
                if (s->names.count(first)) { isGenericMarker = true; break; }
            const std::string& modName = aliasIt->second;
            std::string_view cand = t.name->parts[1];
            bool exportedByModule = false;
            if (const auto* exported = ctx_.importedTypesOf(modName))
                for (TypeSymbol* ts : *exported)
                    if (ts->name == cand) { exportedByModule = true; break; }
            if (!isGenericMarker && !exportedByModule &&
                common::builtinTypeHomeModuleOf(cand) == modName) {
                t.name->parts.erase(t.name->parts.begin());
                first = t.name->parts.front();
                // The qualifier is gone from the AST; remember it so this
                // (and any re-resolution) bypasses scope lookup below.
                const_cast<ast::Type&>(t).aliasQualifiedBuiltin = true;
            }
        }
    }
    // fix #4698: an alias-qualified built-in names the built-in by qualifier;
    // a type parameter, module type or imported type of the same bare name
    // must not capture it (§2.2 "the import stays reachable by qualifier").
    const bool qualifiedBuiltin = t.aliasQualifiedBuiltin;
    // Primitive check first.
    if (t.name->parts.size() == 1) {
        PrimitiveType prim = primitiveFromName(first);
        if (prim != PrimitiveType::None) {
            ref.kind = TypeRef::Kind::Primitive;
            ref.primitive = prim;
            // Primitives shouldn't have generic args (sema enforces);
            // we still resolve them so the AST is fully visited.
            for (const auto& ga : t.genericArgs) {
                if (ga) resolveType(*ga, scope);
            }
            if (!t.genericArgs.empty()) {
                ctx_.diagnostics().report(
                    Severity::Error, t.span.begin,
                    "primitive type cannot take generic arguments");
            }
            return;
        }
    }
    // fix #5186 (§8.1 / §3.4, v0.311; stdlib §2.13): the qualified chain
    // head for the two §3.4-reserved primitive types whose module is
    // `System` (`System.Object`, `System.Type`). The bare-name primitive
    // check above only handles single-part spellings; the qualified form
    // is the §8.1 declaration that `Object`'s module is `System`, so a
    // `var o: System.Object := …` must type to the same Primitive as a
    // bare `var o: Object := …`. No `import System;` is needed (the
    // prefix is a §2.7 rule 1 pseudo-module for the two keyword names,
    // not a real user module). The lookup is gated on the EXACT
    // (prefix, member) pair — `System.<other>` would never reach this
    // block, and a user module named `System` would have shadowed the
    // prefix at the §2.7 rule 1 lookup site anyway.
    //
    // The full block is symmetric with the bare-name arm above: a single
    // primitive-kind TypeRef, the generic-args visit, the no-generic-
    // args guard. The qualified spelling participates in §3.4's
    // identical-type rule with the bare spelling: a `Shared<Object>` /
    // `Shared<System.Object>` parameter must accept both forms.
    if (t.name->parts.size() == 2 && first == "System" &&
        (t.name->parts[1] == "Object" || t.name->parts[1] == "Type")) {
        PrimitiveType prim = primitiveFromName(t.name->parts[1]);
        if (prim != PrimitiveType::None) {
            ref.kind = TypeRef::Kind::Primitive;
            ref.primitive = prim;
            for (const auto& ga : t.genericArgs) {
                if (ga) resolveType(*ga, scope);
            }
            if (!t.genericArgs.empty()) {
                ctx_.diagnostics().report(
                    Severity::Error, t.span.begin,
                    "primitive type cannot take generic arguments");
            }
            return;
        }
    }
    // Look up the first part.
    Symbol* sym = qualifiedBuiltin ? nullptr : lookupName(first, scope);
    if (!sym) {
        // Unknown — but a null lookup may also mean "name is in scope as
        // a generic-param marker" (we used nullptr to indicate that).
        // Distinguish by checking if the key exists.
        bool isGeneric = false;
        if (!qualifiedBuiltin)
            for (Scope* s = scope; s; s = s->parent) {
                if (s->names.count(first)) { isGeneric = true; break; }
            }
        if (isGeneric) {
            // Generic parameter — record as a Named with no declaration.
            // Sema slice 3+ will treat generic-param TypeRefs specially.
            ref.kind = TypeRef::Kind::Named;
            ref.declaration = nullptr;
            // fix #3584 (§11.6 / §14.1, v0.286): a nested type declared
            // inside a generic enclosing class (`class Outer<T> { class Tag {
            // _v: T } }`) silently uses the enclosing class's type parameter
            // T. The compiler previously accepted it, but `classStruct`
            // produced an UNSPECIALIZED layout with `_v` lowered to the i32
            // placeholder — every instantiation of `Outer.Tag` (Outer<int>.Tag,
            // Outer<string>.Tag, …) shared the same struct, so the first
            // instantiation's layout was the one every subsequent store
            // corrupted. A `string` payload read back as a ptr-shaped `int`
            // (4423056 in the issue's repro); an `owned` payload would be a
            // use-after-free-class defect rather than a wrong-value bug.
            //
            // Detect the violation here: if the current type is a nested
            // type (its `enclosingTypePath` is non-empty) and the referenced
            // name is NOT one of its OWN generic-parameter names, the name
            // resolves through a parent scope to the enclosing class's
            // parameter — the bug. A method's own generic-parameter names
            // (currentMethodGenericParams_) are allowed so a generic method
            // inside a nested type can still name its own type parameters;
            // naming the enclosing class's `T` from such a method is the
            // same violation and is caught by the same test.
            //
            // The spec is silent on the spelling (§11.6 / §14.1 / §2.8.3
            // rule 3 — A.5 admits no argument list on an enclosing
            // segment), and the only spec-adjacent precedent is the local
            // type's "may not reference an enclosing generic parameter"
            // restriction. Until a per-instantiation spelling lands, a
            // clear diagnostic at the DECLARATION site is the sound fix —
            // the alternative is silent corruption at runtime.
            if (currentOuterType_ &&
                !currentOuterType_->enclosingTypePath.empty()) {
                bool isOwnParam = false;
                for (const sema::Symbol* gp :
                     currentOuterType_->genericParams) {
                    if (gp && gp->name == first) {
                        isOwnParam = true;
                        break;
                    }
                }
                if (!isOwnParam && currentMethodGenericParams_) {
                    for (const auto& gp : *currentMethodGenericParams_) {
                        if (gp.name == first) {
                            isOwnParam = true;
                            break;
                        }
                    }
                }
                if (!isOwnParam) {
                    // Walk the scope chain to find the owning type for the
                    // diagnostic. `scope->names` carries generic-param
                    // markers as nullptr; the scope that owns the marker is
                    // the type whose body introduced it. We don't have a
                    // direct scope→type mapping, so we approximate: the
                    // enclosing type's name is `currentOuterType_->enclosingTypePath`,
                    // which is populated for both local and imported types
                    // (#3529 §2.8.3 rule 3). That path is the LAST segment
                    // of the dotted nesting chain (the immediate parent
                    // type), which is where the binding lives for the
                    // common single-level nesting case. Multi-level
                    // nesting (`A.B.C` referencing `A`'s T) is named by the
                    // same path: it's still the immediate parent that owns
                    // the binding we walked through.
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        std::string("nested type `") +
                            std::string(currentOuterType_->name) +
                            "` references type parameter `" +
                            std::string(first) +
                            "` of its enclosing type `" +
                            std::string(
                                currentOuterType_->enclosingTypePath) +
                            "` \xe2\x80\x94 a nested type may not use "
                            "the enclosing type's type parameters "
                            "(\xc2\xa7""11.6 / \xc2\xa7""14.1, v0.314). "
                            "Declare `Tag` as its own generic "
                            "(`class Tag<U>` and use `U` instead of `T`), "
                            "move `Tag` to module scope, or make the "
                            "enclosing class carry the field instead "
                            "of the nested type");
                    ref.kind = TypeRef::Kind::Error;
                    return;
                }
            }
            return;
        }
        // fix #5172 (§2.2, v0.310): arity selects among rule 1's types.
        // `lookupName` returned the FIRST scope entry for `first`, which
        // honors the §2.2 current-module-shadowing rule for bare (arity=0)
        // references; but for a reference that supplies generic arguments
        // (`t.genericArgs.size() > 0`) the spec requires arity to select
        // among rule 1's types — a local non-generic `Counter` (arity 0)
        // must NOT block a `Counter<int>` from finding an arity-matching
        // `Counter<K>` (arity 1) somewhere in rule 1's candidate set. Walk
        // the imported-types side table (`importedByArity_`, populated in
        // TypeResolution.cpp's collision-tracking loop) for a (name, refArity)
        // match and, if found, override `sym` with it. The bare-name
        // current-module-shadowing behaviour for arity=0 is unchanged
        // (refArity=0 → no lookup, scope stays wins).
        //
        // When the looked-up sym is itself a TypeSymbol whose arity matches,
        // we keep it — the local declaration WINS over the imported one for
        // the same (name, arity) per §2.2's collision rule's mirror. When
        // the looked-up sym is NOT a TypeSymbol (e.g. a LocalSymbol,
        // ParameterSymbol, TypeAliasSymbol), the arity check below would
        // yield a misleading diagnostic; skip the override in that case.
        //
        // The builtin arity table (Slice S24 below) is ALSO a §2.7 rule 1
        // candidate set: a local non-generic `Set` shadows the bare-name
        // scope entry for `Set`, but for `Set<int>` the §2.3 always-
        // available builtin `Set<K>` (arity 1, in `builtinTypeArity` below)
        // matches the reference's arity. The lookup in `importedByArity_`
        // is empty here (the builtin is not in the import-tracked side
        // table), so we additionally fall through to Slice S24 by clearing
        // `sym` — the next block's `if (t.name->parts.size() == 1)` check
        // re-derives the builtin arity and emits the matching `Named` ref.
        {
            std::size_t refArity = t.genericArgs.size();
            if (refArity > 0 && !qualifiedBuiltin) {
                auto* tsymSym = dynamic_cast<TypeSymbol*>(sym);
                bool symArityMatches = tsymSym &&
                                       tsymSym->genericParams.size() == refArity;
                if (!symArityMatches) {
                    auto it = importedByArity_.find(
                        std::make_pair(std::string(first), refArity));
                    if (it != importedByArity_.end() && it->second) {
                        sym = it->second;
                    } else if (t.name->parts.size() == 1) {
                        // Builtin arity table fallback: when no imported
                        // type matches (e.g. local non-generic `Set`
                        // shadowed the auto-imported `Set<K>` in the
                        // bare-name scope), check the §2.3 always-available
                        // builtin list. If the reference's arity matches
                        // the builtin's, the local declaration does NOT
                        // block it — clear `sym` so Slice S24 below
                        // records the builtin Named ref instead of the
                        // arity check (line 4000+ in this file) emitting
                        // "generic-argument count does not match type's
                        // generic-parameter count" against the local
                        // declaration.
                        int builtinAr = builtinTypeArity(first);
                        if (builtinAr != kNotBuiltin &&
                            builtinAr == static_cast<int>(refArity)) {
                            sym = nullptr;
                        }
                    }
                }
            }
        }
        // Slice S24 (§2.3): always-available built-in type names
        // resolve without import. They have no user TypeSymbol, so
        // we record a Named ref with a null declaration and verify
        // generic arity against the spec's declared form.
        if (t.name->parts.size() == 1) {
            // fix #5140 (§2.3, v0.309): the bare always-available name
            // collides with an imported public of the same identifier —
            // emit the §2.3 diagnostic and turn the TypeRef into an
            // Error so downstream passes do not paper over the error
            // with a degraded zero/null value. Each side is reachable
            // by qualification (the always-available one through its
            // §2.7-rule-2 module-qualified form, the imported one
            // through its source module's dotted name or an alias).
            // The check runs BEFORE the `builtinTypeArity` arm because
            // the always-available set is broader than the builtin-type
            // table (e.g. `BitReader` is in `isAlwaysAvailableBareName`
            // but not in `builtinTypeArity` — the spec's §2.3 Row 10
            // whole-stdlib§4 promise).
            // `Counter` (§2.3 Row 10 / stdlib §4.14) is held pending
            // the (identifier, generic arity) keying spec question, so
            // `isAlwaysAvailableBareName` is paired with the TypeRes-
            // olution.cpp gate that excluded `Counter` at population —
            // here we mirror it so the diagnostic never fires for the
            // held rows even if some other path set the flag.
            if (!qualifiedBuiltin &&
                !ambiguousAlwaysAvailableNames_.empty() &&
                ambiguousAlwaysAvailableNames_.count(std::string(first)) &&
                first != "Counter") {
                auto impIt = ambiguousAlwaysAvailableNameImportingModule_
                    .find(std::string(first));
                std::string importingModule =
                    (impIt !=
                     ambiguousAlwaysAvailableNameImportingModule_.end())
                        ? impIt->second : std::string("(unknown module)");
                ctx_.diagnostics().report(
                    Severity::Error, t.span.begin,
                    std::string("bare name `") + std::string(first) +
                    "` is ambiguous: an imported public from `" +
                    importingModule +
                    "` shadows the always-available type; qualify it "
                    "via a module-qualified name (e.g. `" + importingModule +
                    "." + std::string(first) +
                    "`) or an import alias (\xC2\xA7""2.3 / \xC2\xA7""2.2, "
                    "v0.309; fix #5140)");
                ref.kind = TypeRef::Kind::Error;
                return;
            }
            int arity = builtinTypeArity(first);
            if (arity != kNotBuiltin) {
                // §2.15 / issue #631: BinarySerializer / BinaryDeserializer
                // live in System.Serialization and require an explicit
                // `import System.Serialization;` (stdlib §2.15). The
                // compiler previously resolved them unconditionally (like the
                // always-available builtins), which caused silent codegen on
                // no-import code and a segfault at runtime. Emit a compile
                // error when the required import is absent. The TypeRef is
                // still populated so the pass does not cascade further errors.
                if ((first == "BinarySerializer" ||
                     first == "BinaryDeserializer" ||
                     first == "JsonSerializer" ||
                     first == "JsonDeserializer" ||
                     first == "JsonOptions") &&
                    !sourceImports_.count("System.Serialization")) {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        std::string("`") + std::string(first) +
                        "` is declared in `System.Serialization` and "
                        "requires `import System.Serialization;` "
                        "(\xC2\xA7""2.15, #631)");
                }
                // §8 / issue #5034: Stopwatch (§8.1) and Debug (§8.2) live in
                // System.Diagnostics (stdlib §8) and require an explicit
                // `import System.Diagnostics;` (spec §2.3, v0.307: "All other
                // library modules require explicit `import`"). Before this
                // gate the admission table above let both resolve without
                // import, which contradicted §2.3 and silently codegen'd
                // programs with a hidden module dependency. Mirrors the
                // System.Serialization pattern above (issue #631); sibling
                // of #3807 (System.IO / System.Threading, not yet enforced).
                if ((first == "Stopwatch" || first == "Debug") &&
                    !sourceImports_.count("System.Diagnostics")) {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        std::string("`") + std::string(first) +
                        "` is declared in `System.Diagnostics` and "
                        "requires `import System.Diagnostics;` "
                        "(\xC2\xA7""2.3, #5034)");
                }
                ref.kind = TypeRef::Kind::Named;
                ref.declaration = nullptr;
                int n = static_cast<int>(t.genericArgs.size());
                bool ok = (arity == kBuiltinTaskLike) ? (n == 0 || n == 1)
                                                      : (n == arity);
                if (!ok) {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        std::string("generic-argument count does not "
                                    "match built-in type `") +
                        std::string(first) + "`");
                }
                for (const auto& ga : t.genericArgs) {
                    if (!ga) continue;
                    ref.genericArgs.push_back(resolveType(*ga, scope));
                }
                // stdlib §4.2 / §14.4 (#1327, v0.165): owning collection types
                // require a class or interface element type; value types (all
                // primitives, records, enums, value builtins, inline arrays,
                // function values, the unit type) have no disposal obligation
                // and cannot be owned.  Per §3.6 this is a per-instantiation
                // check at every typing site — typed local, field, parameter,
                // return type, and `new` — not only `new`.  Nullable class /
                // interface wrappers are admitted (the `?` does not change the
                // underlying type's class-ness).  The owned slot is `T` (slot 0)
                // for the list/set/stack/queue families and `V` (slot 1, the
                // value) for `OwningDictionary<K, V>` (the key K is value-like
                // by design, §28.11).
                // fix #2735: the slot table lives in ONE place
                // (owningCollectionOwnedSlot above), shared with the
                // cross-module instantiation-site walk.
                std::size_t ownedSlot = owningCollectionOwnedSlot(first);
                if (ownedSlot != static_cast<std::size_t>(-1) &&
                    ownedSlot < ref.genericArgs.size()) {
                    const TypeRef* el = ref.genericArgs[ownedSlot];
                    // #2713: the decision itself lives in ONE place, shared with
                    // the per-instantiation Pass 2 below, so the two sites cannot
                    // give different answers for the same element type.
                    std::string badDesc = owningElementBadDesc(el);
                    // #2713 / §3.6 rule 2: an unresolved generic parameter is
                    // admitted here (owningElementBadDesc returns "") because no
                    // concrete type is known at a declaration site.  Record the
                    // site so checkOwningElementInstantiations() can re-run the
                    // same predicate against the binding each instantiation of
                    // the enclosing generic type supplies.  Only a BARE
                    // single-part parameter name is recordable: a composite or
                    // concrete element (`OwningArray<Foo>`,
                    // `OwningLinkedList<OwningArray<C>>`) was already decided
                    // above and carries no deferred obligation.
                    if (badDesc.empty() && el &&
                        el->kind == TypeRef::Kind::Named && !el->declaration &&
                        el->astNode && el->astNode->name &&
                        el->astNode->name->parts.size() == 1 &&
                        el->genericArgs.empty() && currentOuterType_) {
                        // #2713: the name must be one the ENCLOSING TYPE binds.
                        // A generic method may declare a parameter of the same
                        // name, SHADOWING the type's — `class Box<T>` with
                        // `method M<T>()` — and nothing forbids it (Appendix
                        // A.3's `GenericParam ::= Variance? Identifier` imposes
                        // no uniqueness; there is no shadowing diagnostic in
                        // sema or the parser). Inside such a method the name
                        // denotes the METHOD's parameter, whose binding the
                        // enclosing type's instantiations know nothing about,
                        // so recording it would validate the method's `T`
                        // against `Box`'s and FALSELY REJECT a legal program.
                        // A method parameter that does not shadow is skipped by
                        // the same test, which is correct for the same reason:
                        // generic-method bindings are not in instantiations_ at
                        // all. That residual is filed as #2736(a); a false
                        // reject would be strictly worse than carrying it.
                        //
                        // fix #2742: the test itself now lives in ONE place
                        // (Resolver::nameBoundByCurrentMethod) — it had been
                        // hand-inlined here and at two more recorders while two
                        // OTHER recorders lacked it entirely.
                        if (!nameBoundByCurrentMethod(el->headName()))
                            owningElementSites_.push_back(
                                {first, el->headName(), &t, currentOuterType_});
                    }
                    if (!badDesc.empty()) {
                        ctx_.diagnostics().report(
                            Severity::Error, t.span.begin,
                            std::string("`") + std::string(first) +
                            "` requires a class or interface element type "
                            "(stdlib \xC2\xA7""4.2 / \xC2\xA7""14.4, #1327) "
                            "\xE2\x80\x94 " + badDesc + " has no disposal "
                            "obligation and cannot be owned in an owning "
                            "collection. Use `" +
                            // v0.168 (#1372): strip the 6-char `Owning` prefix
                            // (was 5-char `Owned`) to name the borrowing
                            // counterpart (e.g. OwningArray → Array).
                            std::string(first.substr(6)) +
                            "<\xE2\x80\xA6>` for value elements.");
                    }
                }
                // §14.2/§14.4 (fix #4305, SHARPENED by send-back review): a
                // null-declaration builtin generic (this whole
                // `if (arity != kNotBuiltin)` branch — no synthesized
                // TypeSymbol) has no `tsym` for `checkGenericConstraints`
                // below (its `if (tsym && ...)` gate) to run against, so a
                // `where <slot>: class` constraint declared on the type's OWN
                // stdlib signature was never verified for any of them.
                // Measured (issue #4305): `default(DrainedEntry<int,
                // SomeRecord>)` compiled and ran clean despite `DrainedEntry
                // <K, V>: IDisposable where V: class` (stdlib §4.9).
                //
                // Spec §14.4's "Class-only restriction on owning generics"
                // paragraph (v0.298; heading "`owned` and generic type
                // arguments" > "Generic constraints and class types" — NOT
                // §14.2, which only defines the general `where T: class`
                // meaning) names THIRTEEN types under one sentence: "Most
                // standard-library owning types (`OwningArray<T>`,
                // `OwningLinkedList<T>`, `OwningSet<T>`, `OwningStack<T>`,
                // `OwningQueue<T>`, `OwningDictionary<K, V>`, `OwningTask<T>`,
                // `IOwningEnumerable<T>`, `IOwningEnumerator<T>`,
                // `IOwningAsyncEnumerable<T>`, `IOwningAsyncEnumerator<T>`,
                // `IOwningCollection<T>`, `IOwningList<T>`) restrict their
                // element type T to class and interface types. ... the
                // standard-library owning types are compiler-enforced to
                // reject the runtime-managed reference types (`string`,
                // function values)." That is STRICTER than the general
                // `where T: class` (§14.2: "Non-nullable reference types
                // only — classes ..., interfaces, `string`, function
                // types" — string/function ARE admitted generically) and
                // the very next spec sentence adds a second divergence:
                // "Nullable class types and interfaces are admitted. The
                // owning-type instantiation rule checks the *underlying*
                // type, not the nullable-wrapper" — the opposite of the
                // general `where T: class`, which requires non-null T.
                //
                // This table covers every null-declaration builtin generic
                // whose stdlib declaration carries a plain `where <slot>:
                // class` constraint AND is not already enforced by a
                // different, purpose-built check. The candidate set was
                // derived MECHANICALLY (send-back review of the original
                // fix, which re-read the doc by eye and missed two): every
                // arity>=1 name in `builtinTypeArity` above was
                // cross-referenced against every TYPE-LEVEL (not
                // method-level) `where <slot>: ...` declaration clause in
                // docs/Bezel_Standard_Library.md. That sweep added exactly
                // two names beyond the eight below: `IOwningCollection<T>`
                // and `IOwningList<T>` (stdlib §4.2, arity 1, slot 0) — both
                // named in the identical §14.4 sentence quoted above as
                // their four already-covered `IOwning*Enumerable/Enumerator`
                // siblings, in neither this table nor the `ownedSlot`
                // exclusion below. No other name fell out of the sweep:
                //   - OwningArray/OwningSet/OwningStack/OwningQueue/
                //     OwningLinkedList (slot 0) and OwningDictionary (slot 1)
                //     are DELIBERATELY EXCLUDED: the `ownedSlot` /
                //     `owningElementBadDesc` check immediately above (stdlib
                //     §4.2/§14.4, #1327) already enforces their identical
                //     requirement.
                //   - `OwningTask<T>` is in `builtinTypeArity` but has no
                //     standalone type-level `where` clause anywhere in the
                //     stdlib doc to register — every `where T: class` hit
                //     naming it (`Task.FromOwnedResult<T>`, `Task.WhenAll<T>`)
                //     is a constraint on that STATIC METHOD's own T, checked
                //     via the method's real TypeSymbol/genericParams, not
                //     this null-declaration-TYPE gap.
                //   - IComparable/IEquatable/IEqualityComparer/IReadOnlySpan/
                //     INumber/ISteppable/IParsable (the latter three declare
                //     a self-referential `where T: <IName><T>` type-bound,
                //     not a `class`-kind constraint, on their OWN type
                //     parameter) get a REAL synthesized TypeSymbol
                //     (`SemaContext::isSynthesizedBuiltinInterfaceName`,
                //     `ctx_.builtinInterfaceSym` a few lines below) — `tsym`
                //     is non-null for them, so `checkGenericConstraints`'s
                //     `if (tsym && ...)` gate below already runs for them;
                //     they were never part of the null-declaration
                //     population this fix patches.
                //   - Atomic/AtomicInteger (`where T: value`), Channel/
                //     ChannelSender (`where T: sendable`), and
                //     SortedDictionary/SortedSet (`where K/T:
                //     IComparable<K/T>`, a self-referential type bound) are a
                //     DIFFERENT constraint kind each — not `class` — and are
                //     out of scope for this table; measured still-open and
                //     filed separately (issue #4318).
                //
                // Reuses `owningElementBadDesc` (TypeResolution_Checks.cpp,
                // NOT modified by this fix — held this cycle) directly,
                // rather than `checkOneConstraint`'s `kind="class"` arm the
                // original fix used: `owningElementBadDesc` is the SAME
                // authority the `ownedSlot` check immediately above calls,
                // and §14.4 enumerates this table's names in the identical
                // sentence as the six `ownedSlot` covers — provably the same
                // question (§6/§25), not a predicate reused on a hunch.
                // `checkOneConstraint`'s generic `"class"` arm was measured
                // to answer that question WRONG on both divergences quoted
                // above: (a) it calls `categorize()`, whose Primitive/
                // Function arms classify `string`/`Function` as `Reference`
                // — not `Value` — so it never rejects them (measured:
                // `IOwningEnumerable<string>` compiled clean on the
                // original fix's binary); (b) it unconditionally rejects
                // `arg->nullable`, the OPPOSITE of §14.4's nullable-class-
                // admitted carve-out. `owningElementBadDesc` gets both
                // right already: it never inspects `->nullable` (a value
                // kind is bad whether or not it is nullable; a class/
                // interface kind is fine either way), and its Primitive/
                // Function arms reject `string`/function outright. The
                // deferred-generic-parameter behaviour is unchanged: an
                // unresolved type parameter reaches `owningElementBadDesc`'s
                // same "no declaration, not a value builtin" fallthrough
                // that returns "" (admitted/deferred), matching
                // `checkOneConstraint`'s `categorize() == Unknown` defer.
                {
                    struct NullDeclClassConstraint {
                        std::string_view name;
                        std::size_t slot;
                        std::string_view paramName;
                    };
                    static constexpr NullDeclClassConstraint
                        kNullDeclClassConstraints[] = {
                            {"DrainedEntry", 1, "V"},               // §4.9
                            {"Shared", 0, "T"},                     // §2.16
                            {"Weak", 0, "T"},                       // §2.16
                            {"IOwningEnumerable", 0, "T"},          // §4.13
                            {"IOwningEnumerator", 0, "T"},          // §4.13
                            {"IOwningAsyncEnumerable", 0, "T"},     // §4.13
                            {"IOwningAsyncEnumerator", 0, "T"},     // §4.13
                            {"OwningTaskCompletionSource", 0, "T"}, // §6.6
                            {"IOwningCollection", 0, "T"},          // §4.2
                            {"IOwningList", 0, "T"},                // §4.2
                        };
                    for (const auto& c : kNullDeclClassConstraints) {
                        if (first != c.name) continue;
                        // §send-back-#4305 fold-in: the table is hand-typed
                        // and the bounds check below (`c.slot <
                        // ref.genericArgs.size()`) would silently stop
                        // firing — no crash, no failing test — if a listed
                        // name's arity ever changed underneath it. Validate
                        // against the SAME authority (`builtinTypeArity`)
                        // every time this entry matches, so a future arity
                        // change fails loudly the first time any program
                        // (including this fix's own regression tests, which
                        // exercise all ten entries) uses the name.
                        // Every entry's declared arity must be a plain
                        // positive count (never `kNotBuiltin` == -1 or
                        // `kBuiltinTaskLike` == -2 — neither sentinel is
                        // meaningful for a name this table expects to carry
                        // a fixed `where <slot>: class` clause), and the
                        // slot must fall inside it.
                        int declaredArity = builtinTypeArity(c.name);
                        bool slotInRange =
                            declaredArity > 0 &&
                            c.slot < static_cast<std::size_t>(declaredArity);
                        if (!slotInRange) {
                            unhandledNullDeclClassConstraintSlot(
                                c.name, c.slot, declaredArity);
                        }
                        if (c.slot < ref.genericArgs.size() &&
                            c.slot < t.genericArgs.size()) {
                            TypeRef* carg = ref.genericArgs[c.slot];
                            std::string badDesc = owningElementBadDesc(carg);
                            if (!badDesc.empty()) {
                                // Render the WRITTEN argument spelling for
                                // the diagnostic. Mirrors argSpelling()
                                // (TypeResolution_Checks.cpp, anonymous
                                // namespace — not externally callable, and
                                // that file is held this cycle) at a scale
                                // too small to justify exposing it: this is
                                // string rendering for a message, not a
                                // second spelling of a classification
                                // decision (§6).
                                const ast::Type* argAst =
                                    t.genericArgs[c.slot].get();
                                std::string argName = "<type>";
                                if (argAst &&
                                    argAst->kind == ast::TypeKind::Named &&
                                    argAst->name) {
                                    argName.clear();
                                    for (std::size_t i = 0;
                                         i < argAst->name->parts.size();
                                         ++i) {
                                        if (i) argName += '.';
                                        argName += std::string(
                                            argAst->name->parts[i]);
                                    }
                                    if (argAst->nullable) argName += '?';
                                }
                                ctx_.diagnostics().report(
                                    Severity::Error, t.span.begin,
                                    std::string("type argument `") +
                                    argName +
                                    "` does not satisfy the constraint "
                                    "`where " + std::string(c.paramName) +
                                    ": class` (\xC2\xA7""14.2) \xE2\x80\x94 " +
                                    badDesc + " has no disposal obligation "
                                    "and cannot satisfy `" +
                                    std::string(c.name) + "<" +
                                    std::string(c.paramName) +
                                    ">`'s class-only owning-generic "
                                    "restriction (\xC2\xA7""14.4).");
                            }
                        }
                        break;
                    }
                }
                // fix #4318 (§14.2, v0.315): three more constraint kinds for
                // null-declaration builtin generics that #4305 deliberately left
                // out (different handling required):
                //
                // 1. `value` (Atomic/AtomicInteger): call checkOneConstraint with
                //    a synthesized GenericParamConstraint{kind="value"}.
                //
                // 2. `sendable` (Channel/ChannelSender): defer to
                //    checkSendableConstraintsLate via pendingSendableConstraints,
                //    exactly as checkOneConstraint's sendable arm does.
                //
                // 3. Self-referential `IComparable<K>` type bound
                //    (SortedDictionary/SortedSet): checkOneConstraint's `type`
                //    arm can't handle this (needs astBound/importedBound we lack);
                //    synthesize the IComparable bound with a bare parameter marker
                //    and call argSatisfiesTypeBound directly.
                //
                // Value-constraint table (§6.4).
                {
                    struct NullDeclValueConstraint {
                        std::string_view name;
                        std::size_t slot;
                        std::string_view paramName;
                    };
                    static constexpr NullDeclValueConstraint
                        kNullDeclValueConstraints[] = {
                            {"Atomic", 0, "T"},        // §6.4
                            {"AtomicInteger", 0, "T"}, // §6.4
                        };
                    for (const auto& c : kNullDeclValueConstraints) {
                        if (first != c.name) continue;
                        int declaredArity = builtinTypeArity(c.name);
                        bool slotInRange =
                            declaredArity > 0 &&
                            c.slot < static_cast<std::size_t>(declaredArity);
                        if (!slotInRange) {
                            // Cannot happen: builtinTypeArity is the authority.
                            break;
                        }
                        if (c.slot < ref.genericArgs.size() &&
                            c.slot < t.genericArgs.size()) {
                            TypeRef* arg = ref.genericArgs[c.slot];
                            const ast::Type* argAst = t.genericArgs[c.slot].get();
                            GenericParamConstraint item;
                            item.kind = "value";
                            checkOneConstraint(t.span.begin, c.paramName,
                                               item, argAst, arg, nullptr);
                        }
                        break;
                    }
                }
                // Sendable-constraint table (§6.9/§30.15).
                {
                    struct NullDeclSendableConstraint {
                        std::string_view name;
                        std::size_t slot;
                        std::string_view paramName;
                    };
                    static constexpr NullDeclSendableConstraint
                        kNullDeclSendableConstraints[] = {
                            {"Channel", 0, "T"},         // §30.15
                            {"ChannelSender", 0, "T"},   // §30.15
                        };
                    for (const auto& c : kNullDeclSendableConstraints) {
                        if (first != c.name) continue;
                        int declaredArity = builtinTypeArity(c.name);
                        bool slotInRange =
                            declaredArity > 0 &&
                            c.slot < static_cast<std::size_t>(declaredArity);
                        if (!slotInRange) {
                            // Cannot happen: builtinTypeArity is the authority.
                            break;
                        }
                        if (c.slot < ref.genericArgs.size()) {
                            TypeRef* arg = ref.genericArgs[c.slot];
                            // argName is only used in diagnostics; the late pass
                            // checkSendableConstraintsLate produces the specific
                            // message. argSpelling (TypeResolution_Checks.cpp) also
                            // returns "<type>" for a resolved TypeRef with no AST
                            // node, so use that for consistency.
                            std::string argName = "<type>";
                            if (arg && arg->kind == TypeRef::Kind::Named) {
                                argName = std::string(arg->headName());
                            }
                            ctx_.addPendingSendableConstraint(
                                SemaContext::PendingSendableConstraint{
                                    t.span.begin,
                                    std::string(c.paramName),
                                    argName,
                                    arg});
                        }
                        break;
                    }
                }
                // Self-referential IComparable-bound table (§4.17/§4.18).
                {
                    struct NullDeclSelfRefConstraint {
                        std::string_view name;
                        std::size_t slot;
                        std::string_view paramName;
                    };
                    static constexpr NullDeclSelfRefConstraint
                        kNullDeclSelfRefConstraints[] = {
                            {"SortedDictionary", 0, "K"}, // §4.17
                            {"SortedSet", 0, "T"},       // §4.18
                        };
                    for (const auto& c : kNullDeclSelfRefConstraints) {
                        if (first != c.name) continue;
                        int declaredArity = builtinTypeArity(c.name);
                        bool slotInRange =
                            declaredArity > 0 &&
                            c.slot < static_cast<std::size_t>(declaredArity);
                        if (!slotInRange) {
                            // Cannot happen: builtinTypeArity is the authority.
                            break;
                        }
                        if (c.slot < ref.genericArgs.size() &&
                            c.slot < t.genericArgs.size()) {
                            TypeRef* arg = ref.genericArgs[c.slot];
                            // Synthesize IComparable bound: build a TypeRef for
                            // IComparable<K> where K is a bare parameter marker.
                            // A bare marker is Named with declaration=nullptr;
                            // argSatisfiesTypeBound identifies it by kind+decl,
                            // not by name.
                            //
                            // fix-forward #5424: both TypeRefs are allocated
                            // from the context, NOT on the stack. The Named
                            // arm DEFERS the failure below (the key class may
                            // be declared after this use, so its base list is
                            // still empty here), and the deferred entry keeps a
                            // raw `bound` pointer that recheckDeferredTypeBounds
                            // dereferences after this frame is long gone. Stack
                            // storage made that a dangling read: the recheck saw
                            // a recycled stack slot, so `genericArgs` came back
                            // as garbage (MEASURED: 1 element at defer time,
                            // 17590550462694 at recheck) and the recheck
                            // reported the very §14.2 violation the deferral
                            // exists to re-adjudicate — the declaration-order
                            // dependence #5424 names. makeTypeRef storage is
                            // stable for the context's lifetime, which is what
                            // DeferredTypeBoundFailure's `arg`/`bound` pointers
                            // already assume.
                            TypeRef* bareMarker = ctx_.makeTypeRef();
                            bareMarker->kind = TypeRef::Kind::Named;
                            bareMarker->declaration = nullptr;
                            TypeRef* iComparableBound = ctx_.makeTypeRef();
                            iComparableBound->kind = TypeRef::Kind::Named;
                            iComparableBound->declaration =
                                ctx_.builtinInterfaceSym("IComparable");
                            iComparableBound->genericArgs.push_back(
                                bareMarker);
                            std::string argName = "<type>";
                            if (arg && arg->kind == TypeRef::Kind::Named) {
                                argName = std::string(arg->headName());
                            }
                            // For primitives: use the same authority as
                            // checkOneConstraint's type arm
                            // (primitiveConformsToBuiltinInstanceIface).
                            // For Named types: use argSatisfiesTypeBound.
                            if (arg && arg->kind == TypeRef::Kind::Primitive) {
                                if (primitiveConformsToBuiltinInstanceIface(
                                        arg->primitive, "IComparable",
                                        iComparableBound->genericArgs) ==
                                    PrimBuiltinIfaceConformance::Rejects) {
                                    ctx_.diagnostics().report(
                                        Severity::Error, t.span.begin,
                                        std::string("type argument `") +
                                        argName +
                                        "` does not satisfy the constraint "
                                        "`where " +
                                        std::string(c.paramName) +
                                        ": IComparable<" +
                                        std::string(c.paramName) +
                                        ">` (§14.2)");
                                }
                            } else if (arg && arg->declaration) {
                                if (!argSatisfiesTypeBound(
                                        arg, iComparableBound, &ctx_)) {
                                    // Defer: same as checkOneConstraint's type
                                    // arm for unresolved base lists.
                                    // per spec §4.18 / §14.2 (v0.315): spell the
                                    // bound as IComparable<paramName>, not the bare
                                    // "IComparable" (issue #5423).
                                    deferredTypeBoundFailures_.push_back(
                                        DeferredTypeBoundFailure{
                                            t.span.begin,
                                            std::string(c.paramName),
                                            "IComparable<" + std::string(c.paramName) + ">",
                                            argName,
                                            arg,
                                            iComparableBound});
                                }
                            }
                        }
                        break;
                    }
                }
                // §3.6 / §8.3 (v0.158, #1338): record a hash-container key /
                // element instantiation (`Dictionary`/`OwningDictionary` key K,
                // `Set`/`OwningSet` element T) so checkHashContainerKeys can
                // verify — after all member types are resolved — that it is
                // hashable. Deferred (not checked inline) because a
                // record key's auto-hashable property depends on the
                // record's own members, which may resolve after this site.
                std::string_view hashRole;
                if (first == "Set" || first == "OwningSet")
                    hashRole = "element";
                else if (first == "Dictionary" || first == "OwningDictionary")
                    hashRole = "key";
                // fix #2742 (§3.6 rule 2 / §14.1, v0.306): apply the #2713
                // MISATTRIBUTION guard here too. `checkHashContainerKeys`'
                // Pass 2 recovers this site's bare parameter NAME and resolves
                // it against `s.enclosingType->genericParams`; when a generic
                // METHOD shadows an enclosing type parameter
                // (`class Box<T>` + `method M<T>()`, which nothing forbids),
                // the recorded `T` is the METHOD's and the check runs against
                // `Box`'s — a different binding. MEASURED as a false reject:
                // `Box2742<NoEq2742>` calling `M2742<int>()` over a body
                // holding `new Set<T>()` was rejected naming `NoEq2742`, while
                // the `Set`'s actual element type is the method's `int`.
                // (The reverse direction — an ill-formed method binding
                // admitted because the enclosing one is fine — is the same
                // misattribution and is closed by the same guard; the residual
                // "generic-method bindings are never re-checked at all" is
                // #2736(a), and a false reject is strictly worse than carrying
                // it, per #2713's own recorded trade-off.)
                if (!hashRole.empty() && !ref.genericArgs.empty() &&
                    !(isBareGenericParamBinding(
                          ref.genericArgs[0],
                          ref.genericArgs[0] ? ref.genericArgs[0]->astNode
                                             : nullptr) &&
                      nameBoundByCurrentMethod(ref.genericArgs[0]->headName())))
                    hashContainerSites_.push_back(
                        {first, hashRole, ref.genericArgs[0], &t,
                         currentOuterType_});  // #1384: for per-instantiation check
                // §28.11 / §15.3 (#3775): record every element-category slot
                // the move-only row covers so checkElementCategoryInstantiations
                // can reject it — after all member types are resolved, exactly
                // like the hashContainerSites_ site above — if the bound is an
                // owning inline array or an owning-classified choice. See
                // moveOnlyElementSites_'s comment for exactly which slots these
                // are and why the owning family's already-checked owned slot
                // (OwningArray/OwningLinkedList/OwningSet/OwningStack/
                // OwningQueue's sole slot, OwningDictionary's VALUE slot) is
                // excluded.
                {
                    // {generic-arg index, spelled slot name}
                    std::vector<std::pair<std::size_t, std::string_view>>
                        categorySlots;
                    if (first == "Array" || first == "LinkedList" ||
                        first == "Set" || first == "Stack" ||
                        first == "Queue" || first == "SortedSet") {
                        categorySlots.push_back({0, "T"});
                    } else if (first == "Dictionary" ||
                               first == "SortedDictionary") {
                        categorySlots.push_back({0, "K"});
                        categorySlots.push_back({1, "V"});
                    } else if (first == "OwningDictionary") {
                        categorySlots.push_back({0, "K"});
                    }
                    for (const auto& slot : categorySlots) {
                        if (slot.first >= ref.genericArgs.size()) continue;
                        // fix #2742 (sibling, same mechanism):
                        // `checkElementCategoryInstantiations`' Pass 2 resolves
                        // this site's bare parameter name against the ENCLOSING
                        // TYPE's params exactly as the hash-container Pass 2
                        // does, so it carries the identical #2713
                        // misattribution when a generic METHOD shadows the
                        // name. This recorder was one of the two that never got
                        // the guard; it now asks the same shared predicate.
                        const TypeRef* el = ref.genericArgs[slot.first];
                        if (isBareGenericParamBinding(
                                el, el ? el->astNode : nullptr) &&
                            nameBoundByCurrentMethod(el->headName()))
                            continue;
                        moveOnlyElementSites_.push_back(
                            {first, slot.second, ref.genericArgs[slot.first],
                             &t, currentOuterType_});
                    }
                }
                // §14.4 / §15.3 (#3794): OwningBuffer<T>/Span<T>/ReadOnlySpan<T>
                // are an EXCEPTION to §28.11's element-slot-policy table — they
                // admit T to be value-typed in general — but the §14.4 exception
                // itself STILL rejects the §15.3 move-only value type kinds
                // (owning inline arrays `[N owned]T` / `[N owned]T?`, and
                // owning-classified choices, §35.4). #3775 deliberately deferred
                // these three to a separate fix because their admission rule
                // differs (value-typed T is otherwise legal) and the diagnostic
                // cites §14.4 not §28.11. The check is INLINE here for the
                // concrete-bound case (Pass 1 of the two-pass shape, exactly
                // like checkElementCategoryInstantiations's Pass 1 — the
                // predicate answers false for a bare generic parameter without
                // recording it, so Pass 2 only needs to walk bare-param sites).
                //
                // A bare generic parameter of the ENCLOSING type (e.g.
                // `field: OwningBuffer<T>` inside `class H<T>`) is recorded for
                // Pass 2 re-check, with the same shadowing guard as the
                // OwningElementSite recorder above: a generic METHOD may declare
                // a parameter of the same name (`class H<T> { method M<T>() }`),
                // and that name inside the method body denotes the METHOD's
                // parameter whose bindings `instantiations_` knows nothing
                // about. Recording it as the enclosing type's would falsely
                // reject a legal program — the OwningElementSite recorder has
                // the same guard; reusing its shape keeps the two sites
                // consistent.
                if (first == "OwningBuffer" || first == "Span" ||
                    first == "ReadOnlySpan" ||
                    first == "ArraySpan" || first == "ReadOnlyArraySpan") {
                    if (!ref.genericArgs.empty()) {
                        TypeRef* el = ref.genericArgs[0];
                        const char* moveOnlyKind = nullptr;
                        if (common::isMoveOnlyValueType(el, ctx_,
                                                        moveOnlyKind)) {
                            // Concrete move-only T → reject inline. fix #3796
                            // (§3.6 rule 2 / §14.4): prefer the offending TYPE-
                            // ARGUMENT's source location (`el->astNode`) over
                            // the enclosing annotation site's, matching the
                            // checkHashContainerKeys / checkElementCategory-
                            // Instantiations pattern. Falls back to `t.span`
                            // if the elRef has no AST node.
                            SourceLocation diagLoc;
                            if (el && el->astNode &&
                                el->astNode->span.begin.line != 0)
                                diagLoc = el->astNode->span.begin;
                            else
                                diagLoc = t.span.begin;
                            ctx_.diagnostics().report(
                                Severity::Error, diagLoc,
                                std::string("`") + std::string(first) +
                                "` cannot use " +
                                std::string(moveOnlyKind) +
                                " as its T type argument "
                                "(\xC2\xA7""14.4 / \xC2\xA7""15.3) \xE2\x80\x94 "
                                "the OwningBuffer/Span/ReadOnlySpan exception "
                                "still rejects move-only value types, both "
                                "kinds of \xC2\xA7""15.3: the owning inline "
                                "arrays and the owning-classified choices "
                                "(\xC2\xA7""35.4)");
                        } else if (isRuntimeManagedReferenceType(el)) {
                            // fix #4679 / §4.14 / §4.15 (v0.307, last §4.14/
                            // §14.4 reject case from the issue's enumeration —
                            // `l4_14_0013` "OwningBuffer<string> is
                            // admitted"): the OwningBuffer/Span/ReadOnlySpan/
                            // ArraySpan/ReadOnlyArraySpan exception admits T
                            // as either class-typed (with ownership
                            // semantics) or value-typed (with plain value
                            // semantics) — but the exception's own first
                            // clause EXCLUDES the runtime-managed reference
                            // types (`string`, function types, §28.9). Those
                            // types have no disposal obligation, so the
                            // "owning" semantics add nothing; collections of
                            // them belong in borrowing form (`Array<string>`,
                            // `Array<T -> R>`), not in OwningBuffer. Reject
                            // inline with `not admitted` wording per the
                            // spec's phrasing at §4.14 *What is not admitted*.
                            // Same source-location preference as the move-only
                            // arm.
                            SourceLocation diagLoc;
                            if (el && el->astNode &&
                                el->astNode->span.begin.line != 0)
                                diagLoc = el->astNode->span.begin;
                            else
                                diagLoc = t.span.begin;
                            std::string_view what =
                                (el->kind == TypeRef::Kind::Function)
                                    ? std::string_view(
                                          "a function type")
                                    : std::string_view("`string`");
                            ctx_.diagnostics().report(
                                Severity::Error, diagLoc,
                                std::string("`") + std::string(first) +
                                "` does not admit " +
                                std::string(what) +
                                " as its T type argument (\xC2\xA7""4.14"
                                " / \xC2\xA7""14.4 \xE2\x80\x94 What is not "
                                "admitted, v0.307) \xE2\x80\x94 runtime-managed"
                                " reference types have no disposal obligation"
                                " (§28.9), so the OwningBuffer/Span/ArraySpan"
                                " owning semantics add nothing; collect them"
                                " in borrowing form (`Array<string>`, "
                                "`Array<T -> R>`) instead");
                        } else if (el && !el->declaration &&
                                   el->kind == TypeRef::Kind::Named &&
                                   el->astNode && el->astNode->name &&
                                   el->astNode->name->parts.size() == 1 &&
                                   currentOuterType_) {
                            // Bare generic parameter of the enclosing type →
                            // record for Pass 2 recheck.
                            // Same method-shadowing guard as the
                            // OwningElementSite recorder immediately above: a
                            // generic method may declare a parameter of the
                            // same name, and that name inside the method body
                            // denotes the METHOD's parameter whose bindings
                            // `instantiations_` knows nothing about. fix #2742
                            // made that one shared predicate.
                            if (!nameBoundByCurrentMethod(
                                    el->astNode->name->parts.front())) {
                                owningBufferSpanMoveOnlySites_.push_back(
                                    {first,
                                     std::string(
                                         el->astNode->name->parts.front()),
                                     &t, currentOuterType_});
                            }
                        }
                    }
                }
                // §16.5 / issue #922: Range<T> is a compiler-intrinsic record.
                // An explicit annotation `Range<int>` must resolve to the SAME
                // TypeRef shape as the inferred form `var r := 1..5` — both
                // must have declaration = ctx_.builtinClassSym("Range") so that
                // isRangeType() / .Start/.End member access / for-in-over-Range
                // codegen in LlvmBackend_Stmts.cpp recognise annotated Ranges
                // identically to inferred ones.  SemaContext::builtinClassSym
                // caches the pointer so this and ExprTC::builtinClassSym return
                // the same object, preserving pointer-equality guards.
                if (first == "Range") {
                    ref.declaration = ctx_.builtinClassSym("Range");
                    // fix #3247 / §16.3 (v0.277): `Range<T>`'s T is restricted
                    // by the compiler to the integer types and `char`. The
                    // annotation site is the FIRST place a non-integer T is
                    // visible — the range expression (which the inferred form
                    // requires) is in ExprTypecheck_Exprs.cpp's visitExpr.
                    // Reject at the annotation site so the user sees the §16.3
                    // diagnostic at the type, not at every range expression
                    // that uses the type. The generic arg has already been
                    // resolved at this point (the `for (auto& arg : ref.genericArgs)`
                    // walk above processes each), so the predicate reads the
                    // post-resolution TypeRef. An unresolved T (a forward
                    // reference) keeps the predicate's null-tolerant contract:
                    // an Error-typed arg is not the compiler's concern at
                    // this site, and the downstream use will re-error.
                    if (!ref.genericArgs.empty() &&
                        !isValidRangeElementType(ref.genericArgs[0])) {
                        ctx_.diagnostics().report(
                            Severity::Error, t.span.begin,
                            std::string("`Range<T>` requires T to be one of "
                                        "the integer types or `char` \xe2\x80\x94 "
                                        "got `") +
                                rangeElementTypeName(ref.genericArgs[0]) +
                                "` (\xc2\xa7""16.3)");
                    }
                }
                // §10.5 / stdlib: `Ptr<T>` — pin declaration to the synthesized
                // builtin so isAdmissibleComponentType (#3327) and any future
                // identity-based builtin predicates (the Range/Buffer pattern)
                // see the same TypeRef as the inferred form. Mirrors the
                // Range<T> binding immediately above.
                if (first == "Ptr") {
                    ref.declaration = ctx_.builtinClassSym("Ptr");
                }
                // per §2.5 / #1357: DateOnly and TimeOnly are value-type builtins
                // (stored as i32). Like DateTime/TimeSpan/Guid they keep
                // declaration=null here so that lowerNew()'s `if (!rt->declaration)`
                // gate at LlvmBackend_Exprs.cpp fires for `new DateOnly(...)`/
                // `new TimeOnly(...)`. The ownership tracker recognises them via
                // Path B (name-string comparison) in Ownership.cpp.
                //
                // #1115 / §2.8–§2.9 / #975: the auto-imported builtin generic
                // interfaces IComparable<T> / IEquatable<T> / IEqualityComparer<T>
                // get a REAL synthesized Interface TypeSymbol (with their contract
                // members) attached as the declaration, so they lower as ordinary
                // heap interface references through the user-interface itable path
                // — usable as a field/parameter/return/local value type, not just
                // a `where` bound. The arity check above (still) diagnoses wrong
                // arity; this only attaches the declaration when name+arity valid.
                // stdlib §2.19 / §11.8 (v0.176, #807) covers the static-
                // abstraction members of that set (INumber/ISteppable/
                // IParsable): they get a real synthesized Interface TypeSymbol
                // so `: INumber<Meters>` conformance and `where T: INumber<T>`
                // bounds resolve to a declaration whose (static) member
                // signatures drive the §11.8 conformance check and T.Member
                // dispatch.
                // #2776: the set now lives in ONE place
                // (SemaContext::isSynthesizedBuiltinInterfaceName) because the
                // module loader must recognise exactly the same names to give
                // an IMPORTED occurrence the same identity.
                if (SemaContext::isSynthesizedBuiltinInterfaceName(first)) {
                    ref.declaration = ctx_.builtinInterfaceSym(first);
                }
                return;
            }
        }
        // §2.2 v0.164 / #1365: bare-name collision — two or more imported modules
        // export a type with this (name, arity). Using it unqualified is a
        // compile error; the fix is a module-qualified name or an import
        // alias (§2.2). fix #5172 (§2.2, v0.310): the key is the PAIR
        // (name, arity), not the bare name — two imports exporting `Counter`
        // (arity 0) and `Counter<T>` (arity 1) are NOT ambiguous; only
        // collisions at the same (name, arity) are the §2.2 collision. The
        // reference's arity is `t.genericArgs.size()`; for a bare reference
        // (arity 0) we check (first, 0).
        std::size_t refArityForAmbiguity = t.genericArgs.size();
        if (!ambiguousImportNames_.empty() &&
            ambiguousImportNames_.count(
                std::make_pair(std::string(first), refArityForAmbiguity))) {
            ctx_.diagnostics().report(
                Severity::Error, t.span.begin,
                std::string("bare name `") + std::string(first) +
                (refArityForAmbiguity == 0
                     ? std::string("")
                     : std::string("<") +
                           std::to_string(refArityForAmbiguity) + ">") +
                "` is ambiguous: multiple imported modules export a type "
                "with this name and arity; qualify it via a module-qualified "
                "name or an import alias (\xC2\xA7""2.2)");
            ref.kind = TypeRef::Kind::Error;
            return;
        }
        // §2.2 v0.164 / §2.7 / #1365: import-alias resolution in type position.
        // `Alias.TypeName[.Nested...]` where `Alias` is an import alias.
        // Per §2.7 rule 1 this is checked before module-prefix matching.
        if (!importAliases_.empty()) {
            auto aliasIt = importAliases_.find(std::string(first));
            if (aliasIt != importAliases_.end()) {
                const std::string& modName = aliasIt->second;
                if (t.name->parts.size() < 2) {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        std::string("`") + std::string(first) +
                        "` is an import alias; use `" + std::string(first) +
                        ".TypeName` to name a type from that module (\xC2\xA7""2.2)");
                    ref.kind = TypeRef::Kind::Error;
                    return;
                }
                std::string_view typeName = t.name->parts[1];
                const auto* exported = ctx_.importedTypesOf(modName);
                TypeSymbol* tsymAlias = nullptr;
                if (exported) {
                    for (TypeSymbol* ts : *exported) {
                        if (ts->name == typeName) { tsymAlias = ts; break; }
                    }
                }
                if (!tsymAlias) {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        std::string("module `") + modName +
                        "` has no public type `" + std::string(typeName) +
                        "` (\xC2\xA7""2.2)");
                    ref.kind = TypeRef::Kind::Error;
                    return;
                }
                // Walk remaining parts as nested types (§11.3).
                for (std::size_t pi = 2; pi < t.name->parts.size(); ++pi) {
                    std::string_view part = t.name->parts[pi];
                    TypeSymbol* inner = nullptr;
                    for (Symbol* m : tsymAlias->members) {
                        if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                            if (nt->name == part && nt->type) {
                                inner = nt->type;
                                break;
                            }
                        }
                    }
                    if (!inner) {
                        ctx_.diagnostics().report(
                            Severity::Error, t.span.begin,
                            std::string("type `") + std::string(tsymAlias->name) +
                            "` has no nested type `" + std::string(part) +
                            "` (\xC2\xA7""11.3)");
                        ref.kind = TypeRef::Kind::Error;
                        return;
                    }
                    tsymAlias = inner;
                }
                ref.kind = TypeRef::Kind::Named;
                ref.declaration = tsymAlias;
                for (const auto& ga : t.genericArgs) {
                    if (!ga) continue;
                    ref.genericArgs.push_back(resolveType(*ga, scope));
                }
                return;
            }
        }
        // §2.7 rule 2 / #1365: module-prefix resolution in type position.
        // Try each prefix of the dotted name (longest-first) against imported
        // module names. The remaining parts then resolve as type + nested types.
        // Only fires when the context has loaded imports (ctx_.hasImports()).
        if (ctx_.hasImports() && t.name->parts.size() >= 2) {
            // Build prefixes longest-first: for N parts, try N-1, N-2, ..., 1.
            for (int prefixLen = (int)t.name->parts.size() - 1;
                 prefixLen >= 1; --prefixLen) {
                std::string prefix;
                for (int k = 0; k < prefixLen; ++k) {
                    if (k > 0) prefix += '.';
                    prefix += std::string(t.name->parts[k]);
                }
                // Only honour modules that are explicitly imported in this file.
                if (!sourceImports_.count(prefix)) continue;
                const auto* exported = ctx_.importedTypesOf(prefix);
                if (!exported) continue;
                // parts[prefixLen] is the type name.
                std::string_view typeName = t.name->parts[prefixLen];
                TypeSymbol* tsymMod = nullptr;
                for (TypeSymbol* ts : *exported) {
                    if (ts->name == typeName) { tsymMod = ts; break; }
                }
                if (!tsymMod) {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        std::string("module `") + prefix +
                        "` has no public type `" + std::string(typeName) +
                        "` (\xC2\xA7""2.7)");
                    ref.kind = TypeRef::Kind::Error;
                    return;
                }
                // Walk remaining parts as nested types.
                for (std::size_t pi = (std::size_t)prefixLen + 1;
                     pi < t.name->parts.size(); ++pi) {
                    std::string_view part = t.name->parts[pi];
                    TypeSymbol* inner = nullptr;
                    for (Symbol* m : tsymMod->members) {
                        if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                            if (nt->name == part && nt->type) {
                                inner = nt->type;
                                break;
                            }
                        }
                    }
                    if (!inner) {
                        ctx_.diagnostics().report(
                            Severity::Error, t.span.begin,
                            std::string("type `") + std::string(tsymMod->name) +
                            "` has no nested type `" + std::string(part) +
                            "` (\xC2\xA7""11.3)");
                        ref.kind = TypeRef::Kind::Error;
                        return;
                    }
                    tsymMod = inner;
                }
                ref.kind = TypeRef::Kind::Named;
                ref.declaration = tsymMod;
                for (const auto& ga : t.genericArgs) {
                    if (!ga) continue;
                    ref.genericArgs.push_back(resolveType(*ga, scope));
                }
                return;
            }
        }
        {
            std::string msg = std::string("unresolved type name `") +
                              std::string(first) + "`";
            std::string hint = owningRenameHint(first);  // v0.168 (#1372)
            if (!hint.empty()) msg += "; " + hint;
            ctx_.diagnostics().report(Severity::Error, t.span.begin, msg);
        }
        ref.kind = TypeRef::Kind::Error;
        return;
    }
    // For multi-part QualifiedNames, descend through the looked-up symbol's
    // nested types for each subsequent part. Per Grammar A.8 / §11.3:
    // `Outer.Inner` means Inner is a nested type declared inside Outer.
    // The final resolved TypeSymbol is the one for the last part.
    //
    // If the first part resolves to a non-TypeSymbol (e.g. a TypeAliasSymbol
    // or a module-level alias), fall back to treating the whole name as a
    // single opaque reference with no declaration — the same as the pre-fix
    // `Module.Name`-style handling for any non-TypeSymbol first part.
    //
    // §11.3 / #482 fix C: a single-part name may resolve via lookupName to a
    // NestedTypeSymbol* (now that fix C stores the real pointer instead of
    // nullptr). NestedTypeSymbol does NOT inherit TypeSymbol, so the
    // dynamic_cast below would give null and we'd set declaration=nullptr.
    // Handle it before the cast: a single-part nested-type reference inside
    // the outer class body is exactly `Inner` — follow NestedTypeSymbol::type
    // as the resolved TypeSymbol. (A multi-part form `Outer.Inner` reaches
    // here via the qualified-name walk in the `else` branch with `sym` = the
    // outer TypeSymbol — that path is unchanged.)
    auto* tsym = dynamic_cast<TypeSymbol*>(sym);
    if (!tsym) {
        // §11.3 / #482 fix C: check for NestedTypeSymbol (inner enum/class
        // referenced by a single unqualified name inside the outer class body).
        if (auto* nts = dynamic_cast<NestedTypeSymbol*>(sym)) {
            if (nts->type) {
                tsym = nts->type;
                // Fall through into the else branch (tsym now non-null) to
                // handle any remaining qualified parts and set declaration.
                // For a single-part name there are no further parts, so the
                // loop below is a no-op and declaration = tsym is set.
            }
        }
    }
    if (!tsym) {
        // §27.8: type aliases are pure synonyms. If the symbol is a
        // TypeAliasSymbol, resolve the alias transparently by copying the
        // underlying type's resolved TypeRef into `ref`. This makes alias-typed
        // locals behave identically to the underlying type for operator dispatch,
        // lambda-target inference, and all other typecheck paths (fix #578).
        if (auto* aliasSym = dynamic_cast<TypeAliasSymbol*>(sym)) {
            const auto* aliasDecl =
                dynamic_cast<const ast::TypeAlias*>(aliasSym->decl);
            if (aliasDecl && aliasDecl->aliasedType) {
                // fix #4764 (§27.8, v0.307): "A generic alias may not
                // reference itself (no recursive aliases)" — and the same
                // rule holds for a non-generic alias and for an indirect
                // (mutual) cycle: nothing downstream can give a self- or
                // mutually-referential alias body "a concrete type after
                // substitution" (§27.8). Detect the cycle BEFORE recursing
                // into `aliasedType` below: push `aliasDecl` on the on-stack
                // guard (RAII-popped on every exit from this block,
                // including the early returns), and if it is already on the
                // stack, this reference reaches back to an alias currently
                // being expanded further up this same call — a cycle direct
                // (`Loop<T> := Loop<T>`) or indirect (`A := B; B := A`).
                // Report once, at the innermost re-entry, and resolve to
                // Error rather than recursing further (previously: unbounded
                // recursion / SIGSEGV, issue #4764).
                //
                // fix #4784 (§27.8, v0.307): a re-entrant push at THIS SAME
                // source location can be reached more than once — a built-in
                // generic head's Named/no-declaration TypeRef looks
                // (to `resolveAliasForwardReferences`, fix #4047) exactly
                // like an unresolved forward alias reference, so it re-walks
                // an already-rejected cyclic body a second time
                // (`Loop2 := Array<Loop2>`); a compound alias body with more
                // than one self-referencing occurrence (a function type's
                // param AND return slot, `Fn3 := (Fn3) -> Fn3`) re-walks the
                // WHOLE body from each occurrence's own expansion, so each
                // occurrence is reported once from the other's walk and once
                // from its own. `reportedRecursiveAliasSites_` is keyed on
                // (aliasDecl, exact source location), so a second report at a
                // location already reported for this alias is suppressed —
                // a genuinely DIFFERENT occurrence (different location) is
                // never suppressed, only a re-visit of one already reported.
                if (!aliasResolutionStack_.insert(aliasDecl).second) {
                    if (reportedRecursiveAliasSites_
                            .insert({aliasDecl,
                                     {t.span.begin.line, t.span.begin.column}})
                            .second) {
                        ctx_.diagnostics().report(
                            Severity::Error, t.span.begin,
                            std::string("type alias `") +
                                std::string(aliasDecl->name) +
                                "` is recursive (\xC2\xA7""27.8) \xE2\x80\x94 a "
                                "type alias may not reference itself, directly "
                                "or through a chain of other aliases");
                    }
                    ref.kind = TypeRef::Kind::Error;
                    return;
                }
                struct AliasStackGuard {
                    std::unordered_set<const ast::TypeAlias*>& stack;
                    const ast::TypeAlias* decl;
                    ~AliasStackGuard() { stack.erase(decl); }
                } aliasStackGuard{aliasResolutionStack_, aliasDecl};
                // fix #4167 (§27.8, v0.298): a reference to a GENERIC alias
                // that does not supply exactly the alias's own declared
                // number of type arguments has no sound resolution — this
                // branch below copies the alias's CACHED underlying TypeRef
                // (resolved once, at the alias's own declaration site, with
                // the alias's generic params bound as `nullptr` markers in a
                // throwaway scope) verbatim into every reference site; it
                // never substitutes `t.genericArgs` into that cached ref, so
                // an omitted or miscounted argument list is silently
                // ignored rather than caught. For an alias whose body IS (or
                // reduces to) one of its own type parameters —
                // `type IdP11<T> := T;` — a bare reference's cached
                // underlying TypeRef is `Kind::Named` with
                // `declaration == nullptr`: exactly the shape §26.2 reserves
                // for "the target is a type parameter", and nothing
                // downstream can tell it apart from the legitimate
                // declaration-less shape locally-resolved builtins use
                // (`Array<T>`, `IEnumerable<T>`, …) — `extension IdP11`
                // (bare) reached codegen with a permanently dead, uncallable
                // member block and zero diagnostics (issue #4167). This
                // check closes the hole at its root, for every reference
                // context a bare/miscounted generic-alias name can appear in
                // (locals, fields, parameters, extension targets, …), not
                // just extension targets — completing, for aliases, the
                // same generic-arity check the ordinary-TypeSymbol arm gives
                // a few lines below (search this file for "Generic-arity
                // check"). NOTE (narrow #4167, review-caught): no normative
                // spec section states this reference-site arity rule for
                // EITHER arm — searched §11.3 "Member body" (member-clash
                // rules, unrelated), Appendix C.4 "Generics" (entirely
                // `where`-clause constraint/substitution semantics, no
                // argument-count rule), §14.1-§14.6 (§14.5 states the
                // omit-or-supply-in-full rule for a generic METHOD CALL's
                // type-argument list, a different subject), and Appendix
                // C.1 "Type system" (no matching bullet); filed as issue
                // #4181 — a genuine spec gap, not a citation this comment
                // can correctly make. Unlike the ordinary-TypeSymbol arm, a
                // wholly bare (zero-argument) reference is
                // ALSO rejected here when the alias is generic: the
                // TypeSymbol check tolerates an omitted argument list (a
                // "raw" reference to the real declared type, which still
                // carries a genuine declaration downstream); a generic
                // alias has no such safe fallback; its cached underlying
                // ref is the ONLY representation available and — as above —
                // is indistinguishable from a bare type-parameter reference
                // when the alias's body is one.
                if (t.genericArgs.size() != aliasDecl->genericParams.size()) {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        "generic-argument count does not match type "
                        "alias `" + std::string(aliasDecl->name) +
                        "`'s generic-parameter count (\xC2\xA7""27.8)");
                    // Resolve any supplied args anyway so the AST is fully
                    // visited (matches the primitive-generic-args error
                    // path above).
                    for (const auto& ga : t.genericArgs) {
                        if (ga) resolveType(*ga, scope);
                    }
                    ref.kind = TypeRef::Kind::Error;
                    return;
                }
                // fix #4105 (aliasScope fallback, narrower): when resolving a
                // generic alias's aliasedType (e.g. `type Alias<T> := [1]T;`)
                // at a reference site whose scope does NOT bind `T` (e.g. a
                // method return type), the inner `T` must still resolve.  Build
                // a per-alias scope binding the alias's own generic params and
                // pass it to resolveType.  This is the broad-coverage fallback
                // for nested non-bare alias bodies.
                Scope aliasScope;
                aliasScope.parent = scope;
                for (const auto& g : aliasDecl->genericParams) {
                    aliasScope.names[g.name] = nullptr;
                }
                // fix #4447 (§27.8, v0.300): when the alias body is EXACTLY a
                // bare generic param (`type IdP11<T> := T;`), the bare `T`
                // must resolve to the CONCRETE type arg (`int`), not an
                // "unresolved type name" error.  Build the alias-substitution
                // map first; if the aliased type is a bare generic param
                // (single-part Named with no generic args and no declaration),
                // look it up in aliasSubst and use the concrete type directly.
                std::unordered_map<std::string_view, TypeRef*> aliasSubst;
                for (std::size_t gi = 0;
                     gi < aliasDecl->genericParams.size() &&
                     gi < t.genericArgs.size(); ++gi) {
                    if (!t.genericArgs[gi]) continue;
                    TypeRef* concreteArg =
                        resolveType(*t.genericArgs[gi], scope);
                    if (!concreteArg) continue;
                    aliasSubst[aliasDecl->genericParams[gi].name] =
                        concreteArg;
                }
                TypeRef* underlying = nullptr;
                if (aliasDecl->aliasedType &&
                    aliasDecl->aliasedType->kind == ast::TypeKind::Named &&
                    aliasDecl->aliasedType->name &&
                    aliasDecl->aliasedType->name->parts.size() == 1 &&
                    aliasDecl->aliasedType->genericArgs.empty()) {
                    auto it = aliasSubst.find(
                        aliasDecl->aliasedType->name->parts.front());
                    if (it != aliasSubst.end()) {
                        underlying = it->second;
                    }
                }
                if (!underlying) {
                    if (TypeRef* resolved =
                            resolveType(*aliasDecl->aliasedType, &aliasScope)) {
                        underlying = resolved;
                    } else {
                        underlying =
                            resolveType(*aliasDecl->aliasedType, scope);
                    }
                }
                if (underlying) {
                    // fix #4188 (§27.8, v0.298): `underlying` is the alias's
                    // CACHED TypeRef, resolved once at the alias's own
                    // declaration against a throwaway scope that binds the
                    // alias's own parameters to nullptr markers
                    // (resolveTopDecl's TypeAlias arm) — so it still carries
                    // those parameters UNBOUND. Without substituting THIS
                    // reference site's concrete arguments in, a generic
                    // alias used as e.g. a call argument's local type
                    // resolved to `declaration = Box` (correct, #1462) but
                    // `genericArgs = [T-unbound]` instead of `[int]` — the
                    // spec's own "each instantiation of the alias is
                    // interchangeable with the corresponding instantiation
                    // of the aliased type" (§27.8) requires the latter.
                    // Downstream, a bare/unbound `T` here is indistinguishable
                    // from "cannot infer this generic method type parameter
                    // from this argument", so a call passing such a value
                    // silently fell back to the UNSPECIALIZED generic-method
                    // template — an IR-verifier failure (call parameter type
                    // vs. the concrete argument's specialized struct type).
                    std::unordered_map<std::string_view, TypeRef*> aliasSubst;
                    for (std::size_t gi = 0;
                         gi < aliasDecl->genericParams.size() &&
                         gi < t.genericArgs.size(); ++gi) {
                        if (!t.genericArgs[gi]) continue;
                        TypeRef* concreteArg =
                            resolveType(*t.genericArgs[gi], scope);
                        if (!concreteArg) continue;
                        aliasSubst[aliasDecl->genericParams[gi].name] =
                            concreteArg;
                    }
                    underlying =
                        substituteAliasGenericArgs(underlying, aliasSubst,
                                                    t.span.begin);
                    // fix #4464 (§14.2, §27.8, v0.298): a generic type alias
                    // may carry `where` clauses (§27.8: "uses of the alias must
                    // satisfy it"). The substitution above substituted the
                    // concrete args into the alias body, but the alias's own
                    // `where` clauses were never checked against those concrete
                    // args. Walk each GenericParamSymbol's constraints (populated
                    // by SymbolCollection::collectGenericParams from
                    // aliasDecl->genericConstraints) and validate the concrete
                    // substitution against each bound.
                    for (std::size_t gi = 0; gi < aliasSym->genericParams.size();
                         ++gi) {
                        auto* gp =
                            dynamic_cast<GenericParamSymbol*>(aliasSym->genericParams[gi]);
                        if (!gp || gp->constraints.empty()) continue;
                        auto it = aliasSubst.find(gp->name);
                        if (it == aliasSubst.end()) continue;
                        TypeRef* arg = it->second;
                        if (!arg) continue;
                        // Build a scope for resolving constraint bounds: module
                        // names plus the alias's own generic-param markers.
                        Scope boundModule;
                        if (ctx_.module()) {
                            for (Symbol* child : ctx_.module()->children) {
                                if (!child->name.empty())
                                    boundModule.names[child->name] = child;
                            }
                        }
                        Scope boundScope;
                        boundScope.parent = &boundModule;
                        for (const auto& g : aliasDecl->genericParams) {
                            boundScope.names[g.name] = nullptr;
                        }
                        for (const auto& c : gp->constraints) {
                            // Spellings for TypeRef* args (no ast::Type needed).
                            auto typeRefSpelling =
                                [](const TypeRef* r) -> std::string {
                                if (!r) return "<type>";
                                if (r->kind == TypeRef::Kind::Named) {
                                    if (r->declaration)
                                        return std::string(r->declaration->name);
                                    if (!r->importedName.empty())
                                        return std::string(r->importedName);
                                } else if (r->kind == TypeRef::Kind::Primitive) {
                                    // Mirror the names in primitiveFromName (TypeResolution.cpp).
                                    switch (r->primitive) {
                                        case PrimitiveType::Bool:    return "bool";
                                        case PrimitiveType::Char:    return "char";
                                        case PrimitiveType::String:   return "string";
                                        case PrimitiveType::Int:      return "int";
                                        case PrimitiveType::UInt:     return "uint";
                                        case PrimitiveType::Long:     return "long";
                                        case PrimitiveType::ULong:    return "ulong";
                                        case PrimitiveType::Byte:     return "byte";
                                        case PrimitiveType::SByte:   return "sbyte";
                                        case PrimitiveType::Short:   return "short";
                                        case PrimitiveType::UShort:  return "ushort";
                                        case PrimitiveType::Float:   return "float";
                                        case PrimitiveType::Double:   return "double";
                                        case PrimitiveType::Decimal:  return "decimal";
                                        case PrimitiveType::Object:   return "Object";
                                        default: break;
                                    }
                                }
                                return "<type>";
                            };
                            std::string argName = typeRefSpelling(arg);

                            // "type" bound: satisfied when arg conforms to the bound
                            // type (BFS over arg's base types).
                            if (c.kind == "type") {
                                TypeRef* bound = nullptr;
                                if (c.astBound) {
                                    bound = resolveType(*c.astBound, &boundScope);
                                } else if (c.importedBound) {
                                    bound = c.importedBound;
                                }
                                if (!bound) continue;
                                // fix-forward #5086 (§14.2, v0.307): pass
                                // ctx_ so the nested-generic substitution
                                // branch does not dereference a null
                                // SemaContext*.
                                bool satisfied = argSatisfiesTypeBound(arg, bound, &ctx_);
                                // A "type" bound that is itself a type parameter:
                                // resolve it through aliasSubst to get the concrete
                                // bound before emitting the diagnostic.
                                if (!satisfied &&
                                    bound->kind == TypeRef::Kind::Named &&
                                    !bound->declaration && bound->astNode &&
                                    bound->astNode->name &&
                                    bound->astNode->name->parts.size() == 1) {
                                    std::string_view bpName =
                                        bound->astNode->name->parts.front();
                                    auto jt = aliasSubst.find(bpName);
                                    if (jt != aliasSubst.end() && jt->second &&
                                        jt->second->declaration) {
                                        bound = jt->second;
                                        // fix-forward #5086 (§14.2, v0.307):
                                        // same ctx_ requirement as above.
                                        satisfied = argSatisfiesTypeBound(arg, bound, &ctx_);
                                    }
                                }
                                if (!satisfied) {
                                    std::string boundName;
                                    if (bound->kind == TypeRef::Kind::Named) {
                                        if (bound->declaration) {
                                            boundName = bound->declaration->name;
                                        } else if (!bound->importedName.empty()) {
                                            boundName = bound->importedName;
                                            if (!bound->genericArgs.empty()) {
                                                std::string withArgs = boundName + "<";
                                                for (std::size_t i = 0;
                                                     i < bound->genericArgs.size();
                                                     ++i) {
                                                    if (i > 0) withArgs += ", ";
                                                    withArgs +=
                                                        typeRefSpelling(
                                                            bound->genericArgs[i]);
                                                }
                                                withArgs += '>';
                                                boundName = std::move(withArgs);
                                            }
                                        }
                                    }
                                    if (boundName.empty()) boundName = "<type>";
                                    ctx_.diagnostics().report(
                                        Severity::Error, t.span.begin,
                                        std::string("type argument `") +
                                            argName +
                                            "` does not satisfy the "
                                            "constraint `where " +
                                            std::string(gp->name) + ": " +
                                            boundName + "` (§14.2)");
                                }
                                continue;
                            }
                            // All other constraint kinds ("class", "value", "notnull",
                            // "new", …): checked inline using the same logic as
                            // checkOneConstraint.
                            bool violated = false;
                            std::string reason;
                            if (c.kind == "notnull") {
                                violated = arg->nullable;
                                reason = "notnull";
                            } else if (c.kind == "class") {
                                // §14.2: non-nullable reference types only.
                                if (arg->nullable) {
                                    violated = true;
                                } else {
                                    TypeCategory cat = categorize(arg);
                                    violated = (cat == TypeCategory::Value);
                                }
                                reason = "class";
                            } else if (c.kind == "value") {
                                // §14.2: non-nullable value types only.
                                if (arg->nullable) {
                                    violated = true;
                                } else {
                                    TypeCategory cat = categorize(arg);
                                    violated = (cat == TypeCategory::Reference);
                                }
                                reason = "value";
                            } else if (c.kind == "new") {
                                bool decided = false;
                                bool ok = argSatisfiesNew(arg, decided);
                                violated = (decided && !ok);
                                reason = "new()";
                            }
                            // "sendable" and "hashable" are deferred (require
                            // late resolution, same as checkOneConstraint).
                            if (violated) {
                                ctx_.diagnostics().report(
                                    Severity::Error, t.span.begin,
                                    std::string("type argument `") + argName +
                                        "` does not satisfy the constraint `where " +
                                        std::string(gp->name) + ": " + reason +
                                        "` (§14.2)");
                            }
                        }
                    }
                    // Copy the underlying kind and payload into ref. The nullable
                    // flag on ref was already set from the syntactic `?` suffix in
                    // resolveType; the underlying type's nullable flag is
                    // independent (a bare alias mention `Meters` is not nullable
                    // even if the aliased type happened to be `double?`).
                    bool savedNullable = ref.nullable;
                    const ast::Type* savedAstNode = ref.astNode;
                    ref = *underlying;
                    // Restore the original astNode and nullable so diagnostics and
                    // null-safety checks still refer to the use site, not the
                    // alias declaration. If the use site wrote `Pred?`, honour it.
                    ref.astNode = savedAstNode;
                    ref.nullable = savedNullable || underlying->nullable;
                    // #1462 / §27.8: restoring astNode to the use site hides the
                    // underlying type's syntactic name-head from every check that
                    // identifies a builtin collection/wrapper by head. Carry the
                    // underlying head on the TypeRef itself so those checks (sema,
                    // ownership, codegen — static or not) see through the alias.
                    // `ref = *underlying` already copied underlying's own
                    // aliasResolvedHead (non-empty for a CHAINED alias
                    // `type A := B := OwningArray`); if it is empty (underlying is a
                    // concrete type), take underlying's syntactic head, then its
                    // importedName.
                    if (ref.aliasResolvedHead.empty()) {
                        if (underlying->astNode && underlying->astNode->name &&
                            !underlying->astNode->name->parts.empty())
                            ref.aliasResolvedHead =
                                underlying->headName();
                        else
                            ref.aliasResolvedHead = underlying->importedName;
                    }
                    // fix #4105 (§27.8, v0.300): update the cache for the
                    // alias's aliasedType AST node so that subsequent
                    // ctx_.resolvedType(aliasDecl->aliasedType) calls return the
                    // fully-resolved underlying type without re-invoking
                    // resolveNamed.  The alias node itself (aliasDecl) is NOT the
                    // cache key — only its aliasedType is (the Type node that
                    // resolveTopDecl and resolveType stored in the cache on the
                    // first forward pass).  Also update the cache for the Type
                    // node `t` that was passed to this resolveNamed call — when
                    // resolveNamed is called with a Type annotation node (e.g.
                    // the "A" in "var x: A"), `t` points to that Type node and
                    // its cache entry was set in the first forward pass as
                    // Kind::Named, TypeAliasSymbol.  This entry is also never
                    // refreshed by the alias-transparency arm, so every consumer
                    // that reads ctx_.resolvedType(t) (including binding-pattern
                    // type annotation resolution) gets the stale entry.
                    // Updating both entries ensures the Type node's cache is
                    // refreshed whenever the alias chain is followed.
                    ctx_.setResolvedType(
                        static_cast<const ast::Type*>(
                            aliasDecl->aliasedType.get()),
                        &ref);
                    ctx_.setResolvedType(&t, &ref);
                    return;
                }
            }
        }
        // Unknown symbol kind (or alias with unresolved underlying type) — treat
        // as a Named pointing to no declaration, matching the prior behaviour.
        ref.kind = TypeRef::Kind::Named;
        ref.declaration = nullptr;
    } else {
        // per §11.3 / Grammar A.8: follow each remaining part of the
        // qualified name as a nested-type lookup within the current TypeSymbol.
        for (std::size_t pi = 1; pi < t.name->parts.size(); ++pi) {
            std::string_view part = t.name->parts[pi];
            TypeSymbol* inner = nullptr;
            for (Symbol* m : tsym->members) {
                if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                    if (nt->name == part && nt->type) {
                        inner = nt->type;
                        break;
                    }
                }
            }
            if (!inner) {
                ctx_.diagnostics().report(
                    Severity::Error, t.span.begin,
                    std::string("type `") + std::string(tsym->name) +
                    "` has no nested type named `" + std::string(part) +
                    "` (§11.3)");
                ref.kind = TypeRef::Kind::Error;
                return;
            }
            tsym = inner;
        }
        ref.kind = TypeRef::Kind::Named;
        ref.declaration = tsym;
        // Generic-arity check (§14.1 / §2.8.3 / Appendix C.1): every type reference
        // supplies exactly the declared number of type arguments.  A bare reference
        // to a generic type (arity 0) must be rejected just as a wrong-arity
        // reference is; the first conjunct (`!t.genericArgs.empty()`) suppressed
        // precisely the bare-reference case that §14.1 makes an error.
        if (t.genericArgs.size() != tsym->genericParams.size()) {
            // fix #5172 (§2.2, v0.310): if the type's bare name has a
            // matching builtin arity in `builtinTypeArity` (a §2.3
            // always-available candidate the local non-generic declaration
            // shadowed in the bare-name scope), the spec's arity-selects
            // rule means the arity error is misleading — the user's
            // arity actually matches the §2.3 builtin. Redirect to
            // Slice S24 (which knows the always-available builtin set) by
            // re-resolving with an empty scope, so it can emit the
            // proper "does not match built-in type" diagnostic (or, for
            // a name that IS a builtin at the same arity, none at all).
            int builtinAr = (t.name && t.name->parts.size() == 1)
                                ? builtinTypeArity(t.name->parts.front())
                                : kNotBuiltin;
            if (builtinAr != kNotBuiltin &&
                builtinAr == static_cast<int>(t.genericArgs.size())) {
                // Local declaration's arity doesn't match the
                // reference's; but the §2.3 always-available builtin
                // does. Clear the local declaration so Slice S24 can
                // re-resolve cleanly (ref.declaration=nullptr is the
                // sentinel Slice S24 emits), and skip the misleading
                // arity error. Slice S24's own re-resolution will be
                // a no-op for this site, but the ref is left in a
                // state downstream passes can recognise.
                ref.declaration = nullptr;
                // Resolve the generic arguments so downstream passes
                // see them (the error path otherwise leaves the ref
                // half-built).
                ref.genericArgs.clear();
                for (const auto& ga : t.genericArgs) {
                    if (!ga) continue;
                    ref.genericArgs.push_back(resolveType(*ga, scope));
                }
            } else {
                ctx_.diagnostics().report(
                    Severity::Error, t.span.begin,
                    "generic-argument count does not match type's "
                    "generic-parameter count");
            }
        }
    }
    for (const auto& ga : t.genericArgs) {
        if (!ga) continue;
        TypeRef* arg = resolveType(*ga, scope);
        ref.genericArgs.push_back(arg);
    }
    // §14.2 / Appendix C.4: validate that each supplied type argument
    // satisfies the constraints declared on its type parameter. Only for a
    // resolved user TypeSymbol whose arity matches (a mismatch is already
    // diagnosed above; checking a misaligned arg list would be noise).
    if (tsym && !t.genericArgs.empty() &&
        t.genericArgs.size() == tsym->genericParams.size()) {
        checkGenericConstraints(t, tsym, ref.genericArgs, scope);
        // fix #4582: collect the spelled argument AST nodes and hand the
        // whole instantiation to the ONE shared recorder, which the
        // static-member-access receiver path calls too.
        std::vector<const ast::Type*> argAsts;
        argAsts.reserve(t.genericArgs.size());
        for (const auto& ga : t.genericArgs) argAsts.push_back(ga.get());
        recordGenericInstantiation(tsym, ref.genericArgs, argAsts,
                                   /*siteType=*/&t, /*siteKey=*/&t,
                                   t.span.begin);
    }
}

void Resolver::resolveTypesInBody(
    const std::vector<std::unique_ptr<ast::Stmt>>& body, Scope* scope) {
    for (const auto& s : body) {
        if (s) resolveTypesInStmt(*s, scope);
    }
}

void Resolver::resolveTypesInStmt(const ast::Stmt& s, Scope* scope) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        if (ld->type) resolveType(*ld->type, scope);
        if (ld->initializer) resolveTypesInExpr(*ld->initializer, scope);
        // fix #5111 (§14.6, v0.307): record this `var`-local's AST node in
        // the Resolver-private `currentAnnotatedLocals_` side table (keyed
        // by name), so a later self-recursive call through it
        // (`p.Go<...>(...)`) is found by
        // resolveGenericSelfEdgeReceiverType's variable-receiver lookup —
        // see currentAnnotatedLocals_'s own comment for why this is a plain
        // map and NOT a Symbol inserted into `scope` (a first attempt did
        // that and non-author review measured it created a second,
        // null-declType LocalSymbol per local, corrupting unrelated
        // by-name lookups in ExprTypecheck). Before this, no local-variable
        // receiver was ever found and an expanding generic self-call
        // through one hung the compiler instead of being rejected
        // (#5045/#5082's residual). Only an explicitly `: Type`-annotated
        // local is recorded: an unannotated one's type isn't known until
        // ExprTypecheck's inference, and resolveGenericSelfEdgeReceiverType
        // only ever reads `ld->type`/`lc->type` (never an inferred type),
        // so an unannotated local would resolve to null there regardless.
        if (ld->type) currentAnnotatedLocals_[ld->name] = ld;
        return;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        if (lc->type) resolveType(*lc->type, scope);
        if (lc->value) resolveTypesInExpr(*lc->value, scope);
        // fix #5111 (§14.6, v0.307): same recording for a `const` local —
        // see the `var` arm above for the full rationale.
        if (lc->type) currentAnnotatedLocals_[lc->name] = lc;
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value) resolveTypesInExpr(*rs->value, scope);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) resolveTypesInExpr(*es->expr, scope);
        return;
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->target) resolveTypesInExpr(*as->target, scope);
        if (as->value) resolveTypesInExpr(*as->value, scope);
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) resolveTypesInExpr(*ds->value, scope);
        return;
    }
    if (auto* tr = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        if (tr->value) resolveTypesInExpr(*tr->value, scope);
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        if (ifs->ifBranch.condition)
            resolveTypesInExpr(*ifs->ifBranch.condition, scope);
        resolveTypesInBody(ifs->ifBranch.body, scope);
        for (const auto& el : ifs->elifBranches) {
            if (el.condition) resolveTypesInExpr(*el.condition, scope);
            resolveTypesInBody(el.body, scope);
        }
        resolveTypesInBody(ifs->elseBody, scope);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        if (ws->condition) resolveTypesInExpr(*ws->condition, scope);
        resolveTypesInBody(ws->body, scope);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        if (fs->varType) resolveType(*fs->varType, scope);
        if (fs->iterable) {
            // fix #4554 (§16.5, v0.307): an open-ended range literal directly
            // in a `for` iterable position cannot be iterated — the spec
            // requires the compiler to emit a warning at the literal site.
            // Defer the runtime check (a `Range<T>` variable of an open-ended
            // shape raises InvalidOperationException at GetEnumerator) to
            // codegen; only the literal-position case is the compile-time
            // warning §16.5 mandates.
            if (!fs->isAwait) {
                if (auto* re = dynamic_cast<const ast::RangeExpr*>(
                        fs->iterable.get())) {
                    if (!re->lo || !re->hi) {
                        ctx_.diagnostics().report(
                            Severity::Warning, re->span.begin,
                            std::string(
                                "open-ended range literal cannot be iterated "
                                "(\xC2\xA7""16.5)"));
                    }
                }
            }
            resolveTypesInExpr(*fs->iterable, scope);
        }
        if (fs->withExpr) resolveTypesInExpr(*fs->withExpr, scope);
        resolveTypesInBody(fs->body, scope);
        return;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        resolveTypesInBody(dob->body, scope);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        resolveTypesInBody(ts->body, scope);
        for (const auto& arm : ts->catches) {
            // Resolve the catch pattern's type(s) and guard, not just the
            // body — else `catch T => …` leaves T with no resolved
            // TypeRef and codegen cannot match the arm.
            for (const auto& ty : arm.types) {
                if (ty) resolveType(*ty, scope);
            }
            if (arm.guard) resolveTypesInExpr(*arm.guard, scope);
            if (arm.body) resolveTypesInStmt(*arm.body, scope);
        }
        resolveTypesInBody(ts->finallyBody, scope);
        return;
    }
    if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        if (is->target) resolveTypesInExpr(*is->target, scope);
        return;
    }
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        if (ys->value) resolveTypesInExpr(*ys->value, scope);
        return;
    }
    // §19: statement-position `case`. Resolve the scrutinee, each arm's
    // pattern type (so a typed binding `var x: T` resolves T), guard, and
    // arm-body statements. Previously CaseStmt was not handled here at all,
    // so a member-access guard over a `var x: T` binding had no resolved
    // member symbol and codegen emitted malformed IR.
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        if (cs->scrutinee) resolveTypesInExpr(*cs->scrutinee, scope);
        for (const auto& arm : cs->arms) {
            if (arm.pattern) resolveTypesInPattern(*arm.pattern, scope);
            if (arm.guard) resolveTypesInExpr(*arm.guard, scope);
            if (arm.exprBody) resolveTypesInExpr(*arm.exprBody, scope);
            for (const auto& st : arm.stmts) {
                if (st) resolveTypesInStmt(*st, scope);
            }
        }
        return;
    }
}

void Resolver::resolveTypesInPattern(const ast::Pattern& p, Scope* scope) {
    // §19: a typed binding `var x: T` (BindingPattern) and a primitive type
    // pattern `T` (TypePattern) each carry a Type node that must be resolved
    // so ExprTypecheck can type the bound name / the type test. A
    // DestructuringPattern composes sub-patterns (`Point(var x: T, ...)`),
    // each of which may itself carry a type node, so recurse. This mirrors
    // the catch-arm pattern-type resolution in resolveTypesInStmt (TryStmt).
    if (auto* bp = dynamic_cast<const ast::BindingPattern*>(&p)) {
        if (bp->type) resolveType(*bp->type, scope);
        return;
    }
    if (auto* tp = dynamic_cast<const ast::TypePattern*>(&p)) {
        if (tp->type) resolveType(*tp->type, scope);
        return;
    }
    if (auto* dp = dynamic_cast<const ast::DestructuringPattern*>(&p)) {
        for (const auto& sp : dp->subPatterns) {
            if (sp.pattern) resolveTypesInPattern(*sp.pattern, scope);
        }
        return;
    }
    // §19 / §A (#727): a ConstantPattern in a SCRUTINEE-LESS `case` holds the
    // arm's boolean CONDITION expression (e.g. `x > 5`), whose identifiers must
    // be resolved like any expression. (In the scrutinee form the constants are
    // literals/enum members, for which this recursion is harmless.)
    if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(&p)) {
        for (const auto& c : cp->constants)
            if (c) resolveTypesInExpr(*c, scope);
        return;
    }
    // §19.3 (v0.183, #1584): a heterogeneous alternative list — resolve names
    // in each member (a ConstantPattern member's constant exprs; a range or
    // relational member carries no name node this pass handles, matching the
    // top-level treatment of those kinds).
    if (auto* ap = dynamic_cast<const ast::AltPattern*>(&p)) {
        for (const auto& m : ap->members)
            if (m) resolveTypesInPattern(*m, scope);
        return;
    }
    // fix #4554 (§16.6, v0.307) is handled by the pre-existing emission at
    // src/sema/ExprTypecheck.cpp:9759 (the `checkRangePattern` lambda inside
    // the arm-resolution loop) — that site covers BOTH top-level RangePattern
    // AND members of an AltPattern, while this pass only sees top-level
    // RangePattern. Adding a parallel emission here caused s16_6_0004 to
    // emit the warning twice. The pre-existing site is the canonical one;
    // s16_6_0004 stays KNOWN divergent as a #4626 instance until the
    // runner gains a severity-rendering channel (#4626 ruling, 2026-09-23).
}

// fix #2911 + fix #2962 (§17.4 / §3.6 rule 2, v0.241): see the declarations in
// TypeResolution_impl.hpp.
void Resolver::recordAsTargetSite(const ast::Type& t, SourceLocation loc) {
    if (!currentOuterType_) return;  // module scope: no parameter to bind
    TypeRef* ref = ctx_.resolvedType(t);
    // Only a BARE parameter defers — a concretely-spelled degenerate target is
    // already rejected outright by ExprTypecheck, and every other concrete
    // target is admissible. `isBareGenericParamBinding` is the SAME predicate
    // #2111/#2713 use for "this binding is itself a type parameter" (it also
    // excludes primitives and builtin names, which carry no declaration
    // either).
    if (!isBareGenericParamBinding(ref, &t)) return;
    const std::string_view name = t.name->parts.front();
    // #2713's misattribution guard, mirrored: a generic METHOD may declare a
    // parameter of the same name, SHADOWING the enclosing type's, and nothing
    // forbids it. Inside such a method the name denotes the METHOD's parameter,
    // whose bindings this pass never sees; recording it would validate the
    // method's `T` against the type's and FALSELY REJECT a legal program. A
    // method parameter that does not shadow is skipped by the same test, which
    // is correct for the same reason — and is not a residual here, because
    // ExprTC::checkDeferredAsTargetInstantiations owns exactly that case.
    // fix #2742: one shared predicate (Resolver::nameBoundByCurrentMethod),
    // not a third hand-inlined copy of the loop.
    if (nameBoundByCurrentMethod(name)) return;
    asTargetSites_.push_back({name, currentOuterType_, loc});
}

// fix #5082 (§14.6, v0.307): find a FIELD member declared on `type` or
// inherited through its base-type chain, by name. Mirrors ExprTC::lookupMember
// (ExprTypecheck.cpp)'s BFS shape, but that helper lives on ExprTC and this
// walk is needed from the Resolver's earlier pass (recordGenericMemberSelfEdge
// runs before ExprTC exists for the enclosing method), so it is a small local
// copy rather than a shared one. Interface members never include fields, so
// walking every `baseTypes` entry (not just classes) is harmless: no
// `FieldSymbol` is ever found there.
static FieldSymbol* findFieldInTypeChain(TypeSymbol* type,
                                          std::string_view name) {
    std::vector<TypeSymbol*> frontier{type};
    for (std::size_t i = 0; i < frontier.size(); ++i) {
        TypeSymbol* t = frontier[i];
        if (!t) continue;
        for (Symbol* m : t->members) {
            if (auto* fs = dynamic_cast<FieldSymbol*>(m))
                if (fs->name == name) return fs;
        }
        for (TypeSymbol* b : t->baseTypes) frontier.push_back(b);
    }
    return nullptr;
}

// fix #5082 (§14.6, v0.307): resolve a FieldSymbol's declared type from
// whichever side actually carries it — mirrors the LOCAL/imported split
// `ownedBareTypeParamOfField` (above) and `fieldDeclTypeOf`
// (FieldSymbolAccessors.hpp) each already document: a positional or
// body-declared field's type is on its AST node (`fieldDeclTypeOf` covers
// both), but an IMPORTED field has `decl == nullptr` and no positional param
// either, so its type rides `SemaContext::importedMemberType` instead
// (ModuleLoader's side table, #3149).
static const TypeRef* selfEdgeFieldType(const FieldSymbol& fs,
                                         SemaContext& ctx) {
    if (const ast::Type* t = fieldDeclTypeOf(&fs))
        return ctx.resolvedType(*t);
    return ctx.importedMemberType(&fs);
}

// fix #5082 (§14.6, v0.307): the ONE receiver-type resolver shared by both
// recordGenericMemberSelfEdge overloads below (see their declarations in
// TypeResolution_impl.hpp for what "receiver" means and why an over-recorded
// edge, not an under-recorded one, is the failure mode to avoid). `recv` is
// the expression a `.member<...>`/`?.member<...>` call is accessed on.
//
// R22 integrator review of the original attempt at this fix (268a6c4e,
// p22-5082-c27) found two defects, both fixed here:
//   (1) the resolution DROPPED #5045's e5138578 ParameterSymbol lookup, its
//       `currentMethodDecl_->params` fallback scan, and the receiver
//       identifier's `typeArgs.empty()` guard. All three are preserved
//       below, verbatim in behavior.
//   (2) the field-chain walk accepted ANY `this.a.b` chain rooted at `this`
//       without checking that any link's OWN declared type is the enclosing
//       type — so `this.helper.Go<...>` would be misrecorded as a self-edge
//       even when `helper` is a different type's field that merely happens
//       to declare a same-named, same-arity generic method. This resolver
//       walks the chain FORWARD from the root so each link's real field type
//       gates whether the NEXT link is even consulted; the final link's type
//       is what must equal the enclosing type.
//
// It is also the fix for #5082's title ("variable receivers found only via
// params in ?. arms; fields/locals missed"): before this, the `.` form (the
// MemberAccessExpr arm) found `this`, the type name, and — via `lookupName`
// — a local or parameter, but rejected any field-access chain outright
// (`ma->target` had to BE a bare IdentifierExpr). Both `?.` arms (the
// NullCondMemberExpr arm nested in the CallExpr overload, and the standalone
// NullCondMemberExpr overload used for `other?.M<...>() ?? 0`) found only
// `this`, the type name, and a variable receiver via a direct scan of
// `currentMethodDecl_->params` — never a local, never a field. Routing every
// arm through this one resolver makes all three see locals, params and field
// chains identically.
TypeSymbol* Resolver::resolveGenericSelfEdgeReceiverType(const ast::Expr& recv,
                                                          Scope* scope) {
    // Collect the field-access chain from its OUTERMOST link (closest to the
    // call) down to the root identifier, e.g. `this.a.b` collects ["b", "a"].
    std::vector<std::string_view> fieldChain;
    const ast::Expr* cursor = &recv;
    while (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(cursor)) {
        fieldChain.push_back(ma->member);
        cursor = ma->target.get();
    }

    auto* rootId = dynamic_cast<const ast::IdentifierExpr*>(cursor);
    if (!rootId || !rootId->typeArgs.empty()) return nullptr;

    TypeSymbol* rootType = nullptr;
    if (rootId->name == "this") {
        rootType = currentOuterType_;
    } else if (fieldChain.empty() && rootId->name == currentOuterType_->name) {
        // A type-name-qualified chain (`C.someField.M<...>`) is not a shape
        // either #5045 or #5082 needs to admit — declining it is an
        // under-recording, which is always safe (see the comment above).
        rootType = currentOuterType_;
    } else if (scope) {
        // fix #5045 (§14.6, v0.307): a variable receiver — local or
        // parameter — of the enclosing type is a potential self-recursive
        // call through that variable. Preserved verbatim from e5138578 (the
        // R22 integrator review's point (1)): both the `LocalSymbol`/
        // `ParameterSymbol` lookup via `lookupName` AND the direct
        // `currentMethodDecl_->params` fallback (scope may not contain the
        // method's own formals at this point in the walk) must survive.
        Symbol* sym = lookupName(rootId->name, scope);
        const TypeRef* recvType = nullptr;
        if (sym) {
            if (auto* ls = dynamic_cast<LocalSymbol*>(sym)) {
                if (auto* ld = dynamic_cast<const ast::LocalDecl*>(ls->decl))
                    recvType = ld->type ? ctx_.resolvedType(*ld->type) : nullptr;
                // §21.1 / §14.6 (v0.307, #3591 family): a `const` local is a
                // different AST node (LocalConstDecl) but the same alias of a
                // value of the enclosing type.
                else if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(
                             ls->decl))
                    recvType = lc->type ? ctx_.resolvedType(*lc->type) : nullptr;
            } else if (auto* ps = dynamic_cast<ParameterSymbol*>(sym)) {
                recvType = ps->resolvedDeclType;
                if (!recvType && ps->declType)
                    recvType = ctx_.resolvedType(*ps->declType);
            }
        }
        // Also check method parameters directly — scope may not contain them.
        if (!recvType && currentMethodDecl_) {
            for (const auto& param : currentMethodDecl_->params) {
                if (param.name == rootId->name) {
                    recvType = param.type ? ctx_.resolvedType(*param.type)
                                          : nullptr;
                    break;
                }
            }
        }
        // fix #5111 (§14.6, v0.307): a LOCAL variable receiver — `lookupName`
        // above can never find one (Resolver registers no LocalSymbol; see
        // currentAnnotatedLocals_'s own comment for why this is a private
        // side table rather than a Symbol). Consult the annotated-locals
        // recorded so far, in statement order, by resolveTypesInStmt.
        if (!recvType) {
            auto it = currentAnnotatedLocals_.find(rootId->name);
            if (it != currentAnnotatedLocals_.end()) {
                if (auto* ld = dynamic_cast<const ast::LocalDecl*>(it->second))
                    recvType = ld->type ? ctx_.resolvedType(*ld->type) : nullptr;
                else if (auto* lc =
                             dynamic_cast<const ast::LocalConstDecl*>(it->second))
                    recvType = lc->type ? ctx_.resolvedType(*lc->type) : nullptr;
            }
        }
        if (recvType && recvType->kind == TypeRef::Kind::Named)
            rootType = recvType->declaration;
    }
    if (!rootType) return nullptr;

    // fix #5082 (§14.6, v0.307), review point (2): walk the field chain
    // FORWARD from the root, checking each link is a real field and
    // threading ITS OWN declared type to the next link — a chain through an
    // unrelated type's field (`this.helper.Go<...>` where `helper` is a
    // DIFFERENT class) must fail here rather than being accepted just
    // because the chain happens to start at `this`.
    TypeSymbol* cur = rootType;
    for (auto it = fieldChain.rbegin(); it != fieldChain.rend(); ++it) {
        if (!cur) return nullptr;
        FieldSymbol* fs = findFieldInTypeChain(cur, *it);
        if (!fs) return nullptr;
        const TypeRef* fieldType = selfEdgeFieldType(*fs, ctx_);
        cur = (fieldType && fieldType->kind == TypeRef::Kind::Named)
                  ? fieldType->declaration
                  : nullptr;
    }
    return cur;
}

// §14.6 (v0.262, #3271): record `call` as a generic member's SELF-edge when
// its callee spells the enclosing generic method's own name with explicit type
// arguments. The four admitted spellings are the four ways a member can name
// itself without going through another declaration:
//
//     M<...>(…)        — unqualified, the ordinary self-recursive call
//     this.M<...>(…)   — instance-qualified
//     C.M<...>(…)      — enclosing-type-qualified (the only spelling a static
//                        member of `static class C` can use besides the bare one)
//     v.M<...>(…)      — through a variable/field `v` whose declared type is
//                        the enclosing type (fix #5045, fix #5082)
//
// Anything else — a call through an unrelated type, an interface, a base
// projection, or another declaration — is NOT recorded. §14.6's rule is
// per-edge over a strongly-connected component, and a direct self-loop is the
// one edge class whose SCC membership is decidable without a resolved call
// graph: a self-loop is unconditionally inside its own component. Every other
// member edge needs the call graph and is the recorded residual (see
// checkExpandingGenericMemberEdges in TypeResolution_Checks.cpp).
//
// Recording is deliberately generous and the VERDICT is where the guards live
// (the overload guard in particular) — recording is side-effect-free, so an
// over-recorded edge costs a vector entry and nothing else. (An over-
// ACCEPTED receiver is a different failure mode — see
// resolveGenericSelfEdgeReceiverType's own comment.)
void Resolver::recordGenericMemberSelfEdge(const ast::CallExpr& call, Scope* scope) {
    if (!currentMethodDecl_ || !currentOuterType_) return;
    // Only a GENERIC member has a self-edge to constrain: a non-generic
    // member's edges attach to its declaring type's node (§14.6), and those
    // are already carried by instantiations_.
    const std::size_t arity = currentMethodDecl_->genericParams.size();
    if (arity == 0) return;
    if (!call.callee) return;

    const std::vector<std::unique_ptr<ast::Type>>* typeArgs = nullptr;
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(call.callee.get())) {
        if (id->name != currentMethodDecl_->name) return;
        typeArgs = &id->typeArgs;
    } else if (auto* ma =
                   dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
        if (ma->member != currentMethodDecl_->name) return;
        // fix #5082 (§14.6, v0.307): the receiver may be `this`, the
        // enclosing type's own name, a local/parameter of the enclosing
        // type, or a field-access chain (`this.a.b`) whose final link is
        // typed as the enclosing type — see
        // resolveGenericSelfEdgeReceiverType.
        if (!ma->target ||
            resolveGenericSelfEdgeReceiverType(*ma->target, scope) !=
                currentOuterType_)
            return;
        typeArgs = &ma->typeArgs;
    } else if (auto* nc = dynamic_cast<const ast::NullCondMemberExpr*>(
                   call.callee.get())) {
        // fix #5027 (§14.6, v0.307): `this?.M<...>(...)` is a fourth spelling
        // of the same self-loop as `this.M<...>(...)` — `this` is never null
        // (§7.9), so the `?.` is a no-op wrapper around the identical access,
        // yet this branch previously handled only MemberAccessExpr (the `.`
        // form) and IdentifierExpr (the unqualified form), so the edge went
        // unrecorded and the expanding-instantiation check never ran on it.
        // MEASURED: `this?.Go<Pair<T, T>>(p)` inside `Go<T>` compiled past
        // sema and hung the compiler in monomorphization (unbounded
        // specialization family, the same pre-#3271 failure mode) rather
        // than being rejected like its `this.Go<Pair<T, T>>(p)` twin.
        // Unlike MemberAccessExpr, the enclosing-type-qualified spelling
        // (`C.M<...>`) has no `?.` counterpart: a type name is not a value
        // that can be null, so only `this` is admitted here (via
        // resolveGenericSelfEdgeReceiverType, same as the `.` arm above).
        //
        // fix #5082 (§14.6, v0.307): this arm previously found only `this`
        // and a variable via a direct `currentMethodDecl_->params` scan — no
        // locals, no fields. Route through the shared resolver so it matches
        // the `.` arm exactly.
        if (nc->member != currentMethodDecl_->name) return;
        if (!nc->target ||
            resolveGenericSelfEdgeReceiverType(*nc->target, scope) !=
                currentOuterType_)
            return;
        typeArgs = &nc->typeArgs;
    } else {
        return;
    }
    // The spelled type-argument count must match the member's declared arity,
    // or the callee is some other member (or an error reported elsewhere).
    if (!typeArgs || typeArgs->size() != arity) return;

    GenericMemberEdge edge;
    edge.enclosingType = currentOuterType_;
    edge.source = currentMethodDecl_;
    edge.loc = call.span.begin.line != 0 ? call.span.begin
                                         : currentMethodDecl_->span.begin;
    for (const auto& ta : *typeArgs) {
        edge.argRefs.push_back(ta ? ctx_.resolvedType(*ta) : nullptr);
        edge.argAsts.push_back(ta.get());
    }
    genericMemberEdges_.push_back(std::move(edge));
}

// fix #5045 (§14.6, v0.307): overload for NullCondMemberExpr, which is the
// callee inside a NullCondCallExpr (the `?.` form). The logic is identical to
// the NullCondMemberExpr branch inside the CallExpr overload, but wrapped in
// its own free function so the NullCondCallExpr arm can call it directly.
void Resolver::recordGenericMemberSelfEdge(const ast::NullCondMemberExpr& ncm,
                                          Scope* scope) {
    if (!currentMethodDecl_ || !currentOuterType_) return;
    const std::size_t arity = currentMethodDecl_->genericParams.size();
    if (arity == 0) return;

    if (ncm.member != currentMethodDecl_->name) return;
    // fix #5082 (§14.6, v0.307): this standalone overload previously found
    // only `this`, the type name, and a variable via a direct
    // `currentMethodDecl_->params` scan — no locals, no fields (#5082's
    // title's "?. arms" half). Route through the shared resolver so
    // `other?.Go<...>() ?? 0` finds a local/field receiver exactly as the
    // `.` and nested-`?.` arms above now do.
    if (!ncm.target ||
        resolveGenericSelfEdgeReceiverType(*ncm.target, scope) !=
            currentOuterType_)
        return;

    const std::vector<std::unique_ptr<ast::Type>>* typeArgs = &ncm.typeArgs;
    if (!typeArgs || typeArgs->size() != arity) return;

    GenericMemberEdge edge;
    edge.enclosingType = currentOuterType_;
    edge.source = currentMethodDecl_;
    edge.loc = ncm.span.begin.line != 0 ? ncm.span.begin
                                        : currentMethodDecl_->span.begin;
    for (const auto& ta : *typeArgs) {
        edge.argRefs.push_back(ta ? ctx_.resolvedType(*ta) : nullptr);
        edge.argAsts.push_back(ta.get());
    }
    genericMemberEdges_.push_back(std::move(edge));
}

void Resolver::resolveTypesInExpr(const ast::Expr& e, Scope* scope) {
    // fix #4554 (§16.4, v0.307): when a `RangeExpr` carries a `step` and all
    // three of `lo`, `hi`, `step` are compile-time-constant integer expressions,
    // the iteration's direction can be proven at compile time. A range whose
    // direction (ascending when lo <= hi, descending when lo >= hi) disagrees
    // with its step's sign produces an empty iteration — emit the §16.4
    // "empty iteration" warning. Only fires on provably-constant ranges;
    // runtime-computed steps are intentionally left alone because a sign
    // mismatch there may be intentional (the value depends on input).
    //
    // §16.4 has an explicit equal-bounds exception: `lo..hi` with lo == hi
    // yields exactly one value regardless of step sign (§16.6, inclusive
    // equal-bounds); `lo..<hi` with lo == hi is empty regardless of step
    // sign (§16.6, half-open equal-bounds). Encode that here so neither
    // shape produces a false-positive warning.
    if (auto* re = dynamic_cast<const ast::RangeExpr*>(&e)) {
        if (re->lo && re->hi && re->step) {
            auto loV = foldIntConstExpr(*re->lo, scope, 0);
            auto hiV = foldIntConstExpr(*re->hi, scope, 0);
            auto stV = foldIntConstExpr(*re->step, scope, 0);
            if (loV && hiV && stV && *stV != 0) {
                bool empty;
                if (re->inclusive) {
                    // `lo..hi` ascending with step<0, OR descending with step>0.
                    // Equal bounds (lo == hi): one value, NOT empty.
                    empty = (*loV < *hiV && *stV < 0) ||
                            (*loV > *hiV && *stV > 0);
                } else {
                    // `lo..<hi` ascending (lo >= hi) with step>0, OR
                    // descending (lo < hi) with step<0. Equal bounds
                    // (lo == hi): empty for any step (hi excluded).
                    empty = (*loV < *hiV && *stV < 0) ||
                            (*loV >= *hiV && *stV > 0);
                }
                if (empty) {
                    ctx_.diagnostics().report(
                        Severity::Warning, re->span.begin,
                        "this range produces an empty iteration — "
                        "the range's direction (lo " +
                            std::string(*loV <= *hiV ? "<=" : ">=") +
                            " hi) disagrees with the step's sign "
                            "(\xC2\xA7""16.4)");
                }
            }
        }
        // Recurse into the children so the bounds / step are themselves
        // type-resolved.
        if (re->lo) resolveTypesInExpr(*re->lo, scope);
        if (re->hi) resolveTypesInExpr(*re->hi, scope);
        if (re->step) resolveTypesInExpr(*re->step, scope);
        return;
    }
    // First handle expressions that own a Type node directly.
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        if (ne->type) resolveType(*ne->type, scope);
        for (const auto& a : ne->args) {
            if (a.value) resolveTypesInExpr(*a.value, scope);
        }
        return;
    }
    if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        if (te->testType) resolveType(*te->testType, scope);
        // fix #2911 + fix #2962 (§17.4 / §3.6 rule 2, v0.241): `as` at a bare
        // TYPE-parameter target defers to the enclosing generic's instantiation
        // sites (checkAsTargetInstantiations). `is` / `is not` are deliberately
        // NOT recorded: §17.4 rejects only the operator whose result type must
        // exist and be informative, and §12.7's type TEST at those same targets
        // is exactly the remedy the diagnostic names.
        if (te->op == "as" && te->testType)
            recordAsTargetSite(*te->testType, e.span.begin);
        if (te->operand) resolveTypesInExpr(*te->operand, scope);
        return;
    }
    if (auto* to = dynamic_cast<const ast::TypeOfExpr*>(&e)) {
        if (to->queriedType) resolveType(*to->queriedType, scope);
        return;
    }
    if (auto* de = dynamic_cast<const ast::DefaultExpr*>(&e)) {
        if (de->type) resolveType(*de->type, scope);
        return;
    }
    // Recurse into children for everything else.
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left) resolveTypesInExpr(*be->left, scope);
        if (be->right) resolveTypesInExpr(*be->right, scope);
        // fix #5045 (§14.6, v0.307): `other?.Go<...>(p) ?? 0` parses as a
        // BinaryExpr with NullCondCallExpr as the left child. Both children
        // are recursed into above, but neither records a generic-member
        // self-edge — only the CallExpr and NullCondCallExpr arms do that.
        // Record it here so the expanding-instantiation check fires.
        if (be->op == "??") {
            if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(be->left.get()))
                if (auto* ncm = dynamic_cast<const ast::NullCondMemberExpr*>(
                        nc->callee.get()))
                    recordGenericMemberSelfEdge(*ncm, scope);
        }
        return;
    }
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (ue->operand) resolveTypesInExpr(*ue->operand, scope);
        return;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) resolveTypesInExpr(*ma->target, scope);
        // per §14.1 (issue #509): resolve any explicit method type args
        // (e.g. `.Identity<int>` → typeArgs[0] resolved to int TypeRef).
        for (const auto& ta : ma->typeArgs) {
            if (ta) resolveType(*ta, scope);
        }
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (nc->target) resolveTypesInExpr(*nc->target, scope);
        // fix #4813 follow-up (§7.5 / §14.5, v0.307): `recv?.Method<T>(...)`
        // carries its explicit method type args exactly as the `.` form
        // does (#1278), so resolve them here too. Unresolved, every
        // `ctx_.resolvedType(ta)` consumer (typeCall's explicit substitution,
        // codegen's explicit binding) saw null: the call counted as having an
        // explicit list but bound nothing, and once #4935 stopped argument
        // inference from backfilling it, `b?.Ret<int>(6)` was typed as the
        // callee's bare `T` (and `b?.Ret<long>(6)` was inferred as `int`).
        for (const auto& ta : nc->typeArgs) {
            if (ta) resolveType(*ta, scope);
        }
        return;
    }
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (ce->callee) resolveTypesInExpr(*ce->callee, scope);
        for (const auto& a : ce->args) {
            if (a.value) resolveTypesInExpr(*a.value, scope);
        }
        // §14.6 (v0.262, #3271): record a generic member's SELF-edge. The
        // callee is inspected AFTER the recursion above, so each spelled type
        // argument already has a resolved TypeRef to record. §14.6 makes each
        // generic member its own node; `instantiations_` carries generic TYPE
        // nodes only, so this is the one place a member self-loop is visible.
        recordGenericMemberSelfEdge(*ce, scope);
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        if (nc->callee) resolveTypesInExpr(*nc->callee, scope);
        for (const auto& a : nc->args) {
            if (a.value) resolveTypesInExpr(*a.value, scope);
        }
        // fix #5045 (§14.6, v0.307): `other?.Go<...>() ?? 0` has a
        // NullCondCallExpr whose callee is a NullCondMemberExpr. The CallExpr
        // arm above calls recordGenericMemberSelfEdge for calls where callee IS
        // a CallExpr, but a NullCondCallExpr's callee is a NullCondMemberExpr,
        // so it takes the NullCondCallExpr arm and the edge went unrecorded.
        // The callee (NullCondMemberExpr) was already recursed into above, so
        // its type args are resolved when we call the overload here.
        if (auto* ncm =
                dynamic_cast<const ast::NullCondMemberExpr*>(nc->callee.get()))
            recordGenericMemberSelfEdge(*ncm, scope);
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) resolveTypesInExpr(*ix->target, scope);
        for (const auto& a : ix->indices) {
            if (a.value) resolveTypesInExpr(*a.value, scope);
        }
        return;
    }
    if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (bg->operand) resolveTypesInExpr(*bg->operand, scope);
        return;
    }
    if (auto* ce = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (ce->condition) resolveTypesInExpr(*ce->condition, scope);
        if (ce->thenValue) resolveTypesInExpr(*ce->thenValue, scope);
        if (ce->elseValue) resolveTypesInExpr(*ce->elseValue, scope);
        return;
    }
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        if (th->operand) resolveTypesInExpr(*th->operand, scope);
        return;
    }
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        // Lambda parameter types are themselves Types when present.
        for (const auto& p : lam->params) {
            if (p.type) {
                TypeRef* pt = resolveType(*p.type, scope);
                // §28.1 "Type requirement" (v0.193): an `owned` lambda
                // parameter with an EXPLICIT type must carry a class,
                // interface, or bare-type-parameter type. `(owned x: int) =>
                // ...` is rejected. (An UNANNOTATED `owned` lambda parameter
                // has no type of its own here — its admissibility is governed
                // by the target function type's slot, checked at that function
                // type's resolution.)
                if (p.modifier == "owned" || p.modifier == "out owned")
                    checkOwnedDeclType(pt, lam->span.begin,
                                       "a lambda parameter");
            }
        }
        // §27.3 (v0.217, #2101): the optional return-type ascription is a
        // Type too — resolve it so ExprTypecheck can read it via
        // resolvedType() and treat it as the lambda's declared return type.
        if (lam->retType) resolveType(*lam->retType, scope);
        // fix #5111 (§14.6, v0.307): a lambda is ITS OWN function — locals
        // are function-scoped (§21.1), and a lambda body's own locals must
        // not leak into the enclosing method's `currentAnnotatedLocals_`
        // once the lambda ends (over-rejection review found this: a local
        // declared inside a lambda shadowed an outer field of the same name
        // for the rest of the enclosing method's self-edge check, wrongly
        // rejecting a field-receiver call after the lambda as if it were a
        // self-recursive call through the long-gone lambda-local). Save the
        // table before walking the lambda body and restore it after,
        // exactly as currentMethodDecl_/currentOuterType_ are saved/restored
        // around a nested scope elsewhere in this file. The saved copy is
        // NOT cleared first: a lambda body can still self-call through an
        // ENCLOSING local it captures (`v.M<...>()` inside the lambda, `v`
        // declared in the outer method) — the same as any other reference to
        // a captured variable — so the outer table must remain visible
        // while the lambda body is walked, not just after it.
        auto savedAnnotatedLocals = currentAnnotatedLocals_;
        if (lam->exprBody) resolveTypesInExpr(*lam->exprBody, scope);
        for (const auto& s : lam->stmts) {
            if (s) resolveTypesInStmt(*s, scope);
        }
        currentAnnotatedLocals_ = std::move(savedAnnotatedLocals);
        return;
    }
    if (auto* cae = dynamic_cast<const ast::CaseExpr*>(&e)) {
        if (cae->scrutinee) resolveTypesInExpr(*cae->scrutinee, scope);
        for (const auto& arm : cae->arms) {
            // §19: resolve the arm pattern's embedded Type nodes (the `T` in a
            // `var x: T` binding or a `T` type pattern) so ExprTypecheck can
            // type the bound name and codegen can resolve member accesses on it.
            if (arm.pattern) resolveTypesInPattern(*arm.pattern, scope);
            if (arm.guard) resolveTypesInExpr(*arm.guard, scope);
            if (arm.exprBody) resolveTypesInExpr(*arm.exprBody, scope);
            for (const auto& s : arm.stmts) {
                if (s) resolveTypesInStmt(*s, scope);
            }
        }
        return;
    }
    if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
        if (we->source) resolveTypesInExpr(*we->source, scope);
        for (const auto& ia : we->assigns) {
            if (ia.value) resolveTypesInExpr(*ia.value, scope);
        }
        return;
    }
    // §17.15 (v0.216, #2098): the binding atom — each clause's optional
    // ascription is a Type node (`var x: T := E;`, resolved exactly as the
    // §21.1 declaration's), each clause initializer and the body are
    // ordinary expression positions (a `new T(...)` initializer's Type node
    // is resolved by the NewExpr arm above through this recursion).
    if (auto* bx = dynamic_cast<const ast::BindExpr*>(&e)) {
        for (const auto& cl : bx->clauses) {
            if (!cl) continue;
            if (cl->type) resolveType(*cl->type, scope);
            if (cl->initializer) resolveTypesInExpr(*cl->initializer, scope);
        }
        if (bx->body) resolveTypesInExpr(*bx->body, scope);
        return;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
        for (const auto& k : cl->keys) {
            if (k) resolveTypesInExpr(*k, scope);
        }
        for (const auto& v : cl->elements) {
            if (v) resolveTypesInExpr(*v, scope);
        }
        return;
    }
    // §18.5: InterpolatedStringExpr — each expression part may contain type
    // nodes (e.g. `{x is IAnimal}` has a testType; `{new T(...)}`has a new-type).
    // Resolve them so that lowerTypeTest / lowerNew find the resolved TypeRef.
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
        for (const auto& part : is->parts) {
            if (part.expr) resolveTypesInExpr(*part.expr, scope);
        }
        return;
    }
    // per §14.1 / §22.12 (issue #520): an IdentifierExpr may carry explicit
    // receiver type arguments (`Registry<int>` in `Registry<int>.Make(...)`).
    // Resolve each type arg so that ctx_.resolvedType() can retrieve them
    // later in ExprTypecheck (the same treatment MemberAccessExpr.typeArgs
    // received for issue #509 at the branch above).
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        for (const auto& ta : id->typeArgs) {
            if (ta) resolveType(*ta, scope);
        }
        // fix #4582 (§3.6 rule 2, v0.306): the receiver is an INSTANTIATION of
        // that generic type, exactly as a type-position spelling is — record
        // it, or the whole §3.6 rule-2 Pass-2 family is blind at this
        // spelling. Runs after the loop above so every argument already has a
        // resolved TypeRef to record.
        recordStaticReceiverInstantiation(*id, scope);
        return;
    }
    // Literals + ThisBaseTypeOf — no nested Types/Exprs.
}

// fix #4038/#4048/#4050/#4051 (§27.8, v0.294): see the declaration comment
// in TypeResolution.hpp for the full rationale — the ONE shared resolution
// this family of "does this written name denote a type" classification
// predicates needs, reachable from both sema and codegen.
TypeSymbol* resolveAliasToTypeSymbol(SemaContext& ctx, TypeAliasSymbol* aliasSym) {
    if (!aliasSym) return nullptr;
    const auto* aliasDecl = dynamic_cast<const ast::TypeAlias*>(aliasSym->decl);
    if (!aliasDecl || !aliasDecl->aliasedType) return nullptr;
    TypeRef* underlying = ctx.resolvedType(*aliasDecl->aliasedType);
    if (!underlying || underlying->kind != TypeRef::Kind::Named) return nullptr;
    return underlying->declaration;
}

// fix #4554 (§2.7, v0.306): the chain-head warning row. A chain head
// resolves to a TYPE while a same-named accessible member exists on the
// enclosing type — per §2.7 the type wins for chain purposes (the rule-1
// "type outranks member" precedence for chain heads), but the spec asks
// for a declaration-site warning so the programmer can choose to qualify
// `this.` to reach the member. The carve-out: *unless* the member's
// declared type is the resolved type (or an instantiation of the resolved
// generic) — the `property Color: Color` idiom; in that shape the chain
// head and the member refer to the same store and the warning would be
// misleading.
//
// Plus the §2.7 *Consequences* sentence: "a type named `A` in scope
// shadows a module-name prefix `A.…` for chain purposes (the compiler
// warns when both readings exist — qualify via an alias to reach the
// module)". This is the second half of the same warning family: a chain
// head resolving to a local type while an imported module with the same
// name is also in scope (rule 2's prefix would have matched). The local
// type wins per §2.2 — locals beat imports — but the spec asks for the
// warning so the module remains reachable via an explicit alias.
//
// Called from ExprTypecheck_Exprs.cpp's IdentifierExpr handler right
// after the chain-head lookup resolves to a type. `shadowedEnclosingMember`
// is the same-named member in the enclosing type (nullable — null when
// the chain head's enclosing type has no such member); the warning row
// is suppressed when its declared type matches the resolved type. Both
// halves are independent — a case may fire only one, both, or neither.
void warnChainHeadTypeShadows(
    SemaContext& ctx,
    std::string_view headName,
    const TypeSymbol* resolvedType,
    const std::unordered_set<std::string>& sourceImports,
    const Symbol* shadowedEnclosingMember,
    SourceLocation loc) {
    // (A) §2.7 warning row: chain head resolves to a type while a
    // same-named accessible member exists. Carve-out: the member's
    // declared type must NOT be the resolved type — the `property
    // Color: Color` idiom is exactly the case where it IS, and the
    // warning would falsely advise `this.` qualification for what is
    // already the same store.
    if (shadowedEnclosingMember && resolvedType) {
        const TypeRef* memberType =
            storageComponentType(shadowedEnclosingMember, ctx);
        bool carveOut = false;
        if (memberType && memberType->kind == TypeRef::Kind::Named &&
            memberType->declaration == resolvedType) {
            carveOut = true;
        }
        // §2.7 generic carve-out: the member's declared type is an
        // instantiation of the resolved generic (e.g.
        // `property Buffer: Buffer<int>` with resolved type `Buffer`).
        // Detected when the member's declared TypeRef is Named, the
        // resolved type is a generic, and the member's declaration is
        // the same generic type symbol. (An exact-arity check isn't
        // necessary: a member declared with the wrong number of type
        // args would already have been rejected at the declaration
        // site, so a Named TypeRef whose declaration matches the
        // resolved generic is by construction an instantiation of it.)
        if (!carveOut && memberType &&
            memberType->kind == TypeRef::Kind::Named &&
            memberType->declaration == resolvedType &&
            !resolvedType->genericParams.empty()) {
            carveOut = true;
        }
        if (!carveOut) {
            std::string memberKind = "member";
            if (dynamic_cast<const FieldSymbol*>(shadowedEnclosingMember))
                memberKind = "field";
            else if (dynamic_cast<const PropertySymbol*>(
                         shadowedEnclosingMember))
                memberKind = "property";
            else if (dynamic_cast<const MethodSymbol*>(
                         shadowedEnclosingMember))
                memberKind = "method";
            else if (dynamic_cast<const ConstMemberSymbol*>(
                         shadowedEnclosingMember))
                memberKind = "const";
            else if (dynamic_cast<const EventSymbol*>(
                         shadowedEnclosingMember))
                memberKind = "event";
            ctx.diagnostics().report(
                Severity::Warning, loc,
                std::string("warning: chain head `") + std::string(headName) +
                    "` resolves to type `" +
                    std::string(resolvedType->name) +
                    "` while the enclosing type declares a same-named " +
                    memberKind + " — the type wins for chain purposes; "
                    "qualify with `this." + std::string(headName) +
                    "` to reach the " + memberKind +
                    " (\xC2\xA7""2.7)");
        }
    }

    // (B) §2.7 consequence: a type named `A` in scope shadows an imported
    // module-name prefix `A.…` — warn when both readings exist. The local
    // type wins per §2.2 (locals beat imports), but the spec asks for the
    // warning so the module remains reachable via an explicit alias.
    if (!sourceImports.empty() &&
        sourceImports.count(std::string(headName))) {
        ctx.diagnostics().report(
            Severity::Warning, loc,
            std::string("warning: chain head `") + std::string(headName) +
                "` resolves to the local type, which shadows the imported "
                "module-name prefix `" + std::string(headName) +
                ".\xE2\x80\xA6` — qualify via an alias to reach the "
                "module (\xC2\xA7""2.7)");
    }
}

// §15.1.1 / §3.6 rule 2 (fix #4678) — Pass 2 for the owning-inline-array
// element check, the `[N owned]T` sibling of checkOwningElementInstantiations
// (TypeResolution_Checks.cpp, #2713).
//
// §15.1.1, v0.307: "The form `[N owned]T` ... denotes an inline array whose
// slots own their (non-null) class-typed contents. T must be a class or
// interface type; value types and runtime-managed reference types are not
// admitted with the `owned` marker. When T is a generic type parameter, the
// class-or-interface restriction is checked at instantiation ... paralleling
// the generic-class-fields rule of §28.5." Per #4367's ruling (v0.300), a
// field-type mention forms the constructed type and is itself an
// instantiation site — `new` is not required.
//
// The declaration-site check (resolveType's InlineArray arm above) admits
// `[N owned]T` inside `class Pool<T>` because T is a placeholder there.
// Without this pass nothing ever re-checked the binding, so `Pool<string>`
// was admitted with no diagnostic at any severity (rule-corpus case
// s15_1_1_0003).
//
// Modelled on checkOwningElementInstantiations's shape (including its
// chained-binding worklist for `class Outer<U>` holding `Inner<U>` whose body
// is the one with the owning inline array) — see that function's own comment
// for the full rationale; the only difference here is the site's shape
// (InlineArray, no container name to key or spell a diagnostic around) and
// the diagnostic wording, which names `[N owned]T` instead of a container.
void Resolver::checkOwningInlineArrayElementInstantiations() {
    // One diagnostic per (instantiation site, parameter) — mirrors
    // checkOwningElementInstantiations's key, minus `container` (there is
    // only one shape here, so the parameter alone disambiguates a body that
    // misuses two different parameters).
    std::set<std::pair<const void*, std::string_view>> reported;
    for (const OwningInlineArrayElementSite& s : owningInlineArrayElementSites_) {
        if (!s.enclosingType) continue;
        std::size_t paramIdx = s.enclosingType->genericParams.size();
        for (std::size_t i = 0; i < s.enclosingType->genericParams.size(); ++i) {
            if (s.enclosingType->genericParams[i] &&
                s.enclosingType->genericParams[i]->name == s.paramName) {
                paramIdx = i;
                break;
            }
        }
        // Not a type parameter of the enclosing TYPE — e.g. a generic
        // METHOD's own parameter, whose bindings are not recorded in
        // instantiations_. Nothing to re-check (the recorder's own
        // nameBoundByCurrentMethod guard should already have excluded a
        // shadowing case, but a NON-shadowing method parameter of the same
        // name is not distinguishable there — same residual as #2736(a)).
        if (paramIdx >= s.enclosingType->genericParams.size()) continue;

        struct Pending {
            TypeSymbol* type;
            std::string_view paramName;
            std::size_t idx;
        };
        std::vector<Pending> work{{s.enclosingType, s.paramName, paramIdx}};
        std::set<std::pair<TypeSymbol*, std::string_view>> seen;
        while (!work.empty()) {
            Pending p = work.back();
            work.pop_back();
            if (!seen.insert({p.type, p.paramName}).second) continue;
            for (const GenericInstantiation& inst : instantiations_) {
                if (inst.type != p.type) continue;
                if (p.idx >= inst.argRefs.size()) continue;
                const TypeRef* arg = inst.argRefs[p.idx];
                const ast::Type* argAst = (p.idx < inst.argAsts.size())
                                              ? inst.argAsts[p.idx] : nullptr;
                if (!arg || arg->kind == TypeRef::Kind::Error) continue;

                if (isBareGenericParamBinding(arg, argAst)) {
                    if (!inst.enclosingType) continue;
                    std::string_view nextName = argAst->name->parts.front();
                    bool boundByMethod = false;
                    if (inst.methodGenericParams) {
                        for (const ast::GenericParam& gp :
                             *inst.methodGenericParams) {
                            if (gp.name == nextName) {
                                boundByMethod = true;
                                break;
                            }
                        }
                    }
                    if (boundByMethod) continue;
                    std::size_t nextIdx = inst.enclosingType->genericParams.size();
                    for (std::size_t i = 0;
                         i < inst.enclosingType->genericParams.size(); ++i) {
                        if (inst.enclosingType->genericParams[i] &&
                            inst.enclosingType->genericParams[i]->name ==
                                nextName) {
                            nextIdx = i;
                            break;
                        }
                    }
                    if (nextIdx >= inst.enclosingType->genericParams.size())
                        continue;
                    work.push_back({inst.enclosingType, nextName, nextIdx});
                    continue;
                }

                std::string badDesc = owningElementBadDesc(arg);
                if (badDesc.empty()) continue;
                if (!reported.insert({inst.siteKey, s.paramName}).second)
                    continue;
                // Names the type that DECLARES the `[N owned]T` field
                // (s.enclosingType) and its parameter, not p.type — through a
                // chain those differ, and it is the declaring site the
                // reader must go fix. The location is the instantiation that
                // made the binding concrete, per §3.6 rule 2.
                ctx_.diagnostics().report(
                    Severity::Error, inst.loc,
                    std::string("`[N owned]T` requires T to be a class or "
                                "interface type (\xC2\xA7""15.1.1) "
                                "\xE2\x80\x94 at this instantiation, `") +
                    std::string(s.enclosingType->name) +
                    "`'s type parameter `" + std::string(s.paramName) +
                    "`, used as an owning inline array's element type, is "
                    "bound to " + badDesc +
                    ", which has no disposal obligation and cannot be owned "
                    "by an inline array's slots (\xC2\xA7""3.6 rule 2 "
                    "\xE2\x80\x94 the property is evaluated at each "
                    "instantiation).");
            }
        }
    }
}

}  // namespace bezel::sema
