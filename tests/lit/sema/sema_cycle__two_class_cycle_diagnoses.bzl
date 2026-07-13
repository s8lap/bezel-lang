// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/inheritance_cycle_test.cpp
//   (BEZEL_TEST sema_cycle, two_class_cycle_diagnoses).

module M; class A : B end A; class B : A end B;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: circular inheritance detected at `A` — base-type chain re-enters this type (circular inheritance is ill-formed: the dispatch model of §12 presupposes a well-founded base chain)
