// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/local_const_eval_fault_1688_test.cpp
//   (BEZEL_TEST local_const_eval_fault_1688, int_overflow_rejected).
//
// §17.14: `const B := 2147483647 + 1;` — the spec's own example, at local
// scope. Pre-fix: compiled clean, raised OverflowException at runtime.

module M; class C public static method Main(): int const B := 2147483647 + 1; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `B` initializer value 2147483648 does not fit its declared type — a const initializer must be a representable compile-time constant
// CHECK-SAME: (§17.14)
