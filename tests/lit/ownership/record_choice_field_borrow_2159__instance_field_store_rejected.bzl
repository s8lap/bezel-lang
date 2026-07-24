// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §35.4 (v0.226, issue #2159): the instance-field escape face of the
// same classification gap. A borrow-bearing record (via its borrow-bearing
// choice field, §9.7 transitivity) may not be stored in a class instance field
// — a class instance's lifetime is not bounded by any borrow source's scope.
// Before the fix `HolderC` was not classified borrow-bearing, so the store was
// admitted.

module M; class Res public method Val(): int return 5; end Val; end Res; choice RefC Some(r: Res), None; end RefC; record HolderC(K: RefC); class Box public r: HolderC; public constructor(x: HolderC) r := x; end constructor; end Box; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow-bearing record may not be stored in an instance field of a class
// CHECK-SAME: (§9.7
