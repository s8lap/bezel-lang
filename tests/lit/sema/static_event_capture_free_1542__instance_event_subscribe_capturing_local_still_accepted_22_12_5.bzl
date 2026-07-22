// Ported from tests/sema/static_event_capture_free_1542_test.cpp
//   (BEZEL_TEST static_event_capture_free_1542, instance_event_subscribe_capturing_local_still_accepted_22_12_5).
//
// §22.12 rule 5's last sentence: "Instance events remain unsynchronized and
// capture-admitting." A capturing handler subscribed to a non-static
// (instance) event must NOT be rejected by this rule.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Ev(N: int); class Src public event Ticked: Ev; end Src; class C public method Use() var total := 0; using var src := new Src(); using var sub := src.Ticked.Subscribe(lambda(e) => do total := total + e.N; end); end Use; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
