// §7.4 (v0.214) / #2116: passing the root as a `ref` argument invalidates the
// facts riding it — the callee may rebind `h` to a different object whose
// readonly field is a different (possibly null) value. After `Mut(ref h)` the
// earlier `h._name` fact is dropped, so the use is REJECTED (mirrors the §7.4
// ref/out invalidation of a local's own narrowing).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Holder public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; end Holder; class C public static method Mut(ref h: Holder) end Mut; public static method Use(h: Holder): int if h._name <> null then Mut(ref h); return h._name.Length; end; return -1; end Use; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)
