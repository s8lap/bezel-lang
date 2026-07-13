// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, all_exact_call_not_ambiguous).
//
// §22.6 — both arguments exact in one candidate, exact-or-convert in the
// other: the all-exact candidate is unambiguously better. Sanity check
// that an all-exact call is never flagged ambiguous.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F(a: int, b: int): int => a; public method F(a: int, b: long): int => 1; public method Use(x: int, y: int) _ := this.F(x, y); end Use; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
