#include "sema/TypeChecker.hpp"

namespace bezel {

TypeChecker::TypeChecker(DiagnosticEngine& diagnostics)
    : diagnostics_(diagnostics) {}

bool TypeChecker::check(ast::Node& unit) {
    (void)unit;
    // TODO(sema milestone): implement type checking per §3–§14.
    return !diagnostics_.hasErrors();
}

} // namespace bezel
