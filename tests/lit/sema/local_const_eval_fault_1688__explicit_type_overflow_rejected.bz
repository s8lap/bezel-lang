// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/local_const_eval_fault_1688_test.cpp
//   (BEZEL_TEST local_const_eval_fault_1688, explicit_type_overflow_rejected).
//
// §17.14: an explicitly-typed narrower local const still range-checks the
// folded value against the ANNOTATED type, not just an inferred `int`.

module M; class C public static method Main(): int const B: byte := 300; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `B` initializer value 300 does not fit its declared type — a const initializer must be a representable compile-time constant
// CHECK-SAME: (§17.14)
