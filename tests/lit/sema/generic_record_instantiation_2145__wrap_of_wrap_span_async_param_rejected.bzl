// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 / §9.7 (v0.220, issue #2145): SELF-NESTED instantiation —
// `Wrap<Wrap<ArraySpan<int>>>`. The outer field `V` substitutes to another
// instantiation of the SAME record declaration, so a visited-set keyed on the
// record TypeDecl alone would block the inner re-visit and vacuously classify
// the whole type not-borrow-bearing (the cycle guard exists for the #1611
// static-field cycle, not for distinct instantiations of one declaration).
// The guard must distinguish instantiations; the type is borrow-bearing by
// §9.7 transitivity (record field → record field → span).

module M; record Wrap<T>(V: T); static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int var local := new Array<int>(); local.Add(7); var w := new Wrap<ArraySpan<int>>(local.AsSpan()); var ww := new Wrap<Wrap<ArraySpan<int>>>(w); var t := F.Work<Wrap<Wrap<ArraySpan<int>>>>(ww); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: instantiating `async` method `Work` with this type argument makes parameter `x` borrow-bearing — a span, or a record or choice carrying a non-runtime-managed borrow, may not be a parameter of an `async` method
// CHECK-SAME: (§30.1, §35.4, re-checked at the instantiation site)
