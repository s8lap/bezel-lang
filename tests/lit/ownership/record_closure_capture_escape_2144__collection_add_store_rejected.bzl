// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §28.11 (v0.220, issue #2144): the COLLECTION-ADD escape route — a
// lambda capturing a borrow-bearing record (`h`, span field) is `.Add`ed into
// a field-reachable collection, which outlives the method; the captured
// record's carried borrow of `local` dies at scope end (§28.3 auto-drop), so
// invoking the stored closure later reads the dangling span. The closure's
// element-store borrow source resolves through the captured record to `local`
// (borrowSourceForExpr Shape 3), and the §28.11 escaping-collection element
// store check rejects it.

module M; record Holder(S: ArraySpan<int>); class C public owned cbs: Array<() -> int>; public constructor() cbs := new Array<() -> int>(); end constructor; public method Arm() var local := new Array<int>(); local.Add(7); var h := new Holder(local.AsSpan()); cbs.Add(lambda(): int => h.S[0]); end Arm; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow of `local` passed to `.Add(...)` on `cbs` would outlive its source
// CHECK-SAME: (§28.11 borrowing-collection store rule
