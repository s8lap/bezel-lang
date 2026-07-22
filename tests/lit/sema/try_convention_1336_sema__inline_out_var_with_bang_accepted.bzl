// Ported from tests/sema/try_convention_1336_test.cpp
//   (BEZEL_TEST try_convention_1336_sema, inline_out_var_with_bang_accepted).
//
// (c') the same inline form with `!` is ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int do const s: Stack<int> := [1]; if s.TryPop(out var x) then return x!; end; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
