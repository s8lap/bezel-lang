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
// — same shape #2434 proved for the local-decl twin): the returned
// expression is an INDEXER read `x[0]` on a bare type parameter `T`. §3.6
// rule 2 / §14.2: a bare type parameter exposes only the members its
// constraints establish, so sema ADMITS the read (a `where` clause could
// establish an indexer) — but the monomorphized clone at `T = int` has no
// indexer to dispatch to, so `lowerExpr` returns null for the index
// expression and the return path's zero/null fallback is what the method
// returns. This is why the new diagnostic is Severity::Warning, not Error:
// an unconditional Error would reject a legal program — turning a
// currently-legal compile into a hard failure, exactly the false-reject
// #2434 warned against checking for before choosing Error.
//
// ⚠ THE TRIGGER WAS RE-BASED BY fix #7973 (§8.1, v0.350). This case used to
// use `o.Anything()` on an `Object`-typed receiver — the §3/#605 permissive
// "unknown member" acceptance. §8.1 declares the root `Object`'s public
// surface ("The declaration above defines the public surface"), and §26.5
// resolves a member against the receiver's declared type, so that shape is
// now a COMPILE ERROR and never reaches codegen. The subject of this case —
// #2491's return-path degrade — is unchanged; only the shape that reaches it
// moved, to the one legal shape that still does.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public method M<T>(x: T): Object return x[0]; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: return-value expression failed to lower to a value (degraded to a zero/null return value) — #2491
