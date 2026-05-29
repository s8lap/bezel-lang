#pragma once

// Sema slice 3 — Expression typecheck.
//
// Walks every Expression in the SourceFile (method bodies, property
// bodies, field initializers, default-arg values, etc.) and attaches a
// TypeRef via SemaContext::setExprType. Later passes (ownership,
// definite-assignment) query the inferred type.
//
// Slice 3 scope (this PR):
//
//   - All literal forms: IntLiteralExpr -> int (default), FloatLiteralExpr
//     -> double (default), CharLiteralExpr -> char, StringLiteralExpr
//     / VerbatimStringExpr / InterpolatedStringExpr -> string,
//     BoolLiteralExpr -> bool, NullLiteralExpr -> nullable Object.
//   - UnitExpr -> unit type.
//   - Composite expressions are visited recursively so their leaf
//     literals get types attached, but their own type is the placeholder
//     "Error" sentinel (later slices wire arithmetic, comparison, calls).
//
// Slice 3 deliberately defers:
//
//   - Identifier / member-access resolution (S3b).
//   - Binary / unary operator-result typing (S3c).
//   - Call overload resolution (S3d).
//   - Lambda / case / lambda-body typing (S3e+).

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"

namespace bezel::sema {

bool typecheckExpressions(const ast::SourceFile& src, SemaContext& ctx);

} // namespace bezel::sema
