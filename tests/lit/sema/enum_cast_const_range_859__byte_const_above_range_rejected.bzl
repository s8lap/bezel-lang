// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_cast_const_range_859_test.cpp
//   (BEZEL_TEST enum_cast_const_range_859, byte_const_above_range_rejected).
//
// §5.4: a constant above a byte-backed enum's range is rejected.
//
// v0.242 (#3044): the VERDICT is unchanged — the message is not. §5.4's checked
// rule subsumes the former range check ("In-range-vs-out-of-range is not a
// user-visible distinction: an `n` outside the underlying type's range is, a
// fortiori, equal to no declared constant, so the single rule subsumes the
// former range check"), so one diagnostic now serves both.

module M; enum Color: byte Red := 0; end Color; class C public static method Main(): int var c: Color := Color(300); return int(c); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum cast `Color(300)` — 300 is not the underlying value of any declared constant of `Color`
// CHECK-SAME: (§5.4)
