// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, new_trivial_dispose_ok).
//
// §21.9: `new T();` for a TRIVIAL-dispose class (no destructor) — accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no ERROR.
//
// fix #6555 (§21.9, v0.338) flipped the second control here. This row pinned
// "no diagnostics of any severity", which §21.9's base rule — the unused-result
// WARNING on an expression statement whose result is non-`()` — now contradicts:
// `new Plain();` produces a `Plain` and discards it. The row's SUBJECT is
// unchanged and is what the flip must not disturb: a trivial-dispose
// production is still ACCEPTED (and still reclaimed by §21.9's
// `disposeOwnedValue` path — #2655), because only the NON-trivial-dispose case
// escalates to a hard error. So `NODIAG-NOT: error` stays and the old blanket
// `NODIAG-NOT: §` is replaced by a W21_9 check pinning the warning §21.9 now
// requires — a strengthening, not a relaxation.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=W21_9 --allow-empty %s

module M; class Plain public _n: int := 0; end Plain; class C public static method Main(): int new Plain(); return 0; end Main; end C;

// NODIAG-NOT: error
// W21_9: warning: the result of this expression statement is unused
