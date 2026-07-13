// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_method_constraint_callsite_746_test.cpp
//   (BEZEL_TEST method_constraint_callsite_746, explicit_int_violates_class).
//
// §14.2: an EXPLICIT `int` type arg violates `where T: class`.

module M; static class F public static method Id<T>(v: T): T where T: class return v; end Id; end F; class C public static method Main(): int var r := F.Id<int>(42); return r; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `int` does not satisfy the constraint `where T: class`
// CHECK-SAME: (§14.2)
