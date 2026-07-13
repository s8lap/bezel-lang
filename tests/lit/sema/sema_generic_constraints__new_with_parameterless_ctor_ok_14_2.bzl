// Ported from tests/sema/generic_constraints_test.cpp
//   (BEZEL_TEST sema_generic_constraints, new_with_parameterless_ctor_ok_14_2).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class HasCtor public constructor() end constructor; end HasCtor; class C<T> where T: new() private _x: T?; end C; class U private _u: C<HasCtor>?; end U;

// NODIAG-NOT: error
// NODIAG-NOT: §
