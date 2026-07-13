// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_method_constraint_callsite_746_test.cpp
//   (BEZEL_TEST method_constraint_callsite_746, class_violates_value).
//
// §14.2: a class type argument violates `where T: value`.

module M; class Widget public n: int := 0; end Widget; static class F public static method Id<T>(v: T): T where T: value return v; end Id; end F; class C public static method Main(): int using var w := new Widget(); var r := F.Id(w); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Widget` does not satisfy the constraint `where T: value`
// CHECK-SAME: (§14.2)
