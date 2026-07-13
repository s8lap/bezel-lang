// Ported from tests/sema/with_target_accessor_visibility_1546_test.cpp
//   (BEZEL_TEST with_target_accessor_visibility_1546, public_property_private_set_with_from_declaring_type_accepted).
//
// §9.5 (v0.180) — ACCEPT (control): the same target from inside the
// declaring type, where both the property and the setter would be visible
// regardless.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record R public property X: int private set; public readonly property Y: int; public constructor(x: int, y: int) X := x; Y := y; end constructor; public method WithX(newX: int): R return this with X := newX end; end WithX; end R; class C public static method Main(): int var r := new R(1, 2); var r2 := r.WithX(42); return r2.X; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
