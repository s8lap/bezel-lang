// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/override_signature_test.cpp
//   (BEZEL_TEST sema_override, ref_value_type_param_widening_still_declined).
//
// #1592: unlike a plain (by-value) parameter, a `ref`/`out` VALUE-typed
// parameter widening stays declined even now — it aliases the CALLER's own
// storage directly (no thunk-owned copy to marshal), and that storage is
// sized for the narrow, un-boxed `int`, with no room for the boxed form's
// extra hasValue byte.

module M; class Base public virtual method F(ref a: int) a := a + 1; end F; end Base; class D : Base public override method F(ref a: int?) a := 2; end F; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `F` does not match the base member's parameter types
// CHECK-SAME: (§12.1)
