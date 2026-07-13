// Ported from tests/sema/scoped_iface_match_1685_test.cpp
//   (BEZEL_TEST sema_scoped_iface_match_1685, inherited_impl_scoped_match_accepted).
//
// §11.2 — the same inherited-implementation shape, but MATCHING: control to
// confirm inherited (not redeclared) implementations are not false-rejected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IEventOwner method OnEvent(scoped cb: () -> ()); end IEventOwner; class Base public method OnEvent(scoped cb: () -> ()) end OnEvent; end Base; class D : Base, IEventOwner end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
