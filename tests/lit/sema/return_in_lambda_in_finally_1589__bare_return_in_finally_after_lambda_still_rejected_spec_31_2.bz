// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/return_in_lambda_in_finally_1589_test.cpp
//   (BEZEL_TEST return_in_lambda_in_finally_1589, bare_return_in_finally_after_lambda_still_rejected_spec_31_2).
//
// §31.2 guard sanity: a bare `return` directly in a `finally`, AFTER a
// lambda declared earlier in that same `finally` has already been visited
// and exited, must still be rejected — verifies `finallyDepth_` is restored
// (not left at 0) once the lambda visit completes.

module M; class C public static method Foo(): int var n := 0; try n := n + 1; finally var f := () => do return 1; end; n := n + f(); return n; end; return n; end Foo; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `return` inside a `finally` block is a compile error
// CHECK-SAME: (§31.2)
