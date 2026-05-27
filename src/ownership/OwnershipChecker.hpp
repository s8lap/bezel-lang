#pragma once

// Ownership and borrow checking — Bezel's defining analysis. Every heap object
// must be disposed exactly once; borrows are checked for validity against
// statically tracked lifetimes. Derives from §28 (Resource Management) and the
// compiler-enforced invariants in Appendix C. No analysis is implemented yet.

#include "ast/Node.hpp"
#include "support/Diagnostic.hpp"

namespace bezel {

class OwnershipChecker {
public:
    explicit OwnershipChecker(DiagnosticEngine& diagnostics);

    // Verifies the ownership and borrowing invariants (§28, Appendix C) over a
    // type-checked compilation unit. Returns true when no violations were
    // reported.
    bool check(ast::Node& unit);

private:
    DiagnosticEngine& diagnostics_;
};

} // namespace bezel
