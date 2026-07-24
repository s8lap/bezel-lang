// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 / §9.7 (v0.220, issue #2145): the type parameter mentioned INSIDE a
// declared generic-record parameter type — `Work2<T>(x: Wrap<T>)` called at
// `T := ArraySpan<int>`. The substitution engine rebuilds `Wrap<T>` as
// `Wrap<ArraySpan<int>>` (nested genericArgs substitution), and the fixed
// instantiation-site classification must then see the substituted field
// `V : ArraySpan<int>` and reject — the dual of the bare-`T`-parameter
// repro, exercising the declared-type substitution route.

module M; record Wrap<T>(V: T); static class F public static async method Work2<T>(x: Wrap<T>): Task<int> return 1; end Work2; end F; class C public static method Main(): int var local := new Array<int>(); local.Add(7); var w := new Wrap<ArraySpan<int>>(local.AsSpan()); var t := F.Work2<ArraySpan<int>>(w); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: instantiating `async` method `Work2` with this type argument makes parameter `x` borrow-bearing — a span, or a record or choice carrying a non-runtime-managed borrow, may not be a parameter of an `async` method
// CHECK-SAME: (§30.1, §35.4, re-checked at the instantiation site)
