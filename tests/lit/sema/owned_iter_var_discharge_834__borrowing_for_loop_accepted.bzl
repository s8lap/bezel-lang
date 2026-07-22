// Ported from tests/sema/owned_iter_var_discharge_834_test.cpp
//   (BEZEL_TEST owned_iter_var_discharge_834, borrowing_for_loop_accepted).
//
// §29.2 / #834 (THE GUARDRAIL): a BORROWING for-loop over an Array<int>
// (IEnumerable<int>, default borrowing iteration) — the iteration variable is
// a borrow with NO discharge obligation. Must be ACCEPTED. An over-broad seed
// (keying on the for-loop alone rather than the owning-enumerable iterable
// type) would wrongly reject this — the most important regression to guard.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int const arr: Array<int> := [1, 2]; var total := 0; for var x in arr do total := total + x; end; return total; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
