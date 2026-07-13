// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/break_continue_in_finally_1578_test.cpp
//   (BEZEL_TEST break_continue_in_finally_1578, break_in_nested_if_inside_finally_rejected_spec_21_5).
//
// The enclosing-loop target is still rejected when the break/continue is
// nested inside an `if` within the finally (lexical nesting through
// non-loop constructs does not change the target loop).

module M; class C public method M(flag: bool, n: int) while flag do try n := n + 1; finally if flag then break; end; end; end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `break` inside a `finally` block may not target a loop that encloses the `finally`
// CHECK-SAME: (§21.5/§31.2)
