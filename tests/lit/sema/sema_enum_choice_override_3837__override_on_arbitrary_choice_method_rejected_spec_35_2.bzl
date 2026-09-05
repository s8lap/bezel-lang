// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §35.2 (v0.288, #3837): the widened admission is exactly the three
// `Object`-virtual members — `override` on any OTHER choice method must
// still reject (do not over-widen). Control for the #3837 admission fix.

module M; choice Shape Circle(radius: double), Square(side: double); public override method Foo(): int => 1; end Shape; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: choice method `Foo` may not be virtual, abstract, or override
// CHECK-SAME: (§35.2)
