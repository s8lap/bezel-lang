// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/async_generic_instantiation_borrow_1595_test.cpp
//   (BEZEL_TEST async_generic_instantiation_1595, result_inferred_record_rejected).
//
// Same instantiation as above: the RESULT type `Task<T>` is ALSO substituted
// to the borrow-bearing `Holder` here (T appears in both positions on
// kAsyncIdSrc), so the result leg of the check fires on the very same call.

module M; class Res end Res; record Holder(R: Res); static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int var res := new Res(); var h := new Holder(res); var t := F.Work(h); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: instantiating `async` method `Work` with this type argument makes parameter `x` borrow-bearing — a span, or a record or choice carrying a non-runtime-managed borrow, may not be a parameter of an `async` method: it is captured into the heap state-machine frame that outlives the synchronous call, so the borrow's source is bounded by no stack frame
// CHECK-SAME: (§30.1, §35.4, re-checked at the instantiation site)
