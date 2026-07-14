// Ported from issue #1842's no-over-reject requirement.
//
// §12.9 (v0.196): nested covariance that also USES subtyping at the inner level.
// `Array<Array<Dog>>` → `IEnumerable<IEnumerable<Animal>>` is LEGAL: the element
// conversion `Array<Dog>` → `IEnumerable<Animal>` composes a covariant reference
// conversion (Array→IEnumerable) with the covariant subtyping `Dog` <: `Animal`.
// It must be accepted — the recursive variance walk composes both steps.
//
// Fails-without-fix: same sibling wrong-reject as the adjacent test; passes-with
// the recursive fix. Compile-only for the same nested-view-iteration codegen
// reason noted there (#1850).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Animal end Animal; class Dog: Animal end Dog; class C public method M(a: Array<Array<Dog>>) var x: IEnumerable<IEnumerable<Animal>> := a; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
