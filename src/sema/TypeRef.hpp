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

    // #1462 / §27.8 / §578: a type alias (`type OA := OwningArray<R>`) resolves
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

} // namespace bezel::sema
