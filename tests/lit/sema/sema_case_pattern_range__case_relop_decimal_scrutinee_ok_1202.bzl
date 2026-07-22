// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, case_relop_decimal_scrutinee_ok_1202).
//
// A `decimal` scrutinee with an integer relational bound must NOT be rejected:
// its codegen path (decOperand/decCmp, #1152) bridges any numeric bound, so the
// compatibility gate excludes a decimal scrutinee. Regression guard. #1202.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(x: decimal): int return (case x of > 5 => 1, _ => 0); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
