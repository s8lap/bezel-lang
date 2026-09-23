// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, shared_documented_surface_allowed_2_16).
//
// (b) Documented Shared<T> surface still compiles without error.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Foo end Foo; class C public static method Test(owned s: Shared<Foo>) using var s2 := s.Clone(); var _f := s2.Get(); using var w := s2.Downgrade(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned parameter `s` is implicitly disposed at the end of the method
// CHECK-SAME: (§28.3 auto-drop)
