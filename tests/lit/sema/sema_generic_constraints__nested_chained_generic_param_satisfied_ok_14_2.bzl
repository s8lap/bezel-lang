// Ported from tests/sema/generic_constraints_test.cpp
//   (BEZEL_TEST sema_generic_constraints, nested_chained_generic_param_satisfied_ok_14_2).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C<T> where T: new() private _x: T?; end C; class Box<W> private _b: C<W>?; end Box; class Outer<X> private _o: Box<X>?; end Outer; class App private _u: Outer<int>?; end App;

// NODIAG-NOT: error
// NODIAG-NOT: §
