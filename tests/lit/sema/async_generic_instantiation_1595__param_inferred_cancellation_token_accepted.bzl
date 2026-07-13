// Ported from tests/sema/async_generic_instantiation_borrow_1595_test.cpp
//   (BEZEL_TEST async_generic_instantiation_1595, param_inferred_cancellation_token_accepted).
//
// §30.1 / §30.8 / #1595: `CancellationToken` is the single stdlib-certified
// borrow-bearing-record exception (its source-outlives-settlement obligation
// is certified normatively, §30.8) — admitted as a type argument that binds
// a parameter/result to it, exactly as it is admitted at the declaration
// site (#1518 `param_cancellation_token_accepted`).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int var t := F.Work(CancellationToken.None); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
