// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/bool_ordering_1574_test.cpp
//   (BEZEL_TEST sema_bool_ordering_1574, unary_plus_char_rejected_spec_3_5).

module M; class C public method M(a: char) _ := +a; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unary operator `+` is not valid on `char` — unary `+` is the identity, defined only on numeric types
// CHECK-SAME: (§3.5)
