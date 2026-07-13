// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_cast_const_range_859_test.cpp
//   (BEZEL_TEST enum_cast_const_range_859, short_const_above_range_rejected).
//
// §5.4: a constant above a short-backed enum's range is rejected.

module M; enum HttpStatus: short Ok := 200; end HttpStatus; class C public static method Main(): int var s: HttpStatus := HttpStatus(40000); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum cast `HttpStatus(40000)` — the constant is outside the enum's underlying type range and cannot be represented
// CHECK-SAME: (§5.4)
