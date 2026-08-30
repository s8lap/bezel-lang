#pragma once

// fix #3574 (§11.6 "Nested types", v0.286), promoted for #3597: the ONE
// recursive walk over every type a module declares — each top-level type
// and, transitively, every type nested inside one, pre-order in declaration
// order. Originally a `Resolver`-private pair of functions in
// TypeResolution_Checks.cpp; #3597 needed the identical walk from
// PureChecker.cpp (a different translation unit with no `Resolver`
// instance), so it is promoted here as free functions any pass over
// `SemaContext`/`ModuleSymbol` can share, rather than becoming a SIXTEENTH
// hand-copy (see CLAUDE.md §6, "duplicated classification predicates" — the
// dominant defect generator this walk exists to retire).
//
// Order is PRE-ORDER on purpose — `fn(t)` before the nested members, in
// member declaration order. On a program whose types nest no deeper than one
// level that reproduces the walk every caller had before this promotion, so
// no diagnostic that already fired at depth 0/1 changes position.
//
// The walk deliberately does NOT widen along two axes: a module child that
// is not a `TypeSymbol` (an `ExtensionSymbol` is visited only where a pass
// opts in) and a §21.10 method-local type (which is not a `NestedTypeSymbol`
// member of any type and was never reachable from this walk).

#include <functional>

#include "sema/Symbol.hpp"

namespace bezel::sema {

// Visits `t` pre-order, then recursively every type nested inside it (any
// depth, any nested-type kind — class/record/enum/interface all recurse;
// only what a caller's `fn` PUSHES onto its own result is filtered by kind).
inline void visitTypeAndNested(TypeSymbol* t,
                               const std::function<void(TypeSymbol*)>& fn) {
    if (!t) return;
    fn(t);
    for (Symbol* m : t->members) {
        if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m))
            visitTypeAndNested(nt->type, fn);
    }
}

// Module-level entry point: every top-level type and, transitively, every
// type nested inside one. A caller that also needs a non-`TypeSymbol` module
// child (e.g. `ExtensionSymbol`) walks `module->children` itself and calls
// `visitTypeAndNested` on the `TypeSymbol` arm, so its diagnostics stay
// interleaved in declaration order.
inline void forEachTypeAndNested(ModuleSymbol* module,
                                 const std::function<void(TypeSymbol*)>& fn) {
    if (!module) return;
    for (Symbol* child : module->children) {
        if (auto* t = dynamic_cast<TypeSymbol*>(child))
            visitTypeAndNested(t, fn);
    }
}

}  // namespace bezel::sema
