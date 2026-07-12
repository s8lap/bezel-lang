// Ported from tests/sema/duplicate_member_test.cpp
//   (BEZEL_TEST sema_dup, method_overloads_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method Add(a: int) _ := 1; end Add; public method Add(a: int, b: int) _ := 2; end Add; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
