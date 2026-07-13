// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/with_visibility_934_test.cpp
//   (BEZEL_TEST with_visibility_934, private_property_with_from_external_rejected).
//
// §13.1, #934 — REJECT: `with` targets a private auto-property from an
// external class. Before the fix, this was a BAD_ACCEPT (silently compiled).

module M; record MyRec private property Secret: int; public constructor(s: int) Secret := s; end constructor; end MyRec; class C public static method Main(): int var r := new MyRec(42); var r2 := r with Secret := 99 end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `Secret` is private to type `MyRec` and not accessible from `C`
// CHECK-SAME: (§13.1 — `private` is visible to the declaring class only)
