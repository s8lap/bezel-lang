// Ported from tests/sema/named_assignability_test.cpp
//   (BEZEL_TEST sema_named, transitive_subtype_ok).
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

module M; class A end A; class B : A end B; class D : B end D; class C public method M() var a: A := new D(); end M; end C;

// CHECK-NOT: §28.3 auto-drop
