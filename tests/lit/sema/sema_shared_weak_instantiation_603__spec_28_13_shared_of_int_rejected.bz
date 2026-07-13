// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/shared_weak_instantiation_603_test.cpp
//   (BEZEL_TEST sema_shared_weak_instantiation_603, spec_28_13_shared_of_int_rejected).
//
// §28.13 (must-fail): Shared<int> — T is a primitive, not a class.

module M; class C public static method M() using var h := new Shared<int>(0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Shared<T> requires T to be a proper class type: `int` is not a class — runtime-managed and value types have no disposal obligation to share
// CHECK-SAME: (§28.13 / stdlib §2.16, #603)
