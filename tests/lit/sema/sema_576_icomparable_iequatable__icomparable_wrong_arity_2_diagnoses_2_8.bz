// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/icomparable_iequatable_576_test.cpp
//   (BEZEL_TEST sema_576_icomparable_iequatable, icomparable_wrong_arity_2_diagnoses_2_8).
//
// §2.8: IComparable<T,U> (arity 2) must diagnose.

module M; class C public _x: IComparable<int, int>?; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: generic-argument count does not match built-in type `IComparable`
