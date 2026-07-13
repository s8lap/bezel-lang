// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_type_test.cpp
//   (BEZEL_TEST sema_const, int_const_overflow_rejected_958).
//
// §6.1 / Appendix C.11 (#958): a const initializer is a compile-time constant;
// an integer const whose folded value does not fit its declared type is a
// compile error (it raised OverflowException at runtime before).

module M; class T end T; class C public const X: int := 2147483647 + 1; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `X` initializer value 2147483648 does not fit its declared type — a const initializer must be a representable compile-time constant
// CHECK-SAME: (§6.1 / Appendix C.11)
