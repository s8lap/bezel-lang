// Ported from tests/sema/bool_condition_test.cpp
//   (BEZEL_TEST sema_cond, if_comparison_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(flag: bool, n: int) if n > 0 then n := 1; end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
