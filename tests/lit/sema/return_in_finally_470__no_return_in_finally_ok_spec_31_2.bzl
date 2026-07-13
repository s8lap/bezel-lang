// Ported from tests/sema/return_in_finally_test.cpp
//   (BEZEL_TEST return_in_finally_470, no_return_in_finally_ok_spec_31_2).
//
// Positive: try/finally without any `return` in finally is OK.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Foo(): int try var x := 1; finally var y := 2; end; return 0; end Foo; public static method Main(): int => Foo(); end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
