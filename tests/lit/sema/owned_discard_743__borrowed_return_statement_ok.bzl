// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, borrowed_return_statement_ok).
//
// §21.9: a BORROWED-return statement is NOT rejected — the caller owns nothing.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class R public _h: int := 0; destructor _h := 1; end destructor; public method Touch(): int return _h; end Touch; end R; class C public static method Main(): int using var r := new R(); r.Touch(); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
