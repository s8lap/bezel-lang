#pragma once

// Resolved-type representation. Distinct from ast::Type, which carries the
// syntactic form. A TypeRef ties a Type AST node to its resolved meaning:
// a primitive type, a TypeSymbol* (with resolved generic args), an inline
// array, a function type, the unit type, or an Error sentinel when the
// reference doesn't resolve.
//
// TypeRefs are produced by the Type Resolution pass (sema slice 2) and
// owned by SemaContext. The resolution itself is queried via
// `SemaContext::resolvedType(const ast::Type&)`.

#include <cstdint>
#include <vector>

#include "ast/Node.hpp"

namespace bezel::sema {

struct TypeSymbol;

// §3 builtin types. Lazily mapped from the primitive-type keyword tokens
// that may appear as a NamedType QualifiedName's first part. The `None`
// value signals "not a primitive" — used in transit, not stored.
enum class PrimitiveType {
    None,
    Bool, Char, String,
    Byte, SByte, Short, UShort, Int, UInt, Long, ULong,
    Float, Double, Decimal,
    Object,  // §4 Object class — treated as a primitive lookup key
};

struct TypeRef {
    enum class Kind {
        Error,          // resolution failed; suppress cascade diagnostics
        Primitive,      // PrimitiveType + nullable
        Named,          // user-declared class/interface/record/enum
        Unit,           // ()
        InlineArray,    // [N]T or [N owned]T
        Function,       // (T1, T2) -> T (or owned T)
    };
    Kind kind = Kind::Error;
    bool nullable = false;
    const ast::Type* astNode = nullptr; // back-pointer for diagnostics

    // Primitive:
    PrimitiveType primitive = PrimitiveType::None;

    // Named:
    TypeSymbol* declaration = nullptr;
    std::vector<TypeRef*> genericArgs;
    // §2.5 / §29.3 (issue #495): for a Named TypeRef reconstructed from module
    // metadata, astNode is null (no AST exists) and declaration may be null when
    // the type is a builtin not exported by the dependency (IEnumerable<T>,
    // Array<T>, Task, etc.).  importedName carries the raw type name in that case
    // so codegen (lowerTypeRef) can recognise it and return the correct LLVM type
    // (ptrTy() for heap-allocated builtins) rather than falling through to i32.
    // Always empty for locally-resolved TypeRefs (astNode / declaration carry the
    // name information there).
    std::string_view importedName;

    // #1462 / §27.8 / #578: a type alias (`type OA := OwningArray<R>`) resolves
    // transparently by copying the underlying TypeRef into this one, then
    // RESTORING astNode to the use-site node (the alias name `OA`) so
    // diagnostics point to the use site. That restore hides the underlying
    // type's syntactic name-head from every check that identifies a builtin
    // collection/wrapper by head (they read `astNode->name->parts[0]`, which for
    // an alias says `OA`). aliasResolvedHead carries the underlying builtin head
    // (`OwningArray`) so those checks — in sema, ownership, AND codegen, static
    // or not — see through the alias with no context threading. Empty for a
    // non-alias TypeRef (readers then fall back to astNode/importedName).
    std::string_view aliasResolvedHead;

    // #1462: the alias-transparent syntactic name-head of a Named TypeRef — the
    // ONE read every builtin-collection/wrapper identity check should use
    // instead of `astNode->name->parts[0]`, so a type alias resolves correctly.
    // Returns aliasResolvedHead (the underlying head, set at alias resolution)
    // when present; otherwise the use-site syntactic head; otherwise the
    // metadata importedName. Empty for a non-Named / headless TypeRef.
    std::string_view headName() const {
        if (!aliasResolvedHead.empty()) return aliasResolvedHead;
        if (astNode && astNode->name && !astNode->name->parts.empty())
            return astNode->name->parts[0];
        return importedName;
    }

    // InlineArray:
    const ast::Expr* arraySize = nullptr;   // ConstIntExpr; sema later evaluates
    // #734: the folded compile-time size (>= 0) when arraySize is a literal or a
    // member-level `const` whose initializer is an int literal. -1 = not folded
    // (codegen falls back to the IntLiteralExpr text, or to a scalar default).
    long long arraySizeValue = -1;
    bool arrayOwned = false;
    TypeRef* arrayElement = nullptr;

    // Function:
    struct FuncParam {
        std::string_view modifier;          // "", "ref", "out", "out owned", "owned"
        std::string_view name;              // optional
        TypeRef* type = nullptr;
    };
    std::vector<FuncParam> funcParams;
    bool funcReturnOwned = false;
    TypeRef* funcReturn = nullptr;
};

// Convert a primitive-type keyword's spelling to the PrimitiveType enum.
// Returns PrimitiveType::None for unknown lexemes.
PrimitiveType primitiveFromName(std::string_view name);

// §7.9 / §22.4 (v0.181): true when `t` is a non-nullable reference type — a
// type with *no* default value, for which a read before assignment is a
// definite-assignment error. These are class/interface types (including the
// builtin collection wrappers `Array<T>` / `OwningArray<T>` and other imported
// builtins), `string`, `Object`, and function types. Value types (primitives
// other than string/Object, records, enums, inline arrays, the unit type) and
// every nullable type carry a default (`0`/`false`/all-default/`null`) and are
// *not* reported. An unclassifiable / error TypeRef returns false (lenient — the
// analysis never invents a false positive). Defined in TypeResolution.cpp
// (needs the complete TypeSymbol to read a Named type's declaration kind).
bool isNonNullableReferenceType(const TypeRef* t);

// stdlib §2.19 (v0.176, #807): the compiler-provided primitive conformance
// table for the static-abstraction interfaces. The numeric tower (byte…ulong,
// float, double, decimal) conforms to INumber<T>/ISteppable<T>/IParsable<T>;
// `char` conforms to ISteppable<char> and IParsable<char> only. `string`
// (and every other primitive) conforms to none.
inline bool primitiveConformsToStaticIface(PrimitiveType p,
                                           std::string_view ifaceName) {
    if (ifaceName != "INumber" && ifaceName != "ISteppable" &&
        ifaceName != "IParsable")
        return false;
    switch (p) {
        case PrimitiveType::Byte:  case PrimitiveType::SByte:
        case PrimitiveType::Short: case PrimitiveType::UShort:
        case PrimitiveType::Int:   case PrimitiveType::UInt:
        case PrimitiveType::Long:  case PrimitiveType::ULong:
        case PrimitiveType::Float: case PrimitiveType::Double:
        case PrimitiveType::Decimal:
            return true;  // the numeric tower conforms to all three
        case PrimitiveType::Char:
            return ifaceName != "INumber";  // ISteppable + IParsable only
        default:
            return false;
    }
}

// spec §14.2 clause 5 (v0.196) + stdlib §9: the verdict of testing a PRIMITIVE
// against one of the four synthesized-declaration *instance* builtin interfaces
// (IComparable<T> / IEquatable<T> / IEqualityComparer<T> / IReadOnlySpan<T>).
// A primitive conforms to these ONLY as its stdlib record declaration lists
// them, and with the generic argument being the primitive itself:
//   • IComparable<P> — every primitive EXCEPT `bool` (stdlib §9.4 `Boolean`
//     lists `IEquatable<bool>` only, no `IComparable`).
//   • IEquatable<P>  — every primitive (stdlib §9.1–§9.6).
//   • IEqualityComparer<…> / IReadOnlySpan<…> — NO primitive conforms: these are
//     an equality-comparer *agent* (stdlib §2.18) and a span view (stdlib §4.16),
//     which no primitive's declaration lists.
// The result is three-valued so the constraint checker never invents a false
// positive: Rejects only when the primitive PROVABLY does not conform (a
// non-conforming interface head, or a decided-concrete WRONG generic argument);
// Defer when the generic argument is not a decided concrete primitive (a type
// parameter, a user/builtin Named type such as the PascalCase `Int32`, a
// nullable or missing argument) so an outer substitution or the recheck pass
// can still decide it. Callers pass the head name (the interface's declared
// name) and the resolved bound's generic arguments.
enum class PrimBuiltinIfaceConformance { Conforms, Rejects, Defer };

inline PrimBuiltinIfaceConformance primitiveConformsToBuiltinInstanceIface(
    PrimitiveType p, std::string_view ifaceHead,
    const std::vector<TypeRef*>& boundArgs) {
    // IEqualityComparer / IReadOnlySpan: no primitive conforms, any argument.
    if (ifaceHead == "IEqualityComparer" || ifaceHead == "IReadOnlySpan")
        return PrimBuiltinIfaceConformance::Rejects;
    if (ifaceHead != "IComparable" && ifaceHead != "IEquatable")
        return PrimBuiltinIfaceConformance::Defer;  // not a decided head
    // IComparable: `bool` declares no `IComparable` conformance (stdlib §9.4).
    if (ifaceHead == "IComparable" && p == PrimitiveType::Bool)
        return PrimBuiltinIfaceConformance::Rejects;
    // The single generic argument must be exactly the primitive itself.
    if (boundArgs.size() != 1 || !boundArgs[0])
        return PrimBuiltinIfaceConformance::Defer;
    const TypeRef* a = boundArgs[0];
    if (a->kind != TypeRef::Kind::Primitive || a->nullable)
        return PrimBuiltinIfaceConformance::Defer;  // type-param / Named / nullable
    return a->primitive == p ? PrimBuiltinIfaceConformance::Conforms
                             : PrimBuiltinIfaceConformance::Rejects;
}

// spec §14.2 clause 5 + §28.2/§9.7: `IDisposable`/`IAsyncDisposable` are
// implemented only by classes (implicitly, §28.2); records, enums, and
// primitives can NEVER implement them (§9.7). These auto-imported builtin
// interfaces resolve to a Named TypeRef WITHOUT a synthesized declaration, so a
// primitive tested against such a bound would otherwise fall through to a silent
// defer. True when a primitive against `head` provably cannot conform.
inline bool primitiveNeverConformsToBuiltinIface(std::string_view head) {
    return head == "IDisposable" || head == "IAsyncDisposable";
}

// fix #3434 (spec §4.7 *The unit type* / §21.7 *The `return` statement*,
// v0.283): does a callable whose return-type ascription RESOLVES to `r`
// produce a value?
//
// §4.7 makes `()` "the type with exactly one value" and "the return type of
// methods declared without an explicit return type", so a written `: ()` and a
// missing ascription name ONE type and one contract — a callable that yields
// nothing. `()?` is a different type ("an optional with two values, `null` and
// `()`"), so a callable ascribed `()?` DOES return a value; that `!nullable`
// conjunct is load-bearing and is #3095's.
//
// The caller supplies the resolved TypeRef for an ascription that EXISTS. A
// missing ascription is the caller's own "no value" case (there is no TypeRef
// to pass), and a null `r` here means an ascription that failed to resolve —
// which must NOT be read as unit, because that would suppress a real diagnostic
// on a broken program.
//
// This exists because the decision had been made TWICE and the two answers had
// drifted: sema's `ExprTC::returnsValue` (which drives §21.7's ReturnCtx, so
// `method Give(): () return C.Nothing(); end Give;` is rejected with "this
// callable has no return type") said yes to the equivalence, while codegen's
// `Emitter::arrowBodyLoweredAsUnit` tested the SPELLING — `!declRet`, whether a
// return type was WRITTEN — and so drew a spurious `#2491` internal-degrade
// note on `method M(): () => UnitCall();`. Both now ask here.
inline bool ascribedReturnTypeProducesValue(const TypeRef* r) {
    return !(r && r->kind == TypeRef::Kind::Unit && !r->nullable);
}

} // namespace bezel::sema
