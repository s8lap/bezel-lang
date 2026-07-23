// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 / §9.7 / §14.5 (v0.220, issue #2145): the same rejection via ARGUMENT
// INFERENCE (`F.Work(w)` — no explicit type argument list). The inference
// substitution path is separate code from the explicit-type-args path
// (mirrors the #1595 explicit/inferred sibling pair) and must classify the
// inferred `T = Wrap<ArraySpan<int>>` identically.

module M; record Wrap<T>(V: T); static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int var local := new Array<int>(); local.Add(7); var w := new Wrap<ArraySpan<int>>(local.AsSpan()); var t := F.Work(w); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: instantiating `async` method `Work` with this type argument makes parameter `x` borrow-bearing — a span, or a record carrying a non-runtime-managed borrow, may not be a parameter of an `async` method
// CHECK-SAME: (§30.1, re-checked at the instantiation site)
