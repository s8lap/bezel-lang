// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, byte_const_in_range_ok_spec_19).
//
// An in-range byte constant pattern is accepted and is the normal, correct
// case the fix must not disturb. #1153.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(x: byte): int return case x of 5 => 1, _ => 0 end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
