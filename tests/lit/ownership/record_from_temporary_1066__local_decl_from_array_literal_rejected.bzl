// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/record_from_temporary_1066_test.cpp
//   (BEZEL_TEST record_from_temporary_1066, local_decl_from_array_literal_rejected).
//
// The #1066 repro: a local bound to `new Bag(<array literal>)`.

module M; record Bag(Items: Array<int>); record Pt(X: int, Y: int); class C public method Run(xs: Array<int>) var b := new Bag([10, 20, 30]); end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow-bearing record may not be constructed from a temporary bound to its borrow-carrying field `Items` — the temporary's lifetime ends at this construction expression, so the record would immediately outlive that borrow's source (a dangling borrow); bind the source to a named local whose scope outlives the record and pass that
// CHECK-SAME: (§9.7 / §28.3 borrow-lifetime rule)
