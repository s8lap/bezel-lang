#pragma once

// Proposal 0082 §3 — the *scoped await-bind*, stage-1 PROTOTYPE (issue #3048).
//
// WHAT THIS IS. Proposal 0082 asks that a Pattern A (`Task<T>`, class T) bind
//
//     var cfg := await FetchConfigAsync(url);      // a §30.5 compile error today
//
// be admitted by making the compiler write the two-line form itself:
//
//     using var $t := FetchConfigAsync(url);       // the hidden scope-pinned slot
//     var cfg := await $t;
//
// The proposal's §3 states the desugaring *is literal* ("`var x := await E;` ⇒
// `using var $t := E; var x := await $t;` with `$t` fresh and inexpressible"),
// and its adoption gate C3 asks a compiler question: **is the hidden binding the
// §28.10 cleanup-stack slot and nothing else** — no new analysis, no consume-
// site-freeze extension (§28.3), no checked-disposal-mode change (§28.2)?
//
// HOW THIS PROTOTYPE ANSWERS IT. By construction, and in the only way that can
// answer it honestly: the desugaring is performed as an **AST-to-AST macro
// expansion into today's language**, before the front-end that enforces §28.10
// runs. Every downstream pass — type resolution, expression typecheck, definite
// assignment, the ownership pass, codegen's cleanup-stack registration, and the
// checked-disposal instrumentation — sees an ordinary `using var` local it
// already knows how to handle, and is COMPILED FROM UNMODIFIED SOURCES. If the
// rewritten program compiles and behaves exactly like the hand-written two-line
// form, C3 holds; if any downstream pass had to learn a new concept, the
// prototype could not have been written this way at all.
//
// THE TRIGGER IS THE EXISTING REJECTION. The set of declarations rewritten is
// not computed by any new classification. It is exactly the set of sites at
// which the *unmodified* ownership pass already reports the §30.5 Pattern A
// rejection (Ownership_Checks.cpp `checkAwaitSitesExpr`) for a `var`/`const`
// LocalDecl whose initializer is exactly the await: the driver runs the
// front-end once over a throwaway DiagnosticEngine, keeps the source locations
// that diagnostic points at, and hands them here. Proposal §1's claim — "the
// rule introduces no new enumeration, only re-points an existing one" — is
// therefore literally true of this implementation.
//
// SCOPE (proposal §1/§2 boundaries the prototype honours):
//   - the initializer must be EXACTLY the await (a composition such as
//     `(await E).Name` never reaches a binding-position rejection, so it can
//     never appear in the site list);
//   - the operand must be an unbound owned temporary — the compiler's existing
//     `isUnboundProducerExpr` shape, i.e. a `CallExpr` or a `NewExpr`. The
//     §30.5 diagnostic alone does not distinguish it (it also fires for, e.g.,
//     `await (a ?? b)`), so this one shape test is re-stated below;
//   - `using var x := await E;` stays rejected (§2: the await produces a
//     borrow, `using var` requires an owned RHS) — `isUsing` decls are skipped;
//   - `x := await E;` (assignment to an existing local) stays rejected — an
//     AssignmentStmt has no LocalDecl to rewrite, so it can never match.
//
// This header is deliberately outside `src/sema`, `src/ownership`, `src/codegen`
// and `src/runtime`: the prototype changes none of them, and keeping the file
// here is part of the C3 evidence.

#include <cstddef>
#include <vector>

#include "ast/Node.hpp"
#include "support/SourceLocation.hpp"

namespace bezel::proto {

// The distinctive prefix of the §30.5 Pattern A consumer-side rejection emitted
// by the ownership pass (src/ownership/Ownership_Checks.cpp). The driver's
// site collector keys on it; a change to that message text makes the prototype
// stop firing, which its regression tests detect immediately.
extern const char kPatternARejectPrefix[];

// Rewrite every `var`/`const` local declaration whose initializer is the await
// at one of `awaitLocs` into the proposal §3 two-statement form. Returns the
// number of declarations rewritten. `unit` is mutated in place; the synthesized
// slot name is unnameable (it contains `$`, which no Bezel identifier may
// contain — §1.3 / App. A), so no program text can read, move, re-await, or
// dispose it.
std::size_t rewriteScopedAwaitBinds(ast::SourceFile& unit,
                                    const std::vector<SourceLocation>& awaitLocs);

} // namespace bezel::proto
