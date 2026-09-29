// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Regression for #1873 (§22.6 post-erasure overload collapse; A2 of the
// #1817 owned-marker-erasure decomposition).
//
// §28.1 (v0.196) makes `owned T` on a BARE TYPE PARAMETER a per-instantiation
// marker: at a value / runtime-managed substitution of `T` the marker is
// ERASED and `owned T` becomes a plain value.  Two overloads that differ ONLY
// by that marker — `Store(x: T)` and `Store(owned x: T)` — therefore collapse
// to the same post-erasure signature `Store(x: T-value)` at every value
// substitution.  §22.6 (v0.181) resolves the ambiguity #1873 raised at the
// DECLARATION, not per instantiation: `owned` (in any position) never
// distinguishes overloads, so the pair is an indistinguishable-overload clash
// (§11.3) reported at the second declaration — uniformly, whether or not any
// particular substitution would erase the marker.  This is interpretation (1)
// of the issue: rejected at declaration.

module M; class Box<T> public method Store(x: T) _ := 1; end Store; public method Store(owned x: T) _ := 2; end Store; end Box;

// CHECK: overload of `Store` is indistinguishable from another overload — overloads must differ in parameter count, types, or `ref`/`out` modifiers, or by ordinary `owned` under the declaration guard; `out owned` and `scoped` never distinguish. At a position whose type fails the guard the pair collapses to one signature
// CHECK-SAME: (§22.6/§11.3, v0.250)
