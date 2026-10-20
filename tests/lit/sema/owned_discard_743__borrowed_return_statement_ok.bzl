// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, borrowed_return_statement_ok).
//
// §21.9: a BORROWED-return statement is NOT rejected — the caller owns nothing.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no ERROR.
//
// fix #6555 (§21.9, v0.338) flipped the second control here. This row pinned
// "no diagnostics of any severity", which §21.9's base rule — the unused-result
// WARNING on an expression statement whose result is non-`()` — now contradicts:
// `r.Touch();` returns `int` and is discarded. The row's SUBJECT is unchanged
// and is what the flip must not disturb: the statement is still ACCEPTED, not
// rejected, because the result is a borrow the caller does not own. So
// `NODIAG-NOT: error` stays and the old blanket `NODIAG-NOT: §` is replaced by
// a W21.9 check pinning the warning §21.9 now requires — a strengthening (a
// specific diagnostic is pinned where silence used to be) rather than a
// relaxation.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=W21_9 --allow-empty %s

module M; class R public _h: int := 0; destructor _h := 1; end destructor; public method Touch(): int return _h; end Touch; end R; class C public static method Main(): int using var r := new R(); r.Touch(); return 0; end Main; end C;

// NODIAG-NOT: error
// W21_9: warning: the result of this expression statement is unused
