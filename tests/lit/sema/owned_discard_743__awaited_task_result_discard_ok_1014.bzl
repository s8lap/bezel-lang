// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, awaited_task_result_discard_ok_1014).
//
// ACCEPT: `_ := await <Task<T>>` — await yields the RESULT type (`int`), not a
// task; discarding a trivial result is fine and must NOT trip the task reject.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static async method Work(): Task<int> return 0; end Work; public static async method Caller(): Task _ := await Work(); return; end Caller; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
