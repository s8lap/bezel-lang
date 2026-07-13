// Ported from tests/ownership/fresh_owned_into_borrow_field_1020_test.cpp
//   (BEZEL_TEST fresh_owned_into_borrow_field_1020, borrow_local_into_borrow_field_accepted).
//
// A bare borrowed LOCAL (aliasing a borrow parameter) into the borrow field is a
// borrow store, not a fresh producer. ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Inner end Inner; record Pt(X: int, Y: int); class Wrap private _b: Inner?; private owned _o: Inner := new Inner(); private _v: Pt := new Pt(0, 0); public static method MakeOwned(): owned Inner return new Inner(); end MakeOwned; public method Set(b: Inner) var t := b; this._b := t; end Set; end Wrap;

// NODIAG-NOT: error
// NODIAG-NOT: §
