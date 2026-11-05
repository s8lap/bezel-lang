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

#include <string>
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

// fix #7377 (§2.18 / §18.5, v0.355): the ONE diagnostic for "this operand has
// no format vocabulary", shared by BOTH spellings.
//
// WHY ONE BUILDER. The two spellings of one rule — the explicit member call
// `x.ToString("F4")` and the interpolation hole `$"{x:F4}"` — are ONE
// OBLIGATION (spec §18.5's "Format dispatch" makes the hole a call to the
// operand's `ToString(format)`), so they must answer with ONE diagnostic.
// Before this fix they answered with TWO DIFFERENT ones: the member spelling
// reported §8.1's ARITY message ("the universal `Object.ToString` member takes
// 0 argument(s) but the call supplies 1") and the hole reported §18.5's
// VOCABULARY message — the reader got a different explanation for the same
// mistake depending on the spelling, and the arity message blamed arity for
// what is a vocabulary fact.
//
// THE TEXT. One sentence, built here so the two call sites cannot drift:
//
//   a format spec requires an intrinsically-formattable operand (a
//   numeric/temporal/Guid) or one implementing `IFormattable`; `E` is neither
//   (§2.18; §18.5)
//
// "operand" is spec §18.5's own word ("A format spec on an OPERAND whose type
// is neither intrinsically formattable nor `IFormattable` is a compile error
// naming this rule"), and the member call's receiver IS the operand the spec
// applies to, so the one sentence reads correctly in both positions. The two
// citations name the two halves of the rule: stdlib §2.18 is the vocabulary
// table (the row that leaves an enum with no specifiers at all) and spec §18.5
// is the dispatch rule that makes the two spellings one obligation.
//
// THE TYPE NAME. The operand's own declared name, in backticks — the primitive
// spelling for a primitive, the declaration's name for a named type — so the
// diagnostic says WHICH type has no vocabulary.

// §2.18 / §18.5: the source spelling of a primitive type, for the shared
// format-vocabulary diagnostic.
inline const char* formatVocabularyPrimitiveName(PrimitiveType p) {
    using P = PrimitiveType;
    switch (p) {
        case P::Bool:    return "bool";
        case P::Char:    return "char";
        case P::String:  return "string";
        case P::Byte:    return "byte";
        case P::SByte:   return "sbyte";
        case P::Short:   return "short";
        case P::UShort:  return "ushort";
        case P::Int:     return "int";
        case P::UInt:    return "uint";
        case P::Long:    return "long";
        case P::ULong:   return "ulong";
        case P::Float:   return "float";
        case P::Double:  return "double";
        case P::Decimal: return "decimal";
        case P::Object:  return "Object";
        case P::None:    return "<none>";
    }
    return "<unknown>";
}

// The operand's name for the shared diagnostic: the primitive spelling or the
// named type's declaration name — BARE, because the MESSAGE adds the
// backticks (so the message's own tail is one contiguous literal). EMPTY when
// the type has no name at all: the reject path cannot reach that case (a type
// with no name is ADMITTED by the predicate — an unresolved generic parameter,
// stdlib §2.18's rule), but the builder stays total.
inline std::string formatVocabularyOperandName(const TypeRef* t) {
    if (t && t->kind == TypeRef::Kind::Primitive)
        return std::string(formatVocabularyPrimitiveName(t->primitive));
    if (t) {
        std::string_view n = namedTypeName(t);
        if (!n.empty()) return std::string(n);
    }
    return std::string();
}

// §2.18 / §18.5: the ONE diagnostic a type with no format vocabulary gets,
// from either spelling.
//
// THE TAIL IS ONE LITERAL ("` is neither (§2.18; §18.5)") so that the text the
// compiler emits is one contiguous string in the source, and a needle written
// against it names the whole tail. The § citations are the raw UTF-8 section
// signs — the same spelling the message this one REPLACED used, so the text
// did not change when the message MOVED from the member path to this shared
// builder.
inline std::string formatVocabularyRejectMessage(const TypeRef* t) {
    std::string who = formatVocabularyOperandName(t);
    if (who.empty())
        return "a format spec requires an intrinsically-formattable operand "
               "(a numeric/temporal/Guid) or one implementing `IFormattable`; "
               "this operand is neither (§2.18; §18.5)";
    return "a format spec requires an intrinsically-formattable operand "
           "(a numeric/temporal/Guid) or one implementing `IFormattable`; `" +
           who + "` is neither (§2.18; §18.5)";
}

}  // namespace bezel::sema
