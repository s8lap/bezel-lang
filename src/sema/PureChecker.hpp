#pragma once

// §22.15 (v0.202, proposal 0042, #2052) — the pure BODY checker.
//
// A member declared `pure` promises that evaluating a call to it writes no
// storage outside the call's own frames, performs no I/O, runs no user code
// through disposal, and creates or touches no task/domain/channel/event/
// synchronization object. The closed body rule set (§22.15 "The pure body")
// enumerates exactly what a pure body may contain; everything outside is
// rejected with a diagnostic naming the operation and §22.15.
//
// This pass walks the body of every pure member and enforces the rules. It
// runs AFTER typecheckExpressions, so resolved types and resolved call
// targets are available on the SemaContext.
//
// Slice 2 covers the load-bearing closed-body rules:
//   * the frame-local mutation root rule (stores whose target root leaves
//     the frame are rejected) with the pure-constructor birth-store
//     extension;
//   * the call rule — a call to a member not pure-declared or pure-classified
//     is rejected;
//   * class construction outside the throw carve-out is rejected;
//   * the throw carve-out (`throw new E(args)` where E is trivial-dispose
//     and E's invoked constructor is pure);
//   * the consuming discard;
//   * IEnumerable-source `for` loops (Range of primitive steppable type is
//     the normative form inside a pure body; IEnumerable<T> sources,
//     including Array<T> and string, are rejected).
//
// The signature/placement restrictions (Slice 1) live in
// TypeResolution_Checks::checkPureModifiers.

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"

namespace bezel::sema {

// Walk every pure member's body and enforce the §22.15 closed-body rules.
// Diagnoses violations; returns false if any error was reported.
bool checkPureBodies(const ast::SourceFile& src, SemaContext& ctx);

}  // namespace bezel::sema
