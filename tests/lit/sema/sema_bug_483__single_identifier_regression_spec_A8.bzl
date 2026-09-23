// Ported from tests/sema/bug_483_nested_type_construction_test.cpp
//   (BEZEL_TEST sema_bug_483, single_identifier_regression_spec_A8).
//
// Regression: `new OuterOnly()` (single-identifier, no nesting) must still
// resolve correctly.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Box public _v: int := 0; destructor end destructor; end Box; class C public static method Main(): int var b := new Box(); return b._v; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned value in `b` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)
