// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/property_override_signature_1614_test.cpp
//   (BEZEL_TEST sema_prop_override_1614, dropped_setter_diagnoses).
//
// §12.1 / #1614: an override that DROPS the base's setter (base get+set,
// override get-only) is rejected — it changes the vtable accessor-slot shape.

module M; class Base private _x: int := 0; public virtual property X: int get => _x; set(v) => _x := v; end; end Base; class D : Base public override property X: int => 7; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `X` does not match the base member's accessor set/get presence
// CHECK-SAME: (§12.1)
