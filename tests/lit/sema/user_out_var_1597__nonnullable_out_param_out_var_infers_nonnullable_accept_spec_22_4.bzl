// Ported from tests/sema/user_method_out_var_nullable_1597_test.cpp
//   (BEZEL_TEST user_out_var_1597, nonnullable_out_param_out_var_infers_nonnullable_accept_spec_22_4).
//
// Control (no over-widening): a NON-nullable out param (`out v: string`)
// still infers plain `string` — the unguarded read stays ACCEPTED, exactly
// as before the fix.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Finder public static method TryGetNonNull(out v: string): bool v := "hi"; return true; end TryGetNonNull; public static method Run(): int var ok := Finder.TryGetNonNull(out var s); return s.Length; end Run; end Finder;

// NODIAG-NOT: error
// NODIAG-NOT: §
