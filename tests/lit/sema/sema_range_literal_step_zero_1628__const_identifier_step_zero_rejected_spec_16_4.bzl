// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §16.4 (v0.242, #3052): "the compile error widens from the literal to any
// constant-folded zero step (`step (2-2)`, `step K` — §17.14's posture)". The
// sibling test covers `step (2 - 2)`; this one covers the `step K` half — a
// §17.14 compile-time constant reached through a `const` identifier, which the
// shared fold resolves exactly as it resolves a bare literal.

module M; class C public static method Main(): int const K: int := 0; for var i in 0..10 step K do end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a range step of zero is a compile error
// CHECK-SAME: (§16.4)
