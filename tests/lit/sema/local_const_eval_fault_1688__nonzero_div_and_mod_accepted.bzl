// Ported from tests/sema/local_const_eval_fault_1688_test.cpp
//   (BEZEL_TEST local_const_eval_fault_1688, nonzero_div_and_mod_accepted).
//
// A non-zero-divisor `/` and `mod` still fold without error (regression
// guard on the fold path itself, mirroring const_type_test.cpp's class-level
// equivalents).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int const A := 10 / 2; const B := 7 mod 3; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
