// Ported from tests/sema/generic_ref_out_exact_unification_1686_test.cpp
//   (BEZEL_TEST generic_ref_out_exact_unification_1686, predeclared_out_single_occurrence_accepted).
//
// §14.5 / #1686 / §22.4 regression guard: a PRE-DECLARED `out s: int`
// (not the inline `out var` form) against `H<T>(out o: T)` inferred SOLELY
// from that out argument — a single ref/out occurrence, trivially exact.
// Must remain accepted (mirrors #1164's predeclared-out inference guard).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class U public static method H<T>(out o: T): int where T: value o := 7; return 3; end H; end U; class C public static method Main(): int var s: int := 0; var r := U.H(out s); return r + s; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
