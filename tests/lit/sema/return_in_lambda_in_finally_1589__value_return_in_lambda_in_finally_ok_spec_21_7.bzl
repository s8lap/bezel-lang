// Ported from tests/sema/return_in_lambda_in_finally_1589_test.cpp
//   (BEZEL_TEST return_in_lambda_in_finally_1589, value_return_in_lambda_in_finally_ok_spec_21_7).
//
// §21.7: same, but the lambda returns a value (matches issue #1589's repro
// shape: a loop with a `break` inside the lambda, then `return i;`).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Foo(): int var n := 0; try n := n + 1; finally var f := lambda() => do var i := 0; while i < 3 do i := i + 1; if i = 2 then break; end; end; return i; end; n := n + f(); end; return n; end Foo; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
