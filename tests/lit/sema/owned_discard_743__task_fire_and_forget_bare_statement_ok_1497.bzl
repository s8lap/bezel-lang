// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, task_fire_and_forget_bare_statement_ok_1497).
//
// ACCEPT: `Task.FireAndForget(Work());` as a bare statement — the named
// remedy itself returns `()`, so it must not re-trip the check.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static async method Work(): Task return; end Work; public static async method Caller(): Task Task.FireAndForget(Work()); return; end Caller; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
