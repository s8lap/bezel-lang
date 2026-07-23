// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 / §9.7 (v0.220, issue #2145): "for a generic async method the check is
// re-applied after substitution, so an instantiation whose type argument makes
// a parameter borrow-bearing is rejected at the instantiation site" (§30.1).
// The type argument here is a GENERIC RECORD instantiated with a span field
// arg — `Wrap<ArraySpan<int>>` — which §9.7 classifies borrow-bearing (field
// `V: T` substituted at a span, a borrow-carrying type). Before the fix the
// classification walked `Wrap`'s field with the UNINSTANTIATED bare `T` and
// answered not-borrow-bearing, so the §30.1 re-check never fired and this
// compiled clean (the issue's exact repro).

module M; record Wrap<T>(V: T); static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int var local := new Array<int>(); local.Add(7); var w := new Wrap<ArraySpan<int>>(local.AsSpan()); var t := F.Work<Wrap<ArraySpan<int>>>(w); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: instantiating `async` method `Work` with this type argument makes parameter `x` borrow-bearing — a span, or a record carrying a non-runtime-managed borrow, may not be a parameter of an `async` method
// CHECK-SAME: (§30.1, re-checked at the instantiation site)
