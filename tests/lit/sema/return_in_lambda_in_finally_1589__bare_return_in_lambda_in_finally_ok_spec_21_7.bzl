// Ported from tests/sema/return_in_lambda_in_finally_1589_test.cpp
//   (BEZEL_TEST return_in_lambda_in_finally_1589, bare_return_in_lambda_in_finally_ok_spec_21_7).
//
// §21.7: a bare `return;` inside a lambda that is itself declared inside a
// `finally` block is legal — the return targets the lambda, not the method
// whose `finally` encloses it.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Foo(): int var n := 0; try n := n + 1; finally var f := lambda() => do return; end; f(); end; return n; end Foo; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
