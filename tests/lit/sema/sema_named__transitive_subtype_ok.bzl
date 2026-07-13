// Ported from tests/sema/named_assignability_test.cpp
//   (BEZEL_TEST sema_named, transitive_subtype_ok).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class A end A; class B : A end B; class D : B end D; class C public method M() var a: A := new D(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `a` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)
