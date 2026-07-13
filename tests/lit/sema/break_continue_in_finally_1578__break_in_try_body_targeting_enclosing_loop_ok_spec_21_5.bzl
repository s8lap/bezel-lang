// Ported from tests/sema/break_continue_in_finally_1578_test.cpp
//   (BEZEL_TEST break_continue_in_finally_1578, break_in_try_body_targeting_enclosing_loop_ok_spec_21_5).
//
// Positive: `break`/`continue` in a `try` body (not `finally`) targeting a
// loop that encloses the whole `try` statement is unaffected by this rule —
// only jumps physically inside the `finally` are restricted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(flag: bool, n: int) while flag do try break; finally n := n + 1; end; end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
