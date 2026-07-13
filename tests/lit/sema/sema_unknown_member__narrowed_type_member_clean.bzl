// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, narrowed_type_member_clean).
//
// §12.7: a member declared only on the narrowed (derived) type resolves after
// an `is` narrowing — and must not be falsely rejected against the static type.
// (Guards the #413 interaction: the diagnostic must run against the narrowed
// type, not the declared base.)
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Animal public method Speak(): int => 0; end Animal; class Dog : Animal public method Bark(): int => 1; end Dog; class C public method M(a: Animal): int if a is Dog then return a.Bark(); end; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
