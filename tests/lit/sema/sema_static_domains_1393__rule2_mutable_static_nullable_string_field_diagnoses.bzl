// §22.12 rule 2 (v0.221 ruling / #1393, spec v0.228): the nullable form
// `string?` carries the same residual-error posture as `string` — the
// ruling text explicitly names "mutable statics of `string` (and
// `string?`)" together.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class App public static _name: string? := null; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static field `_name` has type `string?`, which §22.12 rule 2 does not admit as a mutable static — a `string`(-bearing) static's overwrite must either release the displaced referent (a cross-domain use-after-free) or never release it (an unbounded leak); use a `Mutex`/`Locked<T>` cell instead
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
