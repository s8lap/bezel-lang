// Ported from tests/sema/with_visibility_934_test.cpp
//   (BEZEL_TEST with_visibility_934, public_property_with_from_external_accepted).
//
// §13.1, #934 — ACCEPT: `with` targets a PUBLIC property from an external
// class. Public members are always accessible (§13.1). No over-gating.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record R public property V: int; public constructor(v: int) V := v; end constructor; end R; class C public static method Main(): int var r := new R(1); var r2 := r with(V := 99); return r2.V; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
