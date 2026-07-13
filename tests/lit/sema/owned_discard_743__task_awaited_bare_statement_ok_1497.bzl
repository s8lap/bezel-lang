// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, task_awaited_bare_statement_ok_1497).
//
// ACCEPT: `await Work();` as a bare statement — awaiting to settlement is
// exactly the remedy §21.9 asks for; must NOT trip the bare-statement reject.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static async method Work(): Task return; end Work; public static async method Caller(): Task await Work(); return; end Caller; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
