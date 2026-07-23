// §7.4 (v0.214) / #2116: a drop performed INSIDE a condition expression stays
// dropped in the branch bodies. `Mut(ref h)` may rebind `h`, so the `h._name`
// fact riding it is dropped by the ref-argument invalidation during the inner
// if's CONDITION visit — and the inner then-branch must NOT resurrect it from
// the statement-entry snapshot. The use is REJECTED. (Branch entries derive
// from the drop-respecting condition-context, not the entry state; the legacy
// local sets resurrect here — #2164 addendum, probe-confirmed SIGSEGV.)
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Holder public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; end Holder; class C public static method Mut(ref h: Holder): bool return true; end Mut; public static method Use(h: Holder): int if h._name <> null then if Mut(ref h) then return h._name.Length; end; end; return -1; end Use; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)
