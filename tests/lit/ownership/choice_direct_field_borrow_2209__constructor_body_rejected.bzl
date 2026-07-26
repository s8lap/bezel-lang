// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §35.4 / §9.7 (v0.226, issue #2209): the directly-choice-typed class field
// face. §9.7's instance-field rule ("Borrow-bearing choices are subject to
// every rule of this section, with 'record' read as 'record or choice'
// throughout") was previously only reached via a record-with-choice-field
// record (#2159); a directly-choice-typed class field was admitted. Here
// `Box.r` is of borrow-bearing-choice type `SpanC` (a span payload), and
// the constructor-body `r := x` was the issue's exact repro. // per §35.4, §9.7

module M; choice SpanC Some(s: ArraySpan<int>), None; end SpanC; class Box public r: SpanC; public constructor(x: SpanC) r := x; end constructor; end Box; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow-bearing choice may not be stored in an instance field of a class
// CHECK-SAME: (§9.7
// CHECK-SAME: §35.4