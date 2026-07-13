// Ported from tests/sema/with_target_accessor_visibility_1546_test.cpp
//   (BEZEL_TEST with_target_accessor_visibility_1546, public_property_private_set_with_from_external_accepted).
//
// §9.5 (v0.180) — ACCEPT: `with` targets a `public` auto-property whose
// setter accessor is `private`. The property itself is public (§13.1 is
// satisfied at the member level); `with` never calls the setter, so the
// setter's own restriction does not apply.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record R public property X: int private set; public readonly property Y: int; public constructor(x: int, y: int) X := x; Y := y; end constructor; end R; class C public static method Main(): int var r := new R(1, 2); var r2 := r with X := 42 end; return r2.X; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
