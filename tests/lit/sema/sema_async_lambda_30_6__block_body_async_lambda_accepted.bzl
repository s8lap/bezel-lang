// Ported from tests/sema/async_lambda_diagnostic_test.cpp
//   (BEZEL_TEST sema_async_lambda_30_6, block_body_async_lambda_accepted).
//
// §30.6 — a block-bodied async lambda type-checks; `await` inside its body is
// valid because the lambda is its own async boundary (§30.2).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static async method Producer(n: int): Task<int> return n * 10; end Producer; public static async method Main(): Task<int> var f: (int) -> Task<int> := async (x) => do var d := await Producer(x); return d + 1; end; using var t := f(2); return await t; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
