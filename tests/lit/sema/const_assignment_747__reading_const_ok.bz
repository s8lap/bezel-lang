// Ported from tests/sema/const_assignment_747_test.cpp
//   (BEZEL_TEST const_assignment_747, reading_const_ok).
//
// §10: reading a const (no assignment) is fine — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public const K: int := 100; public static method Main(): int return C.K; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
