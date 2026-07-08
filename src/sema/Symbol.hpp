#pragma once

// Sema symbols (§9-§14). Symbols represent named entities the compiler can
// look up: modules, types, members, parameters, locals. They're produced by
// the Symbol Collection pass (sema slice 1) and pointed at by later passes —
// Type Resolution attaches resolved TypeRefs to type-symbol uses; Typecheck
// attaches resolved-symbol pointers to identifier expressions; Ownership
// uses each symbol's ScopeId for borrow-lifetime checks.
//
// One polymorphic base, one subclass per kind. The hierarchy is open by
// design (a future ExtensionMethodSymbol would slot in without changing
// the base). Symbols are owned by SemaContext (slice 1) and live for the
// duration of compilation.

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "ast/Node.hpp"
#include "support/SourceLocation.hpp"

namespace bezel::sema {

struct TypeRef;  // see TypeRef.hpp; forward-declared so LocalSymbol can
                 // point at one without dragging the full header in.
class SemaContext;  // see SemaContext.hpp; forward-declared so the §9.2
                    // auto-equality helpers can resolve component types
                    // without a circular include (SemaContext.hpp includes
                    // this header).

// Per-symbol scope identifier. Assigned by the symbol-collection pass in
// scope-DFS order so a larger ScopeId is always nested inside a smaller
// one's lifetime. The 0 value is reserved for "no scope" (used for
// module-level entities whose lifetime is the whole compilation).
using ScopeId = std::uint32_t;
inline constexpr ScopeId kModuleScope = 1;

// Polymorphic Symbol base. dynamic_cast distinguishes kinds; no kind tag
// is duplicated on the base.
struct Symbol {
    std::string_view name;
    ScopeId scope = kModuleScope;
    const ast::Node* decl = nullptr;
    SourceLocation declLoc;

    Symbol() = default;
    virtual ~Symbol() = default;

    Symbol(const Symbol&) = delete;
    Symbol& operator=(const Symbol&) = delete;
};

// A.2 ModuleDecl — top-level container. One per source file in slice 1;
// multi-file modules (the same `module X` declared in multiple files) are
// admitted at the spec level but ground out as merged ModuleSymbols when
// the multi-file driver lands.
struct ModuleSymbol : Symbol {
    std::vector<Symbol*> children; // TypeSymbol, TypeAliasSymbol, etc.
};

// A.3 ClassDecl / InterfaceDecl / RecordDecl / EnumDecl share TypeSymbol.
// The §A.3 TypeDeclKind tag distinguishes them (it's a parser-side enum
// already; we just carry it through).
struct TypeSymbol : Symbol {
    ast::TypeDeclKind kind = ast::TypeDeclKind::Class;
    ast::Visibility visibility = ast::Visibility::None;
    std::vector<std::string_view> modifiers; // "abstract", "sealed", "static"
    std::vector<Symbol*> genericParams;      // GenericParamSymbol*
    std::vector<Symbol*> members;            // Field/Method/Property/...
    // Scope inside which members and generic params live. Always greater
    // than `scope` (which is the type's own enclosing scope).
    ScopeId memberScope = kModuleScope;
    // Slice S5: base types resolved by TypeResolution. The first entry
    // (if the AST has any base types and the first resolves to a
    // class) is conventionally the base class; remaining entries are
    // interfaces. Sema slice 5 doesn't yet distinguish — member lookup
    // walks all of them.
    std::vector<TypeSymbol*> baseTypes;
    // Resolved TypeRef for each base type (parallel to baseTypes),
    // including generic args (e.g. Container<int> → TypeRef with
    // genericArgs[0]=int). Used by override-signature checking to
    // apply the concrete substitution before comparing types.
    std::vector<TypeRef*> baseTypeRefs;
    // §2.5 / §2.6 / §6.1 / §28 (issue #434): true for the synthetic builtin
    // TypeSymbol objects that represent VALUE types — DateTime, TimeSpan, Guid,
    // CancellationToken. These are scalar/struct types with no heap allocation and
    // no disposal obligation. They carry kind==Class so that the sema and codegen
    // paths that inspect class-typed member accesses continue to work unchanged;
    // only the ownership checker consults this flag to suppress the §28.3 owned-
    // local obligation that would otherwise be inferred from kind==Class. Owned
    // builtins (CancellationTokenSource, Array, Dictionary, …) leave this false.
    bool isValueBuiltin = false;
    // §9.1 / §9.2 (issue #501): true when this TypeSymbol represents a
    // positional record (a record declared with a `(field...)` positional
    // parameter list). Carried as a flag on the TypeSymbol so that imported
    // positional records — which have no ast::TypeDecl (ts->decl is null) —
    // still pass the §9.2 auto-equality-bearing check in
    // ExprTypecheck_Exprs.cpp. For LOCAL records the check reads this flag
    // or falls back to the ast::TypeDecl; for IMPORTED records only this
    // flag is available.
    bool hasPositionalFields = false;
    // §10.3 (#740): for an ENUM, the resolved underlying-type TypeRef from the
    // `: byte|short|int|long|...` annotation. Null means the default `int`.
    // A primitive base type does not become a `baseTypes` entry (those hold
    // TypeSymbol* declarations only), so the underlying width/signedness is
    // recorded here for codegen to lower the enum to the correct integer type
    // instead of the hardcoded i32.
    TypeRef* enumUnderlyingType = nullptr;
};

// A.2 TypeAliasDecl — its target type is resolved by Type Resolution.
struct TypeAliasSymbol : Symbol {
    std::vector<Symbol*> genericParams;
};

// A.3 GenericParam (`Variance? Identifier`). Lives in the parent type or
// method's scope and is looked up like a local name.
struct GenericParamSymbol : Symbol {
    std::string_view variance; // "in", "out", or empty
};

// A.4 Member subkinds. Each carries the visibility + modifier prefix and a
// pointer back to the parent type's TypeSymbol for upward lookup.
struct MemberSymbol : Symbol {
    ast::Visibility visibility = ast::Visibility::None;
    std::vector<std::string_view> modifiers;
    TypeSymbol* parent = nullptr;
};

struct FieldSymbol : MemberSymbol {
    // §9.1: a record's positional field. When non-null, this field was
    // introduced by a record's positional parameter list rather than a body
    // `FieldDecl`, and its type comes from `posParam->type` (the `decl` of
    // such a symbol is the owning TypeDecl, not a FieldDecl). Null for
    // ordinary body fields.
    const ast::Param* posParam = nullptr;
    // §22.10 (v0.180): for a CLASS primary-constructor parameter (posParam !=
    // null, parent kind == Class) the typechecker records whether the parameter
    // is referenced in a *body* position (method / property-accessor /
    // constructor / destructor body) versus an *initializer* position (field or
    // property initializer, or the inheritance clause's base call — tracked via
    // ExprTC::inFieldInitializer_). An `owned` such parameter referenced in a
    // body position and NOT in any initializer position is captured into an
    // `owned` private field disposed by the generated `Dispose()` (§28.4). The
    // "not in any initializer" side is intentionally conservative: a parameter
    // that also appears in an initializer might be consumed there (the §11.1
    // `ResourceHolder` pattern moves it into an explicit owned field), and
    // disposing the capture slot on top of that owner would double-free — so the
    // capture slot is disposed only when the parameter is used solely in bodies.
    // Both default false and are meaningful only for class posParam fields.
    bool posParamBodyRef = false;
    bool posParamInitRef = false;
};

struct MethodSymbol : MemberSymbol {
    std::vector<Symbol*> genericParams;
    std::vector<Symbol*> params;     // ParameterSymbol*
    // Operator overloads land here too (with an empty `name` carrying the
    // operator-token instead — sema slice 2+ teaches lookup to treat
    // op-symbols specially).
    bool isOperator = false;
    std::string_view operatorToken;  // empty unless isOperator
    // §26 (#433): for an EXTENSION method, the declaring `extension` block's
    // decl (its `targetType` is the extended type). Null for an ordinary
    // class/record/enum/interface method. An extension method has no `parent`
    // TypeSymbol; this is how codegen identifies it and recovers the receiver
    // (`this`) type — by-value for a value/primitive extended type, by-pointer
    // for a class.
    const ast::ExtensionDecl* extensionDecl = nullptr;
};

struct ConstructorSymbol : MemberSymbol {
    std::vector<Symbol*> params;
    bool isStatic = false;
};

struct DestructorSymbol : MemberSymbol {};

struct PropertySymbol : MemberSymbol {
    bool isIndexer = false;
    std::vector<Symbol*> indexerParams; // empty when !isIndexer
};

struct EventSymbol : MemberSymbol {};

struct ConstMemberSymbol : MemberSymbol {
    bool isStatic = false;
};

struct EnumConstantSymbol : MemberSymbol {
    // §10/§10.2: the constant's underlying integer value. For an IMPORTED enum
    // the symbol is reconstructed from dependency metadata (which carries the
    // value, issue #328 FINDING [1]) and has no AST TypeDecl, so codegen reads
    // this field instead of re-deriving from the (absent) AST. For a LOCAL enum
    // codegen still derives values from the AST in computeEnumValues(); this
    // field is the cross-module carrier. Defaults to 0 when not set.
    std::int64_t value = 0;
};

// Nested type — a TypeSymbol that lives inside another TypeSymbol's
// members. Same shape; the parent pointer is captured by the enclosing
// TypeSymbol::members entry. We keep a dedicated wrapper class so
// dynamic_cast distinguishes the nested-as-Member case from the
// top-level case during member-list iteration.
struct NestedTypeSymbol : MemberSymbol {
    TypeSymbol* type = nullptr; // owns the inner TypeSymbol pointer
};

// A.6 LocalDecl / LocalConstDecl / iteration variables. Slice 1 doesn't
// collect locals (those live inside method bodies which sema visits in
// typecheck slice 3+); the struct is here so the kind set is complete.
struct LocalSymbol : Symbol {
    bool isUsing = false;
    bool isAwaitUsing = false;
    bool isConst = false;
    // §28.3 / #1387: set by the ownership pass on an `out owned var` INLINE local
    // (ls->decl is IdentifierExpr, not LocalDecl) that is never discharged on ANY
    // path — a never-moved genuine owner. Codegen (ensureOutVarSlots) registers a
    // dynamicDispose for the slot (the same null-guarded scope-end disposal a
    // `using var` gets), firing at the enclosing scope's end. Mirrors the
    // LocalDecl::autoDropAtScopeEnd flag but lives on the LocalSymbol because the
    // inline local has no LocalDecl node to annotate.
    bool autoDropAtScopeEnd = false;
    // §22.4 / §7.9 (v0.181, #1552): set on an inline `out var x` local whose
    // DECLARED out-parameter type is a NON-nullable reference type (has no
    // default value). Such a variable is definitely assigned only on paths
    // where the introducing call actually ran; a read on a path where a
    // short-circuit operator skipped the call is a definite-assignment error.
    // Recorded here because `declType` below is stripped of its declared
    // nullability at registration (a reference `T?` out var is represented as
    // the non-nullable `T`), so the definite-assignment pass could not recover
    // the declared nullability from `declType` alone. Left false for value-typed
    // and nullable out vars (default-initialized, §7.9 — readable on every path)
    // and for every non-`out var` local.
    bool outVarNeedsDefiniteAssignment = false;
    // Slice 3g: resolved type for the local. Set either from an
    // explicit `: Type` ascription (via TypeResolution) or inferred
    // from the initializer's exprType. Null when neither is available
    // (e.g. plain `var x;` with no later assignment yet — caught by
    // DefiniteAssignment when that pass lands).
    TypeRef* declType = nullptr;
};

// A.4 Param — slice 1 collects method/ctor/property-setter params since
// they're part of the type's surface.
struct ParameterSymbol : Symbol {
    std::string_view modifier; // "", "ref", "out", "out owned", "owned"
    // Pointer back to the ast::Type for this parameter (the ast::Param
    // itself isn't a Node, so we hold its `type` pointer directly).
    // Null if the parameter has no declared type (lambda params with
    // inferred types — slice S3e/lambda only).
    const ast::Type* declType = nullptr;
    // per §24.3: an already-resolved type for a parameter that has no syntactic
    // `: Type` annotation but whose type is supplied by context — currently
    // only an event-handler lambda param, typed as the event payload `T`.
    // Preferred over `declType` (which is null in that case) by identifier
    // resolution. Null otherwise.
    TypeRef* resolvedDeclType = nullptr;
    // §29.8 (#945): set by the ownership pass on an `owned` parameter of a SYNC
    // iterator method whose body never moves the parameter out. The iterator's
    // state machine takes ownership at construction and disposes the parameter
    // at iterator-Dispose (after the body's `using var` / `finally` cleanups,
    // reverse declaration order). Codegen (emitIteratorMethod's Dispose) reads
    // this flag to emit that disposal. Owned params the body discharges are
    // never flagged, so there is no double-free.
    bool iteratorAutoDispose = false;
    // §28.3 v0.150/v0.152 (#1008 Part B, affine auto-drop extended to params):
    // set by the ownership pass on an `owned` parameter of a SYNCHRONOUS, non-
    // iterator, non-destructor method that is never discharged on any path
    // (everDischarged == 0) and never reassigned/out-overwritten — a never-moved
    // genuine owner. Codegen (registerUnwindOwnedParam) disposes it at method
    // end on BOTH the normal and unwind edges (dynamicDispose), the same as a
    // dropped owned local. A param discharged on some path is not flagged.
    bool autoDropAtScopeEnd = false;
    // §27.4 (v0.183, #1408): non-null when this lambda parameter was written
    // as a record-destructuring pattern (`(Entry(k, v)) => ...`). The
    // parameter itself binds no name (`name` stays empty); `resolvedDeclType`
    // carries the exact record type. Codegen's lambda-param prologue reads
    // this to bind each destructured slot's LocalSymbol from the incoming
    // record argument's fields, instead of storing the whole argument into a
    // single named parameter slot.
    const ast::DestructuringPattern* destructure = nullptr;
};

// A.3 ExtensionDecl — collected at module level. Its target type is
// resolved by Type Resolution.
struct ExtensionSymbol : Symbol {
    std::vector<Symbol*> members; // ExtensionMember kinds (slice 1: any)
    std::vector<Symbol*> genericParams; // empty per A.3 (extensions don't
                                        // declare generic params on the
                                        // extension itself, only generic
                                        // constraints over the target type)
};

// A.3 ExternBlock — FFI declarations.
struct ExternBlockSymbol : Symbol {
    std::string_view library;     // the StringLiteral after `extern`
    std::string_view convention;  // the optional `convention "..."`
    std::vector<Symbol*> decls;   // ExternDeclSymbol*
};

struct ExternDeclSymbol : Symbol {
    ast::Visibility visibility = ast::Visibility::None;
    std::vector<Symbol*> params;
    // The originating extern method declaration (for its return type).
    const ast::ExternDecl* externDecl = nullptr;
};

// §9.2 / §22.9: a member is an *auto-property* (`Name: T;`, optionally with an
// initializer, no accessor bodies / arrow getter) — backed by compiler-
// generated storage, exactly like a field. Indexers and static properties are
// excluded. Shared by sema and codegen so both agree on what counts as
// auto-property storage.
bool isAutoPropertySymbol(const Symbol* m);

// §9.3 (v0.158, #1383): true when a positional or combined-form record
// synthesizes equality — i.e. every positional field's type (and every body
// auto-property's type for the combined form) is an admissible component type
// (§9.2 recursive rule) and the body carries no disqualifying member. Imported
// positional records (no ast::TypeDecl) are conservatively treated as true.
// Called from isAdmissibleComponentType (nested record check), isEqualityBearing
// (hash-container key check), and ExprTypecheck (use-site equality error).
bool isPositionalRecordEqualityBearing(const TypeSymbol* rec,
                                        const SemaContext& ctx);

// §9.2 (v0.158) auto-equality eligibility for a *body record* (a record with
// no positional fields). Returns true iff the record is "auto-equality-bearing":
// every member is an auto-property whose type is an **admissible component
// type**, a method, an expression-bodied or block-form (computed) property, a
// constructor, or a non-equality operator overload — and the record declares no
// explicit field storage, no `override Equals`/`GetHashCode` method, and no
// `=`/`<>` operator overload. An eligible body record receives the §9.2/§8.5
// auto-equality package (`Equals`, `GetHashCode`, `ToString`) over its
// auto-properties.
//
// Admissible component types (one recursive rule, §9.2 v0.158): a primitive,
// enum, or `string`; another auto-equality-bearing record; `T?` where T is
// admissible; a non-owning inline array `[N]T`/`[N]T?` with admissible element.
// A class/interface/`Object`/function/owning-inline-array component makes the
// record ineligible.
//
// v0.158 narrowing (was #1249-A): an `override` of `ToString` alone no longer
// disqualifies — it suppresses only auto-`ToString`; auto-`Equals`/`GetHashCode`
// remain. An `override` of `Equals` or `GetHashCode` is still a full
// disqualifier.
//
// The SemaContext resolves each component's declared type; when a component's
// type cannot be resolved (an imported record without an accessible member type,
// or an unresolved generic parameter) the component is treated conservatively as
// admissible — matching the pre-v0.158 lenient behaviour and never over-rejecting.
//
// Returns false for positional / combined-form records (they get the §9.1
// positional synthesis instead) and for non-record types.
bool isAutoEqualityBearingBodyRecord(const TypeSymbol* rec,
                                     const SemaContext& ctx);

// §9.2/§8.4: does this type declare its own equality member, i.e. an
// `operator =` / `operator <>` overload, or an `Equals` / `GetHashCode`
// method? Per §9.2 "Authors who want explicit equality on a record outside
// the auto-equality-bearing set declare `=` and `<>` overloads … along with
// `Equals(Object?)`, `Equals(T)`, and `GetHashCode` overrides." Such a record
// is *not* auto-equality-bearing, yet `=`/`<>`/`.Equals`/`.GetHashCode` are
// valid on it (they dispatch to the user's member), so it must be excluded
// from the §9.2 non-auto-equality compile error. Returns false for null.
bool recordHasUserDeclaredEquality(const TypeSymbol* rec);

// §3.6 / §8.3 (v0.158, #1338): the **equality-bearing** derived type property —
// the classification a hash-container key/element must satisfy (`Dictionary<K,V>`
// / `OwningDictionary<K,V>` keys, `Set<T>` / `OwningSet<T>` elements). Per §3.6's
// table: "every type except a record that is neither auto-equality-bearing nor
// declares user equality (`operator =`/`<>` with `Equals(T)`/`GetHashCode` per
// the §25.2 co-declaration rule)". So every primitive, string, enum, class,
// interface, function value, and inline array is equality-bearing; a record is
// equality-bearing iff it is positional/combined (§9.1 always synthesizes
// equality), is an auto-equality-bearing body record (§9.2), or declares
// `operator =`/`<>`. A null or unresolved type is treated as equality-bearing
// (conservative — never over-rejects at an instantiation site).
bool isEqualityBearing(const TypeRef* ty, const SemaContext& ctx);

} // namespace bezel::sema
