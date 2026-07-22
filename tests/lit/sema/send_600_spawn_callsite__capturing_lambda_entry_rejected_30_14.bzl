// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_spawn_callsite_test.cpp
//   (BEZEL_TEST send_600_spawn_callsite, capturing_lambda_entry_rejected_30_14).
//
// §30.14: A capturing lambda (captures local variable) → rejected.
// "A capturing lambda or an instance method reference is rejected — captures
// and receivers are borrows of the origin domain."

module M; class Main public static async method Go(): Task var x: int := 10; using var t := Domain.Spawn(lambda() => x := x + 1); await t; end Go; end Main;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: expected ')' to close call argument list
