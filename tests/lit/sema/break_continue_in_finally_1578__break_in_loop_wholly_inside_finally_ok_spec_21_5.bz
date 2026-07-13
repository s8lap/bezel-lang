// Ported from tests/sema/break_continue_in_finally_1578_test.cpp
//   (BEZEL_TEST break_continue_in_finally_1578, break_in_loop_wholly_inside_finally_ok_spec_21_5).
//
// Positive: a loop declared WHOLLY INSIDE the `finally` block uses
// `break`/`continue` normally — §21.5: "Loops wholly inside the finally use
// break/continue normally."
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(flag: bool, n: int) try n := n + 1; finally while flag do break; end; end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
