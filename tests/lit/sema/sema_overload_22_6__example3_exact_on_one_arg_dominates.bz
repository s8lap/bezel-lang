// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, example3_exact_on_one_arg_dominates).
//
// §22.6 Example 3 — exact on one argument, conversion on the other.
// Apply(int,long) is placed first; Apply(int,int) is exact on both and is
// unambiguously better for (int, int). It returns int; Apply(int,long)
// returns bool; binding to `var x: int` would error if the wrong one won.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Pair public method Apply(x: int, y: long): bool => true; public method Apply(x: int, y: int): int => x + y; public method Use(a: int, b: int) var r: int := this.Apply(a, b); end Use; end Pair;

// NODIAG-NOT: error
// NODIAG-NOT: §
