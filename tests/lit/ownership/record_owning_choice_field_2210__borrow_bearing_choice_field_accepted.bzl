// RUN: %bezelc -c -o %t.o %s
//
// §9.7 / §35.4.2 (v0.226, issue #2210): the ACCEPT (non-over-rejection)
// control for the borrow-bearing case. A record whose choice field is
// BORROW-BEARING (an unmarked class payload, §35.4.2) is NOT owning —
// the record IS borrow-bearing by §9.7 transitivity (covered by #2159),
// and the declaration site must accept it. The borrow-lifetime rules
// apply at USE sites (return escape, instance field store, async
// parameter); the declaration is not the place to reject.

module M; class Res public method V(): int return 5; end V; end Res; choice RefC Some(r: Res), None; end RefC; record HolderC(K: RefC); class C public static method Main(): int using var local := new Res(); var h := new HolderC(RefC.Some(local)); return 0; end Main; end C;
