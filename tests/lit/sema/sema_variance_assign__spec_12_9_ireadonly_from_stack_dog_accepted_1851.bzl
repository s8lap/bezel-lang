// §12.9 / §5.1 (issue #1851): the type the reported bug over-accepted (`Stack<Dog>`)
// DOES conform to its real interface `IReadOnlyCollection<Dog>` (stdlib §4.11), so
// this assignment must still be accepted. Paired with the
// `ilist_from_stack_dog_rejected_1851` reject test, this pins the fix to the
// precise boundary: `Stack<Dog>` is rejected for `IList<Dog>` (not an ancestor)
// but accepted for `IReadOnlyCollection<Dog>` (a genuine ancestor). Guards against
// over-rejecting the modeled read-only chain.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Animal end Animal; class Dog: Animal end Dog; class C public method M(a: Stack<Dog>) var x: IReadOnlyCollection<Dog> := a; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
