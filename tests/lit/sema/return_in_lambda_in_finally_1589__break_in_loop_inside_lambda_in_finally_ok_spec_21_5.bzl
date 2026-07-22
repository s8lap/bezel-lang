// Ported from tests/sema/return_in_lambda_in_finally_1589_test.cpp
//   (BEZEL_TEST return_in_lambda_in_finally_1589, break_in_loop_inside_lambda_in_finally_ok_spec_21_5).
//
// §21.5/§31.2 sanity (companion to #1578): `break` targeting a loop declared
// wholly inside the lambda's own body, where the lambda sits inside an
// enclosing `finally`, is also legal — the same lambda-boundary reset covers
// `finallyLoopBoundary_` alongside `finallyDepth_`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Foo(): int var n := 0; try n := n + 1; finally var f := lambda() => do var total := 0; while total < 10 do total := total + 1; if total = 3 then break; end; end; return total; end; n := n + f(); end; return n; end Foo; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
