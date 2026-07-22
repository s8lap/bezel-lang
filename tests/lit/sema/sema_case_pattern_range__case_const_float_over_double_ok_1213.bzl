// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, case_const_float_over_double_ok_1213).
//
// `case x 2.5` over a `double` scrutinee — float constant over float scrutinee
// is COMPATIBLE (same comparison class); must be accepted. #1213.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(x: double): int return (case x of 2.5 => 1, _ => 0); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
