// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §35.4 / §9.7 (v0.226, issue #2209): the directly-choice-field face of the
// §9.7 instance-field storage rule. §9.7 reads "A borrow-bearing record value
// may not be stored in an instance field of a class"; §35.4 extends §9.7 with
// "Borrow-bearing choices are subject to every rule of this section, with
// 'record' read as 'record or choice' throughout" — including the
// instance-field-storage bullet. Before the fix, the field-store rejection
// (`MethodOwnership::checkBorrowStoreEscape` at the `Dest::Field` leg)
// consulted only `isBorrowBearingRecord`, which never classifies a choice,
// so `Opt<Res>` (`Borrowed`-case carries a borrow-typed `local`) was admitted
// into a class instance field: the field's runtime value carried the borrow
// past the local's auto-drop, a §9.7 use-after-dispose. Adding a choice leg
// to the predicate with the same null-source exception the #1540 record rule
// uses closes the gap — and the record-with-choice-field face already covered
// by #2159 stays covered.

module M; class Res public method Val(): int return 5; end Val; end Res; choice Opt Some(r: Res), None; end Opt; class Box public v: Opt; public constructor(x: Opt) v := x; end constructor; end Box; class C public static method Main(): int using var local := new Res(); var b := new Box(Opt.Some(local)); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow-bearing choice may not be stored in an instance field of a class
// CHECK-SAME: (§9.7 with "record" read as "record or choice", §35.4
