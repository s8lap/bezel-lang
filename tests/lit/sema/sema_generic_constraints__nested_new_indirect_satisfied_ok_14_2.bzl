// Ported from tests/sema/generic_constraints_test.cpp; flipped by #5998.
// The naming declaration's unconstrained parameter does not satisfy the
// requirement of the nested constraint: §14.2 L2391 (v0.338) says C<W> is
// "rejected in an unconstrained `class Box<W>`", so this is a REJECT row.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
module M; class C<T> where T: new() private _x: T?; end C; class Good end Good; class Box<W> private _b: C<W>?; end Box; class App private _u: Box<Good>?; end App;
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `W` does not satisfy the constraint
// CHECK-SAME: (§14.2)
