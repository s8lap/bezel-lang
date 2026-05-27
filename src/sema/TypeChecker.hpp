#pragma once

// Semantic analysis / type checking. Derives from §3 (Type System), §4
// (Built-in Types), §5 (Type Conversions), §6 (Arithmetic and Overflow), §7
// (Nullability), §8 (The Object Class), and §9–§14 (records, enums, classes,
// inheritance, visibility, generics). No checking is implemented yet.

#include "ast/Node.hpp"
#include "support/Diagnostic.hpp"

namespace bezel {

class TypeChecker {
public:
    explicit TypeChecker(DiagnosticEngine& diagnostics);

    // Checks a compilation unit, reporting type errors through the diagnostic
    // engine. Returns true when no errors were reported.
    bool check(ast::Node& unit);

private:
    DiagnosticEngine& diagnostics_;
};

} // namespace bezel
