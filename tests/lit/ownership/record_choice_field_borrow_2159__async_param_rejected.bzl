// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 / §9.7 / §35.4 (v0.226, issue #2159): a record made borrow-bearing by a
// borrow-bearing choice field is forbidden as an `async` parameter (§30.1 — the
// captured borrow has no stack frame bounding it). This exercises the existing
// record async check (checkAsyncSignatureBorrowBearing → isBorrowBearingRecord)
// now that the record field walk classifies the choice-field record correctly.

module M; class Res public method Val(): int return 5; end Val; end Res; choice RefC Some(r: Res), None; end RefC; record HolderC(K: RefC); static class F public static async method Bad(x: HolderC): Task<int> return 1; end Bad; end F; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `async` parameter `x` has a borrow-bearing type
// CHECK-SAME: (§30.1)
