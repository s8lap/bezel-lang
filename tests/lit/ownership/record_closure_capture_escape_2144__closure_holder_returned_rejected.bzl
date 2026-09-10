// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §28.3 (v0.220, issue #2144): the INDIRECT form — the record-capturing
// lambda is bound to a holder local `f` first, then the bare holder is
// returned. Binding the closure propagates the record's carried source
// (`local`, via `h`) onto `f` exactly as a direct owned-local capture does
// (borrowSourceForExpr Shape 3), so the existing §28.3/§9.7 indirect-return
// escape check diagnoses `return f;`. Mirrors the #1966 one-hop-through-a-
// local laundering shape, applied to the closure route.

module M; record Holder(S: ArraySpan<int>); class C public static method MakeLeak(): () -> int var local := new Array<int>(); local.Add(7); var h := new Holder(local.AsSpan()); var f := lambda(): int => h.S[0]; return f; end MakeLeak; public static method Main(): int var f := MakeLeak(); return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned `f` carries a borrow of `local` whose source is disposed at scope end
// CHECK-SAME: (§28.3 / §9.7 borrow-lifetime rule
