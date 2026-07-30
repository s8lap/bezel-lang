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
//   - MetaMember::methodBodyText (§14.1/#783, additive within v5): for a Method
//     member of a GENERIC class, the FULL method-declaration source text. A
//     SECOND trailing REC_STRING in MEMBER_BLOCK after valueText (methods carry
//     no const value, so valueText is the empty first string) — exactly the
//     shape ctorBodyText uses for Constructor members. It is ADDITIVE: it is
//     emitted ONLY for generic-class methods with a span-extractable body, so a
//     module with no generic methods produces byte-identical metadata to before;
//     a reader predating this field ignores the extra string for Method members
//     (it does not capture a second trailing string for Method kind), and a v4
//     reader rejects a v5 blob via the version check. No bump is required (the
//     rule: a bump is reserved for a change that makes an old reader MISINTERPRET
//     an existing field — this only ADDS a trailing record that old readers skip).
//
// v7 (#2054 / spec §2.8.2, v0.204): contract carriage. The public
// interface records per-member purity plus ordered contract records and
// per-class invariant records. A v7 reader accepts v1-v6 blobs, but marks a
// pre-v7 blob contract-opaque rather than treating it as an empty record set
// (spec §2.8.6 degradation rule).
//
// gains `isExecutable` — true when the artifact was emitted as an executable
// build (§2.8.4 rule 1: it carries a native entry symbol), false for a library.
// It rides the MODULE_BLOCK's top-level counts record as a 4th scalar
// (`[types, functions, constants, isExecutable]`); a reader that finds only 3
// scalars (a v<=5 blob) reads it as false = library, so existing LIBRARY
// artifacts still load unchanged (back-compat). The loader
// (module::loadDependency) reads it to REJECT an executable artifact presented
// as a dependency — a build error per §2.8.4 rule 4. The additive 4th scalar
// alone would not require a bump (old readers skip a trailing scalar), but the
// bump is deliberate: a v5 reader given a v6 executable artifact must REJECT it
// outright (version > its kSchemaVersion) rather than silently mishandle a
// native-entry-carrying artifact as a library — the safe direction for a
// soundness-relevant field. `isExecutable` is NOT folded into semanticHash:
// artifact kind is a build property, not part of the public-INTERFACE content
// that staleness detection tracks.
//
// §26 EXTENSION CARRIAGE (#735, additive within v7): `ModuleMetadata::
// extensions` carries exported `public extension T` blocks as a 6th trailing
// top-level count scalar plus one EXTENSION_BLOCK subblock each, after the
// constants. NO bump: the rule above is that a bump is reserved for a change
// that makes an old reader MISINTERPRET an existing field, and this only ADDS
// a trailing scalar and trailing subblocks. A reader predating the field stops
// after the constants and never sees them — it then reports the pre-#735 clean
// `no member` reject for a cross-module extension call, never a wrong value. A
// module with no exported extension produces byte-identical metadata to before
// (the 6th scalar is only emitted when there is at least one, mirroring the
// #783 methodBodyText precedent).
//
// §2.8.2 / §22.12 / §22.13 (#2351, additive within v7): MetaMember::
// fieldInitText — for a STATIC Field member of a GENERIC class, the
// initializer EXPRESSION source text (span-extracted from `FieldDecl::
// initializer`, not the whole declaration — an Expr carries its own span).
// Mirrors ctorBodyText/methodBodyText exactly: a SECOND trailing REC_STRING
// in MEMBER_BLOCK after valueText for Field members (valueText itself is
// reserved for an INSTANCE field's constant-foldable initializer, §22.8, and
// stays empty for a static field — see ModuleMetadataBuilder), emitted ONLY
// when non-empty, so a module with no generic-class static-field initializer
// produces byte-identical metadata to before. Closes the gap #1513's own
// fix left open: before this, an imported generic type's static field
// reached codegen with `FieldSymbol::decl == nullptr` (ModuleLoader never
// reconstructed one for an imported field), so genericInstSfinitFn's
// `dynamic_cast<const ast::FieldDecl*>(fs2->decl)` was always null and the
// field's initializer never ran on the importing side — §2.8.2 clause 2
// ("an external instantiation behaves identically to a same-module
// instantiation") was violated silently (the shared LinkOnceODR cell #1513
// wired up stayed zero forever). No bump: purely additive, same rule as
// methodBodyText.
//
// §22.4 / §12.1 (#2392, additive within v7): MetaParam::outNullWhen — the
// `when true`/`when false` out-nullability postcondition (0/1/2 mirroring
// ast::OutNullWhen), a SECOND scalar appended to PARAM_BLOCK's existing
// REC_SCALARS record (previously just `[hasDefault]`). No bump: the rule
// above is that a bump is reserved for a change that makes an OLD reader
// misinterpret an EXISTING field; this only adds a trailing scalar a v<=7
// reader's `sc.size() < 1` check still accepts (it never looks past index 0),
// and a v8+ reader given a pre-#2392 blob (only 1 scalar present) reads the
// missing second scalar as 0/None — the same value every such blob's
// parameters implicitly had before this field existed, so no blob is
// misread. Before this field, ModuleLoader::convertParams built every
// imported ParameterSymbol with outNullPostcond stuck at its default None
// regardless of the exporting module's actual declaration, so
// checkOverrideSignatureMatch's exact-match comparison
// (dp->outNullPostcond != bp->outNullPostcond, §12.1) false-rejected any
// correct override of an imported base method/indexer whose base parameter
// declared `when true`/`when false` — recompiling the base module against
// this field picks the fix up for existing dependents (§2.8.5 disclaims ABI
// stability across recompilation; this project's model is recompile-the-
// world, so a stale pre-#2392 dependency artifact keeps the old false-reject
// behavior until its own module is recompiled, same as any other additive
// field in this schema).
inline constexpr std::uint32_t kSchemaVersion = 7;

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
    // §35 (v0.200, #2044): choice kinds carry through the metadata schema so
    // a cross-module importer of a choice does not fall back to Class
    // (#2121). The case-list itself is not yet serialized (cross-module choice
    // consumers are not in scope yet); this enum value is the foundational
    // step that removes the `-Wswitch` warning at toMetaTypeDeclKind /
    // fromMetaTypeDeclKind and keeps the byte in the wire format stable for
    // the future case-serialization work.
    Choice = 4,    // §35
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
    // §22.4 (v0.183, #1585) / §12.1 (v0.235, #2392): the `when true`/`when
    // false` out-nullability postcondition, mirroring ast::OutNullWhen
    // (0 = None, 1 = WhenTrue, 2 = WhenFalse). §12.1 requires an override to
    // match this exactly, so ModuleLoader::convertParams must be able to
    // reconstruct sema::ParameterSymbol::outNullPostcond for an IMPORTED base
    // method's parameter — without this field the reconstructed parameter
    // always read None regardless of what the exporting module declared,
    // producing a false reject of a correct override (issue #2392). Absent
    // (reads as 0/None) on a pre-#2392 blob — see the kSchemaVersion comment.
    std::uint8_t outNullWhen = 0;

    bool operator==(const MetaParam&) const;
};

// §22.16 / §2.8.2 (v0.204, #2054): the by-value contract record carried
// across an artifact boundary. Canonical text is declaration-derived; it is
// deliberately not a semantic normal form (the spec makes textual identity
// the sound, deterministic comparison rule).
enum class MetaContractKind : std::uint8_t {
    Requires = 0,
    Ensures = 1,
    Invariant = 2,
};

enum class MetaContractTier : std::uint8_t {
    Proven = 0,
    Checked = 1,
    Assumed = 2,
};

struct MetaContractRecord {
    MetaContractKind kind = MetaContractKind::Requires;
    bool isStatic = false;
    std::string canonicalText;
    MetaContractTier tier = MetaContractTier::Checked;

    bool operator==(const MetaContractRecord&) const;
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

    // §14.1 / §2.5 (#783, schema v5 additive): for a Method member belonging to
    // a GENERIC class, the FULL method-declaration source text (from the
    // visibility/modifier prefix through the terminating `;`). Emitted as a
    // trailing REC_STRING in MEMBER_BLOCK after valueText (methods have no const
    // value, so valueText is the empty first trailing string and methodBodyText
    // is the second — symmetric to ctorBodyText for Constructors). The importer
    // re-parses this to obtain a live ast::MethodDecl and attaches it to the
    // reconstructed MethodSymbol so the per-instantiation generic-method body
    // (emitInlineGenericMethod / buildSpecializedMethodFn) can be monomorphized
    // importer-side under the concrete genericSubst_ — the method body was
    // compiled in the defining module against the i32 placeholder layout, so a
    // reference-type instantiation (`Box<string>.Get()`) would otherwise return
    // the field at the wrong offset (a silent wrong value; see the #783 sema
    // gate). Empty for non-generic types, for abstract/operator methods, and for
    // any method whose body could not be span-extracted (degenerate). The extra
    // trailing string is additive: a current reader ignores it for Method
    // members (caps[1] is null), and a v4 reader rejects a v5 blob outright.
    std::string methodBodyText;       // §14.1/#783: method source text for generics

    // §2.8.2 / §22.12 / §22.13 (#2351): for a STATIC Field member of a GENERIC
    // class, the initializer EXPRESSION source text (see the schema-header
    // comment above kSchemaVersion for the full rationale). Empty for a
    // non-generic type's static field (it initializes in the declaring
    // module's own compilation and needs no cross-module replay), for an
    // instance field (which uses valueText instead), and for a static field
    // with no initializer.
    std::string fieldInitText;        // §2.8.2/#2351: static field init text

    // §22.15 / §2.8.2: whether this member is declared pure. It is kept as
    // an explicit bit in addition to the general modifier list so consumers
    // need not infer behavioral metadata from spelling conventions.
    bool isPure = false;

    // §22.16 / §2.8.2: clauses in declaration order. For a `when` form from
    // §22.4 the builder emits the equivalent static-ensures record here.
    std::vector<MetaContractRecord> contracts;

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
    // §22.16 / §2.8.2: invariant records, base classes first, then this
    // class's invariant members in declaration order.
    std::vector<MetaContractRecord> invariants;
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

    // §22.15 / §22.16: top-level callable behavioral metadata. The current
    // parser has no free-function declarations, but the schema is complete for
    // the interface requirement and future top-level forms.
    bool isPure = false;
    std::vector<MetaContractRecord> contracts;

    bool operator==(const MetaFunction&) const;
};

// §26 (#735): an exported `public extension T` block. Extensions are
// statically dispatched receiver-directed members (§26.5): the exporter emits
// each extension method as an ExternalLinkage `bz$ext$<T>$<M>` symbol, so the
// importer needs only the block's TARGET type and its methods' SIGNATURES to
// resolve `x.M()` (§26.5 step 2 — "the compiler searches the current module and
// all imported modules for extension blocks on x's type") and emit the call.
// No body text crosses the boundary (unlike the generic-class #662c/#783 paths).
//
// SCOPE (decline-with-reasons, CLAUDE.md §4): the builder exports only the
// shapes whose cross-module dispatch is exactly the exported symbol —
// a `public` block on a NON-generic target with no `where` clause, carrying
// `public` NON-generic methods. A generic extension head (`extension Bag<T>`,
// or a generic extension method `Map<U>`) would need importer-side
// monomorphization of the body (the §14.1/#783 machinery) because the exported
// symbol was compiled against the exporter's placeholder layout; an extension
// PROPERTY has no exported accessor symbol at all (§26.1 properties are
// inlined at the use site). Those shapes are not exported, so the importer
// keeps its existing clean "no member" reject rather than mis-dispatching.
struct MetaExtension {
    MetaType target;                  // §26.1 the extended type
    MetaVisibility visibility = MetaVisibility::Public;
    std::vector<MetaMember> members;  // §26.3 methods (this increment)

    bool operator==(const MetaExtension&) const;
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
    // §26 / §2.5 (#735): exported extension blocks. Rides as a 6th top-level
    // count scalar plus one trailing EXTENSION_BLOCK each, so a blob with no
    // exported extensions is byte-identical to a pre-#735 one.
    std::vector<MetaExtension> extensions;

    // §2.8.4 rule 1 / rule 4 (issue #1733, schema v6): the artifact's KIND.
    // true = this artifact was emitted as an EXECUTABLE build (it carries a
    // native entry symbol); false = a LIBRARY (the default, and the value a
    // v<=5 blob without this field deserializes to). The loader rejects a
    // dependency whose artifact is executable-kind (§2.8.4 rule 4). Not part of
    // the semantic hash (a build property, not public-interface content).
    bool isExecutable = false;

    // §2.8.6 degradation rule (v0.204): false means this metadata came from a
    // pre-carriage artifact and makes no contract claim. Current metadata built
    // by this compiler defaults to true, even when the explicit record vectors
    // are empty; an absent record set is never fabricated as an empty set.
    bool contractRecordsCarried = true;

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
