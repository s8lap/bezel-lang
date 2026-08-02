// §22.12 rule 2 (v0.221 ruling / #1393, spec v0.228): a mutable static
// FIELD of choice type (§35) is a compile error — the same torn-read
// invariant hazard as records. Previously silently admitted with no
// diagnostic.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; choice Shape Circle, Square; end Shape; class App public static _shape: Shape? := null; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static field `_shape` has type `Shape?`, which §22.12 rule 2 does not admit as a mutable static — a multi-word choice static's torn cross-domain read can fabricate an invalid value; use a `Mutex`/`Locked<T>` cell instead
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
