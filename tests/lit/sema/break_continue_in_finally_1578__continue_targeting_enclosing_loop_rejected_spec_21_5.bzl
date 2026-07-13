// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/break_continue_in_finally_1578_test.cpp
//   (BEZEL_TEST break_continue_in_finally_1578, continue_targeting_enclosing_loop_rejected_spec_21_5).
//
// Same rule for `continue`.

module M; class C public method M(flag: bool, n: int) while flag do try n := n + 1; finally continue; end; end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `continue` inside a `finally` block may not target a loop that encloses the `finally`
// CHECK-SAME: (§21.5/§31.2)
