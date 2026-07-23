// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 / §9.7 (v0.220, issue #2145 follow-up): the DIAMOND — one generic
// record (`PairR<X>`, field `v: NRec<X>`) instantiated TWICE inside a single
// outer record, benign first (`MRec<int, Res>`: p at X:=int, q at X:=Res).
// Both walks reach the SAME cached field TypeRef `NRec<X>` (one AST node in
// PairR's declaration); a visited-set cycle guard would let the benign X:=int
// visit permanently mark it, vacuously classifying the borrow-carrying X:=Res
// sibling — an order-dependent unsound accept. The guard is a DFS PATH set
// (erased on frame exit), so each sibling frame re-classifies the node under
// its own substitution env and `MRec<int, Res>` is borrow-bearing via
// `q.v.x : Res` (§9.7 first kind, class reference), rejected as an `async`
// parameter exactly like its `MRec<Res, int>` mirror.

module M; class Res public method Val(): int return 5; end Val; end Res; record NRec<T>(x: T); record PairR<X>(v: NRec<X>); record MRec<A, B>(p: PairR<A>, q: PairR<B>); static class F public static async method Bad(x: MRec<int, Res>): Task<int> return 1; end Bad; end F; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `async` parameter `x` has a borrow-bearing type — a span, or a record carrying a non-runtime-managed borrow, may not be a parameter of an `async` method
// CHECK-SAME: (§30.1)
