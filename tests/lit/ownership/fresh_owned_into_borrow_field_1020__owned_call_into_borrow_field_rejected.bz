// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/fresh_owned_into_borrow_field_1020_test.cpp
//   (BEZEL_TEST fresh_owned_into_borrow_field_1020, owned_call_into_borrow_field_rejected).
//
// An owned-returning CALL result is a fresh owned value (§28.3 producers table);
// into a borrow field it leaks. REJECT.

module M; class Inner end Inner; record Pt(X: int, Y: int); class Wrap private _b: Inner?; private owned _o: Inner := new Inner(); private _v: Pt := new Pt(0, 0); public static method MakeOwned(): owned Inner return new Inner(); end MakeOwned; public method Set() this._b := MakeOwned(); end Set; end Wrap;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a freshly-produced owned value cannot be stored into the non-`owned` field `_b` — the field does not own it, so the value would leak; declare the field `owned`
// CHECK-SAME: (§28.5)
