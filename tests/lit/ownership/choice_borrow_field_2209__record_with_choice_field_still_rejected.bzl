// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §35.4 / §9.7 (v0.226, issue #2209): no-regression on the prior #2159 face.
// A RECORD whose field type is a borrow-bearing CHOICE is itself borrow-
// bearing by §9.7 transitivity, and storing it into a class instance field
// must still be rejected — the "borrow-bearing record may not be stored in
// an instance field of a class" rule applies unchanged. After this fix's
// predicate additionally consults `isBorrowBearingChoice`, the record-leg
// (`isBorrowBearingRecord`) MUST remain the primary gate for record values,
// and this prior #2159 repro must STILL reject with the record-flavoured
// message (NOT the new choice-flavoured one). The diagnostic also pins the
// cross-check that #2159's classification fix (Choice leg of
// `recordFieldTypeIsBorrowCarryingCommon`) is still in place — without it,
// `HolderC` would itself be misclassified, this rule would never fire, and
// the regression would be silent.

module M; class Res public method Val(): int return 5; end Val; end Res; choice RefC Some(r: Res), None; end RefC; record HolderC(K: RefC); class Box public r: HolderC; public constructor(x: HolderC) r := x; end constructor; end Box; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow-bearing record may not be stored in an instance field of a class
// CHECK-SAME: (§9.7;
