// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, trivial_discard_not_task_error_1014).
//
// ACCEPT: a trivial-value discard is unaffected by the task reject.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int _ := 5; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
