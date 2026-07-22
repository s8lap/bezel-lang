// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, case_literal_pattern_still_ok_1205).
//
// §A.7/§19.3: an integer literal pattern is still valid and must NOT be
// flagged by the non-constant gate introduced for #1205. #1205.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(n: int): int return (case n of 1 => 10, 2 => 20, _ => 0); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
