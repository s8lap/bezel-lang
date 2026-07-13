// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, local_variable_narrows_spec_7_4).
//
// §7.4: a local variable (not just a parameter) narrows the same way — this is
// the §7.4 "copy a nullable field to a local before narrowing" pattern.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class User public Name: int := 0; end User; class C public static method Run(): int var u: User? := new User(); if u <> null then return u.Name; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `u` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)
