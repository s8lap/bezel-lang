// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/scoped_type_identity_2712_test.cpp
//   (BEZEL_TEST scoped_type_identity_2712, plain_to_scoped_conversion_rejected_27_2).
//
// §27.2 (v0.175): the reverse direction is equally rejected — a plain-
// signature method reference does not convert to a scoped-position type.

module M; class C public static method TakePlain(f: () -> ()) f(); end TakePlain; public static method Main(): int var g: (scoped () -> ()) -> () := C.TakePlain; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `g` has a function type whose parameter 1 is not `scoped` but the target's is — modifier slots are part of a function signature's identity; there is no conversion between them
// CHECK-SAME: (§27.1, §27.1.1 v0.180, §27.2/§28.14 v0.175)
