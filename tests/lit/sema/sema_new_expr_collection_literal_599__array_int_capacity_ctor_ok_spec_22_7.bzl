// Ported from tests/sema/new_expr_collection_literal_599_test.cpp
//   (BEZEL_TEST sema_new_expr_collection_literal_599, array_int_capacity_ctor_ok_spec_22_7).
//
// §22.7 (#599 must-not-regress): `new Array<int>(8)` — valid capacity
// constructor; an `int` literal is a well-typed `int` argument.
// Must NOT produce an ERROR.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no ERROR.
//
// fix #6555 (§21.9, v0.338) flipped the second control here: `new Array<int>(8);`
// as a bare statement discards a non-`()` result, which §21.9's base
// unused-result rule now warns on. The row's SUBJECT — §22.7 admitting this
// capacity constructor with an `int` literal — is untouched by the flip, and
// the "must NOT produce a diagnostic" claim is narrowed to what #599 was
// actually about: no diagnostic ABOUT THE CONSTRUCTOR. `NODIAG-NOT: error`
// (the acceptance) stays; the old blanket `NODIAG-NOT: §` is replaced by a
// W21_9 check pinning the §21.9 warning — a strengthening, not a relaxation.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=W21_9 --allow-empty %s

module M; class C public static method M() new Array<int>(8); end M; end C;

// NODIAG-NOT: error
// W21_9: warning: the result of this expression statement is unused
