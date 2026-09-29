// Ported from tests/sema/using_var_reassign_830_test.cpp
//   (BEZEL_TEST using_var_reassign_830, plain_var_reassign_accepted).
//
// A plain `var` (not `using var`) value-type local may still be reassigned.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is
// legal), but it DOES emit a diagnostic — the `R.n` counter field below is
// a mutable static of primitive type, which §22.12 rule 2 form (iii)
// (#1393 warned-admission ruling, v0.221) admits only with a
// declaration-site warning; unrelated to this test's own #830 subject, so
// only pinned here to keep the ACCEPT assertion accurate rather than
// silently stale.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class R public static n: int := 0; destructor R.n := R.n + 1; end destructor; end R; class C public static method Main(): int var x := 3; x := 7; return x; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `n` has type `int` — a primitive/enum value static is admitted only with a declaration-site warning (§22.12 rule 2 form (iii))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
