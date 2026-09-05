#include "sema/TypeResolution_impl.hpp"

#include "common/BuiltinRegistry.hpp"  // isBuiltinExceptionName — §31.4 (#2950)
#include "common/OwnershipFacts.hpp"   // isMoveOnlyValueType — §14.4 / §15.3 (#3794)
#include "sema/ConstFold.hpp"
#include "sema/ExprTypecheck_impl.hpp"  // isValidRangeElementType / rangeElementTypeName — fix #3247 / §16.3

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
        // (§24.3). Per §2.3, `IEvent<T>`/`IAsyncEvent<T>` are auto-imported (they
        // are the public type of every event member) but `EventSource<T>` /
        // `AsyncEventSource<T>` require `import System.Threading`, like
        // `CancellationTokenSource` / `Mutex` and the rest of that module.
        // Before this fix neither name was registered at all, so a helper
        // method whose parameter type names the in-class face (spec-legal per
        // §24.3) could not be written. Same import-gating stance as
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
    // issue #545: Buffer<T> — fixed-capacity compiler-intrinsic primitive (arity 1).
    // T restricted to value-type primitives (int/byte/short/long/double/decimal/
    // char/bool) in this revision; class-element Buffer is the §5.20 design pass.
    if (n == "Buffer") return 1;
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
        // compiler does not yet enforce this gate, matching the existing pattern
        // for System.IO types (File/Directory/Path) which also resolve without
        // import in the current implementation.
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
void Resolver::bindExtensionTargetTypeParams(const ast::Type& target,
                                              Scope& extScope,
                                              Scope* parent) {
    for (const auto& ga : target.genericArgs) {
        if (!ga) continue;
        if (ga->kind == ast::TypeKind::Named && ga->name &&
            ga->name->parts.size() == 1 && !ga->genericArgs.empty()) {
            // Concrete generic-instantiation head (e.g. `Array` in
            // `Array<byte>` / `Array<T>`): not a free type parameter.
            // Recurse to bind any nested bare identifiers in its own args.
            bindExtensionTargetTypeParams(*ga, extScope, parent);
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
        }
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
            bindExtensionTargetTypeParams(*ext->targetType, extScope, parent);
        }
        if (ext->targetType) resolveType(*ext->targetType, &extScope);
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
                                        std::string(p.name) + "`");
                }
            }
            if (d.returnType) {
                resolveType(*d.returnType, parent);
                // per §32.5: the return slot may additionally be the unit
                // type `()`. (An omitted `: T` clause leaves returnType null
                // and is already a unit-returning method — §32.5.)
                checkExternType(*d.returnType, /*returnPosition=*/true,
                                "return type");
            }
        }
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
    for (const auto& bt : td.baseTypes) {
        if (!bt) continue;
        TypeRef* ref = resolveType(*bt, &typeScope);
        // §10.3 (#740): an enum's base type is its underlying integer type
        // (a primitive, e.g. `: long`), which does NOT resolve to a TypeSymbol
        // declaration and so is not a `baseTypes` entry. Record it on the enum's
        // TypeSymbol so codegen lowers the enum to the right integer width.
        if (tsym && td.kind == ast::TypeDeclKind::Enum && ref &&
            ref->kind == TypeRef::Kind::Primitive) {
            tsym->enumUnderlyingType = ref;
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
    if (auto* fd = dynamic_cast<const ast::FieldDecl*>(&m)) {
        if (fd->type) resolveType(*fd->type, parent);
        // per §28.14 (v0.175): a `scoped` FIELD must be function-typed.
        for (std::string_view mod : fd->modifiers) {
            if (mod == "scoped") {
                checkScopedIsFunctionTyped(fd->type.get(), parent, fd->name,
                                           "field", fd->span.begin);
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
        for (const auto& g : md->genericParams) {
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
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        "an inline array may not be the element of another "
                        "inline array (`[N][M]T`) — multi-dimensional inline "
                        "arrays are not admitted (§15.1)");
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
        badDesc = "a function type";
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

// fix #3619 (§17.14 / §3619, v0.286): the ONE Resolver-side identifier
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

void Resolver::resolveNamed(const ast::Type& t, Scope* scope, TypeRef& ref) {
    if (!t.name || t.name->parts.empty()) {
        ref.kind = TypeRef::Kind::Error;
        return;
    }
    std::string_view first = t.name->parts.front();
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
    // Look up the first part.
    Symbol* sym = lookupName(first, scope);
    if (!sym) {
        // Unknown — but a null lookup may also mean "name is in scope as
        // a generic-param marker" (we used nullptr to indicate that).
        // Distinguish by checking if the key exists.
        bool isGeneric = false;
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
                            "(\xc2\xa7""11.6 / \xc2\xa7""14.1, v0.286; "
                            "see issue #3584). Move `Tag` to module "
                            "scope, declare `Tag` as its own generic "
                            "(`class Tag<U>`) and pass `T` through, or "
                            "make the enclosing class carry the field "
                            "instead of the nested type");
                    ref.kind = TypeRef::Kind::Error;
                    return;
                }
            }
            return;
        }
        // Slice S24 (§2.3): always-available built-in type names
        // resolve without import. They have no user TypeSymbol, so
        // we record a Named ref with a null declaration and verify
        // generic arity against the spec's declared form.
        if (t.name->parts.size() == 1) {
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
                std::size_t ownedSlot = static_cast<std::size_t>(-1);
                if (first == "OwningArray" || first == "OwningSet" ||
                    first == "OwningStack" || first == "OwningQueue" ||
                    first == "OwningLinkedList")
                    ownedSlot = 0;
                else if (first == "OwningDictionary")
                    ownedSlot = 1;
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
                        bool boundByMethod = false;
                        if (currentMethodGenericParams_) {
                            for (const ast::GenericParam& gp :
                                 *currentMethodGenericParams_) {
                                if (gp.name == el->headName()) {
                                    boundByMethod = true;
                                    break;
                                }
                            }
                        }
                        if (!boundByMethod)
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
                if (!hashRole.empty() && !ref.genericArgs.empty())
                    hashContainerSites_.push_back(
                        {first, hashRole, ref.genericArgs[0], &t,
                         currentOuterType_});  // §1384: for per-instantiation check
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
                    first == "ReadOnlySpan") {
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
                            // `instantiations_` knows nothing about.
                            bool boundByMethod = false;
                            if (currentMethodGenericParams_) {
                                for (const ast::GenericParam& gp :
                                     *currentMethodGenericParams_) {
                                    if (gp.name ==
                                        el->astNode->name->parts.front()) {
                                        boundByMethod = true;
                                        break;
                                    }
                                }
                            }
                            if (!boundByMethod) {
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
        // export a type with this name. Using it unqualified is a compile error;
        // the fix is a module-qualified name or an import alias (§2.2).
        if (!ambiguousImportNames_.empty() &&
            ambiguousImportNames_.count(std::string(first))) {
            ctx_.diagnostics().report(
                Severity::Error, t.span.begin,
                std::string("bare name `") + std::string(first) +
                "` is ambiguous: multiple imported modules export a type "
                "with this name; qualify it via a module-qualified name or "
                "an import alias (\xC2\xA7""2.2)");
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
                if (TypeRef* underlying =
                        ctx_.resolvedType(*aliasDecl->aliasedType)) {
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
        // Generic-arity check.
        if (!t.genericArgs.empty() &&
            t.genericArgs.size() != tsym->genericParams.size()) {
            ctx_.diagnostics().report(
                Severity::Error, t.span.begin,
                "generic-argument count does not match type's "
                "generic-parameter count");
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
                if (pIdx >= ref.genericArgs.size()) continue;
                const TypeRef* arg = ref.genericArgs[pIdx];
                const char* badKind = nullptr;
                if (arg && arg->kind == TypeRef::Kind::Primitive &&
                    arg->primitive == PrimitiveType::String)
                    badKind = "`string`";
                else if (arg && arg->kind == TypeRef::Kind::Function)
                    badKind = "a function value";
                if (badKind) {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
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
        inst.site = &t;
        // #2713: record which names the ENCLOSING METHOD binds here, so a
        // chained owning-element obligation re-keying through this site can
        // tell a name bound by the method from one bound by enclosingType.
        inst.methodGenericParams = currentMethodGenericParams_;
        for (std::size_t i = 0; i < ref.genericArgs.size(); ++i) {
            inst.argRefs.push_back(ref.genericArgs[i]);
            inst.argAsts.push_back(t.genericArgs[i].get());
        }
        instantiations_.push_back(std::move(inst));
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
        return;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        if (lc->type) resolveType(*lc->type, scope);
        if (lc->value) resolveTypesInExpr(*lc->value, scope);
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
        if (fs->iterable) resolveTypesInExpr(*fs->iterable, scope);
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
    if (currentMethodGenericParams_)
        for (const ast::GenericParam& gp : *currentMethodGenericParams_)
            if (gp.name == name) return;
    asTargetSites_.push_back({name, currentOuterType_, loc});
}

// §14.6 (v0.262, #3271): record `call` as a generic member's SELF-edge when
// its callee spells the enclosing generic method's own name with explicit type
// arguments. The three admitted spellings are the three ways a member can name
// itself without going through another declaration:
//
//     M<...>(…)        — unqualified, the ordinary self-recursive call
//     this.M<...>(…)   — instance-qualified
//     C.M<...>(…)      — enclosing-type-qualified (the only spelling a static
//                        member of `static class C` can use besides the bare one)
//
// Anything else — a call through a variable, an interface, a base projection,
// or another declaration — is NOT recorded. §14.6's rule is per-edge over a
// strongly-connected component, and a direct self-loop is the one edge class
// whose SCC membership is decidable without a resolved call graph: a self-loop
// is unconditionally inside its own component. Every other member edge needs
// the call graph and is the recorded residual (see
// checkExpandingGenericMemberEdges in TypeResolution_Checks.cpp).
//
// Recording is deliberately generous and the VERDICT is where the guards live
// (the overload guard in particular) — recording is side-effect-free, so an
// over-recorded edge costs a vector entry and nothing else.
void Resolver::recordGenericMemberSelfEdge(const ast::CallExpr& call) {
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
        // The receiver must be `this` or the enclosing type's own name; any
        // other receiver names a different declaration's member.
        auto* recv = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
        if (!recv || !recv->typeArgs.empty()) return;
        if (recv->name != "this" && recv->name != currentOuterType_->name)
            return;
        typeArgs = &ma->typeArgs;
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

void Resolver::resolveTypesInExpr(const ast::Expr& e, Scope* scope) {
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
        recordGenericMemberSelfEdge(*ce);
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        if (nc->callee) resolveTypesInExpr(*nc->callee, scope);
        for (const auto& a : nc->args) {
            if (a.value) resolveTypesInExpr(*a.value, scope);
        }
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
        if (lam->exprBody) resolveTypesInExpr(*lam->exprBody, scope);
        for (const auto& s : lam->stmts) {
            if (s) resolveTypesInStmt(*s, scope);
        }
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
        return;
    }
    // Literals + ThisBaseTypeOf — no nested Types/Exprs.
}

}  // namespace bezel::sema
