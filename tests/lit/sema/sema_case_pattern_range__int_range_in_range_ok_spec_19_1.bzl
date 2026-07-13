// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, int_range_in_range_ok_spec_19_1).
//
// An in-range range pattern over an `int` is accepted (§19.1 grade idiom). The
// fix must not flag ordinary ranges. #1153.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(x: int): int return case x of 0..100 => 1, _ => 0 end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
