// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/nested_new_owned_arg_835_test.cpp
//   (BEZEL_TEST nested_new_owned_arg_835, nested_new_borrow_param_rejected).
//
// A borrowing PARAMETER flowing into the nested inner owned param is rejected.

module M; class Cell public val: int; public constructor(v: int) val := v; end constructor; destructor end destructor; end Cell; class Box private owned _c: Cell; public constructor(owned v: Cell) _c := v; end constructor; destructor end destructor; end Box; class Outer private owned _b: Box; public constructor(owned v: Box) _b := v; end constructor; destructor end destructor; end Outer; class C public static method Bad(c: Cell): owned Outer return new Outer(new Box(c)); end Bad; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow cannot be passed where an `owned` value is required: an `owned` sink takes responsibility for disposing the value, but a borrow is owned elsewhere
// CHECK-SAME: (§28.8 — transfer an owned value with `move`, `new`, or a call returning `owned T`)
