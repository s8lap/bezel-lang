// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 (issue #2127 fallout): a span-field record may not be an `async`
// method's RESULT type either — a borrow-bearing result would escape the state-
// machine frame with no bounded source. The parameter here is a plain `Array`
// borrow (allowed — its every use is a disposed-flag-checked member access), so
// this isolates the RESULT-type rejection on `Task<Holder>`. // per §30.1

module M; record Holder(S: ArraySpan<int>); static class F public static async method View(a: Array<int>): Task<Holder> var h := new Holder(a.AsSpan()); return h; end View; end F;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: an `async` method may not have a borrow-bearing result or element type
// CHECK-SAME: (§30.1)
