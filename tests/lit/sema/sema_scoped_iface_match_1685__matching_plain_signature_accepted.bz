// Ported from tests/sema/scoped_iface_match_1685_test.cpp
//   (BEZEL_TEST sema_scoped_iface_match_1685, matching_plain_signature_accepted).
//
// CONTROL — a plain (non-`scoped`) interface member matched exactly compiles
// clean (the ordinary, unmarked case must not be false-rejected).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IEventOwner method OnEvent(cb: () -> ()); end IEventOwner; class Impl : IEventOwner public method OnEvent(cb: () -> ()) end OnEvent; end Impl;

// NODIAG-NOT: error
// NODIAG-NOT: §
