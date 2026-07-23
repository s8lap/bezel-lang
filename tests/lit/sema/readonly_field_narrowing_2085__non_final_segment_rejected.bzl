// §7.4 (v0.214, proposal 0058, #2085): the tracked path is exactly ONE field
// off an admitted root; any non-final field segment makes a path non-trackable
// (`a.b._f` establishes nothing — `a.b` is a mutable read). Guarding
// `o._inner._name <> null` narrows nothing, so the use is REJECTED.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Inner public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; end Inner; class Outer public readonly _inner: Inner; public constructor(i: Inner) _inner := i; end constructor; destructor end destructor; end Outer; class C public static method Use(o: Outer): int if o._inner._name <> null then return o._inner._name.Length; end; return -1; end Use; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)
