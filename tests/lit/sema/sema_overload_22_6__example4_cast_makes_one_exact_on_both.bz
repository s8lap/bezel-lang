// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, example4_cast_makes_one_exact_on_both).
//
// §22.6 Example 4 — casting one argument makes one candidate exact on both.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Calculator public method Compute(a: int, b: long): int => 1; public method Compute(a: long, b: int): int => 2; public method Use(x: int, y: int) _ := this.Compute(x, long(y)); end Use; end Calculator;

// NODIAG-NOT: error
// NODIAG-NOT: §
