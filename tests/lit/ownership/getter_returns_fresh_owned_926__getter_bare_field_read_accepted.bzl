// Ported from tests/ownership/getter_returns_fresh_owned_926_test.cpp
//   (BEZEL_TEST getter_returns_fresh_owned_926, getter_bare_field_read_accepted).
//
// A getter returning a bare field read is a BORROW (the legal getter form). The
// property type is nullable to mirror the field. ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell end Cell; class Factory private _cell: Cell?; public static method MakeCell(): owned Cell return new Cell(); end MakeCell; public property Item: Cell? get => _cell; end Item; end Factory;

// NODIAG-NOT: error
// NODIAG-NOT: §
