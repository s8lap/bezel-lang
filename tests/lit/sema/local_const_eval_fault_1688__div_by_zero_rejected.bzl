// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/local_const_eval_fault_1688_test.cpp
//   (BEZEL_TEST local_const_eval_fault_1688, div_by_zero_rejected).
//
// §17.14: `const D := 5 / 0;` — the spec's own example, at local scope.
// Pre-fix: compiled clean, raised DivideByZeroException at runtime.

module M; class C public static method Main(): int const D := 5 / 0; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `D` initializer contains a division or modulo by zero — a const initializer must fold to a representable compile-time-constant value
// CHECK-SAME: (§17.14)
