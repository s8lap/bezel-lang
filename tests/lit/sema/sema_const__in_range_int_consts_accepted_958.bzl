// Ported from tests/sema/const_type_test.cpp
//   (BEZEL_TEST sema_const, in_range_int_consts_accepted_958).
//
// In-range integer consts (incl. a const-of-const and a long literal) are fine.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public const Y: int := 1000 + 1; public const Z: int := Y * 2; public const Big: long := 5000000000L; public const Neg: sbyte := -100; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
