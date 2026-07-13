// Ported from tests/sema/generic_type_args_all_or_nothing_1687_test.cpp
//   (BEZEL_TEST generic_type_args_1687, nongeneric_ctor_unaffected).
//
// Regression control: a NON-generic class constructor (no type parameters
// at all) must remain completely unaffected by the §14.5 Scope check.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Widget private _v: int; public constructor(v: int) _v := v; end constructor; end Widget; class C public static method Main(): int var w := new Widget(7); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `w` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)
