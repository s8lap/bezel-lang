// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/destructor_field_move_763_test.cpp
//   (BEZEL_TEST destructor_field_move_763, destructor_undischarged_owned_local_rejected).
//
// §28.3: the destructor exemption is scoped to the FIELD-restoration rule only;
// the ordinary owned-LOCAL discharge rule still applies inside a destructor — a
// plain `new` owned local that is never discharged is STILL REJECTED.

module M; class Inner public n: int := 0; destructor end destructor; end Inner; class Outer destructor var x := new Inner(); end destructor; public static method Main(): int return 0; end Main; end Outer;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `x` would be dropped here without disposal
// CHECK-SAME: (§28.3 — consider `using var`, explicit `.Dispose()
