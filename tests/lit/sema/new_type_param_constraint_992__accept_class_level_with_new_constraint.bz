// Ported from tests/sema/new_type_param_constraint_992_test.cpp
//   (BEZEL_TEST new_type_param_constraint_992, accept_class_level_with_new_constraint).
//
// §14.1 (#992 ACCEPT): class-level `where T: new()` — T has a parameterless
// constructor — `new T()` in the body is valid.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Widget public constructor() end constructor; destructor end destructor; end Widget; class Box<T> where T: new() public method MakeOne(): T return new T(); end MakeOne; end Box; class C public static method Main(): int using var b := new Box<Widget>(); using var w := b.MakeOne(); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
