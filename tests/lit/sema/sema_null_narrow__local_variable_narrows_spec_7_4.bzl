// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, local_variable_narrows_spec_7_4).
//
// §7.4: a local variable (not just a parameter) narrows the same way — this is
// the §7.4 "copy a nullable field to a local before narrowing" pattern.
//
// ACCEPT-SILENTLY control: bezelc -c must exit zero (the program is legal)
// AND must emit no diagnostic at all.
//
// This fixture previously pinned the §28.3 auto-drop warning here. That
// expectation encoded the #2654 bug: the class bound below is TRIVIAL-DISPOSE
// (no destructor, no `owned` field, no owning-inline-array field, trivial
// base), and spec §28.9 (v0.237) "Trivial-dispose classes" states that "a
// `new` expression of a trivial-dispose class type whose result is not stored
// in an `owned` location or `using var` does *not* produce the standard
// ownership-discharge warning. (There is nothing to discharge.)" The
// CHECK-NOT below now pins that exemption; this file's own subject (above) is
// unchanged, and the program text is untouched.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --allow-empty %s

module M; class User public Name: int := 0; end User; class C public static method Run(): int var u: User? := new User(); if u <> null then return u.Name; end; return -1; end Run; end C;

// CHECK-NOT: §28.3 auto-drop
