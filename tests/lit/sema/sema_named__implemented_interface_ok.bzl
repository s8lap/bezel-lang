// Ported from tests/sema/named_assignability_test.cpp
//   (BEZEL_TEST sema_named, implemented_interface_ok).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; interface IShape method Area(): int; end IShape; class Sq : IShape public method Area(): int => 1; end Sq; class C public method M() var s: IShape := new Sq(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `s` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)
