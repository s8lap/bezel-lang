// Ported from tests/sema/generic_constraints_test.cpp
//   (BEZEL_TEST sema_generic_constraints, generic_param_arg_deferred_14_2).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C<T> where T: class private _x: T?; end C; class Wrapper<U> private _c: C<U>?; end Wrapper;

// NODIAG-NOT: error
// NODIAG-NOT: §
