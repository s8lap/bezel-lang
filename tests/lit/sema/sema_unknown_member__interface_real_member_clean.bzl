// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, interface_real_member_clean).
//
// A real interface member resolves cleanly on an interface-typed receiver.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IShape method Area(): int; end IShape; class C public method M(s: IShape): int => s.Area(); end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
