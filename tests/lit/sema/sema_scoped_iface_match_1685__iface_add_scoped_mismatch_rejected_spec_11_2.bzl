// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/scoped_iface_match_1685_test.cpp
//   (BEZEL_TEST sema_scoped_iface_match_1685, iface_add_scoped_mismatch_rejected_spec_11_2).
//
// §11.2 (v0.184) — the pinned bug: an interface declares a PLAIN function-
// typed parameter; the implementing method adds `scoped`. Callers through
// the interface admitted a call-bounded (non-retaining) argument, so a
// storing implementation is a use-after-free shape. Must be a compile error
// at the implementing class.

module M; interface IEventOwner method OnEvent(cb: () -> ()); end IEventOwner; class Impl : IEventOwner public method OnEvent(scoped cb: () -> ()) end OnEvent; end Impl;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `OnEvent` does not match interface member `OnEvent`'s parameter modifier on parameter 1 — `ref`/`out`/`owned`/`scoped` must match exactly
// CHECK-SAME: (§11.2)
