// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_iface_property_489_test.cpp
//   (BEZEL_TEST sema_field_iface_property_489, iface_method_arity_mismatch_diagnoses_r39).

module M; interface ICalc method Add(x: int, y: int): int; end ICalc; class Calc : ICalc public method Add(x: int): int return x; end Add; end Calc;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Calc` does not implement inherited member `Add` with the required 2-parameter signature — the implementing method's parameter count must match the abstract/interface member exactly
// CHECK-SAME: (§11.2 / §12.1)
