// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_int_conversion_924_test.cpp
//   (BEZEL_TEST enum_int_conversion_924, new_on_enum_rejected_960).
//
// §5.4 / §10 (#960): `new` cannot construct an enum — an enum value is produced
// by the int→enum cast `E(n)` (§5.4), not by construction. `new E(...)` and the
// nested `new Outer.E(...)` form were silently accepted.

module M; enum E A, B, C; end E; class P public static method Main(): int var e := new E(1); return 0; end Main; end P;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `new` cannot construct an enum — an enum value is produced by the cast `E(n)` from its underlying integer, not with `new`
// CHECK-SAME: (§5.4/§10)
