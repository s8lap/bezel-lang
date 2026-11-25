#pragma once

// stdlib §5 (v0.359): the ELEMENT type of an `IGrouping<TKey, T>` TypeRef —
// genericArgs[1], NOT genericArgs[0].
//
// `IGrouping<TKey, T> : IEnumerable<T>` (stdlib §5): the grouping's element
// is T, the SECOND generic argument; the FIRST is the KEY type. Every other
// enumerable the compiler recognises exposes its element at genericArgs[0], so
// a reader that takes genericArgs[0] blindly reads TKey.
//
// WHY THIS PREDICATE IS SHARED. Three sema sites ask the SAME question —
// "what is the ELEMENT type of this enumerable-shaped TypeRef?" — and must
// answer it identically:
//
//   * `ExprTypecheck.cpp`'s `for`-in loop-variable inference (#6627);
//   * `ExprTypecheck_Calls.cpp`'s `linqSourceElementType` — the LINQ
//     lambda-parameter seeding (spec §27.4); and
//   * `ExprTypecheck_Calls.cpp`'s `SelectMany` result-element computation —
//     the INNER sequence's element type (stdlib §5).
//
// Spelling the recognizer twice is how those sites drift: #6627 taught the
// `for`-in site and the LINQ RESULT-type arm to read genericArgs[1], while
// the lambda-parameter seeding and the `SelectMany` inner element kept
// reading genericArgs[0] — the KEY (#8542). So the recognizer lives HERE.
//
// The three shapes an `IGrouping` TypeRef reaches sema in mirror codegen's
// `Emitter::iGroupingElemTypeRef` (LlvmBackend_Calls3.cpp): a declaration-less
// astNode (the synthetic node the GroupBy typing arms build), a builtin
// TypeSymbol whose `decl` is null (a user-written `IGrouping<K, V>`
// annotation), or an `importedName` set by ModuleLoader.

#include <string_view>

#include "sema/Symbol.hpp"  // TypeSymbol — the builtin IGrouping interface's stub
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// The ELEMENT type of `rt` when `rt` names an `IGrouping<TKey, T>`; nullptr
// for every other type — including a USER's own type named `IGrouping` (a
// real user type carries a `decl`, and only the compiler's builtin stub does
// not).
inline TypeRef* iGroupingElemTypeRef(TypeRef* rt) {
    if (!rt || rt->kind != TypeRef::Kind::Named) return nullptr;
    std::string_view hn;
    if (rt->declaration) {
        if (rt->declaration->decl) return nullptr;  // a real user type
        hn = rt->declaration->name;
    } else if (rt->astNode && rt->astNode->name &&
               !rt->astNode->name->parts.empty()) {
        hn = rt->headName();
    } else if (!rt->importedName.empty()) {
        hn = rt->importedName;
    } else {
        return nullptr;
    }
    if (hn != "IGrouping") return nullptr;
    if (rt->genericArgs.size() < 2 || !rt->genericArgs[1]) return nullptr;
    return rt->genericArgs[1];
}

}  // namespace bezel::sema
