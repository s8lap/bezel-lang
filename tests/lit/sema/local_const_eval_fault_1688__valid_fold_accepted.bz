// Ported from tests/sema/local_const_eval_fault_1688_test.cpp
//   (BEZEL_TEST local_const_eval_fault_1688, valid_fold_accepted).
//
// §17.14 example: `const H := int.MaxValue / 2;` style non-overflowing
// division (using a literal in place of int.MaxValue, which is a separate,
// unrelated admissibility gap — #1688 item 2) must still compile clean.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int const H := 2147483647 / 2; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
