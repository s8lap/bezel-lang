// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, shared_documented_surface_allowed_2_16).
//
// (b) Documented Shared<T> surface still compiles without error.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal),
// but it DOES emit the §28.3 owned-parameter must-use warning — pinned below so
// the warning itself stays observed.
//
// narrow #4743 / §28.13 + §28.9, v0.307. #4743 flipped this file to a NODIAG-NOT
// silence on the reading that `Shared<T>` "classifies as trivial-dispose
// (runtime-managed ref-count)". The spec does not support that reading, in two
// places. §28.9's runtime-managed tier is a CLOSED list — string, Type, boxed
// value types, function-typed values — and says so: "This tier is exhaustive:
// no runtime lifetime mechanism exists for user-defined classes at all."
// §28.13 then states the handle's own status outright: "**Handles are ordinary
// owned class instances.** Every rule of this chapter applies to a handle
// unchanged", and "disposing the last live handle disposes the underlying value
// (running its destructor cascade normally)" — user code, which is exactly what
// §28.9's trivial-dispose definition ("provably runs no user code and releases
// only memory") excludes. So the warning is due and the silence was the bug.
//
// The CHECK-SAME needle is the CURRENT diagnostic text. The pre-#4743 line read
// `(§28.3 auto-drop)`, which no longer occurs anywhere in the message — the
// wording is `(§28.3 must-use / auto-drop)` — so restoring it verbatim would
// have re-landed a red row. The §28.10 marked-adoption remedy is pinned too,
// which the old spelling did not cover.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Foo end Foo; class C public static method Test(owned s: Shared<Foo>) using var s2 := s.Clone(); var _f := s2.Get(); using var w := s2.Downgrade(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned parameter `s` is implicitly disposed at the end of the method
// CHECK-SAME: (§28.3 must-use / auto-drop)
// CHECK-SAME: using var s2 := move s;
