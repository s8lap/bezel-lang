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

    // InlineArray:
    const ast::Expr* arraySize = nullptr;   // ConstIntExpr; sema later evaluates
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

} // namespace bezel::sema
