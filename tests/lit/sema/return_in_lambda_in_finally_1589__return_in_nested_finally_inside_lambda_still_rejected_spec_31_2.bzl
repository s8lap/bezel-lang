// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/return_in_lambda_in_finally_1589_test.cpp
//   (BEZEL_TEST return_in_lambda_in_finally_1589, return_in_nested_finally_inside_lambda_still_rejected_spec_31_2).
//
// §31.2 guard sanity: a `return` inside a NESTED `try`/`finally` that is
// itself inside the lambda's body (not the lambda's own top-level statement
// list) is still rejected — the lambda resets `finallyDepth_` to 0 on
// entry, but a `finally` opened inside the lambda body increments it again,
// so a `return` in THAT nested finally is still an escape from a finally
// (the lambda's own finally, not the outer method's).

module M; class C public static method Foo(): int var n := 0; try n := n + 1; finally var f := lambda() => do try var x := 1; finally return x; end; end; n := n + f(); end; return n; end Foo; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `return` inside a `finally` block is a compile error
// CHECK-SAME: (§31.2)
// fix #6190: the unresolved `x` operand of the value `return` must not turn the
// lambda into a unit procedure lambda (§27.3), which cascaded
// "operator `+` is not defined for a unit (`()`) operand".
// CHECK-NOT: is not defined for a unit
