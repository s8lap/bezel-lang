// §12.9 / §5.1 (issue #1851): `Set<T>` implements `ICollection<T>` (stdlib §4.6);
// `Set<Dog>` → `ICollection<Dog>` is a genuine reference conversion (identical
// invariant argument) and must be accepted. This is the OVER-REJECTION guard for
// the #1851 fix: the reject path fires only when no conforming candidate exists,
// never for a real conformance. (A naive fix that flipped the no-candidate case
// to reject WITHOUT completing the `Set : ICollection` model would break this.)
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Animal end Animal; class Dog: Animal end Dog; class C public method M(a: Set<Dog>) var x: ICollection<Dog> := a; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
