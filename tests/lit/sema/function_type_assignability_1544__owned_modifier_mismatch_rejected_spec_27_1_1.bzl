// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, owned_modifier_mismatch_rejected_spec_27_1_1).
//
// §27.1.1 fourth bullet: `owned` likewise must match exactly.

module M; class Box end Box; class C public static method Take(owned b: Box) end Take; public static method Main(): int var f: (owned Box) -> () := Take; var g: (Box) -> () := f; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `g` has a function type whose parameter 1 is `owned` but the target's is (none) — modifier slots are part of a function signature's identity; there is no conversion between them
// CHECK-SAME: (§27.1, §27.1.1 v0.180, §27.2/§28.14 v0.175)
