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

// fix #4865 / §17.14 / §22.12 (v0.307): rejects cyclic class-level `const`
// initializer references (e.g. `K1 := K2; K2 := K1`). Without detection
// codegen's `constMemberValue` re-lowers the referenced const's initializer
// and recurses unboundedly into the same pair until the process stack
// overflows (SIGSEGV). Reports an Error diagnostic for each detected cycle;
// the codegen path keeps a visited-set backstop so a pre-fix-style crash
// cannot resurface if a future fold change reopens a cycle here.
bool checkConstInitCycles(const ast::SourceFile& src, SemaContext& ctx);

}  // namespace bezel::sema
