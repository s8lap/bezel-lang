// §22.12 rule 2 (v0.221 ruling / #1393, spec v0.228): a mutable static
// FIELD of a compiler-known value-builtin type (`Guid`) is a compile
// error — the second of the two value-builtins the ruling text names by
// example ("`DateTime`, `Guid`, …"). Previously silently admitted with no
// diagnostic.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class App public static _id: Guid? := null; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static field `_id` has type `Guid?`, which §22.12 rule 2 does not admit as a mutable static — compiler-known value-builtin types (DateTime, Guid,
// CHECK-SAME: ) are not in the admitted category set; use a `Mutex`/`Locked<T>` cell instead
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
