// Ported from tests/sema/new_expr_collection_literal_599_test.cpp
//   (BEZEL_TEST sema_new_expr_collection_literal_599, array_int_default_ctor_ok_spec_22_7).
//
// §22.7 (#599 must-not-regress): `new Array<int>()` — default (no-arg)
// constructor; must NOT produce a diagnostic.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M() using var arr := new Array<int>(); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
