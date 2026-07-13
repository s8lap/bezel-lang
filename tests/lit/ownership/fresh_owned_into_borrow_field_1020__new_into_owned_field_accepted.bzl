// Ported from tests/ownership/fresh_owned_into_borrow_field_1020_test.cpp
//   (BEZEL_TEST fresh_owned_into_borrow_field_1020, new_into_owned_field_accepted).
//
// A fresh owned producer into an `owned` field is the legal ownership transfer
// (handled by the §28.5 owned-field path); it must NOT be caught by the #1020
// rule. ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Inner end Inner; record Pt(X: int, Y: int); class Wrap private _b: Inner?; private owned _o: Inner := new Inner(); private _v: Pt := new Pt(0, 0); public static method MakeOwned(): owned Inner return new Inner(); end MakeOwned; public method Set() this._o.Dispose(); this._o := new Inner(); end Set; end Wrap;

// NODIAG-NOT: error
// NODIAG-NOT: §
