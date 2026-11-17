// Ported from tests/sema/generic_ref_out_exact_unification_1686_test.cpp
//   (BEZEL_TEST generic_ref_out_exact_unification_1686, predeclared_out_single_occurrence_accepted).
//
// §14.5 / #1686 / §22.4 regression guard: a PRE-DECLARED `out s: int`
// (not the inline `out var` form) against `H<T>(out o: T)` inferred SOLELY
// from that out argument — a single ref/out occurrence, trivially exact.
// Must remain accepted (mirrors #1164's predeclared-out inference guard).
//
// fix #8639 (§5.1, §3.6 rule 2, §14.2, v0.357): the body's write into the
// `out` parameter is re-spelled `o := default(T)` (the §17.11-legal default
// for a `value`-constrained type parameter). The body used to write the
// primitive literal `o := 7`, which is a concrete-primitive-into-a-type-
// parameter assignment — §5.1's conversion list names a type parameter only
// as the SOURCE of a conversion, so under `where T: value` (which admits
// records too) the literal has no conversion to `T` and the body does not
// type-check under the weakest assumption its constraints establish
// (§3.6 rule 2). The change is confined to the body's write: the guard's
// subject — the INFERENCE of `T` from the single pre-declared `out` argument
// — is untouched.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class U public static method H<T>(out o: T): int where T: value o := default(T); return 3; end H; end U; class C public static method Main(): int var s: int := 0; var r := U.H(out s); return r + s; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
