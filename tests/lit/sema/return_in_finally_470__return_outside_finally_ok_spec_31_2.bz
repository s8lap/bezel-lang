// Ported from tests/sema/return_in_finally_test.cpp
//   (BEZEL_TEST return_in_finally_470, return_outside_finally_ok_spec_31_2).
//
// Positive: `return` outside a try/finally is always OK.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Foo(): int return 5; end Foo; public static method Main(): int => Foo(); end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
