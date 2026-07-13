// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, byvalue_arg_keeps_narrowing_1188).
//
// §7.4 (#1188): a BY-VALUE argument cannot mutate the caller's local, so the
// narrowing must SURVIVE the call — the trailing member access stays legal.
// (The invalidation must not over-reject plain by-value calls.)
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class User public Name: int := 0; end User; class C public static method Look(u: User?) end Look; public static method Run(): int var u: User? := new User(); if u <> null then Look(u); return u.Name; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `u` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)
