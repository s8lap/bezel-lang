// fix #3641 (§12.5 / §23.4, v0.286): a THIRD level down the chain — the
// override satisfying the §12.5 obligation need not be `t`'s own
// declaration, only somewhere on `t`'s base-CLASS chain. `A` declares the
// abstract indexer; `B` overrides it (satisfying `B`'s own obligation);
// `C: B` inherits the already-satisfied override and declares nothing of
// its own — the abstract-member completeness walk's candidate search must
// find `B`'s override during `C`'s own base-chain walk, not require a
// per-level re-declaration. Per review of this fix's initial commit: the
// corpus had no test where the satisfying override sits ABOVE the class
// under check rather than ON it (every existing case — including this
// file's own sibling ACCEPT control — has the override on `t` itself), so
// a future refactor of the BFS/`candWireable` machinery could regress this
// shape silently. Also exercises `checkAbstractImplementations`'s
// `classSubst`/BFS reaching `C` a full two hops from the abstract
// declarer, past an intermediate class that is itself concrete (not
// abstract or closed).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; public abstract class A public abstract property [i: int]: int; end A; public class B: A public override property [i: int]: int => i * 2; end B; public class C: B end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
