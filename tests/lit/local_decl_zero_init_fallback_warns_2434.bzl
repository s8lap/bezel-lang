// fix #2434 (codegen, LlvmBackend_Setup.cpp Emitter::lowerLocalDecl, v0.235):
// follow-up to #2423's zero-init defense-in-depth for a local slot whose
// initializer failed to lower (`v == nullptr`). That fallback was SILENT —
// reaching it produced a deterministic zero with no signal at all. This
// pins the fix: an initializer-present case (as opposed to a plain
// `var x: int;` declaration-only local, which stays silent — that path is
// common and legitimate under definite assignment, Appendix C.1) now emits
// an internal Warning naming the slot, so the case is observable instead of
// invisible.
//
// ACCEPT-WITH-WARNING control (mirrors tests/lit/sema/sema_enum__static_
// owned_readonly_field_ok_1564.bzl's pattern): bezelc -c must still exit
// zero — the program IS legal — but it now emits a diagnostic where before
// this fix it emitted none at all.
//
// WHY THIS PROGRAM REACHES THE FALLBACK (verified empirically, not assumed
// — see this fix's commit message): `o.Anything()` is a member access on an
// `Object`-typed receiver, whose static type carries no known-complete
// member set (§3 / #605's permissive "unknown member" acceptance) — sema
// accepts it BY DESIGN (this is byte-for-byte the source text of
// tests/lit/sema/sema_unknown_member__object_typed_receiver_permissive.bzl,
// reused here), but codegen has no concrete member to dispatch to, so
// `lowerExpr` returns null for the CallExpr and `lowerLocalDecl`'s
// zero-init fallback is what stores the local's value. This is why the new
// diagnostic is Severity::Warning, not Error: an unconditional Error would
// reject this (and the other 5 corpus programs sharing this shape) —
// turning a currently-legal compile into a hard failure, exactly the
// false-reject #2434 warned against checking for before choosing Error.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public method M(o: Object): int var s := o.Anything(); return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: lowerLocalDecl's initializer expression failed to lower to a value for this local's slot (degraded to a zero-initialized value) — #2434
