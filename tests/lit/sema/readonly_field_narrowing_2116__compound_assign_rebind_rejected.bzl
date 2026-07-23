// §7.4 (v0.214) / §6.6 / #2116 (pre-gate review): `a += b` desugars to
// `a := a + b` — a REASSIGNMENT rebinding `a` to the user `operator +`'s
// result (§25.1), whose readonly field may hold a different (null) value.
// "A fact rides its root (reassigning `local` drops it)" covers every
// reassignment form, so the fact is dropped and the post-`+=` access is
// REJECTED. (Probe-confirmed SEGV when the stale fact was kept.)
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class B public readonly _t: string?; public constructor(t: string?) _t := t; end constructor; destructor end destructor; public static method operator +(a: B, b: B): owned B => new B(null); end B; class C public static method Main(): int var a := new B("xy"); var b := new B("z"); if a._t <> null then a += b; return a._t.Length; end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)
