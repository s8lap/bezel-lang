// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/event_source_member_dispatch_1610_test.cpp
//   (BEZEL_TEST event_source_member_dispatch_1610, local_bogus_member_rejected).
//
// ---------------------------------------------------------------------------
// (5) §6.5 / §24 / #1610: the LOCAL-variable form (issue repro variant 2:
// `var es: EventSource<T> := Ticked; es.<bogus>(...)`) is rejected too —
// not only the parameter form.
// ---------------------------------------------------------------------------

module Probe; import System.Threading; class Ev public N: int := 0; end Ev; public class Counter public event Ticked: Ev; public method Tick() var es: EventSource<Ev> := Ticked; es.NotARealMember(); end Tick; end Counter;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `NotARealMember` on `EventSource<T>`; documented surface: Subscribe(handler), Raise(payload), Add(handler), Remove(handler) (stdlib §6.5 / §24)
