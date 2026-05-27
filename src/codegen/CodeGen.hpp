#pragma once

// Code generation.
//
// OPEN DECISION — read before implementing: the language spec defines the
// source language but does NOT specify a compilation target or backend. §32
// (Foreign Function Interface) and §33 (Program Entry Point) constrain how
// generated code interoperates and starts, but the target itself (native via
// LLVM, a custom IR, C output, a bytecode VM, …) is unspecified. Choosing one
// is a project-level decision and is out of scope for autonomous work per
// CLAUDE.md; it should be settled by an issue before this stub grows a body.

#include "ast/Node.hpp"
#include "support/Diagnostic.hpp"

namespace bezel {

class CodeGen {
public:
    explicit CodeGen(DiagnosticEngine& diagnostics);

    // Emits code for a checked compilation unit. A no-op until the backend is
    // chosen (see the note above).
    bool emit(ast::Node& unit);

private:
    DiagnosticEngine& diagnostics_;
};

} // namespace bezel
