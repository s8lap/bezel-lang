// Ported from tests/sema/async_lambda_diagnostic_test.cpp
//   (BEZEL_TEST sema_async_lambda_30_6, non_async_lambda_ok).
//
// CONTROL §A.7 / §21.7: a NON-async lambda must be completely unaffected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Apply(f: (int) -> int): int return f(3); end Apply; public static method Main(): int return Apply((x: int) => x + 1); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
