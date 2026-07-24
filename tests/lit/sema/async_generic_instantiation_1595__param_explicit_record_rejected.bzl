// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/async_generic_instantiation_borrow_1595_test.cpp
//   (BEZEL_TEST async_generic_instantiation_1595, param_explicit_record_rejected).
//
// §30.1 / #1595: the same rejection via an EXPLICIT type argument
// (`Work<Holder>(h)`) — the explicit-type-args substitution path
// (ExprTypecheck_Calls.cpp's `explicitSubst`) is a separate code path from
// argument-inference and must be checked independently.

module M; class Res end Res; record Holder(R: Res); static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int var res := new Res(); var h := new Holder(res); var t := F.Work<Holder>(h); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: instantiating `async` method `Work` with this type argument makes parameter `x` borrow-bearing — a span, or a record or choice carrying a non-runtime-managed borrow, may not be a parameter of an `async` method: it is captured into the heap state-machine frame that outlives the synchronous call, so the borrow's source is bounded by no stack frame
// CHECK-SAME: (§30.1, §35.4, re-checked at the instantiation site)
