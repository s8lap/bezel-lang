// Ported from tests/sema/range_reversed_pattern_warn_1340_test.cpp
//   (BEZEL_TEST sema_range_reversed_pattern_warn_1340, correct_halfopen_no_warn_spec_16_6).
//
// §16.6 v0.162: a correct half-open `0..<10` must NOT warn.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(x: int): int return case x of 0..<10 => 1, _ => 0 end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
