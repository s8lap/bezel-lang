// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_operators_1582_test.cpp
//   (BEZEL_TEST sema_enum_operators_1582, override_gethashcode_still_rejected_on_enum_spec_10_5).
//
// §10.5 (v0.183): `override` on the co-declared `GetHashCode` is still
// rejected on an enum — enums dispatch statically and never carry an
// override chain. This confirms the §10.5 restriction and the §25.2
// co-declaration rule compose correctly: the required member is the PLAIN
// spelling, and the `override` spelling remains illegal.

module M; enum Priority Low, Medium, High; public static method operator =(a: Priority, b: Priority): bool => int(a) = int(b); public static method operator <>(a: Priority, b: Priority): bool => int(a) <> int(b); public method Equals(other: Priority): bool => int(this) = int(other); public override method GetHashCode(): int => int(this); end Priority; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum method `GetHashCode` may not be virtual, abstract, or override — enums dispatch statically
// CHECK-SAME: (§10.5)
