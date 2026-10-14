// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_constraints_test.cpp
//   (BEZEL_TEST sema_generic_constraints, nested_indirect_source_order_independent_14_2).

module M; class App private _u: Box<IFoo>?; end App; class Box<W> private _b: C<W>?; end Box; interface IFoo end IFoo; class C<T> where T: new() private _x: T?; end C;

// #5998 (§14.2 L2391, v0.338): an unconstrained `W` does not satisfy the
// requirement of `C<W>` ("decided at that site, by P's own constraints"), so
// the rejection is at the naming declaration `Box<W>`, not at the instantiation.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `W` does not satisfy the constraint `where T: new()`
// CHECK-SAME: (§14.2)
