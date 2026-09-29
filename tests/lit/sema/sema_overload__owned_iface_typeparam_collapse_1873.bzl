// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Regression for #1873 (§22.6 post-erasure overload collapse; A2 of the
// #1817 owned-marker-erasure decomposition) — INTERFACE members.
//
// §28.1 (v0.196) owned-marker erasure is defined per interface/class
// substitution, so an interface whose bare-type-parameter method slots differ
// ONLY by `owned` collapses to the same signature at value substitutions.
// §11.3's member-clash rule applies to interface members exactly as to class
// members; §22.6 (v0.181) makes `owned` never distinguish overloads, so the
// pair is rejected at the interface declaration.

module M; interface IStore<T> method Put(x: T): int; method Put(owned x: T): int; end IStore;

// CHECK: overload of `Put` is indistinguishable from another overload — overloads must differ in parameter count, types, or `ref`/`out` modifiers, or by ordinary `owned` under the declaration guard; `out owned` and `scoped` never distinguish. At a position whose type fails the guard the pair collapses to one signature
// CHECK-SAME: (§22.6/§11.3, v0.250)
