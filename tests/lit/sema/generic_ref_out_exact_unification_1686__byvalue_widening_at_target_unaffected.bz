// Ported from tests/sema/generic_ref_out_exact_unification_1686_test.cpp
//   (BEZEL_TEST generic_ref_out_exact_unification_1686, byvalue_widening_at_target_unaffected).
//
// §14.5 / #1686 control: ordinary BY-VALUE argument widening at a generic
// call's target-typed completion (phase 3) — `Identity<T>(v: T): T` has no
// ref/out parameter at all, so this check's fast bail must leave it
// completely untouched: `Identity(5)` infers `T = int`, and assigning the
// `int` result to an `Object`-typed local converts by the ordinary §5.1
// assignment conversion (the ROOT-doc §14.5 worked example). Must remain
// accepted — the fix must not reach for by-value positions at all.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class Utils public static method Identity<T>(v: T): T return v; end Identity; end Utils; class C public static method Main(): int var o: Object := Utils.Identity(5); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
