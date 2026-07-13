// Ported from tests/sema/generic_ref_out_exact_unification_1686_test.cpp
//   (BEZEL_TEST generic_ref_out_exact_unification_1686, inline_out_var_nullable_unaffected).
//
// §14.5 / #1686 / §22.4 control: an INLINE `out var b` argument against a
// nullable-out generic parameter (`SetIt<T>(x: T, out v: T?)`) — the
// out-var's type is inferred FROM the parameter (§22.4), so it carries no
// static type of its own to compare and must be skipped entirely by both
// halves of this check. Must remain accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class U public static method SetIt<T>(x: T, out v: T?) where T: value v := x; end SetIt; end U; class C public static method Main(): int U.SetIt(88, out var b); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
