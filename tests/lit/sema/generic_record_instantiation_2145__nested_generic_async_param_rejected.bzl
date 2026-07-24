// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 / §9.7 (v0.220, issue #2145): NESTED generic-record substitution —
// `Outer<Res>` whose field is `Inner<T>`, so the borrow-carrying field is
// reached only by composing the outer instantiation's substitution (T := Res)
// through the inner record's own field walk (`Inner<Res>.V : Res`, a class
// reference, §9.7 first kind; then §9.7 transitivity makes `Outer<Res>`
// borrow-bearing through its record-typed field). One-level substitution of
// the directly-mentioned field type alone would miss this shape.

module M; class Res public method Val(): int return 5; end Val; end Res; record Inner<U>(V: U); record Outer<T>(I: Inner<T>); static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int var local := new Res(); var i := new Inner<Res>(local); var o := new Outer<Res>(i); var t := F.Work<Outer<Res>>(o); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: instantiating `async` method `Work` with this type argument makes parameter `x` borrow-bearing — a span, or a record or choice carrying a non-runtime-managed borrow, may not be a parameter of an `async` method
// CHECK-SAME: (§30.1, §35.4, re-checked at the instantiation site)
