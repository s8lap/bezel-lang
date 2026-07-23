// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 / §9.7 (v0.220, issue #2145): the "not span-specific" control — the
// same generic record instantiated with a CLASS type argument (`Wrap<Res>`).
// §9.7's first borrow-carrying-field kind (a class reference field) applies
// to the substituted `V: Res`, so `Wrap<Res>` is borrow-bearing and the §30.1
// instantiation-site re-check must reject the async parameter. Before the fix
// this compiled clean (bare-`T` field walk).

module M; class Res public method Val(): int return 5; end Val; end Res; record Wrap<T>(V: T); static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int var local := new Res(); var w := new Wrap<Res>(local); var t := F.Work<Wrap<Res>>(w); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: instantiating `async` method `Work` with this type argument makes parameter `x` borrow-bearing — a span, or a record carrying a non-runtime-managed borrow, may not be a parameter of an `async` method
// CHECK-SAME: (§30.1, re-checked at the instantiation site)
