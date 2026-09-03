// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lambda_param_no_target_type_reject_3756_test.cpp
//   (BEZEL_TEST lambda_param_no_target_type_reject_3756,
//    owned_modifier_no_annotation_no_context_rejected_spec_27_4).
//
// Issue #3756's exact repro: an `owned`-modifier lambda parameter with no
// type annotation, bound via `var` with no target-type context. §27.4: "In
// top-level contexts with no target type ... parameter types must be
// specified." Pre-fix this compiled cleanly and silently printed `0`
// instead of `5` (the correct answer) — the wrong-value symptom is why this
// is pinned as a REJECT, not merely a diagnostic-text nicety.

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; end R; class C public static method Take(p: owned R): int var h := lambda(owned c): int => c.V(); return h(move p); end Take; public static method Main(): int Console.WriteLine(Take(new R()).ToString()); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: parameter `c` needs a type annotation: no target type to infer from
// CHECK-SAME: (§27.4)
