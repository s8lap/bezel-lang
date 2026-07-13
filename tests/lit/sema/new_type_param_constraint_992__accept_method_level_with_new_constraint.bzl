// Ported from tests/sema/new_type_param_constraint_992_test.cpp
//   (BEZEL_TEST new_type_param_constraint_992, accept_method_level_with_new_constraint).
//
// §14.1 (#992 ACCEPT): method-level `where T: new()` — `new T()` in the
// method body is valid.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Widget public constructor() end constructor; destructor end destructor; end Widget; static class Factory public static method Make<T>(): int where T: class, new() using var t := new T(); return 1; end Make; end Factory; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
