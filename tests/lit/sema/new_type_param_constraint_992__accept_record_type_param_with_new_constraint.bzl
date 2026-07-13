// Ported from tests/sema/new_type_param_constraint_992_test.cpp
//   (BEZEL_TEST new_type_param_constraint_992, accept_record_type_param_with_new_constraint).
//
// §14.1 (#992 ACCEPT): record with `where T: new()` — records satisfy new()
// when all fields have defaults; `new T()` in the body is valid.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Pt(x: int, y: int); class Box<T> where T: new() public method MakeOne(): T return new T(); end MakeOne; end Box; class C public static method Main(): int using var b := new Box<Pt>(); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
