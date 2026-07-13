// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_ref_out_exact_unification_1686_test.cpp
//   (BEZEL_TEST generic_ref_out_exact_unification_1686, predeclared_out_explicit_mismatch_rejected).
//
// §14.5 / #1686: the same pre-declared-out shape, but with an EXPLICIT
// `<int>` type argument and a MISMATCHED pre-declared `out` local (`long`,
// not `int`) — must be rejected via the exactness check (T is fixed by the
// explicit list; only the out argument's own type disagrees).

module M; static class U public static method H<T>(out o: T): int where T: value o := 7; return 3; end H; end U; class C public static method Main(): int var s: long := 0; var r := U.H<int>(out s); return r; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `out` argument for parameter `o` does not unify exactly with the inferred type `int` — a `ref`/`out` position aliases the caller's location, so no conversion is admitted, unlike an ordinary by-value argument
// CHECK-SAME: (§14.5)
