// Ported from tests/sema/user_method_out_var_nullable_1597_test.cpp
//   (BEZEL_TEST user_out_var_1597, out_owned_var_nonnullable_infers_nonnullable_accept_spec_22_4).
//
// Control: a NON-nullable `out owned conn: Cn` still infers plain `Cn` via
// the adopted `out var` form (§22.4 / :4676) —
// unguarded `c.Use()` stays ACCEPTED.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cn public method Use(): int => 1; end Cn; class Pool public method TryTakeNonNull(out owned conn: Cn): bool conn := new Cn(); return true; end TryTakeNonNull; public static method Run(): int var p := new Pool(); var ok := p.TryTakeNonNull(out var c); return c.Use(); end Run; end Pool;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned value in `c` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 must-use / auto-drop)
