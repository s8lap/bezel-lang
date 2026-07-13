// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_cast_const_range_859_test.cpp
//   (BEZEL_TEST enum_cast_arg_type_1171, enum_cast_rejects_bool_arg).
//
// §5.4/§5.1: a `bool` argument to an enum cast is rejected.

module M; enum Color Red, Green, Blue; end Color; class C public static method Main(): int var c: Color := Color(true); return int(c); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the argument to an enum cast `Color(...)` must be an integer of the enum's underlying type (use `int(e)` to convert an enum first)
// CHECK-SAME: (§5.4/§5.1)
