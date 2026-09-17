// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_container_1541_test.cpp
//   (BEZEL_TEST event_container_1541, event_in_record_rejected).
//
// §24: an `event` member in a RECORD body is a compile error naming the
// owning-disposable-container rule.

module M; record Ev(N: int); record Bad public event Boom: Ev; end Bad;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `Bad` may not declare an `event` member — an event's compiler-generated `owned EventSource` backing field requires an owning, disposable container, which a record is not
// CHECK-SAME: (§24, §9.7)
