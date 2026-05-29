#pragma once

// Sema slice 2 — Type Resolution.
//
// Walks every `ast::Type` node in the SourceFile (base-type lists, member
// signatures, generic constraint args, local-decl type ascriptions, etc.)
// and resolves it to a `TypeRef`. The resolved TypeRefs are stored in
// `SemaContext::resolvedType(...)` keyed by the AST node pointer.
//
// Lookup discipline (single-compile-unit; cross-module resolution lands
// when the driver supports multi-file builds):
//
//   1. Primitive-type keywords (int, bool, …) resolve to TypeRef::Primitive.
//   2. The unit shape `()` resolves to TypeRef::Unit.
//   3. Inline arrays `[N]T` and `[N owned]T` resolve to TypeRef::InlineArray.
//   4. Function types `(...) -> T` resolve to TypeRef::Function.
//   5. NamedTypes: their first identifier is looked up in (in order)
//      enclosing-method generic params, enclosing-type generic params,
//      enclosing module's top-level types + aliases, the special
//      auto-imported names (Object, etc.). A nested dotted name selects
//      a child of the resolved root.
//
// Diagnostics:
//   - Unresolved type name.
//   - Generic-arity mismatch (Foo<T, U> referenced with one arg).
//
// What's still deferred:
//   - Multi-file / multi-module name lookup. Single compile unit only.
//   - Constraint-clause resolution (sema slice with constraint handling).
//   - Variance compatibility checks (typecheck-time).

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"

namespace bezel::sema {

bool resolveTypes(const ast::SourceFile& src, SemaContext& ctx);

} // namespace bezel::sema
