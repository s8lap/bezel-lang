// ANTI-OVER-REJECT (§30.1 / §9.7, v0.220, issue #2145 follow-up): the diamond
// with BOTH arms benign — `MRec<int, string>` substitutes a value type and a
// runtime-managed type, so neither `PairR<int>` nor `PairR<string>` carries a
// borrow and the async parameter stays legal. Pins that the path-set cycle
// guard (which re-visits the shared `NRec<X>` node once per sibling frame)
// does not widen the classification: re-walking the same node under a second
// benign env still answers not-borrow-carrying.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record NRec<T>(x: T); record PairR<X>(v: NRec<X>); record MRec<A, B>(p: PairR<A>, q: PairR<B>); static class F public static async method Fine(x: MRec<int, string>): Task<int> return 1; end Fine; end F; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
