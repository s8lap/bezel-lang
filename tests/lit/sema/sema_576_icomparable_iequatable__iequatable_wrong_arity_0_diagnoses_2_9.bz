// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/icomparable_iequatable_576_test.cpp
//   (BEZEL_TEST sema_576_icomparable_iequatable, iequatable_wrong_arity_0_diagnoses_2_9).
//
// §2.9: IEquatable<T> with wrong arity (0 args) must diagnose.

module M; class C public _x: IEquatable?; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: generic-argument count does not match built-in type `IEquatable`
