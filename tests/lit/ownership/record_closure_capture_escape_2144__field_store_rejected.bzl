// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §28.3 (v0.220, issue #2144): the FIELD-STORE escape route — a lambda
// capturing a borrow-bearing record (`h`, span field) is stored into an
// instance field. The field outlives the method call, but the captured
// record's carried borrow of `local` dies at scope end (§28.3 auto-drop), so
// invoking the stored closure later reads the dangling span. The closure's
// holder inherits the record's carried source (borrowSourceForExpr Shape 3),
// and the existing §28.3 field-store borrow escape check rejects the store —
// the same route that already rejects `this.f := lambda() => r.V` for a
// directly-captured owned local `r`.

module M; record Holder(S: ArraySpan<int>); class C public cb: () -> int; public constructor() cb := lambda() => -1; end constructor; public method Arm() var local := new Array<int>(); local.Add(7); var h := new Holder(local.AsSpan()); cb := lambda(): int => h.S[0]; end Arm; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow of `local` stored into a field would outlive its source
// CHECK-SAME: (§28.3
