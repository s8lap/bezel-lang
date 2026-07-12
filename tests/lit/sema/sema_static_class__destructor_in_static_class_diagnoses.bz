// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_class_members_test.cpp
//   (BEZEL_TEST sema_static_class, destructor_in_static_class_diagnoses).

module M; static class Utils public static method M() _ := 1; end M; destructor end destructor; end Utils;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: static class `Utils` may not declare a destructor
// CHECK-SAME: (§11.7)
