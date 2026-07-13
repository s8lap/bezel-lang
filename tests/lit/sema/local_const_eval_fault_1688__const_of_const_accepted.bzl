// Ported from tests/sema/local_const_eval_fault_1688_test.cpp
//   (BEZEL_TEST local_const_eval_fault_1688, const_of_const_accepted).
//
// A local const referencing an earlier local const (const-of-const) still
// folds and range-checks correctly — the fix must not blunt genuine folding.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int const A := 1000 + 1; const B := A * 2; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
