// Ported from tests/sema/sealed_class_test.cpp
//   (BEZEL_TEST sema_sealed, closed_class_same_module_subclass_ok_spec_11_7).
//
// §11.7 / §19.6: the `closed` modifier declares a closed abstract class —
// a class that may be inherited from *within the declaring module*.
// The §11.7 example form (a `closed` Shape with same-module `sealed`
// subclasses) must compile. This is the spec's recommended closed-hierarchy
// idiom for exhaustive `case` matching; rejecting it made the idiom
// uncompilable (issue: FINDING [6]).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; closed class Shape public abstract method Area(): int; end Shape; sealed class Circle : Shape public R: int; public constructor(r: int) R := r; end constructor; public override method Area(): int => 3 * R * R; destructor end destructor; end Circle;

// NODIAG-NOT: error
// NODIAG-NOT: §
