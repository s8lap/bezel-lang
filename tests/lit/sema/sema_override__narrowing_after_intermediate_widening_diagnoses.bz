// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/override_signature_test.cpp
//   (BEZEL_TEST sema_override, narrowing_after_intermediate_widening_diagnoses).
//
// §12.1 (v0.181, #1560): widening is monotone DOWN THE CHAIN — an override
// matches against the NEAREST overridden declaration. Once Mid widens
// Base's `a: string` to `a: string?`, Grand's override matches against
// Mid's (already-widened) declaration, so Grand may keep `string?` but may
// NOT narrow back to `string` — even though `string` is what the original
// (Base) declaration used. (Regression coverage: the override-match check
// must select Mid, the nearest ancestor, as the base to validate against —
// not fall through to Base merely because Grand's narrower signature
// happens to match Base's original, un-widened one.)

module M; class Base public virtual method F(a: string) _ := 1; end F; end Base; class Mid : Base public override method F(a: string?) _ := 2; end F; end Mid; class Grand : Mid public override method F(a: string) _ := 3; end F; end Grand;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `F` does not match the base member's parameter types
// CHECK-SAME: (§12.1)
