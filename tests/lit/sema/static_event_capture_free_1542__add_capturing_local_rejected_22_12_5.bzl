// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_event_capture_free_1542_test.cpp
//   (BEZEL_TEST static_event_capture_free_1542, add_capturing_local_rejected_22_12_5).
//
// §22.12 rule 5: `<StaticEvent>.Add(handler)` (inside the declaring class,
// where Add is on the EventSource<T> face, §24) with a capturing handler
// → rejected the same way as Subscribe.

module M; record Ev(N: int); class Bus public static event Ticked: Ev; public static method Hook(offset: int) Ticked.Add(lambda(e) => do offset := offset + e.N; end); end Hook; end Bus;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: handler subscribed to a `static event` must be capture-free: a non-capturing lambda or a static method reference — a capturing lambda is rejected because the process-global subscriber list transports it to whichever domain raises, and its captures are borrows of the origin domain
// CHECK-SAME: (§22.12 rule 5, #1542)
