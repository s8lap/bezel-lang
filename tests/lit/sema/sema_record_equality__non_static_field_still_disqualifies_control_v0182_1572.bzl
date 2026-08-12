// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, non_static_field_still_disqualifies_control_v0182_1572).
//
// Regression control: a NON-static explicit field still disqualifies (§9.2
// unchanged) — only `static` fields get the v0.182 exemption.

module M; record HasNonStaticField public property X: int; public Cache: int := 0; public constructor(x: int) X := x; end constructor; end HasNonStaticField; class C public method M() var a := new HasNonStaticField(1); var b := new HasNonStaticField(1); _ := a = b; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `=` is not available on record `HasNonStaticField`: it is not auto-hashable
// CHECK-SAME: (§9.2/§9.3)
