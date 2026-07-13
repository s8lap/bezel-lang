// Ported from tests/sema/user_method_out_var_nullable_1597_test.cpp
//   (BEZEL_TEST user_out_var_1597, nullable_out_var_narrows_after_null_check_accepted_spec_7_4).
//
// §7.4: the nullable out-var's type is genuinely narrowable — a guarded
// `s <> null` check ACCEPTS the subsequent `s.Length`, proving `s` is really
// typed `string?` (narrowing has a nullable type to narrow FROM), not just
// left untyped/unchecked.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Finder public static method TryGet(out v: string?): bool v := null; return false; end TryGet; public static method Run(): int var ok := Finder.TryGet(out var s); if s <> null then return s.Length; end; return 0; end Run; end Finder;

// NODIAG-NOT: error
// NODIAG-NOT: §
