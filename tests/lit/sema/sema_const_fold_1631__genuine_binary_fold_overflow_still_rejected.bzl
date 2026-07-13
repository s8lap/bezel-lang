// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_fold_null_operand_1631_test.cpp
//   (BEZEL_TEST sema_const_fold_1631, genuine_binary_fold_overflow_still_rejected).
//
// §17.14 / §6.1 (Appendix C.11, #958): a genuinely-constant fold whose value
// does not fit the declared type is still correctly range-checked (the guard
// must only decline null operands, not blunt the real fold-and-check path).

module M; class C public const K: byte := 200 + 100; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `K` initializer value 300 does not fit its declared type — a const initializer must be a representable compile-time constant
// CHECK-SAME: (§6.1 / Appendix C.11)
