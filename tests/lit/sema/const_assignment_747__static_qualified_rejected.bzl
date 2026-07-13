// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_assignment_747_test.cpp
//   (BEZEL_TEST const_assignment_747, static_qualified_rejected).
//
// §10: a type-qualified static const `C.K` is rejected.

module M; class C public static const K: int := 100; public static method Main(): int C.K := 5; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to const member `K` — a const is a compile-time constant
