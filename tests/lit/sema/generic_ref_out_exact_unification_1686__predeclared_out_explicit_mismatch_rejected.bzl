// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_ref_out_exact_unification_1686_test.cpp
//   (BEZEL_TEST generic_ref_out_exact_unification_1686, predeclared_out_explicit_mismatch_rejected).
//
// §14.5 / #1686: the same pre-declared-out shape, but with an EXPLICIT
// `<int>` type argument and a MISMATCHED pre-declared `out` local (`long`,
// not `int`) — must be rejected via the exactness check (T is fixed by the
// explicit list; only the out argument's own type disagrees).
//
// fix #8639 (§5.1, §3.6 rule 2, §14.2, v0.357): the body's write into the
// `out` parameter is re-spelled `o := default(T)` (the §17.11-legal default
// for a `value`-constrained type parameter). The body used to write the
// primitive literal `o := 7` — a concrete-primitive-into-a-type-parameter
// assignment, which fix #8639 rejects on the §5.1 ground. Without the
// re-spelling this row would be rejected for that reason instead of the one
// it pins (the out argument's type against the explicit type argument), so
// the re-spelling keeps the row measuring what it is named for.

module M; static class U public static method H<T>(out o: T): int where T: value o := default(T); return 3; end H; end U; class C public static method Main(): int var s: long := 0; var r := U.H<int>(out s); return r; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `out` argument for parameter `o` does not unify exactly with the inferred type `int` — a `ref`/`out` position aliases the caller's location, so no conversion is admitted, unlike an ordinary by-value argument
// CHECK-SAME: (§14.5)
