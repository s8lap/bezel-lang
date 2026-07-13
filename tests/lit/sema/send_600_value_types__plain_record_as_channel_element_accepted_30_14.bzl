// Ported from tests/sema/send_600_sendability_value_types_test.cpp
//   (BEZEL_TEST send_600_value_types, plain_record_as_channel_element_accepted_30_14).
//
// §30.14: A record with only primitive fields is not borrow-bearing (§9.7) and
// not move-only (§15.3) → sendable → Channel<Point> is valid.
// This test expects NO error. Against current main it will fail because Channel
// is an unknown type.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point public X: int := 0; public Y: int := 0; end Point; class C public static method Test() using var ch := new Channel<Point>(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
