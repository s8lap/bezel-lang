// Ported from tests/sema/readonly_field_assignment_747_test.cpp
//   (BEZEL_TEST readonly_field_assignment_747, mutable_field_ok).
//
// §22.8: a non-readonly field is freely assignable — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _v: int := 0; public static method Main(): int using var c := new C(); c._v := 9; return c._v; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
