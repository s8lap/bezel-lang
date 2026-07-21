// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_iface_property_489_test.cpp
//   (BEZEL_TEST sema_field_iface_property_489, iface_property_type_mismatch_diagnoses_r39).

module M; interface IVal readonly property Name: string; end IVal; class Impl : IVal public property Name: int => 42; end Impl;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: property `Name` implements interface `IVal` with a different type than the interface declares — property types must match exactly (no covariance)
// CHECK-SAME: (§11.2)
