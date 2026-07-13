// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, interface_typed_receiver_permissive).
//
// An interface-typed receiver stays permissive — interfaces are not treated as
// known-complete here (default/inherited member resolution is incomplete).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IShape method Area(): int; end IShape; class C public method M(s: IShape): int => s.SomethingNotDeclared(); end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
