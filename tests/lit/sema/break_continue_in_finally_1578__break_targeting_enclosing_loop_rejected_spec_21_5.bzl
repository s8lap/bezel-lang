// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/break_continue_in_finally_1578_test.cpp
//   (BEZEL_TEST break_continue_in_finally_1578, break_targeting_enclosing_loop_rejected_spec_21_5).
//
// §21.5/§31.2: `break` inside a `finally` targeting the loop that ENCLOSES
// the `try`/`finally` is a compile error — the loop was opened before the
// `finally`, so the jump would exit past it.

module M; class C public method M(flag: bool, n: int) while flag do try n := n + 1; finally break; end; end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `break` inside a `finally` block may not target a loop that encloses the `finally`
// CHECK-SAME: (§21.5/§31.2)
