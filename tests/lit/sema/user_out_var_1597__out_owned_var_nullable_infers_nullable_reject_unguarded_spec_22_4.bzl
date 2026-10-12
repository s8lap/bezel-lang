// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/user_method_out_var_nullable_1597_test.cpp
//   (BEZEL_TEST user_out_var_1597, out_owned_var_nullable_infers_nullable_reject_unguarded_spec_22_4).
//
// §22.4: a nullable `out owned conn: Cn?` bound via the inline
// `out owned var c` form must type `c` as `Cn?` — unguarded `c.Use()` is
// REJECTED (§7.4).

module M; class Cn public method Use(): int => 1; end Cn; class Pool public method TryTake(out owned conn: Cn?): bool conn := new Cn(); return true; end TryTake; public static method Run(): int var p := new Pool(); var ok := p.TryTake(out var c); return c.Use(); end Run; end Pool;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Use` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
