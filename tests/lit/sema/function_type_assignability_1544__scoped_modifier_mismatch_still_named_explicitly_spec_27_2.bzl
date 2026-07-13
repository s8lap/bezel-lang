// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, scoped_modifier_mismatch_still_named_explicitly_spec_27_2).
//
// §27.2 / §28.14 (v0.175) control, re-verified alongside the v0.180
// generalization above: matching `scoped` positions are still accepted, and
// the mismatch diagnostic still names `scoped` specifically (not just
// "modifier N").
//
// `Take` returns `owned C` (§28.14 v0.180, #1606: `scoped` on a static
// method's parameter requires an `owned` return so a call site can resolve a
// bounding object) — the mismatch under test is on the PARAMETER's `scoped`
// modifier, independent of the return type.

module M; class C public static method Take(scoped f: () -> ()): owned C f(); return new C(); end Take; public static method Main(): int var h: (() -> ()) -> () := C.Take; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `h` has a function type whose parameter 1 is `scoped` but the target's is not — modifier slots are part of a function signature's identity; there is no conversion between them
// CHECK-SAME: (§27.1, §27.1.1 v0.180, §27.2/§28.14 v0.175)
