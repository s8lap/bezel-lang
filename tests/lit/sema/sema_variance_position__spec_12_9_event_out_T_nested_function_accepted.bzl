// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_position, spec_12_9_event_out_T_nested_function_accepted).
//
// §12.9: IEvent<out T> canonical example from the spec. T appears inside
// the parameter of a function type `(T) -> ()` which is itself a parameter of
// Subscribe — contravariant-of-contravariant = covariant position overall.
// fix #6348 implements that composition, so this row is no longer a
// "deferred check must not false-positive" control but a POSITIVE row on the
// composition itself: it must still be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IEvent<out T> method Subscribe(handler: (T) -> ()); end IEvent;

// NODIAG-NOT: error
// NODIAG-NOT: §
