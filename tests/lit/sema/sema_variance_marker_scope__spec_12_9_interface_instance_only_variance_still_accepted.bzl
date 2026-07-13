// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_marker_scope, spec_12_9_interface_instance_only_variance_still_accepted).
//
// §12.9 / §11.8 positive control (issue #1559): an interface with ONLY
// instance members and a variant parameter still compiles — the rejection
// is scoped to (a) non-interface declarations and (b) interfaces that
// declare a static member, not to interface variance in general.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IProducer<out T> method Produce(): T; end IProducer;

// NODIAG-NOT: error
// NODIAG-NOT: §
