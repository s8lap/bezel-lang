// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §16.4 / §6.5 / §17.14 (v0.242, #3052 × #3045) — the interaction of the two
// v0.242 constant-fault rules, pinned so neither can silently swallow the
// other.
//
// A step expression carrying its OWN §17.14 evaluation fault — here a constant
// shift count outside `0..31` (§6.5) — makes the shared integer fold DECLINE
// (it has no value). §16.4's constant-zero-step check consumes that same fold,
// so it must stay silent and let the specific cause be reported: the shift
// domain fault, NOT "a range step of zero" (which would be a fabricated
// diagnosis — the step is not zero, it is undefined).

module M; class C public static method Main(): int for var i in 0..10 step (1 << 64) do end; return 0; end Main; end C;

// #7065: the same needle, written ABOVE the positive match. A negative
// directive scans only the region between the surrounding matches, so the
// copy below the match alone let this wording through when it came FIRST.
// CHECK-NOT: a range step of zero
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: shift count 64 is outside the domain `0..31`
// CHECK-NOT: a range step of zero
