// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/async_generic_instantiation_borrow_1595_test.cpp
//   (BEZEL_TEST async_generic_instantiation_1595, param_explicit_span_rejected).
//
// §30.1 / stdlib §4.15 / #1595: a builtin SPAN type argument
// (`ArraySpan<byte>`, a borrow-bearing VIEW with no user Record declaration,
// §9.7) is rejected the same way a user borrow-bearing record is — the
// shared predicate (common::isBorrowBearingRecordOrSpan) recognises both
// shapes identically.

module M; static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int var arr := new Array<byte>(1); var t := F.Work<ArraySpan<byte>>(arr.AsSpan()); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: instantiating `async` method `Work` with this type argument makes parameter `x` borrow-bearing — a span, or a record or choice carrying a non-runtime-managed borrow, may not be a parameter of an `async` method: it is captured into the heap state-machine frame that outlives the synchronous call, so the borrow's source is bounded by no stack frame
// CHECK-SAME: (§30.1, §35.4, re-checked at the instantiation site)
