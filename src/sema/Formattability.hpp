#pragma once

// §2.18 / §18.5 (v0.344): the FORMAT-SPEC VOCABULARY rule — the one predicate
// that answers "may a format spec apply to an operand of this type?".
//
// WHY THIS PREDICATE IS SHARED. Two spellings apply the same rule to the same
// operand, and they must not disagree:
//
//   * the interpolation hole `$"{x:F4}"` — stdlib §2.18 dispatches a hole's
//     format spec to `x.ToString(spec)` (spec §18.5's "Format dispatch"), so
//     the hole IS a member call; and
//   * the explicit member call `x.ToString("F4")`.
//
// A user who writes the second spelling must get the same answer as the first.
// Before fix #7333 the hole path answered "no" for a `bool` operand (through
// this predicate) and the member path answered NOTHING — the call compiled,
// and `true.ToString("F4")` rendered "True".
//
// THE RULE (stdlib §2.18). The specifier table's last row is
//
//   | `bool`, `char`, `string` | *(none)* | a format spec on these is a compile error |
//
// so those three have an EMPTY vocabulary, and §2.18's general sentence makes
// "a format spec on any other non-conforming operand" a compile error too. A
// type is formattable iff it is one of the intrinsically formattable
// primitives (the numeric tower), one of the temporal/Guid builtins, or a
// user type that declares `IFormattable` (spec §18.5's opt-in).
//
// WHAT THIS IS **NOT**. This is the TYPE-FORMATTABILITY check, which spec
// §18.5 places at compile time ("the type-formattability check; specifier
// recognition remains the runtime rule"). It does NOT decide whether a
// particular SPECIFIER is recognized: an unrecognized specifier on a
// formattable type raises `FormatException` at RUNTIME (stdlib §2.18, spec
// §18.5), so a caller must not reject `42.ToString("Q")` on the strength of
// this predicate.

#include <string_view>

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// §2.18: the intrinsically-formattable primitives (integers, floating, decimal).
// bool/char/string/Object are NOT formattable — a format spec on them is a
// compile error.
inline bool isIntrinsicallyFormattablePrim(PrimitiveType p) {
    using P = PrimitiveType;
    switch (p) {
        case P::Byte:  case P::SByte: case P::Short: case P::UShort:
        case P::Int:   case P::UInt:  case P::Long: case P::ULong:
        case P::Float: case P::Double: case P::Decimal:
            return true;
        default:
            return false;  // Bool, Char, String, Object, None
    }
}

// The last name component of a Named TypeRef (declaration name, else the
// AST/imported spelling). Empty when unavailable.
inline std::string_view namedTypeName(const TypeRef* t) {
    if (!t || t->kind != TypeRef::Kind::Named) return {};
    if (t->declaration && !t->declaration->name.empty())
        return t->declaration->name;
    if (t->astNode && t->astNode->name && !t->astNode->name->parts.empty())
        return t->astNode->name->parts.back();
    if (!t->importedName.empty()) return t->importedName;
    return {};
}

// §2.18: the intrinsically-formattable temporal + Guid builtins.
inline bool isIntrinsicTemporalOrGuid(std::string_view n) {
    return n == "DateTime" || n == "DateOnly" || n == "TimeOnly" ||
           n == "TimeSpan" || n == "Guid";
}

// §18.5 (v0.172): does `ts` (transitively) declare `: IFormattable`? IFormattable
// is a null-declaration builtin interface, so it never lands in `baseTypes`;
// the walk recurses through resolved base classes / user interfaces for the
// `interface IMyFmt: IFormattable` case.
//
// fix #3139 (§18.5 / §2.5, family F1 of #3152): each level's own conformance
// came from its `ast::TypeDecl` base list, which an IMPORTED type does not
// have — so an imported `class Money: IFormattable` answered false and a
// format spec on it was rejected with "`Money` is neither" while the identical
// local declaration was accepted (measured). `sema::inheritsBuiltinInterface`
// is the same transitive walk over the symbol, on both sides.
inline bool declaresIFormattable(const TypeSymbol* ts) {
    return inheritsBuiltinInterface(ts, "IFormattable");
}

// §18.5 / §2.18 (v0.172): may a hole with a real format spec apply to an operand
// of type `t`? Nullable is transparent (null renders "null"; the spec applies to
// the payload). An unresolved generic type parameter is admitted conservatively
// (its concrete formattability is a bound-analysis question left to codegen).
inline bool isFormattableOperandType(const TypeRef* t) {
    if (!t) return true;  // already errored elsewhere; don't cascade
    if (t->kind == TypeRef::Kind::Primitive)
        return isIntrinsicallyFormattablePrim(t->primitive);
    if (t->kind == TypeRef::Kind::Named) {
        std::string_view n = namedTypeName(t);
        if (isIntrinsicTemporalOrGuid(n)) return true;
        if (t->declaration && declaresIFormattable(t->declaration)) return true;
        // Unresolved generic type parameter (no declaration): admit.
        if (!t->declaration) return true;
        return false;  // plain class/record/enum/interface, Object
    }
    return true;  // non-Named/non-Primitive (unit/function/array): not reached
                  // with a spec in practice; don't spuriously error.
}

}  // namespace bezel::sema
