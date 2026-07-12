// Ported from tests/ownership/getter_returns_fresh_owned_926_test.cpp
//   (BEZEL_TEST getter_returns_fresh_owned_926, method_returning_owned_new_accepted).
//
// The recommended alternative the diagnostic points to: a METHOD returning
// `owned T` may return a fresh `new Cell()` (methods can carry `owned`). ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell end Cell; class Factory private _cell: Cell?; public static method MakeCell(): owned Cell return new Cell(); end MakeCell; public method MakeItem(): owned Cell return new Cell(); end MakeItem; end Factory;

// NODIAG-NOT: error
// NODIAG-NOT: §
