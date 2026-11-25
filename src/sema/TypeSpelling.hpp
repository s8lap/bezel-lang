#pragma once

// The spelling of a resolved type reference, for diagnostics that name a
// CONSTRUCTED type.
//
// fix #9052 (§14.2 *Constraints* / §14.1 *Instantiation and its site*,
// v0.359): the §14.2 constraint diagnostic names the bound the constraint
// states AT THE INSTANTIATION SITE. A bound is read there AFTER the
// instantiation's own type arguments are substituted into it (§14.1: "An
// instantiation of a generic declaration is the constructed type or
// constructed member obtained by substituting type arguments for its type
// parameters"), so the name the diagnostic carries is `IEntity<int>` — the
// SUBSTITUTED bound. The head-name-only spelling drops the type argument, and
// that is what issue #9052 reports.
//
// ONE spelling for BOTH constraint sites — the type-instantiation site
// (`Resolver::checkOneConstraint`, src/sema/TypeResolution_Checks.cpp) and the
// generic-method call site (`ExprTC::checkMethodGenericConstraints`,
// src/sema/ExprTypecheck_Calls.cpp) — so the two cannot drift. The file-local
// spellers those files already carry (`spellReturnTypeForDiag`,
// `spellTypeRefForDiag`) answer other questions (a return type, a ref/out
// unification operand) and are left alone.
//
// It lives here rather than in `TypeRef.hpp` because a Named TypeRef's
// DECLARATION is a `TypeSymbol*`, and `TypeSymbol` is only complete in
// `Symbol.hpp` — `TypeRef.hpp` sees it forward-declared.

#include <string>

#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// `IEntity<int>`, `Dictionary<string, int>`, `int?`, `[2]int`, `unit` — the
// resolved reference's own spelling, WITH its type arguments.
inline std::string spellTypeRefWithArgs(const TypeRef* t) {
    if (!t) return "?";
    switch (t->kind) {
        case TypeRef::Kind::Primitive: {
            using P = PrimitiveType;
            const char* n = "?";
            switch (t->primitive) {
                case P::Bool:    n = "bool";    break;
                case P::Char:    n = "char";    break;
                case P::String:  n = "string";  break;
                case P::Byte:    n = "byte";    break;
                case P::SByte:   n = "sbyte";   break;
                case P::Short:   n = "short";   break;
                case P::UShort:  n = "ushort";  break;
                case P::Int:     n = "int";     break;
                case P::UInt:    n = "uint";    break;
                case P::Long:    n = "long";    break;
                case P::ULong:   n = "ulong";   break;
                case P::Float:   n = "float";   break;
                case P::Double:  n = "double";  break;
                case P::Decimal: n = "decimal"; break;
                case P::Object:  n = "Object";  break;
                default:         n = "?";       break;
            }
            std::string s(n);
            if (t->nullable) s += '?';
            return s;
        }
        case TypeRef::Kind::Named: {
            std::string s = t->declaration ? std::string(t->declaration->name)
                                           : std::string(t->headName());
            if (s.empty()) s = "?";
            if (!t->genericArgs.empty()) {
                s += "<";
                for (std::size_t i = 0; i < t->genericArgs.size(); ++i) {
                    if (i) s += ", ";
                    s += spellTypeRefWithArgs(t->genericArgs[i]);
                }
                s += ">";
            }
            if (t->nullable) s += '?';
            return s;
        }
        case TypeRef::Kind::InlineArray: {
            std::string s = std::string("[") +
                            (t->arraySizeValue >= 0
                                 ? std::to_string(t->arraySizeValue)
                                 : std::string("N")) +
                            (t->arrayOwned ? " owned]" : "]");
            s += spellTypeRefWithArgs(t->arrayElement);
            return s;
        }
        case TypeRef::Kind::Unit: return "unit";
        default: return "?";
    }
}

}  // namespace bezel::sema
