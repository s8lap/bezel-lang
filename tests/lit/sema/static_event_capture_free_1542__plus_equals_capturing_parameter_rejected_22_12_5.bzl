// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_event_capture_free_1542_test.cpp
//   (BEZEL_TEST static_event_capture_free_1542, plus_equals_capturing_parameter_rejected_22_12_5).
//
// §22.12 rule 5 / §24: `staticEvent += handler` desugars to Add — a
// capturing handler (captures the enclosing static method's parameter
// `offset`) is rejected exactly as the explicit `.Add(...)` form is.

module M; record Ev(N: int); class Bus public static event Signal: Ev; public static method Fire(n: int) Signal.Raise(new Ev(n)); end Fire; public static method Hook(offset: int) Signal += lambda(e) => do Bus.Fire(e.N + offset); end; end Hook; end Bus;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: handler subscribed to a `static event` must be capture-free: a non-capturing lambda or a static method reference — a capturing lambda is rejected because the process-global subscriber list transports it to whichever domain raises, and its captures are borrows of the origin domain
// CHECK-SAME: (§22.12 rule 5, #1542)
