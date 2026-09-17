// Ported from tests/sema/event_container_1541_test.cpp
//   (BEZEL_TEST event_container_1541, event_in_class_accepted).
//
// §24: a `class` may declare an event (the control case — no error).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Ev(N: int); class Ok public event Boom: Ev; end Ok;

// NODIAG-NOT: error
// NODIAG-NOT: §
