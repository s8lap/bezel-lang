// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/property_override_signature_1614_test.cpp
//   (BEZEL_TEST sema_prop_override_1614, added_setter_diagnoses).
//
// §12.1 / #1614: an override that ADDS a setter the get-only base lacks is
// likewise rejected (the accessor sets must match).

module M; class Base public virtual property X: int => 0; end Base; class D : Base private _x: int := 0; public override property X: int get => _x; set(v) => _x := v; end; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `X` does not match the base member's accessor set/get presence
// CHECK-SAME: (§12.1)
