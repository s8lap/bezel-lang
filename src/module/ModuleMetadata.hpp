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
// §14.1 / §23 / §23.1 / §23.3 / §23.4 / §2.5 / §2.8.2 / §2.8.3 rule 4
// (#2893, additive within v7 — sibling of ctorBodyText/methodBodyText):
// MetaMember::propertyBodyText — for a Property (or Indexer) member
// belonging to a GENERIC class or record, the FULL property/indexer
// declaration source text. Mirrors the ctor/method carriages exactly:
// a SECOND trailing REC_STRING in MEMBER_BLOCK after valueText for
// Property/Indexer members (valueText itself stays empty for these
// shapes — it is reserved for a const value, §22.12, or an instance
// auto-property's constant-foldable initializer, #2467), emitted ONLY
// when non-empty, so a module with no generic-class property/indexer
// body produces byte-identical metadata to before. Closes the
// capability gap #2884 left open as a DECLINE: every spelling of a
// property access on an imported generic class or record was BROKEN
// (computed read → link error, computed `:=` → link error, auto
// read/`:=` → same link error, virtual read/`:=` → SIGSEGV, `+=`/`++`
// → silent dropped write), and the cheap alternative — exporting one
// placeholder-layout stub for the open type — was prototyped and
// MEASURED unsound (the #2884 commit message documents the silent
// wrong-value measurement on `Off<string>`). No bump: purely additive,
// same rule as methodBodyText and fieldInitText.
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
//
// §22.3 / §23 (#2461, additive within v7): MetaMember::hasGetter — the
// getter-presence twin of #2400's hasSetter (an 11th scalar). A 14th
// trailing scalar in MEMBER_BLOCK's REC_SCALARS. NO bump: the rule above is
// that a bump is reserved for a change that makes an OLD reader misinterpret
// an EXISTING field, and this only ADDS a trailing scalar — a pre-#2461
// reader's `sc.size() < 8` gate never looks past index 7, so it is
// unaffected. A v7+ reader given a pre-#2461 blob (at most 13 scalars) reads
// the missing 14th scalar as the field's default `true` (NOT `false` like
// hasSetter — see the hasGetter field comment for why the opposite default is
// load-bearing: a false default would false-reject a legitimate `ref` to a
// read-write property from a stale dependency). Closes the message-quality
// gap where a write-only imported property used as a `ref` argument fell
// through sema to a raw linker error (undefined `...$propget`) instead of a
// clean §22.3/§23 rejection; see checkRefOutPropertyOperand's `!pd` branch
// (ExprTypecheck.cpp) for the consuming side.
//
// §22.15 (#3618, additive within v7): MetaMember::accessorIsPure — whether
// a property or indexer's GETTER ACCESSOR is declared `pure get` (§22.15
// "Where `pure` may appear" / "Purity attaches to the getter"). A 19th
// trailing scalar in MEMBER_BLOCK's REC_SCALARS. NO bump for the same reason
// as #2461's hasGetter — the slot only ADDS a trailing scalar a pre-#3618
// reader's `sc.size() < 18` gate never reaches; a v7+ reader given a
// pre-#3618 blob (at most 18 scalars) reads the missing 19th as the field
// default `false` (the same "not accessor-pure" classification a stale
// dependency already exhibits today — its imported `pure get` is a silent
// non-pure getter from the importer's perspective, exactly the direction the
// bug measures). Closes the #3618 seam where the property-level `pure`
// (read-only / expression-bodied shape) crossed the wire via
// `MetaMember::isPure` but the accessor-level `pure get` (read-write
// block-form shape — §22.15's REQUIRED spelling there, see
// TypeResolution_Checks.cpp:11524) had no slot, so the importer answered
// `false` on every imported block-form pure getter while the exporter
// answered `true` on the character-identical source. Consulted by
// `propertyGetterIsPure` (semantically equivalent to `memberIsPure` for
// properties across BOTH spellings), which feeds §22.15 rule 6's
// accessor-read arm.
//
// §28.9 / §3.6 rule 3 / §2.8.3 rule 3 (fix #3647/#3648, v0.286, DECLINED
// bump — record the reasoning, not just the decision): #3632 widened the
// §28.9 trivial-dispose predicate (an `owned` field of trivial-dispose
// TYPE no longer disqualifies its holder), and #3647 lands that widening
// at the exporter above (`out.nonTrivialDispose = ...`), so a rebuilt
// producer's carried MetaTypeDecl::nonTrivialDispose bit can now read
// `false` where an older producer read `true` for the SAME source. The
// filed plan for #3648 called this a wire-format change requiring a
// bump; it is not, by this constant's own rule stated at its header
// ("reserved for changes that would make an old reader MISINTERPRET an
// existing field") — the wire SHAPE of `nonTrivialDispose` (one bool, same
// position) is unchanged; only the exporter's COMPUTED VALUE for it
// changes, exactly like every other predicate bug fix that touches
// exported content, none of which bump this constant. Two things make a
// bump not just unnecessary but pointless here:
//   1. `nonTrivialDispose` was ALREADY folded into `semanticHash`
//      (ModuleMetadata.cpp's `hashTypeDecl`, fix #3191/0bb85f1, citing
//      §2.8.3 rule 3 by name) and is part of `MetaTypeDecl::operator==`
//      — the §2.8.3 rule 3 "descriptions must agree" staleness check this
//      field exists to satisfy is keyed on CONTENT comparison across the
//      artifact closure, not on this schema counter, so it already
//      detects a diamond where two dependents embed disagreeing
//      descriptions of one type, unconditionally of this bump.
//   2. The `Consult` short-circuit that reads the carried verdict
//      (`TypeSymbol::importedNonTrivialDispose`, OwnershipFacts.cpp) is a
//      literal bit-trust, not a re-derivation: an old (pre-#3647)
//      compiler reading a freshly-rebuilt (post-#3647) dependency reads
//      its correct, relaxed verdict verbatim; a new compiler reading a
//      STALE (not-yet-rebuilt) dependency reads its old, MORE-conservative
//      verdict, which is safe by construction (§28.9's widening is
//      one-directional: non-trivial -> trivial only, never the reverse,
//      for a fixed structural input) rather than unsound. Neither
//      direction needs "reject the artifact until it version-matches" —
//      the recompile-the-world model (§2.8.5) already treats a stale
//      dependency's over-conservative answer as an ordinary
//      not-yet-rebuilt inconvenience, same as any other classification
//      fix.
// §3.6 rule 3 is the correct citation for "a derived-property flip is
// part of the public contract" — #3648's filed body cited "§2.8.3 rule 4",
// which is the runtime-type-identity rule (one copy of static state, one
// `is`/`catch`/pattern identity) and says nothing about verdict flips;
// §2.8.3 rule 3 (staleness/disagreement) and §3.6 rule 3 (a flip is a
// breaking change "exactly as a signature change is") are the two rules
// that actually apply, and both are already the citations the #3191 fold
// above carries.
inline constexpr std::uint32_t kSchemaVersion = 7;

// §2.5 / §2.8.2 (issue #3198, v0.254): the producer's content-revision
// stamp. A 32-bit integer the producer stamps in every artifact's header
// record; the loader compares it against its own stamp and rejects the
// artifact (with a diagnostic naming the dependency and the remedy — rebuild
// it) if they differ.
//
// This is a SECOND axis of versioning orthogonally to `kSchemaVersion`. The
// schema version is reserved for changes that alter the wire format's meaning
// in a way an old reader cannot survive (issue #328's read-older rule, the
// Java .class model). The producer content revision is reserved for changes
// in WHAT THE WRITER WRITES, and in the importer-visible codegen ABI (the
// vtable slot keys and layout an importer reproduces from the metadata). A
// commit that adds a new metadata field, a new fold into the semantic hash, a
// new modifier-rendering, or changes that ABI must bump it by hand.
// Without the stamp, an additive content change is LOADED SILENTLY by a
// new compiler reading an old artifact: the new field's hash fold is missing,
// the new modifier's readers hit the "absent" branch, and the importer
// makes a wrong call about ownership, layout, or disposal — exactly the
// silent-acceptance failure shape the issue measures.
//
// The stamp is ABSENT (hdr.size() < 5 → reads as 0) on any artifact written
// before this field landed, which is precisely the rejection we want — a
// recompile-the-world migration (§2.8.5 disclaims ABI stability across
// recompilation, so the cost is a build-time inconvenience, not a
// compatibility trap). A new reader given an old artifact rejects it with
// a diagnostic naming the dependency; an old reader given a new artifact
// silently ignores the 5th field (the existing reader's `hdr.size() < 4`
// gate never looks past index 3).
// fix #3540 (§11.6 / §2.8.3 rule 3, v0.285) bumped 1 -> 2: a `MetaType::Named`
// for a §11.6 NESTED type now carries the QUALIFIED type name ("Outer.Box")
// where it used to carry the simple one ("Box"). That is a change in WHAT THE
// WRITER WRITES, which is exactly what this stamp exists to police — a v0.285
// compiler reading a pre-#3540 artifact would look "Box" up in a table keyed by
// "Outer.Box", miss, and fall back to the i32 lowering, i.e. reproduce the very
// defect on a stale dependency instead of rejecting it.
// fix #6074 (§12.1 / §27.8, v0.338) bumped 2 -> 3: the vtable slot key of a
// parameter with generic ARGUMENTS now includes those arguments, which changes
// the importer-visible codegen ABI. An artifact stamped with another revision
// is refused at import with the existing diagnostic (rc 1, "built by a
// compiler with a different metadata content revision"), and must be rebuilt.
inline constexpr std::uint32_t kProducerContentRevision = 3;

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

    // fix #7094 (§17.11 / §2.8.2 clause 2, v0.343): this type
    // parameter's substitution MUST have a well-defined `default` value.
    //
    // §17.11 makes the obligation a property of the GENERIC BODY, not of the
    // type argument list: a body that takes `default(T)` (directly, or through
    // a chain that ends in a `default(T)`) is rejected at every non-defaultable
    // substitution of T. The same-module typechecker discharges that obligation
    // by joining the body's use of `default(T)` against the instantiation site
    // (§17.11's closing rule). Across a module boundary the importer has no
    // body to join against, so the exporter ships the obligation itself: this
    // flag says "substituting this parameter requires a defaultable type".
    //
    // It is deliberately NOT a `where` constraint — it is not part of the
    // declared signature, and it is not something the source says. It is the
    // §17.11 obligation, carried, so §2.8.2 clause 2 ("an external
    // instantiation behaves identically to a same-module instantiation of the
    // same generic at the same arguments") can hold.
    //
    // WIRE: a 3rd scalar in GENERIC_BLOCK's REC_SCALARS, additive within the
    // current schema version (the #2392 `outNullWhen` precedent): a reader
    // predating this field never looks past index 1, and a current reader
    // given a predating blob reads the missing 3rd scalar as false — the
    // value every blob had before this field existed.
    bool requiresWellDefinedDefault = false;

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
};

struct MetaContractRecord {
    MetaContractKind kind = MetaContractKind::Requires;
    bool isStatic = false;
    std::string canonicalText;
    MetaContractTier tier = MetaContractTier::Checked;

    bool operator==(const MetaContractRecord&) const;
};

// §35.1 / §35.3 / §35.4 (fix #3144, family F9 of umbrella #3152; additive
// within v7): one payload field of one choice case — `owned`? Identifier ":"
// Type SerializedClause?, per §A.3 `CaseField`. Mirrors the `ast::Param` the
// LOCAL side keeps in `ast::ChoiceCase::payload`, by value.
//
// A choice's payload fields are its ENTIRE instance storage (§35.1: "a choice's
// instance storage *is* its cases' payloads"), so this is not descriptive
// metadata — it is the type's LAYOUT, exactly as `builtinExceptionBase` is a
// class's §31.4 header. See MetaTypeDecl::choiceCases for the measured
// consequence of it not crossing.
struct MetaChoicePayloadField {
    std::string name;
    // "" or "owned" (§28.1's payload marker; §35.1 admits no other modifier on
    // a case field).
    std::string modifier;
    MetaType type;
    // §9.8 / §A.3 SerializedClause on the field. `hasSerialized` distinguishes
    // "no clause" from "a clause with an empty rename" — the two differ for
    // `serialized skip` / `serialized optional`, which carry no name.
    bool hasSerialized = false;
    bool serializedSkip = false;
    bool serializedOptional = false;
    std::string serializedName;

    // §35.7 "Equality, hashing, ToString" / §9.2 "The `equality skip` clause"
    // (v0.307; issue #4652, follow-up to fix #4643/#3152 family F9): this
    // payload field's `equality skip` clause (A.3 `CaseField`'s trailing
    // `EqualityClause?`).
    //
    // §35.7 states the clause reads per §9.2's skip-model table — "excluded
    // from its case's payload comparison and hash, exempt from admissibility"
    // — and §9.2 states the artifact interface carries the clause "exactly as
    // it carries `serialized` clauses": adding or removing it on a public
    // choice's payload is a breaking change (§3.6 rule 3), because it flips
    // `=`/`GetHashCode` results for clients and can flip the choice's §35.7
    // auto-hashable / §3.6 hashable derived properties (the ELIGIBILITY GATE
    // direction — a class-typed payload marked `equality skip` rescues an
    // otherwise-inadmissible case from disqualifying the whole choice).
    // #4643 landed the record-side sibling field but explicitly declared the
    // choice site out of scope (tests/parser/equality_skip_clause_test.cpp);
    // this is the wire-carriage half of closing that gap, following the
    // `MetaMember::equalitySkip` (fix #3152/0892d092) template: a symbol/AST
    // field, one record code, hash+operator==+emit+read.
    //
    // Emitted under its own record code (REC_CHOICE_EQ_CLAUSE) and only when
    // TRUE, so a module using no `equality skip` on a choice payload is
    // byte-identical to a pre-#4652 build. Absent (a pre-#4652 blob) reads as
    // `false`, i.e. "participates in equality" — the reading every
    // dependency's own compiler already produced, since no compiler before
    // this fix read the clause for a choice payload at all (in-module or
    // across an import).
    bool equalitySkip = false;

    bool operator==(const MetaChoicePayloadField&) const;
};

// §35.1 (fix #3144): one case of a choice, in declaration order — the order IS
// the case's tag value (LlvmBackend_Layout.cpp's `choiceCasesOf`), so the
// vector's order is part of the wire contract, not a convenience.
struct MetaChoiceCase {
    std::string name;
    std::vector<MetaChoicePayloadField> payload;  // empty = payload-free case
    // §9.8: the case's `serialized "<name>"` rename (the only clause form §A.3
    // admits on a case). `hasSerialized` as above.
    bool hasSerialized = false;
    std::string serializedName;

    bool operator==(const MetaChoiceCase&) const;
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

    // §14.1 / §23 / §23.1 / §23.3 / §23.4 / §2.5 / §2.8.2 / §2.8.3 rule 4
    // (fix #2893, v0.240): for a Property (or Indexer) member belonging to
    // a GENERIC class or record, the FULL property-declaration source text
    // (from the visibility/modifier prefix through the terminating `;`).
    // Emitted as a second trailing REC_STRING in MEMBER_BLOCK, exactly
    // mirroring `methodBodyText` (the #783 precedent, lines 480-497 above)
    // and `ctorBodyText` (the #662c precedent) — a current reader ignores
    // it for Property/Indexer members (caps[1] is null), and a v7 reader
    // rejects a v8 blob outright. Non-empty ONLY when the property/indexer
    // has a body worth re-parsing AND the parent class is generic — same
    // scope as the ctor/method carriages, which the parent commit's
    // measurements (a generic class's METHODS fall back to a placeholder
    // stub that #2884 measured UNSOUND — `Off<string>` answered 100/8
    // where 108/21 are correct because the placeholder layout does not
    // match the instantiated layout) prove MUST NOT apply to properties
    // either. Empty for a non-generic class (the exporter's
    // `bz$<Class>$<Prop>$propget` ABI is correct), for an Auto-bodied
    // property/indexer (no user body — the #2478 `isAutoProperty` bit is
    // enough), for an abstract property/indexer (no body by §12.5), and
    // for any whose body could not be span-extracted (degenerate span).
    //
    // The importer re-parses this in a fresh SemaContext
    // (ModuleLoader.cpp parseAndAttachPropertyBody, the property/indexer
    // twin of parseAndAttachMethodBody), attaches the resulting
    // ast::PropertyDecl to the imported PropertySymbol, and the existing
    // `computedGetterOf`/`blockGetterOf`/`computedSetterOf` codegen paths
    // take over — monomorphizing the accessor body per instantiation the
    // same way `buildSpecializedMethodFn` already does for methods. The
    // cheap-looking alternative — exporting ONE placeholder-layout stub
    // for the open type, which is what a generic class's METHODS fall back
    // to — was prototyped and MEASURED to be unsound, which is why #2884
    // declined rather than exported stubs: with `class Off<T> _t: T; _s:
    // int;` the stub read and wrote `_s` at the placeholder offset,
    // returning 100/8 for `Off<string>` where 108/21 are correct
    // (a SILENT wrong value replacing a loud link error).
    std::string propertyBodyText;     // §14.1/#2893: property/indexer source for generics

    // §22.15 / §2.8.2: whether this member is declared pure. It is kept as
    // an explicit bit in addition to the general modifier list so consumers
    // need not infer behavioral metadata from spelling conventions.
    bool isPure = false;

    // §23 / §2.8.2/§2.8.3 rule 4 (fix #2400, additive within v7 — same rule as
    // #2392's outNullWhen above): for a Property (or Indexer) member, whether
    // it declares a setter. Needed because — unlike a field's "readonly"
    // modifier — a property's
    // setter-presence is NOT always recoverable from `modifiers`: an
    // expression-bodied property (`P: T => e;`) or a get-only block form is
    // getter-only by its SHAPE, not by a `readonly` modifier (writing
    // `readonly` on either is itself a compile error per §23.2 — "read-onlyness
    // is already expressed by the shape"), and the importer never reconstructs
    // an ast::PropertyDecl to recover that shape from (ModuleLoader.cpp's
    // Property case: "the getter BODY is not reconstructed here"). Without
    // this bit, an importer cannot distinguish a genuinely settable imported
    // property from a getter-only one, and unconditionally assuming "settable"
    // installs a dangling reference to a setter export symbol the exporter
    // never emitted (confirmed by probe: a getter-only base property
    // satisfying a read-write LOCAL interface obligation produced an itable
    // relocation to an undefined `...$propset` symbol — a link error). Ignored
    // for every kind but Property/Indexer. A v7-or-earlier blob has no 11th
    // scalar; the reader defaults this to `false` (degrades to "no
    // cross-module setter for this property" — the pre-#2400 behavior — rather
    // than risking an unresolved symbol against an old library that has not
    // been recompiled).
    bool hasSetter = false;

    // §13.6 / §23.3 / §2.8.2 clause 1 / §2.8.3 rule 4 (fix #2415, additive
    // within v7 — same shape as hasSetter above): for a Property (or
    // Indexer) member, the setter accessor's EFFECTIVE visibility. §13.6:
    // "A property or indexer's `get` or `set` may carry its own visibility,
    // which must not be *less* restrictive than the property's overall
    // visibility... An accessor without explicit visibility inherits the
    // property's overall visibility." A settable property's `hasSetter` bit
    // alone only recovers WHETHER a setter exists, not WHO may call it — a
    // `private set` / `protected set` / `internal set` on an imported
    // property was silently unenforceable at the cross-module assignment
    // site (no ast::PropertyDecl to read `pd->accessors[i].visibility` /
    // `pd->autoAccessorVis` from, and no side-table equivalent existed), so
    // `d.Name := 999` on a `private set` imported property compiled clean —
    // a visibility bypass. This field carries the ALREADY-RESOLVED effective
    // visibility (the accessor override when present, else the property's
    // own declared visibility — mirroring ExprTypecheck.cpp's local `sv`
    // computation exactly, so the exporter and the local same-module check
    // agree by construction). Ignored for every kind but Property/Indexer,
    // and meaningless when `hasSetter` is false (no setter to restrict). A
    // v7-or-earlier blob (predating this fix) has no 12th scalar; the reader
    // defaults this to `MetaVisibility::Private` — the SAFE direction is to
    // reject a cross-module write to a settable property of unknown setter
    // visibility rather than silently permit it (§2.8.2 clause 1: an
    // implementation that cannot carry a shape "must reject... it must not
    // alter the instantiation's semantics"), exactly mirroring why
    // hasSetter's own missing-scalar default is `false` rather than `true`.
    MetaVisibility setterVisibility = MetaVisibility::Public;

    // §9.2 / §22.8 (fix #2478, additive within v7 — same shape as hasSetter/
    // setterVisibility above): for a Property member, whether it is declared
    // in AUTO form (`property X: T := v;` / `property X: T;` — backing
    // storage, no user getter/setter body) as opposed to an expression-
    // bodied or block-accessor (computed) property. An auto-property is a
    // real INSTANCE STORAGE member — it occupies a struct slot exactly like
    // a field (§9.2's "auto-property PropertySymbol, backed by compiler-
    // generated storage like a field") — but the importer has no
    // ast::PropertyDecl to derive this from (ModuleLoader.cpp's Property
    // case never reconstructs one), and `sema::hasAstAutoPropertyDecl`/
    // `sema::hasAstAutoPropertyDecl` are DECL-DEPENDENT (`dynamic_cast<const
    // ast::PropertyDecl*>(ps->decl)`), so they always return `false` for an
    // imported PropertySymbol regardless of the exporter's real shape.
    // Confirmed as a live memory-safety hazard (not merely inferred):
    // `collectInstanceStorage`/`collectRecordStorage`
    // (LlvmBackend_impl.hpp) call the decl-dependent predicate to decide
    // whether a member occupies a layout slot, so an imported class/record
    // with a PUBLIC auto-property was under-counted — a locally-declared
    // derived class's own field then collided with the physical offset the
    // exporter's own compiled getter/setter/init functions use for a LATER
    // member, aliasing two logically-independent fields onto the same slot
    // (the same failure class #2456 fixed for a PRIVATE plain field, here
    // triggered by ANY-visibility auto-property instead). This bit closes
    // ONLY that storage-layout gap (`collectInstanceStorage`/
    // `collectRecordStorage`'s new `isAutoPropertyStorageSymbol` fallback,
    // LlvmBackend_impl.hpp) — the many OTHER call sites of
    // `hasAstAutoPropertyDecl` / `isAutoPropertyStorageSymbol` (definite assignment,
    // ownership/disposal, sendability, serialization eligibility, pattern
    // destructuring — audited during #2478's investigation, ~30 sites) are
    // NOT touched by this fix and remain separately filed follow-up work;
    // widening every consumer at once risked mixing at least three
    // different failure directions (silent wrong data, spurious rejects,
    // and a double-free-adjacent disposal miscount) into one unreviewed
    // change.
    //
    // Computed once by the exporter mirroring `sema::hasAstAutoPropertyDecl`'s
    // own predicate (Auto bodyKind, non-static). Since fix #2523
    // (§12.1/§12.5, v0.236, sibling of #2478), the bit is also set for
    // INDEXERS in Auto body form (e.g. `abstract property [i]: T;`, the
    // §12.5 abstract shorthand — get-only) — the wire bit's predicate is
    // the SEMANTIC "Auto shorthand shape" check, no longer the pure-storage
    // check #2478 originally framed it as. The importer-side
    // `ctx_.importedPropertyIsAutoStorage(base)` now correctly reports
    // "is the base in §12.5 shorthand form" for the abstract-indexer
    // override check (`checkPropertyOverrideSignatureMatch`,
    // TypeResolution_Checks.cpp ~2680). Storage consumers are UNAFFECTED
    // because `sema/Symbol.hpp`'s `isAutoPropertyStorageSymbol` helper still
    // filters `ps->isIndexer` explicitly before consulting this side table
    // (see `isAutoPropertyStorageSymbol` in sema/Symbol.hpp), so an imported
    // Auto-body indexer never appears in
    // `collectInstanceStorage`/`collectRecordStorage`. The
    // `sema::hasAstAutoPropertyDecl` predicate itself is deliberately LEFT
    // UNCHANGED (still excludes indexers) — its many call sites (definite
    // assignment, record equality, ARC, the codegen `hasAstAutoPropertyDecl`
    // consumers) treat the predicate as "real storage member", which an
    // indexer isn't; only this wire-bit repurposes it. A pre-#2478 blob has
    // no 13th scalar; the reader defaults this to `false` — the SAME
    // (already-broken, not newly-regressed) "not storage" / "not shorthand"
    // behavior an old dependency exhibits today, exactly mirroring
    // hasSetter's own missing-scalar default reasoning: a stale
    // (not-yet-recompiled) dependency keeps its pre-fix behavior until
    // recompiled, rather than the reader guessing.
    bool isAutoProperty = false;

    // §22.3 / §23 (fix #2461, additive within v7 — the getter-presence
    // TWIN of hasSetter above): for a Property (or Indexer) member, whether
    // it declares a getter. §22.3 "`ref` and `out` argument locations":
    // "for `ref`, the getter is called once to produce the callee's slot
    // value (for `out` no getter call occurs)" — so a write-only property
    // (hasSetter && !hasGetter) is a legal `out` target but NOT a legal
    // `ref` target (the copy-in has no getter to call). checkRefOutProperty
    // Operand (ExprTypecheck.cpp) enforces exactly that distinction for an
    // IMPORTED property — but it could not before this bit existed: an
    // imported PropertySymbol has no ast::PropertyDecl (ModuleLoader never
    // reconstructs one), so the getter-presence could not be recovered,
    // and a write-only imported property used as `ref` fell through sema
    // to a raw LINKER error (undefined `...$propget` symbol — the exporter
    // never emitted one), rather than a clean §23 rejection. The LOCAL
    // same-module shape was already correctly rejected, but by a SEPARATE
    // earlier check (ExprTypecheck_Calls.cpp's propertyDeclIsSetOnly gate
    // on the `d.W` member-access READ), which is also `pd`-gated and so
    // also does not cover an imported property. This bit is computed by
    // mirroring that propertyDeclIsSetOnly predicate's INVERSE exactly
    // (Expression/Auto bodyKinds always have a getter; a Block bodyKind
    // has a getter iff any accessor is a `get` arm), so the wire-carried
    // classification and the local-decl classification agree by
    // construction. Ignored for every kind but Property/Indexer.
    //
    // MISSING-SCALAR DEFAULT IS `true` — the OPPOSITE direction from
    // hasSetter (which defaults `false` to reject-conservative). The
    // asymmetry is deliberate and load-bearing: a false `hasSetter`
    // REJECTS a cross-module write (the safe direction — refuses rather
    // than risking an unresolved `...$propset` symbol); but a false
    // `hasGetter` would REJECT a cross-module `ref` READ, which would
    // FALSE-REJECT a legitimate read-write property from a stale
    // (pre-#2461, 13-scalar) dependency — a regression that breaks
    // working code. Defaulting `true` instead preserves the pre-#2461
    // behavior for a stale dependency exactly (a write-only property as
    // `ref` stays the pre-existing linker error — still rejected, just
    // confusingly; a read-write property as `ref` stays a clean accept),
    // and only a dependency RECOMPILED with #2461 gets the clean §23
    // diagnostic for its write-only properties. This is the same
    // "a stale dependency keeps its pre-fix behavior until recompiled"
    // model every other additive scalar in this schema follows
    // (§2.8.5 disclaims ABI stability across recompilation); it does NOT
    // violate §2.8.2 clause 1's "must reject" because no semantics are
    // silently altered — the stale write-only-as-ref was already rejected
    // (at link time), not silently accepted.
    bool hasGetter = true;

    // §24 / §2.8.2 (fix #2421, additive within v7 — same shape as
    // hasSetter/hasGetter/isAutoProperty above): for an Event member, the two
    // ast::EventDecl classification bits the codegen event-dispatch path needs
    // to mirror the exporter's struct/itable layout exactly. An imported
    // EventSymbol has NO ast::EventDecl (ModuleLoader.cpp's Event case
    // reconstructs the symbol without one — mirrors Property/Indexer's own
    // #503/#2414 limitation), and SymbolCollection.cpp's EventDecl case never
    // copies these onto EventSymbol::modifiers either (so even a LOCAL event
    // recovers them only via es->decl). Two separate codegen consumers
    // decl-null-gate on these bits and silently dropped an imported event
    // before this field existed (filed as #2421, found one layer past #2407's
    // symbol reconstruction):
    //   - collectInstanceStorage / emitOwnFieldInit / ctor-cleanup
    //     (LlvmBackend_impl.hpp / LlvmBackend_Setup.cpp) need `!isStatic` to
    //     give a DIRECTLY-typed imported event (or an imported-base event) its
    //     per-instance backing-field slot — without it fieldIndex_ gets no
    //     entry and the EventSource lives at no offset.
    //   - the ifaceEventIndex_ index/mirror AND the itab$C$I BUILD loops
    //     (LlvmBackend_Setup.cpp declareVtables / Pass D / record-box itable)
    //     need `!isAsync` to reserve a per-class EventSource-accessor itable
    //     slot for an IMPORTED interface's sync event (§24: only a sync event
    //     is an itable member; an async event is not). Both the INDEX lookup
    //     (eventSourcePtr) and the itable ARRAY build must classify identically
    //     or the importer reserves a slot the exporter's real itable layout
    //     does not have (or vice versa) — an itable/struct-layout MISMATCH
    //     between the two compilations, the exact hazard #2420/#2406 caught
    //     elsewhere. A wrong guess is therefore not just an imprecision; this
    //     wire bit is the only way the importer can recover the exporter's real
    //     classification. Ignored for every kind but Event. A pre-#2421 blob
    //     has no 15th/16th MEMBER_BLOCK scalar; the reader defaults BOTH to
    //     `false` — the classification a plain instance sync `event E: T;`
    //     (the common case) carries, so a stale not-yet-recompiled dependency
    //     keeps its already-broken pre-fix behavior (the imported event was
    //     silently no-op before #2421 regardless) rather than guessing
    //     `static`/`async` and risking the layout mismatch.
    bool isEventStatic = false;
    bool isEventAsync = false;

    // §9.2 / §9.3 / §2.8.3 rule 4 (fix #2516, additive within v7): for a Field
    // member, whether it is a §9.1 positional-record field (`record R(x: int)
    // ... end`'s `x`) rather than a body field (`record R(x: int) private _y:
    // int := 0; end`'s `_y`). The exporter distinguishes the two by
    // `fs->posParam != nullptr` on the local FieldSymbol (§9.1,
    // ModuleMetadataBuilder.cpp's convertMember positional branch) — that
    // distinction was NOT carried across the wire before this fix, so an
    // importer reconstructing a combined-form record's TypeSymbol created
    // FieldSymbols with `posParam == nullptr` for BOTH positional and body
    // fields and the §9.2 auto-equality classifier
    // (`isPositionalRecordEqualityBearing`) had to conservatively treat every
    // imported FieldSymbol as a positional field (false-accept of `=`/`<>` on
    // imported combined-form records whose exporter already rejects them,
    // #2516's finding). A pre-#2516 blob has no 17th scalar; this defaults to
    // `false` — the SAME "treat as positional" classification a stale
    // not-yet-recompiled dependency already exhibits today (its body fields
    // are silently considered positional, so the pre-fix false-accept holds
    // for the dependency until recompiled — §2.8.5 disclaims ABI stability
    // across recompilation). Ignored for every kind but Field.
    bool isPositional = false;

    // §22.10 / §9.8 / §2.8.3 rule 4 (fix #3221, additive within v7 — same
    // shape as isPositional above): for a §22.10 primary-constructor parameter
    // whose positional FieldSymbol has `posParamBodyRef == true` (the parameter
    // is referenced in a *body* position — method body, property accessor,
    // constructor body — §22.10's capture enumeration), the parameter is
    // captured into a synthetic private field of the same name. That synthetic
    // field IS §22.8 instance storage, so #2364's base-with-storage guard
    // (`Resolver::checkClassSerializationEligibilityLate`'s `classHasOwnStorage`
    // walk, fix #3182's `isInstanceFieldStorageSymbol`) must count it.
    //
    // It used to answer FALSE on every import: `ModuleLoader` synthesizes the
    // `posParam` (#2516, the positional vs body distinction above) but NOTHING
    // set `posParamBodyRef`, so an imported base whose only storage was a
    // captured positional parameter slipped past #2364's guard. MEASURED on
    // `226aca1`:
    //
    //   base = `class Base(v: int); public property V: int => v; end Base;`
    //   twin  -> REJECTED, #2364's diagnostic ("carries its own serialized
    //            storage")
    //   import-> COMPILED CLEAN, exited 9 — silent accept of exactly the
    //            shape #2364 exists to refuse
    //
    // The storage really is there — #2325's capture enumeration records it as
    // a private instance field — so this was a classification error over a
    // channel that did not cross. Carried as the 18th trailing scalar, exactly
    // as isPositional is the 17th: a pre-#3221 blob defaults to `false`,
    // which is the SAME "never captured" classification a stale dependency
    // already exhibits today (its captured primary-ctor parameters silently
    // counted as no storage, so the #2364 guard slipped on the same shape).
    // Ignored for every kind but Field.
    bool posParamBodyRef = false;

    // §22.15 / §2.8.2 (fix #3618, additive within v7 — same shape as
    // hasSetter/hasGetter/isAutoProperty above): for a Property (or Indexer)
    // member, whether its GETTER ACCESSOR declares `pure get` (§22.15 "Where
    // `pure` may appear" / "Purity attaches to the getter").
    //
    // WHY THIS BIT EXISTS. The PROPERTY-level `pure` marker rides
    // `ast::PropertyDecl::modifiers`, so the wire's `out.isPure` (line 1006
    // in ModuleMetadataBuilder.cpp, `hasModifier(m.modifiers, "pure")`)
    // carries it. The ACCESSOR-level `pure get` marker does NOT: it lives in
    // `ast::PropertyAccessor::isPure` (a sibling of `isSet`), never in a
    // modifier list. §22.15 FORCES `pure get` for a read-write block-form
    // property — `TypeResolution_Checks.cpp:11524` REJECTS property-level
    // `pure` on a `Block` bodyKind with a setter — so the accessor spelling
    // is not exotic; it is the shape every settable block-form property
    // carries.
    //
    // The PropertySymbol arm in convertMember never read the accessor list,
    // so the bit never crossed the wire. Downstream: `propertyGetterIsPure`
    // (sema/SymbolCollection.cpp, added by #3609) sees the accessor arm only
    // via a `dynamic_cast<const ast::PropertyDecl*>(ps->decl)` walk — null
    // for every imported PropertySymbol — so it answered `false` for every
    // imported `pure get`. The cross-module twin was a silent non-pure getter;
    // tightening §22.15 rule 6's accessor-read arm (#3630) would have
    // rejected a CHARACTER-IDENTICAL exporter-side getter. This bit closes
    // exactly that seam: the importer records the accessor-level purity
    // alongside the property-level one, and `propertyGetterIsPure` consults
    // it ahead of the AST walk so the imported case answers identically to
    // the local one.
    //
    // COMPUTED once by the exporter mirroring
    // `propertyGetterIsPure`'s accessor arm (any accessor with `isPure &&
    // !isSet`). Ignored for every kind but Property/Indexer; a pre-#3618 blob
    // has no 19th scalar; this defaults to `false` — the SAME classification
    // an old dependency already exhibits today (its `pure get` getter is a
    // silent non-pure getter from the importer's perspective, exactly the
    // silent-failure direction the bug measures). No schema bump: the bump
    // rule reserves a bump for a change that makes an OLD reader
    // misinterpret an EXISTING field; this only ADDS a trailing scalar the
    // pre-#3618 reader's `sc.size() < 18` gate never looks past.
    bool accessorIsPure = false;

    // fix #5209 (§23.3 "Explicit setter parameter modifier", v0.313, additive
    // within v7 — same shape as accessorIsPure above): for a Property (or
    // Indexer) member, whether its SETTER is declared `set(owned)`. A 20th
    // trailing scalar in MEMBER_BLOCK's REC_SCALARS; NO schema bump (a
    // pre-#5209 reader's `sc.size() < 19` gates never look past index 18; a
    // #5209 reader given an older blob reads the missing 20th as `false`,
    // i.e. a plain setter — the classification an old dependency already
    // exhibits). Needed because an imported PropertySymbol has no AST decl to
    // walk: the importer's caller-side "dispose a fresh owned temporary passed
    // to a plain setter" (§28.3 *Unbound owned temporaries*) must not fire for
    // a `set(owned)` setter, which takes the value over.
    bool setterTakesOwned = false;

    // fix #5868 (§23.4 / §1.6, v0.334): for an Indexer member, the optional
    // identifier after the `end` keyword. Emitted only when non-empty, so a
    // module whose indexers use no `end` label is byte-identical to before this
    // fix. Needed because `checkIndexerEndLabel` reads `blockEndLabel` from the
    // AST decl — null for an imported indexer — and would skip silently.
    // With this field, the check runs for imported indexers too.
    std::string blockEndLabel;

    // fix #3202 (§22.10 "primary constructors" / §22.5 "Optional parameters
    // and named arguments" / §2.8.3 rule 4; spec v0.254) — the §22.10
    // primary-constructor parameter's DEFAULT-VALUE EXPRESSION TEXT, for a
    // positional Field member whose `isPositional` is set. Empty when the
    // parameter carries no `:= Expr`, and for every non-positional Field
    // member (a record's positional field carries no defaults — §9.1 — and a
    // plain body field is not a parameter at all).
    //
    // Without this carriage an importer cannot tell that a primary parameter
    // is even OPTIONAL, so the §22.5 arity check in
    // `checkPositionalNewArgsFromSymbols` (the imported arm) rejects a
    // correct `new DefBox(1)` against `class DefBox(A: int, B: int := 9)`
    // with "required parameter `B` is not supplied" — `B` is NOT required,
    // it has a default. The local twin
    // (`storePrimaryCtorPositionalArgs`) has its own silent defect at the
    // same shape: it skips the entire store loop when
    // `ne.args.size() != posFields.size()`, so a `new DefBox(1)` leaves the
    // whole object calloc-zero and `Total()` reads 0 instead of 10. This
    // text is what lets the importer's runtime path
    // (`LlvmBackend_Exprs2.cpp` storePrimaryCtorPositionalArgs default arm)
    // materialize the default on demand.
    //
    // Carried as its OWN record (REC_POSPARAM_DEFAULT in ModuleMetadata.cpp)
    // rather than as a 19th trailing scalar or a third trailing REC_STRING,
    // for the same reason REC_SER_CLAUSE / REC_DISPOSE_VERDICT above are:
    // adding yet another trailing REC_STRING slot makes the reader's
    // `capIdx` ordering load-bearing in a way that any future addition
    // silently misparses. A coded record is self-describing, is
    // `skipRecord()`'d by every reader this format has had, and is emitted
    // ONLY when the parameter actually carries a default — so a module with
    // no defaulted primary parameter is byte-identical to before.
    //
    // RENDERED FORM mirrors MetaParam::defaultText / `fillParamDefaults`:
    // `renderConstExpr` is the same machinery, and its non-empty-vs-empty
    // distinction is the one the loader reads back. `default(T)` resolves
    // as `default(T)` text (the importer's runtime path materializes it as
    // the parameter's declared type's zero, #3213's precedent); an integer
    // literal renders as that literal; a const reference renders as its
    // qualified path (#3213). Empty → no default, and a pre-#3202 blob is
    // structurally indistinguishable from "no default", which is exactly
    // the pre-fix behaviour a stale dependency already exhibits — its
    // primary parameters stay required, and the same reject that surfaces
    // today stays. The cross-module fix is therefore SAFE in the
    // dependency-direction: a stale module's primary parameters keep their
    // already-broken reject, not a worse wrong value.
    std::string posParamDefaultText;

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

    // fix #3140 (§9.8 / §10, family F4 of umbrella #3152; schema v7 additive):
    // an enum constant's `serialized "<name>"` RENAME — the string §9.8 writes
    // on the wire for this constant, when it differs from the declared name.
    // Empty when the constant carries no `serialized` clause (the overwhelming
    // majority). ⚠ The EMITTED RECORD is conditional (ModuleMetadata.cpp:648
    // writes it only for a non-empty EnumConstant), but the SEMANTIC HASH fold
    // below is not — see `hashMember`, which now guards it to match. An earlier
    // revision of this comment claimed "the emitted bytes are unchanged for
    // every module that does not use the clause"; that was measured FALSE by
    // review — an unconditional `hashStr` hashes the length first, so an empty
    // string still perturbed the header hash, and the `.o` of a module with no
    // enum at all differed by 46 bytes from byte 17 onward (the VBR-encoded
    // hashLo/hashHi shifting everything after it).
    //
    // The constant's VALUE has crossed since #328; only the wire NAME had no
    // channel, and `emitEnumSerialize` / `emitEnumDeserialize` read it off the
    // `ast::EnumConstant` — which an imported enum does not have. Carried as
    // the SECOND trailing REC_STRING of an EnumConstant member, the same
    // additive slot Constructor/Method/Field already use for their body texts;
    // a reader predating this field stops at the first trailing string and
    // skips the rest of the block, so no schema bump is needed.
    std::string enumWireName;

    // fix #3140 (§9.8, family F2 of umbrella #3152; schema v7 additive): the
    // member's `serialized` clause — `skip`, `optional`, and the `"<name>"`
    // wire rename — for the two STORAGE-bearing sites the A.4 grammar admits it
    // on: a record's positional field (a Field member with `isPositional`) and
    // an auto-property (a Property member with `isAutoProperty`). The third
    // site, an EnumConstant, rides `enumWireName` above (family F4) and is
    // untouched here.
    //
    // Nothing about it crossed before: the clause lived on `ast::Param` /
    // `ast::PropertyDecl` and nowhere else, so the §9.8 emitters recovered it by
    // casting `Symbol::decl` — null for every imported member — and answered "no
    // clause" for all of them. "No clause" is NOT the safe default: it puts a
    // `serialized skip` component back on the wire while the header's field
    // count still excludes it, desynchronising the reader (the defect #1612
    // fixed on the local side), and it writes a renamed component under its
    // declared name, which a name-keyed reader then reports as a missing
    // required field.
    //
    // Carried under its OWN record code (REC_SER_CLAUSE) rather than as a
    // trailing REC_STRING or an 18th positional scalar. Both alternatives were
    // rejected for the reason ModuleMetadata.cpp's `emitTypeDecl` states at
    // length for #3139's 8th scalar: a positional entry can only be added
    // unconditionally (which perturbs every module's bytes AND its header hash)
    // or conditionally (which makes the NEXT author's addition a silent
    // misparse). A distinct code is self-describing, is `skipRecord()`'d by
    // every reader this format has had, and is emitted ONLY when the member
    // actually carries a clause — so a module using none is byte-identical.
    // The hash fold in `hashMember` is guarded by the SAME predicate as the
    // emission, which is the property #3140's own F4 follow-up (c97a49f) had to
    // be corrected for.
    //
    // ⚠ ABSENT FOR A PRE-#3140 BLOB, AND THAT IS NOT A BENIGN DEFAULT. An
    // earlier revision of this comment claimed the clause then "reads as `no
    // clause`, which is exactly the behaviour that dependency's own compiler
    // produced". MEASURED FALSE: that compiler produced a LOUD REJECT (#3139's
    // `serializationSurfaceIsEmittable` interlock), not "no clause". A
    // post-#3140 compiler reading a pre-#3140 artifact therefore puts a
    // `serialized skip` component BACK on the wire and reads it back, with no
    // diagnostic at any severity — a wrong VALUE where the old pair produced a
    // build failure.
    //
    // Left as-is rather than version-gated, deliberately and with the exposure
    // stated: stdlib §2.15 makes the binary format same-build-only, so a
    // dependency and its consumer are expected to be compiled by one compiler,
    // and EVERY additive field this format has ever gained shares the property
    // — including family F4's own `enumWireName` directly above. Gating this
    // one alone would imply the others are safe, which is the more misleading
    // artifact. Filed as the residue: see #3184.
    //
    // What must NOT survive here is the REASONING. "No clause is what the old
    // compiler did, so absence is safe" is precisely the negative-list
    // "not X ⇒ assume Z" shape this whole fix refutes; the correct statement is
    // that absence is a KNOWN-WRONG answer whose blast radius is bounded by the
    // format's same-build contract.
    bool serializedSkip = false;
    bool serializedOptional = false;
    std::string serializedWireName;   // empty => the DECLARED name

    // §9.2 "The `equality skip` clause" (v0.307; additive within v7,
    // follow-up to fix #4643): this storage member's `equality skip` clause.
    //
    // §9.2 states the obligation directly: "the artifact interface ([§2.8.2])
    // carries the clause exactly as it carries `serialized` clauses", and
    // "Adding or removing `equality skip` on a public type's member is a
    // breaking change ([§3.6] rule 3)" — so it is interface content the
    // semantic hash must track, not a local decoration. #4643 landed the
    // clause with no wire carriage at all; MEASURED consequence, on a record
    // exported by one module and used by another: `new Pt(1,10) = new
    // Pt(1,20)` (where `Cache` is `equality skip`) answered `True` in the
    // declaring module and `False` at the import, with the same split on
    // `GetHashCode`, and the body-record shape turned a locally-accepted `=`
    // into a compile error at the import.
    //
    // Emitted under its OWN record code (REC_EQ_CLAUSE) and only when TRUE, so
    // a module using no `equality skip` is byte-identical to a pre-#4643 build
    // — the REC_SER_CLAUSE / REC_POSPARAM_DEFAULT discipline, with `hashMember`
    // guarded by the SAME predicate as the emission (the half c97a49f had to
    // correct on the F4 slice). Absent (a pre-#4643 blob) reads as `false`,
    // i.e. "participates in equality": the pre-fix reading, so a stale
    // dependency degrades to exactly what it did before and no new silent
    // answer appears in the dependency direction.
    bool equalitySkip = false;

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
    // A.3 `Modifier` (v0.242): "abstract","closed","sealed","static".
    std::vector<std::string> modifiers;
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

    // §22.8 step 1 / §22.10 / §11.1 (#3911/#3916, additive within v7): the
    // source text of this class's OWN inheritance-clause argument LIST — the
    // `(args...)` after the first base type — for any class declaring one.
    // `MetaTypeDecl::baseTypes` already carries the base TYPE; this field
    // carries the argument EXPRESSIONS (`Mid<T>(mp: int): Base(mp)` →
    // `"mp"`). The exporter extracts each argument's source-text span (a
    // CallArgument's `value->span` is stamped by the parser, exactly as
    // #662c's `dtorBodyText` does for the destructor body) and joins them
    // with `", "`. The importer re-parses this in a fresh SemaContext with a
    // synthetic class header (the base type comes from `baseTypes[0]`, the
    // primary-ctor parameter list comes from `genericParams` + the field
    // stubs derived from `members`) so a generic imported MIDDLE ancestor's
    // OWN base clause can be evaluated by `emitImplicitBase` and
    // `prePopulateBasePositionalFields`, which both read
    // `primaryBaseCtorArgs` via `dynamic_cast<ast::TypeDecl*>(cls->decl)` —
    // and `cls->decl` is null for every imported type (#2374-class gap,
    // `TypeSymbol::decl` stays null on purpose — widening it is a separate,
    // much larger risk this fix declines to take). Carried as REC_BASE_CTOR_ARGS
    // under its own record code so a pre-#3916 reader `skipRecord()`s it and
    // the field defaults to empty (the pre-fix reading — the exporter emits
    // only when non-empty, so a class with no inheritance-clause arg list is
    // BYTE-IDENTICAL to a pre-#3916 build). See the `additive within v7`
    // precedent: #2950's `builtinExceptionBase` (#2950, REC_EXC_BASE),
    // #3144's `choiceCases` (CHOICE_CASE_BLOCK), #3191's `nonTrivialDispose`
    // (REC_DISPOSE_VERDICT), #3179's `serializedShape` (REC_SER_TYPE_SHAPE).
    std::string baseCtorArgsText;

    // §22.16 / §2.8.2 (#4592, additive within v7): this class's OWN
    // `invariant(E);` members, re-rendered as source text — one
    // `invariant(<expr source>);` per non-`static` invariant member, in
    // declaration order, newline-separated. Empty for every class declaring
    // none and for every non-class kind.
    //
    // WHY A SECOND TEXT CHANNEL BESIDE `invariants`. `MetaContractRecord::
    // canonicalText` (the `invariants` list above) is an IDENTITY artifact,
    // not an executable one: §2.8.2 defines it as the clause's token sequence
    // with "each reference to a parameter of the member … replaced by its
    // zero-based parameter position" and "all other names … recorded as
    // resolved at the declaration (module-qualified)". MEASURED on an
    // exported `invariant(v > 0);` over a field `v`, the record's canonical
    // text is `InvLibA.BaseA.v > 0` — a module-qualified member PATH that
    // does not re-parse into an expression evaluable against `this` in the
    // importer. §2.8.2 anticipates exactly this split: "An implementation may
    // additionally carry each clause's display text (original names) for
    // diagnostics; display text never participates in identity." This field
    // is that second channel, put to the use §22.16's runtime instrumentation
    // needs, and it is deliberately NOT read by any identity/agreement check.
    //
    // WHY OWN-ONLY (not the flattened chain `invariants` carries). The
    // importer re-parses this text in a synthetic re-declaration of THIS
    // class, whose member stubs are `members` — this class's own fields and
    // methods. An inherited clause names the BASE's storage, which that
    // synthetic class does not declare; each class carrying only its own text
    // lets `Emitter::emitInvariantCheck`'s existing base-first chain walk
    // compose the conjunction exactly as it does for a local hierarchy.
    //
    // Carried as REC_INVARIANT_TEXT under its own record code so a pre-#4592
    // reader `skipRecord()`s it and the field defaults to empty (the pre-fix
    // reading — an imported base's invariants silently unchecked). The
    // exporter emits the record only when non-empty, so a module whose
    // classes declare no invariant is BYTE-IDENTICAL to a pre-#4592 build,
    // and `hashTypeDecl`'s fold is conditional on the same predicate. Same
    // additive-within-v7 precedent as `baseCtorArgsText` (#3916),
    // `builtinExceptionBase` (#2950) and `serializedShape` (#3179).
    std::string ownInvariantText;

    // §12 / §31.4 / §2.8.2 (#2950, additive within v7): the name of the
    // BUILT-IN exception type this class names directly as its base
    // ("Exception", "IOException", …), empty when it names none.
    //
    // It cannot ride `baseTypes`: that list is built from
    // `sema::TypeSymbol::baseTypeRefs`, and a built-in exception has no user
    // TypeSymbol at all (its TypeRef carries `declaration == nullptr`), so it
    // is never pushed there on the exporter side either. Without this field the
    // importer had NO channel for the fact — and the fact is part of the
    // class's LAYOUT (§31.4's Message@8 / InnerException@16 / CauseChain@24 /
    // ParamName@32 header), so the importer laid the class out 32 bytes shorter
    // than the exporter and `calloc`'d to its own size while the exporter's
    // compiled constructor stored at the larger offsets. §2.8.2's carriage
    // requirement is explicit that an implementation "must not alter the
    // instantiation's semantics", and §2.8.3 rule 4 requires "one runtime type
    // identity — `is` tests, `catch` clauses, and §19 patterns over values of
    // the instantiation match identically regardless of which artifact produced
    // the value or performs the test".
    //
    // Carried as its own REC_EXC_BASE record inside TYPE_BLOCK rather than as a
    // second trailing REC_STRING, so it has no ordering relationship with
    // #662c's trailing `dtorBodyText` string and cannot be mistaken for it. NO
    // schema bump: the rule above kSchemaVersion reserves a bump for a change
    // that makes an OLD reader MISINTERPRET an existing field, and an unknown
    // record code is `skipRecord()`'d by every reader this format has ever had.
    // A module with no exception subclass produces byte-identical metadata to
    // before (the record is emitted only when non-empty).
    std::string builtinExceptionBase;

    // §9.8 / §18.5 / §21.4 / §14.2 / §2.8.2 (#3139, additive within v7): the
    // bases this type declares that name a COMPILER-PROVIDED interface with no
    // user TypeSymbol — `ISerializable`, `IDeserializable`, `IFormattable`,
    // `IEnumerable<T>` and the rest of the §4 collection family — as types, so
    // their generic args cross too.
    //
    // The same carriage argument as `builtinExceptionBase` above, for a
    // different fact: `baseTypes` is built from `sema::TypeSymbol::baseTypeRefs`,
    // and a built-in interface has no user TypeSymbol, so the exporter never
    // pushed it there either. Without this field the importer had NO channel
    // for the conformance, and every consumer recovered it by walking
    // `ast::TypeDecl::baseTypes` — which an imported type does not have.
    // MEASURED: an exported `record Pt(X: int, Y: int): ISerializable,
    // IDeserializable` got "no member `Serialize` on type `Pt`" in the importer
    // while the identical declaration round-tripped locally. §2.8.2 requires
    // the carriage "must not alter the instantiation's semantics"; a
    // conformance that exists in one artifact and not the other does exactly
    // that.
    //
    // Only the DIRECT bases are carried; an ancestor chain reconstructs itself
    // because each exported ancestor carries its own — same as
    // `builtinExceptionBase`.
    //
    // Carried as METATYPE_BLOCKs trailing the invariants, counted by an 8th
    // entry in TYPE_BLOCK's REC_SCALARS. NO schema bump: a reader predating
    // this field stops at `sc.size() >= 7`, ignores the extra scalar, and its
    // `finishBlock` skips the trailing subblocks — the same tolerance rule
    // stated above kSchemaVersion that #2950 relied on.
    //
    // BOTH the 8th scalar AND the `hashTypeDecl` fold are CONDITIONAL on the
    // list being non-empty, matching #2950's REC_EXC_BASE exactly, so a module
    // whose types declare no built-in interface produces byte-identical
    // metadata to before. That is not tidiness: the first revision of this
    // field emitted the scalar and folded a zero count unconditionally.
    // MEASURED as a one-variable A/B on the same tree — only these two guards
    // added — over a three-type module declaring no built-in interface
    // anywhere is BYTE-IDENTICAL to a pre-#3139 build (verified against 310571d,
    // c97a49f and c22e34d alike). The byte DELTA quoted by earlier revisions was
    // probe-specific and is deliberately not repeated here — see the note in
    // ModuleMetadata.cpp's hashTypeDecl for why a number that does not reproduce
    // is worse than no number.
    // The scalar widens REC_SCALARS for EVERY type in every module, and the
    // fold moves the VBR-encoded hash in the header, which shifts every byte
    // after it. A hash that moves when nothing was written is not a function
    // of the artifact's contents, and §2.8.3 rule 3's staleness check is
    // exactly the consumer that must not be fed that noise.
    //
    // What the tolerance claim rests on, verified rather than asserted: the
    // 4-way old/new reader x old/new artifact matrix over five shapes chosen
    // for offset-shift risk (a generic class with `IEnumerable<T>` plus a
    // destructor body, a class with `Exception, IFormattable` bases plus an
    // invariant, a nested type — i.e. the cases where these new subblocks land
    // BETWEEN the invariants and the pre-existing trailing REC_STRING /
    // REC_EXC_BASE). An old reader on a new artifact gives byte-for-byte
    // identical results and diagnostics; a new reader on an old artifact
    // degrades exactly to pre-fix.
    std::vector<MetaType> builtinInterfaceBases;

    // §35.1 / §35.3 / §35.4 / §2.8.2 / §2.8.3 rule 4 (fix #3144, family F9 of
    // umbrella #3152; additive within v7): the choice's CASES in declaration
    // order, with their payload fields. Empty for every non-choice kind.
    //
    // Until this field existed, `MetaTypeDeclKind::Choice` crossed and the case
    // list did not (the enum's own comment said so: "the case-list itself is not
    // yet serialized"). So an imported choice arrived carrying `kind == Choice`
    // and ZERO cases — a correct predicate fed an empty source, the third shape
    // of the 2026-08-06 taxonomy: `classifyChoiceCore` returned `nullopt`, the
    // §35.4 owning-payload disposal never ran, and `ModuleLoader` built no
    // `ChoiceCaseSymbol` at all, so `Outcome.Fetched` did not even resolve.
    //
    // MEASURED on 137f58f (two-module build, `bezelc -c lib.bzl -o lib.o` then
    // `bezelc --import lib.o app.bzl`) the failure is NOT the leak the triage
    // predicted — it is a silent ABI MISMATCH, the #2950 class:
    //
    //     exporter  %choice.Outcome = type { i32, { ptr }, { i8 }, { i32 } }
    //     importer  %choice.Outcome = type { i32 }
    //
    // A choice is a VALUE type returned by value, so the importer's 4-byte
    // reading put the return in a register while the exporter's compiled
    // function wrote through a hidden sret pointer that the importer never
    // passed — `AddressSanitizer: SEGV on unknown address 0x8 ... WRITE` inside
    // the dependency's own `MakeFetched`. §2.8.2's carriage requirement is
    // explicit that an implementation "must not alter the instantiation's
    // semantics"; two different layouts for one type do exactly that.
    //
    // Carried as trailing CHOICE_CASE_BLOCK subblocks of TYPE_BLOCK, one per
    // case, with NO count scalar — the subblocks are self-describing, which is
    // what ModuleMetadata.cpp's ⚠ block above `emitTypeDecl`'s 8th scalar asks
    // a new field to be ("a distinct record code rather than a positional
    // scalar (preferred)"): a 9th positional scalar would be read as #3139's
    // `nBuiltinIfaces` by any artifact whose type declares no built-in
    // interface, and the reader would then try to read that many
    // METATYPE_BLOCKs — a silent misparse. A reader predating this field
    // `SkipBlock`s the unknown subblock id.
    //
    // BOTH the emission AND the `hashTypeDecl` fold are conditional on the list
    // being non-empty (#3139/#2950's rule), so a module with no choice produces
    // BYTE-IDENTICAL metadata to before — verified as a one-variable A/B.
    std::vector<MetaChoiceCase> choiceCases;

    // §3.6 / §28.9 / §21.9 / §2.8.2 (fix #3191, family M2 of umbrella #3152;
    // additive within v7): the type's TRIVIAL-DISPOSE verdict, inverted — true
    // when this class is NOT trivial-dispose (§28.9: "no destructor, no `owned`
    // fields of non-trivial-dispose type, no owning-inline-array fields,
    // trivial-dispose base", §3.6's table). False for every non-class kind.
    //
    // §2.8.2 names this carriage as a REQUIREMENT, not an optimization: the
    // artifact interface must carry "the **derived type properties** (§3.6):
    // carried as computed verdicts for non-generic public types", and states
    // exactly why — "These properties are computed from structure the public
    // surface does not show — private fields, destructor presence — yet §3.6
    // rule 3 makes them part of the public contract, and rules dispatch on them
    // at the consumer: … trivial-dispose (§28.9) …. An interface that omitted
    // them would force the consumer to either reject imported types at those
    // rules or guess." This field is that carriage for trivial-dispose; the
    // consumer was guessing, and guessing "trivial".
    //
    // The DESTRUCTOR is the leg that structurally could not cross. A class's
    // destructor is deliberately NOT a `MetaMember` — `convertMember` skips it
    // ("§13.3: not part of the public surface — Dispose() is") — and there is no
    // `MetaMemberKind::Destructor` to hold it. So `common::isNonTrivialDisposeClass`,
    // which decides the property by scanning `sema::TypeSymbol::members` for a
    // `DestructorSymbol`, found none on ANY non-generic imported class and
    // answered "trivial-dispose" for all of them. (The `owned`-FIELD leg does
    // cross — `ModuleLoader` interns `FieldSymbol::modifiers` — so it was only
    // the destructor that was invisible.)
    //
    // MEASURED at cf84884 (two-module build, `bezelc -c lib.bzl -o lib.o` then
    // `bezelc --import lib.o app.bzl`), with NO `owned` marker anywhere in
    // either module and no generics: for a `public class Res` declaring a
    // destructor and a local `class Derived: Res`, the statement `new Derived(1);`
    // is a §21.9 HARD ERROR ("this expression produces an owned value that
    // requires disposal (a non-trivial-dispose class …)") in the
    // character-identical SINGLE-MODULE twin, and COMPILES SILENTLY across the
    // module boundary. §2.8.2's carriage requirement is explicit that an
    // implementation "must not alter the instantiation's semantics"; a hard
    // error that fires in one artifact and not the other does exactly that.
    //
    // The COMPUTED VERDICT crosses, not the raw destructor-presence bit, because
    // that is what §2.8.2's sentence above asks for and because it makes the
    // consumer's answer independent of which legs of §28.9's definition happen
    // to be reconstructible on the far side. The base-chain leg still composes
    // the ordinary way: a LOCAL class deriving from an imported base is decided
    // by the walk reaching that base's carried verdict, exactly as
    // `builtinExceptionBase` carries only the DIRECT base and lets the ancestor
    // chain reconstruct itself.
    //
    // Carried as its own REC_DISPOSE_VERDICT record inside TYPE_BLOCK — the
    // "distinct record code rather than a positional scalar (preferred)" that
    // ModuleMetadata.cpp's ⚠ block above `emitTypeDecl`'s 8th scalar asks a new
    // field to be. A 9th positional scalar would be read as #3139's
    // `nBuiltinIfaces` by any artifact whose type declares no built-in
    // interface, a silent misparse. NO schema bump: an unknown record code is
    // `skipRecord()`'d by every reader this format has ever had.
    //
    // BOTH the emission AND the `hashTypeDecl` fold are conditional on the
    // verdict being TRUE (#2950/#3139/#3144's rule), so a module containing only
    // trivial-dispose types produces BYTE-IDENTICAL metadata to before. A new
    // reader on an OLD artifact sees no record, leaves this false, and degrades
    // exactly to pre-fix behaviour — which is why `false` must mean "absent or
    // trivial" and never "known trivial".
    bool nonTrivialDispose = false;

    // §9.8 / §2.8.3 rule 4 (fix #3179, family F10 of umbrella #3152; additive
    // within v7): the class's §9.8 serialized CLASS SHAPE, as decided by
    // `Resolver::checkSerializationEligibility`'s pre-pass on the EXPORTER side
    // and read back across the wire by the IMPORTER. Either empty (every non-
    // class kind, and a class that does not declare ISerializable/
    // IDeserializable), or one of exactly the three values the A.3 grammar
    // admits: "tree" / "graph" / "cyclic". The shape decides which
    // wire-format pass the importer's §9.8 `Deserialize` synthesis takes, the
    // same way it does locally; emitting it carries the §9.8 surface across
    // the boundary the same way `baseTypes` carries the §12 one.
    //
    // Until this field existed, `MetaTypeDeclKind::Class` crossed and the §9.8
    // surface did not (the pre-fix record/enum/choice surface now crosses, but
    // the class half was the remaining gap #3179 records). An imported `class
    // C: ISerializable, IDeserializable` therefore had `serializedShape == ""`
    // on the importer side, and every reader that asked `serializedClassShape`
    // — at least 21 sites in `LlvmBackend_Serialization.cpp` listed at
    // `src/sema/Symbol.hpp` — answered "" and so declined the surface, with
    // the local twin compiling fine. The shape is exactly the information the
    // §9.8 `Deserialize`/polymorphic dispatch synthesis needs to decide which
    // wire path to emit, and without it the importer's `Deserialize` cannot be
    // re-synthesised.
    //
    // Carried as its own REC_SER_TYPE_SHAPE record under TYPE_BLOCK (codes
    // REC_SER_TYPE_SHAPE = 13, typed as the bytes of the shape string).
    // Emitted only when non-empty — for every non-class kind and for a class
    // that declares no ISerializable/IDeserializable conformance the shape is
    // empty, so a module with no class serialization produces BYTE-IDENTICAL
    // metadata to before (the same conditional-emission rule #2950 /
    // #3139 / #3144 / #3191 already use). The matching hashTypeDecl fold is
    // also conditional on the field being non-empty, with the same rationale —
    // a fold the writer did not write moves the VBR-encoded header hash.
    std::string serializedShape;

    // §9.8 tier 4 / §2.8.3 rule 4 (fix #3179, same family): the class's
    // `polymorphic(...)` registry entries in declaration order. Empty for
    // every type whose class did not declare a registry; the polymorphic
    // dispatch path's existence is a tier-4 opt-in (§9.8 tier 4 — "a base
    // class whose serialized components may, at run time, hold instances of
    // registered derived classes declares a `polymorphic` registry as a
    // member of its body"), so empty is the meaningful default.
    //
    // Each entry is the wire discriminator string and the resolved
    // registered concrete type. Until this field existed the registry did
    // not cross (`MetaTypeDecl::polyCases` is read on the local side from
    // `ast::TypeDecl::serializedClass->polyCases`); an imported polymorphic
    // base therefore arrived as a non-polymorphic class, and a component
    // whose static type was that base silently lost the `$type` field on
    // round-trip while the local twin emitted it. The discriminator and the
    // resolved type are the exact two facts the §9.8 wire form needs (the
    // discriminator string becomes the `$type` value, the resolved type
    // names which concrete class to dispatch to on read).
    //
    // Carried as trailing POLY_CASE_BLOCK subblocks under TYPE_BLOCK, one per
    // entry, with NO count scalar — the same self-describing subblock-id
    // pattern #3144 uses for `choiceCases` and the same reason: a 9th
    // positional scalar would be read as #3139's `nBuiltinIfaces` by every
    // type that declares no built-in interface (a silent misparse — see the
    // ⚠ block on emitTypeDecl's 8th scalar). Both the emission and the
    // hashTypeDecl fold are conditional on the list being non-empty, so a
    // module with no polymorphic base is BYTE-IDENTICAL to before.
    struct MetaPolyCase {
        std::string discriminator;
        MetaType type;
        bool operator==(const MetaPolyCase&) const;
    };
    std::vector<MetaPolyCase> serializedPolyCases;

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
struct MetaSourceSpan {
    std::string file;
    std::uint32_t beginLine = 0;
    std::uint32_t beginColumn = 0;
    std::uint32_t endLine = 0;
    std::uint32_t endColumn = 0;

    bool operator==(const MetaSourceSpan&) const = default;
};

struct MetaExtension {
    MetaType target;                  // §26.1 the extended type
    MetaVisibility visibility = MetaVisibility::Public;
    std::vector<MetaMember> members;  // §26.3 methods (this increment)
    // §26.5 rule 4 (#2329): diagnostic identity for a nameless block. The
    // constraints use their source-like `where T: ...` spelling because the
    // importer needs to reconstruct the AST list, not resolve a second API.
    MetaSourceSpan sourceSpan;
    std::vector<std::string> genericConstraints;

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

// §2.5 / §2.8.2 (issue #3198, v0.254): the producer's content-revision
// stamp, read from the blob's header record without fully decoding the body.
// std::nullopt on bad magic or a truncated header. The loader uses this to
// reject a dependency whose producer stamp differs from
// `kProducerContentRevision` — a stale artifact whose content does not match
// what this compiler produces.
std::optional<std::uint32_t> readProducerContentRevision(
    const std::vector<std::uint8_t>& bytes);

// Test-only: serialize with a CUSTOM producer content-revision stamp. Used
// by the #3198 regression tests to forge a "stale" artifact (stamp !=
// kProducerContentRevision) and verify the loader's rejection path. The
// production writer (serialize / serializeWithUnknownExtras) always stamps
// kProducerContentRevision — there is no other call site.
std::vector<std::uint8_t> serializeWithProducerRevision(const ModuleMetadata& md,
                                                       std::uint32_t stamp);

} // namespace bezel::module
