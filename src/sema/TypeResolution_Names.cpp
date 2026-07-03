#include "sema/TypeResolution_impl.hpp"

#include "sema/ConstFold.hpp"

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
        n == "ArgumentNullException" || n == "ArithmeticException" ||
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
        // Dictionary<K,V> indexer getter for an absent key. Per the spec it
        // lives in System.Collections.Generic and requires `import`; the
        // compiler does not yet enforce import-gating for that namespace (its
        // sibling collection types Array/Dictionary/… all resolve without
        // import here too), so it joins the resolvable built-in exception set
        // for consistency, enabling `catch KeyNotFoundException`.
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
        // System.Threading / System.IO and require `import`, but — like
        // KeyNotFoundException (System.Collections.Generic) above — the compiler
        // does not yet enforce import-gating for those namespaces (File/Directory/
        // Path resolve without import too), so they join the resolvable built-in
        // exception set, enabling `catch var ex: IOException` and `throw new
        // IOException(...)`. Hierarchy is in builtinExceptionHierarchy() (codegen).
        n == "TimeoutException" || n == "IOException" ||
        n == "FileNotFoundException" || n == "DirectoryNotFoundException" ||
        n == "EndOfStreamException" || n == "PathTooLongException" ||
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
        // §2.8 / §2.9: auto-imported generic interfaces (stdlib §2.8–§2.9).
        // Registered here so they resolve as base types and field/param types
        // (fix #576: "unresolved type name 'IComparable'").
        // §5 / #975: IEqualityComparer<T> — the user-implemented equality agent
        // (Equals(a,b):bool, GetHashCode(value):int); arity 1.
        n == "IComparable" || n == "IEquatable" || n == "IEqualityComparer" ||
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
        n == "File" || n == "Directory" || n == "Path" ||
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
        if (ext->targetType) {
            for (const auto& ga : ext->targetType->genericArgs) {
                if (!ga) continue;
                if (ga->kind == ast::TypeKind::Named &&
                    ga->name && ga->name->parts.size() == 1 &&
                    !ga->nullable) {
                    // Single-part bare name: treat as a type parameter.
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
                // §11.7: `sealed abstract` is a *closed abstract class* — it
                // may be inherited from within its declaring module, but not
                // across module boundaries. This is the spec's recommended
                // closed-hierarchy idiom for exhaustive `case` matching
                // (§19.6), so a same-module subclass must be accepted; only a
                // cross-module subclass is rejected. The current module owns
                // every locally-declared type, so a base flagged
                // `isImported` (reconstructed from a dependency's metadata,
                // §2.5) is from another module; a non-imported base is
                // same-module.
                //
                // Plain `sealed` (non-abstract) keeps the §12.4 rule: it can
                // never be inherited from, same module or not.
                if (sealed) {
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
            // per §17.11 (#1166): resolve the inner type nodes of a parameter's
            // default-value expression (e.g. the `T` in `:= default(T)`) so the
            // §17.11 defaultIllFormed well-formedness check in ExprTypecheck can
            // run on it — without resolution `resolvedType(de->type)` is null and
            // an ill-formed `default(R)` param default slips through silently.
            if (p.defaultValue) resolveTypesInExpr(*p.defaultValue, &methodScope);
        }
        if (md->returnType) resolveType(*md->returnType, &methodScope);
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
    } else if (auto* od =
                   dynamic_cast<const ast::OperatorDecl*>(&m)) {
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
        if (od->bodyKind == ast::MethodBodyKind::Block) {
            resolveTypesInBody(od->stmts, &methodScope);
        } else if (od->bodyKind == ast::MethodBodyKind::Expression &&
                   od->exprBody) {
            resolveTypesInExpr(*od->exprBody, &methodScope);
        }
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
        resolveTypesInBody(cd->body, &ctorScope);
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
                ref->funcParams.push_back(std::move(fp));
            }
            if (t.funcReturn) {
                ref->funcReturn = resolveType(*t.funcReturn, scope);
            }
            break;
        case ast::TypeKind::Named:
            resolveNamed(t, scope, *ref);
            break;
    }
    ctx_.setResolvedType(&t, ref);
    return ref;
}

// §28.14 / §22.3 (v0.175): `scoped` is valid only on a FUNCTION-typed
// parameter / field. The check runs on the RESOLVED type so a type alias to a
// function type is admitted transparently. A nullable function type
// (`(() -> ())?`) is admitted — the natural shape of a clearable `scoped`
// field. An unresolved (Error) type stays silent (the resolution error was
// already reported; no cascade).
void Resolver::checkScopedIsFunctionTyped(const ast::Type* declaredType,
                                          Scope* scope, std::string_view name,
                                          std::string_view what,
                                          SourceLocation loc) {
    if (!declaredType) return;
    TypeRef* rt = ctx_.resolvedType(*declaredType);
    if (!rt) rt = resolveType(*declaredType, scope);
    if (!rt || rt->kind == TypeRef::Kind::Error) return;
    if (rt->kind == TypeRef::Kind::Function) return;
    ctx_.diagnostics().report(
        Severity::Error, loc,
        std::string("`scoped` is valid only on a function-typed ") +
        std::string(what) + " (§22.3, §28.14 v0.175); `" + std::string(name) +
        "` does not have a function type");
}

std::optional<long long> Resolver::foldIntConstExpr(const ast::Expr& e,
                                                    Scope* scope, int depth) {
    // Identifier resolution for the shared folder: a bare name is a `const`
    // whose initializer is itself an int-const expression. Member consts live
    // on the enclosing type (tracked by currentOuterType_) and its class base
    // chain; module/enclosing-scope consts are reached through the lexical
    // `scope`. (Local consts are not yet registered at type-resolution time —
    // see resolveTypesInBody — so a local-const size is not foldable here; the
    // ExprTypecheck pass folds those, where locals are in scope.)
    std::function<std::optional<long long>(std::string_view, int)> resolve =
        [&](std::string_view name, int d) -> std::optional<long long> {
        for (TypeSymbol* ts = currentOuterType_; ts;) {
            for (Symbol* m : ts->members)
                if (auto* cms = dynamic_cast<ConstMemberSymbol*>(m))
                    if (cms->name == name)
                        if (auto* cd =
                                dynamic_cast<const ast::ConstDecl*>(cms->decl))
                            if (cd->value)
                                return foldIntConst(*cd->value, resolve, d + 1);
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
    };
    return foldIntConst(e, resolve, depth);
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
                    std::string badDesc;
                    if (el) {
                        switch (el->kind) {
                        case TypeRef::Kind::Primitive:
                            // All primitives are rejected: value types and
                            // `string` (runtime-managed, no dispose).
                            badDesc = el->primitive == PrimitiveType::String
                                ? "`string` (a runtime-managed reference type)"
                                : "a primitive value type";
                            break;
                        case TypeRef::Kind::Function:
                            badDesc = "a function value";
                            break;
                        case TypeRef::Kind::Unit:
                            badDesc = "the unit type `()`";
                            break;
                        case TypeRef::Kind::InlineArray:
                            badDesc = "an inline array value type";
                            break;
                        case TypeRef::Kind::Named: {
                            const TypeSymbol* ts = el->declaration;
                            if (ts) {
                                if (ts->isValueBuiltin)
                                    badDesc = "a value-type builtin";
                                else if (ts->kind ==
                                         ast::TypeDeclKind::Record)
                                    badDesc = "a record (value type)";
                                else if (ts->kind ==
                                         ast::TypeDeclKind::Enum)
                                    badDesc = "an enum (value type)";
                                // Class or Interface → admitted; badDesc empty.
                            } else {
                                // No TypeSymbol: builtin type or generic param.
                                // Reject the handful of known value-like builtin
                                // names; admit all class/interface builtins and
                                // generic type parameters.
                                if (el->astNode && el->astNode->name &&
                                    el->astNode->name->parts.size() == 1) {
                                    std::string_view nm =
                                        el->astNode->name->parts[0];
                                    if (nm == "Range" || nm == "Buffer" ||
                                        nm == "IntPtr" || nm == "UIntPtr" ||
                                        nm == "ReadOnlyStringSpan" ||
                                        nm == "Boolean" || nm == "Span" ||
                                        nm == "ReadOnlySpan" ||
                                        nm == "ArraySpan" ||
                                        nm == "ReadOnlyArraySpan")
                                        badDesc = "a value-type builtin";
                                }
                            }
                            break;
                        }
                        default:  // Kind::Error — suppress cascade diagnostics
                            break;
                        }
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
                // equality-bearing. Deferred (not checked inline) because a
                // record key's auto-equality-bearing property depends on the
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
                if (first == "IComparable" || first == "IEquatable" ||
                    first == "IEqualityComparer" || first == "IReadOnlySpan") {
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
        if (tsym->kind == ast::TypeDeclKind::Class) {
            if (auto* gtd = dynamic_cast<const ast::TypeDecl*>(tsym->decl)) {
                for (const auto& mem : gtd->members) {
                    auto* fd = dynamic_cast<const ast::FieldDecl*>(mem.get());
                    if (!fd || !fd->type || !fd->type->name) continue;
                    bool isOwned = false;
                    for (const auto& md : fd->modifiers)
                        if (md == "owned") { isOwned = true; break; }
                    if (!isOwned) continue;
                    // The field's type must be a bare type parameter of tsym
                    // (`T` or `T?`); a concrete or composite type carries its own
                    // §28.5 field check at the declaration.
                    if (fd->type->name->parts.size() != 1) continue;
                    std::string_view pn = fd->type->name->parts.back();
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
                            std::string(fd->name) +
                            "`, and only class or interface types may be "
                            "`owned` \xE2\x80\x94 `string` and function values "
                            "are runtime-managed and carry no disposal "
                            "obligation (\xC2\xA7""28.5 / \xC2\xA7""28.9); use a "
                            "class-typed argument.");
                    }
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
            if (p.type) resolveType(*p.type, scope);
        }
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
