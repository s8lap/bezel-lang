// Ported from tests/sema/named_assignability_test.cpp
//   (BEZEL_TEST sema_named, transitive_subtype_ok).
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

module M; class A end A; class B : A end B; class D : B end D; class C public method M() var a: A := new D(); end M; end C;

// CHECK-NOT: auto-drop
