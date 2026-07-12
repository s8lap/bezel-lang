// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_class_members_test.cpp
//   (BEZEL_TEST sema_static_class, instance_method_in_static_class_diagnoses).

module M; static class Utils public method Inst() _ := 1; end Inst; end Utils;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: static class `Utils` may not declare instance method `Inst`
// CHECK-SAME: (§11.7 — a static class contains only static members)
