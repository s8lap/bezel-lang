// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/primitive_assignability_test.cpp
//   (BEZEL_TEST sema_assign, int_var_to_sbyte_narrowing_rejected_738).
//
// #738 / §5.1: an integer LITERAL/constant narrows implicitly (representable);
// a non-constant int VALUE does not — it needs an explicit cast. Previously the
// retype-to-narrower applied to ANY int initializer, so `var y: sbyte := x`
// (x an int variable) was silently accepted and truncated.

module M; class C public method M() var x: int := 300; var y: sbyte := x; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `y` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
