// Ported from tests/sema/async_lambda_diagnostic_test.cpp
//   (BEZEL_TEST sema_async_lambda_30_6, async_method_unaffected).
//
// CONTROL §30.1 / §30.3: a normal (non-lambda) async METHOD still type-checks.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static async method Producer(): Task<int> return 42; end Producer; public async method Consumer(t: Task<int>): Task<int> var n := await t; return n + 1; end Consumer; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
