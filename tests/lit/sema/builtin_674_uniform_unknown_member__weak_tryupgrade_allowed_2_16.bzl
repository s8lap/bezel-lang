// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, weak_tryupgrade_allowed_2_16).
//
// (b) Documented Weak<T> surface still compiles without error.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Foo end Foo; class C public static method Test(owned w: Weak<Foo>) var ok := w.TryUpgrade(out using var s); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned parameter `w` is implicitly disposed at the end of the method
// CHECK-SAME: (§28.3 auto-drop)
