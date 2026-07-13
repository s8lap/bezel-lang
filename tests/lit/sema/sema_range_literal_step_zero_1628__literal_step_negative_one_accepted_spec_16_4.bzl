// Ported from tests/sema/range_literal_step_zero_1628_test.cpp
//   (BEZEL_TEST sema_range_literal_step_zero_1628, literal_step_negative_one_accepted_spec_16_4).
//
// Control: `step -1` (a descending literal step) is valid and non-zero —
// must NOT error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var sum: int := 0; for var i in 10..0 step -1 do sum := sum + i; end; return sum; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
