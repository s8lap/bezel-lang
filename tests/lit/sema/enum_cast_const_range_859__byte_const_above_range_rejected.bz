// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_cast_const_range_859_test.cpp
//   (BEZEL_TEST enum_cast_const_range_859, byte_const_above_range_rejected).
//
// §5.4: a constant above a byte-backed enum's range is rejected.

module M; enum Color: byte Red := 0; end Color; class C public static method Main(): int var c: Color := Color(300); return int(c); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum cast `Color(300)` — the constant is outside the enum's underlying type range and cannot be represented
// CHECK-SAME: (§5.4)
