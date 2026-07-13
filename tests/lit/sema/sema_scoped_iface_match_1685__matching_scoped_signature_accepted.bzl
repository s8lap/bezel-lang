// Ported from tests/sema/scoped_iface_match_1685_test.cpp
//   (BEZEL_TEST sema_scoped_iface_match_1685, matching_scoped_signature_accepted).
//
// CONTROL — a `scoped` interface member matched exactly by the implementation
// compiles clean.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IEventOwner method OnEvent(scoped cb: () -> ()); end IEventOwner; class Impl : IEventOwner public method OnEvent(scoped cb: () -> ()) end OnEvent; end Impl;

// NODIAG-NOT: error
// NODIAG-NOT: §
