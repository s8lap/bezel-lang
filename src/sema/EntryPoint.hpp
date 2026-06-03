#pragma once

// §33 Program Entry Point — resolution of the executable's `Main` method.
//
// This check runs only for *executable* builds (the driver invokes it when an
// output executable is requested). It verifies §33.5: exactly one method in
// the program qualifies as the entry point. Zero qualifying `Main` methods or
// more than one qualifying `Main` is a compile error. The eligibility rule is
// §33.2 (the eight accepted signatures); a method named `Main` that does not
// match a qualifying signature is an ordinary method and is ignored here.
//
// The check is deliberately *not* part of the shared sema pipeline
// (collectSymbols / resolveTypes / typecheckExpressions): library compilation
// and the test harnesses run that pipeline on snippets that legitimately omit
// `Main`, and they must not be failed by an entry-point requirement (§33.6:
// "A library module has no entry point"). Scoping the check to the driver's
// executable path keeps those callers unaffected.

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"

namespace bezel::sema {

// per §33.5: verify the program has exactly one qualifying entry point.
// Reports a diagnostic (and returns false) when zero or more than one
// qualifying `Main` is found; returns true when exactly one is found.
// Intended to be called by the driver for executable builds only.
bool checkEntryPoint(const ast::SourceFile& src, SemaContext& ctx);

}  // namespace bezel::sema
