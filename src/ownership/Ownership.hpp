#pragma once

// Ownership and borrow checking — Bezel's defining analysis (§28).
//
// Slice O1 — Linear owned-local lifecycle (this file's contract):
//
//   - For each method/ctor body in a SourceFile, walk the statements
//     linearly and track each LocalDecl's ownership state.
//   - Producer detection (initial owned state of a `var x := EXPR;`):
//       * `new T(...)` -> Owned
//       * Everything else -> Borrowed (slice O1 doesn't try harder)
//   - `using var x := EXPR;` carries implicit discharge — no error at
//     scope exit.
//   - `var x := EXPR;` carrying Owned state must reach one of:
//       * `x.Dispose()` (member-call detected syntactically)
//       * `move x` (unary move on the bare identifier)
//       * `return x` under any return type (slice O1 doesn't yet
//         enforce that the method's return type is `owned T` — that
//         restriction is sema's, and is on the deferred list).
//     Failing to reach any of these by scope end is a diagnostic.
//
// What's deferred to later O slices:
//
//   - Branch joins (O4) — multi-path merges with owned/borrowed conflict.
//   - Borrow lifetime checks (O7).
//   - `move` semantics on fields (O8).
//   - Owned-field discharge in synthesized destructor (O9).
//   - Owned parameter discharge (O10).
//   - Exception-path implicit auto-dispose (O11).
//   - Owning-collection / inline-array slot move (O12).

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"

namespace bezel::ownership {

// Runs ownership checking over every method/ctor body in the source
// file. Reports diagnostics through the SemaContext's diagnostic engine.
// Returns true when no errors were emitted.
bool checkOwnership(const ast::SourceFile& src, sema::SemaContext& ctx);

// fix #7968 (§26.7 / §28.3 / §28.9, v0.355): the extension-receiver STORE
// check. §26.7: "The receiver is always a borrow; extensions cannot store
// `this`, return it as `owned`, or dispose it." An extension body may not
// store its receiver into a member slot. (The CALL-ARGUMENT shape — handing
// the receiver to a plain formal a callee may store — is §28.3 clause 3's
// residual, which the conformance corpus admits twice: the cases
// s28_3_0119 and s8_6_0011.) The implementation lives in
// src/ownership/Ownership_Extension.cpp.
void checkExtensionReceiverStore(const ast::MethodDecl& md,
                                 const ast::ExtensionDecl& ext,
                                 sema::SemaContext& ctx);

} // namespace bezel::ownership
