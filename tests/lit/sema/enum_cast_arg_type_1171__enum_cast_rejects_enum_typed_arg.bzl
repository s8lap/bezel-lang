// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_cast_const_range_859_test.cpp
//   (BEZEL_TEST enum_cast_arg_type_1171, enum_cast_rejects_enum_typed_arg).
//
// §5.4/§5.1: an enum-typed argument to a *different* enum's cast is rejected —
// it must be converted to the underlying integer first via `int(e)`.

module M; enum A X, Y, Z; end A; enum B P, Q, R; end B; class C public static method Main(): int var a: A := A.Y; var b: B := B(a); return int(b); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the argument to an enum cast `B(...)` must be an integer of the enum's underlying type (use `int(e)` to convert an enum first)
// CHECK-SAME: (§5.4/§5.1)
