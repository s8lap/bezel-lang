// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 / §9.7 (v0.220, issue #2145 follow-up): the diamond's MIRROR order —
// `MRec<Res, int>` puts the borrow-carrying instantiation FIRST, so even a
// visited-set guard would classify it (the walk answers true before the
// benign sibling is reached). Paired with the benign-first sibling test, this
// pins that the classification is ORDER-INDEPENDENT: both instantiation
// orders of the same diamond reject identically.

module M; class Res public method Val(): int return 5; end Val; end Res; record NRec<T>(x: T); record PairR<X>(v: NRec<X>); record MRec<A, B>(p: PairR<A>, q: PairR<B>); static class F public static async method Bad(x: MRec<Res, int>): Task<int> return 1; end Bad; end F; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `async` parameter `x` has a borrow-bearing type — a span, or a record carrying a non-runtime-managed borrow, may not be a parameter of an `async` method
// CHECK-SAME: (§30.1)
