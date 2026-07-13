// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/return_in_lambda_in_finally_1589_test.cpp
//   (BEZEL_TEST return_in_lambda_in_finally_1589, bare_return_directly_in_finally_still_rejected_spec_31_2).
//
// §31.2 guard sanity: a bare `return` directly in a `finally` (NOT inside a
// lambda) must still be rejected — the fix must not over-open the gate.

module M; class C public static method Foo(): int var n := 0; try n := n + 1; finally return n; end; return n; end Foo; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `return` inside a `finally` block is a compile error
// CHECK-SAME: (§31.2)
