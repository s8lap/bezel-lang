#pragma once

// Sema slice 1 — Symbol Collection.
//
// Walks a parsed SourceFile and registers every named entity in the
// SemaContext: the module, top-level types + aliases + extensions + extern
// blocks, type members (fields, methods, properties, indexers, events,
// const decls, constructors, destructors), nested types, generic params,
// and method/ctor parameters.
//
// What this pass deliberately does NOT do (deferred to later sema slices):
//
//   - Resolve base-type references (slice 2 Type Resolution).
//   - Resolve member return / param types (slice 2).
//   - Type-check method bodies; the body AST is intentionally untouched.
//   - Look up imports against an external symbol catalogue (single-compile-
//     unit only for now per the implementation plan).
//   - Validate member uniqueness, modifier-combo validity, or any other
//     spec rule that requires resolved types. Sema later passes own those.
//
// Diagnostics emitted by this slice:
//
//   - Missing ModuleDecl (a SourceFile must start with `module X;`) —
//     parser already enforces, but sema reports if AST is malformed.

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"

namespace bezel::sema {

// Run symbol collection over a parsed SourceFile. Returns true when no
// errors were reported. Records every symbol in `ctx` and sets the
// SourceFile's module symbol on the context.
bool collectSymbols(const ast::SourceFile& src, SemaContext& ctx);

// §21.10: collect a local (method-body) type declaration's symbol into
// SemaContext. Used by ExprTC::visitStmt when walking method bodies.
// `parentScope` is the ScopeId of the enclosing region (a fresh
// ScopeId allocated by the caller for method-body-lifetime types).
TypeSymbol* collectTypeDecl(const ast::TypeDecl& td, SemaContext& ctx,
                            ScopeId parentScope,
                            std::string_view enclosingTypePath = {});

} // namespace bezel::sema
