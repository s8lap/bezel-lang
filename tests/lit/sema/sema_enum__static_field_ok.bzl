// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, static_field_ok).
//
// fix #3840 (§10.4 / §35.2, v0.288): a mutable static field is not part of
// §10.4's exhaustive admitted-member list — it is admitted only with §22.12's
// declaration-site warning on a class/record body; an enum/choice has no storage
// for a mutable static and no mechanism to confine its writes to one domain.
// The old ACCEPT-with-warning was the retired v0.221 reading; the v0.288 spec
// retires that and requires rejection.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; enum E A, B; public static _x: int := 0; end E;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum `E` may not declare a mutable static field (§10.5)
