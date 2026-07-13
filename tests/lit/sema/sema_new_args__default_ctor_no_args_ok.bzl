// Ported from tests/sema/new_expr_args_test.cpp
//   (BEZEL_TEST sema_new_args, default_ctor_no_args_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class T end T; class C public method M() _ := new T(); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
