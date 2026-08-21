// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #3298 (§19.3 / §16.6, spec v0.286): "The rejection is keyed on the **kind**
// of pattern, not on which bounds are written, so it reaches the open-ended
// forms and the fully open `..` with the closed ones — a rule that counted
// bounds would give a range pattern and another range pattern of the same
// extent opposite verdicts" (§19.3). §16.6 states the same keying.
//
// The bare `..` is therefore rejected over a nullable scrutinee even though it
// performs no ordering test of its own; the reason §19.3 gives for the
// unbounded form is uniformity (v0.273), and the verdict is the same.

module M; class C public static method Main(): int var n: int? := 5; var v := (case n of .. => 1, _ => 2); return v; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a range pattern requires a non-nullable scrutinee — ordering admits no absent participant and pattern position performs no narrowing (§19.3, §16.6, §7.11)
