// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_class_members_test.cpp
//   (BEZEL_TEST sema_static_class, instance_field_in_static_class_diagnoses).

module M; static class Utils public _x: int := 0; end Utils;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: static class `Utils` may not declare instance field `_x`
// CHECK-SAME: (§11.7 — a static class contains only static members)
