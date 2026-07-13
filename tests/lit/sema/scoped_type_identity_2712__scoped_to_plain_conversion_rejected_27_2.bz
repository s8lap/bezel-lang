// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/scoped_type_identity_2712_test.cpp
//   (BEZEL_TEST scoped_type_identity_2712, scoped_to_plain_conversion_rejected_27_2).
//
// §27.2 (v0.175): a method with a `scoped` parameter has the scoped position
// in its FUNCTION-TYPE identity; the method reference does not convert to the
// plain-signature function type.
//
// `Take` returns `owned C` (§28.14 v0.180, #1606: a `scoped` parameter on a
// static method requires an `owned` return so a call site can resolve a
// bounding object) — the identity mismatch under test is on the PARAMETER's
// `scoped` modifier, independent of the return type.

module M; class C public static method Take(scoped f: () -> ()): owned C f(); return new C(); end Take; public static method Main(): int var h: (() -> ()) -> () := C.Take; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `h` has a function type whose parameter 1 is `scoped` but the target's is not — modifier slots are part of a function signature's identity; there is no conversion between them
// CHECK-SAME: (§27.1, §27.1.1 v0.180, §27.2/§28.14 v0.175)
