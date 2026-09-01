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
#include <functional>  // #3145: anyRecordStorageField's predicate
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ast/Node.hpp"
#include "support/SourceLocation.hpp"

// fix #3169: `sema::recordStorageMembers` takes an optional
// `const common::RecordSubstEnv*` default-arg (#3171 generic-base
// substitution). Forward-declared in the OUTER `bezel::` namespace so the
// function's signature — `const common::RecordSubstEnv*` — resolves to the
// real type. Full definition lives in bezel::common::RecordSubstEnv
// (common/OwnershipFacts.hpp), included only by the .cpp.
namespace bezel::common { struct RecordSubstEnv; }

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

// §22.16 / §2.8.2 (v0.204, #2054): imported behavioral facts are kept on
// symbols as an implementation-neutral side table. The module metadata layer
// converts its wire enums to these stable values; no AST is required on the
// importing side.
enum class ContractKind : std::uint8_t {
    Requires = 0,
    Ensures = 1,
    Invariant = 2,
};

enum class ContractTier : std::uint8_t {
    Proven = 0,
    Checked = 1,
    Assumed = 2,
};

struct ContractRecord {
    ContractKind kind = ContractKind::Requires;
    bool isStatic = false;
    std::string canonicalText;
    ContractTier tier = ContractTier::Checked;
};

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
// §9.8 tier 4 (#3140, family F2): one entry of a class's `polymorphic(...)`
// registry — the wire discriminator and the concrete type it selects. Declared
// ahead of `TypeSymbol` because the symbol carries the resolved list.
struct TypeSymbol;
struct SerializedPolyCase {
    std::string_view discriminator;
    TypeSymbol* type = nullptr;
};

struct TypeSymbol : Symbol {
    ast::TypeDeclKind kind = ast::TypeDeclKind::Class;
    ast::Visibility visibility = ast::Visibility::None;
    // A.3 `Modifier` (v0.242): "abstract", "closed", "sealed", "static".
    // Read through the predicates below, not by hand — see their comment.
    std::vector<std::string_view> modifiers;
    std::vector<Symbol*> genericParams;      // GenericParamSymbol*
    std::vector<Symbol*> members;            // Field/Method/Property/...
    // §22.16 / #2054: invariant records carried by an imported class, in
    // base-first order. Local classes derive these from their AST; imported
    // classes have no AST, so the loader publishes this side-table vector.
    std::vector<ContractRecord> invariantContracts;
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
    // still pass the §9.2 auto-hashable check in
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
    // #1488 / §2.1 / §2.8.3 rule 3: the declaring module's full dotted name
    // (`module BankingApp.Transactions;` -> "BankingApp.Transactions"), the
    // module-identity component of this type's program-wide identity ("a type
    // is identified program-wide by its declaring module name, its qualified
    // type name ..., and its generic arity"). Populated at symbol-creation
    // time for BOTH local types (SymbolCollection::collectTypeDecl, from the
    // enclosing ModuleDecl) and imported types (ModuleLoader, from the
    // dependency's ModuleMetadata::moduleName) so the two sides always agree.
    // Codegen (Emitter::mangledTypeName) reads this to qualify every emitted
    // symbol name by module, so two modules exporting a same-named public
    // type no longer collide at link time (`multiple definition of
    // bz$Tx$Hello`) — the bug #1488 describes. Empty only for a synthetic
    // TypeSymbol with no real module (builtins materialized on demand,
    // ts->decl == nullptr); those are never part of the cross-module ABI.
    std::string moduleName;
    // fix #3529 / §11.6 / §2.8.3 rule 3: the dotted path of this type's
    // ENCLOSING types, outermost-first, EXCLUDING the type's own name
    // ("Outer1" for `Outer1.Ord`; "A.B" for `A.B.C`). Empty for a top-level
    // type — which is every type in the pre-#3529 world, so the mangling of a
    // non-nested type is byte-identical to what it was.
    //
    // The other half of §2.8.3 rule 3's identity that `moduleName` above only
    // half-carried: rule 3 identifies a type program-wide by "its declaring
    // module name, its qualified type name (including any enclosing-type path,
    // §11.6), and its generic arity". #1488 gave codegen the module half; this
    // is the enclosing-type half. Without it `Outer1.Ord` and `Outer2.Ord` in
    // one module both mangle to "M.Ord" and share ONE `bz$classcmp$M.Ord`
    // comparator thunk — measured, the second type sorts by the first's
    // `CompareTo` (#3529) — while their `$ctor`/`$Dispose`/`$init`/`td$`/`vt$`
    // symbols are LLVM auto-uniquified with `.1`/`.2` suffixes, names that
    // exist in no other translation unit (the failure #1488's own comment in
    // LlvmBackend_impl.hpp describes).
    //
    // Populated at symbol-creation time on BOTH sides, like `moduleName`:
    // SymbolCollection::collectTypeDecl threads it down the nesting chain for a
    // LOCAL declaration, ModuleLoader::reconstructTypeSymbol takes it from the
    // enclosing reconstructed symbol for an IMPORTED one — so the exporter's
    // definition and every importer's external declaration agree.
    //
    // The separator here is the §11.6 SURFACE one ('.'), because this is the
    // path as the language spells it. `Emitter::mangledTypeName` deliberately
    // re-spells it with '+' for the LINKER symbol: a module name is itself
    // dotted, so a dotted mangling cannot tell `module M.Outer1;` + `class Ord`
    // apart from `module M;` + `class Outer1` + nested `class Ord` (measured:
    // six `multiple definition` link errors). Read the surface form through
    // `qualifiedTypeName()` below, never by hand.
    std::string enclosingTypePath;

    // §11.6 / §2.8.3 rule 3: this type's QUALIFIED type name — the
    // enclosing-type path joined to the simple name ("Outer1.Ord"), or just the
    // simple name for a top-level type. Module-independent: the declaring
    // module is the separate `moduleName` component of rule 3's identity, and
    // the two are joined only where a whole identity is wanted (codegen's
    // `Emitter::mangledTypeName`).
    std::string qualifiedTypeName() const {
        if (enclosingTypePath.empty()) return std::string(name);
        return enclosingTypePath + "." + std::string(name);
    }

    // §12 / §31.4 (#2950): the name of the BUILT-IN exception type this class
    // names DIRECTLY as its base ("Exception", "IOException", …), or empty when
    // it names none. A built-in exception has no user TypeSymbol at all (its
    // TypeRef carries `declaration == nullptr`), so it can never appear in
    // `baseTypes`/`baseTypeRefs` — this field is the only place the
    // relationship is recorded on the symbol, and it is what makes the fact
    // available where no AST exists.
    //
    // Why it must live on the symbol rather than be re-derived from the AST at
    // each use site: the §31.4 exception header (Message@8, InnerException@16,
    // CauseChain@24, ParamName@32) is part of the class's LAYOUT. Every
    // consumer that walked `ast::TypeDecl::baseTypes` instead saw nothing for
    // an IMPORTED class — an imported symbol has no AST — so the importer laid
    // the very same class out 32 bytes shorter than the exporter did and
    // `calloc`'d to its own size, while the exporter's own compiled constructor
    // stored through the 48-byte offsets (#2950, measured: exporter
    // `{ptr,ptr,ptr,ptr,ptr,i8,i32}` vs importer `{ptr,i8,i32}` for one
    // `public class MyEx: Exception` with a single `int` field). §2.8.3 rule 4
    // requires "one runtime type identity — `is` tests, `catch` clauses, and
    // §19 patterns over values of the instantiation match identically
    // regardless of which artifact produced the value or performs the test",
    // so the two sides must agree: ModuleMetadata carries this
    // field and ModuleLoader restores it. Populated for LOCAL types by Type
    // Resolution and for IMPORTED types by ModuleLoader. Read it through
    // `builtinExceptionBaseOf` below, never directly, so the ancestor walk
    // stays in one place.
    std::string builtinExceptionBase;
    // §3.6 / §28.9 / §21.9 / §2.8.2 (fix #3191, family M2 of umbrella #3152):
    // the §3.6 trivial-dispose verdict for an IMPORTED type, INVERTED — true
    // when the exporter computed this class to be NON-trivial-dispose.
    //
    // Set ONLY by `ModuleLoader`, from `MetaTypeDecl::nonTrivialDispose`. A LOCAL
    // type leaves it false and is classified by the ordinary structural walk in
    // `common::isNonTrivialDisposeClass` — which is why this is not a general
    // "is this class non-trivial-dispose" cache and must never be read as one.
    // Read it only through that walk, which consults it PER LEVEL so a local
    // class deriving from an imported base is decided correctly.
    //
    // Why the verdict needs a channel at all: a class's destructor is not part
    // of its exported member list (§13.3 — `Dispose()` is the public surface, and
    // there is no `MetaMemberKind::Destructor`), so the structural walk's
    // `DestructorSymbol` scan found nothing on any non-generic imported class and
    // answered "trivial-dispose" for all of them. §2.8.2 requires the artifact
    // interface to carry the §3.6 derived properties as computed verdicts for
    // exactly this reason: they are "computed from structure the public surface
    // does not show — private fields, destructor presence".
    //
    // FALSE IS NOT "KNOWN TRIVIAL" — it is "absent or trivial". An artifact
    // written before this field carries no verdict, so a false here must fall
    // through to the structural walk rather than short-circuit it.
    bool importedNonTrivialDispose = false;
    // §9.8 / §18.5 / §21.4 / §14.2 (#3139, family F1 of #3152): the resolved
    // TypeRefs of this type's declared bases that carry NO user `TypeSymbol` —
    // exactly the complement, within the declared base list, of what
    // `baseTypes`/`baseTypeRefs` can hold. In practice that is the
    // compiler-provided interfaces (`ISerializable`, `IDeserializable`,
    // `IFormattable`, `IEnumerable<T>` and the rest of the §4 collection
    // family), plus an enum's §10.3 primitive underlying type, plus — locally,
    // on an already-erroring program — a base whose name did not resolve.
    // Generic args are preserved, so a walk can continue through the built-in
    // hierarchy from here.
    //
    // Why a second list rather than an entry in `baseTypes`/`baseTypeRefs`:
    // those two are documented as PARALLEL, and a base with no TypeSymbol has
    // nothing to put in `baseTypes`. `TypeResolution_Names`'s
    // `if (ref->declaration)` guard therefore drops such a base from BOTH — on
    // the LOCAL side too — and `MetaTypeDecl::baseTypes` is built from
    // `baseTypeRefs`, so the fact never reached the wire either. Every consumer
    // recovered it by walking `ast::TypeDecl::baseTypes` instead, which answers
    // "declares none" for every IMPORTED type (an imported symbol has no AST at
    // all). MEASURED on `main` with a two-module build (`bezelc -c lib.bzl` then
    // `--import lib.o app.bzl`): an exported
    // `public record Pt(X: int, Y: int): ISerializable, IDeserializable` got
    // "no member `Serialize` on type `Pt`" in the importer while the identical
    // declaration compiled and round-tripped to 34 locally; an imported
    // `: IFormattable` class got the §18.5 "a format spec requires … one
    // implementing `IFormattable`" reject; and a LOCAL record with an imported
    // serializable record component got the §9.8 "not serialization-eligible"
    // reject.
    //
    // The membership rule is STRUCTURAL ("the base resolved to something with
    // no user declaration"), not a hand-listed set of built-in names — the
    // positive-list shape that has drifted here before (see the name-set
    // discussion on `builtinExceptionBaseOf`). Keeping the non-Named entries
    // rather than filtering down to interfaces is deliberate: two closure walks
    // (`namedAncestorChain`, `userTypeImplementsBuiltinIface`) read the raw AST
    // base list today and treat a non-Named entry as "closure incomplete — do
    // not conclude absence", and that conservative branch has to survive the
    // conversion unchanged.
    //
    // ⚠ ONE ASYMMETRY THAT CREATES, stated because it is real and bounded:
    // only the `Named` entries cross the wire (ModuleMetadataBuilder filters
    // them — an enum's underlying width rides `enumUnderlyingType`, and a
    // module with an unresolved base never gets as far as emitting metadata).
    // So a LOCAL enum's `: long` is in this list and an IMPORTED enum's is not,
    // and the two conservative branches above therefore read a local enum as
    // "closure incomplete" and an imported one as complete. Bounded, and never
    // in the permissive direction for the imported side: both callers are
    // gated on a builtin BORROWING COLLECTION target
    // (`builtinBorrowingCollectionFullyModeled`) or on §31.2.1 catch-arm
    // binding, and an enum is neither, so the difference is unreachable today;
    // where it were reachable it would make the IMPORTED answer the sharper of
    // the two. Recorded rather than equalised because carrying an enum's
    // primitive base a second time, purely to preserve a conservative branch no
    // caller can observe, would be the more confusing artifact.
    //
    // The synthesized-symbol built-in interfaces (`IComparable`, `IEquatable`,
    // … — SemaContext::isSynthesizedBuiltinInterfaceName) DO get a TypeSymbol
    // and so ride `baseTypes` on both sides already; `declaresBuiltinInterface`
    // below consults both places so a caller never has to know which.
    //
    // Read it through the accessors below, never directly.
    std::vector<TypeRef*> nonSymbolBaseRefs;
    // §9.8 (#3140, family F2 of umbrella #3152): the derived §9.8 wire shape of
    // this class's synthesized serialization surface, and its tier-4
    // `polymorphic(...)` registry. Written once, by
    // `Resolver::checkSerializationEligibility`'s synthesis pre-pass; read only
    // through `serializedClassShape` / `serializedPolyCasesOf` below, which
    // carry the note on what an imported class answers and why. Empty for every
    // non-class kind.
    std::string serializedShape;
    std::vector<SerializedPolyCase> serializedPolyCases;
    // #2776 / #3139: true for the synthesized `TypeSymbol`s minted by
    // `SemaContext::builtinInterfaceSym` for the closed set of built-in
    // interfaces that DO get a real declaration. `ModuleLoader` resolves an
    // imported occurrence to the very same cached symbol (#2776), so the flag
    // reads the same on both sides. It is what lets
    // `declaresBuiltinInterface` tell `: IComparable<T>` apart from a USER
    // type that happens to be named `IComparable` — the name-only walks it
    // replaces had no such guard (the same distinction #2950 records for
    // `Exception`).
    bool isSynthesizedBuiltinInterface = false;
};

// §11.7 / §12.5 / §19.6 (v0.242, #3052): the class-shape modifier predicates,
// stated once so the whole `abstract`-keyed rule family reaches `closed`.
//
// `closed` is the dedicated spelling of what through v0.241 was the modifier
// PAIR `sealed abstract` (§11.7, verbatim: "A class declared with the
// **`closed`** modifier is a **closed abstract class**: a class that cannot be
// instantiated directly and cannot be inherited from outside the declaring
// module. … A `closed` class may declare abstract members (§12.5) without
// carrying the `abstract` modifier — `closed` subsumes it"). Three distinct
// questions were being answered by reading the raw modifier list, and they do
// NOT have the same answer for a `closed` class:
//
//   * "has no instances / cannot be `new`ed / need not implement its abstract
//     members / is not a §19.6 leaf" → `abstract` OR `closed`
//     (§12.5: "`new` on an abstract or `closed` class is a compile error";
//      §19.6: "A **leaf type** is every **concrete** (neither `abstract` nor
//      `closed`) class in the closed set").
//   * "admits no subclass anywhere" → plain `sealed` only. A `closed` class is
//     inheritable WITHIN its declaring module (§11.7), exactly as the pair was.
//   * "roots a hierarchy closed across module boundaries" → `closed` only.
inline bool typeSymbolHasModifier(const TypeSymbol* t, std::string_view m) {
    if (!t) return false;
    for (std::string_view mod : t->modifiers)
        if (mod == m) return true;
    return false;
}

// §11.7: does `t` carry the `closed` modifier?
inline bool typeIsClosed(const TypeSymbol* t) {
    return typeSymbolHasModifier(t, "closed");
}

// §12.5 / §19.6: is `t` instance-less — `abstract` or `closed`? This is the
// predicate every former bare-`abstract` class-level test wants.
inline bool typeIsAbstractOrClosed(const TypeSymbol* t) {
    return typeSymbolHasModifier(t, "abstract") || typeIsClosed(t);
}

// §12 / §31.4 (#595 / #2950): the built-in exception type `t` inherits — its
// own `builtinExceptionBase` when it names one directly, else the first one
// found walking its class-ancestor chain — or "" when it inherits none.
//
// The ancestor walk crosses module boundaries in BOTH directions: a class whose
// base is an IMPORTED exception subclass carries the same header as one whose
// base is local, because the §31.4 slots sit at fixed byte offsets that every
// artifact's compiled member access uses. That is why the walk must not require
// an ancestor to have an `ast::TypeDecl` (an imported one never does), and why
// `builtinExceptionBase` is consulted at every level rather than only the first.
std::string_view builtinExceptionBaseOf(const TypeSymbol* t);

// §9.8 / §18.5 / §21.4 / §14.2 (#3139): the built-in-interface conformance
// accessors — the ONE place the question "does this type declare built-in
// interface `name`?" is answered, for a local and an imported symbol alike.
// See `TypeSymbol::nonSymbolBaseRefs` above for why the fact needs its own
// carriage and what a wrong answer measured as.
//
// They consult BOTH sources rather than switching on which one exists (the
// audit's rule): `nonSymbolBaseRefs` for the built-in interfaces that have no
// TypeSymbol, and the `isSynthesizedBuiltinInterface`-flagged entries of
// `baseTypes` for the ones that do. Neither requires an `ast::TypeDecl` at any
// level, because an imported symbol never has one.
//
//   * `declaresBuiltinInterface` — does `t` name it among its OWN bases?
//   * `inheritsBuiltinInterface` — does `t`, or any type in its user-base
//     closure, name it? Cycle-guarded (an inheritance cycle can survive error
//     recovery into these walks).
//
// The empty/null cases answer false.
//
// ⚠ THE ANSWER IS BY IDENTITY, NOT BY NAME — and that is a deliberate LOCAL
// behaviour change, not only a cross-module fix. §2.3 permits a user
// declaration to shadow an auto-imported library name ("the auto-imported
// library types … may be shadowed by local declarations under the normal
// name-resolution rules"), and the AST base-list walks these accessors replace
// matched on the simple name, so a class implementing a SHADOW was admitted
// wherever the real built-in would be. §18.5 is explicit that the opt-in is
// "the auto-imported interface `IFormattable`", so admitting a shadow was an
// over-accept: measured, a `class Fake: IFormattable` over a locally declared
// `interface IFormattable` returned 77 from a `$"{f:X}"` hole, and so did a
// shadow declaring only `Nope()` — with no `ToString(format)` for §18.5's
// dispatch to call at all. Both now report the §18.5 error. Pinned in
// tests/lit/builtin_iface_shadow_reject_3139.bzl and in the two
// `local_shadowed_iformattable_*` arms of
// tests/module/xmod_builtin_iface_conformance_3139_test.cpp (which also pin
// that the shadow stays USABLE — §2.3 permits it to exist; only §18.5's
// admission is denied).
//
// The two built-in families reach that identity by DIFFERENT routes, and both
// are load-bearing:
//
//   * the synthesized-symbol family (IComparable, IEquatable, … — they DO get
//     a TypeSymbol) is separated from a same-named user type by the
//     `isSynthesizedBuiltinInterface` flag, checked instead of the name;
//   * the non-symbol family (ISerializable, IFormattable, IEnumerable<T>, …)
//     is separated structurally: a user-declared base HAS a declaration, so
//     Type Resolution puts it in `baseTypes`/`baseTypeRefs` and it can never
//     land in `nonSymbolBaseRefs`, which is the only place this family lives.
//
// Same rule, two mechanisms — the same distinction #2950 records for a user
// class named `Exception`.
bool declaresBuiltinInterface(const TypeSymbol* t, std::string_view name);
bool inheritsBuiltinInterface(const TypeSymbol* t, std::string_view name);

// The declared bases of `t` that carry no user `TypeSymbol`, for the two
// closure walks that must continue THROUGH the built-in hierarchy and so need
// the resolved TypeRef (generic args included), not just a head name:
// `namedAncestorChain` and `userTypeImplementsBuiltinIface`. Both already visit
// `baseTypeRefs` for the declaration-bearing bases; this supplies exactly the
// ones the parallel-list guard drops, so the two together are the full declared
// base list on either side of a module boundary — which is what the AST base
// walk they replace was reaching for. Entries are never null but need not be
// `Named`; both callers already treat a non-`Named` entry as "closure
// incomplete", and that branch is why they are kept (see the field comment).
const std::vector<TypeRef*>& nonSymbolBaseRefsOf(const TypeSymbol* t);


// §31.4 (v0.240, #2971/#2983/#2984): WHICH member of the §31.4 exception
// surface a name denotes, for a class whose built-in exception base is
// `builtinBase`. §31.4 declares that surface verbatim:
//
//     public class Exception
//       public readonly property Message: string;
//       public readonly property StackTrace: string;
//       public readonly property CauseChain: Array<ExceptionInfo>;
//     end Exception;
//     public class ArgumentException: Exception
//       public readonly property ParamName: string?;
//     end ArgumentException;
//
// so the answer is a function of WHERE `builtinBase` sits in the built-in
// hierarchy, not of a list of type names: the walk runs
// `common::builtinExceptionParentOf` from `builtinBase` to the root and reports
// the first level that DECLARES the name. That is why `ParamName` is available
// on a subclass of ArgumentNullException and not on one of FormatException
// without either being named anywhere.
//
// The three consumers this replaces each keyed on something narrower and each
// got a different subset wrong: member lookup on a subclass's own static type
// tested `mm == "Message"` and so rejected the other three (#2971); `base.X`
// scanned `TypeSymbol::baseTypes`, which never holds a built-in, and so
// rejected all four while claiming the class had no class base (#2983); and
// §12.2's hiding check walked that same collection and so blocked none of them
// (#2984). One helper, one place to state §31.4's surface.
//
// DELIBERATELY EXCLUDED, because the compiler does not implement a reader for
// them and admitting a member sema cannot lower is worse than rejecting it
// (#2972's shape — sema accepts, codegen degrades to a null, the caller
// faults): `TypeInitializationException.InnerException` (§31.4; implemented
// only for a receiver statically typed as that built-in),
// `SerializationException.{Offset,Line,Column}` (stdlib §2.15) and
// `DomainFaultedException.FaultStackTrace` (stdlib §6.8) — the last four are
// declared-but-never-implemented at any layer, tracked by #2337/#2408.
enum class BuiltinExcMember {
    None,
    Message,     // Exception.Message      : string
    StackTrace,  // Exception.StackTrace   : string
    CauseChain,  // Exception.CauseChain   : Array<ExceptionInfo>
    ParamName,   // ArgumentException.ParamName : string?
};

// `owner`, when non-null, receives the name of the built-in that DECLARES the
// member — which is not always `builtinBase`. §31.4 declares `StackTrace` on
// `Exception` and `ParamName` on `ArgumentException`, so a class whose base is
// `ArgumentOutOfRangeException` inherits the two from different levels. A
// diagnostic that named `builtinBase` for both would state something false
// about the user's own hierarchy — the #2983 defect in miniature — so the walk
// that FINDS the member is also the thing that reports where it came from.
// Left untouched when the answer is None.
BuiltinExcMember builtinExceptionSurfaceMemberOf(std::string_view builtinBase,
                                                 std::string_view member,
                                                 std::string_view* owner = nullptr);

// Convenience over the pair above: the §31.4 surface member `member` names on
// `t`, or None when `t` derives from no built-in exception at all.
BuiltinExcMember builtinExceptionSurfaceMember(const TypeSymbol* t,
                                               std::string_view member);

// The §31.4 surface of `builtinBase`, as a comma-separated list for a
// diagnostic ("Message, StackTrace, CauseChain" / "…, ParamName"). Derived from
// the same walk, so a diagnostic can never name a surface the resolver does not
// actually accept.
std::string builtinExceptionSurfaceList(std::string_view builtinBase);

// A.2 TypeAliasDecl — its target type is resolved by Type Resolution.
struct TypeAliasSymbol : Symbol {
    std::vector<Symbol*> genericParams;
};

// §14.2 (v0.252, fix #3146 — family F13 of umbrella #3152): ONE `where T: …`
// constraint item, in the form BOTH sources can produce.
//
// Unlike the other families in that umbrella the data was never missing: the
// artifact has carried `MetaGenericParam::constraintTypes` /
// `specialConstraints` since #1579/v0.182. `ModuleLoader::convertGenericParams`
// read them off the wire and threw them away, because `GenericParamSymbol` had
// nowhere to put them — so `ast::TypeDecl::genericConstraints` was the only
// source, and it is null for every imported type.
struct GenericParamConstraint {
    // The `ast::ConstraintItem` kinds — "class" / "value" / "notnull" / "new" /
    // "type" — plus the §14.2 derived-property spellings ("sendable",
    // "hashable"). The historical `where T is owned` / `is not owned` modifier
    // form (§14.4, removed in v0.97) carries no instantiation-time obligation
    // and is NOT recorded here: the readers skipped a clause whose
    // `modifierConstraint` was non-empty, and the metadata builder folds that
    // spelling into `specialConstraints`, so both populators filter it out.
    std::string_view kind;
    // kind == "type": the bound. A LOCAL parameter carries the unresolved AST
    // node — resolved on demand in the declaring type's scope, exactly as
    // before, because an instantiation site may be visited before the
    // declaring type's own constraint clause. An IMPORTED parameter carries
    // the TypeRef the loader already built from the artifact's MetaType. At
    // most one is non-null.
    const ast::Type* astBound = nullptr;
    TypeRef* importedBound = nullptr;
};

// A.3 GenericParam (`Variance? Identifier`). Lives in the parent type or
// method's scope and is looked up like a local name.
struct GenericParamSymbol : Symbol {
    std::string_view variance; // "in", "out", or empty
    // §14.2 (#3146): this parameter's own constraints, in declaration order.
    // Populated by SymbolCollection::collectGenericParams from the declaring
    // node's `where` clauses and by ModuleLoader::convertGenericParams from
    // `MetaGenericParam`. ⚠ The imported side cannot preserve the INTERLEAVING
    // of a mixed clause (`where T: class, IFoo`) because the wire splits the
    // two into separate vectors: the special items come first, then the type
    // bounds. Only the ORDER OF DIAGNOSTICS can differ, since every reader
    // either any-matches or reports each violated item independently.
    std::vector<GenericParamConstraint> constraints;
};

// A.4 Member subkinds. Each carries the visibility + modifier prefix and a
// pointer back to the parent type's TypeSymbol for upward lookup.
struct MemberSymbol : Symbol {
    ast::Visibility visibility = ast::Visibility::None;
    std::vector<std::string_view> modifiers;
    TypeSymbol* parent = nullptr;
    // §22.15 (v0.286, fix #3609): this member's declared purity, on BOTH
    // sides. `SymbolCollection::collectMember` fills it for a LOCAL declaration
    // and `ModuleLoader::applyBehavior` from `MetaMember::isPure` for an
    // imported one, by the SAME rule the exporter used to write the wire
    // (`ModuleMetadataBuilder::convertMember`): the `pure` modifier, overridden
    // by `ast::OperatorDecl::isPure` for an operator and
    // `ast::ConstructorDecl::isPure` for a constructor, neither of which the
    // parser copies into a modifier list. ASK `memberIsPure` BELOW rather than
    // reading this directly: not every `MemberSymbol` is built by
    // `collectMember` (the builtin-interface shims in SemaContext.hpp and a
    // record's synthesized positional fields are not), and for those this field
    // keeps its `false` default while the accessor's decl and modifier arms
    // still answer.
    //
    // ⚠ It was an IMPORT-ONLY channel until #3609, and `applyBehavior` still
    // FORGES a `pure` entry in `modifiers` for an imported member so the
    // `hasMod(sym, "pure")` sweeps in TypeResolution_Checks.cpp keep seeing it
    // (fix #2054 added both halves in one commit). That forgery is load-bearing
    // — §22.15 dispatch rule 1 at TypeResolution_Checks.cpp:4462 reads an
    // imported base member's purity through it — so it stays until those sites
    // are routed through `memberIsPure` too.
    bool isPure = false;
    // §22.16 / #2054: this member's declaration-derived contract records,
    // reconstructed from an imported artifact (ModuleLoader.cpp:736, :4312).
    //
    // ⚠ NOTHING READS THIS YET, and that is deliberate rather than an
    // oversight — see issue 3611. §22.16's clauses are enforced CALLEE-side
    // (measured: a cross-module `requires` violation raises
    // ContractViolationException from the exporter's own compiled prologue), so
    // an importer needs no records to enforce a direct call. What it WOULD need
    // them for is the half of §22.16 that is unimplemented on both sides of the
    // seam: clauses are "inherited, never restated" by an override, and an
    // override of an IMPORTED base method has no other channel to the base's
    // clauses. `TypeSymbol::invariantContracts` is the same channel one level up
    // and already has that reader (ModuleMetadataBuilder.cpp:858-864). Kept, not
    // deleted, so implementing 3611 does not have to re-derive the carriage.
    std::vector<ContractRecord> contractRecords;
    // fix #3140 (§9.8, family F2 of umbrella #3152): this storage member's
    // `serialized` clause — `skip`, `optional`, and the `"<name>"` wire rename.
    //
    // The grammar admits the clause on exactly three storage-bearing sites
    // (A.4 SerializedClause): a record's PositionalField, an auto-property's
    // PropertyBody, and an EnumConstant (that last one rides
    // `EnumConstantSymbol::serializedName`, #3140 family F4). It rode
    // `ast::Param::serialized` / `ast::PropertyDecl::serialized` and NOTHING
    // else, so every §9.8 emitter recovered it by casting `Symbol::decl` — which
    // is null for every IMPORTED member — and answered "no clause" for all of
    // them. Answering "no clause" is not a safe default here: dropping a
    // `serialized skip` component ADDS it to the wire while the header count
    // says otherwise, desynchronising the reader, which is the exact defect
    // #1612 fixed on the local side.
    //
    // Populated on BOTH sides (SymbolCollection from the AST, ModuleLoader from
    // `MetaMember::serializedSkip`/`serializedOptional`/`serializedWireName`),
    // so `serializedComponentClause` below needs no source switch — the
    // `builtinExceptionBase` (#2950) / `enumWireName` (#3140 F4) template.
    // Read them through that accessor, never directly.
    bool serializedSkip = false;
    bool serializedOptional = false;
    std::string_view serializedWireName;  // empty => the DECLARED name
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
    // §22.15 "Where `pure` may appear" / "Purity attaches to the getter"
    // (#3618, v0.286): whether this property's GETTER ACCESSOR is declared
    // `pure get`. Set on BOTH sides — SymbolCollection's PropertyDecl arm
    // fills it from `pd->accessors` (any accessor with `isPure && !isSet`)
    // and ModuleLoader's Property case fills it from `MetaMember::accessorIsPure`
    // (the wire channel #3618 adds, mirroring the SYMBOL-SIDE channel
    // `MemberSymbol::isPure` has had since #3609). Consulted AHEAD of the
    // `ps->decl` AST-walk by `propertyGetterIsPure` — for a LOCAL symbol
    // that path returns identically (the AST walk would agree); for an
    // IMPORTED symbol the AST walk is null and would have answered "no",
    // the silent-wrong-classification direction the bug measured. Defaults
    // to `false` so a future PropertySymbol built by something other than
    // SymbolCollection/ModuleLoader (e.g. a builtin-interface shim in
    // SemaContext.hpp) keeps its old admission behavior — `propertyGetter
    // IsPure`'s `memberIsPure` arm is what those shims rely on.
    bool accessorIsPure = false;
};

struct EventSymbol : MemberSymbol {};

struct ConstMemberSymbol : MemberSymbol {
    bool isStatic = false;
    // fix #3230 (§17.14 "Constant expressions" / §15.1 "Inline arrays" /
    // §22.12 "Static members", family M11 of umbrella #3152): the constant's
    // typed integer value, when the const's declared type is a primitive
    // integer-family primitive (§4, bytes through ulong) and its initializer
    // is a §17.14 constant expression that folds. Populated for LOCAL consts
    // by sema's resolveTypes / checkConstTypes pass from the AST initializer,
    // and for IMPORTED consts by ModuleLoader from the textual
    // `MetaMember::intValue` carrier (parsed with the same `parseIntLiteral`
    // sign-split as codegen's foldImportedConstText, fix #3216). For an
    // IMPORTED const the symbol is reconstructed from dependency metadata
    // and has no AST TypeDecl, so the fold closures in TypeResolution_Names
    // / TypeResolution_Checks / ExprTypecheck cannot read `decl->value` —
    // they fall back to this field instead. Empty (default) for a const
    // whose declared type is not an integer-family primitive (string, bool,
    // char, float, decimal) and for a non-foldable integer initializer;
    // default is `std::nullopt` so an unset value is distinguishable from
    // a legitimate zero (a `:= 0` initializer sets the value to 0, not to
    // "unset").
    std::optional<std::int64_t> intValue;
};

struct EnumConstantSymbol : MemberSymbol {
    // §10/§10.2: the constant's underlying integer value. For an IMPORTED enum
    // the symbol is reconstructed from dependency metadata (which carries the
    // value, issue #328 FINDING [1]) and has no AST TypeDecl, so codegen reads
    // this field instead of re-deriving from the (absent) AST. For a LOCAL enum
    // codegen still derives values from the AST in computeEnumValues(); this
    // field is the cross-module carrier. Defaults to 0 when not set.
    std::int64_t value = 0;
    // fix #3140 (§9.8, family F4 of umbrella #3152): the constant's
    // `serialized "<name>"` RENAME, or empty when it carries no clause — in
    // which case §9.8's wire name is the declared name. Unlike `value` above
    // this is populated on BOTH sides (SymbolCollection from the
    // `ast::EnumConstant`, ModuleLoader from `MetaMember::enumWireName`), so
    // `enumConstantWireName` (EnumValues.hpp) needs no source switch at all.
    std::string_view serializedName;
};

// §35 (v0.200, #2044): a choice case — payload-free (`NotModified`) or
// payload-carrying (`Failed(status: int, message: string)`). Lives alongside
// the choice's other members. The case's payload fields are reached through
// `caseDecl` (a pointer to the ast::ChoiceCase inside the parent TypeDecl's
// `choiceCases` vector) — they drive §35.4 classification, §35.5 matching,
// and §35.3 call-site binding when payload construction lands. A
// payload-free case has `caseDecl->payload.empty()`; a payload case's arity
// is `caseDecl->payload.size()`.
struct ChoiceCaseSymbol : MemberSymbol {
    const ast::ChoiceCase* caseDecl = nullptr;
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
    // §21.1 v0.201 (#2049): a `const` local's classification split.
    // `isCompileTimeConst` is true iff the initializer is a §17.14 constant
    // expression AND the declared/inferred type is a primitive value type or
    // `string` — the case-pattern / inline-array-size / constant-narrowing
    // positions admit it as a compile-time value (existing behavior). When
    // false, the local is a RUNTIME const: a single-assignment, never-owned
    // binding whose references are NOT constant expressions. Meaningful only
    // when `isConst` is true.
    bool isCompileTimeConst = false;
    // §17.15 (v0.216, #2098): this local is a CLAUSE binding of a binding
    // atom `(var x := E; body)`. Like a runtime `const` (§21.1), the binding
    // is single-assignment and NOT an assignable location: assignment,
    // compound assignment, `++`/`--`, and binding to a `ref`/`out` parameter
    // are compile errors naming §17.15 — including through a `do`-bodied
    // lambda inside the atom that captures the binding. Set by visitExpr's
    // BindExpr arm right after the clause's visitStmt registration.
    bool isBindAtomClause = false;
    // §31.1 / §31.2.1 (fix #3521): this local is a CATCH-ARM BINDING — the
    // `var ex: T` of a `catch` arm pattern. §31.1 "Exception ownership": "the
    // bound exception variable ... is a **borrow** of the in-flight exception;
    // its source is the runtime, and its lifetime is the arm body's."
    //
    // This is the SINGLE spelling of that classification. It replaces three
    // independent ones (#3419's family): sema anchored the fact on `decl`
    // (`bnd->decl = arm.body.get()`), ownership RE-DERIVED it with a syntactic
    // AST walk (`collectCatchBorrowsStmt`), and codegen RECORDED its own copy
    // during lowering (`catchArmBindingDecls_`). Set by ExprTypecheck's catch-arm
    // binding registration, the one place that already knows.
    bool isCatchBinding = false;
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
    // §22.4 (v0.183, #1585): the `when true`/`when false` out-nullability
    // postcondition copied from the ast::Param. Consulted by the caller-side
    // narrowing (collectNarrowings) and the override modifier-match check.
    ast::OutNullWhen outNullPostcond = ast::OutNullWhen::None;
    // §22.5 "Optional parameters" / §2.5 (v0.254, #3178 — family M5 of umbrella
    // #3152): does this formal declare a default (`:= Expr`), and — for an
    // IMPORTED one — what was the exporter's rendered default text?
    //
    // Populated on BOTH sides, so the fact is complete on the symbol rather than
    // reachable only through the AST: `SymbolCollection::collectParams` sets
    // `hasDefault` from `ast::Param::defaultValue` for a locally declared
    // formal, and `ModuleLoader::convertParams` sets both from
    // `MetaParam::hasDefault` / `MetaParam::defaultText`, which
    // ModuleMetadataBuilder has exported and the decoder has read back since the
    // format's REC_SCALARS record — the loader simply dropped them.
    //
    // `defaultText` is EMPTY for a local formal on purpose: a local one carries
    // the real `ast::Expr` on its `ast::Param`, which is strictly better (it can
    // reference earlier parameters, `default(T)`, etc.). The text is the
    // import-only fallback, in the same rendered-literal shape as an imported
    // const's `valueText` (renderConstExpr), and codegen materializes it with
    // the same `foldImportedConstText` those reads already use.
    bool hasDefault = false;
    std::string_view defaultText;
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
    // §29.8 (#945, #2154): set by the ownership pass when iterator teardown
    // may still own this parameter. Codegen null-guards the frame slot. For the
    // narrow linear owning-iterator case, iteratorMoveYieldState records the
    // one-based yield state that transfers the parameter; a per-enumerator
    // Dispose then drops it only while suspended before that state.
    bool iteratorAutoDispose = false;
    unsigned iteratorMoveYieldState = 0;
    // §29.8 / §30.1 (#973a): set by the ownership pass on an `owned` parameter
    // of a state-machine method that IS discharged on some path but whose every
    // occurrence is one of the two forms
    // common::stateMachineOwnedParamIsFrameNullable admits — so the FRAME's
    // capture slot is nulled at the (single, marked-adoption) move-out site and
    // the frame's terminal-state disposal, already null-guarded, becomes an
    // exact runtime moved-from test: the discharged path skips, the fall-through
    // path disposes once. Disjoint from `iteratorAutoDispose` (which is the
    // never-discharged-on-any-path case); codegen's three frame-disposal loops
    // (sync iterator, async iterator, plain async) admit both flags.
    bool iteratorFrameConditionalAutoDispose = false;
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

// §9.1 / §9.2 / §2.8.3 rule 4 — THE record-storage accessors, symbol-side.
//
// fix #3145 (family F10 of umbrella #3152). Every §9.7 / §9.2 field walk used to
// go through an `ast::TypeDecl*` cast of `ts->decl` and then iterate
// `decl->positionalFields` + `decl->members`. `ModuleLoader` never assigns
// `Symbol::decl` for an imported type, so those walks saw NO fields at all for
// an import and every predicate built on them answered the empty-walk default.
// For §9.7 that default is "not borrow-bearing", which silently disabled an
// ownership-SOUNDNESS reject across module boundaries.
//
// The symbol table carries the fields on both sides. A positional field is a
// `FieldSymbol` locally (SymbolCollection.cpp, `posParam` -> the real
// `ast::Param`) and on import too (#2516, `posParam` -> a synthetic `ast::Param`
// with a null `type`, kept precisely so the positional/body distinction survives
// without an AST). So `TypeSymbol::members` is a complete storage list either
// way, and these two accessors are the ONE way to read it.

// Resolve a record storage component (a §9.1 positional field, a body field, or
// an auto-property) to its declared type, CONSULTING BOTH SOURCES in order:
// the positional `Param`'s type, then the body `FieldDecl` / `PropertyDecl`'s
// type, then the importer's `fieldTypes_` side table via
// `SemaContext::importedMemberType`. Null when the type is not resolvable —
// callers treat that conservatively.
//
// This was `static` in SymbolCollection.cpp, which is why Sendability.cpp grew a
// second copy as a local lambda and OwnershipFacts.cpp a third as a raw AST
// walk. It is declared here so there is one.
const TypeRef* storageComponentType(const Symbol* m, const SemaContext& ctx);

// True when `pred(field, fieldType)` holds for ANY of `ts`'s §9.1/§9.2 storage
// FIELDS — positional and body alike, `static` fields excluded (per-TYPE
// storage, not per-value; mirrors Resolver::recordValueContainsSelf).
//
// ⚠ ORDER IS `members` ORDER, WHICH IS NOT DECLARATION ORDER for a combined-form
// record: SymbolCollection appends the §9.1 positional FieldSymbols AFTER the
// body members (SymbolCollection.cpp — body at :298, positional at :326). An
// earlier revision of this comment claimed declaration order; it was wrong. This
// predicate is an existential, so order cannot change its answer — but a future
// caller that needs order must not take it from here. Use
// `recordPositionalFieldSymbols` for the index-aligned positional view.
//
// ⚠ The predicate does NOT receive an `owned` marker, and a caller must not try
// to recover one from `FieldSymbol::modifiers`: that vector is never populated
// for a positional field (SymbolCollection.cpp:310-327). The marker lives on
// `ast::Param::modifier` (`ast/Node.hpp`), which is a DIFFERENT field from
// `MemberDecl::modifiers`, so reading the latter silently answers "not owned"
// for every positional field. That mis-read caused a real false-reject once
// (a `sealed class Node(owned l: Leaf)` rejected as "not `owned`").
//
// Auto-properties are deliberately NOT included: the §9.7 walk this serves has
// always been fields-only, and widening it here would be a silent semantic
// change riding on a plumbing fix.
bool anyRecordStorageField(
    const TypeSymbol* ts, const SemaContext& ctx,
    const std::function<bool(const Symbol*, const TypeRef*)>& pred);

// `ts`'s §9.1 positional fields, in declaration order — the indexed view, for
// the sites that pair constructor argument i with positional field i.
std::vector<const Symbol*> recordPositionalFieldSymbols(const TypeSymbol* ts);

// fix #3169 (§9.1 / §9.2 / §30.14 rule 2, v0.242; refactor of #3145/#3152): the
// unified symbol-side storage enumeration. One primitive that yields per-member
// FACTS — `symbol`, `name`, `type`, `isPositional`, `isStatic`, `isOwned`,
// `isAutoProperty` — for every §30.14 rule 2 storage member of `ts`. The three
// callers that previously walked `members` in three different ways
// (`anyRecordStorageField` for fields-only-existential, `recordPositionalFieldSymbols`
// for the positional-only-indexed view, and `forEachStorageMember` in
// Sendability.cpp for the full per-member walk with the §28.1 `owned` marker)
// now each delegate to this one enumeration.
//
// WHAT IT YIELDS, per member:
//   - `member`:        the symbol (FieldSymbol or PropertySymbol); never null.
//   - `name`:          the member's identifier (posParam name for a §9.1
//                      positional field, else the symbol's declared name).
//   - `type`:          `sema::storageComponentType(m, ctx)`, with optional
//                      substitution via `env` for a generic base (#3171).
//                      Null when the type is not resolvable; callers treat
//                      that conservatively.
//   - `isPositional`:  §9.1 positional field — for a record, every positional
//                      field; for a class, a captured primary-ctor parameter
//                      (uncaptured are filtered, per #3165).
//   - `isStatic`:      per-TYPE storage; the walk filters these out and never
//                      yields them (§30.14 rule 2 is "every INSTANCE field").
//   - `isOwned`:       the §28.1 `owned` marker, read from BOTH
//                      `ast::Param::modifier` (positional) AND
//                      `MemberSymbol::modifiers` (body / auto-prop). A reader
//                      that consults only one silently answers "not owned" for
//                      the other — bc0edd5's warning, the exact defect this
//                      primitive closes.
//   - `isAutoProperty`: true for an auto-property; the §9.2 storage member.
//                      For a record's positional field this is always false
//                      (a positional field is a FieldSymbol).
//
// ORDER: positional pass first, then non-positional pass — matching the order
// the AST walks the umbrella #3152 collapse chose, so a §14.2 component-path
// diagnostic names a positional field before a body one. For a non-class
// non-record the positional pass is skipped (there are no §9.1 positional
// fields).
//
// `env` is the optional substitution environment for generic bases (#3171).
// A non-generic type passes `nullptr` and the walk is unchanged.
struct RecordStorageMember {
    const Symbol* member = nullptr;
    std::string_view name;
    const TypeRef* type = nullptr;
    bool isPositional = false;
    bool isStatic = false;
    bool isOwned = false;
    bool isAutoProperty = false;
};
bool recordStorageMembers(
    const TypeSymbol* ts, const SemaContext& ctx,
    const std::function<bool(const RecordStorageMember&)>& fn,
    const common::RecordSubstEnv* env = nullptr);

// §35.1 / §35.4 / §2.8.2 clause 1 (fix #3144, family F9 of umbrella #3152):
// `t`'s choice cases in DECLARATION ORDER — which is the case's tag value, so
// the order is a semantic contract, not a convenience. Empty when `t` is null
// or is not a choice.
//
// It is a plain walk of `t->members`, so it needs no `ast::TypeDecl` at any
// level and answers identically for a local and an imported choice. That is the
// whole point: every consumer of a case's payload reaches it through
// `ChoiceCaseSymbol::caseDecl`, which `SymbolCollection` points at the real
// `ast::ChoiceCase` and `ModuleLoader` points at a faithful reconstruction
// (#3144) — one predicate, one source, no per-side variant to keep in step.
// `Emitter::choiceCasesOf` FORWARDS here (it was a duplicate until #3144's
// review nit N2). Case index i is the runtime tag, so the two agreeing is the
// tag-value contract — not a convenience.
std::vector<const ChoiceCaseSymbol*> choiceCasesOf(const TypeSymbol* t);

// §9.8 (#3140, family F2 of umbrella #3152): a storage member's `serialized`
// clause, resolved to what the §9.8 emitters actually need — the wire name is
// already defaulted to the declared name, so a caller never re-implements
// "rename if present, else identifier" (three copies of that had drifted).
struct SerializedComponentClause {
    bool skip = false;      // `serialized skip`: off the wire entirely
    bool optional = false;  // `serialized optional`: absence is not a fault
    std::string_view wireName;  // never empty for a named member
};

// The ONE way to ask what a §9.1/§9.2 storage member's §9.8 clause says. Reads
// `MemberSymbol`'s three fields, which SymbolCollection fills from the AST and
// ModuleLoader fills from the dependency's metadata — so it answers for a LOCAL
// and an IMPORTED member alike and never touches `Symbol::decl`.
//
// ⚠ NOT FOR AN ENUM CONSTANT. The third site A.4 admits the clause on is an
// `EnumConstant`, and that one already had its own carriage before this fix —
// `EnumConstantSymbol::serializedName` + `MetaMember::enumWireName`, landed by
// family F4 (5bbc16a) — read through `sema::enumConstantWireName`
// (sema/EnumValues.hpp). The two are deliberately NOT merged: folding them
// would change F4's already-shipped wire field for no behavioural gain. An
// enum constant therefore leaves `MemberSymbol::serializedWireName` EMPTY and
// this accessor would answer "the declared name" for it, which is wrong — call
// `enumConstantWireName` instead. Nothing populates both, so a member is
// carried by exactly one of the two channels.
SerializedComponentClause serializedComponentClause(const Symbol* m);

// §9.8 (#3140, family F2): the §9.8 wire SHAPE of a class's synthesized
// serialization surface — "tree", "graph", "cyclic", or "" for a class that has
// none (which is every class that does not declare
// `ISerializable`/`IDeserializable`, and every class whose conformance is fully
// hand-written).
//
// The shape is DERIVED, not declared: the `serialized tree|graph|cyclic` header
// clause was retired by #703, and `Resolver::checkSerializationEligibility`'s
// pre-pass now computes the shape from the class's component types. That pass
// used to write its answer into `ast::TypeDecl::serializedClass->shape` and
// twelve readers recovered it with `dynamic_cast<const ast::TypeDecl*>`; the
// answer now lives on the symbol and the AST clause carries only the parser's
// `polymorphic(...)` registry.
//
// ⚠ AN IMPORTED CLASS ALWAYS ANSWERS "" — and that is a MEASURED statement about
// this compiler, not an oversight of a channel. Cross-module class
// serialization is not implemented at any level: with the classification gates
// forced open on a real two-module build, an imported `class Box:
// ISerializable, IDeserializable`'s own round-trip SIGSEGV'd the program, and a
// LOCAL class holding an imported serializable class component SIGSEGV'd it too
// (the component is written as nothing and read back as null). Forcing the
// codegen arms open as well SIGSEGV'd `bezelc` ITSELF, because
// `emitClassDeserialize` reconstructs an object by matching the class's
// CONSTRUCTOR parameter names and then running its field initializers — both of
// which are `ast::` walks with no cross-module carriage, the same ABI gap #3162
// records for `new ImportedClass(args)`. So the empty answer keeps today's LOUD
// reject exactly as it is; issue #3179 carries the construction work, and when
// it lands the shape gains a `MetaTypeDecl` record and this comment goes away.
// What this accessor buys today is that the twelve sites stop each deciding it
// for themselves off a dead pointer.
std::string_view serializedClassShape(const TypeSymbol* t);

// §9.8 tier 4 (#3140, family F2): the class's `polymorphic("d": T, …)` registry,
// resolved. Empty for a class with no registry — and, per the note above, for
// every imported class. The discriminator is the string literal's contents; the
// type is the registered case's resolved `TypeSymbol` (null when it did not
// resolve, which is already a diagnosed program).
const std::vector<SerializedPolyCase>& serializedPolyCasesOf(const TypeSymbol* t);

// §9.8 (#3140 follow-up): can the §9.8 emitters actually WRITE a component of
// this type — as opposed to §9.8 ADMITTING it?
//
// The two questions are not the same, and conflating them is what this
// predicate exists to stop. `isSerializationEligibleType` answers the spec
// question ("is this shape in §9.8's admitted list"); this one answers the
// implementation question ("did enough of the shape survive to lower"). They
// diverge for exactly one reason today, and it is a CARRIAGE gap, not a spec
// gap: `ModuleMetadataBuilder::convertType` leaves `MetaType::arraySize` EMPTY
// for an inline array — "the const-expr evaluator is sema-internal and not
// threaded to the builder", its own comment says — so `ModuleLoader` cannot
// reconstruct it either, and `Emitter::lowerTypeRef` falls back to a bare `i32`
// PLACEHOLDER for an imported `[N]T`.
//
// MEASURED (§9.8, two-module build, identical all-zero values so the difference
// is purely structural): a `record InlineArr(A: [3]int, N: int)` serialized 22
// bytes when IMPORTED and 50 when declared locally — because
// `emitSerializeField`'s inline-array arm is a four-conjunct positive guard with
// NO `else`, and the imported field failed its `val->getType()->isArrayTy()`
// conjunct (instrumented: `owned=0 elem=<non-null> isArrayTy=0 size=(nil)
// sizeVal=-1` imported, `isArrayTy=1 sizeVal=3` local). The component silently
// fell through to the write path's final "emit nothing" fallback while the
// header count still counted it, so an imported writer and a local reader
// disagreed by a whole component — and an imported-only round-trip raised
// NOTHING AT ALL, because both halves agreed on the same wrong form. §9.8's
// record clause explicitly admits "a non-owning inline array `[N]T`/`[N]T?` of
// record-admissible element type", so this is an admitted shape, not an edge.
//
// ⚠ THE LIST IS "SHAPES WHOSE CARRIAGE IS INCOMPLETE", NOT "SHAPES §9.8
// ADMITS". Do not grow it into a second copy of the emitter's dispatch — that
// is the duplicated-predicate failure this umbrella is about. The backstop for
// a shape NOT listed here is `emitSerializeField`'s fallback, which #3140's
// follow-up made a LOUD diagnostic precisely so the next carriage gap fails the
// build instead of writing a different wire form.
bool serializationComponentTypeIsEmittable(const TypeRef* ty);

// §9.8 (#3140 follow-up): does EVERY §9.8 component of `t` have an emittable
// shape (per the predicate above), recursively through nested record/choice
// components?
//
// ⚠ THIS IS NOT #3139's DELETED INTERLOCK, which was `t->decl != nullptr` — a
// dead-pointer read that answered "not emittable" for every imported type. This
// one asks the real question of both sources and answers TRUE for the imported
// types that are genuinely fine (which is every shape but the one above).
//
// It exists because §9.8's eligibility check has a "trust the nested
// declaration, it is checked at its own conformance site" posture — and for an
// IMPORTED type that site NEVER RAN: `Resolver::checkSerializationEligibility`
// iterates `ctx_.module()->children`, local types only. So "trust" is
// unbacked across a module boundary, and this walk is what backs it.
bool serializationSurfaceIsEmittable(const TypeSymbol* t, const SemaContext& ctx);

// §9.2 / §22.9 / §2.8.2 clause 1 (umbrella #3152): the BOTH-SOURCES twin of
// `hasAstAutoPropertyDecl`. That predicate reads `ast::PropertyDecl::bodyKind`
// off `ps->decl`, which is null for EVERY imported property (ModuleLoader has
// no AST for a dependency's types), so it answers "not auto-property storage"
// for an imported auto-property — a silent wrong answer, not a null-deref.
// This one asks the same question of whichever source the symbol has: the
// §9.2 / §22.9 / §2.8.2 clause 1 (umbrella #3152): "is this member
// auto-property STORAGE?" — a non-static, non-indexer property in the auto
// form (`Name: T;`, optionally with an initializer, no accessor bodies / arrow
// getter), backed by compiler-generated storage exactly like a field.
//
// This is THE predicate. It consults whichever source the symbol has: the
// declaration when there is one, else the `MetaMember::isAutoProperty` wire bit
// #2478 published as `ctx.importedPropertyIsAutoStorage`. Both sources compute
// the SAME rule (the exporter mirrors `hasAstAutoPropertyDecl` when it sets the
// bit), so they agree by construction. Codegen calls this one too — there is no
// `Emitter` copy; #3152's whole lesson is that a second spelling of one question
// is how the two sides of a module boundary came to disagree.
//
// fix #2931 (§11.2/§23.1, v0.240) — an INTERFACE property is classified `true`
// here, DELIBERATELY. It is spelled `property P: T;`, which the parser records
// as `PropertyBodyKind::Auto` for the interface arm exactly as for a class
// auto-property. The exclusion belongs one level down, in codegen's
// `storageMemberOf` — see the comment there. This predicate has a SECOND family
// of consumers that read it as "is a read of this member a NAMED borrow rather
// than a freshly produced value?" (the disposal classification:
// `sourceIsNamedOwner` in lowerIEnumerableBuiltinFor and its async twin,
// `srcIsNamedOwner` in the using/await paths). Flipping an interface property
// to `false` would reclassify `for x in iface.Prop` from "borrow, do not
// dispose" to "fresh value, dispose it" — the heap-use-after-free direction,
// and precisely the misclassification #2485 fixed in the other direction. The
// static interface type cannot tell an auto-property implementation (a borrow)
// from a computed one (a fresh value), so `true` stays as the leak-over-UAF
// conservative answer.
bool isAutoPropertyStorageSymbol(const Symbol* m, const SemaContext& ctx);

// §9.2 / §22.9: the AST-ONLY half of the predicate above — "does this symbol
// carry an `ast::PropertyDecl` that declares an auto-property?"
//
// ⚠ This is NOT "is this auto-property storage". `Symbol::decl` is null for
// EVERY imported member (ModuleLoader has no AST for a dependency's types), so
// this answers `false` for an imported auto-property — a silent wrong answer,
// not a null-deref. It is named for what it tests so no caller reaches for it
// meaning the other thing (it was `isAutoPropertySymbol`; NINE of its sixteen
// callers wanted `isAutoPropertyStorageSymbol` — the other six are the
// legitimately AST-only sites listed below, and #3152's review corrected an
// earlier "fifteen of sixteen" here. Six of the nine mis-answers were measured:
// §9.5 `with`, §22.7 read, §22.7 write, §22.8 initializer (silent wrong data,
// and a SEGV for a `string` property), §9.8 conformance (silent accept), and
// §9.2 auto-equality).
//
// Legitimate callers are only: the both-sources predicate above; a site that
// goes on to dereference the `ast::PropertyDecl` it just proved present; a
// declaration-site walk over `ctx.module()->children` (which requires
// `t->decl`, so no imported symbol reaches it); and the EXPORTER, where the
// declaration is the authority that computes the wire bit.
bool hasAstAutoPropertyDecl(const Symbol* m);

// §9.1 / §9.2 / §22.8 (v0.254, fix #3182 — umbrella #3149/#3152): is `m`
// INSTANCE FIELD storage — a non-`static` body field, or a §9.1 positional
// parameter captured into a field? The FIELD twin of
// `isAutoPropertyStorageSymbol`, and the same both-sources contract: it answers
// for a LOCAL and an IMPORTED field alike and never touches `Symbol::decl`.
//
// It needs no new wire bit. Both sides already populate `FieldSymbol`:
// `SymbolCollection` copies `ast::FieldDecl::modifiers` onto the symbol, and
// `ModuleLoader` sets `modifiers` from `MetaMember::modifiers` — so the
// `static` test is a symbol read on both. The positional discrimination is
// #2516's synthetic `posParam`, which the loader already reconstructs.
//
// WHAT THE DEFECT WAS. `Resolver::checkClassSerializationEligibilityLate`'s
// `classHasOwnStorage` asked `dynamic_cast<const ast::FieldDecl*>(fs->decl)`
// and, on a null cast, fell through to "no own storage" unless the field was a
// captured positional parameter. That cast is null for EVERY imported field, so
// an imported base carrying a plain body field reported no storage and #2364's
// guard — which refuses `IDeserializable` over a base with storage, because the
// synthesized `Deserialize` does not construct the base subobject — did not
// fire. MEASURED: `class D: ImportedBase, ISerializable, IDeserializable` with
// `private _v: int := 41;` on the base COMPILED AND RAN, while its
// character-identical single-module twin was rejected. Private instance fields
// DO cross the boundary (#2456 carries them for layout), so the storage is
// really there; only the classification was wrong.
//
// ⚠ NOT "does this member contribute to §9.8's wire form". This is the §22.8
// instance-LAYOUT question — "does this level of the hierarchy carry storage of
// its own" — which is why `static` is excluded (process-global, no instance
// footprint) and visibility is not consulted at all (a `private` field occupies
// a slot exactly as a `public` one does; that is #2456's whole point).
//
// ⚠ AN IMPORTED CLASS'S CAPTURED PRIMARY-CONSTRUCTOR PARAMETER answers TRUE
// when (and only when) the parameter was captured into a body field on the
// exporter's side — the `posParamBodyRef` flag now rides the wire as
// `MetaMember::posParamBodyRef` (the 18th trailing scalar, additive within
// v7 — same shape as `isPositional` at #2516 / 17th scalar), and
// `ModuleLoader`'s `convertMember` FieldSymbol arm sets it back on the
// imported symbol. fix #3221. The records side is unchanged: a §9.1 record
// positional field has no body references and `posParamBodyRef` is
// structurally false on both sides, so the `posParam && posParamBodyRef`
// arm returns false for every imported record as it did before.
bool isInstanceFieldStorageSymbol(const Symbol* m);

// §22.15 (v0.254, fix #3174 — family M8 of umbrella #3149/#3152): is this
// constructor `pure`? THE predicate; every §22.15 site that needs a
// constructor's purity asks here.
//
// It consults both sources, symbol-side first: `MemberSymbol::isPure`, which
// `ModuleLoader::applyBehavior` fills from `MetaMember::isPure` (the exporter
// writes it from `ast::ConstructorDecl::isPure`, ModuleMetadataBuilder.cpp);
// then the `ast::ConstructorDecl` when there is one; then the `pure` modifier.
// The three sources are not redundant — each is the ONLY one populated on some
// path:
//
//   * `isPure` was an IMPORTED-ONLY channel when this predicate was written;
//     since #3609 `SymbolCollection` fills it locally too, from
//     `ConstructorDecl::isPure` (it still does not copy `cd->modifiers` onto a
//     `ConstructorSymbol`, so the modifier arm remains empty for locals);
//   * `decl` is null for every constructor of a non-generic IMPORTED CLASS —
//     `ModuleLoader` re-parses ctor body text only for a generic type or a
//     record (ModuleLoader.cpp, the `parseAndAttachCtorBody` caller);
//   * the modifier scan is what `callTargetIsPure` and `throwCarveOutAdmits`
//     already did and is kept exactly, so no admitted program changes verdict.
//
// WHAT THE DEFECT WAS. `PureBodyWalker::checkPureCtorChainPurity` resolved the
// `base(…)`/`this(…)` chain target to an `ast::ConstructorDecl*` and skipped
// every candidate whose cast came back null. For a non-generic imported base
// class that is EVERY candidate, so the loop found nothing and the function's
// "no matching ctor — admit silently" early-out ran: a local `pure constructor`
// chaining to an imported IMPURE base constructor compiled clean, while the
// exporter's own compilation of the character-identical shape rejected it
// (measured on `ec6a02f`). §22.15: "A pure constructor's `base(…)`/`this(…)`
// chain target must itself be pure."
bool constructorIsPure(const ConstructorSymbol* cs);

// §22.15 (v0.286, fix #3609): is this member declared `pure`? THE predicate
// for every member kind; `constructorIsPure` above is its constructor-typed
// alias, kept because #3174's contract is recorded against that name.
//
// It consults the same three sources in the same order, generalised off
// constructors: `MemberSymbol::isPure` (now filled on BOTH sides — see the
// field), then the AST node when there is one (`ast::OperatorDecl::isPure` for
// an operator, `ast::ConstructorDecl::isPure` for a constructor — the two
// spellings of `pure` the parser does NOT put in a modifier list), then the
// `pure` modifier. The set matters even though the field now subsumes most of
// it: `decl` is the only source for a local operator or constructor built
// before the symbol's `isPure` is assigned, and the modifier arm is what every
// pre-#3609 call site did and is kept verbatim so no admitted program changes
// verdict.
//
// WHAT IT REPLACED. Five sites in PureChecker.cpp each asked a DIFFERENT SUBSET
// of those sources — `callTargetIsPure`'s method arm (all three), the `in`/
// `Contains` receiver scan (field + modifier, its own comment claiming it was
// the "same predicate"), `checkOneMember`'s method arm (decl + modifier),
// its property arm (modifier + accessor-level `pure get`), and the pure-ctor
// field-initializer gate (decl only). None disagreed in a reachable shape,
// because `ModuleLoader::applyBehavior` forges the `pure` modifier for every
// imported member; the drift risk was the point, not a measured miscompile.
bool memberIsPure(const MemberSymbol* m);

// §22.15 "Purity attaches to the getter" (v0.286, fix #3609): is THIS
// property's GETTER pure? A strictly wider question than `memberIsPure(ps)`,
// because `pure get` is an accessor-level marker riding
// `ast::PropertyAccessor::isPure` — a channel the property's own modifier list
// does not carry, and one `MetaMember` has no slot for either (the exporter
// writes `MetaMember::isPure` from the PROPERTY's modifiers,
// ModuleMetadataBuilder.cpp), so this arm answers for local declarations only
// and an imported `pure get` property reads as not-pure. That asymmetry is a
// real seam gap, filed as issue 3618; it is inert today because
// `callTargetIsPure` admits every property read unconditionally.
bool propertyGetterIsPure(const PropertySymbol* ps);

// §9.1 / §9.2 (issue #501, #3600): is `rec` a POSITIONAL (or combined-form)
// record — the §9.1 package that always synthesizes value semantics?
//
// BOTH sources are load-bearing. `ModuleLoader` sets
// `TypeSymbol::hasPositionalFields` for an IMPORTED record and never assigns
// `TypeSymbol::decl`, so an `ast::TypeDecl*` cast of `rec->decl` is null for
// every imported type; a LOCAL record has the flag mirrored onto the symbol
// at collection time too (`SymbolCollection.cpp`), so the symbol-side check
// alone already answers correctly for both — the AST fallback exists only as
// a defensive second source. The canonical statement of why reading only the
// AST half is a live defect (a MEASURED cross-module `ToString` miscompile,
// f1175c6) lives on codegen's own copy, `Emitter::recordIsPositional`
// (`LlvmBackend_impl.hpp`) — that copy predates this one and is not yet
// routed through it (#3600 tracks finishing that).
//
// This is the ONE sema-side spelling; #3600 found ~9 independent, correct-
// today re-derivations of exactly this both-sources shape scattered across
// sema. Route new callers through this rather than re-deriving it again.
bool recordIsPositional(const TypeSymbol* rec);

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
// no positional fields). Returns true iff the record is "auto-hashable":
// every member is an auto-property whose type is an **admissible component
// type**, a method, an expression-bodied or block-form (computed) property, a
// constructor, or a non-equality operator overload — and the record declares no
// explicit field storage, no `override Equals`/`GetHashCode` method, and no
// `=`/`<>` operator overload. An eligible body record receives the §9.2/§8.5
// auto-equality package (`Equals`, `GetHashCode`, `ToString`) over its
// auto-properties.
//
// Admissible component types (one recursive rule, §9.2 v0.158): a primitive,
// enum, or `string`; another auto-hashable record; `T?` where T is
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

// §9.2 v0.210 (#2083) auto-equality eligibility for a *value-like choice*
// (a choice whose §35.4 classification is ValueLike — every payload is a
// value, a `string`, a runtime-managed type, or (transitively) a value-like
// record or choice, with no `owned`-marked payload and no borrow-carrying
// payload — v0.200, #2044). The §9.2 admissible-component list now includes
// "a value-like choice that is itself auto-hashable" — applying the
// recursive rule to every case's payload fields. Borrow-bearing and owning
// choices are not admissible in any component (class-typed payloads are
// outside the rule, exactly as class-typed record fields are — §9.7 / §35.7).
//
// The method body disjunction ("case methods / computed members / non-
// equality operator overloads / constructors don't disqualify") does not
// carry over to a choice — a choice has no instance storage beyond its
// case-payload fields, so equality is purely per-payload. v0.210 cites
// §35.7 directly: every payload field of every case must be a §9.2
// admissible component type, with "another auto-hashable choice"
// admitted to the recursive rule.
//
// Returns false for non-choice types and for choices whose classification
// is NOT ValueLike or whose payloads are not all admissible.
//
// #2047: `inst` is the concrete instantiation TypeRef (with genericArgs) when
// known, so §28.1 per-instantiation erasure of `owned` payload markers applies
// (`Result<int, string>` — markers erased — is auto-hashable). Null
// judges against the bare template (a non-generic choice, or a nested-payload
// recursive call where no instantiation is available).
bool isAutoEqualityBearingChoice(const TypeSymbol* ch,
                                 const SemaContext& ctx,
                                 const TypeRef* inst = nullptr);

// §9.2/§8.4: does this type declare its own equality member, i.e. an
// `operator =` / `operator <>` overload, or an `Equals` / `GetHashCode`
// method? Per §9.2 "Authors who want explicit equality on a record outside
// the auto-hashable set declare `=` and `<>` overloads … along with
// `Equals(Object?)`, `Equals(T)`, and `GetHashCode` overrides." Such a record
// is *not* auto-hashable, yet `=`/`<>`/`.Equals`/`.GetHashCode` are
// valid on it (they dispatch to the user's member), so it must be excluded
// from the §9.2 non-auto-equality compile error. Returns false for null.
bool recordHasUserDeclaredEquality(const TypeSymbol* rec);

// §3.6 / §35.7 (v0.276, #3333): does this choice declare its own equality
// surface — a HOMOGENEOUS `=`/`<>` overload alongside the §25.2 co-declared
// `Equals(T)`/`GetHashCode` pair? When yes, the choice is **hashable** even
// though `isAutoEqualityBearingChoice` returns false (the §35.7 disqualifier
// fired). The rescue applies ONLY to value-like choices: per §3.6's hashable
// row "a choice that is neither value-like auto-hashable nor declares user
// equality (§35.7 — owning and borrow-bearing choices are not hashable; a
// value-like choice outside auto-hashable **only** because it carries a
// homogeneous `=`/`<>` overload is hashable on the strength of the
// `Equals(T)`/`GetHashCode` §25.2's co-declaration rule obliges beside it,
// v0.276)". An owning or borrow-bearing choice with the co-declared pair is
// still NOT hashable.
//
// `inst` is the concrete instantiation TypeRef (with genericArgs) when known
// — the §28.1 per-instantiation erasure of `owned` payload markers applies
// to the classification check, exactly as in isAutoEqualityBearingChoice.
bool choiceHasUserDeclaredEquality(const TypeSymbol* ch,
                                   const SemaContext& ctx,
                                   const TypeRef* inst = nullptr);

// §3.6 / §8.3 (v0.158, #1338): the **hashable** derived type property —
// the classification a hash-container key/element must satisfy (`Dictionary<K,V>`
// / `OwningDictionary<K,V>` keys, `Set<T>` / `OwningSet<T>` elements). Per §3.6's
// table: "every type except a record that is neither auto-hashable nor
// declares user equality (`operator =`/`<>` with `Equals(T)`/`GetHashCode` per
// the §25.2 co-declaration rule)". So every primitive, string, enum, class,
// interface, function value, and inline array is hashable; a record is
// hashable iff it is positional/combined (§9.1 always synthesizes
// equality), is an auto-hashable body record (§9.2), or declares
// `operator =`/`<>`. A null or unresolved type is treated as hashable
// (conservative — never over-rejects at an instantiation site).
// v0.214 (#2087): a **choice** is hashable iff it is value-like
// auto-hashable (§35.7); owning and borrow-bearing choices are not,
// decided by the same §35.4 classifier the use-site `=`/`<>` error consults.
bool isEqualityBearing(const TypeRef* ty, const SemaContext& ctx);

// §14.2 (v0.214, #2087): given a type that has just FAILED isEqualityBearing,
// return a component-path fragment naming the offending type + the §3.6/§9.2/
// §35.7 structural reason, shaped to APPEND after the flat `where T: hashable`
// constraint-failure diagnostic's §14.2 citation, e.g.
//   ": record `Bad` is neither auto-hashable (§9.2) nor declares custom
//     equality (…, §25.2)"
//   ": choice `Handle` is owning (§35.7), not value-like auto-hashable"
// The hashable property surfaces its failure at the top-level type (a
// non-auto-hashable record, or a non-value-like choice), so the path is
// the offending type itself — the analog of sendableFailureComponentPath at the
// detail level §3.6 supports for this property. Returns empty (caller keeps the
// flat message) for any type outside that record/choice failure shape.
std::string hashableFailureComponentPath(const TypeRef* ty,
                                         const SemaContext& ctx);

// §22.3 "Parameter modifiers" / §2.5 "Dependency resolution" (v0.237, #2659,
// #2374): the declared modifier of constructor formal `pos` — one of "", "ref",
// "out", "owned", "out owned", "scoped".
//
// A constructor's formal list has two sources and every consumer must read
// BOTH. A LOCALLY declared constructor carries its formals as `ast::Param`s on
// its `ast::ConstructorDecl`; an IMPORTED one has NO AST at all — ModuleLoader
// (`MetaMemberKind::Constructor`) reconstructs a `ConstructorSymbol` whose
// `decl` is null and whose formals survive only as `ParameterSymbol` metadata,
// with the exporter's modifier string on `ParameterSymbol::modifier`
// (ModuleMetadataBuilder writes it, ModuleLoader::convertParams reads it back).
// A consumer gated on `dynamic_cast<const ast::ConstructorDecl*>(cs.decl)`
// therefore silently takes a "no modifiers at all" path for every imported
// constructor — the #2374 defect class. #2659 is that bug at its worst: an
// `owned` formal seen as non-owned means the caller's transfer is never
// recorded, so caller AND callee both dispose the argument (a double free).
//
// Returns "" when `pos` is out of range or no source carries a modifier, so a
// caller can treat "" and "not an owned sink" identically.
std::string_view formalParamModifier(const ConstructorSymbol& cs,
                                     std::size_t pos);

// Declared NAME of constructor formal `pos`, same dual sourcing. Needed by the
// §22.5 named-argument binding (a named argument binds the formal whose name
// matches, wherever it sits). Empty when out of range.
std::string_view formalParamName(const ConstructorSymbol& cs, std::size_t pos);

// Arity of the constructor's formal list, same dual sourcing.
std::size_t formalParamCount(const ConstructorSymbol& cs);

// §22.3 / §2.5 (v0.254, #3176 — family M1 of umbrella #3152): the same three
// accessors for a METHOD's formal list. A `MethodSymbol` covers an ordinary
// method, a free function, an extension method and an `operator` overload; the
// local source is the `ast::MethodDecl` (or `ast::OperatorDecl`) `params`, and
// the IMPORTED source is the reconstructed `ParameterSymbol` list on
// `MethodSymbol::params`, which ModuleLoader populates from `MetaParam` for
// every imported method (class members at ModuleLoader.cpp:524, extension
// members at :2891) — `MemberSymbol::decl` is set only where the loader
// re-parsed body text for a GENERIC type, so for a non-generic imported class
// every method's `decl` is null.
//
// #3176 is #2659's defect on the method path, and it is measured: an unmarked
// call to an imported `owned` formal recorded no move, so the caller's binding
// stayed Owned and was auto-dropped after the callee had already disposed it —
// `AddressSanitizer: heap-use-after-free`, freed in the callee, read in `main`.
// The marked spelling `M(owned r)` was rejected as spurious at the same time,
// so across a module boundary there was no correct way to write the call.
//
// Both sources are consulted in the same order as the constructor overloads —
// AST first (it carries the syntax), the symbol list second — so a member both
// of them describe can never get two answers. Returns "" / 0 out of range.
std::string_view formalParamModifier(const MethodSymbol& ms, std::size_t pos);
std::string_view formalParamName(const MethodSymbol& ms, std::size_t pos);
std::size_t formalParamCount(const MethodSymbol& ms);

// The declared TYPE of method formal `pos`, resolved. §28.14's call-bounded and
// deferred-activation gates ask "is this formal function-typed", which needs a
// type and not just a modifier. The local source is the `ast::Type` node (the
// caller resolves it through `SemaContext`); the imported source is
// `ParameterSymbol::resolvedDeclType`. `formalParamDeclType` returns the
// syntactic node (null for an import), `formalParamResolvedType` the already-
// resolved imported one (null for a local).
const ast::Type* formalParamDeclType(const MethodSymbol& ms, std::size_t pos);
const TypeRef* formalParamResolvedType(const MethodSymbol& ms, std::size_t pos);
const ast::Type* formalParamDeclType(const ConstructorSymbol& cs,
                                     std::size_t pos);
const TypeRef* formalParamResolvedType(const ConstructorSymbol& cs,
                                       std::size_t pos);

// §28.1 "Where the `owned` modifier can appear" / §28.3 "Producers" / §2.5
// "Dependency resolution" (v0.254, #3189 — family M2 of umbrella #3152):
// does this method's DECLARED RETURN carry the `owned` marker?
//
// This is the single most consumed ownership fact in the compiler: it is the
// sound discriminator between "this call transfers a fresh +1 the caller must
// dispose" and "this call hands back a BORROW that reclaiming would
// use-after-free". Fifteen sites across sema, the ownership pass and codegen
// asked it, and every one of them asked it as
//
//     dynamic_cast<const ast::MethodDecl*>(ms->decl)->returnOwned
//
// `ModuleLoader` sets `MemberSymbol::decl` only where it re-parsed exported
// body text for a GENERIC type, so for a method of any non-generic imported
// class that cast is null and each site took its `false` branch — which is not
// "unknown" but the substantive claim "returns nothing owned". Measured on a
// real two-module build (`bezelc -c lib.bzl` then `--import lib.o app.bzl`),
// against character-identical single-module twins:
//
//   * `Lib.Make(1);` as a bare statement — the twin is a hard §21.9 error,
//     the imported spelling compiles silently and leaks 3200 bytes in 200
//     allocations under LSan;
//   * `Lib.Make(i).v` as an unbound receiver temporary — the twin is clean,
//     the imported spelling leaks 3200 bytes in 200 allocations;
//   * `using var c := a + b` over an imported `operator +(): owned Cell` —
//     the twin's live-object count returns to 0, the imported spelling leaves
//     2 objects live (the intermediate and the result);
//   * `using var sub := imported.Subscribe(lambda…)` — the twin compiles, the
//     imported spelling is a §28.14 FALSE REJECT (#3189).
//
// BOTH AST decl kinds are consulted. A `MethodSymbol` also backs an `operator`
// overload, whose decl is an `ast::OperatorDecl` — a sibling of
// `ast::MethodDecl`, not a subclass — and both carry `returnOwned`. Reading
// only one kind is how drift #5 of the audit arose (two arms of ONE lambda,
// thirteen lines apart, disagreeing about the same fact).
//
// The sources are consulted in the AST-then-symbol order the `formalParam*`
// accessors above use, so a member both sources describe can never get two
// answers: for an imported GENERIC type the loader sets BOTH `decl` (body
// re-parse) and the `returnOwned` wire bit, and the AST answer wins — i.e.
// this accessor changes NOTHING for that case. `SemaContext::
// importedReturnOwned` is populated only by `ModuleLoader` (via
// `setImportedReturnType`, for class members at ModuleLoader.cpp:531 and for
// imported EXTENSION methods at :2897), so a locally synthesized decl-less
// `MethodSymbol` still answers `false` exactly as before.
bool methodReturnsOwned(const MethodSymbol& ms, const SemaContext& ctx);

// §22.5 "Optional parameters" / §2.5 (v0.254, #3178 — family M5 of umbrella
// #3152): does formal `pos` declare a default (`:= Expr`)? Same dual sourcing —
// the `ast::Param`'s `defaultValue` locally, `ParameterSymbol::hasDefault` for
// an import. `formalParamDefaultText` is the IMPORT-only companion: the
// exporter's rendered default literal, empty for a local formal (which carries
// the real expression on its `ast::Param` and needs no text).
bool formalParamHasDefault(const MethodSymbol& ms, std::size_t pos);
bool formalParamHasDefault(const ConstructorSymbol& cs, std::size_t pos);
std::string_view formalParamDefaultText(const MethodSymbol& ms,
                                        std::size_t pos);
std::string_view formalParamDefaultText(const ConstructorSymbol& cs,
                                        std::size_t pos);

// Can an IMPORTED formal's exported default text be materialized at a call site
// that omits the argument?
//
// §22.5 lets a default be NON-CONSTANT (`DateTime.Now`, `TimeSpan.FromSeconds
// (30)`), but only a compile-time constant survives export:
// `ModuleMetadataBuilder::renderConstExpr` renders literals (and folds literal
// integer arithmetic) and returns "" for everything else, and it renders a bare
// identifier BY NAME — a spelling with no guaranteed meaning on the importing
// side, which the imported-const reader (`Emitter::foldImportedConstText`)
// therefore declines rather than silently folding as 0.
//
// This is the ONE predicate for that question, so sema's admission and codegen's
// materialization cannot disagree: without it, sema would accept the omission
// (`hasDefault` is true) and codegen would then decline the call, turning a loud
// §22.5 reject into a silent bail. It is a syntactic test over exactly the
// shapes `renderConstExpr` emits for a literal — a number, a quoted string, a
// char literal, `true`/`false`, or `null`.
//
// Carrying the remaining §22.5 default forms across a module boundary is #3213.
bool importedDefaultTextIsMaterializable(std::string_view text);

// §22.5 "Argument binding" / §2.5 (v0.254, #3178; the shape #3173 specifies) —
// ONE view of a callable's formal list, for the code that only needs to BIND
// arguments to it.
//
// The §22.5 binder (`ExprTC::bindArgsToParams`) needs exactly three facts per
// formal: how many there are, each one's NAME, and whether each one may be left
// unbound. All three already have both-sources accessors above, but the binder
// took a `const std::vector<ast::Param>&` — a shape an IMPORTED callee does not
// have — so every cross-module call-check path had to open-code its own
// arity-only substitute. #3141 measured what that costs (a second copy of the
// rule, with five byte-identical diagnostic strings held identical by nothing
// but two authors typing the same sentence) and filed #3173; #3178 measured the
// other cost (the imported substitute demanded EXACT arity, so an optional
// parameter could not be omitted, and it bailed entirely on a named argument,
// so a cross-module named call was not checked at all).
//
// A view rather than a materialized vector: the producers are trivial, the
// binder is on the hot path of every call-site check, and a copy would be a
// third place a formal list can be spelled.
//
// ⚠ Only the BINDING facts live here. The per-argument §5.1 / §14.5 / §22.3
// checks genuinely differ between the two sources (the local path resolves an
// `ast::Type` and runs `substituteMemberType`; the imported one reads an
// already-resolved `TypeRef`), so they read `formalParam*` directly and are not
// forced through this view.
class FormalsView {
public:
    FormalsView() = default;
    // A concrete AST formal list — a locally declared method / constructor /
    // operator / lambda, or any caller that already holds the `ast::Param`s.
    static FormalsView of(const std::vector<ast::Param>& params) {
        FormalsView v;
        v.params_ = &params;
        return v;
    }
    // A method or constructor SYMBOL. Consults the AST first and the imported
    // `ParameterSymbol` list second, via the accessors above, so one view
    // answers for a local and an imported callee alike.
    static FormalsView of(const MethodSymbol& ms) {
        FormalsView v;
        v.ms_ = &ms;
        return v;
    }
    static FormalsView of(const ConstructorSymbol& cs) {
        FormalsView v;
        v.cs_ = &cs;
        return v;
    }
    // fix #3173 (§22.5, v0.254): the POSITIONAL-FIELD view — a class / record's
    // §9.1 / §22.10 primary-ctor parameter list, materialized as the per-slot
    // symbol list `recordPositionalFieldSymbols` already returns. The view
    // reads each slot's NAME from `Symbol::name` and its `hasDefault` predicate
    // off `FieldSymbol::posParam` (the synthetic or real `ast::Param*`), so
    // `bindArgsToParams` over this view applies the SAME rule and emits the
    // SAME five diagnostic strings it already emits for a method / ctor /
    // `ast::Param`-list callee. Before this view existed, the positional-field
    // binder was a SECOND COPY of the rule, with five byte-identical
    // diagnostics held identical only by authors typing the same sentence
    // (#3141). The view is non-owning: callers pass the vector they already
    // hold (`recordPositionalFieldSymbols(t)` returns by value, so the caller
    // either stores it in a local or hands the rvalue to a temporary whose
    // lifetime this view's binding reads from — the lifetime is the binder's
    // call, which is the lifetime of `FormalsView`).
    static FormalsView of(const std::vector<const Symbol*>& fields) {
        FormalsView v;
        v.fields_ = &fields;
        return v;
    }

    // fix #3425 / #3424 (§22.5 "Optional parameters and named arguments",
    // v0.283): the NAME-ONLY view — the built-in registry's parameter-name
    // rows (`common::BuiltinFormals`). A built-in member or constructor has
    // no `MethodSymbol`, no `ConstructorSymbol` and no `ast::Param` list at
    // all (its implementation is an arm of codegen's `lowerCallDispatch` /
    // `lowerNew`, not a declaration sema can see), so none of the four views
    // above can answer for it — yet the binder needs only its three facts,
    // and the registry carries exactly those: the declared names in order and
    // the count of LEADING parameters that declare no default. Without this
    // view #3398's built-in path had to materialize a throw-away
    // `std::vector<ast::Param>` per call site, which could not express a
    // §22.5 optional parameter at all (an `ast::Param` reports `hasDefault`
    // from a `defaultValue` expression node the registry has nothing to put
    // there). Non-owning, like every other view: the caller holds the names
    // for the duration of the `bindArgsToParams` call.
    static FormalsView ofNames(const std::vector<std::string_view>& names,
                               std::size_t required) {
        FormalsView v;
        v.names_ = &names;
        v.required_ = required;
        return v;
    }

    std::size_t size() const {
        if (params_) return params_->size();
        if (ms_) return formalParamCount(*ms_);
        if (cs_) return formalParamCount(*cs_);
        if (fields_) return fields_->size();
        if (names_) return names_->size();
        return 0;
    }
    std::string_view name(std::size_t i) const {
        if (params_) return i < params_->size() ? (*params_)[i].name
                                                : std::string_view{};
        if (ms_) return formalParamName(*ms_, i);
        if (cs_) return formalParamName(*cs_, i);
        if (fields_)
            return i < fields_->size() ? (*fields_)[i]->name
                                       : std::string_view{};
        if (names_)
            return i < names_->size() ? (*names_)[i] : std::string_view{};
        return {};
    }
    bool hasDefault(std::size_t i) const {
        if (params_)
            return i < params_->size() && (*params_)[i].defaultValue != nullptr;
        if (ms_) return formalParamHasDefault(*ms_, i);
        if (cs_) return formalParamHasDefault(*cs_, i);
        // §22.10 / #3202: a primary-ctor parameter MAY carry a default (a
        // record cannot — A.3 `PositionalField` admits no default syntax — so
        // a record's posParam has neither defaultValue nor defaultText and
        // this answers false, matching the prior behaviour).
        if (fields_) {
            if (i >= fields_->size()) return false;
            if (auto* fs = dynamic_cast<const FieldSymbol*>((*fields_)[i]))
                return fs->posParam &&
                       (fs->posParam->defaultValue != nullptr ||
                        !fs->posParam->defaultText.empty());
            return false;
        }
        // fix #3425: the registry's rows put every defaulted parameter LAST
        // (the stdlib declares no other shape — §22.5 requires it: "Every
        // parameter after the first defaulted parameter must itself have a
        // default"), so one count answers for the whole row.
        if (names_) return i >= required_;
        return false;
    }

private:
    const std::vector<ast::Param>* params_ = nullptr;
    const MethodSymbol* ms_ = nullptr;
    const ConstructorSymbol* cs_ = nullptr;
    // fix #3173 (§22.5, v0.254): see `of(const std::vector<const Symbol*>&)`
    // above. A view, not a copy, so `bindArgsToParams` does not materialize a
    // third spelling of the formal list.
    const std::vector<const Symbol*>* fields_ = nullptr;
    // fix #3425 / #3424 (§22.5, v0.283): see `ofNames` above.
    const std::vector<std::string_view>* names_ = nullptr;
    std::size_t required_ = 0;
};

} // namespace bezel::sema
