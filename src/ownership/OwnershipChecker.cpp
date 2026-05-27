#include "ownership/OwnershipChecker.hpp"

namespace bezel {

OwnershipChecker::OwnershipChecker(DiagnosticEngine& diagnostics)
    : diagnostics_(diagnostics) {}

bool OwnershipChecker::check(ast::Node& unit) {
    (void)unit;
    // TODO(ownership milestone): implement ownership and borrow checking per
    // §28 and Appendix C.
    return !diagnostics_.hasErrors();
}

} // namespace bezel
