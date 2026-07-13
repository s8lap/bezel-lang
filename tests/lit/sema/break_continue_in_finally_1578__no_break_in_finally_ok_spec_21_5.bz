// Ported from tests/sema/break_continue_in_finally_1578_test.cpp
//   (BEZEL_TEST break_continue_in_finally_1578, no_break_in_finally_ok_spec_21_5).
//
// Positive: a `try`/`finally` with no `break`/`continue` in the finally at
// all is unaffected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(flag: bool, n: int) while flag do try n := n + 1; finally n := n + 2; end; end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
