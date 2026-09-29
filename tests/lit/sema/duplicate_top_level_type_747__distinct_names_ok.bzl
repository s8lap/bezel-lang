// Ported from tests/sema/duplicate_top_level_type_747_test.cpp
//   (BEZEL_TEST duplicate_top_level_type_747, distinct_names_ok).
//
// §11.3 regression guard: distinct top-level type names are accepted.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is
// legal), but it DOES emit diagnostics — both `A.V` and `B.V` below are
// mutable statics of primitive type, which §22.12 rule 2 form (iii) (#1393
// warned-admission ruling, v0.221) admits only with a declaration-site
// warning each; unrelated to this test's own #747 subject (distinct
// top-level type names), so only pinned here (one of the two — FileCheck
// does not require exclusivity) to keep the ACCEPT assertion accurate
// rather than silently stale.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class A public static V: int := 1; end A; class B public static V: int := 2; end B; record C(X: int); enum E Red, Green; end E; interface I method M(): int; end I; class P public static method Main(): int return A.V + B.V; end Main; end P;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `V` has type `int` — a primitive/enum value static is admitted only with a declaration-site warning (§22.12 rule 2 form (iii))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
