// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/scoped_iface_match_1685_test.cpp
//   (BEZEL_TEST sema_scoped_iface_match_1685, iface_drop_scoped_mismatch_rejected_spec_11_2).
//
// §11.2 (v0.184) — the reverse direction: an interface declares the
// parameter `scoped`; the implementation drops it. Semantically the safer
// direction (retains less), but rejected all the same — `scoped` is part of
// function-type identity and never distinguishes overloads (§22.6).

module M; interface IEventOwner method OnEvent(scoped cb: () -> ()); end IEventOwner; class Impl : IEventOwner public method OnEvent(cb: () -> ()) end OnEvent; end Impl;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `OnEvent` does not match interface member `OnEvent`'s parameter modifier on parameter 1 — `ref`/`out`/`owned`/`scoped` must match exactly
// CHECK-SAME: (§11.2)
