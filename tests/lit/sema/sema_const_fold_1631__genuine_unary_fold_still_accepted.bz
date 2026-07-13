// Ported from tests/sema/const_fold_null_operand_1631_test.cpp
//   (BEZEL_TEST sema_const_fold_1631, genuine_unary_fold_still_accepted).
//
// §17.14: unary `-` on a constant operand still folds correctly.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public const K: int := -5; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
