// Ported from tests/sema/generic_constraints_test.cpp
//   (BEZEL_TEST sema_generic_constraints, new_with_default_record_ok_14_2).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point(X: int, Y: int); class C<T> where T: new() private _x: T?; end C; class U private _u: C<Point>?; end U;

// NODIAG-NOT: error
// NODIAG-NOT: §
