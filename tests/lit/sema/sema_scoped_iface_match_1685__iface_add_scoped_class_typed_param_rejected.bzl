// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/scoped_iface_match_1685_test.cpp
//   (BEZEL_TEST sema_scoped_iface_match_1685, iface_add_scoped_class_typed_param_rejected).
//
// §11.2 / §28.14 (v0.180, #1439 scoped construction) — the same exact-match
// rule applies to a `scoped` parameter of CLASS type (a passive borrow, not
// a callback), not just function-typed parameters. Add-mismatch rejected.

module M; class Stream end Stream; interface IHolder method Attach(s: Stream); end IHolder; class Impl : IHolder private _s: Stream? := null; public method Attach(scoped s: Stream) _s := s; end Attach; end Impl;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `Attach` does not match interface member `Attach`'s parameter modifier on parameter 1 — `ref`/`out`/`owned`/`scoped` must match exactly
// CHECK-SAME: (§11.2)
