// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_type_test.cpp
//   (BEZEL_TEST sema_const, byte_const_overflow_rejected_958).
//
// §6.1 (#958): a byte const out of range is rejected.

module M; class T end T; class C public const B: byte := 300; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `B` initializer value 300 does not fit its declared type — a const initializer must be a representable compile-time constant
// CHECK-SAME: (§6.1 / Appendix C.11)
