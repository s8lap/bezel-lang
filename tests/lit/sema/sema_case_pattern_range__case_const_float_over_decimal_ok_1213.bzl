// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, case_const_float_over_decimal_ok_1213).
//
// `case x 1.5` over a `decimal` scrutinee — decimal's codegen path bridges
// any numeric bound (decOperand/decCmp), so a float constant over a decimal
// scrutinee must NOT be rejected. Regression guard for the sc==5 exclusion.
// #1213.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(x: decimal): int return (case x of 1.5 => 1, _ => 0); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
