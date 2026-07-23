// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §28.11 (issue #2127 fallout): a span-field record copied into a
// collection reachable beyond the method's scope (here `this.list`, a field) is
// rejected — "a store into a collection that is a field, is reachable from one,
// escapes the method … is rejected at the store site … there is no trust
// residual for record-carried borrows." A span's access paths bypass the
// disposed-flag check the escape analysis relies on for a direct borrow, so the
// store is rejected regardless of whether the source provably outlives it.
// // per §9.7, §28.11

module M; record Holder(S: ArraySpan<int>); class Box public owned list: Array<Holder>; public constructor() list := new Array<Holder>(); end constructor; public method Put(h: Holder) this.list.Add(h); end Put; end Box;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow-bearing record passed to `.Add(...)` on `this` may not be copied into a collection reachable beyond this method's scope
// CHECK-SAME: (§9.7 / §28.11
