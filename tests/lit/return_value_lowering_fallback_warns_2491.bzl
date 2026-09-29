// fix #2491 (codegen, emitReturn/arrow-body/expression-body/getter return
// paths, v0.236): return-path twin of #2434. When a return statement's value
// expression fails to lower (lowerExpr returns null), emitReturn's
// `v ? v : null` fallback silently substitutes a zero/null return — the
// correct degrade (definite assignment / §28.3 ownership make the zero
// safe), but completely SILENT, exactly the gap #2434 closed for the
// local-decl twin. This pins the fix: the return path now emits an internal
// Warning naming the failed lowering, so the case is observable instead of
// invisible.
//
// ACCEPT-WITH-WARNING control (mirrors tests/lit/
// local_decl_zero_init_fallback_warns_2434.bzl's pattern): bezelc -c must
// still exit zero — the program IS legal — but it now emits a diagnostic
// where before this fix it emitted none at all.
//
// WHY THIS PROGRAM REACHES THE FALLBACK (verified empirically, not assumed
// — same shape #2434 proved for the local-decl twin): `o.Anything()` is a
// member access on an `Object`-typed receiver, whose static type carries no
// known-complete member set (§3 / #605's permissive "unknown member"
// acceptance) — sema accepts it BY DESIGN, but codegen has no concrete
// member to dispatch to, so `lowerExpr` returns null for the CallExpr and
// the return path's zero/null fallback is what the method returns. This is
// why the new diagnostic is Severity::Warning, not Error: an unconditional
// Error would reject this (and the sibling shapes) — turning a currently-
// legal compile into a hard failure, exactly the false-reject #2434 warned
// against checking for before choosing Error. The IR for this method
// confirmed the silent degrade pre-fix: `define ptr @"bz$M.C$M"(...) {
// entry: ... ret ptr null }` — a literal null return with ZERO diagnostic
// output, while the local-decl twin `var s := o.Anything();` in the same
// program DID print #2434's warning for the identical underlying lowerExpr
// failure (the loud-vs-silent inconsistency between the two twins was
// itself load-bearing evidence while diagnosing #2474, per #2491's report).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public method M(o: Object): Object return o.Anything(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: return-value expression failed to lower to a value (degraded to a zero/null return value) — #2491
