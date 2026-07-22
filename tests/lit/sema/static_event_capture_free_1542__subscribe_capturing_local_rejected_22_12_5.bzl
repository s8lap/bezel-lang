// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_event_capture_free_1542_test.cpp
//   (BEZEL_TEST static_event_capture_free_1542, subscribe_capturing_local_rejected_22_12_5).
//
// §22.12 rule 5: `<StaticEvent>.Subscribe(handler)` where the handler lambda
// captures a local variable of the enclosing method → rejected.

module M; record Ev(N: int); class Bus public static event Ticked: Ev; end Bus; class C public method Use() var total := 0; using var sub := Bus.Ticked.Subscribe(lambda(e) => do total := total + e.N; end); end Use; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: handler subscribed to a `static event` must be capture-free: a non-capturing lambda or a static method reference — a capturing lambda is rejected because the process-global subscriber list transports it to whichever domain raises, and its captures are borrows of the origin domain
// CHECK-SAME: (§22.12 rule 5, #1542)
