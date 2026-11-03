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
// — see this fix's commit message): the initializer is an INDEXER read
// `x[0]` on a bare type parameter `T`. §3.6 rule 2 / §14.2: a bare type
// parameter exposes only the members its constraints establish, so sema
// admits the read (a `where` clause could establish an indexer) — but the
// monomorphized clone at `T = int` has no indexer to dispatch to, so
// `lowerExpr` returns null for the index expression and `lowerLocalDecl`'s
// zero-init fallback is what stores the local's value.
//
// ⚠ THE TRIGGER WAS RE-BASED BY fix #7973 (§8.1, v0.350). This case used to
// use `o.Anything()` on an `Object`-typed receiver — the §3/#605 permissive
// "unknown member" acceptance. §8.1 declares the root `Object`'s public
// surface ("The declaration above defines the public surface"), and §26.5
// resolves a member against the receiver's declared type, so that shape is
// now a COMPILE ERROR and never reaches codegen. The subject of this case —
// #2434's local-declaration zero-init fallback — is unchanged; only the
// shape that reaches it moved, to the one legal shape that still does. The
// diagnostic is Severity::Warning, not Error: an unconditional Error would
// reject a legal program.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;
class C
  public method M<T>(x: T): int
    var s := x[0];
    return 0;
  end M;
  public static method Main(): int
    using var c := new C();
    return c.M(1);
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: lowerLocalDecl's initializer expression failed to lower to a value for this local's slot (degraded to a zero-initialized value) — #2434
