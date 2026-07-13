// Ported from tests/sema/hiding_override_test.cpp
//   (BEZEL_TEST sema_hide, widened_override_through_two_levels_ok).
//
// §12.2 (v0.181, #1560): the hiding-escape clause reads "a matching
// signature per §12.1" — which now admits the base-`T`/override-`T?`
// nullability widening (§12.1) — so a widened `override` is recognized as a
// legitimate override (escaping the hiding rule), not a same-name hiding
// declaration. Regression coverage for #1090's overload-selection loop: the
// hiding check picks a base candidate to validate the derived member
// against BEFORE it can decide override-vs-hiding, and that selection must
// use the SAME widening-aware predicate as the override-match check itself
// (Resolver::overrideParamTypeMatches, shared via overrideParamsMatch), or
// this widened member would be spuriously matched against the wrong
// candidate and mis-diagnosed.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A public virtual method M(a: string) _ := 1; end M; end A; class B : A public override method M(a: string?) _ := 2; end M; end B; class C : B public override method M(a: string?) _ := 3; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
