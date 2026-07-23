// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 (issue #2127 fallout): a span-field record is borrow-bearing, so it may
// not be an `async` method parameter — it is captured into the heap state-
// machine frame that outlives the synchronous call, so the borrow's source is
// bounded by no stack frame. Before the fix `Holder` was not classified borrow-
// bearing, so checkAsyncSignatureBorrowBearing skipped it. // per §30.1

module M; record Holder(S: ArraySpan<int>); static class F public static async method Work(h: Holder): Task<int> return h.S[0]; end Work; end F;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `async` parameter `h` has a borrow-bearing type
// CHECK-SAME: (§30.1)
