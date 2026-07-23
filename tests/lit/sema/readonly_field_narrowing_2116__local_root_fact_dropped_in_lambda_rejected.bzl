// §7.4 (v0.214) / #2116: a LOCAL-rooted fact (`h._name`) does NOT ride into a
// lambda body: the closure executes at an arbitrary later time and the captured
// root may be rebound between the closure's creation and its invocation — the
// enclosing walk cannot see that. Only ambient-rooted (`this`/static) facts
// cross the boundary. The lambda body's `h._name.Length` is REJECTED.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Holder public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; end Holder; class C public static method Use(h: Holder): int if h._name <> null then var f := lambda() => h._name.Length; return f(); end; return -1; end Use; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)
