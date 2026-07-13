// Ported from tests/sema/break_continue_in_finally_1578_test.cpp
//   (BEZEL_TEST break_continue_in_finally_1578, break_outside_finally_ok_spec_21_5).
//
// Positive: ordinary `break`/`continue` with no try/finally involved at all
// still compiles (baseline, mirrors break_continue_test.cpp).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(flag: bool, n: int) while flag do break; end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
