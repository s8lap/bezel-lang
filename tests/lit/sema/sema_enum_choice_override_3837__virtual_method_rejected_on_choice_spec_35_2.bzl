// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §35.2 (v0.288, #3837): "Choice methods are statically dispatched; `virtual`
// and `abstract` are rejected as on enums (§10.5)." MEASURED pre-fix: nothing
// rejected this at all — a choice method marked `virtual` compiled with zero
// diagnostics (checkEnumRestrictions is enum-only; the general base-class
// override-match walk never populates candidates for a choice, whose
// `baseTypes` holds only Interface-kind entries, never Class). This is
// therefore a regression test for a NEW check (checkChoiceDispatchModifiers),
// not a widening of an existing one.

module M; choice Shape Circle(radius: double), Square(side: double); public virtual method Area(): double => 0.0; end Shape; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: choice method `Area` may not be virtual, abstract, or override
// CHECK-SAME: (§35.2)
