// Ported from tests/sema/owned_iter_var_discharge_834_test.cpp
//   (BEZEL_TEST owned_iter_var_discharge_834, range_for_loop_accepted).
//
// §29.2 / #834 (guardrail): a range for-loop — the iteration variable is a
// value (int), no discharge obligation. Must be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var t := 0; for var i in 0..<3 do t := t + i; end; return t; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
