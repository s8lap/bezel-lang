// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, real_members_on_class_clean).
//
// Real field / method / property on a user class resolve cleanly.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Animal public _legs: int; public property Name: int; public method Speak(): int => 1; end Animal; class C public method M(a: Animal): int => a._legs + a.Name + a.Speak(); end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
