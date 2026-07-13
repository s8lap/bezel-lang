// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/alias_transparency_578_test.cpp
//   (BEZEL_TEST sema_alias_transparency_578, alias_of_class_without_operator_still_errors_spec_27_8).
//
// Control: the error for a true user-defined type with NO operator is still
// reported. The fix must NOT suppress legitimate §25 errors.

module M; class Kg public V: double := 0.0; end Kg; type Weight := Kg; class C public static method M(a: Weight, b: Weight): double var s := a + b; return 0.0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` is not defined for type `Kg` with these operands — declare a matching `operator +` whose parameters match the operand order (Bezel does not auto-commute operator overloads)
// CHECK-SAME: (§25)
