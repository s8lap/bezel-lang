// Ported from tests/sema/named_assignability_test.cpp
//   (BEZEL_TEST sema_named, same_type_ok).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class T end T; class C public method M() var x: T := new T(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `x` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)
