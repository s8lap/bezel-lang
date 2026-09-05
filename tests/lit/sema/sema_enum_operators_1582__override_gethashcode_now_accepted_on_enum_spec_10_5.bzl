// Ported from tests/sema/enum_operators_1582_test.cpp
//   (BEZEL_TEST sema_enum_operators_1582, override_gethashcode_still_rejected_on_enum_spec_10_5).
//
// §10.5 (v0.288, #3837): `override` on the co-declared `GetHashCode` is now
// ADMITTED on an enum — the two spellings (`override` or plain) declare the
// SAME member, since enums dispatch statically and have no override chain
// or `base` access: `override` here just names the `Object` slot the member
// fills. This file used to pin the OPPOSITE (pre-v0.288) rejection; flipped
// to an accept control by #3837 alongside the compiler admission fix, in
// the same commit (the spec-ahead-of-impl gap #3837 tracked).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Priority Low, Medium, High; public static method operator =(a: Priority, b: Priority): bool => int(a) = int(b); public static method operator <>(a: Priority, b: Priority): bool => int(a) <> int(b); public method Equals(other: Priority): bool => int(this) = int(other); public override method GetHashCode(): int => int(this); end Priority; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
