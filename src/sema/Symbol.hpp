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
};

struct MethodSymbol : MemberSymbol {
    std::vector<Symbol*> genericParams;
    std::vector<Symbol*> params;     // ParameterSymbol*
    // Operator overloads land here too (with an empty `name` carrying the
    // operator-token instead — sema slice 2+ teaches lookup to treat
    // op-symbols specially).
    bool isOperator = false;
    std::string_view operatorToken;  // empty unless isOperator
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

// §9.2 auto-equality eligibility for a *body record* (a record with no
// positional fields). Returns true iff the record is "auto-equality-bearing":
// every member is an auto-property, a method, an expression-bodied or
// block-form (computed) property, a constructor, or a non-equality operator
// overload — and the record declares no explicit field storage, no `Equals`/
// `GetHashCode`/`ToString` method, and no `=`/`<>` operator overload. An
// eligible body record receives the §9.2/§8.5 auto-equality package
// (`Equals`, `GetHashCode`, `ToString`) over its auto-properties.
//
// Returns false for positional / combined-form records (they get the §9.1
// positional synthesis instead) and for non-record types.
bool isAutoEqualityBearingBodyRecord(const TypeSymbol* rec);

// §9.2/§8.4: does this type declare its own equality member, i.e. an
// `operator =` / `operator <>` overload, or an `Equals` / `GetHashCode`
// method? Per §9.2 "Authors who want explicit equality on a record outside
// the auto-equality-bearing set declare `=` and `<>` overloads … along with
// `Equals(Object?)`, `Equals(T)`, and `GetHashCode` overrides." Such a record
// is *not* auto-equality-bearing, yet `=`/`<>`/`.Equals`/`.GetHashCode` are
// valid on it (they dispatch to the user's member), so it must be excluded
// from the §9.2 non-auto-equality compile error. Returns false for null.
bool recordHasUserDeclaredEquality(const TypeSymbol* rec);

} // namespace bezel::sema
