#pragma once

// §15.1 / §22.12: compile-time integer constant folding shared by the passes
// that need to evaluate an integer-valued constant expression — inline-array
// sizes (`[N]T`, type resolution) being the first consumer. Kept as a free
// function taking an identifier-resolution callback so each caller supplies its
// own name lookup (member consts on the enclosing type, locals in the lexical
// scope, …) while the structural arithmetic logic lives in exactly one place.

#include <functional>
#include <optional>

#include "ast/Node.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// Fold `e` to a compile-time `long long`. Admits integer literals, unary
// `+ - ~`, binary arithmetic/bitwise (`+ - * / % << >> & | ^`), and bare
// identifiers — the latter resolved through `resolveIdent(name, depth)`, which
// returns the identifier's integer value or nullopt when the name is not an
// integer-valued compile-time constant. `resolveIdent` should thread `depth`
// back into a nested foldIntConst call when it recurses into a const's
// initializer, so the shared depth guard bounds cyclic const references.
// Returns nullopt for any expression that is not a foldable integer constant.
std::optional<long long> foldIntConst(
    const ast::Expr& e,
    const std::function<std::optional<long long>(std::string_view, int)>&
        resolveIdent,
    int depth = 0);

// §17.14 (v0.184, #1688): does the compile-time-constant value `v` fit the
// representable range of integer-family primitive type `p`? Shared by every
// const evaluation-fault check (class-level and local `const` alike) so a
// folded value that overflows its declared/inferred type — `const B :=
// 2147483647 + 1;` folds to 2147483648 in `foldIntConst`'s unchecked 64-bit
// arithmetic, which does not fit `int` — is reported the same way everywhere.
// `p` must be one of the eight integer-family PrimitiveTypes; Long/ULong
// admit any 64-bit pattern (the literal/fold path already bounds them to 64
// bits), so they always fit.
bool fitsConstIntRange(long long v, PrimitiveType p);

// §17.14 (v0.184, #1688): does `e` contain a `/` or `mod` node whose RHS
// folds (via `resolveIdent`) to a compile-time zero? Called when
// `foldIntConst` on the whole expression returns nullopt, to distinguish
// "division/modulo by zero" (a specific evaluation fault, §17.14 — a compile
// error at the constant expression, never deferred to runtime) from "not
// structurally a constant expression" (a different diagnostic, e.g.
// unresolved names). `resolveIdent` should be the SAME resolver passed to
// the sibling `foldIntConst` call so both walks see consistent const values.
bool containsConstDivByZero(
    const ast::Expr& e,
    const std::function<std::optional<long long>(std::string_view, int)>&
        resolveIdent,
    int depth = 0);

}  // namespace bezel::sema
