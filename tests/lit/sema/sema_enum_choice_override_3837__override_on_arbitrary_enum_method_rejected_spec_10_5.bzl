// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §10.5 (v0.288, #3837): the widened admission is exactly the three
// `Object`-virtual members (`Equals`, `GetHashCode`, `ToString`) — `override`
// on any OTHER enum method must still reject (do not over-widen). Control
// for the #3837 admission fix.

module M; enum Priority Low, Medium, High; public override method Foo(): int => 1; end Priority; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum method `Foo` may not be virtual, abstract, or override
// CHECK-SAME: (§10.5)
