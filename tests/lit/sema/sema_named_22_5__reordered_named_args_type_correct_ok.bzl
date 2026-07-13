// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, reordered_named_args_type_correct_ok).
//
// §22.5 + §5.1: a reordered named-arg call that is type-correct in the
// parameter it actually fills must NOT be flagged — the int literal binds
// to the int parameter `a` regardless of source order.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F(a: int, b: long) end F; public method G() this.F(b := 2L, a := 1); end G; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
