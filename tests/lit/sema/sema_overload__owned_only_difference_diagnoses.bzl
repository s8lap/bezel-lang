// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// FLIPPED FROM REJECT TO ACCEPT for #4939 (§22.6, v0.250, proposal 0091 §5).
//
// Per proposal 0091 §5: "One genuine flip — `sema_overload__owned_only_difference_diagnoses.bzl`
// (a concrete class-typed pair) becomes an admitted-pair test." The
// ordinary-`owned` vs plain pair on a concrete class type passes the §22.6
// declaration guard: every substitution of `T` (a class) keeps the marker
// (proposal 0091 §1: `where T: class` does NOT qualify, but a CONCRETE-class
// `T` parameter with NO constraint set does qualify — the position is just
// a class type). The four REJECT siblings (sibling lit files) cover the
// standing halves: `out owned`/plain clash, `scoped`/plain clash,
// unconstrained-type-parameter collapse (proposal 0091 §1 erasure-test
// declaration guard), and identical-signature clash.
//
// Was: tests/sema/overload_signature_test.cpp (BEZEL_TEST sema_overload,
// owned_only_difference_diagnoses). Now: ACCEPT.
//
// per §22.6 / §11.3 / §28.1 / proposal 0091 §1 / §5
// (issue #4939, spec v0.250 / v0.307)

module M; class T end T; class C public method F(t: T) _ := 1; end F; public method F(owned t: T) _ := 2; end F; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
