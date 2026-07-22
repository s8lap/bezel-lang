// §7.4 (v0.214, proposal 0058, #2085): a `local._f` fact rides its root under
// §7.4's existing captured-local regime. When the root `h` is mutably captured
// by a lambda and a call intervenes between the guard and the use, the root's
// own narrowing is invalidated (a closure could rebind `h`), so the `h._name`
// fact riding it is dropped and the use is REJECTED. (A `this`- or static-rooted
// readonly fact, by contrast, is never invalidated.)
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Holder public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; end Holder; class C public static method Touch() end Touch; public static method Use(h: Holder, h2: Holder): int var f := () => do h := h2; end; if h._name <> null then Touch(); return h._name.Length; end; return -1; end Use; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)
