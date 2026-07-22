// Ported from tests/sema/bool_condition_test.cpp
//   (BEZEL_TEST sema_cond, if_expression_bool_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(flag: bool, n: int) n := (if flag then 1 else 2); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
