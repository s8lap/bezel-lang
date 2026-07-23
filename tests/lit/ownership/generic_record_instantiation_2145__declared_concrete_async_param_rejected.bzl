// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 / §9.7 (v0.220, issue #2145): the DECLARATION-site face of the same
// classification gap — an async method declaring a parameter of an already-
// concrete generic-record instantiation (`Wrap<ArraySpan<int>>`, no method
// type parameter involved). checkAsyncSignatureBorrowBearing runs on the
// resolved declared type, whose genericArgs are concrete, so the fixed
// substituting field walk classifies it borrow-bearing and the §30.1
// declaration-site rejection fires (before the fix the bare-`T` field walk
// answered not-borrow-bearing and the declaration compiled).

module M; record Wrap<T>(V: T); static class F public static async method Bad(x: Wrap<ArraySpan<int>>): Task<int> return 1; end Bad; end F; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `async` parameter `x` has a borrow-bearing type — a span, or a record carrying a non-runtime-managed borrow, may not be a parameter of an `async` method
// CHECK-SAME: (§30.1)
