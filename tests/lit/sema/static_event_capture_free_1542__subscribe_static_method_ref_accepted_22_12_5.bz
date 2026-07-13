// Ported from tests/sema/static_event_capture_free_1542_test.cpp
//   (BEZEL_TEST static_event_capture_free_1542, subscribe_static_method_ref_accepted_22_12_5).
//
// §22.12 rule 5: a static method reference (qualified `Type.Method`) as the
// `Subscribe` handler → accepted, the same admission as a Domain.Spawn entry.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Ev(N: int); class Bus public static event Ticked: Ev; public static method OnTick(e: Ev) end OnTick; end Bus; class C public method Use() using var sub := Bus.Ticked.Subscribe(Bus.OnTick); end Use; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
