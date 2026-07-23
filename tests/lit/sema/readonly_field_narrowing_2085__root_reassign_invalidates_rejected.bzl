// §7.4 (v0.214, proposal 0058, #2085): a `local._f` fact rides its root —
// `obj.F` narrows exactly while `obj`'s own tracking survives; reassigning `obj`
// invalidates the path's facts (the new referent's field is a different value).
// After `h := h2` the earlier `h._name` fact is dropped, so the use is REJECTED.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Holder public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; end Holder; class C public static method Use(h: Holder, h2: Holder): int if h._name <> null then h := h2; return h._name.Length; end; return -1; end Use; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)
