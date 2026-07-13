// Ported from tests/sema/alias_transparency_578_test.cpp
//   (BEZEL_TEST sema_alias_transparency_578, alias_of_class_member_lookup_spec_27_8).
//
// Control: alias of a user class type — member lookup must still work.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Point public X: int := 0; end Point; type Pt := Point; class C public static method M(p: Pt): int => p.X; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
