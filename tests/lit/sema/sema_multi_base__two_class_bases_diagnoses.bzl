// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/multi_base_test.cpp
//   (BEZEL_TEST sema_multi_base, two_class_bases_diagnoses).

module M; class A end A; class B end B; class C : A, B end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `C` already has a base class; only the first base may be a class (single-inheritance — §11.1)
