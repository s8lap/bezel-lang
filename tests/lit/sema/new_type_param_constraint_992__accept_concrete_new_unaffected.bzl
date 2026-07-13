// Ported from tests/sema/new_type_param_constraint_992_test.cpp
//   (BEZEL_TEST new_type_param_constraint_992, accept_concrete_new_unaffected).
//
// §14.1 (#992 SANITY): ordinary concrete `new Foo()` is completely unaffected
// — the gate only applies when the constructed type is a generic type parameter.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Widget public constructor() end constructor; destructor end destructor; end Widget; class C public static method Main(): int using var w := new Widget(); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
