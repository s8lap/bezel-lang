// RUN: %bezelc -c -o %t.o %s
//
// §35.4 / §9.7 (v0.226, issue #2209): the ACCEPT (non-over-rejection) control
// for the directly-choice-typed class field rule. `VOpt` is VALUE-LIKE
// (§35.4.3, payloads are `int` and `string`), NOT borrow-bearing — so a
// directly-choice-typed class field of `VOpt` is ordinary value storage and
// stays accepted (matching the §35.4.3 record-transitivity value-like
// control of #2159).

module M; choice VOpt Some(n: int), Named(s: string), None; end VOpt; class Box public r: VOpt; public constructor(x: VOpt) r := x; end constructor; end Box; class C public static method Main(): int var b := new Box(VOpt.Some(7)); return 0; end Main; end C;