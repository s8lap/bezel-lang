// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, void_call_statement_ok).
//
// §21.9: a void-returning method call statement is unaffected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Noop() end Noop; public static method Main(): int Noop(); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
