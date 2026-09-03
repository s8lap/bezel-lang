// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lambda_param_no_target_type_reject_3756_test.cpp
//   (BEZEL_TEST lambda_param_no_target_type_reject_3756,
//    no_modifier_no_annotation_no_context_rejected_spec_27_4).
//
// The no-modifier twin of #3756's repro — no `owned`, no type annotation,
// no target-type context. Measured on this tree to ALSO silently default
// pre-fix (same wrong-value symptom as the `owned`-modifier repro), so the
// fix's reject condition is not keyed on the modifier at all.

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; end R; class C public static method Take(p: owned R): int var h := lambda(c): int => c.V(); return h(move p); end Take; public static method Main(): int Console.WriteLine(Take(new R()).ToString()); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: parameter `c` needs a type annotation: no target type to infer from
// CHECK-SAME: (§27.4)
