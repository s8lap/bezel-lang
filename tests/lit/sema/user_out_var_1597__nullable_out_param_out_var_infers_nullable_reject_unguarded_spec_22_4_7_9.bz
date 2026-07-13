// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/user_method_out_var_nullable_1597_test.cpp
//   (BEZEL_TEST user_out_var_1597, nullable_out_param_out_var_infers_nullable_reject_unguarded_spec_22_4_7_9).
//
// §22.4/§7.9 (#1597): a USER-declared method's nullable out param
// (`out v: string?`) bound with an inline `out var s` must type `s` as
// `string?` — a direct (unguarded) `s.Length` after the call is a §7.4
// nullable-member-access error, exactly as it would be for a directly
// declared `var s: string? := null;`.

module M; class Finder public static method TryGet(out v: string?): bool v := null; return false; end TryGet; public static method Run(): int var ok := Finder.TryGet(out var s); return s.Length; end Run; end Finder;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
