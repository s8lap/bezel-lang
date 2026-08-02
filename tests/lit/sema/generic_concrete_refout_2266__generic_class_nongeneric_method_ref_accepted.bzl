// §22.3 / §14.5 (spec v0.228) / #2266 over-rejection control: a NON-generic
// method on a GENERIC class taking a ref of a class-type-parameter-typed slot
// (`Coll<K>.Copy(ref r: [4]K)` called on a `Coll<int>` with a `[4]int`). This is
// the exact shape whose false rejection caused #1756's fourth revert (2bf3eb5);
// the re-land fixed it by threading the receiver (`recvTarget` / `ms->parent`)
// into checkArgsAgainstParams so substituteMemberType resolves `[4]K` to
// `[4]int` before the exact comparison. #2266's per-parameter skip must not
// regress that: the method declares no type parameters of its own, so nothing
// here is deferred to §14.5 — the substituted, fully concrete referent type is
// compared and matches.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Coll<K> private _d: [4]K; public constructor() end constructor; public method Copy(ref r: [4]K, ref n: int) r[0] := _d[0]; n := n + 1; end Copy; destructor end destructor; end Coll; class C public static method Main(): int using var c := new Coll<int>(); var arr: [4]int; var n: int := 0; c.Copy(ref arr, ref n); return n; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
