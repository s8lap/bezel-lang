// §2.5 / §22.13 (#797): compile-time detection of circular static-initialization
// dependencies. The spec (§2.5): "Circular type-level dependencies between
// modules are permitted. Circular static-initialization dependencies are not;
// the compiler detects and rejects them."
//
// This pass builds the DIRECT static-initialization dependency graph — a type T
// depends on type U when T's static field initializers or static-constructor body
// directly reference a static member (field, method, or property) of U — and
// rejects any cycle with a diagnostic. Run after typecheckExpressions (so
// exprSymbol is populated). Direct references only: a dependency reached only
// THROUGH a called method's body is a (safe) false negative — never a false
// positive — and is a documented follow-up.

#pragma once

namespace bezel::ast { struct SourceFile; }
namespace bezel::sema { class SemaContext; }

namespace bezel::sema {

// Returns true iff no circular static-initialization dependency was found.
// Reports an Error diagnostic for each detected cycle.
bool checkStaticInitCycles(const ast::SourceFile& src, SemaContext& ctx);

}  // namespace bezel::sema
