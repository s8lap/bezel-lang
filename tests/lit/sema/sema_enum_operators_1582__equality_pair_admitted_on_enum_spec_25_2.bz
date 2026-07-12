// Ported from tests/sema/enum_operators_1582_test.cpp
//   (BEZEL_TEST sema_enum_operators_1582, equality_pair_admitted_on_enum_spec_25_2).
//
// §25.2: `=`/`<>` ARE overloadable on enums (unlike classes) — a homogeneous
// pair with the required co-declaration (§25.2 v0.158/v0.183) is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Priority Low, Medium, High; public static method operator =(a: Priority, b: Priority): bool => int(a) = int(b); public static method operator <>(a: Priority, b: Priority): bool => int(a) <> int(b); public method Equals(other: Priority): bool => int(this) = int(other); public method GetHashCode(): int => int(this); end Priority; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
