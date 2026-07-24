// §22.8 / §22.7 / §7.4 (v0.226) single-assignment (#2187) — ACCEPT control.
//
// Observe-then-single-assign is sound and must stay accepted: the this-escape
// helper observes `_f` while it is still the null watermark — which "establishes
// nothing" per §7.4 — and the ctor then assigns `_f` exactly once. Only the
// re-assignment-after-a-non-null-observation shape is the hole; a single
// assignment (whenever it occurs) is fine.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _f: string?; public constructor(n: string) Helper(); _f := n; end constructor; destructor end destructor; public method Helper(): int if _f <> null then return _f.Length; end; return 0; end Helper; public static method Main(): int using var c := new C("hi"); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §22.8
