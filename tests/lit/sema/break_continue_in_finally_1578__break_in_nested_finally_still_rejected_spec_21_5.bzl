// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/break_continue_in_finally_1578_test.cpp
//   (BEZEL_TEST break_continue_in_finally_1578, break_in_nested_finally_still_rejected_spec_21_5).
//
// Two levels of enclosing try/finally: the target loop encloses BOTH, and
// the break sits in the innermost finally — still rejected.

module M; class C public method M(flag: bool, n: int) while flag do try n := n + 1; finally try n := n + 2; finally break; end; end; end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `break` inside a `finally` block may not target a loop that encloses the `finally`
// CHECK-SAME: (§21.5/§31.2)
