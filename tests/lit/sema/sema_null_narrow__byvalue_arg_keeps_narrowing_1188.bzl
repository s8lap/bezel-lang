// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, byvalue_arg_keeps_narrowing_1188).
//
// §7.4 (#1188): a BY-VALUE argument cannot mutate the caller's local, so the
// narrowing must SURVIVE the call — the trailing member access stays legal.
// (The invalidation must not over-reject plain by-value calls.)
//
// ACCEPT-SILENTLY control: bezelc -c must exit zero (the program is legal)
// AND must emit no diagnostic at all.
//
// The class below is TRIVIAL-DISPOSE (no destructor, no `owned` field, no
// owning-inline-array field, trivial base). Per spec §28.9 (v0.237) "Trivial-
// dispose classes", a `new` expression of a trivial-dispose class type whose
// result is not stored in an `owned` location or `using var` does *not*
// produce the standard ownership-discharge warning. (There is nothing to
// discharge.) The ACCEPT-SILENTLY control pins this exemption; the CHECK-NOT
// uses the invariant "auto-drop" substring to survive the #4735 diagnostic rename.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --allow-empty %s

module M; class User public Name: int := 0; end User; class C public static method Look(u: User?) end Look; public static method Run(): int var u: User? := new User(); if u <> null then Look(u); return u.Name; end; return -1; end Run; end C;

// CHECK-NOT: auto-drop
