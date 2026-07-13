// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unary_minus_1588_test.cpp
//   (BEZEL_TEST sema_unary_minus_1588, unary_minus_string_rejected_spec_3_5).
//
// The reported bug's worst variant: `-stringExpr` used to reach codegen and
// crash on LLVM IR verification (an integer `sub` on a string's ptr
// representation) instead of being rejected here in sema.

module M; class C public method M(a: string) _ := -a; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unary operator `-` is not valid on `string` — unary `-` (negation) is defined only on numeric types
// CHECK-SAME: (§3.5)
