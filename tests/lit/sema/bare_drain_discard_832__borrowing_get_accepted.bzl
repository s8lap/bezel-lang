// Ported from tests/sema/bare_drain_discard_832_test.cpp
//   (BEZEL_TEST bare_drain_discard_832, borrowing_get_accepted).
//
// A borrowing access (Get) returns a borrow, not an owned value → not rejected.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is
// legal), but it DOES emit a diagnostic — the `R.n` counter field below is
// a mutable static of primitive type, which §22.12 rule 2 form (iii)
// (#1393 warned-admission ruling, v0.221) admits only with a
// declaration-site warning; unrelated to this test's own #832 subject, so
// only pinned here to keep the ACCEPT assertion accurate rather than
// silently stale.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class R public static n: int := 0; destructor R.n := R.n + 1; end destructor; end R; class C public static method Main(): int using var a := new OwningArray<R>(); a.Add((new R())); a.Get(0); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `n` has type `int` — a primitive/enum value static is admitted only with a declaration-site warning (§22.12 rule 2 form (iii))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
