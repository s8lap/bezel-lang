// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_assignment_747_test.cpp
//   (BEZEL_TEST const_assignment_747, this_receiver_rejected).
//
// §10: assignment through `this.K` inside a method is rejected.

module M; class C public const K: int := 100; public method M() this.K := 5; end M; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to const member `K` — a const is a compile-time constant
