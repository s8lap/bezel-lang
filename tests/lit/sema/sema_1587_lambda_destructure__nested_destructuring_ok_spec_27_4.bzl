// Ported from tests/sema/lambda_param_record_destructuring_1587_test.cpp
//   (BEZEL_TEST sema_1587_lambda_destructure, nested_destructuring_ok_spec_27_4).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Inner(A: int, B: int); record Outer(I: Inner, C: int); class C public static method Main(): int var f: (Outer) -> int := (Outer(Inner(a, b), c)) => a + b + c; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
