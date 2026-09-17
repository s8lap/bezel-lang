// Ported from tests/sema/event_source_member_dispatch_1610_test.cpp
//   (BEZEL_TEST event_source_member_dispatch_1610, real_members_accepted).
//
// ---------------------------------------------------------------------------
// (2) §6.5 / §24 / #1610: a REAL member call on an `EventSource<T>`
// parameter compiles cleanly — Raise / Subscribe / Add / Remove are all on
// the producer surface (stdlib §6.5). (Before the fix these compiled too,
// but only because EVERYTHING compiled; this guards against an over-broad
// rejection accidentally forbidding a valid method.)
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Probe; import System.Threading; class Ev public N: int := 0; public constructor(n: int) N := n; end constructor; end Ev; public class Counter public event Ticked: Ev; public method Fire(es: EventSource<Ev>) es.Raise(new Ev(1)); es.Add(lambda(e: Ev) => do end); es.Remove(lambda(e: Ev) => do end); end Fire; public method Sub(es: EventSource<Ev>) using var s := es.Subscribe(lambda(e: Ev) => do end); end Sub; end Counter;

// NODIAG-NOT: error
// NODIAG-NOT: §
