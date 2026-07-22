// Ported from tests/sema/range_reversed_pattern_warn_1340_test.cpp
//   (BEZEL_TEST sema_range_reversed_pattern_warn_1340, equal_bounds_closed_no_warn_spec_16_6).
//
// §16.6 v0.162: an equal-bound closed inclusive `5..5` must NOT warn
// (it matches exactly the value 5, so it is not empty).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(x: int): int return (case x of 5..5 => 1, _ => 0); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
