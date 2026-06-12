#pragma once

// Module public-interface metadata (§2.5 / §2.6).
//
// Separate compilation (§2.5) requires that a compiled module embed its
// PUBLIC-INTERFACE metadata — type signatures, method signatures, generic
// constraints — so a downstream module's compilation can resolve `import`s
// (§2.2) and type-check against the imported module without its sources.
// §2.6 step 4 ("Generate native code; emit the artifact with embedded
// interface metadata") and §2.6's closing note ("Build-tool integration and
// project-file format are implementation-defined") delegate the *format* to
// the implementation; issue #328 records the locked design:
//
//   - storage:        embed in a dedicated object/`.so` section (not sidecar)
//   - serialization:  LLVM's bitstream library (zero new dependency)
//   - versioning:     schema-version + semantic-hash header, with
//                     read-older / skip-unknown semantics (the Java .class
//                     model) — NOT tied to the compiler version
//
// THIS FILE IS INCREMENT 1 ONLY: a standalone, unit-tested in-memory schema
// plus bitstream serialize/deserialize. It does NOT reuse live sema Symbol*
// pointers (that conversion is Increment 2's codegen-emit hook), is not wired
// into codegen, the driver, or sema import-resolution (Increments 2/3), and
// touches no other source file. The schema is modelled on src/sema/Symbol.hpp
// and src/sema/TypeRef.hpp but is fully DECOUPLED: it is a serialization
// schema by value, holding owned std::strings and indices, not symbol graph
// pointers — so it survives across compilations and process boundaries.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bezel::module {

// --- Header / versioning (issue #328: schema-version + semantic-hash) -------

// Magic bytes opening every serialized blob ("BzMd" = Bezel Module metadata).
// Lets a reader reject a section that is not Bezel module metadata before it
// trusts any other field.
inline constexpr std::uint32_t kMetadataMagic = 0x644D7A42; // 'B','z','M','d' LE

// Schema version. Bumped when the *meaning* of the wire format changes in a way
// a reader must understand. Read-older / skip-unknown semantics (issue #328,
// Java .class model) mean: a newer reader can read an older blob, and a reader
// skips records/blocks it does not recognise — so additive changes (new
// optional record codes, new trailing fields read via skip) do NOT require a
// bump. A bump is reserved for changes that would make an old reader
// MISINTERPRET an existing field.
//
// v2 (issue #328 FINDING [3] BUG A): MetaMember carries `valueText`, the
// compile-time-constant value of a class-level `const` (§22.12), so a
// cross-module importer folds the value at the read site. It is emitted as a
// trailing REC_STRING in the MEMBER_BLOCK; a v2 reader reads it tolerantly
// (absent → empty), so a v2 reader still reads a v1 blob. The bump exists
// because a hypothetical v1 reader given a v2 blob would not expect the trailing
// record — but a v1 reader rejects a v2 blob anyway (version > its
// kSchemaVersion), per the read-older / skip-unknown rule.
//
// v3 (issue #328 FINDING [7]): IMPORTED CONSTRUCTORS + FIELD INITIALIZERS join
// the cross-module surface. No WIRE-FORMAT change is introduced: the
// MetaMemberKind::Constructor member and the per-member trailing `valueText`
// REC_STRING were both already part of the v2 format (the builder emitted ctor
// members, and `valueText` rides every member). What changed is the MEANING the
// CONSUMER assigns to those bytes — the loader now reconstructs constructor
// members as ConstructorSymbols, and reads an INSTANCE field's `valueText` as a
// constant-foldable §22.8 field initializer (not just a §22.12 const value). The
// bump records that semantic step and lets a build tool distinguish an artifact
// whose importer-side ctor/field-init support is present. Read-older holds in
// both directions: a v3 reader reads a v2 (or v1) blob unchanged — a v2 blob
// simply carries no instance-field `valueText` (absent → empty) and its ctor
// members reconstruct exactly as a v3-produced blob's would — and a v2 reader
// rejects a v3 blob (version > its kSchemaVersion), per the documented rule.
//
// v4 (issue #501 / #502): two metadata extensions:
//   - MetaTypeDecl::hasPositionalFields (§9.2 / issue #501): a new 6th scalar
//     in TYPE_BLOCK's header. v3 readers reject v4 blobs (version check) and
//     v4 readers read v3 blobs with only 5 scalars (missing 6th → false).
//   - MetaMemberKind::NestedType = 8 (§11.3 / issue #502): a new member kind
//     that carries an embedded TYPE_BLOCK for the nested type's full declaration.
//     v3 readers reject v4 blobs. v4 readers reconstruct NestedTypeSymbol so
//     that `new Outer.Inner()` resolves cross-module.
//
// v5 (#662 foundation C): constructor/destructor bodies for imported generic
// classes.
//   - MetaMember::ctorBodyText (§14/§22.7): for a Constructor member belonging
//     to a GENERIC class, the FULL constructor-declaration source text (from
//     the constructor keyword through `end constructor;`). A second trailing
//     REC_STRING in MEMBER_BLOCK after valueText. Empty for non-generic types
//     (they cross the boundary via the external bz$T$ctor ABI) and for any
//     constructor whose body could not be extracted (degenerate — no span).
//     v4 readers tolerate the extra string by the finishBlock skip-unknown path.
//   - MetaTypeDecl::dtorBodyText (§14/§28.3): for a GENERIC class whose
//     destructor has a non-empty body, the FULL destructor-declaration source
//     text. A trailing REC_STRING appended to TYPE_BLOCK after the members.
//     v4 readers tolerate it via finishBlock.
inline constexpr std::uint32_t kSchemaVersion = 5;

// --- Type representation (decoupled mirror of sema::TypeRef) ----------------

// §3 type kinds, as they appear in a serialized signature. Mirrors
// sema::TypeRef::Kind but is a standalone wire enum (its numeric values are
// part of the format and must stay stable across schema versions).
enum class TypeKind : std::uint8_t {
    Error = 0,       // unresolved sentinel (should not normally be emitted)
    Primitive = 1,   // §4 built-in type
    Named = 2,       // §9-§12 user-declared class/interface/record/enum
    Unit = 3,        // () §3.1
    InlineArray = 4, // [N]T or [N owned]T §15
    Function = 5,    // (T1, ...) -> R §27
};

// §4 primitive types. Mirrors sema::PrimitiveType; numeric values are part of
// the wire format. `None` is the in-transit "not a primitive" sentinel.
enum class PrimitiveKind : std::uint8_t {
    None = 0,
    Bool = 1, Char = 2, String = 3,
    Byte = 4, SByte = 5, Short = 6, UShort = 7,
    Int = 8, UInt = 9, Long = 10, ULong = 11,
    Float = 12, Double = 13, Decimal = 14,
    Object = 15,
};

// A type as it appears in a signature (field type, return type, parameter
// type, base type, generic argument). Decoupled from sema::TypeRef: rather
// than a TypeSymbol* it carries the referent's fully-qualified name by value
// (`namedName`), so the downstream loader resolves it by name (§2.5) against
// the imported module's exported declarations.
struct MetaType {
    TypeKind kind = TypeKind::Error;
    bool nullable = false; // §3.3 trailing `?`

    // Primitive:
    PrimitiveKind primitive = PrimitiveKind::None;

    // Named: the referent's name (a module-qualified or simple type name, or a
    // generic-parameter name when this type is a use of an in-scope `T`). The
    // loader resolves it; this schema does not embed a symbol pointer.
    std::string namedName;
    std::vector<MetaType> genericArgs; // §14 type arguments

    // InlineArray: [arraySize]element, owning when arrayOwned. arraySize is the
    // textual form of the size expression (§15: a compile-time constant); the
    // loader re-evaluates it. Empty when unknown.
    std::string arraySize;
    bool arrayOwned = false;

    // Function: parameter list + return. funcReturnOwned mirrors `owned T`
    // returns (§28). The single optional child for InlineArray's element and
    // Function's return is stored in `children`:
    //   InlineArray -> children[0] is the element type
    //   Function    -> children[0..n-1] are param types (with funcParamMods /
    //                  funcParamNames parallel), children[n] is the return type
    std::vector<MetaType> children;
    std::vector<std::string> funcParamMods;  // "", "ref", "out", "owned", ...
    std::vector<std::string> funcParamNames; // optional, parallel to params
    bool funcReturnOwned = false;

    bool operator==(const MetaType&) const;
    bool operator!=(const MetaType& other) const { return !(*this == other); }
};

// --- Declaration representation (decoupled mirror of sema::Symbol) ----------

// §13 visibility. Mirrors ast::Visibility; numeric values are part of the wire
// format. Only PUBLIC (and PROTECTED, which is part of a public type's
// inheritance surface) members are emitted for a library's interface, but the
// field is carried for fidelity and so the loader can enforce §13.
enum class MetaVisibility : std::uint8_t {
    None = 0,
    Public = 1,
    Private = 2,
    Internal = 3,
    Protected = 4,
};

// §A.3 TypeDeclKind. Numeric values are part of the wire format.
enum class MetaTypeDeclKind : std::uint8_t {
    Class = 0,     // §11
    Interface = 1, // §11
    Record = 2,    // §9
    Enum = 3,      // §10
};

// §14 generic parameter (`Variance? Identifier`). Variance is "in", "out", or
// empty (§14.x). Constraints are the resolved `where T: ...` bounds as types
// (e.g. a base class or interface) plus textual special constraints
// ("class", "value", "new") that are not nameable types.
struct MetaGenericParam {
    std::string name;
    std::string variance; // "", "in", "out"
    std::vector<MetaType> constraintTypes;       // §14.2 type constraints
    std::vector<std::string> specialConstraints; // "class", "value", "new", ...

    bool operator==(const MetaGenericParam&) const;
};

// §22 parameter. Mirrors sema::ParameterSymbol: a modifier prefix
// ("", "ref", "out", "out owned", "owned" — §22 / §28), the parameter type,
// and whether it has a default value (§22.5; the value itself is its textual
// form so the loader can reproduce the default for named/optional-arg calls).
struct MetaParam {
    std::string name;
    std::string modifier; // "", "ref", "out", "out owned", "owned"
    MetaType type;
    bool hasDefault = false;
    std::string defaultText; // §22.5 textual default; empty when !hasDefault

    bool operator==(const MetaParam&) const;
};

// The member kinds of a type's public surface (§9-§12, §22-§25). Numeric
// values are part of the wire format.
enum class MetaMemberKind : std::uint8_t {
    Field = 0,        // §11 field / §9.2 auto-property storage
    Method = 1,       // §22 method (incl. operator overloads §25)
    Property = 2,     // §22.9 property
    Indexer = 3,      // §22.9 indexer (property with index params)
    Constructor = 4,  // §11 constructor
    Event = 5,        // §24 event
    Const = 6,        // §11 const member
    EnumConstant = 7, // §10 enum constant
    NestedType = 8,   // §11.3 nested class/record/enum (issue #502)
};

// Forward-declare MetaTypeDecl for use in MetaMember::nestedType.
struct MetaTypeDecl;

// A public member of a type. One struct covers all member kinds (distinguished
// by `kind`); fields irrelevant to a kind stay default. This mirrors the
// sema MemberSymbol hierarchy but flattened for serialization.
struct MetaMember {
    MetaMemberKind kind = MetaMemberKind::Field;
    std::string name;                 // operator token for operator methods
    MetaVisibility visibility = MetaVisibility::Public;
    std::vector<std::string> modifiers; // "static","abstract","virtual",
                                        // "override","sealed","async","readonly"
    MetaType type;                    // field/property/const type; method return

    // Const (§22.12): the member's compile-time-constant value, textual. A
    // class-level `const` initializer "must be a compile-time-constant
    // expression" (spec §22.12), so a cross-module importer can FOLD this value
    // at every read site without referring to any storage symbol — the library
    // emits no global for a const (issue #328 FINDING [3] BUG A). Empty for
    // non-const members and for a const whose initializer renderConstExpr could
    // not render (the importer then falls back to no value, as before the fix).
    std::string valueText;            // §22.12 const value, textual

    // §14 / §22.7 (#662 foundation C, schema v5): for a Constructor member
    // belonging to a GENERIC class, the FULL constructor-declaration source text
    // extracted from the exporter's source (span-based slice). Non-empty only
    // when the class has generic parameters AND the constructor has a body that
    // could be span-extracted. The importer re-parses this to obtain a live
    // ast::ConstructorDecl, runs the minimal sema passes, and attaches it to the
    // reconstructed ConstructorSymbol so the inline-ctor path (inlineCtorBody)
    // can emit the body with genericSubst_ active (§14.1 monomorphization).
    // Empty for non-generic types (they use the external bz$T$ctor ABI) and for
    // any constructor whose body was absent or could not be extracted.
    std::string ctorBodyText;         // §14/#662c: ctor source text for generics

    // Method / Constructor / Indexer:
    std::vector<MetaGenericParam> genericParams; // §14 (methods only)
    std::vector<MetaParam> params;
    bool isOperator = false;          // §25 operator overload
    bool returnOwned = false;         // §28 `owned T` return

    // EnumConstant (§10): the declared underlying value, when explicit.
    bool hasEnumValue = false;
    std::int64_t enumValue = 0;

    // NestedType (§11.3, issue #502): when kind == NestedType, this holds
    // the complete MetaTypeDecl for the nested type. Heap-allocated via
    // unique_ptr to break the cyclic MetaMember → MetaTypeDecl → MetaMember
    // dependency (MetaTypeDecl::members is std::vector<MetaMember>). Null
    // for all other member kinds.
    std::unique_ptr<MetaTypeDecl> nestedType;

    // MetaMember must be movable (for vector resize) but not copyable in
    // the presence of unique_ptr. We provide explicit copy semantics via
    // operator==; the serializer/deserializer and loader only move or
    // construct in place, so move-only is fine for production paths.
    MetaMember() = default;
    MetaMember(MetaMember&&) = default;
    MetaMember& operator=(MetaMember&&) = default;
    MetaMember(const MetaMember& o);
    MetaMember& operator=(const MetaMember& o);

    bool operator==(const MetaMember&) const;
};

// A public top-level type declaration (§9-§12). Mirrors sema::TypeSymbol.
struct MetaTypeDecl {
    std::string name;
    MetaTypeDeclKind kind = MetaTypeDeclKind::Class;
    MetaVisibility visibility = MetaVisibility::Public;
    std::vector<std::string> modifiers; // "abstract","sealed","static"
    std::vector<MetaGenericParam> genericParams; // §14
    // §12 base class + interfaces, as types (carrying their generic args). By
    // convention the first entry, if it is a class, is the base class; the rest
    // are interfaces — same convention as sema::TypeSymbol::baseTypeRefs.
    std::vector<MetaType> baseTypes;
    std::vector<MetaMember> members;
    // §9.1 / §9.2 (issue #501): true when this type is a positional record
    // (declared with a `(field...)` list). Carried in the metadata so an
    // importer can reconstruct TypeSymbol::hasPositionalFields, which the
    // §9.2 auto-equality check reads when there is no ast::TypeDecl.
    bool hasPositionalFields = false;

    // §14 / §28.3 (#662 foundation C, schema v5): for a GENERIC class whose
    // destructor has a user-written body, the FULL destructor-declaration source
    // text extracted from the exporter's source (span-based slice). Non-empty
    // only when the class has generic parameters AND the destructor body is
    // non-empty AND span extraction succeeded. The importer re-parses this to
    // obtain a live ast::DestructorDecl and attaches it to the reconstructed
    // TypeSymbol's destructor slot so the §28.3 disposal path can run the body.
    // Empty means "no user destructor body" (structural disposal still runs).
    std::string dtorBodyText;         // §14/#662c: dtor source text for generics

    bool operator==(const MetaTypeDecl&) const;
};

// A public top-level function (§22 free function). Top-level functions and
// constants are part of a module's exported surface.
struct MetaFunction {
    std::string name;
    MetaVisibility visibility = MetaVisibility::Public;
    std::vector<std::string> modifiers; // "async","static",...
    std::vector<MetaGenericParam> genericParams;
    std::vector<MetaParam> params;
    MetaType returnType;
    bool returnOwned = false;

    bool operator==(const MetaFunction&) const;
};

// A public top-level constant (§11 const at module scope).
struct MetaConstant {
    std::string name;
    MetaVisibility visibility = MetaVisibility::Public;
    MetaType type;
    std::string valueText; // §11 const value, textual

    bool operator==(const MetaConstant&) const;
};

// The complete public interface of a module (§2.1 module name + §2.4 exported
// surface). This is the root serialized object.
struct ModuleMetadata {
    std::string moduleName;               // §2.1 the `module X;` name
    std::vector<std::string> exports;     // exported top-level names (§2.4)
    std::vector<MetaTypeDecl> types;
    std::vector<MetaFunction> functions;
    std::vector<MetaConstant> constants;

    bool operator==(const ModuleMetadata&) const;
    bool operator!=(const ModuleMetadata& other) const {
        return !(*this == other);
    }
};

// --- Semantic hash (issue #328 staleness detection) -------------------------

// A stable 64-bit hash over the PUBLIC-INTERFACE *content* (not the wire bytes:
// it is computed from the in-memory schema, so it is independent of encoding
// details and stable across a serialize/deserialize roundtrip). Two modules
// with the same public interface hash identically; any change to the public
// interface (a renamed/added/removed/retyped member, a changed signature)
// changes the hash. Increment 3's loader compares this against a recorded
// value to detect a stale dependency without re-reading the whole blob.
std::uint64_t semanticHash(const ModuleMetadata& md);

// --- Serialize / deserialize (issue #328: LLVM bitstream) -------------------

// Encode `md` to a self-describing byte blob via LLVM's bitstream library. The
// blob opens with a header record (magic, schema version, semantic hash) so a
// reader can validate compatibility before decoding the body. The body uses
// unabbreviated bitstream records inside nested subblocks; a reader skips any
// record code or subblock id it does not recognise (read-older /
// skip-unknown, issue #328).
std::vector<std::uint8_t> serialize(const ModuleMetadata& md);

// Decode a blob produced by serialize(). Returns std::nullopt if the blob is
// not Bezel module metadata (bad magic) or its schema version is newer than
// this reader understands (forward-incompatible). The returned metadata's
// semantic hash equals the value recorded in the blob header on a clean
// roundtrip.
std::optional<ModuleMetadata> deserialize(const std::vector<std::uint8_t>& bytes);

// The schema version recorded in a blob's header, without fully decoding it.
// std::nullopt on bad magic or a truncated header. (Lets a build tool report a
// stale/incompatible artifact cheaply.)
std::optional<std::uint32_t> readSchemaVersion(
    const std::vector<std::uint8_t>& bytes);

// The semantic hash recorded in a blob's header, without fully decoding the
// body. std::nullopt on bad magic or a truncated header. (Lets the loader
// staleness-check a dependency without decoding it.)
std::optional<std::uint64_t> readSemanticHash(
    const std::vector<std::uint8_t>& bytes);

// Test-only: emit the same content as serialize(), plus an unknown record code
// and an unknown subblock id (as a FUTURE schema version would). Used to verify
// the reader's read-older / skip-unknown semantics (issue #328, the Java
// .class forward-compat model): a current reader must skip what it does not
// recognise and still decode the known content. Not used outside tests.
std::vector<std::uint8_t> serializeWithUnknownExtras(const ModuleMetadata& md);

} // namespace bezel::module
