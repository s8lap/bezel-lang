// §7.4 (v0.214) / §17.6 / #2116 (pre-gate review): `a++` desugars to
// `a := B.operator++(a)` — a reassignment rebinding `a` to the operator's
// result, so a `a._t` readonly-field fact rode the OLD referent and drops;
// the post-increment access is REJECTED.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class B public readonly _t: string?; public constructor(t: string?) _t := t; end constructor; destructor end destructor; public static method operator ++(a: B): owned B => new B(null); end B; class C public static method Main(): int var a := new B("xy"); if a._t <> null then a++; return a._t.Length; end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)
