// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_container_1541_test.cpp
//   (BEZEL_TEST event_container_1541, event_in_enum_rejected).
//
// §24: an `event` member in an ENUM body is a compile error naming the same
// rule.

module M; record Ev(N: int); enum Color Red, Green, Blue; public event Boom: Ev; end Color;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum `Color` may not declare an `event` member — an event's compiler-generated `owned EventSource` backing field requires an owning, disposable container, which an enum is not
// CHECK-SAME: (§24, §9.7)
