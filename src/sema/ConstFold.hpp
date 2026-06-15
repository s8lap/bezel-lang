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

}  // namespace bezel::sema
